#!/usr/bin/env python3
"""Creation-time latency of the rmw perf_test cells, from the creation traces (rmw_keepall_rig.sh CREATION_TRACE=1).

Why (the user's decision of 2026-10-17): latency = the receiver's arrival time - the data's CREATION time. In a fixed-rate
cell the creation time is the SCHEDULED publish time, start + k / rate, which does not slip when the send loop falls
behind (the coordinated-omission correction). The latency measured from the sender's own stamp stays as the secondary,
diagnostic figure. perf_test stamps a sample just before its publish() (perf_test_creation_trace.py, docstring); a DDS
publish() of a 4 MB sample blocks until the sample is in the socket, so a publisher that is behind stamps its next
sample only after that wait, and the wait is in no sample's stamp latency. rmw_tickle's publish does not block and its
next sample waits inside the rmw, inside its stamp latency. docs/RESULTS.md, L4 table, I4s note.

Usage:
  creation_latency_summary.py <runs dir> [DUR] [--control <round-trip runs dir>]   one rmw cell family (one
        rmw_keepall_rig.sh OUT), read by the rules below; with --control, that control is read first and its verdict
        gates this one's creation figures
  creation_latency_summary.py --rtt <campaign_sweep.sh output .txt>               the core RTT cells (reliable_latency),
        whose clients print crt_* (creation) beside rtt_* (stamp) since 2026-10-17
  creation_latency_summary.py --pc-check <creation_latency_pc.sh OUT>              that dry run's P1-P4
  creation_latency_summary.py --selftest                                          synthetic traces with known answers

PER SAMPLE (one-way cells). From the publisher's trace, P <stamp> <gap_ns> <id>: its schedule is stamp - gap (on
  perf_test's clock). From the subscriber's, S <stamp> <lat_ns> <realtime>: joined to P by the stamp. stamp latency =
  lat_ns - offset(realtime), offset = the probe's server - client clock interpolated in time (largemsg_rig_summary.py
  clock_model, the same model and rules as the L4 table); creation latency = stamp latency + gap. In a round-trip run
  (ROUNDTRIP=1) both records are the Main's, one clock, no offset: the round trip from the schedule and from the stamp.

HOW TO READ IT, written 2026-10-17 before any rig run and enforced below:
  R0 IDENTITY and TRACE. A run is VOID when its maps do not show the arm's rmw (as largemsg_rig_summary.py), when the
     publisher's trace is missing or has no P line with a gap ("no trace" is never read as "no wait": a perf_test
     without perf_test_creation_trace.py writes nothing), when the trace dropped lines (max_lines), or when any S line
     does not join a P line (INSTRUMENT FAULT: an arrival with no known schedule).
  R1 SAMPLE COUNT FIRST. Per run: offered ticks in the window, created (published) ones, delivered ones. A median is
     read only with >= 50 offered ticks, a p99 only with >= 500 (at least 5 above it); else NOT READ. Printed before
     any value, with the smallest loss rate n samples would show at 95%, 1 - 0.05^(1/n).
  R2 WINDOW AND EDGES. The window is on SCHEDULED times, the same rule for every arm: from 3 s after the first
     schedule to 2 s before the publisher's last stamp (it runs --max_runtime DUR and stops). Offered = every tick
     of the grid in the window, rate x window, the same for every framework (the offered denominator, the user's rule
     of 2026-10-12 for saturating cells). A tick in the window that was never published by the end (a blocking
     publisher that fell behind) or never delivered counts as not delivered, latency +inf: its latency is at least
     the time left to the end of the run, and a percentile that lands on such a tick reads UNBOUNDED (inf), worse
     than any finite value. So the creation percentiles are over OFFERED ticks; the stamp percentiles, the old
     definition, over the delivered samples only (as perf_test computes them).
     In a saturating cell (offered > what the link carries) a publisher that blocks falls further behind every tick
     and its creation latency grows without bound: its value depends on the window's length. Printed per run: the
     growth across the window (least-squares slope x window length, delivered samples) and the window length. Arms
     are compared only when their windows agree within 1 s, else NOT COMPARABLE. In a cell that is not saturating, a
     growth above max(1 ms, 10% of the median) is flagged GROWING (the sender falls behind where it should not).
     Saturating = offered bytes x 8 x 1538/1452 per second above the link (1 Gb/s on the rig; --link-mbps or meta
     link_mbps for a shaped PC link).
  R3 THE TREATMENT APPLIED. Per run the realized gap (stamp - schedule): median, p99, max, share > 1 ms; and the grid
     check: the recorded schedules must lie on one fixed grid, schedule = a + id / rate (least squares over id >= 4;
     slope within 0.1% of 1 / rate and every residual within 0.5 ms) - a schedule that slipped with the send loop
     would be the old stamp under a new name. A run failing the grid check is VOID. In a saturating cell every arm
     whose publish blocks (fastdds*, cyclonedds, tickle@block) must show a median gap above one period; one that does
     not means the trace did not see the wait the delivered counts show: the cell is VOID (instrument).
  R4 CONTROL (a round-trip run dir, ROUNDTRIP=1, or --control): the ping-pong, where nothing waits at the sender, must
     read the same on both definitions within its noise. Per arm, >= 2 reps of >= 200 round trips; d = median over
     reps of (median creation RTT - median stamp RTT), g = median realized gap, noise = range of the reps' median stamp
     RTTs. Because creation = stamp + gap per sample, d is g up to how the medians fall; the control's teeth are that
     nothing waits: PASS when g <= 0.2 ms (a timer wake, not a sample waiting behind another), no gap below -0.02 ms
     (a stamp before its schedule is a mis-read schedule), every grid check passes and |d - g| <= noise. Printed
     beside: whether |d| <= noise literally ("same within noise"), else "differs by the sender's timer wake g".
     The core --rtt reading allows |d - g| <= noise + 1 us (RTT_PRINT_RES_MS): its d is the difference of two
     medians the clients print to 1 us. Changed AFTER the first rig run (2026-10-17, the user agreed): c16 and c17
     TickLE read |d - g| = 1.1 us against a noise of 1.0 us, below what the printed figures can resolve.
     Since 2026-10-18 the core clients also print rtt_p50_us and crt_p50_us to 0.1 us (CreationTime.h), and when
     every rep of a framework carries both, rtt_p50s() reads those with a 0.1 us resolution term
     (RTT_PRINT_RES_FINE_MS) instead; a file without them reads exactly as before. Written after the same-host S10c
     run at 2579e822, whose TickLE reps all printed the same p50s (noise 0) and read |d - g| 1.1 us against 1 us.
     In rtt_r4 (--rtt and the same-host and rmw round-trip readers) noise is the range of the reps' stamp p50s PLUS
     the range of their creation p50s: the median of a sum is not the sum of the medians, so the creation median
     scatters between reps on its own account (rtt_r4's docstring has the reasoning and the evidence). Changed AFTER
     the same-host S7 run at 596e9c9c (2026-10-18, the user agreed): its TickLE |d - g| 1.3 us against 1 us, noise 0
     because every rep printed stamp p50 0.020 ms; NOT READ under the old rule. The trace-based control() below,
     whose medians are not rounded, keeps the stamp range alone: no run has needed more.
     A vendor arm that delivers nothing in every rep does not run there: listed, not read. The control needs
     tickle@head and at least one vendor read, else it FAILS. A FAILED control makes every creation figure of the
     gated cell NOT READ. (The same "delivered nothing" arm is excluded from R3 and R5 in a cell: a publisher whose
     samples go nowhere need not block. A subscriber that received nothing opens no trace file; its own log, every
     row received 0, is what tells that apart from a perf_test without the trace, which is VOID.)
  R5 SCORING (each cell, rmw_tickle tickle@head, and tickle@block when present, against each vendor arm): the range
     rule of docs/TESTING.md section 4 on the creation median (primary) and creation p99: WIN when every rmw_tickle rep
     is below every rep of the vendor, LOSE when every vendor rep is below every rmw_tickle rep, DRAW otherwise; two
     UNBOUNDED values are equal. The stamp median is scored the same way and printed as the secondary figure. Latency
     needs a usable clock (R0 of largemsg_rig_summary.py: probe ok, round trip < 1 ms, drift < 200 us/s, corrected
     minimum not below 95% of the wire floor); fewer than 2 usable reps on a side: "no clock".
"""
import math
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from largemsg_rig_summary import SIZES, clock_model, read_meta, wire_floor_ms  # noqa: E402
from rmw_keepall_rig_summary import INDEX_RE, loaded, rows  # noqa: E402

