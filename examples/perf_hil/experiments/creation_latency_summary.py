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
     A FAILED control makes every creation figure of the gated cell NOT READ.
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
from rmw_keepall_rig_summary import INDEX_RE, loaded  # noqa: E402

WARM_S, COOL_S = 3.0, 2.0
N_MED, N_P99 = 50, 500
GAP_BIG_NS = 1_000_000
GRID_SLOPE_TOL, GRID_RESID_NS = 1e-3, 500_000
CTL_REPS, CTL_N = 2, 200
CTL_WAKE_NS, CTL_NEG_NS = 200_000, -20_000
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
              f"{f2(r['growth_ms'])}  [{r['clock']}; {r['grid']}]")


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
    lines, passed = [], True
    arms = sorted({r["arm"] for r in recs})
    for arm in arms:
        rs = [r for r in recs if r["arm"] == arm]
        good = [r for r in rs if not r["void"] and r["delivered"] >= CTL_N and not math.isnan(r["stamp_med"])]
        voids = [r for r in rs if r["void"]]
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
        lines.append(f"  {arm}: reps {len(good)}, round trips {[r['delivered'] for r in good]}, stamp RTT medians "
                     f"{[round(m, 4) for m in meds]} ms, d = {d * 1e3:+.1f} us, g = {g * 1e3:.1f} us, noise "
                     f"{noise * 1e3:.1f} us, min gap {gmin * 1e3:+.1f} us: {same}; {verdict}")
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
        for arm in sorted({r["arm"] for r in good}):
            gm = [r["gap_med"] for r in good if r["arm"] == arm]
            out.append(f"  treatment {arm:<16} realized gap (stamp - schedule) median per rep "
                       f"{[round(x, 3) for x in gm]} ms, p99 {[round(r['gap_p99'], 3) for r in good if r['arm'] == arm]}"
                       f" ms, share > 1 ms {[round(r['gap_big'], 1) for r in good if r['arm'] == arm]}%")
            if sat and arm.startswith(BLOCKING) and gm and st.median(gm) * 1e6 <= 1e9 / rate:
                out.append(f"  VOID CELL (R3): {arm} blocks on publish but its median gap {st.median(gm):.3f} ms is not"
                           f" above one period {1e3 / rate:.1f} ms - the trace did not see the wait")
                void_cell = True
        if void_cell:
            continue
        # R5: scoring
        vendors = sorted({r["arm"] for r in good} - {"tickle@head", "tickle@block", "tickle@aa"}
                         - {a for a in {r["arm"] for r in good} if a.startswith("tickle@")})
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
            stamp = [float(f["rtt_p50_ms"]) for f in good]
            crt = [float(f["crt_p50_ms"]) for f in good]
            gap = [float(f["gap_p50_us"]) / 1e3 for f in good]
            gmin = min(float(f["gap_min_us"]) for f in good) / 1e3
            d = st.median(c - s for c, s in zip(crt, stamp))
            g = st.median(gap)
            noise = max(stamp) - min(stamp)
            why = []
            if g * 1e6 > CTL_WAKE_NS:
                why.append(f"median gap {g * 1e3:.1f} us")
            if gmin * 1e6 < CTL_NEG_NS:
                why.append(f"gap {gmin * 1e3:.1f} us < 0")
            if abs(d - g) > noise:
                why.append(f"|d - g| {abs(d - g) * 1e3:.1f} us > noise {noise * 1e3:.1f} us")
            same = "same within noise" if abs(d) <= noise else f"differs by the timer wake g = {g * 1e3:.1f} us"
            print(f"  {fw:<10} n {n}  RTT stamp p50 {stamp} creation p50 {crt} ms  d {d * 1e3:+.1f} us g "
                  f"{g * 1e3:.1f} us noise {noise * 1e3:.1f} us: {same}; {'PASS' if not why else 'FAIL ' + str(why)}")
            ok_all = ok_all and not why
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
        idx = [write_run(d, "tickle@head", r, "rtt", roundtrip=True) for r in (1, 2)]
        (d / "index.txt").write_text("\n".join(idx) + "\n")
        _m, _r, rtrip, recs, _mb = load(d)
        print("case 5: the round-trip control (60 us timer wake, nothing waits)")
        check("round trip read from the Main's own trace", rtrip and all(not r["void"] for r in recs))
        ok, lines = control(recs)
        print("\n".join(lines))
        check("control PASS, and it says the difference is the timer wake", ok and "timer wake" in lines[0])
    print(f"selftest: {'PASS' if not fails else 'FAIL (' + str(len(fails)) + ')'}")
    return 1 if fails else 0


def main(argv):
    if argv[1:2] == ["--selftest"]:
        return selftest()
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
