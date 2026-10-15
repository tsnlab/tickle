#!/usr/bin/env python3
"""largemsg_control_spin.py RUNS_DIR CELL [ARM_A ARM_B] - a subscriber CPU reading that perf_test's own wait cannot move.

Written 2026-10-15 (ab/largemsg-stepB-ctl) after the step B rig session went VOID on its control (Array16k BEST_EFFORT,
subscriber CPU/MB +5% on tickle@head, 5 of 5 reps). What the subscriber's CPU in a 30 Hz cell is made of:

  perf_test's AnalyzeRunner (the main thread) sleeps 1 s, then calls DataRunner::sync_reset(), which takes the
  DataRunner's SpinLock - a pause-free test_and_set loop. ROS2CallbackCommunicator::update_subscription() holds that
  same lock across executor.spin_once(100 ms), i.e. while the subscriber waits for the next sample. So every second
  the main thread busy-waits until the next sample has been delivered: about one sample period (33 ms at 30 Hz) of
  CPU per second, against ~1.4 ms/s (Pi) of everything the rmw and the callback do. When the main thread loses the
  race for the lock at that release - the data thread re-takes it a few instructions later - it spins one more
  period: that second's T_loop is 1 s + 2 periods instead of 1 s + 1 period, and its CPU has doubled.

  On the rig the control's +5% was exactly those seconds: tickle@head lost the race in 18 seconds over 5 reps,
  tickle@pre in 1; without them the two arms' CPU per sample differ by +0.2%. On the PC (stepb_ctl_pc.sh) the main
  thread is ~87% of the subscriber's CPU and carries all of its run-to-run spread, while the subscriber's other threads,
  its kernel instructions, its syscalls and the user instructions outside sync_reset() are the same in both arms.

THE READING (fixed here, before any re-run of the rig; applied after the fact to the 2026-10-15 session only as a
diagnostic, never as its verdict):
  steady seconds  rows with received > 0, the first 2 and the last 1 dropped (largemsg_rig_summary.py's window)
  lost race       a steady second whose T_loop exceeds 1 s + 1.5 periods AND whose samples came on time (latency_max
                  below half a period) - the extra time is perf_test's spin, not a delivery gap. A long second with a
                  late sample is a delivery gap and is KEPT: that one is the rmw's.
  metric          sub CPU (ru_utime + ru_stime) per received sample over the steady seconds that are not lost races,
                  in ms; and the lost-race seconds per arm, printed beside it, never deciding
  verdict         paired by rep, ARM_B - ARM_A: HELD when |mean| <= 2 x SE of the paired differences, MOVED otherwise,
                  UNREADABLE with fewer than 3 pairs or a run with fewer than 20 kept seconds
Defaults: ARM_A tickle@pre, ARM_B tickle@head. The rate is meta.txt's pub_rate. --selftest checks the reading on
synthetic rows and must print SELFTEST PASS.
"""
import math
import re
import sys
from pathlib import Path

WARM, COOL = 2, 1


def rows_of(path):
    """perf_test's per-second rows as dicts: t_loop, received, lat_max (ms), cpu (s, cumulative)."""
    out = []
    for line in open(path, errors="replace"):
        if not re.match(r"^\d+\.\d+,", line):
            continue
        c = [x.strip() for x in line.split(",")]
        try:
            out.append({"t_loop": float(c[1]), "received": float(c[2]), "lat_max": float(c[8]),
                        "cpu": float(c[19]) + float(c[20])})
        except (ValueError, IndexError):
            continue
    return out


