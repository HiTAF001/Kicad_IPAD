#!/usr/bin/env python3
# Netloom - PCB design for iPad
# Copyright (C) 2026 Seyhan Labs
"""
Collects the localizable English strings:
  ui   - SwiftUI / String(localized:) literals in App/*.swift (interpolations -> %@ / %lld)
  core - Tr("...") / TrF("...") literals in core/src (placeholders {0}, {1} kept)
Prints them as  table<TAB>key  lines.
"""
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SWIFT_CALLS = [
    r'Text\(', r'Label\(', r'Button\(', r'Section\(', r'\.navigationTitle\(', r'TextField\(',
    r'Picker\(', r'\.alert\(', r'\.confirmationDialog\(', r'\.accessibilityLabel\(', r'Toggle\(',
    r'String\(localized:\s*', r'helpRow\("[^"]*",\s*', r'row\(', r'Menu\(',
]
LIT = r'"((?:[^"\\]|\\.)*)"'
INT_HINTS = ('Int(', '.count', '1_048_576', 'Index + 1')


def swift_key(lit):
    """Literal source text -> localization key (SwiftUI rules)."""
    out, i = '', 0
    while i < len(lit):
        if lit.startswith('\\(', i):
            depth, j = 1, i + 2
            while j < len(lit) and depth:
                depth += lit[j] == '('
                depth -= lit[j] == ')'
                j += 1
            expr = lit[i + 2:j - 1]
            out += '%lld' if any(h in expr for h in INT_HINTS) else '%@'
            i = j
        elif lit.startswith('\\"', i):
            out += '"'
            i += 2
        else:
            out += lit[i]
            i += 1
    return out


def ui_keys():
    keys = []
    for path in sorted(glob.glob(os.path.join(ROOT, 'App', '*.swift'))):
        src = open(path, encoding='utf-8').read()
        for call in SWIFT_CALLS:
            for m in re.finditer(call + LIT, src):
                keys.append(swift_key(m.group(1)))
        # helpRow detail (third argument) and ternaries typed as LocalizedStringKey
        for m in re.finditer(r'helpRow\("[^"]*",\s*' + LIT + r',\s*' + LIT, src):
            keys.append(swift_key(m.group(2)))
        for m in re.finditer(LIT + r' as LocalizedStringKey\s*:\s*' + LIT, src):
            keys.append(swift_key(m.group(1)))
            keys.append(swift_key(m.group(2)))
        for m in re.finditer(r'\?\s*' + LIT + r' as LocalizedStringKey', src):
            keys.append(swift_key(m.group(1)))
    skip = {'', 'Netloom', 'KiCad', 'mm', 'SMD', 'TrueType', 'Ratsnest'}
    out = []
    for k in keys:
        if k in skip or k.startswith('%') and k.replace('%lld', '').replace('/', '') == '':
            continue
        if not re.search(r'[A-Za-z]', k) or k in out:
            continue
        out.append(k)
    return out


def core_keys():
    out = []
    for path in sorted(glob.glob(os.path.join(ROOT, 'core', 'src', '*.cpp'))):
        if path.endswith('stroke_glyphs.cpp'):
            continue
        src = open(path, encoding='utf-8').read()
        for m in re.finditer(r'(?:\bTrF?\(|\bN_\(|info\.add\()\s*' + LIT, src):
            k = m.group(1).replace('\\"', '"')
            if k not in out:
                out.append(k)
        # fail("...") messages are translated inside SReader::fail
        if path.endswith('sexpr.cpp'):
            for m in re.finditer(r'\bfail\(\s*' + LIT, src):
                k = m.group(1)
                if k not in out:
                    out.append(k)
    # field names translated through Tr( field.c_str() )
    for k in ('Reference', 'Value'):
        if k not in out:
            out.append(k)
    return out


if __name__ == '__main__':
    for k in ui_keys():
        print('ui\t' + k)
    for k in core_keys():
        print('core\t' + k)
