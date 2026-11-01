#!/usr/bin/env python3
# Netloom - PCB design for iPad
# Copyright (C) 2026 Seyhan Labs
#
# This program is free software: you can redistribute it and/or modify it under the terms of
# the GNU General Public License as published by the Free Software Foundation, either
# version 3 of the License, or (at your option) any later version.
"""
Seyhan Labs logo lockups: the badge (branding/seyhan-labs-mark.png) with the SEYHAN LABS
wordmark set in Netloom's own stroke font underneath.

Writes branding/seyhan-labs-logo-light.png (transparent, dark lettering) and
branding/seyhan-labs-logo-dark.png (dark background, light lettering).
"""
import math
import os
import re

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRANDING = os.path.join(ROOT, 'branding')
SS = 3                                   # supersampling for smooth strokes


def stroke_font():
    src = open(os.path.join(ROOT, 'core', 'src', 'stroke_glyphs.cpp'), encoding='utf-8').read()
    table = {}
    for m in re.finditer(r'\{ 0x([0-9A-F]+), "((?:[^"\\]|\\.)*)" \}', src):
        table[int(m.group(1), 16)] = re.sub(r'\\(.)', r'\1', m.group(2))
    return table


def text_paths(text, x, y, size, font):
    """Polylines of text; (x, y) = left end of the baseline."""
    k = size / 21.0
    paths = []
    for ch in text:
        g = font.get(ord(ch), font[ord('?')])
        left, right = ord(g[0]) - 82, ord(g[1]) - 82
        cur = []
        for i in range(2, len(g) - 1, 2):
            pair = g[i:i + 2]
            if pair == ' R':
                if len(cur) > 1:
                    paths.append(cur)
                cur = []
                continue
            cur.append((x + (ord(pair[0]) - 82 - left) * k, y + (ord(pair[1]) - 82 - 9) * k))
        if len(cur) > 1:
            paths.append(cur)
        x += (right - left) * k
    return paths, x


def smooth(path, sharp_deg=55, rounds=3):
    """Round the faceted curves of the stroke font, keeping real corners sharp."""
    def turn(a, b, c):
        v1 = (b[0] - a[0], b[1] - a[1])
        v2 = (c[0] - b[0], c[1] - b[1])
        n1, n2 = math.hypot(*v1), math.hypot(*v2)
        if n1 == 0 or n2 == 0:
            return 0
        cosv = max(-1.0, min(1.0, (v1[0] * v2[0] + v1[1] * v2[1]) / (n1 * n2)))
        return math.degrees(math.acos(cosv))

    parts, cur = [], [path[0]]
    for i in range(1, len(path) - 1):
        cur.append(path[i])
        if turn(path[i - 1], path[i], path[i + 1]) > sharp_deg:
            parts.append(cur)
            cur = [path[i]]
    cur.append(path[-1])
    parts.append(cur)

    out = []
    for part in parts:
        pts = part
        for _ in range(rounds if len(part) > 2 else 0):
            q = [pts[0]]
            for a, b in zip(pts, pts[1:]):
                q.append((0.75 * a[0] + 0.25 * b[0], 0.75 * a[1] + 0.25 * b[1]))
                q.append((0.25 * a[0] + 0.75 * b[0], 0.25 * a[1] + 0.75 * b[1]))
            q.append(pts[-1])
            pts = q
        out.extend(pts if not out else pts[1:])
    return out


def lockup(background, ink, name):
    mark = Image.open(os.path.join(BRANDING, 'seyhan-labs-mark.png')).convert('RGBA')
    W = 1200
    m = 900
    mark = mark.resize((m, m), Image.LANCZOS)
    H = m + 330
    canvas = Image.new('RGBA', (W * SS, H * SS), background)
    draw = ImageDraw.Draw(canvas)

    font = stroke_font()
    size = 104 * SS
    width = text_paths('SEYHAN LABS', 0, 0, size, font)[1]
    paths, _ = text_paths('SEYHAN LABS', (W * SS - width) / 2, (m + 210) * SS, size, font)
    pen = int(11 * SS)
    for p in paths:
        pts = smooth(p)
        draw.line(pts, fill=ink, width=pen, joint='curve')
        for x, y in (pts[0], pts[-1]):
            draw.ellipse([x - pen / 2, y - pen / 2, x + pen / 2, y + pen / 2], fill=ink)

    canvas = canvas.resize((W, H), Image.LANCZOS)
    canvas.alpha_composite(mark, ((W - m) // 2, 30))
    canvas.save(os.path.join(BRANDING, name))
    print(name)


if __name__ == '__main__':
    lockup((0, 0, 0, 0), (24, 70, 44, 255), 'seyhan-labs-logo-light.png')
    lockup((0, 16, 35, 255), (233, 216, 166, 255), 'seyhan-labs-logo-dark.png')