WARM_S, COOL_S = 3.0, 2.0
N_MED, N_P99 = 50, 500
GAP_BIG_NS = 1_000_000
GRID_SLOPE_TOL, GRID_RESID_NS = 1e-3, 500_000
CTL_REPS, CTL_N = 2, 200
CTL_WAKE_NS, CTL_NEG_NS = 200_000, -20_000
RTT_PRINT_RES_MS = 0.001  # the reliable_latency clients print rtt_*/crt_* p50 in ms to 3 decimals
RTT_PRINT_RES_FINE_MS = 0.0001  # ... and, since 2026-10-18, rtt_p50_us/crt_p50_us to 1 decimal (0.1 us)
FLOAT_EPS_MS = 1e-9  # rtt_r4: float rounding of a difference of decimal ms, not a tolerance
BLOCKING = ("fastdds", "cyclonedds", "tickle@block")
SUBJECTS = ("tickle@head", "tickle@block")
INF = float("inf")


def pct(vals, q):
    """Nearest-rank percentile of a list that may hold inf; nan when empty."""
    if not vals:
        return float("nan")
    s = sorted(vals)
    k = max(0, math.ceil(q / 100.0 * len(s)) - 1)
    return s[k]


def read_trace(path):
    """{'p': [(stamp, gap or None, id)], 's': [(stamp, lat_ns, realtime)], 'head': bool, 'end': str or None}."""
    out = {"p": [], "s": [], "head": False, "end": None}
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return None
    for line in text.splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "#":
            if "creation trace" in line:
                out["head"] = True
            elif len(f) > 1 and f[1] == "end":
                out["end"] = line
        elif f[0] == "P" and len(f) >= 4:
            out["p"].append((int(f[1]), None if f[2] == "nan" else int(f[2]), int(f[3])))
        elif f[0] == "S" and len(f) >= 4:
            out["s"].append((int(f[1]), int(f[2]), int(f[3])))
    return out


def fit(xs, ys):
    """Least squares y = a + b x: (a, b)."""
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return my, 0.0
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return my - b * mx, b


def grid_check(prs, rate):
    """(ok, (a, b) or None, text): the recorded schedules on one grid, schedule = a + id / rate (R3)."""
    pts = [(i, s - g) for s, g, i in prs if g is not None and i >= 4]
    if len(pts) < 10:
        return False, f"grid: {len(pts)} points", None
    ids = [p[0] for p in pts]
    sch = [p[1] for p in pts]
    a, b = fit(ids, sch)
    period = 1e9 / rate
    worst = max(abs(y - (a + b * x)) for x, y in zip(ids, sch))
    ok = abs(b / period - 1) <= GRID_SLOPE_TOL and worst <= GRID_RESID_NS
    return ok, (a, b), f"grid slope {b / 1e6:.4f} ms/id (1/rate {period / 1e6:.4f}), worst residual {worst / 1e3:.1f} us"


def link_mbps(meta, override):
    if override:
        return override
    if meta.get("link_mbps"):
        return float(meta["link_mbps"])
    m = re.search(r"tbf rate (\d+(?:\.\d+)?)(g|m)bit", meta.get("pf_shape", ""))
    if m:
        return float(m.group(1)) * (1000 if m.group(2) == "g" else 1)
    return 1000.0


def saturating(topic, rate, mbps):
    size = SIZES.get(topic, 0)
    return rate > 0 and size * 8 * 1538 / 1452 * rate / 1e6 > mbps


