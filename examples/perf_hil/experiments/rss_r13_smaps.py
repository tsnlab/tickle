#!/usr/bin/env python3
"""rss_r13_smaps.py <smaps> [<smaps> ...] - Rss (kB) of one or more /proc/PID/smaps snapshots by mapping, grouped:
each file-backed object by its base name, [heap], [stack], anonymous (split by size: thread stacks are 8 MiB + guard,
others), /dev/shm segments. Columns are the files in the order given; the last column is last minus first."""
import collections
import os
import re
import sys

HDR = re.compile(r"^[0-9a-f]+-[0-9a-f]+ ")


def groups(path):
    out = collections.Counter()
    name = None
    for line in open(path):
        if HDR.match(line):
            parts = line.split()
            lo, hi = (int(x, 16) for x in parts[0].split("-"))
            size_kb = (hi - lo) // 1024
            target = parts[5] if len(parts) > 5 else ""
            if target.startswith("/dev/shm") or "/dev/shm/" in target:
                name = "shm:" + os.path.basename(target)
            elif target.startswith("/"):
                name = os.path.basename(target)
            elif target:
                name = target
            else:
                name = "anon(8M stack-sized)" if 8180 <= size_kb <= 8200 else "anon(other)"
        elif line.startswith("Rss:") and name is not None:
            out[name] += int(line.split()[1])
    return out


def main():
    gs = [groups(p) for p in sys.argv[1:]]
    keys = sorted(set().union(*gs), key=lambda k: -max(g[k] for g in gs))
    print(f"{'mapping':44s}" + "".join(f"{os.path.basename(os.path.dirname(p))[:12]:>13s}" for p in sys.argv[1:])
          + f"{'last-first':>11s}")
    for k in keys:
        vals = [g[k] for g in gs]
        if max(vals) < 16:
            continue
        print(f"{k[:44]:44s}" + "".join(f"{v:13d}" for v in vals) + f"{vals[-1] - vals[0]:+11d}")
    tot = [sum(g.values()) for g in gs]
    print(f"{'TOTAL':44s}" + "".join(f"{v:13d}" for v in tot) + f"{tot[-1] - tot[0]:+11d}")


if __name__ == "__main__":
    main()
