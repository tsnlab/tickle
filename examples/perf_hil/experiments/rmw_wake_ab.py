#!/usr/bin/env python3
"""rmw_wake_campaign.sh runs read as an A/B on the PC: arm B against arm A, and arm A2 against arm A (the A/A).

Written 2026-10-20, before the first A/B run, for the executor hold-over (rmw_tickle.h, holdover_thread): an rmw_wait()
that found something keeps the poll role, so the release's timerfd_settime() - 0.61 us median on the PC, between the
datagram and its callback on both the ping and the pong (rmw_trace_split.py, ~2,950 round trips) - and the next
claim's disarm are no longer made.

Per run (prefix P): the round trip's median from P.ping.stamps (send -> reply callback, CLOCK_MONOTONIC), the pong's
whole-run CPU (P.pong_sched.txt: its threads' schedstat, 3 s idle start included) and the ping's (P.ping_cpu.txt:
perf stat task-clock), and the treatment each process reports on its shutdown line (holdover=, holdovers=).

Per cell (QoS x gap), arm X against arm A: the difference of the means over the repetitions, and its SE
(sqrt(varA/nA + varX/nX)). BETTER: lower beyond 2 SE; WORSE: higher beyond 2 SE; else HELD.

The reading, fixed before the run:
- VOID, the whole session: the A/A (A2 against A) is BETTER or WORSE in any metric of any cell - the PC was too noisy
  to read 2 SE - or any run lacks its RESULT line, or an arm did not report its treatment (B: holdover=1 and holdovers
  at least 90% of its round trips on the pong; A and A2: holdover=0 or no holdover field at all - a parent build).
- PASS: round trip BETTER at the 5 ms gap in both QoS, and nothing WORSE in any cell (round trip, pong CPU, ping CPU).
- FAIL: anything WORSE. HELD: neither.
- Predicted, not a criterion: -0.5 to -1.5 us at the 5 ms gap (two releases a round trip, one each side), and both
  CPUs a little lower (two timerfd_settime() fewer per round trip per side). At the 100 ms gap the ping's role still
  goes to the poll thread between pings (a hold-over ends after the lease), so less is expected there.
- rtt_p10 (each run's 10th percentile) is printed beside them as a diagnostic only, added 2026-10-20 after the
  unpinned session read HELD with per-run medians spread 39-58 us within one arm: it enters no verdict.
- Falsified: the round trip HELD at the 5 ms gap in both QoS - the release's system call is not on what the round
  trip sees, and the change has CPU merit at most.

Usage: rmw_wake_ab.py <campaign dir> A=<tag> B=<tag> [A2=<tag>] [P=<tag> ...]   (every other arm is read against A)
"""

import glob
import os
import re
import statistics
import sys


