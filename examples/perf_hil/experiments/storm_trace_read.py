#!/usr/bin/env python3
"""storm_trace_read.py - the tv repair storm's time line, from one run's writer trace (2026-10-25).

Reads RUN_DIR/pub.trace (storm_trace.patch: r4_trace.patch with the storm hooks added, applied to main + this harness
and run with LARGE_TRACE=1 - K a Karn sample of the writer's
repair srtt - a = sample us, b = srtt us after it << 2 | source (0 the watermark passed it, 1 an ACKNACK left it out);
O one ACKNACK read for the resend order - a = repair timeout, b = RACK time, both ns >> 10; S a resend; R a held unit
released by the timeout; H a request held; N an ACKNACK received - a = its seq_no, b = units named) and the run's
tv_phases.txt, and prints per BIN ms: ACKNACKs, units named, resends, timeout releases, holds, Karn samples by source
with their median, the srtt and repair timeout at the bin's end, and the tv phase. With --summary only the totals:
Karn samples by source and how many were under 1 ms.
Usage: storm_trace_read.py RUN_DIR [--bin MS] [--summary]
"""
import os
import statistics
import sys
import time


def main():
    run = sys.argv[1]
    bin_ms = 250.0
    if "--bin" in sys.argv:
        bin_ms = float(sys.argv[sys.argv.index("--bin") + 1])
    summary = "--summary" in sys.argv
    offset = time.time() - time.monotonic()  # realtime - monotonic, the same boot
    phases = []
    try:
        for line in open(os.path.join(run, "tv_phases.txt")):
            f = line.split()
            phases.append((float(f[0]) - offset, int(f[-1])))
    except OSError:
        pass
    bins = {}
    t0 = None
    karn = {0: [], 1: []}
    srtt = timeout = 0
    for line in open(os.path.join(run, "pub.trace")):
        f = line.split()
        if len(f) != 4:
            continue
        k, t, a, b = f[0], int(f[1]) / 1e9, int(f[2]), int(f[3])
        if t0 is None:
            t0 = t
        i = int((t - t0) * 1000 / bin_ms)
        r = bins.setdefault(i, {"N": 0, "bits": 0, "S": 0, "R": 0, "H": 0, "K0": [], "K1": [], "t": t})
        if k == "N":
            r["N"] += 1
            r["bits"] += b
        elif k in "SRH":
            r[k] += 1
        elif k == "K":
            src = b & 3
            karn[src].append(a)
            r["K%d" % src].append(a)
            srtt = b >> 2
        elif k == "O":
            timeout = a
        r["srtt"] = srtt
        r["timeout"] = timeout
    for src, name in ((0, "watermark"), (1, "left out")):
        v = karn[src]
        under = sum(1 for x in v if x < 1000)
        med = statistics.median(v) if v else float("nan")
        print(f"karn {name:9} samples {len(v):6} median {med:9.0f} us  under 1 ms {under:6}")
    if summary or t0 is None:
        return
    print(f"{'t s':>6} {'phase':>5} {'acknk':>5} {'named':>7} {'resent':>7} {'tmout_rel':>9} {'held':>7} "
          f"{'K_wm':>5} {'med_us':>7} {'K_lo':>5} {'med_us':>7} {'srtt_us':>8} {'tmo_us':>7}")
    for i in sorted(bins):
        r = bins[i]
        ph = "-"
        for pt, p in phases:
            if pt <= r["t"]:
                ph = str(p)
        m0 = statistics.median(r["K0"]) if r["K0"] else 0
        m1 = statistics.median(r["K1"]) if r["K1"] else 0
        print(f"{i * bin_ms / 1000:6.2f} {ph:>5} {r['N']:5} {r['bits']:7} {r['S']:7} {r['R']:9} {r['H']:7} "
              f"{len(r['K0']):5} {m0:7.0f} {len(r['K1']):5} {m1:7.0f} {r['srtt']:8} {r['timeout']:7}")


if __name__ == "__main__":
    main()
