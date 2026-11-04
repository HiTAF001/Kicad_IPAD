"""Compare Netloom Gerbers/drill files with kicad-cli (KiCad 7) by rasterizing both with gerbv.
usage: fab_compare.py kpcli workdir boards...
Prints per board and layer the fraction of pixels that differ (XOR / union)."""
import sys, os, subprocess, re, shutil
import numpy as np
from PIL import Image

KPCLI, WORK = sys.argv[1], sys.argv[2]
LAYERS = ['F.Cu', 'B.Cu', 'F.Mask', 'B.Mask', 'F.Paste', 'B.Paste', 'F.SilkS', 'B.SilkS', 'Edge.Cuts']
os.makedirs(WORK, exist_ok=True)


def ours_name(base, layer):
    n = layer.replace('F.SilkS', 'F.Silkscreen').replace('B.SilkS', 'B.Silkscreen').replace('.', '_')
    return '%s-%s.gbr' % (base, n)


def kicad_files(d):
    out = {}
    for f in os.listdir(d):
        m = re.search(r'-(F_Cu|B_Cu|F_Mask|B_Mask|F_Paste|B_Paste|F_Silkscreen|B_Silkscreen|Edge_Cuts)\.\w+$', f)
        if m:
            out[m.group(1).replace('_Silkscreen', '_SilkS').replace('_', '.')] = os.path.join(d, f)
        if f.endswith('-PTH.drl'): out['PTH'] = os.path.join(d, f)
        if f.endswith('-NPTH.drl'): out['NPTH'] = os.path.join(d, f)
    return out


def raster(path, origin, size, out, dpi=400):
    r = subprocess.run(['gerbv', '-x', 'png', '-o', out, '-D', str(dpi), '-B', '0', '-O', '%fx%f' % origin,
                        '-W', '%fx%f' % size, '-f', '#FFFFFFFF', '-b', '#000000', path], capture_output=True, text=True, timeout=300)
    err = [l for l in (r.stderr or '').splitlines() if 'rror' in l or 'arning' in l]
    if not os.path.exists(out):
        return None, err
    a = np.asarray(Image.open(out).convert('L')) > 127
    return a, err


results = []
for board in sys.argv[3:]:
    base = os.path.basename(board)[:-10]
    od = os.path.join(WORK, base, 'ours'); kd = os.path.join(WORK, base, 'kicad')
    shutil.rmtree(os.path.join(WORK, base), ignore_errors=True)
    os.makedirs(od); os.makedirs(kd)
    subprocess.run([KPCLI, 'fab', board, od], capture_output=True)
    r = subprocess.run(['kicad-cli', 'pcb', 'export', 'gerbers', '--output', kd + '/', '--layers', ','.join(LAYERS), '--no-protel-ext', board], capture_output=True, text=True)
    subprocess.run(['kicad-cli', 'pcb', 'export', 'drill', '--output', kd + '/', '--format', 'excellon', '--excellon-separate-th',
                    '--excellon-units', 'mm', '--excellon-zeros-format', 'decimal', board], capture_output=True, text=True)
    kf = kicad_files(kd)
    code = ("import sys; sys.path.insert(0,'/usr/lib/python3/dist-packages'); import pcbnew; "
            "bb=pcbnew.LoadBoard(sys.argv[1]).ComputeBoundingBox(False); "
            "print(pcbnew.ToMM(bb.GetX()), pcbnew.ToMM(bb.GetY()), pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()))")
    vals = subprocess.run(['/usr/bin/python3', '-c', code, board], capture_output=True, text=True).stdout.split()[-4:]
    x0, y0, x1, y1 = map(float, vals)
    BOX = (x0 - 2, y0 - 2, x1 + 2, y1 + 2)
    line = []
    for layer in LAYERS + ['PTH', 'NPTH']:
        ours = os.path.join(od, ours_name(base, layer)) if layer not in ('PTH', 'NPTH') else os.path.join(od, '%s-%s.drl' % (base, layer))
        if layer not in kf or not os.path.exists(ours):
            line.append('%s:missing(%s)' % (layer, 'k' if layer not in kf else 'o')); continue
        # common window from both files' extents: let gerbv render the kicad one first to get bounds -> use fixed big window
        x0, y0, x1, y1 = BOX
        origin = (x0 / 25.4, -y1 / 25.4)
        size = ((x1 - x0) / 25.4, (y1 - y0) / 25.4)
        ka, kerr = raster(kf[layer], origin, size, os.path.join(kd, layer + '.png'))
        oa, oerr = raster(ours, origin, size, os.path.join(od, layer + '.png'))
        if ka is None or oa is None:
            line.append('%s:render-fail' % layer); continue
        h = min(ka.shape[0], oa.shape[0]); w = min(ka.shape[1], oa.shape[1])
        ka, oa = ka[:h, :w], oa[:h, :w]
        union = np.logical_or(ka, oa).sum(); diff = np.logical_xor(ka, oa).sum()
        frac = diff / union if union else 0.0
        line.append('%s:%.4f%s' % (layer, frac, ('!' + str(len(oerr))) if oerr else ''))
        results.append((base, layer, frac, int(union)))
    print(base, ' '.join(line), flush=True)

if results:
    import collections
    per = collections.defaultdict(list)
    for b, l, f, u in results: per[l].append(f)
    print('median diff per layer:', {l: round(float(np.median(v)), 4) for l, v in per.items()})
