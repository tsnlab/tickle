#!/usr/bin/env python3
"""compare_samehost.py OUTD REF X... REF2: samehost_qemu.sh's reading (written before the runs, see its header).

Per cell and role (client, server): instructions per sample of every arm in every repetition, then per function the
mean B - A next to the A/A spread |REF2 - REF|. A function's difference is ATTRIBUTED when it exceeds 2 x that spread
(and 0.5 instructions) and has the same sign in every repetition; the total likewise.
"""
import glob
import os
import re
import sys
from collections import Counter, defaultdict

outd, labels = sys.argv[1], sys.argv[2:]
ref, ref2 = labels[0], labels[-1]


def load(prefix, role):
    log = open(prefix + ".client.log", errors="replace").read()
    m = re.search(r"^RESULT.* sent=(\d+)", log, re.M)
    if not m or not os.path.exists(prefix + "." + role + ".count"):
        return None
    sent = int(m.group(1))
    by = Counter()
    for ln in open(prefix + "." + role + ".count"):
        p = ln.split()
        if p and p[0] == "PC" and len(p) == 5:
            by[re.sub(r"\.(part|isra|constprop)\.\d+", "", p[2])] += int(p[3]) * int(p[4])
    return {k: v / sent for k, v in by.items()} if sent else None


runs = defaultdict(list)  # (cell, role, arm) -> [per-function dict]
for path in sorted(glob.glob(os.path.join(outd, "*.status"))):
    base = os.path.basename(path)[: -len(".status")]
    m = re.match(r"(.+?_p\d)_(.+)_r(\d+)$", base)
    if not m:
        continue
    cell, arm = m.group(1), m.group(2)
    for role in ("client", "server"):
        d = load(path[: -len(".status")], role)
        if d is not None:
            runs[(cell, role, arm)].append(d)


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


cells = sorted({k[0] for k in runs})
for cell in cells:
    for role in ("client", "server"):
        rows = {a: runs.get((cell, role, a), []) for a in labels}
        if any(len(v) == 0 for v in rows.values()):
            print("%s %s: VOID (an arm has no run)" % (cell, role))
            continue
        tot = {a: [sum(d.values()) for d in rows[a]] for a in labels}
        print("%s %s per %s: %s" % (cell, role, "round trip" if cell.startswith("lat") else "sample",
                                     "  ".join("%s %s" % (a, "/".join("%.1f" % t for t in tot[a])) for a in labels)))
        spread_t = abs(mean(tot[ref2]) - mean(tot[ref]))
        for a in labels[1:-1]:
            d = mean(tot[a]) - mean(tot[ref])
            print("    TOTAL %s-%s %+.2f  (A/A %.2f) %s" % (a, ref, d, spread_t,
                                                         "ATTRIBUTED" if abs(d) > max(2 * spread_t, 0.5) else "within"))
            fns = set()
            for x in rows[a] + rows[ref] + rows[ref2]:
                fns |= set(x)
            out = []
            for fn in fns:
                va = [x.get(fn, 0.0) for x in rows[ref]]
                vb = [x.get(fn, 0.0) for x in rows[a]]
                v2 = [x.get(fn, 0.0) for x in rows[ref2]]
                dd = mean(vb) - mean(va)
                sp = abs(mean(v2) - mean(va))
                same_sign = all((y - mean(va)) * dd > 0 for y in vb)
                if abs(dd) > max(2 * sp, 0.5) and same_sign:
                    out.append((abs(dd), fn, mean(va), mean(vb), dd, sp))
            for _, fn, x, y, dd, sp in sorted(out, reverse=True)[:12]:
                print("      %-42s %s %9.2f  %s %9.2f  %+7.2f  (A/A %.2f)" % (fn, ref, x, a, y, dd, sp))
