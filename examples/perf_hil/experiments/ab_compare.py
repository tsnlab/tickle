#!/usr/bin/env python3
"""A/B comparison of one TickLE build against another, per cell and metric, at 2 x SE.

Written for WIRE_PLAN.md section 6's no-regression rule and RMW_PERF_PLAN.md 8.6 (2026-09-27): for every
cell, each metric's mean over the repetitions in arm A (the parent) and arm B (the change), the difference,
and t = (B - A) / sqrt(SE_A^2 + SE_B^2). A metric is

  better  |t| > 2 in its good direction
  WORSE   |t| > 2 in its bad direction
  held    otherwise (listed too: "nothing moved" is a result, not an absence of one)

With SE 0 in both arms (a deterministic figure such as wire bytes per sample) any difference counts as beyond.
Only rows whose verdict is ok are used; the void count per arm is printed.

Usage:
  ab_compare.py campaign A.txt B.txt          campaign_sweep.sh outputs (TickLE rows)
  ab_compare.py rmw --a A1.txt [A2.txt ...] --b B1.txt [...]
                                              rmw_crosshost_rtt.sh outputs; several files per arm are pooled
"""
import math
import re
import statistics
import sys
from collections import OrderedDict

sys.path.insert(0, __import__("os").path.dirname(__file__))
from campaign_summary import DIRECTION, parse  # noqa: E402

# rmw rows: lower is better for all of them
RMW_METRICS = ("rtt_avg_ms", "rtt_min_ms", "pong_cpu_ns", "pong_idle_cpu_ns", "ping_cpu_s", "pong_maxrss_kb",
               "ping_maxrss_kb")
RMW_LINE = re.compile(r"^(?P<rmw>rmw_\w+) (?P<msg>\w+) (?P<qos>\w+) rep(?P<rep>\d+)(?P<tags>[^|]*)\|\s*(?P<verdict>\S+)"
                      r"[^|]*\|\s*(?P<fields>.*)$")


def stats(vals):
    n = len(vals)
    if n == 0:
        return None
    mean = statistics.fmean(vals)
    se = statistics.stdev(vals) / math.sqrt(n) if n > 1 else float("nan")
    return mean, se, n


def judge(a, b, hi_better):
    ma, sa, _ = a
    mb, sb, _ = b
    diff = mb - ma
    se = math.sqrt((0 if math.isnan(sa) else sa) ** 2 + (0 if math.isnan(sb) else sb) ** 2)
    if se == 0:
        t = 0.0 if diff == 0 else math.copysign(math.inf, diff)
    else:
        t = diff / se
    good = (t > 2) if hi_better else (t < -2)
    bad = (t < -2) if hi_better else (t > 2)
    return diff, t, ("better" if good else "WORSE" if bad else "held")


def report(rows):
    counts = {"better": 0, "WORSE": 0, "held": 0}
    for cell, metric, a, b, hi in rows:
        diff, t, v = judge(a, b, hi)
        counts[v] += 1
        pct = 100 * diff / a[0] if a[0] else float("nan")
        print(f"{v:6s} {cell:44s} {metric:32s} A {a[0]:12.4f} (se {a[1]:.4f}, n{a[2]})  B {b[0]:12.4f} "
              f"(se {b[1]:.4f}, n{b[2]})  {pct:+6.1f}%  t {t:+.1f}")
    print(f"--- {counts['better']} better, {counts['held']} held, {counts['WORSE']} WORSE")
    return counts["WORSE"]


def campaign(pa, pb):
    ca, cb = parse(pa), parse(pb)
    rows = []
    for key in ca:
        if key not in cb:
            print(f"cell {key} missing in B")
            continue
        fa, fb = ca[key].get("tickle"), cb[key].get("tickle")
        if not fa or not fb:
            continue
        cell = "c%d %s %s %s %s" % key
        if fa["void"] or fb["void"]:
            print(f"{cell}: void A {len(fa['void'])} B {len(fb['void'])}")
        for metric in sorted(set(fa["vals"]) & set(fb["vals"])):
            name = metric.split(".", 1)[1]
            if name not in DIRECTION:
                continue
            a, b = stats(fa["vals"][metric]), stats(fb["vals"][metric])
            if a and b:
                rows.append((cell, metric, a, b, DIRECTION[name]))
    return report(rows)


def rmw_parse(paths):
    cells, voids = OrderedDict(), 0
    for path in paths:
        for line in open(path, encoding="utf-8"):
            m = RMW_LINE.match(line.rstrip())
            if not m:
                continue
            if not m["verdict"].startswith("ok"):
                voids += 1
                continue
            f = dict(re.findall(r"([A-Za-z_]\w*)=([-\d.]+)", m["fields"]))
            if "ping_utime_s" in f and "ping_stime_s" in f:
                f["ping_cpu_s"] = float(f["ping_utime_s"]) + float(f["ping_stime_s"])
            tags = " ".join(t for t in m["tags"].split() if not t.startswith("execpoll="))
            cell = f"{m['rmw']} {m['msg']} {m['qos']} {tags}".strip()
            d = cells.setdefault(cell, {})
            for k in RMW_METRICS:
                if k in f:
                    d.setdefault(k, []).append(float(f[k]))
    return cells, voids


def rmw(pa, pb):
    ca, va = rmw_parse(pa)
    cb, vb = rmw_parse(pb)
    print(f"void rows: A {va}, B {vb}")
    rows = []
    for cell in ca:
        for k in RMW_METRICS:
            a, b = stats(ca[cell].get(k, [])), stats(cb.get(cell, {}).get(k, []))
            if a and b:
                rows.append((cell, k, a, b, False))
    return report(rows)


def main(argv):
    if len(argv) == 4 and argv[1] == "campaign":
        return 1 if campaign(argv[2], argv[3]) else 0
    if len(argv) > 4 and argv[1] == "rmw" and "--a" in argv and "--b" in argv:
        ia, ib = argv.index("--a"), argv.index("--b")
        return 1 if rmw(argv[ia + 1:ib], argv[ib + 1:]) else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