def run_record(runs, stem, fields, meta, rate, roundtrip):
    pmaps, smaps, _pk, _sk, want = fields
    arm, topic, loss_text, rep = re.match(r"(.+?)_(Array\w+|Struct\w+)_l([\d.]+)_r(\d+)$", stem).groups()
    rec = dict(arm=arm, topic=topic, loss=float(loss_text), rep=int(rep), void="", clock="-")
    why = []
    ptext = (runs / f"{stem}_pub.log").read_text(errors="replace") if (runs / f"{stem}_pub.log").exists() else ""
    header_rmw = re.search(r"RMW Implementation: (\S+)", ptext)
    vendor_by_log = not want.startswith("/") and header_rmw and want.startswith("lib" + header_rmw.group(1))
    if not loaded(want, pmaps) and not vendor_by_log:
        why.append(f"publisher did not load {want}")
    if not loaded(want, smaps):
        why.append(f"subscriber did not load {want}")
    pt = read_trace(runs / f"{stem}_pub.ctrace")
    st_ = pt if roundtrip else read_trace(runs / f"{stem}_sub.ctrace")
    if pt is None or not pt["head"]:
        why.append("no publisher trace (perf_test without the creation trace?)")
    elif not any(g is not None for _s, g, _i in pt["p"]):
        why.append("publisher trace has no scheduled sample (rate 0?)")
    if st_ is None and not roundtrip:
        # The trace opens at its first line, so a subscriber that received nothing writes no file. Only its own log
        # can tell that from a perf_test without the trace: every data row reading received 0 is "delivered nothing"
        # (every offered tick +inf), anything else is VOID.
        srows, _stext = rows(runs / f"{stem}_sub.log")
        if srows and all(r.get("received", 0) == 0 for r in srows):
            st_ = {"p": [], "s": [], "head": False, "end": None}
            rec["note"] = "subscriber received nothing (its log: received 0 in every row), so it opened no trace"
    if st_ is None:
        why.append("no subscriber trace")
    for t, who in ((pt, "pub"), (st_, "sub")):
        if t and t["end"] and not re.search(r"dropped=0\b", t["end"]):
            why.append(f"{who} trace dropped lines ({t['end']})")
    if why:
        rec["void"] = "; ".join(why)
        return rec
    prs = pt["p"]
    by_stamp = {s: (g, i) for s, g, i in prs}
    joined, unjoined, dup = {}, 0, 0
    for s, lat, rt in st_["s"]:
        if s not in by_stamp:
            unjoined += 1
        elif s in joined:
            dup += 1
        else:
            joined[s] = (lat, rt)
    if unjoined:
        rec["void"] = f"INSTRUMENT FAULT: {unjoined} arrivals join no published sample"
        return rec
    gok, line, gtext = grid_check(prs, rate)
    rec["grid_ok"], rec["grid"] = gok, gtext
    if not gok:
        rec["void"] = f"grid check failed ({gtext})"
        return rec
    sched = [(s - g, s, g, i) for s, g, i in prs if g is not None]
    t0 = min(x[0] for x in sched)
    t_end = max(s for s, _g, _i in prs)
    w0, w1 = t0 + WARM_S * 1e9, t_end - COOL_S * 1e9
    win = [x for x in sched if w0 <= x[0] <= w1]
    rec["window_s"] = (w1 - w0) / 1e9
    # Offered: every tick of the fitted grid (schedule = a + b x id) inside the window, published or not (R2).
    ga, gb = line
    rec["offered"] = max(0, math.floor((w1 - ga) / gb + 1e-3) - math.ceil((w0 - ga) / gb - 1e-3) + 1)
    rec["created"] = len(win)
    rec["dup"] = dup
    gaps = [x[2] for x in win]
    rec["gap_med"] = pct(gaps, 50) / 1e6 if gaps else float("nan")
    rec["gap_p99"] = pct(gaps, 99) / 1e6 if gaps else float("nan")
    rec["gap_max"] = max(gaps) / 1e6 if gaps else float("nan")
    rec["gap_min"] = min(gaps) / 1e6 if gaps else float("nan")
    rec["gap_big"] = 100.0 * sum(1 for g in gaps if g > GAP_BIG_NS) / len(gaps) if gaps else float("nan")
    # The clock: one-way runs subtract the probe offset at each arrival (realtime), round trips need none.
    if roundtrip:
        offset, label = (lambda t: 0.0), "one clock (round trip)"
    else:
        offset, label = clock_model(runs, stem, meta.get("samehost") == "1")
    rec["clock"] = label
    deliv = []  # (sched, creation_ms, stamp_ms)
    for sch, s, g, _i in win:
        if s in joined:
            lat, rt = joined[s]
            stamp_ms = (lat - (offset(rt) if offset else 0.0)) / 1e6
            deliv.append((sch, stamp_ms + g / 1e6, stamp_ms))
    rec["delivered"] = len(deliv)
    rec["clock_ok"] = offset is not None
    size = SIZES.get(topic, 0)
    if offset is not None and deliv and not roundtrip and meta.get("pc_preflight") != "1" and size:
        floor = wire_floor_ms(size)
        if min(d[2] for d in deliv) < 0.95 * floor:
            rec["clock_ok"] = False
            rec["clock"] = f"{label}; UNUSABLE: corrected min {min(d[2] for d in deliv):.2f} ms below the wire floor"
    crt = [d[1] for d in deliv]
    stm = [d[2] for d in deliv]
    n_off = max(rec["offered"], rec["delivered"])
    offered_vals = crt + [INF] * (n_off - len(crt))
    rec["n_offered_vals"] = n_off
    ok = rec["clock_ok"]
    rec["crt_med"] = pct(offered_vals, 50) if ok and n_off >= N_MED else float("nan")
    rec["crt_p99"] = pct(offered_vals, 99) if ok and n_off >= N_P99 else float("nan")
    rec["crt_med_deliv"] = pct(crt, 50) if ok and len(crt) >= N_MED else float("nan")
    rec["stamp_med"] = pct(stm, 50) if ok and len(stm) >= N_MED else float("nan")
    rec["stamp_p99"] = pct(stm, 99) if ok and len(stm) >= N_P99 else float("nan")
    rec["delivered_pct_offered"] = 100.0 * rec["delivered"] / n_off if n_off else float("nan")
    if ok and len(deliv) >= 10:
        a, b = fit([(d[0] - w0) / 1e9 for d in deliv], [d[1] for d in deliv])
        rec["growth_ms"] = b * rec["window_s"]
    else:
        rec["growth_ms"] = float("nan")
    return rec


def load(runs, mbps_override=None):
    meta = read_meta(runs)
    rate = float(meta.get("pub_rate", "0") or 0)
    roundtrip = meta.get("roundtrip") == "1"
    index = {}
    for line in (runs / "index.txt").read_text().splitlines():
        m = re.match(INDEX_RE, line)
        if m:
            index[m.group(1)] = m.groups()[1:]
    recs = [run_record(runs, stem, f, meta, rate, roundtrip) for stem, f in sorted(index.items())]
    return meta, rate, roundtrip, recs, link_mbps(meta, mbps_override)


def f2(x, w=8, p=2):
    if isinstance(x, float) and math.isnan(x):
        return f"{'-':>{w}}"
    if x == INF:
        return f"{'UNBOUND':>{w}}"
    return f"{x:{w}.{p}f}"


def print_runs(recs):
    print("\nper run (window on scheduled times; creation percentiles over OFFERED ticks, undelivered = +inf; stamp "
          "over delivered):")
    print(f"  {'arm':<16} {'topic':<8} {'l%':>3} {'rep':>3} {'win s':>6} {'offered':>7} {'created':>7} "
          f"{'deliv':>6} {'1-.05^(1/n)':>11} | {'crt med':>8} {'crt p99':>8} {'crt dlv':>8} | {'stamp med':>9} "
          f"{'st p99':>8} | {'gap med':>8} {'gap p99':>8} {'gap max':>8} {'>1ms%':>6} | {'growth':>8}")
    for r in recs:
        if r["void"]:
            print(f"  {r['arm']:<16} {r['topic']:<8} {r['loss']:>3.0f} {r['rep']:>3}  VOID: {r['void']}")
            continue
        n = r["delivered"]
        detect = 1 - 0.05 ** (1 / n) if n else float("nan")
        print(f"  {r['arm']:<16} {r['topic']:<8} {r['loss']:>3.0f} {r['rep']:>3} {r['window_s']:>6.1f} "
              f"{r['offered']:>7} {r['created']:>7} {n:>6} {f2(100 * detect, 10, 3)}% | {f2(r['crt_med'])} "
              f"{f2(r['crt_p99'])} {f2(r['crt_med_deliv'])} | {f2(r['stamp_med'], 9)} {f2(r['stamp_p99'])} | "
              f"{f2(r['gap_med'], 8, 3)} {f2(r['gap_p99'], 8, 3)} {f2(r['gap_max'], 8, 1)} {f2(r['gap_big'], 6, 1)} | "
              f"{f2(r['growth_ms'])}  [{r['clock']}; {r['grid']}]" + (f" NOTE: {r['note']}" if r.get("note") else ""))


