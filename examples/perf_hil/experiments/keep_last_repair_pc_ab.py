#!/usr/bin/env python3
"""Reads keep_last_repair_pc_ab.sh's runs.txt by the rules written in that script's header (2026-10-15).

Per (cell, loss, arm): the mean of each metric over valid runs; per metric, new's and base_aa's difference from base
(paired by repetition), the floor from base_aa, and a verdict.
"""
import math
import re
import sys
from collections import defaultdict

ARMS = ("base", "base_aa", "new")
# metric -> (higher is better, relative difference)
METRICS = {
    "delivered_pct": (True, False),
    "gap_abandoned": (False, False),
    "gap_evicted": (False, False),
    "wire_b_per_sample": (False, True),
    "p99_ms": (False, True),
    "send_mbps": (True, True),
}
REL_FLOOR = 0.01
ABS_FLOOR = {"delivered_pct": 0.1, "gap_abandoned": 0.5, "gap_evicted": 0.5}


def fields(text):
    return dict(re.findall(r"(\w+)=([^\s|]+)", text))


def metrics(cell, wire, pub, sub):
    out = {}
    if cell.startswith("T"):
        sent, recv = float(pub["sent"]), float(sub["recv"])
        out["delivered_pct"] = 100.0 * recv / sent if sent else None
        out["gap_abandoned"] = float(sub["gap_abandoned"])
        out["gap_evicted"] = float(sub["gap_evicted"])
        out["wire_b_per_sample"] = wire / sent if sent else None
        out["send_mbps"] = float(pub["send_mbps"])
    elif cell.startswith("L"):
        sent, recv = float(pub["sent"]), float(pub["recv"])
        out["delivered_pct"] = 100.0 * recv / sent if sent else None
        out["gap_abandoned"] = float(pub.get("gap_abandoned", 0)) + float(sub.get("gap_abandoned", 0))
        out["wire_b_per_sample"] = wire / sent if sent else None
        out["p99_ms"] = float(pub["rtt_p99_ms"])
    else:  # I1L, the large-sample bench
        sent, recv = float(pub["published"]), float(sub["delivered"])
        out["delivered_pct"] = 100.0 * recv / sent if sent else None
        out["gap_abandoned"] = float(sub.get("gap_abandoned", "nan"))
        out["gap_evicted"] = float(sub.get("gap_evicted", "nan"))
        out["wire_b_per_sample"] = wire / sent if sent else None
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

    print("cell  loss metric              base          base_aa d     new d (SE)        floor     verdict  (n)")
    for (cell, loss), by in sorted(runs.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        reps = sorted({rep for (_a, rep) in by})
        counts = {arm: sum(1 for (a, _r) in by if a == arm) for arm in ARMS}
        if min(counts.values()) < 3:
            print(f"{cell:5} {loss:>3}%  VOID: valid runs {counts}, voids "
                  f"{ {a: voids[(cell, loss, a)] for a in ARMS} }")
            continue
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
            beyond = abs(new_d) > floor and (math.isnan(new_se) or abs(new_d) > 2 * new_se)
            if beyond:
                better = (new_d > 0) == higher_better
                verdict = "BETTER" if better else "WORSE"
            unit = "%" if relative else ""
            scale = 100.0 if relative else 1.0
            print(f"{cell:5} {loss:>3}%  {metric:18} {base_mean:12.3f}  {aa_d * scale:+9.3f}{unit:1}  "
                  f"{new_d * scale:+9.3f}{unit:1} ({new_se * scale:.3f})  {floor * scale:8.3f}{unit:1}  {verdict:7}"
                  f"  ({counts['new']})")
        if cell == "I1L":
            torn = sum(m.get("torn", 0) for m in by.values())
            print(f"{cell:5} {loss:>3}%  torn over all runs: {torn:.0f}")


if __name__ == "__main__":
    main()
