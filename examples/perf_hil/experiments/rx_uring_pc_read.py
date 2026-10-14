#!/usr/bin/env python3
"""rx_uring_pc_read.py - reads rx_uring_pc.sh's results by the rules written in its header before the first run.

Usage: rx_uring_pc_read.py RESULTS   (LARGE from RESULTS, PING from RESULTS.ping; the last session of each)
       rx_uring_pc_read.py --selftest
Arms: the first is the control A (its A/A twin is BASE2), then B (the ring), then BASE (the branch's parent).
Exit 0 when a verdict was reached, 2 when the PING part or a RELIABLE cell could not be read.
"""
import math
import os
import statistics
import sys

PRIMARY = "insn_per_mb"
LARGE_METRICS = (PRIMARY, "insn_u_per_mb", "insn_k_per_mb", "cyc_per_mb")
OLD_WAIT = ("epoll_pwait2", "recvfrom", "recvmsg")  # the calls the ring replaces (the design's MECHANISM rule)
MIN_RUNS = 2
ENGAGED_SHARE = 0.10


def kv(text):
    return dict(item.split("=", 1) for item in text.split() if "=" in item)


def last_session(lines, marker):
    starts = [i for i, line in enumerate(lines) if line.startswith(marker)]
    return (lines[starts[-1]], lines[starts[-1]:]) if starts else (None, [])


def arm_order(header):
    """The arm labels in the order the header lists them, BASE2 (the A/A twin of the first) last."""
    arms = []
    for item in header.split(" arms:")[1].split():
        label = item.split("=", 1)[0]
        if "=" not in item or not label[:1].isupper():  # the arms' labels are capitalised; the fields after are not
            break
        arms.append(label)
    return arms


def mean_se(xs):
    m = statistics.mean(xs)
    se = statistics.stdev(xs) / math.sqrt(len(xs)) if len(xs) > 1 else float("inf")
    return m, se


def paired_se(a, b):
    """SE of mean(b) - mean(a): paired by block when both have every block, else unpaired."""
    if len(a) == len(b) and len(a) > 1:
        return mean_se([y - x for x, y in zip(a, b)])[1]
    return math.sqrt(mean_se(a)[1] ** 2 + mean_se(b)[1] ** 2)


def compare(a, b, aa, lower_is_better=True):
    """(d, floor, verdict) of mean(b) - mean(a) against max(A/A, 2 SE)."""
    d = statistics.mean(b) - statistics.mean(a)
    floor = max(aa, 2 * paired_se(a, b))
    if d > floor:
        return d, floor, "WORSE" if lower_is_better else "BETTER"
    if d < -floor:
        return d, floor, "BETTER" if lower_is_better else "WORSE"
    return d, floor, "HELD"


# ---- LARGE ---------------------------------------------------------------------------------------------------------

def large_void(head, pub, sub):
    published, delivered = int(pub["published"]), int(sub["delivered"])
    if int(sub["torn"]) or int(sub["out_of_order"]):
        return "torn/out_of_order"
    if int(sub["reliable"]) and delivered < published:
        return f"RELIABLE delivered {delivered} < published {published}"
    if not int(sub["reliable"]) and delivered < 0.98 * published:
        return f"BEST_EFFORT delivered {delivered} < 98% of {published}"
    if head.get("mode") == "perf":
        for key in ("insn_u", "insn_k", "cyc_u", "cyc_k"):
            if not head.get(key, "none").isdigit():
                return f"no perf count {key}"
    if delivered == 0:
        return "nothing delivered"
    return None


