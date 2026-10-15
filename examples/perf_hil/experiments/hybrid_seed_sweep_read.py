#!/usr/bin/env python3
"""Compares two hybrid_seed_sweep.sh outputs (2026-10-15): which seeds pass, and what the readers got.

  hybrid_seed_sweep_read.py <out_dir> <label_a> <label_b>

Only test functions present in both trees and unchanged in what they do are summed (a tree's own new tests have no
counterpart; CHANGED lists those rewritten by the change under test), over the seeds both sweeps finished. Per
function: the samples wanted and delivered, gap_abandoned and gap_evicted summed over seeds, and how many seeds fell
short.
"""
import sys
from collections import defaultdict

# Rewritten by the KEEP_LAST repair change (2026-10-15): the same name, a different scenario.
CHANGED = {"test_a_reader_without_a_discovery_table_learns_keep_last"}


def load(out, label, keep=None):
    verdicts = {}
    for line in open(f"{out}/{label}.verdicts"):
        seed, rc = line.split()
        verdicts[int(seed)] = rc
    rows = defaultdict(lambda: [0, 0, 0, 0, set()])  # function -> want, delivered, abandoned, evicted, short seeds
    for line in open(f"{out}/{label}.tsv"):
        seed, _kind, func, want, delivered, abandoned, evicted = line.split()
        if keep is not None and int(seed) not in keep:
            continue
        r = rows[func]
        r[0] += int(want)
        r[1] += int(delivered)
        r[2] += int(abandoned)
        r[3] += int(evicted)
        if int(delivered) < int(want):
            r[4].add(int(seed))
    return verdicts, rows


def main():
    out, a, b = sys.argv[1:4]
    va, _ = load(out, a)
    vb, _ = load(out, b)
    seeds = sorted(set(va) & set(vb))  # both finished: a sweep still running has one more in its tsv than verdicts
    _, ra = load(out, a, set(seeds))
    _, rb = load(out, b, set(seeds))
    for label, v in ((a, va), (b, vb)):
        failed = [s for s in seeds if v[s] != "0"]
        print(f"{label}: {len(seeds)} seeds, {len(seeds) - len(failed)} pass, failed: {failed}")
    common = sorted((set(ra) & set(rb)) - CHANGED)
    totals = {a: [0, 0, 0, 0, 0], b: [0, 0, 0, 0, 0]}
    print(f"{'function':62} {'arm':6} {'want':>6} {'deliv':>6} {'aband':>6} {'evict':>6} short seeds")
    for func in common:
        for label, rows in ((a, ra), (b, rb)):
            want, delivered, abandoned, evicted, short = rows[func]
            t = totals[label]
            t[0] += want
            t[1] += delivered
            t[2] += abandoned
            t[3] += evicted
            t[4] += len(short)
            if want != delivered or abandoned or evicted or func == common[0]:
                print(f"{func[:62]:62} {label:6} {want:6} {delivered:6} {abandoned:6} {evicted:6} "
                      f"{len(short)} {sorted(short)[:12]}")
    for label in (a, b):
        t = totals[label]
        print(f"TOTAL {label}: want {t[0]} delivered {t[1]} gap_abandoned {t[2]} gap_evicted {t[3]} "
              f"(function, seed) pairs short {t[4]}")


if __name__ == "__main__":
    main()
