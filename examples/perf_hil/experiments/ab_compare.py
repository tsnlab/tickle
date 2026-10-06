#!/usr/bin/env python3
"""A/B comparison of one TickLE build against another, per cell and metric.

Written for WIRE_PLAN.md section 6's no-regression rule and RMW_PERF_PLAN.md 8.6 (2026-09-27): for every
cell, each metric's mean over the repetitions in arm A (the parent) and arm B (the change), the difference,
and t = (B - A) / sqrt(SE_A^2 + SE_B^2). With SE 0 in both arms (a deterministic figure such as wire bytes
per sample) any difference counts as beyond. Only rows whose verdict is ok are used; the void count per arm
is printed.

PRIMARY METRICS DECIDE; NOTHING ELSE DOES (the user, 2026-10-06). A chain reads ~40 metrics, and at 2 x SE
each has about a 4.6% chance of crossing by noise alone, so two or three cross on every run whatever the
change did. The verdict is therefore taken on the metrics pre-registered with --primary only:

  --primary "metric[,metric...]"   each item is one of
      name              e.g. recv_mbps          - that metric, every role, every cell
      role.name         e.g. server.recv_mbps   - that role only, every cell
      cN:name           e.g. c6:server.cpu_s_per_Msample (or c6:rtt_avg_ms) - that cell only
  k = the number of (cell, metric) comparisons the items select. Each is judged at a Bonferroni threshold:
  the two-sided normal z at a per-comparison alpha of 0.0455 / k, which is exactly 2 x SE at k = 1 and
  grows with k (k=3 -> 2.43, k=10 -> 2.84), so the chance that ANY primary crosses by noise stays ~4.6%.

  VERDICT: WORSE   some primary is beyond the threshold in its bad direction
  VERDICT: PASS    no primary is (better and held both pass; the counts are printed)
  NO VERDICT       no --primary given, or an item selects nothing (a pre-registered metric that was not
                   measured must not pass by being absent), or a selected metric has no non-void rows

Every other metric is printed as "secondary", still flagged at plain 2 x SE so a reader can see it move, and
never contributes to the verdict. Without --primary the whole table is printed and the verdict line reads
"NO VERDICT: no primary metrics pre-registered". Exit status: 0 PASS, 1 WORSE, 3 NO VERDICT, 2 usage.

Usage:
  ab_compare.py campaign A.txt B.txt [--primary SPEC]       campaign_sweep.sh outputs (TickLE rows)
  ab_compare.py rmw --a A1.txt [A2.txt ...] --b B1.txt [...] [--primary SPEC]
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
FAMILY_ALPHA = 0.0455   # two-sided P(|z| > 2), the 2 x SE rule's own false-positive rate
SECONDARY_Z = 2.0


def stats(vals):
    n = len(vals)
    if n == 0:
        return None
    mean = statistics.fmean(vals)
    se = statistics.stdev(vals) / math.sqrt(n) if n > 1 else float("nan")
    return mean, se, n


def judge(a, b, hi_better, z):
    ma, sa, _ = a
    mb, sb, _ = b
    diff = mb - ma
    se = math.sqrt((0 if math.isnan(sa) else sa) ** 2 + (0 if math.isnan(sb) else sb) ** 2)
    if se == 0:
        t = 0.0 if diff == 0 else math.copysign(math.inf, diff)
    else:
        t = diff / se
    good = (t > z) if hi_better else (t < -z)
    bad = (t < -z) if hi_better else (t > z)
    return diff, t, ("better" if good else "WORSE" if bad else "held")


def bonferroni_z(k):
    """The two-sided normal threshold for k comparisons sharing FAMILY_ALPHA."""
    if k <= 1:
        return SECONDARY_Z
    return statistics.NormalDist().inv_cdf(1 - FAMILY_ALPHA / k / 2)


def parse_primary(spec):
    """'a,c6:server.b' -> [(cell_or_None, metric)]; raises ValueError on an empty item."""
    items = []
    for raw in (spec or "").split(","):
        raw = raw.strip()
        if not raw:
            continue
        m = re.fullmatch(r"(?:c(\d+):)?([A-Za-z_][\w.]*)", raw)
        if not m:
            raise ValueError(f"cannot read primary item {raw!r} (want name, role.name or cN:name)")
        items.append((int(m[1]) if m[1] else None, m[2]))
    return items


def cell_number(cell):
    m = re.match(r"c(\d+)\s", cell)
    return int(m[1]) if m else None


def selects(item, cell, metric):
    num, name = item
    if num is not None and cell_number(cell) != num:
        return False
    return metric == name or ("." not in name and metric.split(".", 1)[-1] == name)


def line(tag, v, cell, metric, a, b, diff, t):
    pct = 100 * diff / a[0] if a[0] else float("nan")
    print(f"{tag:9s} {v:6s} {cell:44s} {metric:32s} A {a[0]:12.4f} (se {a[1]:.4f}, n{a[2]})  B {b[0]:12.4f} "
          f"(se {b[1]:.4f}, n{b[2]})  {pct:+6.1f}%  t {t:+.1f}")


def report(rows, primary_spec):
    """Prints the table and the verdict; returns the exit status (0 PASS, 1 WORSE, 3 NO VERDICT)."""
    items = parse_primary(primary_spec)
    chosen = {i for i, (cell, metric, *_rest) in enumerate(rows) if any(selects(it, cell, metric) for it in items)}
    unmatched = [it for it in items if not any(selects(it, r[0], r[1]) for r in rows)]
    k = len(chosen)
    z = bonferroni_z(k)
    counts = {"better": 0, "WORSE": 0, "held": 0}
    sec = {"better": 0, "WORSE": 0, "held": 0}
    if items:
        print(f"--- primary: {primary_spec}  ->  k = {k} comparison(s), Bonferroni threshold |t| > {z:.2f}")
    for i, (cell, metric, a, b, hi) in enumerate(rows):
        if i in chosen:
            diff, t, v = judge(a, b, hi, z)
            counts[v] += 1
            line("PRIMARY", v, cell, metric, a, b, diff, t)
    for i, (cell, metric, a, b, hi) in enumerate(rows):
        if i not in chosen:
            diff, t, v = judge(a, b, hi, SECONDARY_Z)
            sec[v] += 1
            line("secondary", v, cell, metric, a, b, diff, t)
    print(f"--- secondary (never decide, flagged at 2 x SE): {sec['better']} better, {sec['held']} held, "
          f"{sec['WORSE']} WORSE of {sum(sec.values())}; at 2 x SE about {FAMILY_ALPHA * sum(sec.values()):.1f} "
          "cross by noise alone")
    if not items:
        print("NO VERDICT: no primary metrics pre-registered (pass --primary, or PRIMARY= to campaign_ab_chain.sh)")
        return 3
    if unmatched:
        names = ", ".join((f"c{n}:" if n else "") + m for n, m in unmatched)
        print(f"NO VERDICT: primary item(s) {names} matched no non-void (cell, metric) in both arms")
        return 3
    print(f"--- primary: {counts['better']} better, {counts['held']} held, {counts['WORSE']} WORSE of {k}")
    if counts["WORSE"]:
        print(f"VERDICT: WORSE ({counts['WORSE']} of {k} primary comparison(s) beyond |t| > {z:.2f})")
        return 1
    print(f"VERDICT: PASS (no primary comparison of {k} worse beyond |t| > {z:.2f})")
    return 0


def campaign(pa, pb, primary):
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
    return report(rows, primary)


def rmw_parse(paths):
    cells, voids = OrderedDict(), 0
    for path in paths:
        for text in open(path, encoding="utf-8"):
            m = RMW_LINE.match(text.rstrip())
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


def rmw(pa, pb, primary):
    ca, va = rmw_parse(pa)
    cb, vb = rmw_parse(pb)
    print(f"void rows: A {va}, B {vb}")
    rows = []
    for cell in ca:
        for k in RMW_METRICS:
            a, b = stats(ca[cell].get(k, [])), stats(cb.get(cell, {}).get(k, []))
            if a and b:
                rows.append((cell, k, a, b, False))
    return report(rows, primary)


def main(argv):
    args = list(argv[1:])
    primary = ""
    if "--primary" in args:
        i = args.index("--primary")
        if i + 1 >= len(args):
            print(__doc__)
            return 2
        primary = args[i + 1]
        del args[i:i + 2]
    try:
        parse_primary(primary)
    except ValueError as err:
        print(err)
        return 2
    if len(args) == 3 and args[0] == "campaign":
        return campaign(args[1], args[2], primary)
    if len(args) > 3 and args[0] == "rmw" and "--a" in args and "--b" in args:
        ia, ib = args.index("--a"), args.index("--b")
        return rmw(args[ia + 1:ib], args[ib + 1:], primary)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
