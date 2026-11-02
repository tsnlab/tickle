#!/usr/bin/env python3
"""k_recheck_read.py OUT TAG TREES_DIR - the pre-registered reading of k_recheck_netns.sh (its header holds the rules
in prose; this code applies them and decides). Written 2026-11-02 before the first run."""
import math
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import wifi_burst_read as wbr  # noqa: E402

LABELS = ["M", "M2", "K"]
LOSSY = ["ge1b2", "ge5b2", "ge5b4", "ge10b4"]
MIN_REPS = 15
BASE_RTT_MS = 6.88  # ctl rel_small p50 RTT of the 200eefc6 WIFI baseline (wifi_merged/summary.txt), for R4 only
CTL_P50_FLOOR, CTL_P99_FLOOR = 1.0, 5.0


def num(d, k):
    try:
        return float(d.get(k, "nan"))
    except (TypeError, ValueError):
        return float("nan")


def timer_acknacks(run_dir):
    try:
        for line in open(os.path.join(run_dir, "echo.log"), encoding="utf-8", errors="replace"):
            if line.startswith("RSTATS: role=sub acknack_tx"):
                m = re.search(r"\btimer=(\d+)", line)
                if m:
                    return int(m.group(1))
    except OSError:
        pass
    return -1


def mean_se(v):
    if len(v) < 2:
        return float("nan"), float("nan")
    return statistics.mean(v), statistics.stdev(v) / math.sqrt(len(v))


def diff(a, b):
    """K-side list a against base list b: (d, se)."""
    ma, sa = mean_se(a)
    mb, sb = mean_se(b)
    return ma - mb, math.sqrt(sa**2 + sb**2)


