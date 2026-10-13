#!/usr/bin/env python3
"""tx_gap_pc_read.py - reads tx_gap_pc.sh's results by the rules written in its header before the first run.

Usage: tx_gap_pc_read.py RESULTS   (the last "=== tx_gap_pc" session in the file)
       tx_gap_pc_read.py --selftest
Exit 0 when every arm step in every BEST_EFFORT cell has a verdict, 2 when one could not be read.
"""
import math
import statistics
import sys

PRIMARY = "insn_per_mb"
METRICS = (PRIMARY, "insn_u_per_mb", "insn_k_per_mb", "cyc_per_mb", "task_ms_per_mb")
TREATMENT = ("send_waits_per_mb", "tx_datagrams_per_mb", "runs_per_mb", "ctx_per_mb", "empty_reads_per_mb",
             "timer_fires_per_mb")
MIN_RUNS = 2


def kv(text):
    return dict(item.split("=", 1) for item in text.split() if "=" in item)


def parse_lines(lines):
    starts = [i for i, line in enumerate(lines) if line.startswith("=== tx_gap_pc")]
    if not starts:
        return None, [], []
    header = lines[starts[-1]]
    arms = [item.split("=", 1)[0] for item in header.split(" arms:", 1)[1].split(" pub_flags=")[0].split()]
    arms = [arm for arm in arms if arm != "BASE2"]  # the A/A arm is not a step of the branch
    runs = []
    for line in lines[starts[-1]:]:
        if not line.startswith("RUN ") or " mode=perf " not in line:
            continue
        parts = line.split(" | ")
        head = kv(parts[0][4:])
        if len(parts) < 3:
            print(f"VOID {head.get('cell')} {head.get('arm')} block {head.get('block')}: no RESULT")
            continue
        runs.append((head, kv(parts[1]), kv(parts[2])))
    return header, arms, runs


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
    if published == 0:
        return "nothing published"
    return None


def values(head, pub):
    mb = int(pub["published"]) * int(pub["size"]) / 1e6
    iu, ik = int(head["insn_u"]), int(head["insn_k"])
    out = {
        PRIMARY: (iu + ik) / mb,
        "insn_u_per_mb": iu / mb,
        "insn_k_per_mb": ik / mb,
        "cyc_per_mb": (int(head["cyc_u"]) + int(head["cyc_k"])) / mb,
    }
    try:
        out["task_ms_per_mb"] = float(head["task_ms"]) / mb
    except (KeyError, ValueError):
        pass
    for name, source, key in (("send_waits_per_mb", pub, "large_send_waits"), ("tx_datagrams_per_mb", pub,
                                                                                 "tx_datagrams"),
                              ("runs_per_mb", pub, "hybrid_runs"), ("ctx_per_mb", head, "ctx"),
                              ("empty_reads_per_mb", head, "pub_empty_reads"),
                              ("timer_fires_per_mb", head, "pub_timer_fires")):
        if source.get(key, "none").isdigit():
            out[name] = int(source[key]) / mb
    out["n_hist"] = pub.get("hybrid_n_hist", "-")
    return out


def mean_se(xs):
    m = statistics.mean(xs)
    se = statistics.stdev(xs) / math.sqrt(len(xs)) if len(xs) > 1 else float("inf")
    return m, se


def paired_se(a, b):
    """SE of mean(b) - mean(a): paired by block when both have every block, else unpaired."""
    if len(a) == len(b) and len(a) > 1:
        return mean_se([y - x for x, y in zip(a, b)])[1]
    return math.sqrt(mean_se(a)[1] ** 2 + mean_se(b)[1] ** 2)


def judge(cells, arms, reliable_cells):
    """cells: {cell: {arm: [values]}} -> (lines, steps {cell: {arm: UP/DOWN/FLAT/None}})."""
    out, steps = [], {}
    base = arms[0]
    for cell in sorted(cells):
        by_arm = cells[cell]
        out.append(f"=== {cell} ({'RELIABLE' if cell in reliable_cells else 'BEST_EFFORT'}) ===")
        for arm in arms + ["BASE2"]:
            rows = by_arm.get(arm, [])
            if not rows:
                out.append(f"  {arm:8s} no usable run")
                continue
            shown = " ".join(f"{m} {statistics.mean(r[m] for r in rows):.1f}" for m in METRICS + TREATMENT
                             if all(m in r for r in rows))
            out.append(f"  {arm:8s} n={len(rows)} {shown} n_hist={rows[0]['n_hist']}")
        steps[cell] = {}
        aa = None
        if len(by_arm.get(base, [])) >= MIN_RUNS and len(by_arm.get("BASE2", [])) >= MIN_RUNS:
            aa = abs(statistics.mean(r[PRIMARY] for r in by_arm["BASE2"]) -
                     statistics.mean(r[PRIMARY] for r in by_arm[base]))
        for prev, arm in zip(arms, arms[1:]):
            a = [r[PRIMARY] for r in by_arm.get(prev, [])]
            b = [r[PRIMARY] for r in by_arm.get(arm, [])]
            if len(a) < MIN_RUNS or len(b) < MIN_RUNS or aa is None:
                out.append(f"  STEP {prev} -> {arm}: NOT READ (runs {len(a)}/{len(b)}, A/A {aa})")
                steps[cell][arm] = None
                continue
            d = statistics.mean(b) - statistics.mean(a)
            floor = max(aa, 2 * paired_se(a, b))
            verdict = "UP" if d > floor else "DOWN" if d < -floor else "FLAT"
            steps[cell][arm] = verdict
            out.append(f"  STEP {prev} -> {arm}: {PRIMARY} d {d:+.1f} ({100 * d / statistics.mean(a):+.2f}%) "
                       f"floor {floor:.1f} (A/A {aa:.1f}) -> {verdict}")
    return out, steps


