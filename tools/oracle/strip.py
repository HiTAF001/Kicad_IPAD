"""Write a copy of a board without tracks, arcs, vias and zones (top-level items only)."""
import sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src, encoding='utf-8', errors='surrogateescape').read()
out = []; i = 0; depth = 0; n = len(t); start = None; drop = False
while i < n:
    c = t[i]
    if c == '"':
        j = i + 1
        while j < n and t[j] != '"':
            j += 2 if t[j] == '\\' else 1
        if not drop: out.append(t[i:j + 1])
        i = j + 1; continue
    if c == '(':
        depth += 1
        if depth == 2:
            k = i + 1
            while k < n and t[k] not in ' \t\r\n()': k += 1
            head = t[i + 1:k]
            drop = head in ('segment', 'arc', 'via', 'zone')
    if not drop: out.append(c)
    if c == ')':
        if depth == 2 and drop:
            drop = False
        depth -= 1
    i += 1
open(dst, 'w', encoding='utf-8', errors='surrogateescape').write(''.join(out))