def large_exception(tree):
    """True when the tree's repair_timing_times() still holds the large-writer exception K removes."""
    try:
        src = open(os.path.join(tree, "src", "tickle.c"), encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    m = re.search(r"^static bool repair_timing_times\([^;{]*\)\s*\{(.*?)^\}", src, re.S | re.M)
    if not m:
        return None
    return "large_arrival_ns" in m.group(1)


def main():
    out, tag, trees_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    void = []
    data = {}  # (label, arm) -> dict of lists
    deliv = {}
    cell_void = {}
    sha = {}
    for lab in LABELS:
        o = os.path.join(out, f"{tag}_{lab}")
        if not os.path.isdir(os.path.join(o, "runs")):
            void.append(f"tree {lab}: no runs under {o}")
            continue
        prov = wbr.kv(" ".join(wbr.read_lines(os.path.join(o, "provenance.txt"))))
        sha[lab] = prov.get("tree", "?")
        runs = wbr.load_runs(o, "wifi")
        cells = wbr.pool(runs, int(prov.get("dur", "30")))
        cv, campaign = wbr.validity(cells)
        for msg in campaign:
            void.append(f"tree {lab} campaign VOID: {msg}")
        for (arm, cell), msgs in cv.items():
            if cell == "rel_large" and msgs:
                cell_void.setdefault(arm, []).append(f"{lab}: {'; '.join(msgs)}")
            c = cells.get((arm, cell))
            if c is not None and c.get("offered"):
                deliv[(lab, arm)] = 100.0 * c["goodput"]
        for r in runs:
            if r["cell"] != "rel_large":
                continue
            d = data.setdefault((lab, r["arm"]), {"p50": [], "p99": [], "retx": [], "timer": [], "reps": 0,
                                                    "excluded": []})
            d["reps"] += 1
            if r["void"] or r["loaded"] or r["echo"] is None or r["ping"] is None:
                why = "; ".join(r["void"]) or ("PC load > nproc/2 at start" if r["loaded"] else "no RESULT")
                d["excluded"].append(f"rep {r['rep']}: {why}")
                continue
            p50, p99 = num(r["echo"], "latency_median_us") / 1000, num(r["echo"], "latency_p99_us") / 1000
            if math.isnan(p50) or math.isnan(p99):
                d["excluded"].append(f"rep {r['rep']}: no latency in RESULT")
                continue
            d["p50"].append(p50)
            d["p99"].append(p99)
            d["retx"].append(int(num(r["ping"], "retransmitted")))
            d["timer"].append(timer_acknacks(r["dir"]))

    # Treatment: K's tree lacks the exception, both main trees hold it; and each tree ran at its own HEAD.
    for lab in LABELS:
        tree = os.path.join(trees_dir, lab)
        exc = large_exception(tree)
        want = lab != "K"
        head = subprocess.run(["git", "-C", tree, "rev-parse", "--short", "HEAD"], capture_output=True,
                              text=True).stdout.strip()
        print(f"treatment {lab}: tree {tree} HEAD {head}, harness tree={sha.get(lab, '?')}, "
              f"large-writer exception in repair_timing_times: {exc}")
        if exc is None:
            void.append(f"treatment: cannot read repair_timing_times() in {lab}'s tree")
        elif exc != want:
            void.append(f"treatment: {lab}'s tree {'holds' if exc else 'lacks'} the large-writer exception")
        if not head or not sha.get(lab, "?").startswith(head[:7]):
            void.append(f"treatment: {lab}'s harness ran tree {sha.get(lab)} but the tree is at {head}")

    # The control.
    for lab in LABELS:
        d = data.get((lab, "ctl"))
        if d is None or len(d["p50"]) < MIN_REPS:
            void.append(f"control: ctl has {0 if d is None else len(d['p50'])} scored reps in {lab} (< {MIN_REPS})")
        elif any(x != 0 for x in d["retx"]):
            void.append(f"control: ctl writer resends in {lab}: {d['retx']}")
    if "ctl" in cell_void:
        void.append("control: harness VOID on ctl: " + " | ".join(cell_void["ctl"]))
    if all((lab, "ctl") in data for lab in LABELS):
        for lab in ("M2", "K"):
            for met, floor in (("p50", CTL_P50_FLOOR), ("p99", CTL_P99_FLOOR)):
                dd, se = diff(data[(lab, "ctl")][met], data[("M", "ctl")][met])
                lim = max(floor, 3 * se) if se == se else floor
                moved = not abs(dd) <= lim
                print(f"control ctl {met}: {lab} - M = {dd:+.2f} ms (limit +-{lim:.2f}){'  MOVED' if moved else ''}")
                if moved:
                    void.append(f"control moved: ctl {met} {lab} - M = {dd:+.2f} ms beyond {lim:.2f}")

    # Per-arm table.
    print("\nper tree and arm (scored reps; means of per-rep values; resends per rep; timer ACKNACKs median):")
    print(f"{'arm':7} {'tree':4} {'n':>3} {'p50 ms':>14} {'p99 ms':>16} {'resends':>14} {'2x reps':>7} "
          f"{'timer':>6} {'deliv%':>7}")
    judged = []
    for arm in ["ctl"] + LOSSY:
        main_retx = data.get(("M", arm), {}).get("retx", []) + data.get(("M2", arm), {}).get("retx", [])
        ref = statistics.median(main_retx) if main_retx else float("nan")
        for lab in LABELS:
            d = data.get((lab, arm))
            if d is None:
                print(f"{arm:7} {lab:4}   - no data")
                continue
            m50, s50 = mean_se(d["p50"])
            m99, s99 = mean_se(d["p99"])
            mr, sr = mean_se([float(x) for x in d["retx"]])
            d["hi"] = sum(1 for x in d["retx"] if ref > 0 and x > 2 * ref)
            tm = statistics.median(d["timer"]) if d["timer"] else float("nan")
            print(f"{arm:7} {lab:4} {len(d['p50']):3} {m50:7.2f}+-{s50:5.2f} {m99:8.2f}+-{s99:6.2f} "
                  f"{mr:8.0f}+-{sr:5.0f} {d['hi']:7} {tm:6.0f} {deliv.get((lab, arm), float('nan')):7.2f}")
            for e in d["excluded"]:
                print(f"        excluded {lab} {e}")
        if arm == "ctl":
            continue
        why = []
        for lab in LABELS:
            n = len(data.get((lab, arm), {}).get("p50", []))
            if n < MIN_REPS:
                why.append(f"{lab} {n} scored reps < {MIN_REPS}")
        if arm in cell_void:
            why.append("harness VOID: " + " | ".join(cell_void[arm]))
        if why:
            print(f"  NOT JUDGED {arm}: {'; '.join(why)}")
        else:
            judged.append(arm)

    print("\nper judged arm, K against each main tree (d = K - base, 2 SE):")
    better, worse = [], []
    for arm in judged:
        for met in ("p50", "p99"):
            res = {}
            for base in ("M", "M2"):
                res[base] = diff(data[("K", arm)][met], data[(base, arm)][met])
            aa = diff(data[("M2", arm)][met], data[("M", arm)][met])
            b = all(dd < -2 * se for dd, se in res.values())
            w = all(dd > 2 * se for dd, se in res.values())
            tag_ = "BETTER" if b else ("WORSE" if w else "-")
            print(f"  {arm:7} {met}: vs M {res['M'][0]:+8.2f} (2SE {2 * res['M'][1]:.2f}), vs M2 {res['M2'][0]:+8.2f} "
                  f"(2SE {2 * res['M2'][1]:.2f}); A/A M2-M {aa[0]:+8.2f} (2SE {2 * aa[1]:.2f})"
                  f"{'  A/A beyond 2SE' if abs(aa[0]) > 2 * aa[1] else ''} -> {tag_}")
            if b:
                better.append(f"{arm} {met} {res['M'][0]:+.2f}/{res['M2'][0]:+.2f} ms")
            if w:
                worse.append(f"{arm} {met} {res['M'][0]:+.2f}/{res['M2'][0]:+.2f} ms")
        dk, dm = deliv.get(("K", arm)), deliv.get(("M", arm))
        if dk is not None and dm is not None and dk < dm - 0.1:
            worse.append(f"{arm} delivered {dk:.2f}% < {dm:.2f}% - 0.1")

    if not judged:
        void.append("no lossy arm left to judge")
    ctl50 = statistics.mean(data[("M", "ctl")]["p50"]) if data.get(("M", "ctl"), {}).get("p50") else float("nan")
    lim = ctl50 + 4 * BASE_RTT_MS
    print(f"\nR4 (informational; base_rtt {BASE_RTT_MS} ms from the 200eefc6 baseline): limit = M ctl p50 {ctl50:.2f} + "
          f"4 x {BASE_RTT_MS} = {lim:.2f} ms")
    for arm in ("ge5b4", "ge10b4"):
        for lab in LABELS:
            v = data.get((lab, arm), {}).get("p50", [])
            if v:
                print(f"  R4 {arm} {lab}: median per-rep p50 {statistics.median(v):.2f} ms "
                      f"{'PASS' if statistics.median(v) <= lim else 'FAIL'}")
    hi = " ".join(f"{lab} {sum(data.get((lab, a), {}).get('hi', 0) for a in LOSSY)}/"
                  f"{sum(len(data.get((lab, a), {}).get('retx', [])) for a in LOSSY)}" for lab in LABELS)
    hi52 = " ".join(f"{lab} {data.get((lab, 'ge5b2'), {}).get('hi', 0)}/"
                    f"{len(data.get((lab, 'ge5b2'), {}).get('retx', []))}" for lab in LABELS)
    print(f"\n2x-resend reps (resends > 2 x main's median rep on that arm): lossy arms {hi}; ge5b2 {hi52}")
    for v in void:
        print(f"VOID reason: {v}")
    if void:
        verdict = "VOID - " + "; ".join(void)
    elif worse:
        verdict = "WORSE - " + "; ".join(worse) + (f" (also BETTER: {'; '.join(better)})" if better else "")
    elif better:
        verdict = "GAIN - " + "; ".join(better)
    else:
        verdict = f"NO_GAIN - no judged arm ({' '.join(judged)}) has K beyond 2 SE of both main trees"
    print(f"VERDICT: {verdict} | 2x-resend reps ge5b2: {hi52}")


if __name__ == "__main__":
    main()
