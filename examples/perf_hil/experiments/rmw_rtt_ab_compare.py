#!/usr/bin/env python3
"""An A/B of one rmw cross-host RTT cell from interleaved rmw_crosshost_rtt.sh blocks, read by two rules at once.

Written for ~/rig_queue_largemsg_A4.sh stage 1 (2026-10-10) before its run: the step-A L2 queue A3 held the cell
array1k / best_effort / poll at 511.8 -> 515.7 us (+0.7%, t +3.96) only because CycloneDDS moved -0.9% in the same
session; by docs/DESIGN.md section 8 L2's literal wording ("A WORSE metric outside the parent-vs-parent spread blocks
landing") it was WORSE. This re-runs that cell alone and prints both readings, so neither is chosen after the fact.

Input: --block A:<file> / --block B:<file>, in run order (e.g. A B B A A B B A), each one rmw_crosshost_rtt.sh output
(one block = one build of that commit on both Pis). --cell msg/qos/wait picks the cell (rows without wait= are poll).

HOW TO READ IT (pre-registered, enforced below):
  VOID row: status not 'ok', loss_pct not 0, RESULT's framework not the row's rmw, or RESULT's wait= not the cell's
    (a row without wait= in its label is a poll row, rmw_crosshost_rtt.sh's default). Listed, never averaged.
  NO VERDICT: fewer than --min-n usable rmw_tickle reps in either arm, fewer than two A blocks, or a vendor without
    two usable reps per arm (the control would be missing).
  PRIMARIES (k = 2): rmw_tickle's rtt_avg_us (the ping's mean of its 100 round trips, x 1000) and cpu_us_per_rt
    ((pong_cpu_ns / 1000 + (ping utime + stime) x 1e6) / sent). Reps of every A block pooled against every B block;
    Welch t = (mean B - mean A) / sqrt(var A / nA + var B / nB); Bonferroni z_k at family alpha 4.55% two-sided
    (z = 2.28 for k = 2). d% = 100 x (mean B - mean A) / mean A. Both metrics are lower-is-better.
  PARENT-VS-PARENT SPREAD P% (the design's control): 100 x (largest - smallest A block mean) / mean A. (B-vs-B is
    printed beside it, not used.)
  VENDOR DRIFT V% (the rig's control): for rmw_fastrtps_cpp and rmw_cyclonedds_cpp, 100 x |median B reps - median A
    reps| / median A of the same metric, in the same blocks; V% is the larger of the two.
  RULE L (docs/DESIGN.md L2, literal): WORSE if t > z_k and d% > P%; BETTER if t < -z_k and -d% > P%; else HELD.
  RULE V (the A3 reading): the same with V% in place of P%.
  LANDING follows RULE L, the design's rule; RULE V is printed with it and does not overturn it:
    L HELD/BETTER                -> PASS: A3's rise did not reproduce beyond the parent's own spread.
    L WORSE and V WORSE          -> WORSE: blocks landing (docs/DESIGN.md L2).
    L WORSE and V HELD           -> WORSE (vendors moved as much in this session): blocks landing by the design's
                                    rule; the vendor drift is reported so Plan can see it, not used to excuse it.
"""
import argparse
import math
import re
import statistics as st
import sys
from statistics import NormalDist

FAMILY_ALPHA = 0.0455
ROW = re.compile(r"^(\S+) (\S+) (\S+) rep(\d+)(?: wait=(\S+))?(?: [^|]*)? \| (\S+) \|(.*)$")
VENDORS = ("rmw_fastrtps_cpp", "rmw_cyclonedds_cpp")
METRICS = ("rtt_avg_us", "cpu_us_per_rt")


def read_block(path, cell):
    """{rmw: {metric: [values]}} and the VOID lines, for one block file and one cell."""
    out, voids = {}, []
    with open(path, errors="replace") as f:
        for line in f:
            m = ROW.match(line.rstrip("\n"))
            if not m:
                continue
            rmw, msg, qos, rep, wait, status, rest = m.groups()
            if f"{msg}/{qos}/{wait or 'poll'}" != cell:
                continue
            fields = dict(re.findall(r"(\w+)=(\S+)", rest))
            want_wait = wait or "poll"
            if (status != "ok" or fields.get("loss_pct") != "0" or fields.get("framework") != rmw
                    or fields.get("wait", "poll") != want_wait):
                voids.append(f"{path}: {rmw} rep{rep} {status} loss_pct={fields.get('loss_pct')} "
                             f"framework={fields.get('framework')} wait={fields.get('wait')}")
                continue
            sent = float(fields["sent"])
            d = out.setdefault(rmw, {m_: [] for m_ in METRICS})
            d["rtt_avg_us"].append(float(fields["rtt_avg_ms"]) * 1e3)
            d["cpu_us_per_rt"].append((float(fields["pong_cpu_ns"]) / 1e3
                                       + (float(fields["ping_utime_s"]) + float(fields["ping_stime_s"])) * 1e6) / sent)
    return out, voids


def welch_t(a, b):
    se = math.sqrt(st.variance(a) / len(a) + st.variance(b) / len(b))
    return (st.mean(b) - st.mean(a)) / se if se > 0 else 0.0


