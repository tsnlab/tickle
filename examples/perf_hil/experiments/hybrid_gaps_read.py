#!/usr/bin/env python3
"""hybrid_gaps_read.py - reads hybrid_gaps_pc.sh's results by the rules written in its header before the first run.

Usage: hybrid_gaps_read.py RESULTS [--since LINE_PREFIX]   (the last "=== hybrid_gaps_pc" session in the file by default)
"""
import math
import statistics
import sys

N_BUCKET = {45: 6, 8: 3}  # hybrid_bucket(): 1, 2-3, 4-7, 8-15, 16-31, 32-44, 45
FORCED_N = {"ip45": 45, "ip45c1": 45, "ip45c2": 45, "ip8c11": 8}
METRICS = ("latency_median_us", "latency_p99_us", "pub_cpu", "sub_cpu")


def parse(path):
    lines = open(path, encoding="utf-8").read().splitlines()
    start = max(i for i, line in enumerate(lines) if line.startswith("=== hybrid_gaps_pc"))
    print(lines[start])
    runs = []
    for line in lines[start:]:
        if not line.startswith("RUN "):
            continue
        parts = line.split(" | ")
        head = dict(kv.split("=", 1) for kv in parts[0].split()[1:] if "=" in kv)
        if len(parts) < 3:
            print(f"VOID {head}: no RESULT")
            continue
        pub = dict(kv.split("=", 1) for kv in parts[1].split() if "=" in kv)
        sub = dict(kv.split("=", 1) for kv in parts[2].split() if "=" in kv)
        runs.append((head, pub, sub))
    return runs


def void_reason(arm, pub, sub):
    published, delivered = int(pub["published"]), int(sub["delivered"])
    if int(sub["torn"]) or int(sub["out_of_order"]):
        return "torn/out_of_order"
    if int(sub["reliable"]) and delivered < published:
        return f"RELIABLE delivered {delivered} < published {published}"
    if not int(sub["reliable"]) and delivered < 0.98 * published:
        return f"BEST_EFFORT delivered {delivered} < 98% of {published}"
    hist = [int(x) for x in pub["hybrid_n_hist"].split("/")]
    if arm in FORCED_N:
        want = N_BUCKET[FORCED_N[arm]]
        if hist.index(max(hist)) != want:
            return f"n histogram {pub['hybrid_n_hist']} not at the forced n {FORCED_N[arm]}"
    elif int(pub["hybrid_runs"]) != 0:
        return f"mtu arm sent {pub['hybrid_runs']} runs"
    return None


def welch(a, b):
    """d = mean(a) - mean(b), and its standard error."""
    d = statistics.mean(a) - statistics.mean(b)
    se = math.sqrt(statistics.variance(a) / len(a) + statistics.variance(b) / len(b)) if min(len(a), len(b)) > 1 else 0
    return d, se


def main():
    runs = parse(sys.argv[1])
    data = {}  # (cell, arm) -> metric -> [values]
    for head, pub, sub in runs:
        reason = void_reason(head["arm"], pub, sub)
        if reason:
            print(f"VOID cell={head['cell']} arm={head['arm']} block={head['block']}: {reason}")
            continue
        row = data.setdefault((head["cell"], head["arm"]), {m: [] for m in METRICS})
        row["latency_median_us"].append(float(sub["latency_median_us"]))
        row["latency_p99_us"].append(float(sub["latency_p99_us"]))
        row["pub_cpu"].append(float(pub["cpu_ms_per_mb"]))
        row["sub_cpu"].append(float(sub["cpu_ms_per_mb"]))
    cells = sorted({c for c, _ in data})
    arms = ["mtu", "mtuc", "ip45", "ip45c2", "ip45c1", "ip8c11"]
    print("\nMEANS (n runs): latency median / p99 us, cpu ms/MB pub / sub")
    for cell in cells:
        for arm in arms:
            row = data.get((cell, arm))
            if not row:
                continue
            print(f"  {cell:6s} {arm:7s} n={len(row['latency_median_us'])} "
                  + " ".join(f"{m}={statistics.mean(row[m]):.3f}" for m in METRICS))
    verdicts = {}

    def compare(cell, a, b, metric, label):
        ra, rb = data.get((cell, a)), data.get((cell, b))
        rc, rm = data.get((cell, "mtuc")), data.get((cell, "mtu"))
        if not ra or not rb or len(ra[metric]) < 3 or len(rb[metric]) < 3 or not rc or not rm:
            print(f"  {label} {cell} {metric}: NOT READ (fewer than 3 runs of an arm, or no control)")
            return None
        d, se = welch(ra[metric], rb[metric])
        control = abs(statistics.mean(rc[metric]) - statistics.mean(rm[metric]))
        floor = max(control, 2 * se)
        verdict = "BETTER" if d < -floor else "WORSE" if d > floor else "HELD"
        print(f"  {label} {cell} {metric}: {a} - {b} = {d:+.1f} (floor {floor:.1f}: control {control:.1f}, 2SE {2 * se:.1f})"
              f" {verdict}")
        verdicts[(label, cell, metric)] = (verdict, d)
        return verdict

    print("\nH_A batching: ip45c1 vs ip45")
    for cell in cells:
        compare(cell, "ip45c1", "ip45", "latency_median_us", "H_A")
    gains = {c: verdicts.get(("H_A", c, "latency_median_us"), (None, 0))[1] for c in cells}
    four = [c for c in cells if "4m" in c]
    read = [c for c in four if ("H_A", c, "latency_median_us") in verdicts]
    if len(read) < len(four) or not four:
        print(f"  H_A: NOT DECIDED (4 MB cells read: {len(read)} of {len(four)})")
    else:
        falsified = any(verdicts[("H_A", c, "latency_median_us")][0] != "BETTER" for c in four)
        larger = all(gains.get(c.replace("4m", "1m"), 0) > gains[c] for c in four)
        print(f"  H_A: {'FALSIFIED' if falsified else 'SUPPORTED'}; 4 MB gain larger than 1 MB: {larger}")
    print("\nH_A2 remainder: ip45c1 vs mtu")
    for cell in cells:
        compare(cell, "ip45c1", "mtu", "latency_median_us", "H_A2")
    print("\nH_B store-and-forward: ip8c11 vs ip45c2")
    for cell in cells:
        compare(cell, "ip8c11", "ip45c2", "latency_median_us", "H_B")
    print("\nshipped run path vs MTU: ip45 vs mtu")
    for cell in cells:
        compare(cell, "ip45", "mtu", "latency_median_us", "SHIPPED")
        compare(cell, "ip45", "mtu", "sub_cpu", "SHIPPED")
        compare(cell, "ip45", "mtu", "pub_cpu", "SHIPPED")
    print("\nNO HARM: ip45c1 vs ip45 CPU and p99")
    for cell in cells:
        for metric in ("pub_cpu", "sub_cpu", "latency_p99_us"):
            compare(cell, "ip45c1", "ip45", metric, "NOHARM")


if __name__ == "__main__":
    main()
