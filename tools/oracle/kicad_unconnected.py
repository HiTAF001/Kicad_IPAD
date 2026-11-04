# Usage: /usr/bin/python3 kicad_unconnected.py <board>...  -> prints "<unconnected> <path>" using KiCad's own connectivity
import sys
sys.path.insert(0, '/usr/lib/python3/dist-packages')
import pcbnew
for path in sys.argv[1:]:
    try:
        b = pcbnew.LoadBoard(path)
        b.BuildConnectivity()
        conn = b.GetConnectivity()
        n = conn.GetUnconnectedCount(True)   # visible-only airwires
        print(n, path, flush=True)
    except Exception as e:
        print("ERR", path, str(e)[:80], flush=True)
