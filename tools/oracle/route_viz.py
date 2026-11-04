"""Render route test cases: board after routing (kpcli render) with the raw stroke overlaid."""
import sys, json, subprocess, os
from PIL import Image, ImageDraw, ImageFont
KPCLI, REPORT, OUTDIR = sys.argv[1], sys.argv[2], sys.argv[3]
sel = sys.argv[4] if len(sys.argv) > 4 else 'bad'
os.makedirs(OUTDIR, exist_ok=True)
rep = json.load(open(REPORT))
cases = [r for r in rep if (sel == 'all' or (sel == 'bad' and r['probs']) or (sel == 'ok' and not r['probs'] and 'blocked=0' in r['head']) or (sel == 'blocked' and 'blocked=1' in r['head']))]
tiles = []
for k, r in enumerate(cases[:int(os.environ.get('MAX', '12'))]):
    xs = [p[0] for p in r['stroke']]; ys = [p[1] for p in r['stroke']]
    m = 1.5
    x0, x1, y0, y1 = min(xs) - m, max(xs) + m, min(ys) - m, max(ys) + m
    # square-ish view
    w, h = x1 - x0, y1 - y0
    if w < h: x0 -= (h - w) / 2; x1 += (h - w) / 2
    else: y0 -= (w - h) / 2; y1 += (w - h) / 2
    W = 600
    png = os.path.join(OUTDIR, 'case%02d.png' % k)
    src = r['out'] if os.path.exists(r['out']) else r['board']
    subprocess.run([KPCLI, 'render', src, png, str(W), '--view', str(x0), str(y0), str(x1), str(y1), '--no-labels'] + (['--back'] if 'route B.Cu' in r['head'] else []), capture_output=True)
    im = Image.open(png).convert('RGB')
    s = im.width / (x1 - x0)
    d = ImageDraw.Draw(im, 'RGBA')
    back = 'route B.Cu' in r['head']
    X = (lambda x: (x1 - x) * s) if back else (lambda x: (x - x0) * s)
    pts = [(X(x), (y - y0) * s) for x, y in r['stroke']]
    d.line(pts, fill=(255, 255, 0, 200), width=2)
    for sg in r['segs']:
        a = (X(sg[0]), (sg[1] - y0) * s); b = (X(sg[2]), (sg[3] - y0) * s)
        d.ellipse([a[0]-3, a[1]-3, a[0]+3, a[1]+3], outline=(0, 255, 255, 255))
        d.ellipse([b[0]-3, b[1]-3, b[0]+3, b[1]+3], outline=(0, 255, 255, 255))
    txt = '%s %s %s %s' % (os.path.basename(r['board']), r['style'], 'B.Cu' if back else 'F.Cu', ('BLOCKED ' if 'blocked=1' in r['head'] else '') + '; '.join(r['probs'])[:60])
    d.rectangle([0, 0, im.width, 16], fill=(0, 0, 0, 180))
    d.text((4, 2), txt, fill=(255, 255, 255, 255))
    im.save(png)
    tiles.append(im)
if tiles:
    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    M = Image.new('RGB', (cols * 600, rows * max(t.height for t in tiles)), (30, 30, 30))
    for i, t in enumerate(tiles):
        M.paste(t, ((i % cols) * 600, (i // cols) * max(tt.height for tt in tiles)))
    M.save(os.path.join(OUTDIR, 'montage_%s.png' % sel))
    print('montage', len(tiles))
