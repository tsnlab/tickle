#!/usr/bin/env python3
"""reader_collapse_prefix.py - pre-fix (before 8d0d2eed) traced tv reps: did the reader's interval collapse follow
the writer's repair-timeout collapse? (RESULTS `RLC`, 2026-10-26; post hoc, over traces taken for `TVS`, no verdict.)

Per rep (pub.trace / sub.trace from trace_patch_T*.diff, one CLOCK_MONOTONIC for both namespaces): the writer's first
O event (resend order read: timeout, rack) with 0 < timeout < 1 ms (O carries ns >> 10: < 977; 0: no estimate yet), and the reader's first I event (timer ACKNACK) with
interval < 1 ms; the reader's timer ACKNACKs. Prints a 2 x 2 count over the reps: reader collapse (timer > 500 and
>= 50% of its timer ACKNACKs under 1 ms) by writer timeout collapse, with which came first.
Usage: reader_collapse_prefix.py RUN_DIR...
"""
import glob
import os
import sys


def first(path, kind, pred):
    n = 0
    hit = None
    t0 = None
    with open(path, errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) != 4:
                continue
            if t0 is None:
                t0 = int(p[1])
            if p[0] != kind:
                continue
            n += 1
            if hit is None and pred(int(p[2]), int(p[3])):
                hit = int(p[1])
    return hit, n, t0


def main():
    table = {}
    for d in sys.argv[1:]:
        for r in sorted(glob.glob(os.path.join(d, "runs", "*"))):
            pub, sub = os.path.join(r, "pub.trace"), os.path.join(r, "sub.trace")
            if not (os.path.exists(pub) and os.path.exists(sub)):
                continue
            w, _, _ = first(pub, "O", lambda a, b: 0 < a < 977)
            short = 0
            tot = 0
            rfirst = None
            with open(sub, errors="replace") as f:
                for line in f:
                    p = line.split()
                    if len(p) == 4 and p[0] == "I":
                        tot += 1
                        if int(p[2]) < 1000:
                            short += 1
                            if rfirst is None:
                                rfirst = int(p[1])
            rc = tot > 500 and short >= 0.5 * tot
            key = (rc, w is not None)
            table[key] = table.get(key, 0) + 1
            if rc or w is not None:
                order = "-"
                if rc and w is not None:
                    order = "writer first" if w < rfirst else "reader first"
                print(f"{os.path.basename(d)} {os.path.basename(r)}: reader timer {tot} short {short} collapse {int(rc)}; "
                      f"writer timeout < 1 ms {'yes' if w is not None else 'no'}; {order}"
                      + (f" (writer {w / 1e9:.3f}, reader {rfirst / 1e9:.3f})" if rc and w is not None else ""))
    print("2x2 (reader collapse, writer timeout < 1 ms): " + ", ".join(f"{k}: {v}" for k, v in sorted(table.items())))


main()
