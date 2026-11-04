import os
"""Pair-level comparison of geometry checks (report-all mode on both sides).
usage: drc_pairs.py kpcli kicad_drc_all.json [filter]"""
import sys, json, subprocess, re, collections, os
KPCLI, ORACLE = sys.argv[1], sys.argv[2]
flt = sys.argv[3] if len(sys.argv) > 3 else ''
CATS = {'clearance', 'shorting_items', 'tracks_crossing', 'copper_edge_clearance', 'hole_clearance'}

def sig_kicad(desc):
    m = re.match(r'(?:PTH |SMD |NPTH |Connector |)[Pp]ad (\S+) \[([^\]]*)\] of (\S+)', desc)
    if m: return ('pad', m.group(3) + '.' + m.group(1))
    m = re.match(r'(Track \(arc\)|Track|(?:Blind/Buried |Micro)?Via|Zone) \[([^\]]*)\](?: on (\S+))?', desc)
    if m:
        k = {'Track (arc)': 'track', 'Track': 'track', 'Zone': 'zone'}.get(m.group(1), 'via')
        net = '' if m.group(2) == '<no net>' else m.group(2)
        return (k, net)
    if ' on Edge.Cuts' in desc or ' on Margin' in desc: return None
    return ('graphic', '')

def sig_ours(desc):
    m = re.match(r'Pad (\S+) of (\S+) \((.*)\)$', desc)
    if m: return ('pad', m.group(2) + '.' + m.group(1))
    m = re.match(r'(Arc track|Track) \((.*)\) on \S+$', desc) or re.match(r'(Via|Zone) \((.*)\)$', desc)
    if m:
        k = {'Arc track': 'track', 'Track': 'track', 'Zone': 'zone', 'Via': 'via'}[m.group(1)]
        net = '' if m.group(2) == 'no net' else m.group(2)
        return (k, net)
    return ('graphic', '')

d = json.load(open(ORACLE))
tk = to = tmatch = 0
for path, v in sorted(d.items()):
    if flt and flt not in path: continue
    if os.path.exists(path[:-10] + '.kicad_dru'): continue
    K = collections.Counter()
    for it in v['items']:
        if it['cat'] not in CATS: continue
        sigs = tuple(sorted(s for s in (sig_kicad(p[2]) for p in it['pos']) if s))
        K[(it['cat'], sigs)] += 1
    r = subprocess.run([KPCLI, 'drc', path, '--all'], capture_output=True, text=True, timeout=600)
    O = collections.Counter()
    for line in r.stdout.splitlines():
        m = re.match(r'(\w+)\s+[EW] \(.*?\| (.*)$', line)
        if not m or m.group(1) not in CATS: continue
        parts = m.group(2).split(' — ', 1)
        items = parts[1].split(' / ') if len(parts) > 1 else []
        sigs = tuple(sorted(sig_ours(x) for x in items))
        O[(m.group(1), sigs)] += 1
    common = sum((K & O).values())
    tk += sum(K.values()); to += sum(O.values()); tmatch += common
    if K != O:
        print('==', os.path.relpath(path, os.path.join(os.environ.get('NETLOOM_WORK', 'work'), 'src/kicad/qa')),
              'kicad', sum(K.values()), 'ours', sum(O.values()), 'common', common)
        for k, n in (K - O).most_common(4): print('   only kicad', n, k)
        for k, n in (O - K).most_common(4): print('   only ours ', n, k)
print('TOTAL kicad', tk, 'ours', to, 'matched', tmatch)
