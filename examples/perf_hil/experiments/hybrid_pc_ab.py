#!/usr/bin/env python3
"""hybrid_pc_ab.py - the verdict of hybrid_pc_ab.sh, by the rules written in its header before the first run.

Usage: hybrid_pc_ab.py OUTDIR "ARM ARM ..."   (the first arm is the reference, the second its A/A twin)
"""
import math
import pathlib
import re
import sys

REPS_MIN = 3
FLOOR_MIN = 0.01
RELIABLE_SLACK = 10  # KEEP_LAST may let a sample behind a 4 MB send go; more than this is a fault
N_MAX_BUCKET = 6     # the last of the seven n-histogram buckets: n = n_max


def fields(line):
    return dict(kv.split("=", 1) for kv in line.split() if "=" in kv)


def result(path):
    if not path.exists():
        return None
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("RESULT:"):
            return fields(line[len("RESULT:"):])
    return None


def instructions(path):
    if not path.exists():
        return None
    total = 0
    seen = 0
    for line in path.read_text().splitlines():
        parts = line.split(",")
        if len(parts) > 2 and parts[2].startswith("instructions"):
            try:
                total += int(parts[0])
                seen += 1
            except ValueError:
                return None
    return total if seen == 2 else None


def mean_se(values):
    n = len(values)
    if n == 0:
        return float("nan"), float("nan")
    m = sum(values) / n
    if n < 2:
        return m, float("inf")
    var = sum((v - m) ** 2 for v in values) / (n - 1)
    return m, math.sqrt(var / n)


def main():
    outd = pathlib.Path(sys.argv[1])
    arms = sys.argv[2].split()
    ref, twin = arms[0], arms[1]
    runs = {}  # (cell, arm, rep) -> dict
    voids = []
    hist_hits = {}
    for job in sorted(outd.glob("runs/*/job")):
        meta = fields(job.read_text())
        rd = job.parent
        pub, sub = result(rd / "pub.log"), result(rd / "sub.log")
        ipub, isub = instructions(rd / "pub.perf"), instructions(rd / "sub.perf")
        key = (meta["cell"], meta["arm"], int(meta["rep"]))
        why = []
        if pub is None or sub is None:
            why.append("no RESULT")
        if ipub is None or isub is None:
            why.append("perf missing")
        if sub is not None and sub.get("torn", "1") != "0":
            why.append(f"torn={sub.get('torn')}")
        if pub is not None and sub is not None and meta["qos"] == "REL":
            if int(sub["delivered"]) < int(pub["published"]) - RELIABLE_SLACK:
                why.append(f"delivered {sub['delivered']}/{pub['published']}")
        if why:
            voids.append(f"{rd.name}: {', '.join(why)}")
            continue
        mb = int(pub["published"]) * int(meta["size"]) / 1e6
        delivered_mb = int(sub["delivered"]) * int(meta["size"]) / 1e6
        runs[key] = dict(pub=ipub / mb, sub=isub / mb, sub_delivered=isub / delivered_mb if delivered_mb else
                         float("nan"), share=int(sub["delivered"]) / max(1, int(pub["published"])),
                         p50=float(sub.get("latency_median_us", "nan")), p99=float(sub.get("latency_p99_us", "nan")),
                         hist=pub.get("hybrid_n_hist", "-"), cap=sub.get("hybrid_advertised", "-"))
        if meta["arm"] not in (ref, twin) and meta["loss"] == "0" and "hybrid_n_hist" in pub:
            buckets = [int(x) for x in pub["hybrid_n_hist"].split("/")]
            hist_hits.setdefault(meta["cell"], []).append(buckets[N_MAX_BUCKET] > 0)
    for v in voids:
        print(f"VOID run {v}")
    cells = sorted({k[0] for k in runs})
    overall_void = []
    for cell in cells:
        for arm in arms:
            reps = [k[2] for k in runs if k[0] == cell and k[1] == arm]
            if len(reps) < REPS_MIN:
                overall_void.append(f"{cell}/{arm}: {len(reps)} valid runs")
        if cell in hist_hits and not all(hist_hits[cell]):
            overall_void.append(f"{cell}: the AUTO arm never reached n_max at 0% loss")
    print("cell        process  arm        instr/MB(ref)   d        SE       floor   verdict   delivered  p50us  p99us")
    for cell in cells:
        for proc in ("pub", "sub"):
            reps = sorted({k[2] for k in runs if k[0] == cell})
            ref_vals = {r: runs[(cell, ref, r)][proc] for r in reps if (cell, ref, r) in runs}
            twin_d = [runs[(cell, twin, r)][proc] / ref_vals[r] - 1 for r in reps
                      if (cell, twin, r) in runs and r in ref_vals]
            td, tse = mean_se(twin_d)
            floor = max(FLOOR_MIN, abs(td) + 2 * tse) if twin_d else float("nan")
            ref_mean = mean_se(list(ref_vals.values()))[0]
            for arm in arms[1:]:
                ds = [runs[(cell, arm, r)][proc] / ref_vals[r] - 1 for r in reps
                      if (cell, arm, r) in runs and r in ref_vals]
                d, se = mean_se(ds)
                if arm == twin:
                    verdict = "A/A"
                elif d < -floor and d < -2 * se:
                    verdict = "BETTER"
                elif d > floor and d > 2 * se:
                    verdict = "COST"
                else:
                    verdict = "HELD"
                arm_runs = [runs[(cell, arm, r)] for r in reps if (cell, arm, r) in runs]
                share = mean_se([x["share"] for x in arm_runs])[0]
                p50 = mean_se([x["p50"] for x in arm_runs])[0]
                p99 = mean_se([x["p99"] for x in arm_runs])[0]
                print(f"{cell:<11} {proc:<8} {arm:<10} {ref_mean / 1e6:>10.2f}M  {d:+7.2%} {se:7.2%} {floor:7.2%}"
                      f"   {verdict:<8} {share:9.3f} {p50:7.0f} {p99:7.0f}")
    print("realized treatment (last rep of each arm): cell arm n_hist sub_advertised")
    for cell in cells:
        for arm in arms:
            reps = sorted(k[2] for k in runs if k[0] == cell and k[1] == arm)
            if reps:
                x = runs[(cell, arm, reps[-1])]
                print(f"  {cell:<11} {arm:<10} {x['hist']:<22} {x['cap']}")
    if overall_void:
        print("OVERALL VOID: " + "; ".join(overall_void))
    else:
        print("overall: valid")


if __name__ == "__main__":
    main()
