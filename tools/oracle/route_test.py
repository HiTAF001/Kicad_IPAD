"""Pencil routing test: synthetic hand-drawn strokes between unconnected pads.

For each board, take Netloom airwires whose ends are pads sharing an outer copper layer, draw a
wobbly stroke between them (straight, L, 45-degree dog-leg or curved), route it with kpcli and
check:
  * the route commits (unless it would short; then it must say so),
  * every segment is horizontal, vertical or 45 degrees,
  * the ends sit exactly on the pad centres,
  * the clean track stays close to the stroke,
  * KiCad loads the result, the tracks carry the pad's net, and KiCad's unconnected count
    drops by one (when the route has no shorts).
usage: route_test.py kpcli outdir seed boards...
"""
import sys, os, re, math, random, subprocess, json
sys.path.insert(0, '/usr/lib/python3/dist-packages')
import pcbnew

KPCLI, OUT, SEED = sys.argv[1], sys.argv[2], int(sys.argv[3])
PER = int(os.environ.get('PER', '1'))
rng = random.Random(SEED)
os.makedirs(OUT, exist_ok=True)
stats = dict(ok=0, bad=0, skip=0, shorts=0)
report = []


def ours(path):
    r = subprocess.run([KPCLI, 'stats', path], capture_output=True, text=True)
    m = re.search(r'unconnected \(airwires\): (\d+)', r.stdout)
    wires = [tuple(map(float, w)) for w in re.findall(r'airwire \(([-\d.]+), ([-\d.]+)\) -> \(([-\d.]+), ([-\d.]+)\)', r.stdout)]
    return (int(m.group(1)) if m else -1), wires


def wobble(pts, amp, step):
    """resample at ~step and add low-frequency hand wobble plus jitter"""
    out = []
    total = sum(math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1))
    ph = [rng.uniform(0, 6.3) for _ in range(4)]
    fr = [rng.uniform(0.15, 0.6), rng.uniform(0.8, 2.0)]
    s = 0
    for i in range(len(pts) - 1):
        a, b = pts[i], pts[i + 1]
        L = math.dist(a, b)
        n = max(1, int(L / step))
        for k in range(n):
            t = k / n
            x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
            u = s + L * t
            w = amp * (math.sin(u * fr[0] + ph[0]) * 0.7 + math.sin(u * fr[1] + ph[1]) * 0.3)
            nx, ny = (-(b[1] - a[1]) / L, (b[0] - a[0]) / L) if L > 0 else (0, 0)
            env = min(1.0, u / 1.0, (total - u) / 1.0)  # calmer near the ends
            out.append((x + nx * w * env + rng.gauss(0, amp * 0.15), y + ny * w * env + rng.gauss(0, amp * 0.15)))
        s += L
    out.append(pts[-1])
    return out