def range_verdict(subj, other, lower_better=True):
    """docs/TESTING.md section 4's range rule; inf compares as worse than any finite and equal to inf."""
    if len(subj) < 2 or len(other) < 2:
        return "too few reps"
    if lower_better:
        if max(subj) < min(other):
            return "WIN"
        if max(other) < min(subj):
            return "LOSE"
    return "DRAW"


def usable(recs, arm, topic, loss, key):
    return [r[key] for r in recs if r["arm"] == arm and r["topic"] == topic and r["loss"] == loss and not r["void"]
            and not math.isnan(r[key])]


def control(recs):
    """R4: (passed, lines)."""
    lines, passed, read = [], True, []
    arms = sorted({r["arm"] for r in recs})
    for arm in arms:
        rs = [r for r in recs if r["arm"] == arm]
        good = [r for r in rs if not r["void"] and r["delivered"] >= CTL_N and not math.isnan(r["stamp_med"])]
        voids = [r for r in rs if r["void"]]
        if not voids and rs and all(r["delivered"] == 0 for r in rs) and not arm.startswith("tickle@"):
            # A vendor that delivers nothing does not run here: listed, not read, and it cannot pass or fail the
            # instrument. rmw_tickle delivering nothing is a failure.
            lines.append(f"  {arm}: delivered nothing in {len(rs)} reps - does not run here; not read")
            continue
        if len(good) < CTL_REPS:
            passed = False
            lines.append(f"  {arm}: NOT READ - {len(good)} reps with >= {CTL_N} round trips (need {CTL_REPS}); "
                         f"{len(voids)} VOID")
            continue
        d = st.median(r["crt_med_deliv"] - r["stamp_med"] for r in good)
        g = st.median(r["gap_med"] for r in good)
        meds = [r["stamp_med"] for r in good]
        noise = max(meds) - min(meds)
        gmin = min(r["gap_min"] for r in good)
        why = []
        if g * 1e6 > CTL_WAKE_NS:
            why.append(f"median gap {g * 1e3:.1f} us > {CTL_WAKE_NS / 1e3:.0f} us: something waits at the sender")
        if gmin * 1e6 < CTL_NEG_NS:
            why.append(f"a gap of {gmin * 1e3:.1f} us: a stamp before its schedule")
        if abs(d - g) > noise:
            why.append(f"|d - g| {abs(d - g) * 1e3:.1f} us > noise {noise * 1e3:.1f} us")
        same = "same within noise" if abs(d) <= noise else f"differs by the sender's timer wake g = {g * 1e3:.1f} us"
        verdict = "PASS" if not why else "FAIL (" + "; ".join(why) + ")"
        if why:
            passed = False
        read.append(arm)
        lines.append(f"  {arm}: reps {len(good)}, round trips {[r['delivered'] for r in good]}, stamp RTT medians "
                     f"{[round(m, 4) for m in meds]} ms, d = {d * 1e3:+.1f} us, g = {g * 1e3:.1f} us, noise "
                     f"{noise * 1e3:.1f} us, min gap {gmin * 1e3:+.1f} us: {same}; {verdict}")
    if "tickle@head" not in read or len(read) < 2:
        passed = False
        lines.append(f"  control read for {read or 'no arm'}: it needs tickle@head and at least one vendor")
    return passed, lines


def cells(recs, meta, rate, mbps, control_ok):
    out = []
    keys = sorted({(r["topic"], r["loss"]) for r in recs})
    for topic, loss in keys:
        rs = [r for r in recs if r["topic"] == topic and r["loss"] == loss]
        sat = saturating(topic, rate, mbps)
        out.append(f"\ncell {topic} {rate:g} Hz loss {loss:g}% qos [{meta.get('qos_args', '?')}]: "
                   f"{'SATURATING' if sat else 'not saturating'} at {mbps:g} Mb/s")
        good = [r for r in rs if not r["void"]]
        # R1: sample count first
        for arm in sorted({r["arm"] for r in rs}):
            ar = [r for r in rs if r["arm"] == arm]
            g = [r for r in ar if not r["void"]]
            out.append(f"  count {arm:<16} reps {len(g)}/{len(ar)} usable; offered {[r['offered'] for r in g]} "
                       f"created {[r['created'] for r in g]} delivered {[r['delivered'] for r in g]}")
        # R2: windows comparable
        wins = [r["window_s"] for r in good]
        if wins and max(wins) - min(wins) > 1.0:
            out.append(f"  WINDOWS NOT COMPARABLE: {min(wins):.1f}-{max(wins):.1f} s (R2)")
            continue
        if wins:
            out.append(f"  window {min(wins):.1f}-{max(wins):.1f} s on scheduled times (R2)")
        for r in good:
            if not sat and not math.isnan(r["growth_ms"]) and r["growth_ms"] > max(1.0, 0.1 * r["crt_med_deliv"]):
                out.append(f"  GROWING {r['arm']} r{r['rep']}: +{r['growth_ms']:.1f} ms across the window in a cell "
                           "that should not saturate (R2)")
            elif sat and not math.isnan(r["growth_ms"]):
                out.append(f"  growth {r['arm']} r{r['rep']}: {r['growth_ms']:+.1f} ms over {r['window_s']:.1f} s"
                           " (saturating: the value depends on the window length)")
        # R3: the treatment applied
        void_cell = False
        silent = set()
        for arm in sorted({r["arm"] for r in good}):
            gm = [r["gap_med"] for r in good if r["arm"] == arm]
            out.append(f"  treatment {arm:<16} realized gap (stamp - schedule) median per rep "
                       f"{[round(x, 3) for x in gm]} ms, p99 {[round(r['gap_p99'], 3) for r in good if r['arm'] == arm]}"
                       f" ms, share > 1 ms {[round(r['gap_big'], 1) for r in good if r['arm'] == arm]}%")
            if all(r["delivered"] == 0 for r in good if r["arm"] == arm):
                out.append(f"  {arm}: delivered nothing in every usable rep - does not run here (listed with its "
                           "numbers, not scored, not an R3 check: a publisher whose samples go nowhere need not block)")
                silent.add(arm)
            elif sat and arm.startswith(BLOCKING) and gm and st.median(gm) * 1e6 <= 1e9 / rate:
                out.append(f"  VOID CELL (R3): {arm} blocks on publish but its median gap {st.median(gm):.3f} ms is not"
                           f" above one period {1e3 / rate:.1f} ms - the trace did not see the wait")
                void_cell = True
        if void_cell:
            continue
        # R5: scoring
        vendors = sorted({r["arm"] for r in good} - {"tickle@head", "tickle@block", "tickle@aa"}
                         - {a for a in {r["arm"] for r in good} if a.startswith("tickle@")} - silent)
        for subj in [a for a in SUBJECTS if any(r["arm"] == a for r in good)]:
            for key, label in (("crt_med", "creation median (primary)"), ("crt_p99", "creation p99"),
                               ("stamp_med", "send-stamp median (secondary)")):
                parts = []
                sv = usable(good, subj, topic, loss, key)
                for v in vendors:
                    ov = usable(good, v, topic, loss, key)
                    verdict = range_verdict(sv, ov)
                    if key.startswith("crt") and control_ok is False:
                        verdict = "NOT READ (control failed)"
                    parts.append(f"{v} {verdict} ({f2(min(ov) if ov else float('nan'), 0)}-"
                                 f"{f2(max(ov) if ov else float('nan'), 0)})")
                rng = f"{f2(min(sv), 0)}-{f2(max(sv), 0)}" if sv else "-"
                out.append(f"  {label:<31} {subj} {rng} ms vs " + "; ".join(parts))
    return out