def large_parse(lines):
    header, session = last_session(lines, "=== rx_uring_pc")
    if header is None:
        return None, {}, {}, []
    perf, strace, notes = {}, {}, []
    for line in session:
        if not line.startswith("RUN "):
            continue
        parts = line.split(" | ")
        head = kv(parts[0])
        if len(parts) < 3:
            notes.append(f"VOID {head.get('cell')} {head.get('arm')} block {head.get('block')}: no RESULT")
            continue
        pub, sub = kv(parts[1]), kv(parts[2])
        why = large_void(head, pub, sub)
        if why:
            notes.append(f"VOID {head['cell']} {head['arm']} block {head['block']} {head['mode']}: {why}")
            continue
        mb = int(sub["delivered"]) * int(sub["size"]) / 1e6
        if head["mode"] == "perf":
            iu, ik = int(head["insn_u"]), int(head["insn_k"])
            row = {PRIMARY: (iu + ik) / mb, "insn_u_per_mb": iu / mb, "insn_k_per_mb": ik / mb,
                   "cyc_per_mb": (int(head["cyc_u"]) + int(head["cyc_k"])) / mb,
                   "latency_median_us": float(sub.get("latency_median_us", "nan"))}
            for key in ("uring_waits", "uring_recvs", "uring_fallbacks", "direct_reads", "empty_reads"):
                if head.get(key, "").isdigit():
                    row[key + "_per_mb"] = int(head[key]) / mb
            perf.setdefault(head["cell"], {}).setdefault(head["arm"], []).append(row)
        else:
            calls = {}
            for key, value in head.items():
                if key.startswith("sc_") and value.split("/")[0].isdigit():  # strace's header row is not a call
                    calls[key[3:]] = int(value.split("/")[0]) / mb
            calls["total"] = sum(v for k, v in calls.items())
            strace[head["cell"]] = strace.get(head["cell"], {})
            strace[head["cell"]][head["arm"]] = calls
    return header, perf, strace, notes


def large_read(header, perf, strace):
    arms = arm_order(header)
    a_arm = arms[0]
    out, worse_rel, unread_rel, worse_any = [], 0, 0, 0
    reliable = [c for c in perf if c.startswith("rel")]
    for cell in sorted(perf, key=lambda c: (c[-2:], c)):
        by_arm = perf[cell]
        out.append(f"=== LARGE {cell} ===")
        for arm in arms:
            rows = by_arm.get(arm, [])
            if not rows:
                out.append(f"  {arm:6s} no usable run")
                continue
            keys = [k for k in rows[0] if all(k in r for r in rows)]
            out.append(f"  {arm:6s} n={len(rows)} " + " ".join(f"{k} {statistics.mean(r[k] for r in rows):.1f}"
                                                              for k in keys))
        a, aa_rows = by_arm.get(a_arm, []), by_arm.get("BASE2", [])
        for b_arm in [x for x in arms if x not in (a_arm, "BASE2")]:
            b = by_arm.get(b_arm, [])
            if len(a) < MIN_RUNS or len(b) < MIN_RUNS or len(aa_rows) < MIN_RUNS:
                out.append(f"  {b_arm} - {a_arm}: NOT READ (runs A {len(a)}, {b_arm} {len(b)}, A/A {len(aa_rows)})")
                unread_rel += cell in reliable and b_arm == "B"
                continue
            for metric in LARGE_METRICS:
                aa = abs(statistics.mean(r[metric] for r in aa_rows) - statistics.mean(r[metric] for r in a))
                d, floor, verdict = compare([r[metric] for r in a], [r[metric] for r in b], aa)
                pct = 100 * d / statistics.mean(r[metric] for r in a)
                out.append(f"  {b_arm} - {a_arm} {metric}: d {d:+.1f} ({pct:+.2f}%) floor {floor:.1f} (A/A {aa:.1f}) "
                           f"-> {verdict}")
                if b_arm == "B" and metric == PRIMARY and verdict == "WORSE":
                    worse_any += 1
                    worse_rel += cell in reliable
            calls = strace.get(cell, {})
            if a_arm in calls and b_arm in calls:
                ca, cb = calls[a_arm], calls[b_arm]
                names = sorted(set(ca) | set(cb), key=lambda n: -(ca.get(n, 0) + cb.get(n, 0)))
                out.append(f"  {b_arm} syscalls/MB " + " ".join(f"{n} {ca.get(n, 0):.0f}->{cb.get(n, 0):.0f}"
                                                                for n in names if ca.get(n, 0) + cb.get(n, 0) >= 0.5))
                old_a = sum(ca.get(n, 0) for n in OLD_WAIT)
                old_b = sum(cb.get(n, 0) for n in OLD_WAIT)
                share = old_b / old_a if old_a else float("inf")
                ring = cb.get("io_uring_enter", 0) > 0
                mech = "ENGAGED" if ring and share <= ENGAGED_SHARE else "ENGAGED-PART" if ring else "NOT ENGAGED"
                out.append(f"  {b_arm} MECHANISM {mech}: io_uring_enter/MB {cb.get('io_uring_enter', 0):.0f}, "
                           f"epoll_pwait2+recvfrom+recvmsg/MB {old_a:.0f} -> {old_b:.0f} ({100 * share:.0f}% of A)")
            else:
                out.append(f"  {b_arm} MECHANISM not read (no strace run of A and {b_arm})")
    return out, worse_rel, unread_rel, worse_any, len(reliable)


