#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""Reads rmw_loan_bench.sh's runs.txt by the rules its header states, and prints a table and a verdict per metric.

One line per run: `rep=R qos=Q arm=A RESULT ...sub... | RESULT ...pub... | loans_in_place=N loans_copied=M`.
"""
import re
import statistics
import sys

ARMS = ("copy", "copy2", "loan", "loan_ring")
MIN_MESSAGES = 1000
# (metric, which process, field, higher is better)
METRICS = (
    ("delivered/s", "sub", "rate", True),
    ("sub CPU us/msg", "sub", "cpu_us_per_msg", False),
    ("pub CPU us/msg", "pub", "cpu_us_per_msg", False),
)


def fields(text):
    return dict(re.findall(r"(\w+)=(\S+)", text))


def void_reason(arm, sub, pub, loans):
    for role, values in (("sub", sub), ("pub", pub)):
        if values.get("ok") != "1":
            return f"{role} RESULT missing or ok=0 ({values.get('why', 'no line')})"
        if int(values.get("messages", 0)) < MIN_MESSAGES:
            return f"{role} window holds {values.get('messages')} messages"
    want_loan = "0" if arm.startswith("copy") else "1"
    if sub.get("can_loan") != want_loan or pub.get("can_loan") != want_loan:
        return f"can_loan sub={sub.get('can_loan')} pub={pub.get('can_loan')}, arm wants {want_loan}"
    lent = int(loans.get("loans_in_place", 0)) + int(loans.get("loans_copied", 0))
    if want_loan == "1" and lent == 0:
        return "a loan arm whose subscriber lent nothing"
    if arm == "loan_ring" and int(loans.get("loans_in_place", 0)) == 0:
        return "loan_ring read nothing in place"
    return None


def main():
    runs = {}
    voids = []
    for line in open(sys.argv[1]):
        head, _, rest = line.partition("RESULT")
        parts = ("RESULT" + rest).split("|")
        if len(parts) != 3:
            voids.append(f"unparsable: {line.strip()}")
            continue
        h = fields(head)
        sub, pub, loans = fields(parts[0]), fields(parts[1]), fields(parts[2])
        why = void_reason(h["arm"], sub, pub, loans)
        if why:
            voids.append(f"rep={h['rep']} qos={h['qos']} arm={h['arm']}: VOID {why}")
            continue
        runs.setdefault((h["qos"], h["arm"]), []).append((sub, pub, loans))
    for v in voids:
        print(v)
    for qos in ("reliable", "best_effort"):
        print(f"\n{qos}")
        print(f"  {'arm':<10} {'n':>2} " + " ".join(f"{m:>28}" for m, _, _, _ in METRICS) + "  in place / lent")
        values = {}
        for arm in ARMS:
            got = runs.get((qos, arm), [])
            row = []
            for metric, role, field, _ in METRICS:
                xs = [float((s if role == "sub" else p)[field]) for s, p, _ in got]
                values[(arm, metric)] = xs
                row.append(f"{statistics.median(xs):>10.3f} [{min(xs):.3f}-{max(xs):.3f}]" if xs else f"{'-':>28}")
            in_place = sum(int(l.get("loans_in_place", 0)) for _, _, l in got)
            lent = in_place + sum(int(l.get("loans_copied", 0)) for _, _, l in got)
            share = f"{in_place / lent:.3f}" if lent else "-"
            print(f"  {arm:<10} {len(got):>2} " + " ".join(f"{c:>28}" for c in row) + f"  {share}")
        for metric, _, _, higher in METRICS:
            base, control = values[("copy", metric)], values[("copy2", metric)]
            if len(base) < 2 or len(control) < 2:
                print(f"  {metric}: too few valid runs to read")
                continue
            noise = abs(statistics.median(control) - statistics.median(base))
            for arm in ("loan", "loan_ring"):
                xs = values[(arm, metric)]
                if len(xs) < 2:
                    print(f"  {metric} {arm}: too few valid runs")
                    continue
                delta = statistics.median(xs) - statistics.median(base)
                better_side = min(xs) > max(base) if higher else max(xs) < min(base)
                worse_side = max(xs) < min(base) if higher else min(xs) > max(base)
                if abs(delta) <= noise:
                    verdict = "NO DIFFERENCE (within the control's own move)"
                elif better_side:
                    verdict = "BETTER"
                elif worse_side:
                    verdict = "WORSE"
                else:
                    verdict = "NO DIFFERENCE (ranges overlap)"
                rel = delta / statistics.median(base) * 100 if statistics.median(base) else 0.0
                print(f"  {metric} {arm} vs copy: {verdict}, median {rel:+.1f}% (control moved {noise:.3f})")


if __name__ == "__main__":
    main()