def rtt_r4(stamp, crt, gap, gap_min, res_ms=RTT_PRINT_RES_MS):
    """R4 on one framework's reps of one round-trip cell, from per-rep medians (all in ms): stamp RTT p50s, creation
    RTT p50s, realized-gap p50s, and the smallest gap of any rep. (ok, text). Shared by --rtt and the same-host and rmw
    round-trip readers (fair_samehost_summary.py, rmw_samehost_summary.py, rmw_crosshost_creation.py), so the control
    is one rule wherever a round trip is read from creation time.

    noise = range of the reps' stamp p50s + range of the reps' creation p50s. Until 2026-10-18 it was the stamp range
    alone, which assumed the creation median moves in lockstep with stamp median + gap median. That holds for means
    and not for medians: creation = stamp + gap per sample, but median(stamp + gap) is not median(stamp) +
    median(gap), so each rep's crt_p50 - rtt_p50 - gap_p50 carries an offset of its own and the creation median
    scatters between reps on its own account. d is formed from both medians, and the range of a difference is at most
    the sum of the two ranges, so both count. Measured on the per-sample traces of the 192 rmw same-host reps of
    2026-10-18 (18f16b85 and d0650110), that offset is never above 1.0 us. The other candidate, an allowance from the
    gap's per-sample spread (its IQR), was not taken: the core clients print only the gap's p50/p99/min/max, and the
    distribution-free bound on the offset needs the quartiles of both stamp and gap (on the poll rows the stamp's
    half-IQR is ~40 us, which would leave the check no teeth). Changed AFTER the same-host S7 run at 596e9c9c, with
    the user's agreement (2026-10-18): its TickLE reps all printed stamp p50 0.020 ms (noise 0) and creation p50
    0.081/0.080/0.081 ms, |d - g| 1.3 us against a 1 us bound, and it was NOT READ under the old rule. The teeth
    stay: a sender that waits shows as g above 0.2 ms, or as a d - g far beyond both ranges (selftest)."""
    d = st.median(c - s for c, s in zip(crt, stamp))
    g = st.median(gap)
    noise_s, noise_c = max(stamp) - min(stamp), max(crt) - min(crt)
    noise = noise_s + noise_c
    why = []
    if g * 1e6 > CTL_WAKE_NS:
        why.append(f"median gap {g * 1e3:.1f} us")
    if gap_min * 1e6 < CTL_NEG_NS:
        why.append(f"gap {gap_min * 1e3:.1f} us < 0")
    # The clients print rtt_p50_ms and crt_p50_ms to 1 us (%.3f ms), so d carries up to 1 us of rounding on top of the
    # reps' noise (RTT_PRINT_RES_MS; added 2026-10-17 after the first rig run, see R4). A caller that read the 0.1 us
    # fields passes RTT_PRINT_RES_FINE_MS (rtt_p50s()).
    # FLOAT_EPS_MS: d is a difference of decimal ms read as binary floats, so a |d - g| of exactly the bound (0.061 -
    # 0.060 = 0.0010000000000000009) must not read as above it (found 2026-10-17 on the same-host p4 cell, printed
    # "1.0 us > ... 1 us"). It is not a widening of the rule: 1e-9 ms is 1 ps.
    if abs(d - g) > noise + res_ms + FLOAT_EPS_MS:
        why.append(f"|d - g| {abs(d - g) * 1e3:.1f} us > noise {noise * 1e3:.1f} us + print resolution "
                   f"{res_ms * 1e3:g} us")
    # The descriptive label keeps its pre-2026-10-18 meaning (|d| within the stamp range); it decides nothing.
    same = "same within noise" if abs(d) <= noise_s else f"differs by the timer wake g = {g * 1e3:.1f} us"
    return not why, (f"d {d * 1e3:+.1f} us g {g * 1e3:.1f} us noise {noise * 1e3:.1f} us (stamp {noise_s * 1e3:.1f} "
                     f"+ creation {noise_c * 1e3:.1f}): {same}; {'PASS' if not why else 'FAIL ' + str(why)}")


def rtt_p50s(reps, num=lambda f, k: float(f[k])):
    """(stamp p50s, creation p50s, print resolution), all in ms, from the RESULT fields of one framework's reps, for
    rtt_r4. Since 2026-10-18 the core reliable_latency clients print rtt_p50_us and crt_p50_us (0.1 us) beside
    rtt_p50_ms and crt_p50_ms (1 us); the fine fields are read, with RTT_PRINT_RES_FINE_MS, only when EVERY rep has
    both, so a file from before them, or a mix, reads the ms fields with RTT_PRINT_RES_MS exactly as before. num(f, k)
    reads one field as a float."""
    if reps and all("rtt_p50_us" in f and "crt_p50_us" in f for f in reps):
        return ([num(f, "rtt_p50_us") / 1e3 for f in reps], [num(f, "crt_p50_us") / 1e3 for f in reps],
                RTT_PRINT_RES_FINE_MS)
    return [num(f, "rtt_p50_ms") for f in reps], [num(f, "crt_p50_ms") for f in reps], RTT_PRINT_RES_MS