def rtt_median_us(path):
    vals = []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) == 3 and fields[0] != "0" and fields[2] != "0":
                vals.append((int(fields[2]) - int(fields[1])) / 1000)
    if not vals:
        return None, None, 0
    vals.sort()
    return statistics.median(vals), vals[len(vals) // 10], len(vals)


def pong_cpu_ms(path):
    try:
        with open(path) as f:
            return sum(int(line.split()[0]) for line in f if line.strip()) / 1e6
    except (OSError, ValueError, IndexError):
        return None


def ping_cpu_ms(path):
    try:
        with open(path) as f:
            for line in f:
                fields = line.strip().split(",")
                if len(fields) > 2 and fields[2] == "task-clock":
                    return float(fields[0])
    except (OSError, ValueError):
        pass
    return None


def treatment(log):
    try:
        text = open(log).read()
    except OSError:
        return None, None
    m = re.search(r"holdover=(\d) holdovers=(\d+)", text)
    if m is None:
        return "absent", 0
    return m.group(1), int(m.group(2))


def read_runs(directory):
    runs = {}
    for stamps in glob.glob(os.path.join(directory, "*.ping.stamps")):
        prefix = stamps[: -len(".ping.stamps")]
        m = re.match(r"(.+?)_(best_effort|reliable)_g([0-9.]+)_r(\d+)$", os.path.basename(prefix))
        if m is None:
            continue
        tag, qos, gap, rep = m.groups()
        rtt, rtt_p10, n = rtt_median_us(stamps)
        hold, kept = treatment(prefix + ".pong.log")
        result = os.path.exists(prefix + ".ping.log") and "RESULT" in open(prefix + ".ping.log").read()
        runs.setdefault((qos, gap), {}).setdefault(tag, []).append({
            "rep": int(rep), "rtt": rtt, "rtt_p10": rtt_p10, "n": n, "pong_cpu": pong_cpu_ms(prefix + ".pong_sched.txt"),
            "ping_cpu": ping_cpu_ms(prefix + ".ping_cpu.txt"), "hold": hold, "kept": kept, "result": result})
    return runs


def compare(a, x):
    ma, mx = statistics.mean(a), statistics.mean(x)
    se = (statistics.variance(a) / len(a) + statistics.variance(x) / len(x)) ** 0.5 if len(a) > 1 and len(x) > 1 \
        else float("inf")
    diff = mx - ma
    verdict = "BETTER" if diff < -2 * se else "WORSE" if diff > 2 * se else "HELD"
    return diff, se, verdict


def main(argv):
    directory = argv[0]
    roles = dict(arg.split("=", 1) for arg in argv[1:])
    a_tag, b_tag, a2_tag = roles["A"], roles["B"], roles.get("A2")
    runs = read_runs(directory)
    void, verdicts = [], []
    for cell in sorted(runs):
        arms = runs[cell]
        print(f"== {cell[0]} gap {cell[1]} s")
        for tag, rs in sorted(arms.items()):
            n = sum(r["n"] for r in rs)
            print(f"  {tag:<6} reps={len(rs)} round_trips={n} rtt_median_us="
                  + ",".join(f"{r['rtt']:.2f}" for r in sorted(rs, key=lambda r: r["rep"]) if r["rtt"] is not None)
                  + f" holdover={sorted({r['hold'] for r in rs})}")
            for r in rs:
                if not r["result"] or r["rtt"] is None:
                    void.append(f"{tag} {cell} rep {r['rep']}: no RESULT")
                if tag == b_tag and (r["hold"] != "1" or r["kept"] < 0.9 * r["n"]):
                    void.append(f"{tag} {cell} rep {r['rep']}: treatment not realized (holdover={r['hold']} "
                                f"holdovers={r['kept']} round trips={r['n']})")
                if tag != b_tag and r["hold"] not in ("0", "absent"):
                    void.append(f"{tag} {cell} rep {r['rep']}: control ran the hold-over (holdover={r['hold']})")
        if a_tag not in arms:
            continue
        for tag in sorted(arms):
            if tag == a_tag:
                continue
            for metric in ("rtt", "pong_cpu", "ping_cpu", "rtt_p10"):
                a = [r[metric] for r in arms[a_tag] if r[metric] is not None]
                x = [r[metric] for r in arms[tag] if r[metric] is not None]
                if len(a) < 2 or len(x) < 2:
                    continue
                diff, se, verdict = compare(a, x)
                unit = "us" if metric.startswith("rtt") else "ms"
                print(f"  {tag} - {a_tag} {metric:<8} {diff:+8.2f} {unit} (2 SE {2 * se:.2f}) {verdict}")
                if tag == a2_tag and verdict != "HELD":
                    void.append(f"A/A {cell} {metric}: {verdict}")
                if tag == b_tag and metric != "rtt_p10":
                    verdicts.append((cell, metric, verdict))
    print()
    if void:
        print("VERDICT VOID: " + "; ".join(void))
        return 0
    worse = [v for v in verdicts if v[2] == "WORSE"]
    better_5ms = [v for v in verdicts if v[1] == "rtt" and v[0][1] == "0.005" and v[2] == "BETTER"]
    if worse:
        print("VERDICT FAIL: WORSE in " + ", ".join(f"{c[0]} {c[1]} {m}" for c, m, _ in worse))
    elif len(better_5ms) == 2:
        print("VERDICT PASS: round trip BETTER at 5 ms in both QoS, nothing WORSE")
    else:
        print(f"VERDICT HELD: round trip BETTER at 5 ms in {len(better_5ms)} of 2 QoS, nothing WORSE")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