def make_stroke(a, b, style):
    dx, dy = b[0] - a[0], b[1] - a[1]
    if style == 'straight':
        pts = [a, b]
    elif style == 'L':
        pts = [a, (b[0], a[1]), b] if rng.random() < 0.5 else [a, (a[0], b[1]), b]
    elif style == 'dogleg':
        d = min(abs(dx), abs(dy))
        sx, sy = math.copysign(1, dx), math.copysign(1, dy)
        if abs(dx) > abs(dy):
            m = (abs(dx) - d) / 2
            pts = [a, (a[0] + sx * m, a[1]), (a[0] + sx * (m + d), a[1] + sy * d), b]
        else:
            m = (abs(dy) - d) / 2
            pts = [a, (a[0], a[1] + sy * m), (a[0] + sx * d, a[1] + sy * (m + d)), b]
    else:  # curve: quadratic bezier bulging sideways
        L = math.hypot(dx, dy)
        c = ((a[0] + b[0]) / 2 - dy / L * L * 0.25, (a[1] + b[1]) / 2 + dx / L * L * 0.25)
        pts = [((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * c[0] + t * t * b[0],
                (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * c[1] + t * t * b[1]) for t in [k / 24 for k in range(25)]]
    # hand starts and ends a little off the pad centre
    off = lambda: (rng.uniform(-0.15, 0.15), rng.uniform(-0.15, 0.15))
    o1, o2 = off(), off()
    pts[0] = (pts[0][0] + o1[0], pts[0][1] + o1[1])
    pts[-1] = (pts[-1][0] + o2[0], pts[-1][1] + o2[1])
    return wobble(pts, rng.uniform(0.03, 0.12), rng.uniform(0.15, 0.4))


def seg_dist(p, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    l2 = dx * dx + dy * dy
    t = 0 if l2 == 0 else max(0, min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2))
    return math.dist(p, (a[0] + t * dx, a[1] + t * dy))


def poly_dist(p, pts):
    return min(seg_dist(p, pts[i], pts[i + 1]) for i in range(len(pts) - 1))


for path in sys.argv[4:]:
    try:
        b = pcbnew.LoadBoard(path)
    except Exception:
        stats['skip'] += 1
        continue
    b.BuildConnectivity()
    before = b.GetConnectivity().GetUnconnectedCount(True)
    pads = list(b.GetPads())

    def padAt(x, y):
        for p in pads:
            q = p.GetPosition()
            if abs(pcbnew.ToMM(q.x) - x) < 0.0015 and abs(pcbnew.ToMM(q.y) - y) < 0.0015:
                return p

    _, wires = ours(path)
    cands = []
    for (x1, y1, x2, y2) in wires:
        s, t = padAt(x1, y1), padAt(x2, y2)
        if not s or not t or s.GetNetCode() != t.GetNetCode():
            continue
        if s.GetOffset().x != 0 or s.GetOffset().y != 0 or t.GetOffset().x != 0 or t.GetOffset().y != 0:
            continue
        common = [l for l in (pcbnew.F_Cu, pcbnew.B_Cu) if s.IsOnLayer(l) and t.IsOnLayer(l)]
        if common and 1.0 < math.dist((x1, y1), (x2, y2)) < 60:
            cands.append((s, t, common[0], (x1, y1), (x2, y2)))
    if not cands:
        stats['skip'] += 1
        continue
    for trial in range(min(PER, len(cands))):
        s, t, layer, A, B = cands.pop(rng.randrange(len(cands)))
        style = rng.choice(['straight', 'L', 'dogleg', 'curve'])
        stroke = make_stroke(A, B, style)
        lname = b.GetStandardLayerName(layer)
        out = os.path.join(OUT, '%s_%d.kicad_pcb' % (os.path.basename(path)[:-10], trial))
        args = [KPCLI, 'edit', path, out, 'route', lname, '0.25', str(len(stroke))]
        for (x, y) in stroke:
            args += ['%.4f' % x, '%.4f' % y]
        r = subprocess.run(args, capture_output=True, text=True)
        txt = r.stdout
        segs = [tuple(map(float, m)) for m in re.findall(r'seg \(([-\d.]+), ([-\d.]+)\) -> \(([-\d.]+), ([-\d.]+)\)', txt)]
        head = re.search(r'route .*', txt)
        head = head.group(0) if head else txt[:200]
        probs = []
        shorts = 'shorts=1' in head
        committed = 'commit: ok' in txt
        if not segs:
            probs.append('no segments')
        else:
            for (x1, y1, x2, y2) in segs:
                ang = math.degrees(math.atan2(y2 - y1, x2 - x1)) % 45
                seglen = max(1e-9, math.dist((x1, y1), (x2, y2)))
                if min(ang, 45 - ang) > math.degrees(4e-5 / seglen) + 0.001:  # C API returns floats
                    probs.append('angle %.3f' % ang)
                    break
            if math.dist(segs[0][:2], A) > 1e-3 or math.dist(segs[-1][2:], B) > 1e-3:
                probs.append('ends off pads %s %s' % (segs[0][:2], segs[-1][2:]))
            for i in range(len(segs) - 1):
                if math.dist(segs[i][2:], segs[i + 1][:2]) > 1e-6:
                    probs.append('gap')
                    break
            clean = [segs[0][:2]] + [sg[2:] for sg in segs]
            dev = max(poly_dist(p, clean) for p in stroke)
            dev2 = max(poly_dist(p, stroke) for p in clean)
            # a straight stroke at an odd angle can only be followed with bends
            dx, dy = abs(B[0] - A[0]), abs(B[1] - A[1])
            D = math.hypot(dx, dy)
            two = abs(dx - dy) * min(dx, dy) / D if D > 0 else 0  # offset of a single-bend corner
            allowed = max(1.2, two / 2 + 0.7) if style == 'straight' else (1.5 if style == 'curve' else 1.2)
            if max(dev, dev2) > allowed:
                probs.append('deviation %.2f/%.2f (allowed %.2f)' % (dev, dev2, allowed))
        if shorts:
            stats['shorts'] += 1
            if committed and 'blocked=1' not in head:
                pass  # shorts are only flagged, not blocked (yet)
        if not committed and not ('blocked=1' in head):
            probs.append('not committed: ' + head)
        if committed and not probs:
            try:
                g = pcbnew.LoadBoard(out)
                g.BuildConnectivity()
                after = g.GetConnectivity().GetUnconnectedCount(True)
                mine, _ = ours(out)
                if mine != after:
                    probs.append('unconnected kicad %d vs ours %d' % (after, mine))
                if not shorts and after != before - 1:
                    probs.append('kicad unconnected %d -> %d' % (before, after))
                netname = s.GetNetname()
                newt = [x for x in g.GetTracks() if x.Type() == pcbnew.PCB_TRACE_T and x.GetWidth() == pcbnew.FromMM(0.25)
                        and x.GetLayer() == layer and x.GetNetname() == netname]
                if len(newt) < len(segs):
                    probs.append('tracks with net %d < %d' % (len(newt), len(segs)))
            except Exception as e:
                probs.append('kicad load: %s' % e)
        rec = dict(board=path, out=out, style=style, A=A, B=B, stroke=stroke, segs=segs, head=head, probs=probs)
        report.append(rec)
        if probs:
            stats['bad'] += 1
            print('BAD', os.path.basename(path), style, '; '.join(probs), '|', head[:160])
        else:
            stats['ok'] += 1
            print('ok ', os.path.basename(path), style, len(segs), 'segs', 'SHORTS' if shorts else '')
json.dump(report, open(os.path.join(OUT, 'report.json'), 'w'))
print(stats)
