#!/usr/bin/env python3
"""layout_qemu.py OUTD APP_LO APP_HI LABEL... REF2: layout_qemu.sh's reading (pre-registered in its header).

Per cell and role: per arm and repetition, the executable's own instructions per sample (per round trip on a latency
cell), its executed NOPs per sample, and everything else's (libc, ld.so). Then per family (arms whose labels share the
leading letters: OLD64/OLD128/OLD256 -> OLD) INVARIANT or DIFFERS, and the run's reading:
  NEW  must be INVARIANT and execute 0 app NOPs;  PCO (positive control) must DIFFER in executed NOPs, else VOID;
  PCN  must be INVARIANT. Tolerances, VOID runs and why the rule was revised: layout_qemu.sh header.
"""
import glob
import os
import re
import sys
from collections import defaultdict

outd, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
labels = sys.argv[4:]
ref, ref2 = labels[0], labels[-1]
NOP_TOL = 0.01
MIN_SPREAD = 0.5
T_LIMIT = 3.0  # |mean B - mean A| beyond T_LIMIT x sqrt(se_A^2 + se_B^2) (and MIN_SPREAD) is DIFFERS
SHORT_RUN = 0.5  # a run with fewer samples than this share of its cell's median is VOID (start-up dominates it)


def nops_of(arm, binary_dir, role):
    path = os.path.join(outd, "bin", arm, binary_dir, role + ".nops")
    try:
        return {int(x, 16) for x in open(path).read().split()}
    except OSError:
        return None


def load(prefix, role):
    """(app insns, app nops, other insns) per sample, or None when the run cannot be read."""
    try:
        log = open(prefix + ".client.log", errors="replace").read()
        arm, binary_dir = open(prefix + ".arm").read().split()
        lines = open(prefix + "." + role + ".count").read().splitlines()
    except (OSError, ValueError):
        return None
    m = re.search(r"^RESULT.* sent=(\d+)", log, re.M)
    if not m or int(m.group(1)) == 0 or not any(ln.startswith("TOTAL") for ln in lines):
        return None
    sent = int(m.group(1))
    nops = nops_of(arm, binary_dir, role)
    if nops is None:
        return None
    app = app_nop = other = 0
    for ln in lines:
        p = ln.split()
        if not p or p[0] != "PC" or len(p) != 5:
            continue
        pc, n, ex = int(p[1], 16), int(p[3]), int(p[4])
        if lo <= pc < hi:
            app += n * ex
            off = pc - lo
            app_nop += ex * sum(1 for i in range(n) if off + 4 * i in nops)
        else:
            other += n * ex
    if app == 0:
        return None
    return app / sent, app_nop / sent, other / sent, sent


runs = defaultdict(list)
for path in sorted(glob.glob(os.path.join(outd, "*.status"))):
    base = os.path.basename(path)[: -len(".status")]
    m = re.match(r"((?:lat|rt)_p\d)_(.+)_r(\d+)$", base)
    if not m:
        continue
    for role in ("client", "server"):
        d = load(path[: -len(".status")], role)
        if d is not None:
            runs[(m.group(1), role, m.group(2))].append(d)


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def se(xs):
    if len(xs) < 2:
        return float("inf")
    m = mean(xs)
    return (sum((x - m) ** 2 for x in xs) / (len(xs) - 1) / len(xs)) ** 0.5


short = []
by_cell = defaultdict(list)
for (cell, role, arm), rs in runs.items():
    by_cell[cell] += [r[3] for r in rs]
for key in list(runs):
    med = sorted(by_cell[key[0]])[len(by_cell[key[0]]) // 2]
    keep = [r for r in runs[key] if r[3] >= SHORT_RUN * med]
    if len(keep) != len(runs[key]):
        short.append("%s %s %s: %d short run(s) VOID (sent < %.0f%% of the cell's median %d)" % (
            key[0], key[1], key[2], len(runs[key]) - len(keep), 100 * SHORT_RUN, med))
    runs[key] = keep


def family(label):
    return re.match(r"[A-Za-z]+", label).group(0)


fams = defaultdict(list)
for a in labels[:-1]:
    fams[family(a)].append(a)
verdict = {f: "INVARIANT" for f in fams}
max_nop = {f: 0.0 for f in fams}
void = []
cells = sorted({k[0] for k in runs})
for cell in cells:
    for role in ("client", "server"):
        rows = {a: runs.get((cell, role, a), []) for a in labels}
        missing = [a for a, v in rows.items() if not v]
        if missing:
            void.append("%s %s: no readable run for %s" % (cell, role, " ".join(missing)))
            continue
        spread = abs(mean([r[0] for r in rows[ref2]]) - mean([r[0] for r in rows[ref]]))
        print("%s %s per %s (A/A %s-%s %+.2f):" % (
            cell, role, "round trip" if cell.startswith("lat") else "sample", ref2, ref, spread))
        for a in labels:
            print("    %-8s app %s  nops %s  other %s" % (
                a, " ".join("%10.2f" % r[0] for r in rows[a]), " ".join("%7.2f" % r[1] for r in rows[a]),
                " ".join("%9.1f" % r[2] for r in rows[a])))
        for f, arms in fams.items():
            first = arms[0]
            n0 = mean([r[1] for r in rows[first]])
            i0 = mean([r[0] for r in rows[first]])
            s0 = se([r[0] for r in rows[first]])
            for a in arms:
                n, i = mean([r[1] for r in rows[a]]), mean([r[0] for r in rows[a]])
                tol = max(T_LIMIT * (s0 ** 2 + se([r[0] for r in rows[a]]) ** 2) ** 0.5, MIN_SPREAD)
                max_nop[f] = max(max_nop[f], n)
                if abs(n - n0) > NOP_TOL or abs(i - i0) > tol:
                    verdict[f] = "DIFFERS"
                    print("    %s DIFFERS: %s vs %s app %+.2f (tolerance %.2f) nops %+.2f" % (f, a, first, i - i0, tol,
                                                                                    n - n0))
for v in short:
    print("VOID run:", v)
for v in void:
    print("VOID row:", v)
for f in fams:
    print("FAMILY %-4s %-9s (%s; max executed app NOPs per sample %.2f)" % (f, verdict[f], " ".join(fams[f]), max_nop[f]))
ok = True
reasons = []
if void or not cells:
    ok, reasons = False, reasons + ["VOID rows or no cells"]
if "PCO" in verdict and verdict["PCO"] != "DIFFERS":
    ok, reasons = False, reasons + ["VOID: the positive control PCO did not differ - this instrument cannot see padding"]
if "NEW" in verdict and (verdict["NEW"] != "INVARIANT" or max_nop["NEW"] > 0):
    ok, reasons = False, reasons + ["NEW is not invariant or executes NOPs"]
if "PCN" in verdict and verdict["PCN"] != "INVARIANT":
    ok, reasons = False, reasons + ["PCN is not invariant"]
print("READING: %s" % ("NEW FLAGS LAYOUT-INVARIANT" if ok else "NOT SHOWN: " + "; ".join(reasons)))
