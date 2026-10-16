#!/usr/bin/env python3
"""Reads srtt_busy_writer_pc_ab.sh's runs.txt by the rules written in that script's header (2026-10-16).

Per (cell, loss, arm): each metric over valid runs; per metric, new's and base_aa's difference from base (paired by
repetition), the floor from base_aa, and a verdict. The control cell (T4 at 1%) is checked: anything but HELD
there says the session cannot be read, and the summary says so.
"""
import math
import re
import sys
from collections import defaultdict

ARMS = ("base", "base_aa", "new")
# metric -> (higher is better, relative difference)
METRICS = {
    "delivered_pct": (True, False),
    "wire_b_per_sample": (False, True),
    "writer_dgrams_per_sample": (False, True),
    "reader_dgrams_per_sample": (False, True),
    "socket_drops_per_sample": (False, True),
    "writer_resends_per_sample": (False, True),
    "median_ms": (False, True),
    "p99_ms": (False, True),
    "send_mbps": (True, True),
}
REL_FLOOR = 0.01
ABS_FLOOR = {"delivered_pct": 0.1}
CONTROL = ("T4", "1")


def fields(text):
    return dict(re.findall(r"(\w+)=([^\s|]+)", text))


def metrics(cell, wire, pub, sub):
    out = {}
    if cell == "T4":
        sent, recv = float(pub["sent"]), float(sub["recv"])
        out["delivered_pct"] = 100.0 * recv / sent if sent else None
        out["wire_b_per_sample"] = wire / sent if sent else None
        out["send_mbps"] = float(pub["send_mbps"])
    elif cell == "L4":
        sent, recv = float(pub["sent"]), float(pub["recv"])
        out["delivered_pct"] = 100.0 * recv / sent if sent else None
        out["wire_b_per_sample"] = wire / sent if sent else None
        out["p99_ms"] = float(pub["rtt_p99_ms"])
    else:  # I4, the large-sample bench
        sent, recv = float(pub["published"]), float(sub["delivered"])
        out["delivered_pct"] = 100.0 * recv / sent if sent else None
        out["wire_b_per_sample"] = wire / sent if sent else None
        out["writer_dgrams_per_sample"] = float(pub["tx_datagrams"]) / sent if sent else None
        out["reader_dgrams_per_sample"] = float(sub["tx_datagrams"]) / sent if sent else None
        out["socket_drops_per_sample"] = float(sub["socket_drops"]) / sent if sent else None
        out["writer_resends_per_sample"] = float(pub["retransmitted"]) / sent if sent else None
        out["socket_drops"] = float(sub["socket_drops"])
        out["median_ms"] = float(sub["latency_median_us"]) / 1000.0
        out["p99_ms"] = float(sub["latency_p99_us"]) / 1000.0
        out["torn"] = float(sub["torn"])
    out["sent"] = sent
    return out


def mean_se(xs):
    if not xs:
        return float("nan"), float("nan")
    m = sum(xs) / len(xs)
    if len(xs) < 2:
        return m, float("nan")
    var = sum((x - m) ** 2 for x in xs) / (len(xs) - 1)
    return m, math.sqrt(var / len(xs))


def verdicts(by):
    """(metric, base mean, aa d, new d, new se, floor, verdict, relative) per metric of one cell."""
    reps = sorted({rep for (_a, rep) in by})
    rows = []
    for metric, (higher_better, relative) in METRICS.items():
        if metric not in next(iter(by.values())):
            continue
        base_vals = [by[("base", r)][metric] for r in reps if ("base", r) in by]
        diffs = {}
        for arm in ("base_aa", "new"):
            d = []
            for r in reps:
                if ("base", r) in by and (arm, r) in by:
                    b, x = by[("base", r)][metric], by[(arm, r)][metric]
                    if b is None or x is None or math.isnan(b) or math.isnan(x):
                        continue
                    if relative:
                        if b != 0:
                            d.append(x / b - 1.0)
                    else:
                        d.append(x - b)
            diffs[arm] = mean_se(d)
        base_mean, _ = mean_se([v for v in base_vals if v is not None])
        aa_d, aa_se = diffs["base_aa"]
        new_d, new_se = diffs["new"]
        floor = max(REL_FLOOR if relative else ABS_FLOOR.get(metric, 0.0),
                    abs(aa_d) + 2 * (aa_se if not math.isnan(aa_se) else 0.0))
        verdict = "HELD"
        if abs(new_d) > floor and (math.isnan(new_se) or abs(new_d) > 2 * new_se):
            verdict = "BETTER" if (new_d > 0) == higher_better else "WORSE"
        rows.append((metric, base_mean, aa_d, new_d, new_se, floor, verdict, relative))
    return rows