# ---- PING ----------------------------------------------------------------------------------------------------------

def ping_parse(lines):
    header, session = last_session(lines, "=== lat_gap_pc")
    if header is None:
        return None, {}, {}
    perf, strace = {}, {}
    for line in session:
        if not line.startswith("RUN ") or " VOID " in line:
            continue
        head = kv(line.split(" | ")[0].replace("per_rtt:", ""))
        sent = int(head.get("sent", "0"))
        if sent == 0:
            continue
        if head.get("mode") == "perf":
            perf.setdefault(head["arm"], []).append({"insn_k_rt": int(head["insn_k"]) / sent,
                                                     "insn_u_rt": int(head["insn_u"]) / sent})
        elif head.get("mode") == "strace":
            calls = {k: float(v.split("/")[0]) for k, v in head.items()
                     if k not in ("arm", "block", "mode", "sent") and "/" in v}
            calls["total"] = sum(calls.values())
            strace[head["arm"]] = calls
    return header, perf, strace


def ping_read(header, perf, strace):
    arms = arm_order(header)
    a_arm = arms[0]
    out, regressions, unread = [], [], False
    out.append("=== PING (client, per round trip) ===")
    for arm in arms:
        rows = perf.get(arm, [])
        calls = strace.get(arm, {})
        shown = " ".join(f"{n} {calls[n]:.2f}" for n in sorted(calls, key=lambda n: -calls[n]) if calls[n] >= 0.01)
        if rows:
            out.append(f"  {arm:6s} n={len(rows)} insn_k/rt {statistics.mean(r['insn_k_rt'] for r in rows):.0f} "
                       f"insn_u/rt {statistics.mean(r['insn_u_rt'] for r in rows):.0f} | syscalls/rt {shown}")
    aa_rows = perf.get("BASE2", [])
    pairs = [(a_arm, "B")] + ([("BASE", "B")] if "BASE" in arms else []) + \
        [(a_arm, x) for x in arms if x not in (a_arm, "B", "BASE", "BASE2")]
    for ref, arm in pairs:
        decides = arm == "B"
        a, b = perf.get(ref, []), perf.get(arm, [])
        if len(a) < MIN_RUNS or len(b) < MIN_RUNS or len(aa_rows) < MIN_RUNS:
            out.append(f"  {arm} vs {ref}: NOT READ")
            unread = unread or decides
            continue
        for metric in ("insn_k_rt", "insn_u_rt"):
            aa = abs(statistics.mean(r[metric] for r in aa_rows) - statistics.mean(r[metric] for r in perf[a_arm]))
            d, floor, verdict = compare([r[metric] for r in a], [r[metric] for r in b], aa)
            pct = 100 * d / statistics.mean(r[metric] for r in a)
            out.append(f"  {arm} - {ref} {metric}: d {d:+.0f} ({pct:+.2f}%) floor {floor:.0f} -> {verdict}")
            if verdict == "WORSE" and decides:
                regressions.append(f"{metric} vs {ref}")
        ta, tb = strace.get(ref, {}).get("total"), strace.get(arm, {}).get("total")
        if ta is None or tb is None:
            out.append(f"  {arm} vs {ref}: system calls not read")
            unread = unread or decides
        else:
            out.append(f"  {arm} - {ref} system calls/rt: {tb - ta:+.2f} ({ta:.2f} -> {tb:.2f})")
            if tb > ta + 0.005 and decides:
                regressions.append(f"system calls vs {ref}")
    return out, regressions, unread


