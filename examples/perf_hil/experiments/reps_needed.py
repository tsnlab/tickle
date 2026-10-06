#!/usr/bin/env python3
"""How many repetitions an A/B needs, from the rep-to-rep spread campaigns have already measured.

The user, 2026-10-06: choose REPS from data rather than habit. For each cell, framework and scored metric (campaign_
summary.DIRECTION, role-qualified as ab_compare.py reads them) this reads every given campaign_sweep.sh output, takes
the sample variance of the repetitions WITHIN each file, and pools those variances over the files (weighted by their
degrees of freedom). Within a file, because files are often the two arms of an A/B, and pooling them as one sample
would count the change itself as noise. A file that is already two blocks (campaign_ab_chain's _A.txt = b1 + b4)
keeps its block-to-block drift, which is the noise the A/B design really faces.

From the pooled SD and the mean, CV = SD / mean, and for a relative change delta the repetitions per ARM needed so
that ab_compare.py's rule (|t| > z, t = diff / sqrt(SE_A^2 + SE_B^2)) sees it with the given power:

    n = 2 * (z + z_power)^2 * CV^2 / delta^2        z = 2 (2 x SE), z_power = 0.84 for 80% power

(at 50% power, z_power = 0, n is about half). Normal approximation; with few reps the t distribution makes the real
need somewhat larger, so these are floors. A metric with SD 0 (a deterministic count) needs 2. The recommended REPS
per cell is the largest n over its metrics (or over --metrics), and since an A/B arm is two blocks (A B B A), the
campaign_ab_chain.sh REPS per block is half of it, rounded up.

Not wired into anything (the user's instruction): read the table, then choose REPS and PRIMARY yourself.

Usage: reps_needed.py [--delta 1,3] [--power 0.8] [--fw tickle] [--metrics name,...] [--cap 100] FILE...
"""
import argparse
import math
import statistics
import sys
from collections import OrderedDict

sys.path.insert(0, __import__("os").path.dirname(__file__))
from campaign_summary import DIRECTION, parse  # noqa: E402

Z_SE = 2.0


def z_for_power(power):
    return statistics.NormalDist().inv_cdf(power) if power > 0.5 else 0.0


def pooled(per_file):
    """[(values from one file)] -> (mean over all, pooled within-file SD, total n, pooled df) or None."""
    num = df = 0.0
    allv = []
    for vals in per_file:
        allv.extend(vals)
        if len(vals) >= 2:
            num += statistics.variance(vals) * (len(vals) - 1)
            df += len(vals) - 1
    if df == 0 or not allv:
        return None
    return statistics.fmean(allv), math.sqrt(num / df), len(allv), int(df)


def reps(cv, delta, zp):
    if cv == 0:
        return 2
    return max(2, math.ceil(2 * (Z_SE + zp) ** 2 * cv * cv / (delta * delta)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--delta", default="1,3", help="relative changes in percent, comma-separated (default 1,3)")
    ap.add_argument("--power", type=float, default=0.8)
    ap.add_argument("--fw", default="tickle", help="framework, or 'all'")
    ap.add_argument("--metrics", default="", help="only these metric names (e.g. send_mbps,rtt_avg_ms)")
    ap.add_argument("--cap", type=int, default=1000, help="print >cap instead of larger counts")
    args = ap.parse_args()
    deltas = [float(d) / 100 for d in args.delta.split(",") if d.strip()]
    zp = z_for_power(args.power)
    only = {m.strip() for m in args.metrics.split(",") if m.strip()}

    # (cell, fw, metric) -> [values of file 1, values of file 2, ...]
    data = OrderedDict()
    for path in args.files:
        for key, fws in parse(path).items():
            cell = "c%d %s %s %s %s" % key
            for fw, d in fws.items():
                if args.fw != "all" and fw != args.fw:
                    continue
                for metric, vals in d["vals"].items():
                    name = metric.split(".", 1)[1]
                    if name not in DIRECTION or (only and name not in only):
                        continue
                    data.setdefault((cell, fw, metric), []).append(vals)

    def fmt(n):
        return f">{args.cap}" if n > args.cap else str(n)

    dcols = "".join(f"  n@{d * 100:g}%" for d in deltas)
    print(f"reps per ARM for a relative change at |t| > {Z_SE:g} with {args.power:.0%} power, from "
          f"{len(args.files)} file(s); SD pooled within files")
    # df = the degrees of freedom behind the pooled SD. At df 2 (one file of 3 reps) the SD itself is uncertain by a
    # factor of ~2 either way, and n goes with its square: treat those rows as an order of magnitude, not a count.
    print(f"{'cell':34s} {'fw':10s} {'metric':30s} {'df':>4s} {'mean':>12s} {'sd':>10s} {'cv%':>6s}{dcols}")
    per_cell = OrderedDict()
    for (cell, fw, metric), per_file in data.items():
        p = pooled(per_file)
        if p is None:
            continue
        mean, sd, _n, df = p
        if mean == 0:
            # A relative change of a zero (loss_pct on a lossless link) is undefined; listed, never recommending.
            print(f"{cell:34s} {fw:10s} {metric:30s} {df:4d} {mean:12.4g} {sd:10.4g} {'-':>6s}   (mean 0: no "
                  "relative change to detect)")
            continue
        cv = sd / abs(mean)
        need = [reps(cv, d, zp) for d in deltas]
        print(f"{cell:34s} {fw:10s} {metric:30s} {df:4d} {mean:12.4g} {sd:10.4g} {cv * 100:6.2f}"
              + "".join(f"  {fmt(x):>{len(f'  n@{d * 100:g}%') - 2}}" for x, d in zip(need, deltas)))
        cur = per_cell.setdefault((cell, fw), [(0, "")] * len(deltas))
        per_cell[(cell, fw)] = [max(c, (x, metric)) for c, x in zip(cur, need)]

    print()
    print("recommended per cell (the metric that needs most decides; REPS/block = ceil(n/2) for campaign_ab_chain):")
    for (cell, fw), best in per_cell.items():
        parts = []
        for d, (n, metric) in zip(deltas, best):
            parts.append(f"{d * 100:g}%: n={fmt(n)} REPS/block={fmt(math.ceil(n / 2))} ({metric})")
        print(f"  {cell:34s} {fw:10s} " + " | ".join(parts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
