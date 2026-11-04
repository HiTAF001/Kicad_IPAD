#!/usr/bin/env python3
"""Apply translation corrections collected in the browser (OmniRoute run) to tools/i18n/*.tsv.

usage: apply_review.py <repo> <changes.json> <report.md>
changes.json: [{"file", "lang", "en", "before", "after", "reason", "model"}, ...]
A change is applied only when the cell still holds "before" and the placeholders match.
"""
import json
import re
import sys

LANGS = ['tr', 'de', 'fr', 'es', 'it', 'fi', 'ru', 'ja', 'ko', 'zh-Hans']
NAMES = {'tr': 'Turkish', 'de': 'German', 'fr': 'French', 'es': 'Spanish', 'it': 'Italian', 'fi': 'Finnish',
         'ru': 'Russian', 'ja': 'Japanese', 'ko': 'Korean', 'zh-Hans': 'Simplified Chinese'}
SEP = ' ‖ '


def ph(s):
    return sorted(re.findall(r'%lld|%@|\{\d\}', s))


def main():
    repo, changes_path, report_path = sys.argv[1:4]
    changes = json.load(open(changes_path, encoding='utf-8'))
    applied, skipped = [], []

    for name in ('ui.tsv', 'core.tsv'):
        path = '%s/tools/i18n/%s' % (repo, name)
        lines = open(path, encoding='utf-8').read().split('\n')
        index = {}

        for i, line in enumerate(lines):
            if line.strip() and not line.startswith('#'):
                index[line.split(SEP)[0].strip()] = i

        for c in changes:
            if c['file'] != name:
                continue

            i = index.get(c['en'])

            if i is None:
                skipped.append((c, 'row not found'))
                continue

            cols = [x.strip() for x in lines[i].split(SEP)]
            col = LANGS.index(c['lang']) + 1

            if cols[col] != c['before']:
                skipped.append((c, 'cell changed'))
                continue

            if ph(c['after']) != ph(cols[0]) or SEP.strip() in c['after'] or '\n' in c['after']:
                skipped.append((c, 'placeholders'))
                continue

            cols[col] = c['after']
            lines[i] = SEP.join(cols)
            applied.append(c)

        open(path, 'w', encoding='utf-8').write('\n'.join(lines))

    models = sorted({c['model'] for c in applied})
    out = ['# Translation review (%s)' % ', '.join(models), '', '%d change(s) applied, %d skipped.' % (len(applied), len(skipped)), '']

    for lang in LANGS:
        for name in ('ui.tsv', 'core.tsv'):
            rows = [c for c in applied if c['lang'] == lang and c['file'] == name]

            if not rows:
                continue

            out += ['## %s — %s (%d)' % (NAMES[lang], name, len(rows)), '', '| English | Before | After | Reason |',
                    '|---|---|---|---|']

            for c in rows:
                cell = lambda s: s.replace('|', '\\|')
                out.append('| %s | %s | %s | %s |' % (cell(c['en']), cell(c['before']), cell(c['after']), cell(c['reason'])))

            out.append('')

    if skipped:
        out += ['## Skipped', '']

        for c, why in skipped:
            out.append('- %s %s: %s → %s (%s)' % (c['lang'], c['en'], c['before'], c['after'], why))

    open(report_path, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
    print('applied %d, skipped %d' % (len(applied), len(skipped)))


if __name__ == '__main__':
    main()
