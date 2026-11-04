#!/usr/bin/env python3
"""Compare kpcli's schematic netlist with kicad-cli's (KiCad 7) for a list of .kicad_sch files.

usage: sch_net_compare.py <kpcli> <list-of-files> [--verbose N]
Reports, per file: nets in both (same pins), same pins but other name, and pin sets that differ.
"""
import os, re, subprocess, sys, tempfile

ESC = {"{slash}": "/", "{backslash}": "\\", "{colon}": ":", "{dblquote}": '"', "{quote}": "'",
       "{lt}": "<", "{gt}": ">", "{bar}": "|", "{space}": " ", "{dollar}": "$", "{tab}": " ",
       "{return}": " ", "{comma}": ",", "{brace}": "{"}


def unescape(s):
    for k, v in ESC.items():
        s = s.replace(k, v)
    return s


def tokens(text):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "()":
            yield c
            i += 1
        elif c.isspace():
            i += 1
        elif c == '"':
            j = i + 1
            out = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    j += 1
                out.append(text[j])
                j += 1
            yield ("s", "".join(out))
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "()":
                j += 1
            yield ("a", text[i:j])
            i = j


def parse(text):
    stack = [[]]
    for t in tokens(text):
        if t == "(":
            stack.append([])
        elif t == ")":
            l = stack.pop()
            stack[-1].append(l)
        else:
            stack[-1].append(t[1])
    return stack[0][0]


def find(l, key):
    for c in l:
        if isinstance(c, list) and c and c[0] == key:
            return c
    return None


def kicad_nets(path, tmp, reference=False):
    out = os.path.join(tmp, "k.net")
    if reference:
        out = os.path.splitext(path)[0] + ".net"
    else:
        if os.path.exists(out):
            os.remove(out)
        subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", out, path],
                       capture_output=True, text=True, timeout=120)
    if not os.path.exists(out):
        return None
    tree = parse(open(out, encoding="utf-8").read())
    nets = {}
    for net in find(tree, "nets")[1:]:
        name = unescape(find(net, "name")[1])
        nodes = set()
        for c in net[1:]:
            if isinstance(c, list) and c and c[0] == "node":
                nodes.add((find(c, "ref")[1], find(c, "pin")[1]))
        if nodes:
            nets[frozenset(nodes)] = name
    return nets


def our_nets(kpcli, path):
    r = subprocess.run([kpcli, "netlist", path], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        return None
    nets = {}
    for line in r.stdout.splitlines():
        if "\t" not in line:
            continue
        name, rest = line.split("\t", 1)
        nodes = set()
        for tok in rest.split(" "):
            if ":" in tok:
                ref, pin = tok.split(":", 1)
                nodes.add((ref, pin))
        if nodes:
            nets[frozenset(nodes)] = name
    return nets


def main():
    kpcli, listfile = sys.argv[1], sys.argv[2]
    verbose = int(sys.argv[sys.argv.index("--verbose") + 1]) if "--verbose" in sys.argv else 0
    files = [l.strip() for l in open(listfile) if l.strip()]
    tot = dict(files=0, same=0, renamed=0, kiconly=0, ouronly=0, failed=0)
    perfect = 0
    with tempfile.TemporaryDirectory() as tmp:
        for f in files:
            k = kicad_nets(f, tmp, "--reference" in sys.argv)
            o = our_nets(kpcli, f)
            if k is None or o is None:
                tot["failed"] += 1
                print(f"FAIL {'kicad' if k is None else 'ours'}: {f}")
                continue
            tot["files"] += 1
            same = sum(1 for s in k if s in o and k[s] == o[s])
            renamed = [(k[s], o[s]) for s in k if s in o and k[s] != o[s]]
            kiconly = [s for s in k if s not in o]
            ouronly = [s for s in o if s not in k]
            tot["same"] += same
            tot["renamed"] += len(renamed)
            tot["kiconly"] += len(kiconly)
            tot["ouronly"] += len(ouronly)
            if not renamed and not kiconly and not ouronly:
                perfect += 1
                continue
            print(f"{os.path.relpath(f, os.path.dirname(listfile))}: same {same}, renamed {len(renamed)}, "
                  f"kicad-only {len(kiconly)}, ours-only {len(ouronly)}")
            if verbose:
                for a, b in renamed[:verbose]:
                    print(f"    name  kicad {a!r}  ours {b!r}")
                for s in kiconly[:verbose]:
                    print(f"    kicad {k[s]!r}: {' '.join(sorted(r + ':' + p for r, p in s))[:300]}")
                for s in ouronly[:verbose]:
                    print(f"    ours  {o[s]!r}: {' '.join(sorted(r + ':' + p for r, p in s))[:300]}")
    print(f"\nfiles {tot['files']} (all equal: {perfect}), failed {tot['failed']}")
    print(f"nets equal {tot['same']}, same pins other name {tot['renamed']}, "
          f"kicad-only pin sets {tot['kiconly']}, ours-only pin sets {tot['ouronly']}")


if __name__ == "__main__":
    main()
