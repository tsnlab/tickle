#!/usr/bin/env python3
"""rmw_wake_campaign.sh's split.txt as one table: per arm, QoS and gap, each segment's per-run medians across the
repetitions - the median of them and their range [min, max] - and the round trips counted (2026-10-20).

The segment names are rmw_wake_split.py's. A difference between two arms is read only when their ranges do not
overlap; overlapping ranges are "not resolved", whatever the medians say.

Usage: rmw_wake_table.py <split.txt>
"""

import collections
import re
import statistics
import sys

COLS = ["rtt", "fwd", "turn", "back", "kernel_fwd", "pong_wait_to_recv", "pong_rx_user", "kernel_back", "ping_rx_user"]


def main(path):
    cells = collections.defaultdict(lambda: collections.defaultdict(list))
    counts = collections.defaultdict(lambda: [0, 0])
    with open(path) as f:
        for line in f:
            if not line.startswith("TSV"):
                continue
            fields = line.rstrip("\n").split("\t")
            name = fields[1].rsplit("/", 1)[-1]
            m = re.match(r"(.+)_r\d+$", name)
            cell = m.group(1) if m else name
            kv = dict(f.split("=", 1) for f in fields[2:])
            for col in COLS:
                if kv.get(col, "na") != "na":
                    cells[cell][col].append(float(kv[col]))
            counts[cell][0] += int(kv["n"])
            counts[cell][1] += int(kv["nsys"])
    print("cell\tround_trips\t" + "\t".join(COLS))
    for cell in sorted(cells):
        out = []
        for col in COLS:
            v = cells[cell][col]
            out.append(f"{statistics.median(v):.2f} [{min(v):.2f}-{max(v):.2f}]" if v else "-")
        print(f"{cell}\t{counts[cell][0]}/{counts[cell][1]}\t" + "\t".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
