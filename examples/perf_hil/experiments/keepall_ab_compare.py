#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""A against B for RESULTS rows 72-75 (rmw KEEP_ALL at equal bounds), from rmw_keepall_rig.sh EQUAL_BOUND runs.

EQUAL_BOUND runs one rmw_tickle arm (tickle@head) beside the vendors, so an A/B is several runs, each with HEAD_SHA
at A or at B (ABBA). Here they are read together. Written before the first run (2026-10-09), as the large-message L2
pre-registration (~/rig_queue_largemsg_A3.sh) says:

  PRIMARY  per cell (topic x loss), tickle@head delivered msgs/s: the usable reps of every A run pooled against those
           of every B run; Welch t, Bonferroni over the cells (|t| > z_k, two-sided 4.55% family-wise).
  CONTROL  the vendor arms (fastdds, cyclonedds) - no TickLE code: per cell, |median(B runs) - median(A runs)| / median(A
           runs) of each vendor with usable reps in both, the largest of them. That is the rig's drift between the runs.
  VERDICT  WORSE when some cell's tickle rate is lower in B beyond z_k AND by more than the control's drift (in %);
           PASS otherwise. NO VERDICT when a cell lacks two usable tickle reps on either side, or no run is given.
  Usable rep: a per-run line of the arm that is neither REFUSED nor listed under "VOID runs:".

Usage: keepall_ab_compare.py --a A1.txt [A2.txt ...] --b B1.txt [B2.txt ...]     exit 0 PASS, 1 WORSE, 3 NO VERDICT
"""
import argparse
import math
import re
import statistics
import sys
from statistics import NormalDist

RUN = re.compile(r"^(?P<topic>\S+)\s+l(?P<loss>\d+)\s+(?P<arm>\S+)\s+r(?P<rep>\d+)\s+rate\s+(?P<rate>[\d.]+)/s(?P<rest>.*)$")
VOID = re.compile(r"^\s+(?P<arm>[^_\s]+)_(?P<topic>[^_\s]+)_l(?P<loss>\d+)_r(?P<rep>\d+):")
VENDORS = ("fastdds", "cyclonedds")
TICKLE = "tickle@head"
FAMILY_ALPHA = 0.0455


def read(paths):
    """{(topic, loss): {arm: [rate, ...]}} over every usable rep of every file."""
    cells = {}
    for path in paths:
        lines = open(path, encoding="utf-8").read().split("\n")
        # The summary's per-run table follows the LAST "=== per run" header (the dry run has its own before it).
        starts = [i for i, line in enumerate(lines) if line.startswith("=== per run")]
        if not starts:
            continue
        body = lines[starts[-1]:]
        void = set()
        for line in body:
            m = VOID.match(line)
            if m:
                void.add((m["arm"], m["topic"], m["loss"], m["rep"]))
        for line in body:
            m = RUN.match(line)
            if not m or "REFUSED" in m["rest"] or (m["arm"], m["topic"], m["loss"], m["rep"]) in void:
                continue
            cells.setdefault((m["topic"], int(m["loss"])), {}).setdefault(m["arm"], []).append(float(m["rate"]))
    return cells


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", nargs="+", required=True)
    ap.add_argument("--b", nargs="+", required=True)
    args = ap.parse_args()
    ca, cb = read(args.a), read(args.b)
    keys = sorted(set(ca) | set(cb))
    if not keys:
        print("NO VERDICT: no per-run lines in the files given")
        return 3
    z = NormalDist().inv_cdf(1 - FAMILY_ALPHA / (2 * len(keys)))
    print(f"--- {len(keys)} cell(s), Bonferroni |t| > {z:.2f}; control = the vendors' own B-vs-A drift ---")
    worse, undecided = 0, 0
    for key in keys:
        ta, tb = ca.get(key, {}).get(TICKLE, []), cb.get(key, {}).get(TICKLE, [])
        name = f"{key[0]} l{key[1]}"
        if len(ta) < 2 or len(tb) < 2:
            print(f"{name:14s} NO VERDICT: tickle@head usable reps A {len(ta)}, B {len(tb)}")
            undecided += 1
            continue
        ma, mb = statistics.fmean(ta), statistics.fmean(tb)
        se = math.sqrt(statistics.variance(ta) / len(ta) + statistics.variance(tb) / len(tb))
        t = (mb - ma) / se if se > 0 else 0.0
        pct = 100 * (mb - ma) / ma
        drift = []
        for v in VENDORS:
            va, vb = ca.get(key, {}).get(v, []), cb.get(key, {}).get(v, [])
            if va and vb:
                drift.append(100 * abs(statistics.median(vb) - statistics.median(va)) / statistics.median(va))
        control = max(drift) if drift else 0.0
        bad = t < -z and -pct > control
        worse += bad
        print(f"{name:14s} tickle A {ma:10.1f}/s (n{len(ta)})  B {mb:10.1f}/s (n{len(tb)})  {pct:+6.1f}%  t {t:+5.1f}  "
              f"control drift {control:5.1f}% ({len(drift)} vendor(s))  {'WORSE' if bad else 'held'}")
    if undecided:
        print(f"NO VERDICT: {undecided} cell(s) without two usable tickle reps on both sides")
        return 3
    if worse:
        print(f"VERDICT: WORSE ({worse} of {len(keys)} cell(s))")
        return 1
    print(f"VERDICT: PASS (no cell worse beyond |t| > {z:.2f} and the control's drift)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
