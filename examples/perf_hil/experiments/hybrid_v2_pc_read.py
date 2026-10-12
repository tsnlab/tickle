#!/usr/bin/env python3
"""hybrid_v2_pc_read.py - reads hybrid_v2_pc.sh's results by the rules written in its header before the first run.

Usage: hybrid_v2_pc_read.py RESULTS   (the last "=== hybrid_v2_pc" session in the file)
Exit 0 when every hypothesis has a verdict, 2 when a cell could not be read.
"""
import math
import statistics
import sys

METRICS = ("latency_median_us", "latency_p99_us", "pub_cpu", "sub_cpu", "waits_per_mb", "insn_per_mb")


PERF = [False]  # the session counted instructions (perf=1): read insn_per_mb, and nothing timed (hybrid_v2_pc.sh)


def parse(path):
    lines = open(path, encoding="utf-8").read().splitlines()
    start = max(i for i, line in enumerate(lines) if line.startswith("=== hybrid_v2_pc"))
    print(lines[start])
    PERF[0] = " perf=1 " in lines[start]
    runs = []
    for line in lines[start:]:
        if not line.startswith("RUN "):
            continue
        parts = line.split(" | ")
        head = dict(kv.split("=", 1) for kv in parts[0].split()[1:] if "=" in kv)
        if len(parts) < 3:
            print(f"VOID {head.get('cell')} {head.get('arm')} block {head.get('block')}: no RESULT")
            continue
        pub = dict(kv.split("=", 1) for kv in parts[1].split() if "=" in kv)
        sub = dict(kv.split("=", 1) for kv in parts[2].split() if "=" in kv)
        runs.append((head, pub, sub))
    return runs


def void_reason(arm, head, pub, sub):
    published, delivered = int(pub["published"]), int(sub["delivered"])
    if int(sub["torn"]) or int(sub["out_of_order"]):
        return "torn/out_of_order"
    if int(sub["reliable"]) and delivered < published:
        return f"RELIABLE delivered {delivered} < published {published}"
    if not int(sub["reliable"]) and delivered < 0.98 * published:
        return f"BEST_EFFORT delivered {delivered} < 98% of {published}"
    if PERF[0] and (not head.get("insn_u", "none").isdigit() or not head.get("insn_k", "none").isdigit()):
        return "no perf counts"
    waits, writable = int(pub["large_send_waits"]), pub.get("large_send_writable")
    if arm == "B" and (writable is None or (waits > 0 and int(writable) == 0)):
        return f"B's treatment did not apply: send_waits {waits}, send_writable {writable}"
    if arm in ("A", "A2", "Bt") and writable is not None and int(writable) > 0:
        return f"{arm} resumed {writable} sends on writability"
    return None


def values(head, pub, sub):
    mb = int(pub["published"]) * int(pub["size"]) / 1e6
    return {
        "latency_median_us": float(sub["latency_median_us"]),
        "latency_p99_us": float(sub["latency_p99_us"]),
        "pub_cpu": float(pub["cpu_ms_per_mb"]),
        "sub_cpu": float(sub["cpu_ms_per_mb"]),
        "waits_per_mb": int(pub["large_send_waits"]) / mb,
        "insn_per_mb": (int(head["insn_u"]) + int(head["insn_k"])) / mb / 1e6 if PERF[0] else float("nan"),
    }


def welch(a, b):
    d = statistics.mean(a) - statistics.mean(b)
    se = math.sqrt(statistics.variance(a) / len(a) + statistics.variance(b) / len(b))
    return d, se


def judge(x, y, floor_d):
    """BETTER / WORSE / HELD for x against y (lower is better) beyond max(floor, 2 SE)."""
    d, se = welch(x, y)
    bound = max(floor_d, 2 * se)
    if d < -bound:
        return "BETTER", d, bound
    if d > bound:
        return "WORSE", d, bound
    return "HELD", d, bound


