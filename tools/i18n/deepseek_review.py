#!/usr/bin/env python3
# Netloom - PCB design for iPad
# Copyright (C) 2026 Seyhan Labs
"""
Second opinion on the translations: sends tools/i18n/ui.tsv and core.tsv to the DeepSeek API,
language by language, and writes back only the corrections that keep every placeholder. A
Markdown report lists each change with the reason the model gave, for a human to review in a
pull request (see .github/workflows/translations.yml).

usage: DEEPSEEK_API_KEY=... tools/i18n/deepseek_review.py [--model deepseek-v4-pro]
                                                           [--languages tr,de] [--report out.md]
                                                           [--dry-run]

Any OpenAI-compatible gateway works too, e.g. a local OmniRoute (PowerShell):
    $env:OMNIROUTE_API_KEY = "<key from the OmniRoute dashboard>"
    python tools\i18n\deepseek_review.py --base-url http://localhost:20128/v1 `
        --key-env OMNIROUTE_API_KEY --model <DeepSeek model id from /v1/models>
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
LANGS = ['tr', 'de', 'fr', 'es', 'it', 'fi', 'ru', 'ja', 'ko', 'zh-Hans']
NAMES = {'tr': 'Turkish', 'de': 'German', 'fr': 'French', 'es': 'Spanish', 'it': 'Italian', 'fi': 'Finnish',
         'ru': 'Russian', 'ja': 'Japanese', 'ko': 'Korean', 'zh-Hans': 'Simplified Chinese'}
SEP = ' ‖ '
BATCH = 50
DEEPSEEK_URL = 'https://api.deepseek.com'

SYSTEM = """You review the translations of Netloom, an iPad app for viewing and editing printed circuit
boards and schematics (KiCad files): layers, footprints, pads, vias, tracks, copper zones, nets,
design rule checks, Gerber/drill fabrication files, schematic symbols, wires, labels, sheets.

