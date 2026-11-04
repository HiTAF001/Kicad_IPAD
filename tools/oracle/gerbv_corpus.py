"""Run every Gerber / Excellon file of a corpus through Netloom's reader and writer and compare
with the original, both rasterized by gerbv. usage: gerbv_corpus.py kpcli workdir files..."""
import sys, os, subprocess, re, shutil
import numpy as np
from PIL import Image
KPCLI, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)

def raster(path, origin, size, out, dpi):
    r = subprocess.run(['gerbv', '-x', 'png', '-o', out, '-D', str(dpi), '-B', '0', '-O', '%fx%f' % origin,
                        '-W', '%fx%f' % size, '-f', '#FFFFFFFF', '-b', '#000000', path], capture_output=True, text=True, timeout=300)
    if not os.path.exists(out): return None
    return np.asarray(Image.open(out).convert('L')) > 127

rows = []
for i, f in enumerate(sys.argv[3:]):
    tag = '%02d_%s' % (i, re.sub(r'\W', '_', os.path.basename(f)))
    d = os.path.join(WORK, tag); shutil.rmtree(d, ignore_errors=True); os.makedirs(d + '/in'); os.makedirs(d + '/out')
    head = open(f, 'rb').read(4096)
    drill = b'M48' in head and b'%FS' not in head
    name = 'x-PTH.drl' if drill else 'x-F_Cu.gbr'
    shutil.copy(f, os.path.join(d, 'in', name))
    r = subprocess.run([KPCLI, 'fab', d + '/in', d + '/out'], capture_output=True, text=True)
    if r.returncode:
        rows.append((tag, 'LOADFAIL ' + (r.stderr.strip()[-80:]))); continue
    outs = [o for o in os.listdir(d + '/out') if o.endswith('.drl' if drill else '.gbr')]
    out = None
    for o in outs:
        if drill and 'NPTH' not in o: out = o
        if not drill and 'Cu' in o:
            if subprocess.run(['grep', '-qE', 'D0[123]', os.path.join(d, 'out', o)]).returncode == 0: out = o
    if not out:
        rows.append((tag, 'NOOUT ' + ' '.join(outs))); continue
    out = os.path.join(d, 'out', out)
    # window from our output (mm, FS 4.6 for Gerber, decimal mm for drill)
    txt = open(out).read()
    if drill:
        xs = [float(a) for a in re.findall(r'X(-?[\d.]+)', txt)]; ys = [float(a) for a in re.findall(r'Y(-?[\d.]+)', txt)]
    else:
        xs = [int(a) / 1e6 for a in re.findall(r'X(-?\d+)', txt)]; ys = [int(a) / 1e6 for a in re.findall(r'Y(-?\d+)', txt)]
    if not xs or not ys:
        rows.append((tag, 'EMPTY')); continue
    m = 3
    x0, x1, y0, y1 = min(xs) - m, max(xs) + m, min(ys) - m, max(ys) + m
    span = max(x1 - x0, y1 - y0)
    dpi = int(max(100, min(1000, 2500 / (span / 25.4))))
    origin = (x0 / 25.4, y0 / 25.4); size = ((x1 - x0) / 25.4, (y1 - y0) / 25.4)
    a = raster(os.path.join(d, 'in', name), origin, size, d + '/k.png', dpi)
    b = raster(out, origin, size, d + '/o.png', dpi)
    if a is None or b is None: rows.append((tag, 'RENDERFAIL')); continue
    h = min(a.shape[0], b.shape[0]); w = min(a.shape[1], b.shape[1]); a, b = a[:h, :w], b[:h, :w]
    u = np.logical_or(a, b).sum(); x = np.logical_xor(a, b).sum()
    img = np.zeros((h, w, 3), np.uint8); img[a & b] = (120, 120, 120); img[a & ~b] = (255, 60, 60); img[b & ~a] = (60, 200, 255)
    Image.fromarray(img).save(d + '/xor.png')
    rows.append((tag, '%.4f (orig-only %d, ours-only %d, dpi %d)' % (x / u if u else 0, (a & ~b).sum(), (b & ~a).sum(), dpi)))
for t, r in rows: print('%-48s %s' % (t, r))