def culprits(steps, arms, reliable_cells):
    be = [c for c in steps if c not in reliable_cells]
    rel = [c for c in steps if c in reliable_cells]
    lines, unread = [], False
    named = []
    for arm in arms[1:]:
        verdicts = [steps[c].get(arm) for c in be]
        if any(v is None for v in verdicts):
            unread = True
            continue
        if be and all(v == "UP" for v in verdicts):
            also = [c for c in rel if steps[c].get(arm) == "UP"]
            named.append(f"{arm}{' (also RELIABLE: ' + ','.join(also) + ')' if also else ''}")
    if named:
        lines.append("TX GAP CULPRIT: " + "; ".join(named))
    else:
        lines.append("TX GAP CULPRIT: NOT LOCALISED on the PC" + (" (some steps not read)" if unread else ""))
    return lines, unread


def read(lines):
    header, arms, runs = parse_lines(lines)
    if header is None:
        return ["no tx_gap_pc session"], 2
    out = [header]
    cells, reliable_cells = {}, set()
    for head, pub, sub in runs:
        why = void_reason(head, pub, sub)
        if why:
            out.append(f"VOID {head['cell']} {head['arm']} block {head['block']}: {why}")
            continue
        if int(sub["reliable"]):
            reliable_cells.add(head["cell"])
        cells.setdefault(head["cell"], {}).setdefault(head["arm"], []).append(values(head, pub))
    judged, steps = judge(cells, arms, reliable_cells)
    out += judged
    named, unread = culprits(steps, arms, reliable_cells)
    out += named
    return out, 2 if unread or not steps else 0


def selftest():
    def line(cell, arm, block, iu, rel=0, waits=10):
        return (f"RUN cell={cell} arm={arm} block={block} mode=perf insn_u={iu} insn_k=1000000 cyc_u=1 cyc_k=1 "
                f"task_ms=10.0 ctx=5 pub_empty_reads=1 | role=pub size=1048576 published=100 large_send_waits={waits} "
                f"tx_datagrams=2300 hybrid_runs=2300 hybrid_n_hist=0/0/0/0/0/0/100 | role=sub size=1048576 "
                f"reliable={rel} delivered=100 torn=0 out_of_order=0")
    hdr = "=== tx_gap_pc x tip=1 arms: M=aaa G1=bbb G2=ccc BASE2=aaa pub_flags='-F auto' blocks=3"

    def session(g2_be_cost):
        lines = [hdr]
        for b in (1, 2, 3):
            noise = b * 1000
            for cell, rel in (("be1m", 0), ("be4m", 0), ("rel1m", 1)):
                lines.append(line(cell, "M", b, 5000000 + noise, rel))
                lines.append(line(cell, "BASE2", b, 5000000 + noise + 500, rel))
                lines.append(line(cell, "G1", b, 5000000 + noise + 200, rel))
                lines.append(line(cell, "G2", b, (5000000 if rel else 5000000 + g2_be_cost) + noise + 200, rel))
        return lines

    lines = session(300000)  # G2 costs 5% in BE only - the culprit
    out, rc = read(lines)
    text = "\n".join(out)
    assert rc == 0, text
    assert "TX GAP CULPRIT: G2" in text and "also RELIABLE" not in text, text
    assert "STEP M -> G1: insn_per_mb" in text and "-> FLAT" in text, text
    # control: no step moves -> not localised
    out, rc = read(session(0))
    assert "NOT LOCALISED" in "\n".join(out), "\n".join(out)
    # a void run (BE below 98%) leaves G2 with too few runs in that cell -> not read
    voided = [ln.replace("delivered=100", "delivered=50") if " arm=G2 " in ln and "be1m" in ln and "block=1" in ln
              else ln for ln in lines]
    voided = [ln.replace("delivered=100", "delivered=50") if " arm=G2 " in ln and "be1m" in ln and "block=2" in ln
              else ln for ln in voided]
    out, rc = read(voided)
    assert rc == 2 and "NOT READ" in "\n".join(out), "\n".join(out)
    print("SELFTEST OK")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        selftest()
        sys.exit(0)
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    with open(sys.argv[1], encoding="utf-8") as f:
        result, code = read(f.read().splitlines())
    print("\n".join(result))
    sys.exit(code)
