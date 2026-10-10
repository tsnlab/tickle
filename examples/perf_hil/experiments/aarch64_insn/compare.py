#!/usr/bin/env python3
"""qcmp.py A.count A.client.log B.count B.client.log: per-function guest instructions per sample, A vs B."""
import re
import sys
from collections import Counter


def load(cnt, log):
    sent = int(re.search(r" sent=(\d+)", open(log).read()).group(1))
    by = Counter()
    for ln in open(cnt):
        p = ln.split()
        if p[0] == "PC":
            fn = p[2].replace(".part.0", "")
            by[fn] += int(p[3]) * int(p[4])
    return sent, by


sa, a = load(sys.argv[1], sys.argv[2])
sb, b = load(sys.argv[3], sys.argv[4])
rows = []
for fn in set(a) | set(b):
    x, y = a[fn] / sa, b[fn] / sb
    rows.append((abs(y - x), fn, x, y))
print("sent A %d B %d; total/sample A %.1f B %.1f" % (sa, sb, sum(a.values()) / sa, sum(b.values()) / sb))
for d, fn, x, y in sorted(rows, reverse=True)[:25]:
    print("%-45s A %8.2f  B %8.2f  %+7.2f" % (fn, x, y, y - x))
