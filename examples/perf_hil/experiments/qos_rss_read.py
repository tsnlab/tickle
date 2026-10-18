#!/usr/bin/env python3
"""Reads qos_rss_pc.sh's output: peak RSS per cell, role and arm, and the verdict its header pre-registers.

Lines read, tag = 'rep=R cell=C arm=A role=<client|server>': '<tag> RUSAGE ... exit=E maxrss_kb=K' (rusage_run.py,
wait4()'s ru_maxrss) and '<tag> RESULT: ... lost=L ...'.
Rules (the harness header says why):
  SAMPLES   a process counts only with exit=0 and maxrss_kb > 0, and a throughput server only if its RESULT says lost=0;
            fewer than MIN_REPS per arm in a cell/role -> that row VOID.
  TREATMENT pre - main of the reliable servers (the Subscriber's reorder slots) >= 300 KB at p1 and >= 4,000 KB at
            p4; otherwise the instrument cannot see what fix A is for and every verdict is VOID.
  CONTROL   floor = max(64, 2 x |median(fix) - median(pre)|) over the control cell's roles.
  VERDICT   delta = median(fix) - median(main): SAME within the floor, LOWER below it, HIGHER above it.
Exit status: 0 when every row is SAME or LOWER and the reading is not VOID; 1 otherwise."""
import re
import statistics
import sys

MIN_REPS = 3
CONTROL = "best_effort_throughput_p1"
TREATMENT = {"reliable_throughput_p1": 300, "reliable_latency_p1": 300,
             "reliable_throughput_p4": 4000, "reliable_latency_p4": 4000}
FLOOR_MIN_KB = 64

TAG = re.compile(r"rep=(\d+) cell=(\S+) arm=(\w+) role=(\w+) (RUSAGE|RESULT)")
MAXRSS = re.compile(r"\bexit=(-?\d+) maxrss_kb=(\d+)")
LOST = re.compile(r"\blost=(\d+)")


def read(path):
    """{(cell, role, arm): [maxrss_kb per counted rep]}, and the reasons reps were not counted."""
    maxrss, lost = {}, {}
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            tag = TAG.search(line)
            if not tag:
                continue
            rep, cell, arm, role, kind = tag.groups()
            key = (rep, cell, arm, role)
            if kind == "RUSAGE":
                found = MAXRSS.search(line)
                maxrss[key] = (int(found.group(1)), int(found.group(2))) if found else (None, 0)
            else:
                found = LOST.search(line)
                if found:
                    lost[key] = int(found.group(1))
    rss, dropped = {}, []
    for key, (code, kb) in sorted(maxrss.items()):
        rep, cell, arm, role = key
        if code != 0 or kb <= 0:
            dropped.append(f"{key}: exit={code} maxrss_kb={kb}")
        elif role == "server" and "throughput" in cell and lost.get(key) != 0:
            dropped.append(f"{key}: lost={lost.get(key)}")
        else:
            rss.setdefault((cell, role, arm), []).append(kb)
    return rss, dropped


def median_of(rss, cell, role, arm):
    values = rss.get((cell, role, arm), [])
    return statistics.median(values) if len(values) >= MIN_REPS else None


def main():
    rss, dropped = read(sys.argv[1])
    for reason in dropped:
        print("NOT COUNTED", reason)
    cells = sorted({key[0] for key in rss})
    if not cells:
        print("VERDICT VOID: no counted RUSAGE line")
        return 1

    void_reasons = []
    for cell, need in TREATMENT.items():
        pre = median_of(rss, cell, "server", "pre")
        base = median_of(rss, cell, "server", "main")
        if pre is None or base is None:
            void_reasons.append(f"TREATMENT {cell} server: fewer than {MIN_REPS} reps")
        elif pre - base < need:
            void_reasons.append(f"TREATMENT {cell} server: pre - main = {pre - base:.0f} KB < {need} KB")
        else:
            print(f"TREATMENT {cell} server: pre - main = {pre - base:+.0f} KB (>= {need}): seen")

    noise = []
    for role in ("client", "server"):
        pre = median_of(rss, CONTROL, role, "pre")
        fix = median_of(rss, CONTROL, role, "fix")
        if pre is None or fix is None:
            void_reasons.append(f"CONTROL {CONTROL} {role}: fewer than {MIN_REPS} reps")
        else:
            noise.append(abs(fix - pre))
    floor = max([FLOOR_MIN_KB] + [2 * n for n in noise])
    print(f"CONTROL {CONTROL}: |fix - pre| = {noise} KB, floor {floor:.0f} KB")

    failed = False
    void_rows = 0
    print(f"{'cell':28} {'role':6} {'main':>8} {'pre':>8} {'fix':>8} {'fix-main':>9} {'n':>7}  verdict")
    for cell in cells:
        for role in ("client", "server"):
            meds = {arm: median_of(rss, cell, role, arm) for arm in ("main", "pre", "fix")}
            counts = "/".join(str(len(rss.get((cell, role, arm), []))) for arm in ("main", "pre", "fix"))
            if meds["main"] is None or meds["fix"] is None:
                verdict, delta = "VOID (samples)", None
                void_rows += 1
            else:
                delta = meds["fix"] - meds["main"]
                verdict = "SAME" if abs(delta) <= floor else ("LOWER" if delta < 0 else "HIGHER")
                failed = failed or verdict == "HIGHER"
            show = {arm: "-" if value is None else f"{value:.0f}" for arm, value in meds.items()}
            print(f"{cell:28} {role:6} {show['main']:>8} {show['pre']:>8} {show['fix']:>8} "
                  f"{'-' if delta is None else f'{delta:+.0f}':>9} {counts:>7}  {verdict}")
    if void_reasons:
        for reason in void_reasons:
            print("VOID because", reason)
        print("VERDICT VOID")
        return 1
    if failed:
        print("VERDICT HIGHER somewhere: explain each byte")
    elif void_rows:
        print(f"VERDICT VOID: {void_rows} row(s) without {MIN_REPS} counted reps per arm")
    else:
        print("VERDICT no increase beyond noise")
    return 1 if failed or void_rows else 0


if __name__ == "__main__":
    sys.exit(main())
