#!/usr/bin/env python3
"""rx_gap_pc_read.py - reads rx_gap_pc.sh's results by the rules written in its header before the first run.

Usage: rx_gap_pc_read.py RESULTS   (the last "=== rx_gap_pc" session in the file)
       rx_gap_pc_read.py --selftest
Exit 0 when every RELIABLE cell has a verdict, 2 when one could not be read.
"""
import math
import statistics
import sys

PRIMARY = "insn_per_mb"
METRICS = (PRIMARY, "insn_u_per_mb", "insn_k_per_mb", "cyc_per_mb", "latency_median_us")
MIN_RUNS = 2


def parse_lines(lines):
    starts = [i for i, line in enumerate(lines) if line.startswith("=== rx_gap_pc")]
    if not starts:
        return None, []
    header = lines[starts[-1]]
    runs = []
    for line in lines[starts[-1]:]:
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
    return header, runs


def void_reason(head, pub, sub):
    published, delivered = int(pub["published"]), int(sub["delivered"])
    if int(sub["torn"]) or int(sub["out_of_order"]):
        return "torn/out_of_order"
    if int(sub["reliable"]) and delivered < published:
        return f"RELIABLE delivered {delivered} < published {published}"
    if not int(sub["reliable"]) and delivered < 0.98 * published:
        return f"BEST_EFFORT delivered {delivered} < 98% of {published}"
    for key in ("insn_u", "insn_k", "cyc_u", "cyc_k"):
        if not head.get(key, "none").isdigit():
            return f"no perf count {key}"
    if delivered == 0:
        return "nothing delivered"
    return None


def values(head, sub):
    mb = int(sub["delivered"]) * int(sub["size"]) / 1e6
    iu, ik = int(head["insn_u"]), int(head["insn_k"])
    return {
        PRIMARY: (iu + ik) / mb,
        "insn_u_per_mb": iu / mb,
        "insn_k_per_mb": ik / mb,
        "cyc_per_mb": (int(head["cyc_u"]) + int(head["cyc_k"])) / mb,
        "latency_median_us": float(sub["latency_median_us"]),
    }


def mean_se(xs):
    m = statistics.mean(xs)
    se = statistics.stdev(xs) / math.sqrt(len(xs)) if len(xs) > 1 else float("inf")
    return m, se


def judge(cells):
    """cells: {cell: {arm: [values dict]}} -> (lines, verdicts {cell: verdict of PRIMARY or None})."""
    out = []
    verdicts = {}
    for cell in sorted(cells):
        arms = cells[cell]
        if any(len(arms.get(a, [])) < MIN_RUNS for a in ("A", "B", "A2")):
            out.append(f"CELL {cell} NOT READ: runs A {len(arms.get('A', []))} B {len(arms.get('B', []))} "
                       f"A2 {len(arms.get('A2', []))} (fewer than {MIN_RUNS})")
            verdicts[cell] = None
            continue
        for metric in METRICS:
            a = [v[metric] for v in arms["A"]]
            b = [v[metric] for v in arms["B"]]
            a2 = [v[metric] for v in arms["A2"]]
            ma, sa = mean_se(a)
            mb, sb = mean_se(b)
            ma2, _ = mean_se(a2)
            floor = max(abs(ma2 - ma), 2 * math.sqrt(sa * sa + sb * sb))
            delta = mb - ma
            verdict = "HELD"
            if delta < -floor:
                verdict = "BETTER"
            elif delta > floor:
                verdict = "WORSE"
            if metric == PRIMARY:
                verdicts[cell] = verdict
            out.append(f"CELL {cell} {metric}: A {ma:,.0f} A2 {ma2:,.0f} B {mb:,.0f} delta {delta:+,.0f} "
                       f"({100 * delta / ma:+.2f}%) floor {floor:,.0f} -> {verdict}")
    return out, verdicts


def read(lines):
    header, runs = parse_lines(lines)
    if header is None:
        print("NOT READ: no '=== rx_gap_pc' session")
        return 2
    print(header)
    cells = {}
    for head, pub, sub in runs:
        reason = void_reason(head, pub, sub)
        if reason:
            print(f"VOID {head['cell']} {head['arm']} block {head['block']}: {reason}")
            continue
        cells.setdefault(head["cell"], {}).setdefault(head["arm"], []).append(values(head, sub))
    out, verdicts = judge(cells)
    for line in out:
        print(line)
    # Context, not deciding (added 2026-10-13 after the first sessions, so not part of the rule above): the PC's own
    # socket drops (other VMs on the host) bring repairs whose units go the slow way and move instructions per MB by
    # several percent. The same PRIMARY over the runs whose subscriber dropped nothing, with their count.
    clean = {}
    for head, pub, sub in runs:
        if void_reason(head, pub, sub) is None and sub.get("socket_drops") == "0":
            clean.setdefault(head["cell"], {}).setdefault(head["arm"], []).append(values(head, sub)[PRIMARY])
    for cell in sorted(clean):
        parts = [f"{arm} {statistics.mean(xs):,.0f} (n={len(xs)})" for arm, xs in sorted(clean[cell].items())]
        print(f"CONTEXT {cell} {PRIMARY}, runs without socket drops only: " + ", ".join(parts))
    reliable = [c for c in verdicts if c.startswith("rel")]
    if not reliable or any(verdicts[c] is None for c in reliable):
        print("READING: NOT READ (a RELIABLE cell has no verdict)")
        return 2
    if all(verdicts[c] == "BETTER" for c in reliable):
        print("READING: SUPPORTED - fewer receive instructions per MB in every RELIABLE cell")
    elif any(verdicts[c] == "WORSE" for c in reliable):
        print("READING: REFUTED - more receive instructions per MB in a RELIABLE cell")
    else:
        print("READING: NOT SUPPORTED - no RELIABLE cell beyond its floor in both directions")
    return 0


def selftest():
    def line(cell, arm, block, iu, ik, delivered=300, torn=0, lat=900.0):
        return (f"RUN cell={cell} arm={arm} block={block} insn_u={iu} insn_k={ik} cyc_u={iu} cyc_k={ik} | "
                f"role=pub size=1048576 published=300 | role=sub size=1048576 reliable=1 delivered={delivered} "
                f"torn={torn} out_of_order=0 latency_median_us={lat}")
    base = ["=== rx_gap_pc test ==="]
    better = base + [line("rel1m", a, k, 1000000 + k * 1000 - (200000 if a == "B" else 0), 2000000)
                     for k in (1, 2, 3) for a in ("A", "B", "A2")]
    lost = base + [line("rel1m", a, k, 1000000, 2000000, delivered=299 if a == "B" else 300)
                   for k in (1, 2, 3) for a in ("A", "B", "A2")]
    worse = base + [line("rel1m", a, k, 1000000 + k * 1000 + (300000 if a == "B" else 0), 2000000)
                    for k in (1, 2, 3) for a in ("A", "B", "A2")]
    checks = [(better, 0, "SUPPORTED"), (lost, 2, "NOT READ"), (worse, 0, "REFUTED"), ([], 2, "NOT READ")]
    import contextlib
    import io
    for lines, want_rc, want in checks:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            got_rc = read(lines)
        if got_rc != want_rc or want not in buf.getvalue():
            print(f"SELFTEST FAILED: wanted rc {want_rc} and '{want}', got rc {got_rc}:\n{buf.getvalue()}")
            return 1
    print("SELFTEST OK")
    return 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        return read(f.read().splitlines())


if __name__ == "__main__":
    sys.exit(main())
