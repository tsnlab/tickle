#!/usr/bin/env python3
"""eth_storm_p99_read.py - the pre-registered reading of eth_storm_p99_ab.sh (RESULTS `EWF`, 2026-10-27).

Written and committed before the run; this code, not this text, decides. Trees M (main before G, b61e2302), A (M
again), G (45129b71, the progress-held timeout), S (178199cb, G + the stall back-off). Cell: PC netns ETHERNET 1%
random loss (ge1b1), 1 MiB RELIABLE at 30 Hz. NOT the rig's cell (cross-host Array1m 1% at 30 Hz): a null here does
not clear the rig's +0.54 ms.

Per rep: the subscriber's p99 and p50 (echo.log RESULT latency_p99_us / latency_median_us), delivered / published,
writer resends, start load (meta.txt). A rep missing a RESULT is VOID and listed. Reps with start load > nproc / 2,
and STORM reps (resends >= 100,000), are left out of the latency and listed.
VOID (the run): a tree with fewer than 0.9 x the latency reps of the one with the most.
CONTROL (A/A): |median p99 A - median p99 M| <= 0.25 ms (half the shift asked about) -> PASS; else FAIL, and every P99
  verdict is VOID (the PC's spread hides a 0.5 ms shift at this n).
P99, for each pair (G - M, S - M, S - G): d = difference of the median per-rep p99; a 95% bootstrap interval of d
  (2,000 resamples of each tree's reps, seed 1). band = max(|A - M|, 0.10 ms).
  d > band and the interval above 0 -> HIGHER (a cost shown on the PC);
  d < -band and the interval below 0 -> LOWER;
  otherwise, the interval's upper end < 0.5 ms -> NO 0.5 ms SHIFT (a +0.5 ms cost excluded on the PC, not on the rig);
  otherwise UNDECIDED.
Printed beside it, not deciding: median p50 and delivered % per tree, STORM reps per tree.
Usage: eth_storm_p99_read.py OUT TAG
"""
import glob
import os
import random
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
TREES = ("M", "A", "G", "S")


def kv(line):
    return dict(KV.findall(line))


def lines(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def collect(out, tag):
    nproc = os.cpu_count() or 1
    reps = {t: [] for t in TREES}
    for t in TREES:
        for d in sorted(glob.glob(os.path.join(out, f"{tag}_{t}", "runs", "*rel_large*"))):
            meta = kv(" ".join(lines(os.path.join(d, "meta.txt"))))
            pub = [kv(x) for x in lines(os.path.join(d, "ping.log")) if x.startswith("RESULT: role=pub")]
            sub = [kv(x) for x in lines(os.path.join(d, "echo.log")) if x.startswith("RESULT: role=sub")]
            if not pub or not sub:
                print(f"VOID rep {t} {os.path.basename(d)}: missing RESULT")
                continue
            r = {"rep": os.path.basename(d), "load": float(meta.get("load", 0) or 0),
                 "resends": int(pub[0].get("retransmitted", 0)), "published": int(pub[0].get("published", 0)),
                 "delivered": int(sub[0].get("delivered", 0)),
                 "p99": float(sub[0].get("latency_p99_us", "nan")) / 1000,
                 "p50": float(sub[0].get("latency_median_us", "nan")) / 1000}
            r["storm"] = r["resends"] >= 100_000
            r["use"] = not r["storm"] and r["load"] <= nproc / 2 and r["p99"] == r["p99"]
            reps[t].append(r)
    return reps


def boot(a, b, n=2000, seed=1):
    rnd = random.Random(seed)
    ds = sorted(statistics.median(rnd.choices(a, k=len(a))) - statistics.median(rnd.choices(b, k=len(b)))
                for _ in range(n))
    return ds[int(0.025 * n)], ds[int(0.975 * n) - 1]


def main():
    out, tag = sys.argv[1], sys.argv[2]
    reps = collect(out, tag)
    p99 = {t: [r["p99"] for r in reps[t] if r["use"]] for t in TREES}
    for t in TREES:
        left = [r for r in reps[t] if not r["use"]]
        st = sum(1 for r in reps[t] if r["storm"])
        dl = 100.0 * sum(r["delivered"] for r in reps[t]) / max(1, sum(r["published"] for r in reps[t]))
        p50 = [r["p50"] for r in reps[t] if r["use"]]
        print(f"TREE {t}: reps {len(reps[t])}, latency reps {len(p99[t])}, left out {len(left)} (STORM {st}), "
              f"median p99 {statistics.median(p99[t]) if p99[t] else float('nan'):.3f} ms, median p50 "
              f"{statistics.median(p50) if p50 else float('nan'):.3f} ms, delivered {dl:.2f}%")
        for r in left:
            print(f"   left out {r['rep']}: load {r['load']:.1f} resends {r['resends']}")
    most = max(len(v) for v in p99.values())
    void = [t for t in TREES if len(p99[t]) < 0.9 * most or len(p99[t]) < 5]
    if void:
        print(f"VOID run: too few latency reps for {void}")
        return 0
    med = {t: statistics.median(p99[t]) for t in TREES}
    aa = abs(med["A"] - med["M"])
    ctl = "PASS" if aa <= 0.25 else "FAIL"
    print(f"CONTROL A/A: median p99 A {med['A']:.3f} M {med['M']:.3f}, |A - M| {aa:.3f} ms -> {ctl}")
    band = max(aa, 0.10)
    for x, y in (("G", "M"), ("S", "M"), ("S", "G")):
        d = med[x] - med[y]
        lo, hi = boot(p99[x], p99[y])
        if ctl != "PASS":
            v = "VOID (control)"
        elif d > band and lo > 0:
            v = "HIGHER"
        elif d < -band and hi < 0:
            v = "LOWER"
        elif hi < 0.5:
            v = "NO 0.5 ms SHIFT"
        else:
            v = "UNDECIDED"
        print(f"P99 {x} - {y}: {d:+.3f} ms, 95% interval [{lo:+.3f}, {hi:+.3f}], band {band:.3f} -> {v}")
    print("NOTE: PC netns ETHERNET ge1b1, not the rig's cross-host Array1m 1% at 30 Hz; a null here does not clear the "
          "rig's +0.54 ms.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