def rtt_campaign(path):
    """--rtt: the core reliable_latency cells of a campaign_sweep.sh output (R4 per framework and cell, R5)."""
    pat = re.compile(r"^(c\d+) L (\S+) (\S+) (.*?)\s+(tickle|cyclonedds|fastdds)\s+rep(\d+) \| (\S+) \| (.*)$")
    rows = {}
    for line in Path(path).read_text(errors="replace").splitlines():
        m = pat.match(line)
        if not m:
            continue
        cell, payload, _q, net, fw, rep, status, kv = m.groups()
        f = dict(re.findall(r"(\w+)=(\S+)", kv))
        rows.setdefault((cell, payload, net.strip()), {}).setdefault(fw, []).append((int(rep), status, f))
    ok_all = True
    for (cell, payload, net), fws in sorted(rows.items()):
        print(f"\n{cell} L {payload} {net}:")
        meds = {}
        for fw, reps in sorted(fws.items()):
            good = [f for _r, s, f in reps if s == "ok" and "crt_p50_ms" in f and f.get("window") == "ok"]
            if len(good) < 2:
                print(f"  {fw}: NOT READ ({len(good)} usable reps with crt_* fields)")
                ok_all = False
                continue
            n = [int(f["measured"]) for f in good]
            stamp, crt, res = rtt_p50s(good)
            gap = [float(f["gap_p50_us"]) / 1e3 for f in good]
            gmin = min(float(f["gap_min_us"]) for f in good) / 1e3
            ok, text = rtt_r4(stamp, crt, gap, gmin, res)
            print(f"  {fw:<10} n {n}  RTT stamp p50 {[round(x, 4) for x in stamp]} creation p50 "
                  f"{[round(x, 4) for x in crt]} ms  {text}")
            ok_all = ok_all and ok
            meds[fw] = (crt, stamp)
        if "tickle" in meds:
            for v in ("fastdds", "cyclonedds"):
                if v in meds:
                    print(f"  creation p50 tickle vs {v}: {range_verdict(meds['tickle'][0], meds[v][0])}; "
                          f"stamp p50: {range_verdict(meds['tickle'][1], meds[v][1])}")
    print(f"\nRTT CONTROL {'PASS' if ok_all else 'FAIL'}")
    return 0 if ok_all else 1


