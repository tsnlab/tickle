#!/usr/bin/env python3
"""The hold-over's own path, A/B from rmw_tickle's trace stamps (2026-10-20): per run, rmw_trace_split.py's chain on the
pong's and the ping's dumps (a -DRMW_TICKLE_TRACE=ON build), then per arm and QoS the mean of the runs' medians.

What it reads, fixed before the run: the segments the change touches - signaled -> release (core's poll return,
drain, the readiness check), release -> timer_set (the release's timerfd_settime(); with the hold-over two stamps
with nothing between them), and rx_wake -> tx_start (the responder's whole user path, wake to the reply's send).
Each run's median is over its ~2,000 messages; the arms are compared by the mean of those medians, with the run-to-
run spread (min-max) beside it. The round trip itself is rmw_wake_ab.py's business; this shows where the change
acts and by how much, which the PC's round trips are too noisy to show (rmw_wake_ab.py on 6-8 repetitions: A/A 2 SE
of 6-10 us).

Usage: rmw_wake_trace_ab.py <campaign dir>
"""

import glob
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rmw_trace_split as rts  # noqa: E402  - the chain rmw_trace_split.py already builds

SEGMENTS = [(4, 14), (14, 15), (15, 5), (1, 11)]


def run_medians(path):
    stamps = rts.read_stamps(path)
    delivers = [i for i, stamp in enumerate(stamps) if stamp[2] == 3]
    chains = []
    for k, index in enumerate(delivers):
        end = delivers[k + 1] if k + 1 < len(delivers) else len(stamps)
        chain = rts.chain_at(stamps, index, end)
        if all(point in chain for point in rts.CHAIN) and chain[8] - chain[1] < rts.LONGEST_NS:
            chains.append(chain)
    if not chains:
        return None, 0
    return {seg: statistics.median((c[seg[1]] - c[seg[0]]) / 1000 for c in chains) for seg in SEGMENTS}, len(chains)


def main(directory):
    table = {}
    for trace in sorted(glob.glob(os.path.join(directory, "*.trace"))):
        m = re.match(r"(.+?)_(best_effort|reliable)_g([0-9.]+)_r(\d+)\.(ping|pong)\.trace$", os.path.basename(trace))
        if m is None:
            continue
        tag, qos, _gap, _rep, role = m.groups()
        medians, n = run_medians(trace)
        if medians is None:
            print(f"{os.path.basename(trace)}: no complete chain")
            continue
        entry = table.setdefault((role, qos), {}).setdefault(tag, {"n": 0, "runs": []})
        entry["n"] += n
        entry["runs"].append(medians)
    for (role, qos), arms in sorted(table.items()):
        print(f"== {role} {qos}")
        for tag, entry in sorted(arms.items()):
            cells = []
            for seg in SEGMENTS:
                vals = [r[seg] for r in entry["runs"]]
                cells.append(f"{rts.NAMES[seg[0]]}->{rts.NAMES[seg[1]]} {statistics.mean(vals):.2f} "
                             f"[{min(vals):.2f}-{max(vals):.2f}]")
            print(f"  {tag:<4} runs={len(entry['runs'])} messages={entry['n']}  " + "  ".join(cells))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
