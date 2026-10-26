#!/usr/bin/env python3
"""reader_collapse_seed.py - one traced rep's seed in detail (RESULTS `RLC`, 2026-10-26; descriptive, no verdict).

For the first timed probe (sub.trace R with path=1, busy=0; r4_trace.patch + k5_add_r_event.py): its unit, when the
probe began (R.t - took), every earlier ACKNACK naming the unit (A/a pairs) and how long before the probe each went,
and the copies of the unit the reader recorded (U) or dropped as duplicates (D/X) around it. Then the first N timer
ACKNACKs after the seed (I: interval, retry) and the next timed probes.
Usage: reader_collapse_seed.py REP_DIR [N]
"""
import os
import sys


def main():
    d = sys.argv[1]
    n_show = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    ev = []
    with open(os.path.join(d, "sub.trace"), errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) == 4:
                ev.append((p[0], int(p[1]), int(p[2]), int(p[3])))
    t0 = ev[0][1]
    names = []
    pend = None
    seed_i = None
    for i, (k, t, a, b) in enumerate(ev):
        if k == "A":
            pend = (t, a, b)
        elif k == "a" and pend is not None:
            if pend[2] > 0:
                names.append((pend[0], pend[1], a + (b >> 8), chr(b & 0xFF)))
            pend = None
        elif k == "R" and (b >> 8) & 1 and not (b >> 10) & 1:
            seed_i = i
            break
    if seed_i is None:
        print("no timed probe")
        return
    k, t, took, flags = ev[seed_i]
    seq = next((a for k2, _, a, _ in ev[seed_i + 1:seed_i + 6] if k2 in "UX"), None)
    start = t - took * 1000
    print(f"seed: ended {(t - t0) / 1e6:.3f} ms, took {took} us, retry {flags & 0xFF}, unit {seq}")
    for tn, lo, hi, why in names:
        if lo <= seq <= hi:
            print(f"  named by ACKNACK at {(tn - t0) / 1e6:.3f} ms ({(start - tn) / 1e3:.0f} us before the probe began), "
                  f"range {lo}-{hi}, reason {why}")
    for k2, t2, a2, b2 in ev:
        if k2 in "UDX" and a2 == seq:
            print(f"  {k2} unit {seq} at {(t2 - t0) / 1e6:.3f} ms")
    shown = 0
    for k2, t2, a2, b2 in ev[seed_i + 1:]:
        if k2 == "I" and shown < n_show:
            print(f"  timer ACKNACK at {(t2 - t0) / 1e6:.3f} ms interval {a2} us retry {b2}")
            shown += 1
        if k2 == "R" and (b2 >> 8) & 1 and not (b2 >> 10) & 1:
            print(f"  next timed probe at {(t2 - t0) / 1e6:.3f} ms took {a2} us retry {b2 & 0xFF}")
            break


main()
