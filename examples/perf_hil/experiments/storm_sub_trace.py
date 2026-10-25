#!/usr/bin/env python3
"""storm_sub_trace.py - the reader's side of a traced tv run (storm_trace.patch, RUN_DIR/sub.trace), per BIN ms:
ACKNACKs sent and the units they named (A), units recorded (U), duplicates (D), arrivals not recorded (X), timer
retries (I), and the tv phase. Shows when a storm starts and ends from the reader, whose trace (unlike the writer's,
capped at 4 M events) covers the whole rep.
Usage: storm_sub_trace.py RUN_DIR [--bin MS]
"""
import os
import sys
import time


def main():
    run = sys.argv[1]
    bin_ms = 500.0
    if "--bin" in sys.argv:
        bin_ms = float(sys.argv[sys.argv.index("--bin") + 1])
    off = time.time() - time.monotonic()
    phases = []
    for line in open(os.path.join(run, "tv_phases.txt")):
        f = line.split()
        phases.append((float(f[0]) - off, f[-1]))
    bins = {}
    t0 = None
    for line in open(os.path.join(run, "sub.trace")):
        f = line.split()
        if len(f) != 4:
            continue
        k, t, b = f[0], int(f[1]) / 1e9, int(f[3])
        if t0 is None:
            t0 = t
        r = bins.setdefault(int((t - t0) * 1000 / bin_ms), {"A": 0, "bits": 0, "U": 0, "D": 0, "X": 0, "I": 0, "t": t})
        if k == "A":
            r["A"] += 1
            r["bits"] += b
        elif k in "UDXI":
            r[k] += 1
    print(f"{'t s':>6} {'phase':>5} {'acknk':>6} {'named':>8} {'recorded':>8} {'dup':>7} {'not_rec':>7} {'timer':>6}")
    for i in sorted(bins):
        r = bins[i]
        ph = "-"
        for pt, p in phases:
            if pt <= r["t"]:
                ph = p
        print(f"{i * bin_ms / 1000:6.1f} {ph:>5} {r['A']:6} {r['bits']:8} {r['U']:8} {r['D']:7} {r['X']:7} {r['I']:6}")


if __name__ == "__main__":
    main()