def selftest():
    """Synthetic traces with known answers (no rig): a non-blocking arm that waits inside its stamp latency, a blocking
    arm that falls behind, a run whose schedule slipped (grid VOID), an arrival with no schedule (INSTRUMENT FAULT),
    and a round-trip control."""
    import tempfile
    fails = []

    def check(name, cond):
        print(f"  {'ok  ' if cond else 'FAIL'} {name}")
        if not cond:
            fails.append(name)

    rate, dur = 30.0, 60.0
    per = 1e9 / rate

    def write_run(d, arm, rep, mode, wire_ms=35.0, roundtrip=False):
        stem = f"{arm}_Array4m_l0_r{rep}"
        p, s = ["# ctl creation trace v1 realtime_ns=0 steady_ns=0 max_lines=4000000"], []
        t0 = 1_000_000_000_000
        free = t0  # when the link is free (blocking arm: publish returns then)
        stamp_prev = t0
        for c in range(1, int(dur * rate) + 1):
            sched = t0 + max(0, c - 2) * per if c >= 2 else t0
            if mode == "block":  # publish blocks for the sample's wire time; the next stamp waits for it
                stamp = max(sched, stamp_prev)
                stamp_prev = stamp + wire_ms * 1e6 * 1.05  # 5% over the period: falls behind
                if stamp > t0 + dur * 1e9:
                    break
                arrival = stamp + wire_ms * 1e6 * 1.05
            elif mode == "queue":  # stamps on time, waits inside behind the one in flight; KEEP_LAST drops the rest
                stamp = sched + 20_000
                if free - stamp > wire_ms * 1e6 * 1.05:  # one already waiting: this one is dropped at the sender
                    p.append(f"P {int(stamp)} {int(stamp - sched)} {c} 0")
                    continue
                free = max(free, stamp) + wire_ms * 1e6 * 1.05
                arrival = free
            elif mode == "slip":  # schedule recorded as the stamp itself: gap always 0
                stamp = max(sched, stamp_prev)
                stamp_prev = stamp + wire_ms * 1e6 * 1.05
                if stamp > t0 + dur * 1e9:
                    break
                arrival = stamp + wire_ms * 1e6
                p.append(f"P {int(stamp)} 0 {c} 0")
                s.append(f"S {int(stamp)} {int(arrival - stamp)} {int(arrival)}")
                continue
            else:  # "rtt": nothing waits, 60 us timer wake
                stamp = sched + 60_000
                arrival = stamp + 400_000
            p.append(f"P {int(stamp)} {int(stamp - sched)} {c} 0")
            s.append(f"S {int(stamp)} {int(arrival - stamp)} {int(arrival)}")
        tail = "# end lines=1 dropped=0"
        if roundtrip:
            (d / f"{stem}_pub.ctrace").write_text("\n".join(p + s + [tail]) + "\n")
            (d / f"{stem}_sub.ctrace").write_text("# ctl creation trace v1\n" + tail + "\n")
        else:
            (d / f"{stem}_pub.ctrace").write_text("\n".join(p + [tail]) + "\n")
            (d / f"{stem}_sub.ctrace").write_text("\n".join(["# ctl creation trace v1"] + s + [tail]) + "\n")
        (d / f"{stem}_clock.txt").write_text(
            "before PROBE ok=1 wall_ns=0 offset_ns=0 delay_min_ns=100000 used=16 n=64\n"
            "after PROBE ok=1 wall_ns=900000000000000000 offset_ns=0 delay_min_ns=100000 used=16 n=64\n")
        want = "/x/librmw_tickle.so" if arm.startswith("tickle") else f"librmw_{arm}.so"
        maps = want if want.startswith("/") else f"/opt/{want}"
        return f"RUN {stem} pub_maps=[{maps}] sub_maps=[{maps}] pub_killed=no sub_killed=no want={want}"

    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp) / "i4s.runs"
        d.mkdir()
        (d / "meta.txt").write_text("qos_args=--keep_last --history_depth 5\npub_rate=30\nsamehost=0\nclock_probe=1\n"
                                    "pc_preflight=1\ncreation_trace=1\nroundtrip=0\n")
        idx = [write_run(d, "tickle@head", r, "queue") for r in (1, 2)]
        idx += [write_run(d, "fastdds", r, "block") for r in (1, 2)]
        idx += [write_run(d, "cyclonedds", 1, "slip")]
        (d / "index.txt").write_text("\n".join(idx) + "\n")
        meta, rt, rtrip, recs, mbps = load(d)
        print("case 1: a saturating cell, non-blocking (queues inside) vs blocking (falls behind)")
        t = [r for r in recs if r["arm"] == "tickle@head"]
        f = [r for r in recs if r["arm"] == "fastdds"]
        c = [r for r in recs if r["arm"] == "cyclonedds"]
        check("the cell is saturating at 1 Gb/s", saturating("Array4m", rt, mbps))
        check(f"tickle gap ~0.02 ms {[r['gap_med'] for r in t]}", all(abs(r["gap_med"] - 0.02) < 1e-6 for r in t))
        check("tickle: creation = stamp + 0.02 ms", all(abs(r["crt_med_deliv"] - r["stamp_med"] - 0.02) < 1e-3
                                                          for r in t))
        check(f"fastdds gap grows past a period {[round(r['gap_med'], 1) for r in f]}",
              all(r["gap_med"] > 1e3 / rt for r in f))
        check(f"fastdds stamp latency hides it: stamp median {[round(r['stamp_med'], 2) for r in f]} ~36.75 ms",
              all(abs(r["stamp_med"] - 36.75) < 0.01 for r in f))
        check(f"fastdds creation median {[round(r['crt_med'], 1) for r in f]} far above its stamp median",
              all(r["crt_med"] > 10 * r["stamp_med"] for r in f))
        check("fastdds created fewer than offered (fell behind)", all(r["created"] < r["offered"] for r in f))
        check(f"cyclonedds 'slip' run (gap always 0, schedule = stamp) is VOID by the grid: {c[0]['void']}",
              "grid check failed" in c[0]["void"])
        out = "\n".join(cells(recs, meta, rt, mbps, True))
        print(out)
        check("tickle@head creation median WINS against fastdds",
              re.search(r"creation median \(primary\)\s+tickle@head .* fastdds WIN", out) is not None)
        check("tickle@head send-stamp median LOSES against fastdds (the old reading)",
              re.search(r"send-stamp median \(secondary\)\s+tickle@head .* fastdds LOSE", out) is not None)
        check("growth printed for the saturating cell", "growth fastdds r1:" in out)
        out_gated = "\n".join(cells(recs, meta, rt, mbps, False))
        check("a failed control makes the creation figures NOT READ", "NOT READ (control failed)" in out_gated)
        print("case 2: an arrival with no schedule is an INSTRUMENT FAULT")
        pt = d / "tickle@head_Array4m_l0_r2_sub.ctrace"
        pt.write_text(pt.read_text().replace("# end", "S 5 1000 0\n# end"))
        _m, _r, _t, recs2, _mb = load(d)
        r2 = [r for r in recs2 if r["arm"] == "tickle@head" and r["rep"] == 2][0]
        check(f"VOID: {r2['void']}", r2["void"].startswith("INSTRUMENT FAULT"))
        print("case 3: no trace is never 'no wait'")
        (d / "fastdds_Array4m_l0_r2_pub.ctrace").unlink()
        _m, _r, _t, recs3, _mb = load(d)
        r3 = [r for r in recs3 if r["arm"] == "fastdds" and r["rep"] == 2][0]
        check(f"VOID: {r3['void']}", "no publisher trace" in r3["void"])
        print("case 4: a blocking arm whose gap stays ~0 in a saturating cell voids the cell (R3)")
        fake = [dict(r) for r in recs if not r["void"]]
        for r in fake:
            if r["arm"] == "fastdds":
                r["gap_med"] = 0.01
        out4 = "\n".join(cells(fake, meta, rt, mbps, True))
        check("VOID CELL (R3) printed", "VOID CELL (R3): fastdds" in out4)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp) / "ctl.runs"
        d.mkdir()
        (d / "meta.txt").write_text("qos_args=--reliable\npub_rate=30\nsamehost=0\nclock_probe=1\npc_preflight=1\n"
                                    "creation_trace=1\nroundtrip=1\n")
        idx = [write_run(d, a, r, "rtt", roundtrip=True) for a in ("tickle@head", "cyclonedds") for r in (1, 2)]
        (d / "index.txt").write_text("\n".join(idx) + "\n")
        _m, _r, rtrip, recs, _mb = load(d)
        print("case 5: the round-trip control (60 us timer wake, nothing waits)")
        check("round trip read from the Main's own trace", rtrip and all(not r["void"] for r in recs))
        ok, lines = control(recs)
        print("\n".join(lines))
        check("control PASS, and it says the difference is the timer wake", ok and "timer wake" in lines[0])
        print("case 6: a control read for rmw_tickle alone is not a control")
        ok1, lines1 = control([r for r in recs if r["arm"] == "tickle@head"])
        check("FAIL without a vendor read", not ok1 and "needs tickle@head and at least one vendor" in lines1[-1])
    print("case 7: rtt_r4 from printed per-rep medians (ms)")
    s7 = ([0.020] * 3, [0.081, 0.080, 0.081], [0.0597, 0.0597, 0.0599], 0.0283)  # same-host S7 TickLE, 596e9c9c
    ok7, t7 = rtt_r4(*s7)
    print("   ", t7)
    check("S7 (stamp range 0, creation range 1 us, |d - g| 1.3 us) PASSES on noise 0 + 1 + print 1 us", ok7)
    ok7b, t7b = rtt_r4([0.020] * 3, [0.081] * 3, [0.0597, 0.0597, 0.0599], 0.0283)
    print("   ", t7b)
    check("the same |d - g| 1.3 us with no creation scatter still FAILS (noise 0 + print 1 us)",
          not ok7b and "|d - g| 1.3 us" in t7b)
    ok7c, t7c = rtt_r4([0.100, 0.101, 0.100], [0.360, 0.361, 0.360], [0.200] * 3, 0.190)
    print("   ", t7c)
    check("a sender that waits (g 200 us, d 260 us) FAILS on |d - g| although g is not above 200 us",
          not ok7c and "|d - g| 60.0 us" in t7c and "median gap" not in t7c)
    ok7d, t7d = rtt_r4([0.100, 0.101, 0.100], [0.420, 0.421, 0.420], [0.320] * 3, 0.300)
    check("a sender that waits (g 320 us) FAILS on g", not ok7d and "median gap 320.0 us" in t7d)
    ok7e, t7e = rtt_r4([0.110, 0.118, 0.113], [0.172, 0.180, 0.174], [0.062, 0.0615, 0.062], 0.030)
    print("   ", t7e)
    check("a vendor-like spread (stamp range 8 us, creation range 8 us) passes", ok7e)
    print("case 8: the 0.1 us fields (rtt_p50_us, crt_p50_us) when every rep prints them, the ms fields otherwise")
    # Same-host S10c TickLE at 2579e822: every rep printed rtt_p50_ms 0.020 and crt_p50_ms 0.036 (noise 0), gap p50
    # 14.9 us, so |d - g| 1.1 us > 1 us on rounding alone. The fine fields carry what the rounding hid.
    old = [dict(rtt_p50_ms="0.020", crt_p50_ms="0.036", gap_p50_us=g, gap_min_us="8.0") for g in ("14.9",) * 3]
    s8, c8, r8 = rtt_p50s(old)
    ok8a, t8a = rtt_r4(s8, c8, [14.9e-3] * 3, 8e-3, r8)
    print("   ", t8a)
    check("absent: the ms fields with a 1 us term, the S10c reading (FAIL, |d - g| 1.1 us) is unchanged",
          r8 == RTT_PRINT_RES_MS and not ok8a and "|d - g| 1.1 us" in t8a and "print resolution 1 us" in t8a)
    fine = [dict(f, rtt_p50_us=s, crt_p50_us=c) for f, s, c in
            zip(old, ("20.4", "20.3", "20.4"), ("35.3", "35.2", "35.2"))]
    s8, c8, r8 = rtt_p50s(fine)
    ok8b, t8a = rtt_r4(s8, c8, [14.9e-3] * 3, 8e-3, r8)
    print("   ", t8a)
    check("present: the 0.1 us fields, 0.1 us term; d 14.9 us = g, noise 0.2 us: PASS",
          r8 == RTT_PRINT_RES_FINE_MS and abs(c8[0] - 0.0353) < 1e-12 and ok8b and "d +14.9 us" in t8a)
    fine_off = [dict(f, crt_p50_us=f"{float(f['crt_p50_us']) + 0.4:.1f}") for f in fine]
    ok8c, t8c = rtt_r4(*rtt_p50s(fine_off)[:2], [14.9e-3] * 3, 8e-3, rtt_p50s(fine_off)[2])
    print("   ", t8c)
    check("present: |d - g| 0.4 us over noise 0.2 us + 0.1 us FAILS (would pass at 1 us: the term is really finer)",
          not ok8c and "print resolution 0.1 us" in t8c and rtt_r4(*rtt_p50s(fine_off)[:2], [14.9e-3] * 3, 8e-3)[0])
    mixed = fine[:2] + old[2:]
    check("a mix of reps with and without the fine fields reads the ms fields (1 us)",
          rtt_p50s(mixed)[2] == RTT_PRINT_RES_MS and rtt_p50s(mixed)[0] == [0.020] * 3)
    check("rtt_p50s takes a num(f, k) reader (s6_reps.num)",
          all(abs(x - y) < 1e-12 for x, y in zip(rtt_p50s(fine, lambda f, k: float(f[k]) if k in f else None)[0],
                                                  (0.0204, 0.0203, 0.0204))))
    print(f"selftest: {'PASS' if not fails else 'FAIL (' + str(len(fails)) + ')'}")
    return 1 if fails else 0


