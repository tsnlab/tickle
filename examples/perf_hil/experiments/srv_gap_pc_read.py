#!/usr/bin/env python3
"""srv_gap_pc_read.py - reads srv_gap_pc.sh's results by the rules its header pre-registers.

Usage: srv_gap_pc_read.py RESULTS --steps "A>C3 C3>H H>B" [--total A>B] [--extra "M>B"] [--layout A>B,PA>PB]
       [--header N]   (N: read the N-th "=== srv_gap_pc" section, default the last)

Per (cell, role, metric) - roles srv and cli, metrics insn_u and insn_k per sample:
  floor     |BASE2 - first arm| (the A/A floor; BASE2 is the first arm run again in its own rotation slots).
  MOVED     |d| > max(2 x floor, 3 x SE(d), 1.0) - SE(d) from the two arms' block-to-block spread.
  steps     each listed step's d, and whether the steps' sum comes within 3 SE of the total ("NOT ADDITIVE" if not).
  layout    total (B - A) against the placement arms' (PB - PA): SURVIVES LAYOUT when both MOVED the same way and differ
            by less than half of B - A; LAYOUT when PB - PA did not move; MIXED otherwise.
  An arm with fewer than 2 valid blocks in a cell is not read there.
"""
import argparse
import math
import re
import statistics
import sys

RUN = re.compile(r"^RUN arm=(\S+) sha=\S+ flavor=\S+ cell=(\S+) block=(\S+) mode=perf client_sent=(\d+) "
                 r"server_samples=(\d+) srv_insn_u=(\d+) srv_insn_k=(\d+) cli_insn_u=(\d+) cli_insn_k=(\d+)")
METRICS = ("srv_insn_u", "srv_insn_k", "cli_insn_u", "cli_insn_k")
MIN_BLOCKS = 2


def load(path, header):
    sections = []
    for line in open(path, encoding="utf-8"):
        if line.startswith("=== srv_gap_pc"):
            sections.append([])
        elif sections and (m := RUN.match(line)):
            sections[-1].append(m)
    if not sections:
        sys.exit("no srv_gap_pc section")
    data = {}
    for m in sections[header]:
        arm, cell = m.group(1), m.group(2)
        sent, srv_n = int(m.group(4)), int(m.group(5))
        vals = [int(m.group(i)) for i in range(6, 10)]
        per = {"srv_insn_u": vals[0] / srv_n, "srv_insn_k": vals[1] / srv_n,
               "cli_insn_u": vals[2] / sent, "cli_insn_k": vals[3] / sent}
        for k, v in per.items():
            data.setdefault((cell, k), {}).setdefault(arm, []).append(v)
    return data


def mean_se(xs):
    if len(xs) < MIN_BLOCKS:
        return None
    return statistics.fmean(xs), statistics.stdev(xs) / math.sqrt(len(xs))


def diff(arms, a, b):
    """b - a, its SE; None when either arm is not read."""
    if a not in arms or b not in arms:
        return None
    ma, mb = mean_se(arms[a]), mean_se(arms[b])
    if ma is None or mb is None:
        return None
    return mb[0] - ma[0], math.hypot(ma[1], mb[1])


def moved(d, floor):
    return abs(d[0]) > max(2.0 * floor, 3.0 * d[1], 1.0)


def fmt(d, floor):
    if d is None:
        return "not read"
    return f"{d[0]:+9.2f} (se {d[1]:6.2f}) {'MOVED' if moved(d, floor) else 'flat '}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--steps", required=True)
    ap.add_argument("--total")
    ap.add_argument("--extra", default="")
    ap.add_argument("--layout")
    ap.add_argument("--header", type=int, default=-1)
    args = ap.parse_args()
    data = load(args.results, args.header)
    steps = [s.split(">") for s in args.steps.split()]
    extra = [s.split(">") for s in args.extra.split()]
    first = steps[0][0]
    for (cell, metric), arms in sorted(data.items()):
        base = diff(arms, first, "BASE2")
        floor = abs(base[0]) if base else float("nan")
        means = " ".join(f"{a}={statistics.fmean(v):.2f}(n{len(v)})" for a, v in sorted(arms.items()))
        print(f"{cell:8s} {metric:10s} A/A floor {floor:7.2f}  | {means}")
        tot_sum, tot_var, all_read = 0.0, 0.0, True
        for a, b in steps:
            d = diff(arms, a, b)
            print(f"    step {a:>4s} -> {b:<4s} {fmt(d, floor)}")
            if d is None:
                all_read = False
            else:
                tot_sum += d[0]
                tot_var += d[1] ** 2
        if args.total:
            a, b = args.total.split(">")
            d = diff(arms, a, b)
            add = ""
            if d is not None and all_read:
                add = "additive" if abs(tot_sum - d[0]) <= 3.0 * math.sqrt(tot_var + d[1] ** 2) else "NOT ADDITIVE"
            print(f"    total {a:>4s} -> {b:<4s} {fmt(d, floor)}  steps sum {tot_sum:+.2f}: {add}")
        for a, b in extra:
            print(f"    extra {a:>4s} -> {b:<4s} {fmt(diff(arms, a, b), floor)}")
        if args.layout:
            (a, b), (pa, pb) = (x.split(">") for x in args.layout.split(","))
            d, p = diff(arms, a, b), diff(arms, pa, pb)
            if d is None or p is None:
                verdict = "not read"
            elif not moved(p, floor):
                verdict = "LAYOUT" if moved(d, floor) else "nothing to place"
            elif moved(d, floor) and (d[0] > 0) == (p[0] > 0) and abs(d[0] - p[0]) < 0.5 * abs(d[0]):
                verdict = "SURVIVES LAYOUT"
            else:
                verdict = "MIXED"
            print(f"    layout {pa}->{pb} {fmt(p, floor)}  vs {a}->{b}: {verdict}")


if __name__ == "__main__":
    main()
