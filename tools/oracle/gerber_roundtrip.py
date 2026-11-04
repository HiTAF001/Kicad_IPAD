"""Gerber reader oracle: KiCad's Gerbers -> Netloom reader -> Netloom writer -> compare with
KiCad's originals rasterized by gerbv. Only reader bugs (or writer bugs) show up as differences.
usage: gerber_roundtrip.py kpcli workdir fabcmp_dirs..."""
import sys, os, subprocess, re, shutil
import numpy as np
from PIL import Image

KPCLI, WORK = sys.argv[1], sys.argv[2]
LAYERS = ['F_Cu', 'B_Cu', 'F_Mask', 'B_Mask', 'F_Paste', 'B_Paste', 'F_Silkscreen', 'B_Silkscreen', 'Edge_Cuts', 'PTH', 'NPTH']

def raster(path, origin, size, out, dpi=400):
    subprocess.run(['gerbv', '-x', 'png', '-o', out, '-D', str(dpi), '-B', '0', '-O', '%fx%f' % origin,
                    '-W', '%fx%f' % size, '-f', '#FFFFFFFF', '-b', '#000000', path], capture_output=True, text=True, timeout=300)
    if not os.path.exists(out): return None
    return np.asarray(Image.open(out).convert('L')) > 127

def bounds(path):
    # gerbv-free: read the Edge.Cuts coordinates directly (FS 4.6 mm)
    xs, ys = [], []
    for m in re.finditer(r'X(-?\d+)Y(-?\d+)', open(path).read()):
        xs.append(int(m.group(1)) / 1e6); ys.append(int(m.group(2)) / 1e6)
    return min(xs), min(ys), max(xs), max(ys)

per = {}
for d in sys.argv[3:]:
    name = os.path.basename(d.rstrip('/'))
    kd = os.path.join(d, 'kicad')
    src = os.path.join(WORK, name, 'in'); out = os.path.join(WORK, name, 'out')
    shutil.rmtree(os.path.join(WORK, name), ignore_errors=True); os.makedirs(src); os.makedirs(out)
    files = [f for f in os.listdir(kd) if not f.endswith('.png')]
    for f in files: shutil.copy(os.path.join(kd, f), src)
    r = subprocess.run([KPCLI, 'fab', src, out], capture_output=True, text=True)
    if r.returncode: print(name, 'FAIL', r.stderr.strip()); continue
    edge = [f for f in files if 'Edge_Cuts' in f]
    allx = []
    for f in os.listdir(out):
        if f.endswith('.gbr'):
            try: allx.append(bounds(os.path.join(out, f)))
            except ValueError: pass
    x0 = min(b[0] for b in allx) - 2; y0 = min(b[1] for b in allx) - 2; x1 = max(b[2] for b in allx) + 2; y1 = max(b[3] for b in allx) + 2
    origin = (x0 / 25.4, y0 / 25.4); size = ((x1 - x0) / 25.4, (y1 - y0) / 25.4)
    line = []
    for L in LAYERS:
        kf = [f for f in files if re.search(r'-%s\.\w+$' % L, f) and not f.endswith('.gbrjob')]
        of = [f for f in os.listdir(out) if re.search(r'-%s\.(gbr|drl)$' % L, f)]
        if not kf: continue
        if not of: line.append('%s:missing' % L); continue
        ka = raster(os.path.join(src, kf[0]), origin, size, os.path.join(out, L + '.k.png'))
        oa = raster(os.path.join(out, of[0]), origin, size, os.path.join(out, L + '.o.png'))
        if ka is None or oa is None: line.append('%s:render-fail' % L); continue
        h = min(ka.shape[0], oa.shape[0]); w = min(ka.shape[1], oa.shape[1]); ka, oa = ka[:h, :w], oa[:h, :w]
        u = np.logical_or(ka, oa).sum(); x = np.logical_xor(ka, oa).sum()
        frac = x / u if u else 0.0
        per.setdefault(L, []).append(frac)
        line.append('%s:%.4f' % (L, frac))
    print(name, ' '.join(line), flush=True)
print('median:', {k: round(float(np.median(v)), 4) for k, v in per.items()})
print('max:', {k: round(float(np.max(v)), 4) for k, v in per.items()})
