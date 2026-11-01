#!/usr/bin/env python3
# Netloom - PCB design for iPad
# Copyright (C) 2026 Seyhan Labs
#
# This program is free software: you can redistribute it and/or modify it under the terms of
# the GNU General Public License as published by the Free Software Foundation, either
# version 3 of the License, or (at your option) any later version.
"""
Generates core/src/stroke_glyphs.cpp, the stroke font used to draw board text.

Input: Hershey font data (core/third_party/hershey/*.jhf, see the NOTICE there).
- ASCII from the Roman simplex set (rowmans.jhf)
- Greek (and micro / ohm) from the Greek simplex set (greeks.jhf)
- everything else is written here: accented Latin letters are composed from a base letter
  and an accent drawn below, plus typographic symbols (degree, plus-minus, dashes, quotes...)

Glyph strings use the Hershey .jhf encoding: two characters for the left/right bearing, then
coordinate pairs as characters relative to 'R', " R" = pen up. Units: cap height 21
(capitals span y = -12 .. 9, baseline at y = 9, y grows downwards).

usage: tools/gen_stroke_font.py [out.cpp]
"""
import os
import re
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERSHEY = os.path.join(ROOT, 'core', 'third_party', 'hershey')


class Glyph:
    def __init__(self, left, right, strokes):
        self.left = left
        self.right = right
        self.strokes = [list(s) for s in strokes]

    def copy(self):
        return Glyph(self.left, self.right, self.strokes)

    def ink(self):
        pts = [p for s in self.strokes for p in s]
        if not pts:
            return (0, 0, 0, 0)
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        return (min(xs), max(xs), min(ys), max(ys))

    def moved(self, dx, dy):
        return [[(x + dx, y + dy) for x, y in s] for s in self.strokes]

    def encode(self):
        def c(v):
            v = int(round(v))
            if not -40 <= v <= 40:
                raise ValueError('coordinate out of range: %d' % v)
            return chr(ord('R') + v)

        out = c(self.left) + c(self.right)
        first = True
        for s in self.strokes:
            if len(s) == 0:
                continue
            if not first:
                out += ' R'
            first = False
            pts = s if len(s) > 1 else [s[0], s[0]]
            for x, y in pts:
                out += c(x) + c(y)
        return out


def load_jhf(name):
    """Glyphs of a .jhf file in file order (ASCII-ordered sets start at space)."""
    entries = []
    cur = ''
    with open(os.path.join(HERSHEY, name), encoding='latin-1') as f:
        for line in f.read().split('\n'):
            if not line.strip():
                continue
            if re.match(r'^[ 0-9]{5}[ 0-9]{3}', line) and cur:
                entries.append(cur)
                cur = line
            else:
                cur = cur + line if cur else line
    if cur:
        entries.append(cur)

    glyphs = []
    for e in entries:
        n = int(e[5:8])
        body = e[8:8 + 2 * n]
        left = ord(body[0]) - ord('R')
        right = ord(body[1]) - ord('R')
        strokes = [[]]
        for k in range(2, len(body), 2):
            pair = body[k:k + 2]
            if pair == ' R':
                strokes.append([])
                continue
            strokes[-1].append((ord(pair[0]) - ord('R'), ord(pair[1]) - ord('R')))
        glyphs.append(Glyph(left, right, [s for s in strokes if s]))
    return glyphs


roman = load_jhf('rowmans.jhf')
greek = load_jhf('greeks.jhf')
glyphs = {}


def ascii_glyph(ch):
    return roman[ord(ch) - 32]


# ---- ASCII --------------------------------------------------------------------------------
for code in range(32, 127):
    glyphs[code] = roman[code - 32].copy()

# The Hershey set draws a few ASCII positions differently from what text on a board means.
glyphs[ord("'")] = Glyph(-4, 4, [[(0, -12), (0, -7)]])                 # straight apostrophe
glyphs[ord('`')] = Glyph(-4, 4, [[(-1, -12), (1, -9)]])                # grave accent
glyphs[ord('^')] = Glyph(-7, 7, [[(-4, -7), (0, -12), (4, -7)]])       # caret, not an arrow

# ---- Greek, micro, ohm --------------------------------------------------------------------
GREEK_UPPER = 'ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ'   # at A..X of the Greek set
GREEK_LOWER = 'αβγδεζηθικλμνξοπρστυφχψω'   # at a..x
for i, ch in enumerate(GREEK_UPPER):
    glyphs[ord(ch)] = greek[ord('A') - 32 + i].copy()
for i, ch in enumerate(GREEK_LOWER):
    glyphs[ord(ch)] = greek[ord('a') - 32 + i].copy()
glyphs[0x03C2] = glyphs[ord('σ')].copy()   # final sigma
glyphs[0x00B5] = glyphs[ord('μ')].copy()   # micro sign
glyphs[0x2126] = glyphs[ord('Ω')].copy()   # ohm sign