def pc_check(out):
    """creation_latency_pc.sh's pre-registered reading P1-P4 (its header) over <out>/{sat,free,ctl}.runs."""
    bad = []
    for tag in ("sat", "free", "ctl"):
        _meta, rate, _rt, recs, _mbps = load(out / f"{tag}.runs")
        bad += [f"{tag} VOID {r['arm']} r{r['rep']}: {r['void']}" for r in recs if r["void"]]
        per = 1e3 / rate
        for arm in sorted({r["arm"] for r in recs}):
            g = [r for r in recs if r["arm"] == arm and not r["void"]]
            if g and all(r["delivered"] == 0 for r in g) and not arm.startswith("tickle@"):
                print(f"PC_CHECK {tag} {arm}: delivered nothing in {len(g)} reps - does not run here, not read")
                continue
            if len(g) < 2:
                bad.append(f"{tag} {arm}: {len(g)} usable reps (NOT READ)")
                continue
            gap = [r["gap_med"] for r in g]
            diff = [r["crt_med_deliv"] - r["stamp_med"] for r in g]
            print(f"PC_CHECK {tag} {arm}: gap med {[round(x, 3) for x in gap]} ms, creation - stamp median "
                  f"{[round(x, 2) for x in diff]} ms, creation (offered) {[round(r['crt_med'], 2) for r in g]}, stamp "
                  f"{[round(r['stamp_med'], 2) for r in g]}, growth {[round(r['growth_ms'], 1) for r in g]} ms")
            if tag == "sat" and arm.startswith(BLOCKING):
                if not all(x > per for x in gap) or not all(x > per for x in diff):
                    bad.append(f"P1 {arm}: gap {gap} / creation - stamp {diff} not above one period {per:.1f} ms")
            if tag == "sat" and arm == "tickle@head" and not all(x < 1.0 for x in gap):
                bad.append(f"P2 tickle@head: gap {gap} not below 1 ms")
            if tag == "free" and (not all(x < 1.0 for x in gap) or
                                  any(r["growth_ms"] > max(1.0, 0.1 * r["crt_med_deliv"]) for r in g)):
                bad.append(f"P3 {arm}: gap {gap} or growth {[r['growth_ms'] for r in g]}")
        if tag == "ctl":
            ok, lines = control(recs)
            print("\n".join(lines))
            if not ok:
                bad.append("P4 control FAILED")
    print("PC_CHECK " + ("PASS" if not bad else "FAIL:\n  " + "\n  ".join(bad)))
    return 1 if bad else 0


def main(argv):
    if argv[1:2] == ["--selftest"]:
        return selftest()
    if argv[1:2] == ["--pc-check"]:
        return pc_check(Path(argv[2]))
    if argv[1:2] == ["--rtt"]:
        return rtt_campaign(argv[2])
    if len(argv) < 2:
        print(__doc__)
        return 2
    runs = Path(argv[1])
    mbps = None
    if "--link-mbps" in argv:
        mbps = float(argv[argv.index("--link-mbps") + 1])
    control_ok = None
    if "--control" in argv:
        cdir = Path(argv[argv.index("--control") + 1])
        _cm, _cr, crt, crecs, _cmb = load(cdir)
        print(f"=== control {cdir} (round trip {int(crt)}) ===")
        print_runs(crecs)
        control_ok, lines = control(crecs)
        print("\n".join(lines))
        print(f"CONTROL {'PASS' if control_ok else 'FAIL'}")
    meta, rate, roundtrip, recs, mbps = load(runs, mbps)
    print(f"\n=== {runs}: qos [{meta.get('qos_args', '?')}] rate {rate:g} Hz, round trip {int(roundtrip)}, link "
          f"{mbps:g} Mb/s, pc_preflight {meta.get('pc_preflight', '0')} ===")
    print_runs(recs)
    if roundtrip:
        ok, lines = control(recs)
        print("\ncontrol (R4):\n" + "\n".join(lines))
        print(f"CONTROL {'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1
    print("\n".join(cells(recs, meta, rate, mbps, control_ok)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
