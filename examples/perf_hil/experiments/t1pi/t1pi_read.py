#!/usr/bin/env python3
"""Reader of the T1-Pi runs (PLAN.md). Python 3, no dependencies.

  t1pi_read.py DIR [DIR ...]          table of every run and per-group spreads (a run directory holds NAME.result files)
  t1pi_read.py --v1 DIR               rule V1 verdict (p = 0, be_small: delivery >= 99.9%, RTT p99 < 40 ms, 3 of 3 reps)
  t1pi_read.py --noise DIR [DIR ...]  A/A floor per group: delivered spread, p50/p99 spread, A2 vs A noise rule, against the PC
Every run is printed with scored/sent: a run without a RESULT line is printed as NO_RESULT, never dropped.
"""
import glob
import os
import re
import statistics
import sys

PC_FLOOR = {  # PC N1 (RESULTS.md section 5): delivered spread in percent, 120 s, seeded and pinned
    ("o60", "be"): 1.6, ("o200", "be"): 0.4, ("o60", "rel"): 2.1, ("o200", "rel"): 1.2,
}
FIELDS = ("published", "warmup", "scored", "p50_us", "p90_us", "p99_us", "retransmitted")


def parse(path):
    try:
        line = open(path).readline()
    except OSError:
        return None
    if not line.startswith("RESULT"):
        return None
    out = {}
    for k in FIELDS:
        m = re.search(r"\b%s=([0-9.]+)" % k, line)
        out[k] = float(m.group(1)) if m else float("nan")
    out["sent"] = out["published"] - out["warmup"]
    out["delivery"] = 100.0 * out["scored"] / out["sent"] if out["sent"] > 0 else 0.0
    return out


def group_key(name):
    return re.sub(r"_(A2|A)(?=_r\d+$)", "", re.sub(r"_r\d+$", "", name))


def load(dirs):
    runs = []
    for d in dirs:
        for tp in sorted(glob.glob(os.path.join(d, "*.topo"))):
            name = os.path.basename(tp)[:-5]
            res = parse(os.path.join(d, name + ".result"))
            runs.append((name, res, d))
    return runs


def spread(vals):
    m = statistics.median(vals)
    return 100.0 * (max(vals) - min(vals)) / m if m else float("nan")


def table(runs):
    print("| run | sent | scored | delivery % | p50 ms | p90 ms | p99 ms | retx |")
    print("|---|---|---|---|---|---|---|---|")
    for name, r, _ in runs:
        if r is None:
            print("| %s | NO_RESULT | | | | | | |" % name)
            continue
        print("| %s | %d | %d | %.2f | %.2f | %.2f | %.2f | %d |" % (
            name, r["sent"], r["scored"], r["delivery"], r["p50_us"] / 1e3, r["p90_us"] / 1e3, r["p99_us"] / 1e3,
            r["retransmitted"]))


def groups(runs):
    g = {}
    for name, r, _ in runs:
        g.setdefault(group_key(name), []).append((name, r))
    return g


def v1(runs):
    ok = 0
    for name, r, _ in runs:
        if r is None:
            print("%s: NO_RESULT -> FAIL" % name)
            continue
        good = r["delivery"] >= 99.9 and r["p99_us"] < 40000
        ok += good
        print("%s: delivery %.2f%% p99 %.2f ms -> %s" % (name, r["delivery"], r["p99_us"] / 1e3, "PASS" if good else "FAIL"))
    print("V1: %s (%d of %d reps)" % ("PASS" if ok == len(runs) == 3 else "FAIL", ok, len(runs)))


def noise(runs):
    print("| group | runs | delivered spread % | PC spread % | vs PC | p50 spread % | p99 spread % | retx spread % | A2 vs A (delivered) | noise rule |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for key, items in sorted(groups(runs).items()):
        rs = [r for _, r in items if r]
        if len(rs) < 2:
            print("| %s | %d | too few results | | | | | | | |" % (key, len(rs)))
            continue
        sp = spread([r["scored"] for r in rs])
        parts = key.split("_")
        pc = PC_FLOOR.get((parts[0], parts[1]))
        if pc is None:
            verdict = "n/a"
        elif abs(sp - pc) < 0.3:
            verdict = "TIE"
        else:
            verdict = "quieter" if sp < pc else "noisier"
        a2 = [r["scored"] for n, r in items if r and re.search(r"_A2_", n)]
        a = [r["scored"] for n, r in items if r and not re.search(r"_A2_", n)]
        if a and a2:
            d = abs(statistics.median(a2) - statistics.median(a)) / statistics.median(a) * 100
            rule = "VOID" if d > 10 else "ok"
            ab = "%.2f%%" % d
        else:
            ab, rule = "n/a", "n/a"
        retx = [r["retransmitted"] for r in rs]
        print("| %s | %d | %.2f | %s | %s | %.1f | %.1f | %.1f | %s | %s |" % (
            key, len(rs), sp, "%.1f" % pc if pc is not None else "n/a", verdict,
            spread([r["p50_us"] for r in rs]), spread([r["p99_us"] for r in rs]),
            spread(retx) if statistics.median(retx) else 0.0, ab, rule))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    mode = "table"
    if argv[1] in ("--v1", "--noise"):
        mode = argv[1][2:]
        argv = argv[1:]
    runs = load(argv[1:])
    if not runs:
        print("no runs found")
        return 1
    if mode == "v1":
        v1(runs)
    elif mode == "noise":
        table(runs)
        print()
        noise(runs)
    else:
        table(runs)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
