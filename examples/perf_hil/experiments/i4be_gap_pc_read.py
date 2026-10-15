#!/usr/bin/env python3
"""i4be_gap_pc_read.py - reads i4be_gap_pc.sh's results by the rules in its header, written before the first run.

Usage: i4be_gap_pc_read.py RESULTS   (the last "=== i4be_gap_pc" session in the file)
       i4be_gap_pc_read.py --selftest
Per arm: mean and standard error of every metric per sample. Then, per QoS, every rmw_tickle build against the first
one named in ARMS (B - A, 2 SE, and the floor: the larger of 2 SE and the fdds arm's own max-min spread in that QoS),
and fdds:be against fdds:rel (the control). Exit 0 when every comparison could be read, 2 otherwise.
"""
import math
import statistics
import sys

METRICS = ("sub_work_ms", "sub_main_ms", "sub_ms", "pub_ms", "sub_kcyc", "sub_ucyc", "pub_kcyc", "pub_ucyc", "sub_stime_ms", "sub_utime_ms",
           "pub_stime_ms", "pub_utime_ms")
MIN_RUNS = 2


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def parse(lines):
    starts = [i for i, l in enumerate(lines) if l.startswith("=== i4be_gap_pc")]
    if not starts:
        return None, []
    runs = []
    for line in lines[starts[-1]:]:
        if not line.startswith("RUN "):
            continue
        head = line.split(" | ")[0]
        kv = dict(t.split("=", 1) for t in head.split()[1:] if "=" in t)
        sent, recv = num(kv.get("sent")), num(kv.get("recv"))
        if kv.get("ok") != "1":
            print(f"VOID {kv.get('arm')} rep {kv.get('rep')}: maps did not show the arm's library")
            continue
        if not sent or not recv or recv < 0.98 * (sent - 15):
            print(f"VOID {kv.get('arm')} rep {kv.get('rep')}: sent={sent} recv={recv}")
            continue
        r = {"arm": kv["arm"]}
        tc_s, tc_p = num(kv.get("sub_task-clock")), num(kv.get("pub_task-clock"))
        if tc_s is None or tc_p is None:
            print(f"VOID {kv.get('arm')} rep {kv.get('rep')}: no perf stat counts")
            continue
        r["sub_ms"] = tc_s / recv
        # perf_test's main thread spins on its stats lock (i4be_gap_pc.sh); every other thread is the receive work
        mt, wt = num(kv.get("sub_main_ticks")), num(kv.get("sub_work_ticks"))
        r["sub_main_ms"] = mt * 10.0 / recv if mt is not None else None
        r["sub_work_ms"] = wt * 10.0 / recv if wt is not None else None
        r["pub_ms"] = tc_p / sent
        for role, n in (("sub", recv), ("pub", sent)):
            for k in ("u", "k"):
                v = num(kv.get(f"{role}_cycles_{k}"))
                r[f"{role}_{k}cyc"] = v / n / 1e3 if v is not None else None  # thousand cycles per sample
            for k in ("utime", "stime"):
                v = num(kv.get(f"{role}_{k}"))
                r[f"{role}_{k}_ms"] = v * 1e3 / n if v is not None else None
        runs.append(r)
    return lines[starts[-1]], runs


def stats(vals):
    vals = [v for v in vals if v is not None]
    if len(vals) < MIN_RUNS:
        return None
    m = statistics.mean(vals)
    se = statistics.stdev(vals) / math.sqrt(len(vals))
    return m, se, max(vals) - min(vals), len(vals)


def compare(by, a, b, metric):
    sa, sb = stats([r[metric] for r in by.get(a, [])]), stats([r[metric] for r in by.get(b, [])])
    if not sa or not sb:
        return None
    d = sb[0] - sa[0]
    se = math.sqrt(sa[1] ** 2 + sb[1] ** 2)
    return d, se, sa[0]


