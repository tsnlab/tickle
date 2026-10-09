#!/usr/bin/env python3
"""rmw_rss_breakdown_summary.py OUT_DIR [--top N]: the per-mapping split of rmw_rss_breakdown.sh's peak snapshots.

Reading rules are in rmw_rss_breakdown.sh's header; the VOID rule (no snapshot, or snapshot peak more than 5% below the
role's VmHWM) is applied here.
"""
import collections
import os
import re
import sys

HDR = re.compile(r"^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \d+\s*(.*)$")


def classify(path):
    if path == "[heap]":
        return "heap"
    if path == "[stack]":
        return "stack"
    if path == "":
        return "anon"
    if path.startswith("[") and path.endswith("]"):
        return "kernel"
    if path.startswith("/dev/shm") or path.startswith("/memfd:") or path.startswith("memfd:"):
        return "shm"
    base = os.path.basename(path.replace(" (deleted)", ""))
    if ".so" in base:
        return "lib:" + base.split(".so")[0]
    return "file"


def parse(fn):
    """[(class, path, size_kB, rss_kB, anon_kB)] and the watcher's VmRSS."""
    maps, cur, vmrss = [], None, 0
    with open(fn) as f:
        for line in f:
            if line.startswith("# VmRSS"):
                vmrss = int(line.split()[2])
                continue
            m = HDR.match(line)
            if m:
                cur = {"path": m.group(4).strip(), "Size": 0, "Rss": 0, "Anonymous": 0, "perm": m.group(3)}
                maps.append(cur)
                continue
            k, _, v = line.partition(":")
            if cur is not None and k in ("Size", "Rss", "Anonymous"):
                cur[k] = int(v.split()[0])
    return [(classify(x["path"]), x["path"], x["Size"], x["Rss"], x["Anonymous"], x["perm"]) for x in maps], vmrss


def hwm_of(cell, role):
    """The watcher's last VmHWM read for the role's own PID (not the cell sampler's, which can hold a launcher's)."""
    try:
        with open(os.path.join(cell, role + ".hwm")) as f:
            return int(f.read().split()[1])
    except (OSError, ValueError, IndexError):
        return 0


def main():
    out = sys.argv[1]
    top = 12
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
    # (cell key, label) -> role -> [per-rep dict class->(rss, anon)], and the biggest anon mappings of rep 1
    agg = collections.defaultdict(lambda: collections.defaultdict(list))
    big = {}
    for d in sorted(os.listdir(out)):
        cell = os.path.join(out, d)
        if not os.path.isdir(cell):
            continue
        m = re.match(r"^(.*)_([^_]+)_r(\d+)$", d)
        if not m:
            continue
        key, label, rep = m.group(1), m.group(2), int(m.group(3))
        roles = ("pub", "sub") if key.startswith("tput") else ("ping", "pong")
        for role in roles:
            fn = os.path.join(cell, role + ".smaps")
            if not os.path.exists(fn):
                agg[(key, label)][role].append(None)
                print(f"VOID {d} {role}: no snapshot")
                continue
            maps, vmrss = parse(fn)
            hwm = hwm_of(cell, role)
            if hwm and vmrss < 0.95 * hwm:
                print(f"VOID {d} {role}: snapshot {vmrss} kB < 95% of VmHWM {hwm} kB")
                agg[(key, label)][role].append(None)
                continue
            cls = collections.defaultdict(lambda: [0, 0])
            for c, _p, _s, r, a, _perm in maps:
                cls[c][0] += r
                cls[c][1] += a
            cls["TOTAL"] = [sum(x[3] for x in maps), sum(x[4] for x in maps)]
            cls["VmHWM"] = [hwm, 0]
            agg[(key, label)][role].append(dict(cls))
            if rep == 1:
                big[(key, label, role)] = sorted(
                    [x for x in maps if x[0] in ("anon", "shm", "heap") and x[3] > 0], key=lambda x: -x[3])[:top]
    keys = sorted({k for k, _ in agg})
    for key in keys:
        labels = sorted({lab for k, lab in agg if k == key})
        roles = ("pub", "sub") if key.startswith("tput") else ("ping", "pong")
        for role in roles:
            print(f"\n=== {key} {role}: Rss kB per class, mean over reps (min-max) [Anonymous kB] ===")
            classes = set()
            for lab in labels:
                for rep in agg[(key, lab)][role]:
                    if rep:
                        classes |= set(rep)
            order = ["VmHWM", "TOTAL", "heap", "anon", "shm", "stack", "file", "kernel"]
            libs = sorted(c for c in classes if c.startswith("lib:"))
            print(f"{'class':34s} " + " ".join(f"{lab:>26s}" for lab in labels))
            for c in order + libs:
                if c not in classes:
                    continue
                cells = []
                for lab in labels:
                    v = [rep.get(c, [0, 0]) for rep in agg[(key, lab)][role] if rep]
                    if not v:
                        cells.append(f"{'VOID':>26s}")
                        continue
                    rs = [x[0] for x in v]
                    an = sum(x[1] for x in v) / len(v)
                    cells.append(f"{sum(rs) / len(rs):8.0f} ({min(rs)}-{max(rs)}) [{an:6.0f}]".rjust(26))
                # libs below 100 kB in every arm are folded away
                if c.startswith("lib:"):
                    mx = max((rep.get(c, [0, 0])[0] for lab in labels for rep in agg[(key, lab)][role] if rep),
                             default=0)
                    if mx < 100:
                        continue
                print(f"{c:34s} " + " ".join(cells))
            for lab in labels:
                b = big.get((key, lab, role))
                if b:
                    print(f"  biggest anon/heap/shm mappings, {lab} r1: " +
                          "; ".join(f"{c}{'' if not p else ' ' + os.path.basename(p)} {perm} size {s} rss {r}"
                                    for c, p, s, r, _a, perm in b))


if __name__ == "__main__":
    main()