# ---- accents ------------------------------------------------------------------------------
def dot(x, y):
    """A small closed diamond, the way the Hershey data draws dots."""
    return [(x, y - 1), (x + 1, y), (x, y + 1), (x - 1, y), (x, y - 1)]


def accent(mark, cx, y0):
    """Strokes of an accent above a letter: centred on cx, bottom edge at y0."""
    if mark == 'acute':
        return [[(cx - 1, y0), (cx + 2, y0 - 3)]]
    if mark == 'grave':
        return [[(cx - 2, y0 - 3), (cx + 1, y0)]]
    if mark == 'circumflex':
        return [[(cx - 3, y0), (cx, y0 - 3), (cx + 3, y0)]]
    if mark == 'caron':
        return [[(cx - 3, y0 - 3), (cx, y0), (cx + 3, y0 - 3)]]
    if mark == 'tilde':
        return [[(cx - 4, y0), (cx - 3, y0 - 2), (cx - 1, y0 - 2), (cx + 1, y0), (cx + 3, y0), (cx + 4, y0 - 2)]]
    if mark == 'macron':
        return [[(cx - 3, y0 - 1), (cx + 3, y0 - 1)]]
    if mark == 'breve':
        return [[(cx - 3, y0 - 3), (cx - 2, y0 - 1), (cx, y0), (cx + 2, y0 - 1), (cx + 3, y0 - 3)]]
    if mark == 'diaeresis':
        return [dot(cx - 3, y0 - 1), dot(cx + 3, y0 - 1)]
    if mark == 'dot':
        return [dot(cx, y0 - 1)]
    if mark == 'ring':
        return [[(cx, y0 - 3), (cx + 1, y0 - 3), (cx + 2, y0 - 2), (cx + 2, y0 - 1), (cx + 1, y0),
                 (cx, y0), (cx - 1, y0), (cx - 2, y0 - 1), (cx - 2, y0 - 2), (cx - 1, y0 - 3), (cx, y0 - 3)]]
    if mark == 'doubleacute':
        return [[(cx - 3, y0), (cx - 1, y0 - 3)], [(cx + 1, y0), (cx + 3, y0 - 3)]]
    raise KeyError(mark)


def below(mark, cx, right):
    """Strokes of a mark under the baseline (y = 9)."""
    if mark == 'cedilla':
        return [[(cx, 9), (cx, 11), (cx + 2, 12), (cx + 1, 14), (cx - 2, 14)]]
    if mark == 'ogonek':
        x = right - 1
        return [[(x, 9), (x - 2, 11), (x - 1, 13), (x + 1, 13)]]
    raise KeyError(mark)


COMBINING = {
    0x0300: 'grave', 0x0301: 'acute', 0x0302: 'circumflex', 0x0303: 'tilde', 0x0304: 'macron',
    0x0306: 'breve', 0x0307: 'dot', 0x0308: 'diaeresis', 0x030A: 'ring', 0x030B: 'doubleacute',
    0x030C: 'caron', 0x0327: 'cedilla', 0x0328: 'ogonek', 0x0326: 'cedilla',  # comma below drawn as cedilla
}
ASCENDERS = set('bdfhklt')


def dotless(ch):
    """i and j without their dot (strokes entirely above the x-height)."""
    g = ascii_glyph(ch).copy()
    g.strokes = [s for s in g.strokes if max(p[1] for p in s) >= -6]
    return g


glyphs[0x0131] = dotless('i')   # ı dotless i (Turkish)
glyphs[0x0237] = dotless('j')   # ȷ dotless j


def compose(base_ch, mark):
    base = dotless(base_ch) if base_ch in 'ij' and mark not in ('cedilla', 'ogonek') else ascii_glyph(base_ch).copy()
    x0, x1, y0, y1 = base.ink()
    cx = int(round((x0 + x1) / 2))
    g = base.copy()

    if mark in ('cedilla', 'ogonek'):
        g.strokes += below(mark, cx, x1)
        return g

    if mark == 'caron' and base_ch in 'dlt':
        # ď ľ ť: the caron becomes an apostrophe after the ascender
        g.strokes.append([(x1 + 3, -12), (x1 + 2, -9)])
        g.right += 2
        return g

    tall = base_ch.isupper() or base_ch in ASCENDERS
    g.strokes += accent(mark, cx, -14 if tall else -8)
    return g


def letter_variants():
    for code in list(range(0x00C0, 0x0250)):
        ch = chr(code)
        dec = unicodedata.decomposition(ch)
        if not dec or dec.startswith('<'):
            continue
        parts = [int(p, 16) for p in dec.split()]
        if len(parts) != 2 or not (0x41 <= parts[0] <= 0x7A) or parts[1] not in COMBINING:
            continue
        base = chr(parts[0])
        if not base.isalpha():
            continue
        yield code, base, COMBINING[parts[1]]


