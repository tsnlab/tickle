#!/usr/bin/env python3
"""Initial STA minstrel state of every topology in the diagnosis data: best rate at the first sampler line (taken ~2 s after bring-up), by arm and set. Usage: t1pi_wdiag_state.py BASE"""
import glob, json, os, sys, collections
base = sys.argv[1]
c = collections.defaultdict(lambda: [0, 0])
for f in sorted(glob.glob(base + "/*/*.rc") + glob.glob(base + "/*/*/*/*.rc")):
    try:
        r = json.loads(open(f).readline())
    except (OSError, ValueError):
        continue
    best = next((x[0] for x in r["sta"] if "A" in x[1]), None)
    rel = os.path.relpath(f, base).split("/")
    st = rel[0]
    nm = os.path.basename(f)
    arm = rel[2] if len(rel) > 3 else nm.split("_")[0]
    fw = nm.split("_")[1] if len(rel) > 3 else "udp"
    c[(st, arm, fw)][0 if best == "1.0M" else 1] += 1
tot = [0, 0]
print("set arm fw slow fast")
for k in sorted(c):
    print(*k, *c[k])
    tot[0] += c[k][0]; tot[1] += c[k][1]
print("TOTAL slow %d fast %d" % tuple(tot))