# The second reading (srtt_busy_writer_pc_ab.sh header, written before the third run): a run whose reader dropped more
# than COLLAPSE_DROPS datagrams at its socket is a collapse, counted per arm; the rest compared by their median.
COLLAPSE_DROPS = 1000
ROBUST = ("median_ms", "p99_ms", "writer_resends_per_sample", "wire_b_per_sample")
ROBUST_REL = 0.05


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return float("nan")
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def robust(cell, loss, by):
    collapses = {a: sum(1 for (arm, _r), m in by.items() if arm == a and m["socket_drops"] > COLLAPSE_DROPS)
                 for a in ARMS}
    runs = {a: sum(1 for (arm, _r) in by if arm == a) for a in ARMS}
    worst = max(collapses["base"], collapses["base_aa"])
    verdict = "WORSE" if collapses["new"] >= worst + 3 else ("BETTER" if collapses["new"] + 3 <= min(
        collapses["base"], collapses["base_aa"]) else "HELD")
    print(f"{cell:5} {loss:>3}%  [second reading] collapses (socket drops > {COLLAPSE_DROPS}) of runs: "
          + ", ".join(f"{a} {collapses[a]}/{runs[a]}" for a in ARMS) + f"  {verdict}")
    for metric in ROBUST:
        med = {}
        for a in ARMS:
            med[a] = median([m[metric] for (arm, _r), m in by.items()
                             if arm == a and m["socket_drops"] <= COLLAPSE_DROPS and m.get(metric) is not None])
        base = med["base"]
        if not base or math.isnan(base):
            continue
        aa_d, new_d = med["base_aa"] / base - 1.0, med["new"] / base - 1.0
        floor = max(ROBUST_REL, 2 * abs(aa_d))
        v = "HELD"
        if abs(new_d) > floor:
            v = "BETTER" if new_d < 0 else "WORSE"
        print(f"{cell:5} {loss:>3}%  [second reading] {metric:24} median of non-collapsed runs: base {base:.3f}, "
              f"base_aa {aa_d * 100:+.1f}%, new {new_d * 100:+.1f}%, floor {floor * 100:.1f}%  {v}")


def main():
    runs = defaultdict(dict)  # (cell, loss) -> {(arm, rep): metrics}
    voids = defaultdict(int)
    for line in open(sys.argv[1]):
        head = line.split("|")[0].split()
        if len(head) < 4:
            continue
        cell = head[0]
        loss = head[1].split("=")[1]
        arm = head[2].split("=")[1]
        rep = int(head[3].split("=")[1])
        if "VOID" in line.split("|")[0]:
            voids[(cell, loss, arm)] += 1
            continue
        parts = line.split("|")
        wire = float(fields(parts[0])["wire_bytes"])
        pub, sub = fields(parts[1]), fields(parts[2])
        try:
            m = metrics(cell, wire, pub, sub)
        except (KeyError, ValueError):
            voids[(cell, loss, arm)] += 1
            continue
        if not m["sent"]:
            voids[(cell, loss, arm)] += 1
            continue
        runs[(cell, loss)][(arm, rep)] = m

    control_ok = None
    print("cell  loss metric                    base          base_aa d     new d (SE)        floor     verdict  (n)")
    for (cell, loss), by in sorted(runs.items(), key=lambda kv: (kv[0][0], int(kv[0][1]))):
        counts = {arm: sum(1 for (a, _r) in by if a == arm) for arm in ARMS}
        if min(counts.values()) < 3:
            print(f"{cell:5} {loss:>3}%  VOID: valid runs {counts}, voids "
                  f"{ {a: voids[(cell, loss, a)] for a in ARMS} }")
            if (cell, loss) == CONTROL:
                control_ok = False
            continue
        rows = verdicts(by)
        if (cell, loss) == CONTROL:
            control_ok = all(v == "HELD" for (_m, _b, _a, _n, _s, _f, v, _r) in rows)
        for metric, base_mean, aa_d, new_d, new_se, floor, verdict, relative in rows:
            unit = "%" if relative else ""
            scale = 100.0 if relative else 1.0
            print(f"{cell:5} {loss:>3}%  {metric:24} {base_mean:12.3f}  {aa_d * scale:+9.3f}{unit:1}  "
                  f"{new_d * scale:+9.3f}{unit:1} ({new_se * scale:.3f})  {floor * scale:8.3f}{unit:1}  {verdict:7}"
                  f"  ({counts['new']})")
        if cell == "I4":
            torn = sum(m.get("torn", 0) for m in by.values())
            print(f"{cell:5} {loss:>3}%  torn over all runs: {torn:.0f}")
            robust(cell, loss, by)
    if control_ok is None:
        print("CONTROL MISSING: no T4 at 1% - the session cannot be read")
    elif not control_ok:
        print("CONTROL NOT HELD: T4 at 1% moved, where the change cannot act - the session is too noisy to read")
    else:
        print("control HELD: T4 at 1% unchanged")


if __name__ == "__main__":
    main()
