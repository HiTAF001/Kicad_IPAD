"""For each board: take one of Netloom's airwires whose ends are pads sharing an outer copper
layer, add a track along it (and a via at one end) with kpcli, then check with KiCad that the
items exist with the right net/layer/width and that KiCad's and Netloom's unconnected counts
agree (and dropped by one). Boards without such an airwire get a track between two pads of one
net instead, which still exercises the writer."""
import sys, subprocess, os, re
sys.path.insert(0, '/usr/lib/python3/dist-packages')
import pcbnew

KPCLI, OUT = sys.argv[1], sys.argv[2]
ok = bad = skip = 0

def ours(path):
    r = subprocess.run([KPCLI, 'stats', path], capture_output=True, text=True)
    m = re.search(r'unconnected \(airwires\): (\d+)', r.stdout)
    wires = [tuple(map(float, w)) for w in re.findall(r'airwire \(([-\d.]+), ([-\d.]+)\) -> \(([-\d.]+), ([-\d.]+)\)', r.stdout)]
    return (int(m.group(1)) if m else -1), wires

for path in sys.argv[3:]:
    try:
        b = pcbnew.LoadBoard(path)
    except Exception:
        skip += 1; continue
    b.BuildConnectivity()
    before = b.GetConnectivity().GetUnconnectedCount(True)
    pads = list(b.GetPads())
    def padAt(x, y):
        for p in pads:
            q = p.GetPosition()
            if abs(pcbnew.ToMM(q.x) - x) < 0.0015 and abs(pcbnew.ToMM(q.y) - y) < 0.0015:
                return p
    pick = None
    _, wires = ours(path)
    for (x1, y1, x2, y2) in wires:
        s, t = padAt(x1, y1), padAt(x2, y2)
        if not s or not t or s.GetNetCode() != t.GetNetCode(): continue
        common = [l for l in (pcbnew.F_Cu, pcbnew.B_Cu) if s.IsOnLayer(l) and t.IsOnLayer(l)]
        if common:
            pick = (s, t, common[0], True); break
    if not pick:
        bynet = {}
        for p in pads:
            if p.GetNetCode() > 0: bynet.setdefault(p.GetNetCode(), []).append(p)
        for net, ps in sorted(bynet.items()):
            for t in ps[1:]:
                common = [l for l in (pcbnew.F_Cu, pcbnew.B_Cu) if ps[0].IsOnLayer(l) and t.IsOnLayer(l)]
                if common and ps[0].GetPosition() != t.GetPosition():
                    pick = (ps[0], t, common[0], False); break
            if pick: break
    if not pick:
        skip += 1; continue
    s, t, layer, airwire = pick
    a, c, netname = s.GetPosition(), t.GetPosition(), s.GetNetname()
    out = os.path.join(OUT, os.path.basename(path))
    args = [KPCLI, 'edit', path, out, 'track', '%.6f' % pcbnew.ToMM(a.x), '%.6f' % pcbnew.ToMM(a.y),
            '%.6f' % pcbnew.ToMM(c.x), '%.6f' % pcbnew.ToMM(c.y), '0.2', b.GetStandardLayerName(layer), netname,
            'addvia', '%.6f' % pcbnew.ToMM(a.x), '%.6f' % pcbnew.ToMM(a.y), '0.6', '0.3', netname]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0 or 'FAILED' in r.stdout:
        print('KPCLI FAIL', path, r.stdout[-300:], r.stderr[-300:]); bad += 1; continue
    try:
        g = pcbnew.LoadBoard(out)
    except Exception as e:
        print('KICAD LOAD FAIL', out, e); bad += 1; continue
    g.BuildConnectivity()
    after = g.GetConnectivity().GetUnconnectedCount(True)
    mine, _ = ours(out)
    tracks = [x for x in g.GetTracks() if x.Type() == pcbnew.PCB_TRACE_T]
    vias = [x for x in g.GetTracks() if x.Type() == pcbnew.PCB_VIA_T]
    nt = [x for x in tracks if x.GetStart() == a and x.GetEnd() == c]
    nv = [v for v in vias if v.GetPosition() == a]
    probs = []
    if len(nt) != 1: probs.append('track missing (%d)' % len(nt))
    else:
        x = nt[0]
        if x.GetNetname() != netname: probs.append('net %r vs %r' % (x.GetNetname(), netname))
        if x.GetLayer() != layer: probs.append('layer')
        if x.GetWidth() != pcbnew.FromMM(0.2): probs.append('width')
    if len(nv) != 1 or nv[0].GetNetname() != netname: probs.append('via')
    if mine != after: probs.append('unconnected kicad %d vs ours %d' % (after, mine))
    if airwire and after != before - 1: probs.append('kicad unconnected %d -> %d' % (before, after))
    if probs:
        print('MISMATCH', os.path.basename(path), '; '.join(probs)); bad += 1
    else:
        ok += 1
        print('ok', os.path.basename(path), 'airwire' if airwire else 'pair', before, '->', after)
print('ok', ok, 'bad', bad, 'skip', skip)