def rule(t, dpct, ctl, z):
    if t > z and dpct > ctl:
        return "WORSE"
    if t < -z and -dpct > ctl:
        return "BETTER"
    return "HELD"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--block", action="append", required=True, help="A:<file> or B:<file>, in run order")
    ap.add_argument("--cell", default="array1k/best_effort/poll")
    ap.add_argument("--min-n", type=int, default=12, help="usable rmw_tickle reps needed per arm")
    args = ap.parse_args()

    blocks = []  # (arm, path, data)
    voids = []
    for spec in args.block:
        arm, _, path = spec.partition(":")
        if arm not in ("A", "B") or not path:
            sys.exit(f"--block takes A:<file> or B:<file>, not {spec}")
        data, v = read_block(path, args.cell)
        blocks.append((arm, path, data))
        voids += v
    order = "".join(b[0] for b in blocks)
    k = len(METRICS)
    z = NormalDist().inv_cdf(1 - FAMILY_ALPHA / (2 * k))
    print(f"cell {args.cell}; blocks in run order {order}; primaries k={k}, z_k={z:.2f}; units us")
    for arm, path, data in blocks:
        tk = data.get("rmw_tickle", {}).get("rtt_avg_us", [])
        print(f"  {arm} {path}: rmw_tickle n={len(tk)} rtt mean {st.mean(tk) if tk else float('nan'):.1f}")
    if voids:
        print("VOID rows:\n  " + "\n  ".join(voids))

    def pooled(arm, rmw, metric):
        return [x for a, _, d in blocks if a == arm for x in d.get(rmw, {}).get(metric, [])]

    def block_means(arm, metric):
        return [st.mean(d["rmw_tickle"][metric]) for a, _, d in blocks
                if a == arm and len(d.get("rmw_tickle", {}).get(metric, [])) >= 1]

    why = []
    for arm in ("A", "B"):
        n = len(pooled(arm, "rmw_tickle", "rtt_avg_us"))
        if n < args.min_n:
            why.append(f"rmw_tickle has {n} usable reps in arm {arm} (< {args.min_n})")
        for v in VENDORS:
            if len(pooled(arm, v, "rtt_avg_us")) < 2:
                why.append(f"{v} has fewer than 2 usable reps in arm {arm}")
    if len(block_means("A", "rtt_avg_us")) < 2:
        why.append("fewer than two A blocks: no parent-vs-parent spread")
    if why:
        print("NO VERDICT: " + "; ".join(why))
        return 2

    print(f"{'metric':14} {'A mean':>8} {'B mean':>8} {'d%':>7} {'t':>6} {'nA/nB':>6} {'P%':>6} {'BvB%':>6} "
          f"{'V%':>6}  rule L   rule V")
    readings = {}
    for metric in METRICS:
        a, b = pooled("A", "rmw_tickle", metric), pooled("B", "rmw_tickle", metric)
        t = welch_t(a, b)
        dpct = 100 * (st.mean(b) - st.mean(a)) / st.mean(a)
        am, bm = block_means("A", metric), block_means("B", metric)
        p = 100 * (max(am) - min(am)) / st.mean(a)
        bvb = 100 * (max(bm) - min(bm)) / st.mean(b) if len(bm) >= 2 else float("nan")
        drifts = []
        for v in VENDORS:
            va, vb = pooled("A", v, metric), pooled("B", v, metric)
            drifts.append((v, 100 * (st.median(vb) - st.median(va)) / st.median(va)))
        vpct = max(abs(x) for _, x in drifts)
        rl, rv = rule(t, dpct, p, z), rule(t, dpct, vpct, z)
        readings[metric] = (rl, rv)
        print(f"{metric:14} {st.mean(a):8.1f} {st.mean(b):8.1f} {dpct:+7.2f} {t:+6.2f} {len(a):>3}/{len(b):<3}"
              f"{p:6.2f} {bvb:6.2f} {vpct:6.2f}  {rl:7}  {rv}")
        print(f"{'':14} A block means {', '.join(f'{x:.1f}' for x in am)}; B block means "
              f"{', '.join(f'{x:.1f}' for x in bm)}; vendors B-A: "
              + ", ".join(f"{v[4:]} {x:+.2f}%" for v, x in drifts))
    print()
    worst = "PASS"
    for metric, (rl, rv) in readings.items():
        if rl == "WORSE" and rv == "WORSE":
            landing = "WORSE: blocks landing (docs/DESIGN.md L2)"
        elif rl == "WORSE":
            landing = ("WORSE (vendors moved as much in this session): blocks landing by the design's rule; the "
                       "vendor drift does not excuse it")
        elif rl == "BETTER":
            landing = "BETTER"
        else:
            landing = "PASS: not worse beyond the parent's own spread"
        if rl == "WORSE":
            worst = "WORSE"
        elif rl == "BETTER" and worst == "PASS":
            worst = "IMPROVED"
        print(f"  {metric}: rule L {rl}, rule V {rv} -> {landing}")
    print(f"VERDICT (rule L, the design's): {worst}")
    print(f"vendor-drift reading (rule V, printed, not deciding): "
          f"{'WORSE' if any(rv == 'WORSE' for _, rv in readings.values()) else 'PASS'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