def read_run(rows, rate):
    """(ms per sample over the kept seconds, lost-race seconds, kept seconds) or None."""
    period = 1.0 / rate
    live = [i for i, r in enumerate(rows) if r["received"] > 0]
    if len(live) <= WARM + COOL + 1:
        return None
    idx = live[WARM:-COOL] if COOL else live[WARM:]
    cpu = n = 0.0
    lost = kept = 0
    for i in idx:
        if i == 0:
            continue
        r = rows[i]
        if r["t_loop"] > 1.0 + 1.5 * period and r["lat_max"] < 1e3 * period / 2:
            lost += 1
            continue
        cpu += r["cpu"] - rows[i - 1]["cpu"]
        n += r["received"]
        kept += 1
    if n == 0:
        return None
    return 1e3 * cpu / n, lost, kept


def mean_se(xs):
    m = sum(xs) / len(xs)
    if len(xs) < 2:
        return m, float("nan")
    return m, math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1) / len(xs))


def read_cell(runs, cell, arm_a, arm_b, rate):
    res = {}
    for arm in (arm_a, arm_b):
        for p in sorted(Path(runs).glob(f"{arm}_{cell}_l0_r*_sub.log")):
            rep = int(re.search(r"_r(\d+)_sub\.log$", p.name).group(1))
            res.setdefault(arm, {})[rep] = read_run(rows_of(p), rate)
    lines = []
    for arm in (arm_a, arm_b):
        runs_ = res.get(arm, {})
        shown = " ".join(f"r{k}:{v[0]:.4f}/{v[1]}" if v else f"r{k}:VOID" for k, v in sorted(runs_.items()))
        lost = sum(v[1] for v in runs_.values() if v)
        lines.append(f"ARM {arm} {cell}: ms/sample/lost-race seconds {shown}; lost-race seconds {lost}")
    pairs = []
    for rep, a in res.get(arm_a, {}).items():
        b = res.get(arm_b, {}).get(rep)
        if a and b and a[2] >= 20 and b[2] >= 20:
            pairs.append(b[0] - a[0])
    if len(pairs) < 3:
        lines.append(f"CONTROL {cell} cpu without lost races: UNREADABLE ({len(pairs)} readable pairs)")
        return lines, "UNREADABLE"
    m, se = mean_se(pairs)
    base = mean_se([v[0] for v in res[arm_a].values() if v])[0]
    verdict = "HELD" if abs(m) <= 2 * se else "MOVED"
    lines.append(f"CONTROL {cell} cpu without lost races: {arm_b} - {arm_a} = {m:+.4f} ms/sample ({100 * m / base:+.2f}%)"
                 f" (2 x SE {2 * se:.4f}, n {len(pairs)}) -> {verdict}")
    return lines, verdict


def selftest():
    def row(t_loop, received, lat_max, cpu):
        return {"t_loop": t_loop, "received": received, "lat_max": lat_max, "cpu": cpu}

    base = [row(1.0, 0, 0, 0.0)]
    cpu = 0.0
    for i in range(40):
        lost = i == 10
        late = i == 20
        cpu += 0.068 if lost or late else 0.034
        base.append(row(1.0667 if lost or late else 1.0333, 30, 34.0 if late else 0.08, cpu))
    r = read_run(base, 30)
    ok = r is not None and r[1] == 1 and r[2] == 36  # the race excluded, the late second kept
    # the kept seconds: 35 at 34 ms and the late one at 68 ms, 36 x 30 samples
    ok = ok and abs(r[0] - 1e3 * (35 * 0.034 + 0.068) / (36 * 30)) < 1e-9
    print("SELFTEST PASS" if ok else f"SELFTEST FAIL {r}")
    return 0 if ok else 1


def main(argv):
    if argv[1:2] == ["--selftest"]:
        return selftest()
    runs, cell = argv[1], argv[2]
    arm_a, arm_b = (argv[3], argv[4]) if len(argv) > 4 else ("tickle@pre", "tickle@head")
    meta = dict(line.strip().split("=", 1) for line in open(Path(runs) / "meta.txt") if "=" in line)
    lines, verdict = read_cell(runs, cell, arm_a, arm_b, float(meta["pub_rate"]))
    print("\n".join(lines))
    return 0 if verdict == "HELD" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
