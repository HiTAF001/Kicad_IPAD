import os
"""Compare Netloom DRC with KiCad 7's per board and category.
usage: drc_compare.py kpcli kicad_drc.json [board-substring]"""
import sys, json, subprocess, re, collections, os
KPCLI, ORACLE = sys.argv[1], sys.argv[2]
flt = sys.argv[3] if len(sys.argv) > 3 else ''
CATS = ['clearance', 'shorting_items', 'tracks_crossing', 'hole_clearance', 'copper_edge_clearance', 'track_width',
        'via_diameter', 'annular_width', 'drill_out_of_range', 'microvia_drill_out_of_range', 'hole_near_hole',
        'holes_co_located', 'courtyards_overlap', 'malformed_courtyard', 'unconnected_items', 'track_dangling',
        'via_dangling', 'invalid_outline']
d = json.load(open(ORACLE))
tot_k = collections.Counter(); tot_o = collections.Counter(); match = collections.Counter(); boards_bad = collections.Counter()
for path, v in d.items():
    if flt and flt not in path: continue
    k = collections.Counter(it['cat'] for it in v['items'])
    r = subprocess.run([KPCLI, 'drc', path, '--quiet'], capture_output=True, text=True, timeout=300)
    o = collections.Counter({m.group(1): int(m.group(2)) for m in re.finditer(r'count (\w+) (\d+)', r.stdout)})
    summ = re.search(r'summary .*', r.stdout)
    diffs = []
    for c in CATS:
        tot_k[c] += k[c]; tot_o[c] += o[c]
        if k[c] == o[c]: match[c] += 1
        else:
            diffs.append('%s kicad=%d ours=%d' % (c, k[c], o[c])); boards_bad[c] += 1
    if diffs:
        print(os.path.relpath(path, os.path.join(os.environ.get('NETLOOM_WORK', 'work'), 'src/kicad/qa')), '|', '; '.join(diffs), '|', summ.group(0)[8:60] if summ else r.stderr[-100:])
print()
print('%-28s %7s %7s %s' % ('category', 'kicad', 'ours', 'boards-mismatch'))
for c in CATS:
    print('%-28s %7d %7d %d' % (c, tot_k[c], tot_o[c], boards_bad[c]))