def main(path):
    large_lines, ping_lines = [], []
    if os.path.exists(path):  # PARTS=ping writes no LARGE file
        with open(path) as f:
            large_lines = f.read().splitlines()
    if os.path.exists(path + ".ping"):
        with open(path + ".ping") as f:
            ping_lines = f.read().splitlines()
    status = 0
    header, perf, strace, notes = large_parse(large_lines)
    worse_rel = worse_any = unread_rel = n_rel = 0
    if header is not None:
        print(header)
        for note in notes:
            print(note)
        lines, worse_rel, unread_rel, worse_any, n_rel = large_read(header, perf, strace)
        print("\n".join(lines))
    pheader, pperf, pstrace = ping_parse(ping_lines)
    regressions, punread = [], True
    if pheader is not None:
        print(pheader)
        lines, regressions, punread = ping_read(pheader, pperf, pstrace)
        print("\n".join(lines))
    if punread or unread_rel:
        print("VERDICT: NOT READ - the ping-pong or a RELIABLE cell could not be read")
        status = 2
    elif regressions:
        print("VERDICT: REFUTED on the PC - ping-pong regression: " + ", ".join(regressions))
    elif n_rel and worse_rel == n_rel:
        print("VERDICT: REFUTED on the PC - WORSE on instructions per MB in every RELIABLE cell")
    elif worse_any == 0:
        print("VERDICT: SUPPORTED on the PC - no ping-pong regression, no large cell WORSE")
    else:
        print(f"VERDICT: MIXED - no ping-pong regression, {worse_any} large cell(s) WORSE")
    return status


def selftest():
    a = [100.0, 101.0, 99.0]
    assert compare(a, [90.0, 91.0, 89.0], 0.5)[2] == "BETTER"
    assert compare(a, [110.0, 111.0, 109.0], 0.5)[2] == "WORSE"
    assert compare(a, [100.5, 100.0, 99.5], 0.5)[2] == "HELD"
    assert compare(a, [101.0, 102.0, 100.0], 5.0)[2] == "HELD"  # inside the A/A floor
    head = "=== rx_uring_pc x tip=1 arms: A=1[TT_RX_URING_WAIT=0] B=1 BASE=2 BASE2=1 pub=1 pub_flags=''"
    assert arm_order(head) == ["A", "B", "BASE", "BASE2"]
    pub = {"published": "100", "size": "1048576"}
    assert large_void({"mode": "strace"}, pub, {"delivered": "99", "torn": "0", "out_of_order": "0",
                                                  "reliable": "1"})
    assert large_void({"mode": "strace"}, pub, {"delivered": "99", "torn": "0", "out_of_order": "0",
                                                  "reliable": "0"}) is None
    ping = ["=== lat_gap_pc t arms: A=1[X] B=1 BASE=2 BASE2=1 blocks=3",
            "RUN arm=A block=1 mode=perf sent=100 insn_u=1000 insn_k=2000 cyc_u=1 cyc_k=1 | RESULT",
            "RUN arm=A block=2 mode=perf sent=100 insn_u=1010 insn_k=2010 cyc_u=1 cyc_k=1 | RESULT",
            "RUN arm=B block=1 mode=perf sent=100 insn_u=1000 insn_k=3000 cyc_u=1 cyc_k=1 | RESULT",
            "RUN arm=B block=2 mode=perf sent=100 insn_u=1010 insn_k=3010 cyc_u=1 cyc_k=1 | RESULT",
            "RUN arm=BASE block=1 mode=perf sent=100 insn_u=1000 insn_k=2000 cyc_u=1 cyc_k=1 | RESULT",
            "RUN arm=BASE block=2 mode=perf sent=100 insn_u=1010 insn_k=2010 cyc_u=1 cyc_k=1 | RESULT",
            "RUN arm=BASE2 block=1 mode=perf sent=100 insn_u=1000 insn_k=2000 cyc_u=1 cyc_k=1 | RESULT",
            "RUN arm=BASE2 block=2 mode=perf sent=100 insn_u=1010 insn_k=2011 cyc_u=1 cyc_k=1 | RESULT",
            "RUN arm=A block=s mode=strace sent=100 per_rtt: recvfrom=2.000/1.000 epoll_pwait2=2.000/0.000 ",
            "RUN arm=B block=s mode=strace sent=100 per_rtt: io_uring_enter=3.000/0.000 recvfrom=1.000/1.000 ",
            "RUN arm=BASE block=s mode=strace sent=100 per_rtt: recvfrom=2.000/1.000 epoll_pwait2=2.000/0.000 "]
    h, p, s = ping_parse(ping)
    _, regressions, unread = ping_read(h, p, s)
    assert not unread and "insn_k_rt vs A" in regressions and "insn_k_rt vs BASE" in regressions, regressions
    assert not any("system calls" in r for r in regressions), regressions
    print("rx_uring_pc_read selftest: PASS")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
