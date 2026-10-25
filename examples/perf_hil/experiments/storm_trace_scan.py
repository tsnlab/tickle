#!/usr/bin/env python3
"""storm_trace_scan.py - one line per traced run (storm_trace_read.py's trace): resends, timeout releases, Karn
samples by source and how many under 1 ms, the least srtt the repair timeout stood on and the least timeout, and the
reader's ACKNACKs (timer / immediate, from echo.log's RSTATS).
Usage: storm_trace_scan.py OUT_DIR... (each holding runs/<arm>-<cell>-<rep>/)
"""
import os
import re
import sys

for out in sys.argv[1:]:
    runs = os.path.join(out, "runs")
    for name in sorted(os.listdir(runs), key=lambda n: (n.rsplit("-", 1)[0], int(n.rsplit("-", 1)[1]))):
        d = os.path.join(runs, name)
        tr = os.path.join(d, "pub.trace")
        if not os.path.exists(tr):
            continue
        n = {"S": 0, "R": 0, "N": 0}
        k = {0: [0, 0], 1: [0, 0]}
        min_srtt = min_tmo = None
        for line in open(tr):
            f = line.split()
            if len(f) != 4:
                continue
            c = f[0]
            if c in n:
                n[c] += 1
            elif c == "K":
                a, b = int(f[2]), int(f[3])
                k[b & 3][0] += 1
                k[b & 3][1] += a < 1000
                s = b >> 2
                min_srtt = s if min_srtt is None or s < min_srtt else min_srtt
            elif c == "O":
                t = int(f[2])
                if t:
                    min_tmo = t if min_tmo is None or t < min_tmo else min_tmo
        echo = open(os.path.join(d, "echo.log"), errors="replace").read()
        m = re.search(r"acknack_tx immediate=(\d+) timer=(\d+)", echo)
        print(f"{os.path.basename(out):18} {name:16} resent {n['S']:8} timeout_rel {n['R']:8} acknacks {n['N']:6} "
              f"karn wm {k[0][0]:5} (<1ms {k[0][1]:4}) lo {k[1][0]:5} (<1ms {k[1][1]:4}) min_srtt_us {min_srtt} "
              f"min_timeout_us {min_tmo} reader imm/timer {m.group(1) if m else '?'}/{m.group(2) if m else '?'}")