for code, base, mark in letter_variants():
    glyphs[code] = compose(base, mark)

# İ (dotted capital I, Turkish) decomposes to I + dot above: covered above. Check it is there.
assert 0x0130 in glyphs


def with_extra(base_ch, extra, widen=0):
    g = ascii_glyph(base_ch).copy()
    g.strokes += extra
    g.right += widen
    return g


def ink_of(ch):
    return ascii_glyph(ch).ink()


# letters with a stroke or bar (no decomposition in Unicode)
x0, x1, y0, y1 = ink_of('O')
glyphs[0x00D8] = with_extra('O', [[(x0 - 1, y1 + 1), (x1 + 1, y0 - 1)]])            # Ø
x0, x1, y0, y1 = ink_of('o')
glyphs[0x00F8] = with_extra('o', [[(x0 - 1, y1 + 1), (x1 + 1, y0 - 1)]])            # ø
x0, x1, y0, y1 = ink_of('D')
glyphs[0x0110] = with_extra('D', [[(x0 - 3, -2), (x0 + 4, -2)]])                    # Đ
glyphs[0x00D0] = glyphs[0x0110].copy()                                               # Ð
x0, x1, y0, y1 = ink_of('d')
glyphs[0x0111] = with_extra('d', [[(x1 - 4, -9), (x1 + 2, -9)]], 1)                 # đ
x0, x1, y0, y1 = ink_of('H')
glyphs[0x0126] = with_extra('H', [[(x0 - 2, -8), (x1 + 2, -8)]])                    # Ħ
x0, x1, y0, y1 = ink_of('h')
glyphs[0x0127] = with_extra('h', [[(x0 - 3, -9), (x0 + 3, -9)]])                    # ħ
x0, x1, y0, y1 = ink_of('L')
glyphs[0x0141] = with_extra('L', [[(x0 - 3, 0), (x0 + 4, -5)]])                     # Ł
x0, x1, y0, y1 = ink_of('l')
glyphs[0x0142] = with_extra('l', [[(x0 - 3, -1), (x0 + 3, -6)]])                    # ł


def ligature(a, b, overlap):
    ga, gb = ascii_glyph(a), ascii_glyph(b)
    shift = ga.right - gb.left - overlap
    return Glyph(ga.left, gb.right + shift, ga.strokes + gb.moved(shift, 0))


glyphs[0x00C6] = ligature('A', 'E', 6)   # Æ
glyphs[0x00E6] = ligature('a', 'e', 5)   # æ
glyphs[0x0152] = ligature('O', 'E', 5)   # Œ
glyphs[0x0153] = ligature('o', 'e', 5)   # œ

# ß (sharp s), drawn here
glyphs[0x00DF] = Glyph(-9, 9, [
    [(-5, 9), (-5, -7), (-4, -10), (-2, -12), (1, -12), (3, -11), (4, -9), (4, -7), (3, -5), (0, -4)],
    [(0, -4), (3, -3), (5, -1), (5, 3), (4, 6), (2, 8), (0, 9), (-2, 9)],
])

# ---- symbols ------------------------------------------------------------------------------
glyphs[0x00A0] = glyphs[0x20].copy()                    # no-break space
glyphs[0x00B0] = roman[95].copy()                       # degree (small circle of the set)

px0, px1, py0, py1 = ink_of('+')
pc = (py0 + py1) // 2
glyphs[0x00B1] = with_extra('+', [[(px0, py1 + 2), (px1, py1 + 2)]])                # ±
glyphs[0x00B1].strokes = [[(x, y - 2) for x, y in s] for s in ascii_glyph('+').strokes] + \
    [[(px0, py1 + 1), (px1, py1 + 1)]]
r = (px1 - px0) // 3
glyphs[0x00D7] = Glyph(ascii_glyph('+').left, ascii_glyph('+').right,              # ×
                       [[(-r, pc - r), (r, pc + r)], [(r, pc - r), (-r, pc + r)]])
glyphs[0x00F7] = Glyph(ascii_glyph('+').left, ascii_glyph('+').right,              # ÷
                       [[(px0, pc), (px1, pc)], dot(0, pc - 5), dot(0, pc + 5)])

