import os
import sys, subprocess, random, pcbnew, os
SP = os.environ.get('NETLOOM_WORK', 'work')
random.seed(7)
boards = [l.strip() for l in open(SP + '/oracle/k7boards.txt') if l.strip()]
tot = bad = 0
for path in boards:
    try:
        b = pcbnew.LoadBoard(path)
    except Exception:
        continue
    refs = [f.GetReference() for f in b.GetFootprints() if not f.IsLocked() and f.GetReference()]
    refs = [r for r in refs if refs.count(r) == 1 and ' ' not in r and r and not r.startswith('-')]
    if not refs:
        continue
    ops = []
    for r in random.sample(refs, min(6, len(refs))):
        k = random.random()
        if k < 0.4:
            ops += ['rotate', r, random.choice(['90', '-90', '180', '45', '270', '30'])]
        elif k < 0.7:
            ops += ['move', r, '%.3f' % random.uniform(-5, 5), '%.3f' % random.uniform(-5, 5)]
        elif k < 0.85:
            ops += ['move', r, '1.27', '0', 'rotate', r, '90']
        else:
            ops += ['field', r, 'Value', 'X_' + r]
    out = SP + '/edit/sweep.kicad_pcb'
    e = subprocess.run([SP + '/build/kpcli', 'edit', path, out] + ops, capture_output=True, text=True)
    tot += 1
    if e.returncode != 0:
        print('KPCLI FAIL', path, e.stdout[-300:], e.stderr[-300:]); bad += 1; continue
    v = subprocess.run(['python3', SP + '/oracle/verify_edit.py', path, out] + ops, capture_output=True, text=True)
    if v.returncode != 0:
        bad += 1
        print('MISMATCH', os.path.basename(path), ' '.join(ops))
        print(v.stdout[-1500:], v.stderr[-500:])
print('%d boards edited, %d bad' % (tot, bad))
