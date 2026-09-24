#!/usr/bin/env python3
"""Attribute every 'Data consistency violated' abort in a ctest log to its rmw implementation.

Written because both sessions investigating this abort had been grepping the logs for '_tickle_',
which cannot find an abort under rmw_fastrtps_cpp even when the same run produced four of them.
The filter encoded the conclusion it was being used to test.

Attribution is by line range against ctest's own 'N/M Testing: <name>' markers rather than by
proximity, so an abort is credited to the test that was actually running when it was printed.

The two message variants are reported separately and never summed: 'not strictly higher id' and
'not strictly older timestamp' are different assertions in perf_test and have behaved differently
across implementations.
"""
import re
import sys
from collections import defaultdict

TESTING = re.compile(r"\s*\d+/\d+ Testing:\s+(\S+)")


def vendor_of(test_name):
    for vendor in ("tickle", "fastrtps", "cyclonedds"):
        if vendor in test_name:
            return vendor
    return "?"


def main(paths):
    ran = defaultdict(set)
    aborts = defaultdict(lambda: defaultdict(int))
    cells = defaultdict(lambda: defaultdict(int))

    for path in paths:
        current = None
        try:
            lines = open(path, errors="replace").read().split("\n")
        except OSError as exc:
            print(f"  cannot read {path}: {exc}", file=sys.stderr)
            continue
        for line in lines:
            match = TESTING.match(line)
            if match:
                current = match.group(1)
                if "two_process_rmw_" in current:
                    ran[vendor_of(current)].add(current)
                continue
            if "Data consistency violated" in line and current and "two_process_rmw_" in current:
                kind = "timestamp" if "not strictly older timestamp" in line else "id"
                aborts[vendor_of(current)][kind] += 1
                cells[current.split("two_process_")[-1]][kind] += 1

    if not ran:
        print("VOID: no two_process_rmw_ tests found in these logs at all - this check did not "
              "read what it thinks it did", file=sys.stderr)
        return 1

    print(f"{'vendor':<12} {'cells run':>9} {'id-variant':>11} {'timestamp-variant':>18}")
    void = False
    for vendor in sorted(ran):
        counts = aborts[vendor]
        print(f"{vendor:<12} {len(ran[vendor]):>9} {counts['id']:>11} {counts['timestamp']:>18}")
    for vendor in ("tickle", "fastrtps", "cyclonedds"):
        if vendor not in ran:
            print(f"VOID: {vendor} ran no cells - zero aborts from it means nothing", file=sys.stderr)
            void = True

    if cells:
        print("\nby cell:")
        for cell in sorted(cells):
            counts = cells[cell]
            print(f"  {cell:<48} id={counts['id']:<4} timestamp={counts['timestamp']}")
    else:
        print("\nno aborts in any cell - INCONCLUSIVE, not a pass (see the script header)")

    return 1 if void else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
