#!/usr/bin/env python3
"""rx_uring_prof_diff.py - two profile reports side by side: instructions per unit by symbol, B - A. The unit is a
round trip (rx_uring_prof_pc.sh: cli.log's sent=) or a MB delivered (rx_uring_prof_large_pc.sh: units.txt).

Usage: rx_uring_prof_diff.py <A dir> <B dir> [top]
Each dir holds report.txt (perf report --stdio --sort sym) and cli.log or units.txt.
"""
import os
import re
import sys


def load(path):
    sent = 0
    if os.path.exists(f"{path}/units.txt"):  # rx_uring_prof_large_pc.sh: MB delivered
        with open(f"{path}/units.txt") as f:
            sent = float(f.read().strip())
    else:  # rx_uring_prof_pc.sh: round trips the client sent
        with open(f"{path}/cli.log") as f:
            for line in f:
                match = re.search(r" sent=(\d+)", line)
                if line.startswith("RESULT") and match:
                    sent = int(match.group(1))
    events, current, total = {}, None, 0.0
    with open(f"{path}/report.txt") as f:
        for line in f:
            match = re.match(r"# Samples: .* of event '([^']+)'", line)
            if match:
                current = match.group(1)
                events[current] = {"_total": 0.0}
                continue
            match = re.match(r"# Event count \(approx\.\): (\d+)", line)
            if match and current:
                total = float(match.group(1))
                events[current]["_total"] = total / sent
                continue
            match = re.match(r"\s+([\d.]+)%\s+\[(.)\]\s+(\S+)", line)
            if match and current:
                events[current][match.group(3)] = float(match.group(1)) / 100.0 * total / sent
    return sent, events


def main():
    a_sent, a = load(sys.argv[1])
    b_sent, b = load(sys.argv[2])
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
    print(f"sent A {a_sent} B {b_sent}")
    for event in sorted(set(a) & set(b)):
        ea, eb = a[event], b[event]
        print(f"=== {event}: per unit A {ea['_total']:.0f} B {eb['_total']:.0f} d {eb['_total'] - ea['_total']:+.0f}")
        names = (set(ea) | set(eb)) - {"_total"}
        rows = sorted(names, key=lambda n: -abs(eb.get(n, 0) - ea.get(n, 0)))[:top]
        for name in rows:
            print(f"  {eb.get(name, 0) - ea.get(name, 0):+8.0f}  A {ea.get(name, 0):7.0f}  B {eb.get(name, 0):7.0f}  {name}")


if __name__ == "__main__":
    main()
