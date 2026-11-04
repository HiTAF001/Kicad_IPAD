"""Schematic renderer oracle: kicad-cli 7 SVG export vs kpcli render, per sheet.
usage: sch_compare.py kpcli workdir files.kicad_sch...
Ink masks (pixels away from the paper colour) are compared with a tolerance of TOL px:
 missing = KiCad ink with none of ours nearby, extra = our ink with no KiCad ink nearby."""
import sys, os, subprocess, re, shutil
import numpy as np
from PIL import Image
from scipy import ndimage
import cairosvg

KPCLI, WORK = sys.argv[1], sys.argv[2]
PPMM, TOL = 8, 2
os.makedirs(WORK, exist_ok=True)
BG = np.array([245, 244, 239])

def ink(img):
    a = np.asarray(img.convert('RGB')).astype(int)
    return np.abs(a - BG).sum(axis=2) > 90

rows = []
for f in sys.argv[3:]:
    base = os.path.basename(f)[:-10]
    d = os.path.join(WORK, base); shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    r = subprocess.run(['kicad-cli', 'sch', 'export', 'svg', '--output', d + '/', f], capture_output=True, text=True, timeout=300)
    svgs = {x[:-4]: os.path.join(d, x) for x in os.listdir(d) if x.endswith('.svg')}
    if not svgs:
        rows.append((base, '-', 'KICAD-FAIL', '')); continue
    sh = subprocess.run([KPCLI, 'sheets', f], capture_output=True, text=True)
    if sh.returncode:
        rows.append((base, '-', 'OURS-FAIL', sh.stderr.strip()[-100:])); continue
    for line in sh.stdout.splitlines():
        parts = line.split('\t')
        if parts[0] == 'missing':
            rows.append((base, parts[1], 'MISSING-FILE', '')); continue
        idx, page, path = int(parts[0]), parts[1], parts[2]
        name = base if path == '/' else base + '-' + '-'.join(p for p in path.strip('/').split('/'))
        name = re.sub(r'[\\:*?"<>|]', '_', name)
        svg = svgs.get(name)
        if not svg:
            # KiCad replaces some characters differently: fall back to case-insensitive match
            cand = [k for k in svgs if k.lower() == name.lower()]
            svg = svgs[cand[0]] if cand else None
        if not svg:
            rows.append((base, path, 'NO-SVG', name)); continue
        txt = open(svg, encoding='utf-8', errors='replace').read(2000)
        m = re.search(r'viewBox="0\.0000 0\.0000 ([\d.]+) ([\d.]+)"', txt)
        W, H = float(m.group(1)), float(m.group(2))
        wpx = int(round(W * PPMM))
        kp = os.path.join(d, 'k%d.png' % idx); op = os.path.join(d, 'o%d.png' % idx)
        try:
            cairosvg.svg2png(url=svg, write_to=kp, output_width=wpx, background_color='#F5F4EF')
        except Exception as e:
            rows.append((base, path, 'SVG-RENDER-FAIL', str(e)[:60])); continue
        rr = subprocess.run([KPCLI, 'render', f, op, str(wpx), '--view', '0', '0', str(W), str(H), '--sheet', str(idx), '--no-labels'],
                            capture_output=True, text=True, timeout=300)
        if rr.returncode or not os.path.exists(op):
            rows.append((base, path, 'RENDER-FAIL', rr.stderr.strip()[-80:])); continue
        a = ink(Image.open(kp)); b = ink(Image.open(op))
        e = int(1.5 * PPMM); a[:e, :] = a[-e:, :] = a[:, :e] = a[:, -e:] = False; b[:e, :] = b[-e:, :] = b[:, :e] = b[:, -e:] = False
        h = min(a.shape[0], b.shape[0]); w = min(a.shape[1], b.shape[1]); a, b = a[:h, :w], b[:h, :w]
        st = ndimage.generate_binary_structure(2, 1)
        ad = ndimage.binary_dilation(a, st, iterations=TOL); bd = ndimage.binary_dilation(b, st, iterations=TOL)
        missing = (a & ~bd).sum() / max(1, a.sum()); extra = (b & ~ad).sum() / max(1, b.sum())
        img = np.zeros((h, w, 3), np.uint8); img[:] = 255
        img[a & b] = (150, 150, 150); img[a & ~bd] = (230, 40, 40); img[b & ~ad] = (40, 120, 255)
        Image.fromarray(img).save(os.path.join(d, 'x%d.png' % idx))
        rows.append((base, path, '%.4f' % missing, '%.4f' % extra))
for r in rows:
    print('%-40s %-30s %-14s %s' % r)
vals = [(float(r[2]), float(r[3])) for r in rows if re.match(r'^\d', r[2])]
if vals:
    m = np.array(vals)
    print('sheets %d  median missing %.4f extra %.4f  mean missing %.4f extra %.4f' % (len(vals), np.median(m[:, 0]), np.median(m[:, 1]), m[:, 0].mean(), m[:, 1].mean()))
