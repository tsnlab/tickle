#!/usr/bin/env python3
"""Reads w1_bench.sh's output against WIRE_PLAN.md 9.1 step 1: per case, the W1 arm minus the base arm, paired by
round, for recv and send ns per sample - mean +- SE, and better / held / WORSE at 2 x SE. Also the W1 counters
(short_unrouted must be 0 in every W1 row) and the bytes per sample of each arm.

Usage: w1_bench_read.py <dir> <base ref> <w1 ref>
"""

import re
import statistics
import sys
from pathlib import Path

CASES = ["w1", "w1D", "w8", "w8D", "w32", "w32D", "R", "cR", "w32collide"]


def rows(path):
    table = {}
    with open(path) as results:
        for line in results:
            m = re.match(r"ref=(\S+) round=(\d+) RESULT: (.*)", line)
            if m and "failed" not in m.group(3):
                kv = dict(p.split("=", 1) for p in m.group(3).split() if "=" in p)
                table.setdefault(int(m.group(2)), {})[m.group(1)] = kv
    return table


def paired(table, base, w1, key):
    d = [float(r[w1][key]) - float(r[base][key]) for r in table.values() if base in r and w1 in r]
    if len(d) < 2:
        return None
    mean = statistics.mean(d)
    se = statistics.stdev(d) / len(d) ** 0.5
    verdict = "WORSE" if mean > 2 * se else "better" if mean < -2 * se else "held"
    return mean, se, verdict, len(d)


def main(directory, base, w1):
    for case in CASES:
        path = Path(directory) / f"{case}.txt"
        if not path.exists():
            print(f"{case:>11}: no file")
            continue
        table = rows(path)
        base_recv = statistics.median(float(r[base]["recv_ns_per_sample"]) for r in table.values() if base in r)
        parts = [f"{case:>11}: base recv {base_recv:6.1f}"]
        for key, name in [("recv_ns_per_sample", "recv"), ("send_ns_per_sample", "send")]:
            p = paired(table, base, w1, key)
            parts.append(f"{name} {p[0]:+6.2f} +- {p[1]:4.2f} {p[2]:<6} (n={p[3]})" if p else f"{name} n/a")
        unrouted = {r[w1].get("short_unrouted") for r in table.values() if w1 in r}
        short = {r[w1].get("short_sent") for r in table.values() if w1 in r}
        nbytes = {ref: {r[ref].get("wire_bytes_per_sample") for r in table.values() if ref in r} for ref in (base, w1)}
        parts.append(
            f"unrouted {sorted(unrouted)} short_sent {sorted(short)[:2]} bytes {sorted(nbytes[base])}->{sorted(nbytes[w1])}"
        )
        print("  ".join(parts))
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:4]))
