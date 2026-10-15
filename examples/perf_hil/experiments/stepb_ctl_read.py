#!/usr/bin/env python3
"""stepb_ctl_read.py SUMMARY - reads stepb_ctl_pc.sh's RUN lines.

Per arm: mean, SE and n of each subscriber/publisher counter over VALID runs. Then each arm against the FIRST arm
named in the runs (the control arm, e.g. main), paired by rep: mean of the per-rep differences, its SE, and
MOVED when |mean| > 2 x SE, else HELD. A run that is VOID (wrong library mapped) or has no counter is left out and
counted; an arm with fewer than 3 valid reps reads UNREADABLE, never HELD.
"""
import math
import sys

KEYS = ["sub_main_ms", "sub_other_ms", "sub_task-clock", "sub_instructions:u", "sub_instructions:k", "sub_cycles:u",
        "sub_cycles:k", "pub_task-clock", "pub_instructions:u", "pub_instructions:k", "sub_VmHWM", "sub_RssAnon",
        "sub_RssShmem", "sub_RssFile"]


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def mean_se(xs):
    n = len(xs)
    m = sum(xs) / n
    se = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1) / n) if n > 1 else float("nan")
    return m, se


def main(path):
    runs, order, void = {}, [], {}
    for line in open(path):
        if not line.startswith("RUN "):
            continue
        f = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
        arm, rep = f["arm"], int(f["rep"])
        if arm not in order:
            order.append(arm)
        if "VALID" not in line.split():
            void[arm] = void.get(arm, 0) + 1
            continue
        runs.setdefault(arm, {})[rep] = {k: num(f.get(k)) for k in KEYS}
    for arm in order:
        r = runs.get(arm, {})
        print(f"ARM {arm}: valid {len(r)} void {void.get(arm, 0)}")
        for k in KEYS:
            xs = [v[k] for v in r.values() if v[k] is not None]
            if len(xs) >= 2:
                m, se = mean_se(xs)
                print(f"  {k:22s} mean {m:16.1f} SE {se:12.1f} n {len(xs)}")
    if not order:
        print("NO RUNS")
        return 1
    ctl = order[0]
    for arm in order[1:]:
        for k in KEYS:
            d = [runs[arm][rep][k] - runs[ctl][rep][k] for rep in runs.get(arm, {})
                 if rep in runs.get(ctl, {}) and runs[arm][rep][k] is not None and runs[ctl][rep][k] is not None]
            if len(d) < 3:
                print(f"DELTA {arm}-{ctl} {k}: UNREADABLE (n {len(d)})")
                continue
            m, se = mean_se(d)
            base = sum(runs[ctl][rep][k] for rep in runs[ctl] if runs[ctl][rep][k] is not None) / len(runs[ctl])
            verdict = "MOVED" if abs(m) > 2 * se else "HELD"
            print(f"DELTA {arm}-{ctl} {k}: {m:+.1f} ({100 * m / base:+.2f}%) 2xSE {2 * se:.1f} n {len(d)} -> {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
