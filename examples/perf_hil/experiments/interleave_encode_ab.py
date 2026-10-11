#!/usr/bin/env python3
"""interleave_encode_ab.py - reads interleave_encode_pc.sh's results by the rules in its header, written before the
first run (2026-10-11). The change: a large sample is encoded a send call at a time, just ahead of the datagrams that
carry it (docs/DESIGN.md section 8, "Encoding while sending").

Arms: P (the parent), H0 (this tree, no ranged encode: the control - the change cannot touch this path), H1 (this
tree, ranged encode: the treatment).

RULES (enforced below):
  VOID       a run with torn > 0 or out_of_order > 0, or (RELIABLE) delivered < published; an H1 run with
             range_calls = 0, a P or H0 run with range_calls != 0 (the arm did not get its treatment). A cell with fewer
             than 3 valid runs in any arm: NO VERDICT for the cell.
  FLOOR      per metric: max(|mean(H0) - mean(P)|, 2 x SE(H1 - H0)), SE by Welch.
  VERDICT    d = mean(H1) - mean(H0): BETTER iff d < -floor, WORSE iff d > +floor, else HELD.
  HYPOTHESIS every 4 MB cell's latency median BETTER, with -d >= memcpy_us / 2 (half of one cold 4 MiB memcpy, the
             encode moved off the critical path, measured by the harness at the start). Otherwise FALSIFIED.
  NO HARM    latency p99, publisher CPU ms/MB and subscriber CPU ms/MB not WORSE in any cell.
Exit 0 when the reading is complete and the hypothesis holds with no harm, 1 when it is complete and does not, 2 when
any cell is VOID / NO VERDICT, 3 on missing input.
Usage: interleave_encode_ab.py RESULTS.txt   (the last session in the file is read)
"""
import math
import re
import statistics
import sys

METRICS = (
    ("latency median us", "sub", "latency_median_us"),
    ("latency p99 us", "sub", "latency_p99_us"),
    ("pub CPU ms/MB", "pub", "cpu_ms_per_mb"),
    ("sub CPU ms/MB", "sub", "cpu_ms_per_mb"),
)
MIN_RUNS = 3


def fields(text):
    return dict(re.findall(r"(\w+)=(\S+)", text))


def welch_se(a, b):
    if len(a) < 2 or len(b) < 2:
        return math.inf
    return math.sqrt(statistics.variance(a) / len(a) + statistics.variance(b) / len(b))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 3
    try:
        lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
    except OSError as err:
        print(f"cannot read: {err}")
        return 3
    starts = [i for i, line in enumerate(lines) if line.startswith("=== interleave_encode_pc ")]
    if not starts:
        print("no session in the file")
        return 3
    header = lines[starts[-1]]
    memcpy_us = float(fields(header).get("memcpy_us", "nan"))
    print(header)
    runs = {}
    voids = 0
    for line in lines[starts[-1] + 1:]:
        if not line.startswith("RUN "):
            continue
        parts = line.split(" | ")
        head = fields(parts[0])
        cell, arm = head["cell"], head["arm"]
        if len(parts) != 3:
            print(f"  VOID {cell} {arm} block {head.get('block')}: no RESULT")
            voids += 1
            continue
        pub, sub = fields(parts[1]), fields(parts[2])
        reasons = []
        if int(sub["torn"]) or int(sub["out_of_order"]):
            reasons.append(f"torn={sub['torn']} out_of_order={sub['out_of_order']}")
        if pub["reliable"] == "1" and int(sub["delivered"]) < int(pub["published"]):
            reasons.append(f"delivered {sub['delivered']} < published {pub['published']}")
        calls = int(pub.get("range_calls", "0"))
        if (arm == "H1") != (calls > 0):
            reasons.append(f"treatment: {arm} with range_calls={calls}")
        if reasons:
            print(f"  VOID {cell} {arm} block {head['block']}: {'; '.join(reasons)}")
            voids += 1
            continue
        runs.setdefault(cell, {}).setdefault(arm, []).append((pub, sub))

    complete = True
    holds = True
    for cell, arms in runs.items():
        counts = {arm: len(arms.get(arm, [])) for arm in ("P", "H0", "H1")}
        print(f"\n{cell}: valid runs {counts}")
        if min(counts.values()) < MIN_RUNS:
            print("  NO VERDICT: fewer than 3 valid runs in an arm")
            complete = False
            continue
        size = int(arms["H1"][0][0]["size"])
        for name, side, key in METRICS:
            value = {arm: [float((p if side == "pub" else s)[key]) for p, s in arms[arm]] for arm in ("P", "H0", "H1")}
            mean = {arm: statistics.mean(v) for arm, v in value.items()}
            d = mean["H1"] - mean["H0"]
            floor = max(abs(mean["H0"] - mean["P"]), 2 * welch_se(value["H1"], value["H0"]))
            verdict = "BETTER" if d < -floor else "WORSE" if d > floor else "HELD"
            print(f"  {name:20s} P {mean['P']:10.1f}  H0 {mean['H0']:10.1f}  H1 {mean['H1']:10.1f}  "
                  f"d {d:+9.1f}  floor {floor:8.1f}  {verdict}")
            if name != "latency median us" and verdict == "WORSE":
                print("    NO HARM broken")
                holds = False
            if name == "latency median us" and size >= 4194304:
                need = memcpy_us / 2
                ok = verdict == "BETTER" and -d >= need
                print(f"    HYPOTHESIS (4 MB): BETTER by >= {need:.0f} us -> {'HOLDS' if ok else 'FALSIFIED'}")
                holds = holds and ok
    if voids or not complete:
        print(f"\n{voids} void run(s); reading {'complete' if complete else 'INCOMPLETE'}")
        return 2
    print("\nreading complete:", "hypothesis holds, no harm" if holds else "hypothesis falsified or harm found")
    return 0 if holds else 1


if __name__ == "__main__":
    sys.exit(main())
