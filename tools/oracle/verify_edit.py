"""Apply the same edits with KiCad's own API to the original board and compare with the
board KiPad saved.  usage: verify_edit.py orig edited op...   (ops as for kpcli edit)"""
import sys
import pcbnew

orig_path, edit_path, ops = sys.argv[1], sys.argv[2], sys.argv[3:]
ref = pcbnew.LoadBoard(orig_path)
got = pcbnew.LoadBoard(edit_path)

def fps(b):
    return {f.GetReference(): f for f in b.GetFootprints()}

R = fps(ref)
i = 0
deleted = set()
while i < len(ops):
    op = ops[i]; i += 1
    if op == 'move':
        f = R[ops[i]]; dx = pcbnew.FromMM(float(ops[i+1])); dy = pcbnew.FromMM(float(ops[i+2])); i += 3
        f.Move(pcbnew.VECTOR2I(dx, dy))
    elif op == 'rotate':
        f = R[ops[i]]; a = float(ops[i+1]); i += 2
        f.Rotate(f.GetPosition(), pcbnew.EDA_ANGLE(a, pcbnew.DEGREES_T))
    elif op == 'field':
        f = R[ops[i]]; name = ops[i+1]; val = ops[i+2]; i += 3
        if name == 'Reference':
            R[val] = f; del R[ops[i-3]]; f.SetReference(val)
        elif name == 'Value':
            f.SetValue(val)
    elif op == 'delete':
        deleted.add(ops[i]); i += 1
    elif op == 'width':
        i += 2
    else:
        raise SystemExit('bad op ' + op)

G = fps(got)
problems = 0
def bad(msg):
    global problems
    problems += 1
    if problems <= 30:
        print('  MISMATCH', msg)

for name in deleted:
    if name in G:
        bad('%s should be deleted' % name)

for name, f in R.items():
    if name in deleted:
        continue
    g = G.get(name)
    if g is None:
        bad('%s missing' % name); continue
    if (f.GetPosition() - g.GetPosition()).EuclideanNorm() > 2:
        bad('%s pos %s vs %s' % (name, f.GetPosition(), g.GetPosition()))
    if abs((f.GetOrientation() - g.GetOrientation()).Normalize180().AsDegrees()) > 1e-3:
        bad('%s angle %s vs %s' % (name, f.GetOrientation().AsDegrees(), g.GetOrientation().AsDegrees()))
    if f.GetValue() != g.GetValue():
        bad('%s value %r vs %r' % (name, f.GetValue(), g.GetValue()))
    fp_pads = list(f.Pads())
    gp_pads = list(g.Pads())
    if len(fp_pads) != len(gp_pads):
        bad('%s pad count' % name); continue
    for p, q in zip(fp_pads, gp_pads):
        if (p.GetPosition() - q.GetPosition()).EuclideanNorm() > 2:
            bad('%s pad %s pos %s vs %s' % (name, p.GetNumber(), p.GetPosition(), q.GetPosition()))
        if abs((p.GetOrientation() - q.GetOrientation()).Normalize180().AsDegrees()) > 1e-3:
            bad('%s pad %s angle %s vs %s' % (name, p.GetNumber(), p.GetOrientation().AsDegrees(), q.GetOrientation().AsDegrees()))
    for tf, tg in ((f.Reference(), g.Reference()), (f.Value(), g.Value())):
        if (tf.GetPosition() - tg.GetPosition()).EuclideanNorm() > 2:
            bad('%s text %s pos %s vs %s' % (name, tf.GetText(), tf.GetPosition(), tg.GetPosition()))
        if abs((tf.GetTextAngle() - tg.GetTextAngle()).Normalize180().AsDegrees()) > 1e-3:
            bad('%s text %s angle %s vs %s' % (name, tf.GetText(), tf.GetTextAngle().AsDegrees(), tg.GetTextAngle().AsDegrees()))
    fz = list(f.Zones()); gz = list(g.Zones())
    for zf, zg in zip(fz, gz):
        for k in range(zf.Outline().TotalVertices()):
            if (zf.Outline().CVertex(k) - zg.Outline().CVertex(k)).EuclideanNorm() > 2:
                bad('%s zone vertex %d' % (name, k)); break

print('%s: %d footprints compared, %d mismatches' % (edit_path.split('/')[-1], len(R) - len(deleted), problems))
sys.exit(1 if problems else 0)