mx0, mx1, my0, my1 = ink_of('-')
dash_y = my0
glyphs[0x2010] = ascii_glyph('-').copy()                                             # ‐ hyphen
glyphs[0x2212] = Glyph(px0 - 4, px1 + 4, [[(px0, pc), (px1, pc)]])                   # − minus
glyphs[0x2013] = Glyph(-10, 10, [[(-7, dash_y), (7, dash_y)]])                       # – en dash
glyphs[0x2014] = Glyph(-14, 14, [[(-12, dash_y), (12, dash_y)]])                     # — em dash
glyphs[0x2018] = roman[ord('`') - 32].copy()                                         # ‘
glyphs[0x2019] = roman[ord("'") - 32].copy()                                         # ’
q = roman[ord('`') - 32]
glyphs[0x201C] = Glyph(q.left - 2, q.right + 2, q.moved(-2, 0) + q.moved(2, 0))      # “
q = roman[ord("'") - 32]
glyphs[0x201D] = Glyph(q.left - 2, q.right + 2, q.moved(-2, 0) + q.moved(2, 0))      # ”
glyphs[0x2022] = Glyph(-5, 5, [[(0, 0), (1, 0), (2, 1), (2, 2), (1, 3), (0, 3), (-1, 3), (-2, 2),
                                (-2, 1), (-1, 0), (0, 0)], [(0, 1), (1, 2), (0, 2), (-1, 1), (0, 1)]])  # •
p = ascii_glyph('.')
glyphs[0x2026] = Glyph(-12, 12, p.moved(-7, 0) + p.moved(0, 0) + p.moved(7, 0))      # …

lx0, lx1, ly0, ly1 = ink_of('<')
glyphs[0x2264] = Glyph(ascii_glyph('<').left, ascii_glyph('<').right,               # ≤
                       [[(x, y - 2) for x, y in s] for s in ascii_glyph('<').strokes] + [[(lx0, ly1 + 1), (lx1, ly1 + 1)]])
gx0, gx1, gy0, gy1 = ink_of('>')
glyphs[0x2265] = Glyph(ascii_glyph('>').left, ascii_glyph('>').right,               # ≥
                       [[(x, y - 2) for x, y in s] for s in ascii_glyph('>').strokes] + [[(gx0, gy1 + 1), (gx1, gy1 + 1)]])
t = ascii_glyph('~')
tx0, tx1, ty0, ty1 = t.ink()
tc = (ty0 + ty1) // 2
glyphs[0x2248] = Glyph(t.left, t.right, t.moved(0, pc - tc - 3) + t.moved(0, pc - tc + 3))   # ≈

cx0, cx1, cy0, cy1 = ink_of('C')
glyphs[0x20AC] = with_extra('C', [[(cx0 - 3, -4), (cx0 + 7, -4)], [(cx0 - 3, 0), (cx0 + 7, 0)]])  # €


def circled(letter):
    """© / ®: a circle around a small capital."""
    import math
    ring = []
    for k in range(25):
        a = 2 * math.pi * k / 24
        ring.append((round(10 * math.cos(a)), round(-1.5 + 10 * math.sin(a))))
    inner = []
    for s in ascii_glyph(letter).strokes:
        inner.append([(round(x * 0.55), round(-1.5 + (y + 1.5) * 0.55)) for x, y in s])
    return Glyph(-13, 13, [ring] + inner)


glyphs[0x00A9] = circled('C')   # ©
glyphs[0x00AE] = circled('R')   # ®

# ---- output -------------------------------------------------------------------------------
def c_string(s):
    out = '"'
    for ch in s:
        if ch in '\\"?':
            out += '\\' + ch
        else:
            out += ch
    return out + '"'


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'core', 'src', 'stroke_glyphs.cpp')
    lines = [
        '/*',
        ' * Netloom - PCB design for iPad',
        ' * Copyright (C) 2026 Seyhan Labs',
        ' *',
        ' * GENERATED by tools/gen_stroke_font.py - do not edit.',
        ' *',
        ' * Stroke font glyphs. Latin and Greek letter shapes come from the Hershey font data:',
        ' *   The Hershey Fonts were originally created by Dr. A. V. Hershey while working at the',
        ' *   U. S. National Bureau of Standards.',
        ' *   The format of the Font data in this distribution was originally created by',
        ' *   James Hurt, Cognition, Inc., 900 Technology Park Drive, Billerica, MA 01821.',
        ' * (full notice: core/third_party/hershey/NOTICE). Accented letters and symbols were',
        ' * composed or drawn by the generator.',
        ' */',
        '#include "stroke_glyphs.h"',
        '',
        'namespace kp',
        '{',
        '',
        'const StrokeGlyphData g_strokeGlyphs[] = {',
    ]
    for code in sorted(glyphs):
        name = unicodedata.name(chr(code), '') if code > 32 else 'SPACE'
        lines.append('    { 0x%04X, %s }, // %s' % (code, c_string(glyphs[code].encode()), name.lower()))
    lines += [
        '};',
        '',
        'const int g_strokeGlyphCount = (int) ( sizeof( g_strokeGlyphs ) / sizeof( g_strokeGlyphs[0] ) );',
        '',
        '} // namespace kp',
        '',
    ]
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
    print('%d glyphs -> %s' % (len(glyphs), path))


if __name__ == '__main__':
    main()