You get English source strings with the current {lang} translation. Correct a translation only
when it is wrong, misleading, ungrammatical, unnatural for a native speaker, or not the usual
electronics / PCB design term in {lang} (prefer the terms KiCad's own {lang} translation uses).
Keep the tone short and plain, like iPadOS system apps; keep it about as long as the current text.
Placeholders such as {{0}}, {{1}}, %@ and %lld must stay exactly as they are, the same number of
times. "Netloom", "Seyhan Labs", "KiCad", "Gerber", "Excellon", "Apple Pencil" are names.
Leave good translations unchanged.

Answer in json only, in this form:
{{"items": [{{"id": 3, "text": "corrected translation", "reason": "short reason in English"}}]}}
List only the items you changed; an empty list is fine."""


def load(name):
    """Rows of a TSV file: comments kept as strings, data rows as [key, tr, de, ...]."""
    rows = []

    with open(os.path.join(HERE, name), encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')

            if not line.strip() or line.startswith('#'):
                rows.append(line)
                continue

            cols = [c.strip() for c in line.split(SEP)]

            if len(cols) != len(LANGS) + 1:
                raise SystemExit('%s: bad row: %s' % (name, line[:60]))

            rows.append(cols)

    return rows


def save(name, rows):
    with open(os.path.join(HERE, name), 'w', encoding='utf-8') as f:
        for r in rows:
            f.write((r if isinstance(r, str) else SEP.join(r)) + '\n')


def placeholders(s):
    return sorted(re.findall(r'%lld|%@|\{\d\}', s))


def ask(url, key, model, lang, items):
    payload = {
        'model': model,
        'messages': [
            {'role': 'system', 'content': SYSTEM.format(lang=NAMES[lang])},
            {'role': 'user', 'content': json.dumps({'language': NAMES[lang], 'items': items}, ensure_ascii=False)},
        ],
        'response_format': {'type': 'json_object'},
        'temperature': 0.2,
        'max_tokens': 8000,
        'stream': False,
    }
    body = json.dumps(payload).encode('utf-8')

    for attempt in range(5):
        headers = {'Content-Type': 'application/json'}

        if key:
            headers['Authorization'] = 'Bearer ' + key

        req = urllib.request.Request(url, data=body, headers=headers)

        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                data = json.load(r)

            content = (data['choices'][0]['message'].get('content') or '').strip()

            # some gateways wrap the json in a code fence
            if content.startswith('```'):
                content = re.sub(r'^```[a-z]*\s*|\s*```$', '', content)

            if content:
                return json.loads(content).get('items', [])

            print('  empty answer, retrying', file=sys.stderr)
        except urllib.error.HTTPError as e:
            text = e.read().decode('utf-8', 'replace')[:300]
            print('  HTTP %d: %s' % (e.code, text), file=sys.stderr)

            if e.code in (400, 401, 402, 403, 404):
                raise SystemExit('The model service refused the request (HTTP %d): %s' % (e.code, text))
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, KeyError) as e:
            print('  %s, retrying' % e, file=sys.stderr)

        time.sleep(5 * (attempt + 1))

    raise SystemExit('The model service did not answer after several attempts')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--model', default='deepseek-v4-pro')
    ap.add_argument('--languages', default='')
    ap.add_argument('--report', default='')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--base-url', default=DEEPSEEK_URL,
                    help='OpenAI-compatible API root (DeepSeek by default, e.g. http://localhost:20128/v1 for OmniRoute)')
    ap.add_argument('--key-env', default='DEEPSEEK_API_KEY', help='environment variable holding the API key')
    args = ap.parse_args()

    key = os.environ.get(args.key_env, '')
    url = args.base_url.rstrip('/') + '/chat/completions'

    if not key and not args.dry_run and args.base_url == DEEPSEEK_URL:
        raise SystemExit('DEEPSEEK_API_KEY is not set (add it under Settings > Secrets and variables > Actions)')

    langs = [l.strip() for l in args.languages.split(',') if l.strip()] or LANGS

    for l in langs:
        if l not in LANGS:
            raise SystemExit('unknown language %s (one of %s)' % (l, ', '.join(LANGS)))

    report = ['# Translation review (%s)' % args.model, '']
    total = 0

    for name in ('ui.tsv', 'core.tsv'):
        rows = load(name)
        data = [r for r in rows if not isinstance(r, str)]

        for lang in langs:
            col = LANGS.index(lang) + 1
            changes = []

            for start in range(0, len(data), BATCH):
                chunk = data[start:start + BATCH]
                items = [{'id': start + i, 'en': r[0], 'current': r[col]} for i, r in enumerate(chunk)]
                print('%s %s %d-%d' % (name, lang, start, start + len(chunk) - 1), file=sys.stderr)

                if args.dry_run:
                    continue

                for it in ask(url, key, args.model, lang, items):
                    try:
                        idx = int(it.get('id', -1))
                    except (TypeError, ValueError):
                        continue

                    text = str(it.get('text', '')).strip()

                    if not ( start <= idx < start + len(chunk) ) or not text or SEP.strip() in text or '\n' in text:
                        continue

                    row = data[idx]

                    if text == row[col]:
                        continue

                    if placeholders(text) != placeholders(row[0]):
                        print('  skipped (placeholders): %s' % text, file=sys.stderr)
                        continue

                    changes.append((row[0], row[col], text, str(it.get('reason', '')).strip()))
                    row[col] = text

            if changes:
                total += len(changes)
                report += ['## %s — %s (%d)' % (NAMES[lang], name, len(changes)), '',
                           '| English | Before | After | Reason |', '|---|---|---|---|']

                for en, old, new, why in changes:
                    cell = lambda s: s.replace('|', '\\|')
                    report.append('| %s | %s | %s | %s |' % (cell(en), cell(old), cell(new), cell(why)))

                report.append('')

        if not args.dry_run:
            save(name, rows)

    report.insert(2, '%d change(s).\n' % total)
    text = '\n'.join(report) + '\n'

    if args.report:
        with open(args.report, 'w', encoding='utf-8') as f:
            f.write(text)

    print(text)


if __name__ == '__main__':
    main()
