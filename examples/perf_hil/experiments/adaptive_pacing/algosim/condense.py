#!/usr/bin/env python3
"""A compact rendering of the aggregated tables (one table per workload family, both sample sizes side by side).
usage: condense.py <tables.md>   (the output of aggregate.py)"""
import re, sys
rows = {}
cur = None
for line in open(sys.argv[1]):
    m = re.match(r"#### (.+), (128 KiB|1 MiB)$", line.strip())
    if m:
        cur = (m.group(1), m.group(2))
        rows[cur] = {}
        continue
    if line.startswith("####") and not m:
        cur = None
    if cur and line.startswith("| ") and not line.startswith("| candidate") and not line.startswith("|---"):
        c = [x.strip() for x in line.strip().strip("|").split("|")]
        rows[cur][c[0]] = c[1:]
KEEP = ["oracle", "fixed", "adapt-a", "adapt_hi-a", "adapt-b", "bbr3-a", "bbr3-b", "nada-b", "gcc-b", "train-b", "train1-b", "train1t-b", "traint-b"]
# columns of an aggregate row: in30 over under q100 p50x p95x p99x bad retx rec startup slow over3 fb
fams = []
for (fam, sz) in rows:
    if fam not in fams:
        fams.append(fam)
for fam in fams:
    print(f"\n**{fam}**; each cell: 128 KiB / 1 MiB\n")
    print("| candidate | in +-30% | over 1.3x | under 0.7x | queue>100ms | p50 x oracle | p99 x oracle | undeliv+late | retx | recovery up~down s | startup s |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for k in KEEP:
        a = rows[(fam, "128 KiB")].get(k)
        b = rows[(fam, "1 MiB")].get(k)
        if not a or not b:
            continue
        def g(r, i):
            return r[i] if i < len(r) else "-"
        def two(i):
            return f"{g(a, i)} / {g(b, i)}"
        def rec(r):
            up, _, dn = g(r, 9).partition(" / ")
            return f"{up}~{dn}"
        print(f"| {k} | {two(0)} | {two(1)} | {two(2)} | {two(3)} | {two(4)} | {two(6)} | {two(7)} | {two(8)} | {rec(a)} / {rec(b)} | {two(10)} |")