def read(lines, out=print):
    header, runs = parse(lines)
    if header is None:
        out("no i4be_gap_pc session")
        return 2
    out(header)
    by = {}
    for r in runs:
        by.setdefault(r["arm"], []).append(r)
    out(f"{'arm':12}" + "".join(f"{m:>22}" for m in METRICS))
    for arm in sorted(by):
        cells = []
        for m in METRICS:
            s = stats([r[m] for r in by[arm]])
            cells.append(f"{s[0]:12.3f} ±{s[1]:7.3f}" if s else f"{'-':>22}")
        out(f"{arm:12}" + "".join(f"{c:>22}" for c in cells) + f"  n={len(by[arm])}")
    rc = 0
    order = []
    for line in lines:
        if line.startswith("=== i4be_gap_pc") and "arms='" in line:
            order = line.split("arms='")[1].split("'")[0].split()
    names = []
    for a in order:
        n = a.split(":")[0]
        if n != "fdds" and n not in names:
            names.append(n)
    for q in [q for q in ("be", "rel") if any(a.endswith(":" + q) for a in order)]:
        spread = {}
        for m in METRICS:
            s = stats([r[m] for r in by.get(f"fdds:{q}", [])])
            spread[m] = s[2] if s else 0.0
        for n in names[1:]:
            for m in ("sub_work_ms", "sub_ms", "pub_ms", "sub_kcyc", "pub_kcyc"):
                c = compare(by, f"{names[0]}:{q}", f"{n}:{q}", m)
                if c is None:
                    out(f"{q} {n} vs {names[0]} {m}: not read (fewer than {MIN_RUNS} runs)")
                    rc = 2
                    continue
                d, se, base = c
                floor = max(2 * se, spread[m])
                v = "BETTER" if d < -floor else "WORSE" if d > floor else "HELD"
                out(f"{q} {n} vs {names[0]} {m}: {d:+.3f} ({100 * d / base:+.2f}%) 2SE {2 * se:.3f} floor {floor:.3f} {v}")
    for m in ("sub_work_ms", "sub_ms", "pub_ms", "sub_kcyc", "pub_kcyc"):
        c = compare(by, "fdds:rel", "fdds:be", m)
        if c:
            d, se, base = c
            out(f"control fdds be vs rel {m}: {d:+.3f} ({100 * d / base:+.2f}%) 2SE {2 * se:.3f}")
    return rc


def selftest():
    hdr = "=== i4be_gap_pc 2026 reps=2 dur=20s arms='A:be B:be fdds:be' pf=x profile=0 load=1 ==="

    def run(arm, rep, sub, ok=1, recv=300):
        return (f"RUN arm={arm} rep={rep} ok={ok} sent=300 recv={recv} sub_task-clock={sub} sub_cycles_u=1e9 "
                f"sub_cycles_k=1e9 pub_task-clock=3000 pub_cycles_u=1e9 pub_cycles_k=1e9 sub_utime=1 sub_stime=1 "
                f"pub_utime=1 pub_stime=1 sub_main_ticks=100 sub_work_ticks={sub // 20} | x")
    lines = [hdr, run("A:be", 1, 3000), run("A:be", 2, 3010), run("B:be", 1, 2700), run("B:be", 2, 2710),
             run("fdds:be", 1, 2800), run("fdds:be", 2, 2830), run("B:be", 3, 1, ok=0), run("A:be", 3, 1, recv=10)]
    got = []
    assert read(lines, got.append) == 0, got
    assert any("be B vs A sub_ms" in l and "BETTER" in l for l in got), got
    assert sum(1 for l in got if l.startswith("VOID")) == 0  # VOIDs print through print(), not out
    _, runs = parse(lines)
    assert len(runs) == 6, runs  # the not-ok run and the 10-of-300 run are void
    lines2 = [hdr, run("A:be", 1, 3000), run("A:be", 2, 3010), run("B:be", 1, 3005), run("B:be", 2, 3001),
              run("fdds:be", 1, 2800), run("fdds:be", 2, 2830)]
    got = []
    read(lines2, got.append)
    assert any("be B vs A sub_ms" in l and "HELD" in l for l in got), got
    print("selftest ok")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    with open(sys.argv[1]) as fh:
        sys.exit(read(fh.read().splitlines()))
