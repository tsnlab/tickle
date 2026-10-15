#!/usr/bin/env python3
"""A hybrid_seed_sweep.sh run's SWEEP rows, prefixed with the seed: hybrid_seed_sweep_rows.py <seed> <run log>.

stdout is block-buffered and the log lines (stderr) are not, and both go to one file, so a log line can land inside a
row ("...SWEE[2026-10-15 20:48:18] [INFO] ...\\nP intact ..."). Log lines are cut out wherever they start before the
rows are read, or that row is lost (2026-10-15: one row in ~14 of a seed, read with a plain grep).
"""
import re
import sys

LOG_LINE = re.compile(r"\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\] \[[A-Z]+\][^\n]*\n")


def main():
    seed, path = sys.argv[1], sys.argv[2]
    text = LOG_LINE.sub("", open(path, errors="replace").read())
    for line in text.splitlines():
        if line.startswith("SWEEP "):
            print(seed, line[len("SWEEP "):])


if __name__ == "__main__":
    main()
