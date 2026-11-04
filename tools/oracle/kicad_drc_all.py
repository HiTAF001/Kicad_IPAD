import os
"""KiCad 7 DRC oracle: per board, the violation list (category, positions) and design settings.
usage: kicad_drc.py out.json boards..."""
import sys, re, json, os
sys.path.insert(0, '/usr/lib/python3/dist-packages')
import pcbnew
out = {}
for path in sys.argv[2:]:
    try:
        b = pcbnew.LoadBoard(path)
        rpt = os.path.join(os.environ.get('NETLOOM_WORK', '.'), 'drc_tmp.rpt')
        pcbnew.WriteDRCReport(b, rpt, pcbnew.EDA_UNITS_MILLIMETRES, True)
        items = []
        cur = None
        for line in open(rpt, encoding='utf-8', errors='replace'):
            m = re.match(r'\[(\w+)\]: (.*)', line)
            if m:
                cur = dict(cat=m.group(1), msg=m.group(2).strip(), pos=[], sev='')
                items.append(cur)
                continue
            m = re.match(r'\s+@\(([-\d.]+) mm, ([-\d.]+) mm\): (.*)', line)
            if m and cur is not None:
                cur['pos'].append((float(m.group(1)), float(m.group(2)), m.group(3).strip()))
            m = re.search(r'Severity: (\w+)', line)
            if m and cur is not None:
                cur['sev'] = m.group(1)
        ds = b.GetDesignSettings()
        o = b.GetDesignSettings().GetAuxOrigin()
        g = b.GetDesignSettings().GetGridOrigin()
        out[path] = dict(items=items, settings=dict(
            min_clearance=pcbnew.ToMM(ds.m_MinClearance), track_min=pcbnew.ToMM(ds.m_TrackMinWidth),
            edge=pcbnew.ToMM(ds.m_CopperEdgeClearance), hole_to_hole=pcbnew.ToMM(ds.m_HoleToHoleMin),
            hole_clearance=pcbnew.ToMM(ds.m_HoleClearance), annular=pcbnew.ToMM(ds.m_ViasMinAnnularWidth),
            via_min=pcbnew.ToMM(ds.m_ViasMinSize), drill_min=pcbnew.ToMM(ds.m_MinThroughDrill)),
            aux=(pcbnew.ToMM(o.x), pcbnew.ToMM(o.y)), grid=(pcbnew.ToMM(g.x), pcbnew.ToMM(g.y)))
        print(len(items), path, flush=True)
    except Exception as e:
        print('ERR', path, e, flush=True)
json.dump(out, open(sys.argv[1], 'w'))