def main():
    runs = parse(sys.argv[1])
    by = {}
    for head, pub, sub in runs:
        why = void_reason(head["arm"], head, pub, sub)
        if why:
            print(f"VOID {head['cell']} {head['arm']} block {head['block']}: {why}")
            continue
        by.setdefault(head["cell"], {}).setdefault(head["arm"], []).append(values(head, pub, sub))
    status = 0
    h1_cells, h1_gain = [], {}
    h2_ok, h2_cells, harm = True, 0, False
    metrics = ("insn_per_mb", "waits_per_mb") if PERF[0] else METRICS[:-1]
    print(f"session perf={int(PERF[0])}: reading {', '.join(metrics)} (hybrid_v2_pc.sh header)")
    for cell in sorted(by):
        arms = by[cell]
        if any(len(arms.get(a, [])) < 3 for a in ("A", "A2", "B", "Bt")):
            print(f"=== {cell}: fewer than 3 runs of an arm - not read ({ {a: len(v) for a, v in arms.items()} })")
            status = 2
            continue
        print(f"=== {cell} (means over {len(arms['A'])} runs) ===")
        col = {a: {m: [v[m] for v in arms[a]] for m in metrics} for a in arms}
        for m in metrics:
            print(f"  {m:18s} " + "  ".join(f"{a} {statistics.mean(col[a][m]):10.2f}" for a in ("A", "A2", "B", "Bt")))
        floor = {m: abs(statistics.mean(col["A2"][m]) - statistics.mean(col["A"][m])) for m in metrics}
        verdicts = {}
        for (x, y) in (("B", "A"), ("B", "Bt"), ("Bt", "A")):
            for m in metrics:
                v, d, bound = judge(col[x][m], col[y][m], floor[m])
                verdicts[(x, y, m)] = v
                print(f"  {x} vs {y} {m:18s} d {d:+10.3f} bound {bound:8.3f} -> {v}")
        if not PERF[0]:
            h1_cells.append(verdicts[("B", "A", "latency_median_us")] == "BETTER")
            h1_gain[cell] = (statistics.mean(col["A"]["latency_median_us"]) -
                             statistics.mean(col["B"]["latency_median_us"]))
            harm = harm or verdicts[("B", "A", "pub_cpu")] == "WORSE" or verdicts[("B", "A", "sub_cpu")] == "WORSE"
        bt_w, b_w = statistics.mean(col["Bt"]["waits_per_mb"]), statistics.mean(col["B"]["waits_per_mb"])
        if bt_w > 0:
            h2_cells += 1
            halved = b_w <= 0.5 * bt_w
            insn_ok = (not PERF[0] or
                       all(verdicts[(x, y, "insn_per_mb")] != "WORSE" for x, y in (("B", "Bt"), ("B", "A"))))
            print(f"  H2 send_waits/MB Bt {bt_w:.1f} -> B {b_w:.1f} ({'halved' if halved else 'NOT halved'}); "
                  f"insn/MB never WORSE: {'yes' if insn_ok else 'no'}{'' if PERF[0] else ' (not counted here)'}")
            h2_ok = h2_ok and halved and insn_ok
    print("=== hypotheses ===")
    gains = " ".join(f"{c} {g:+.0f} us" for c, g in sorted(h1_gain.items()))
    four = [g for c, g in h1_gain.items() if "4m" in c]
    one = [g for c, g in h1_gain.items() if "1m" in c]
    bigger = bool(four and one) and min(four) > max(one)
    if PERF[0]:
        print(f"H2 (writability), instructions: {'SUPPORTED' if h2_ok and h2_cells else 'NOT SUPPORTED'} over "
              f"{h2_cells} cell(s); H1 and NO HARM are read from a perf=0 session")
        return status
    print(f"H1 (call cap): {'SUPPORTED' if h1_cells and all(h1_cells) and bigger else 'NOT SUPPORTED'} - B latency "
          f"median BETTER than A in {sum(h1_cells)} of {len(h1_cells)} cells, 4 MB gain larger: {bigger} ({gains})")
    if h2_cells == 0:
        print("H2 (writability): NOT EXERCISED - Bt never waited for room")
    else:
        print(f"H2 (writability): {'SUPPORTED' if h2_ok else 'NOT SUPPORTED'} over {h2_cells} cell(s) where Bt waited")
    print(f"NO HARM (B vs A CPU per MB): {'HELD' if not harm else 'WORSE somewhere'}")
    return status


if __name__ == "__main__":
    sys.exit(main())
