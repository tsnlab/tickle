#!/usr/bin/env python3
"""time_varying_read.py OUT - the reader of time_varying_netns.sh (release stage 3, T0; 2026-11-02).

Implements the pre-registered rules of that harness's header IN CODE: V1 (copies after delivery after a delay step
up), V2 (repair wait after a delay step down), V3 (goodput across a loss step; the hybrid's p convergence reported),
V4 (copies after delivery under jitter), each tv arm judged as (tv value - worst static phase value), with STATIC when
the static arm of a phase already fails the same number, and VOID read from outcomes only.

Prints one line per (rule, tv arm, cell): "<rule> <arm> <cell> PASS|FAIL|STATIC|VOID|N/A <text>", then a summary.
N/A is V2 on a family whose down step lands in a lossless phase (LD: nothing to repair). A "CONTROL_RESENDS" line
reports the clean control's rel_large resends, which void every rel_large verdict; the verdict each would have read
is printed after "read past the control (not scored)".
"""

import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wifi_burst_read import counters_of, kv, num, quantile, read_lines, result_of  # noqa: E402

# --- thresholds: a DRAFT pending the user (TIME_VARYING_STAGE_PLAN_2026-11-02, decision 2). Change them only before
# --- a run, and say so in the harness header.
V1_COPIES_MAX = 0.01  # copies after delivery / delivered: tv minus worst static, and the STATIC limit (R7's 1%)
V2_WAIT_FACTOR = 1.5  # repair-wait p99 after the settle time <= this x the destination static arm's p99
V2_SETTLE_S = 2.0  # "returns within 2 s of the step": samples sent from here to the next step are judged
V3_GOODPUT_DROP_MAX = 0.10  # transition goodput >= (1 - this) x the worst static phase's goodput
V3_WINDOW_S = 2.0  # the transition: samples sent within this long after a loss step
V4_COPIES_MAX = 0.01  # copies after delivery / delivered under jitter: tv minus worst static, and the STATIC limit
P_CONVERGE_FACTOR = 2.0  # report only: the publisher's p within this factor of the static arm's final p

# --- VOID limits (outcomes) ---
MIN_SCORED_FRAC = 0.6  # a small run scoring under this share of DUR x 100 Hz is VOID
MIN_DELIVERED = 1000  # V1/V4: pooled delivered samples of an arm and cell, small cells
MIN_DELIVERED_LARGE = 50  # V1/V4 on rel_large
MIN_TRACE_FRAC = 0.99  # a trace holding fewer lines than this share of the scored samples is short
MIN_PHASE_CHANGES = 2  # a tv run with fewer changes inside its samples' send interval did not vary
V2_MIN_WINDOW = 400  # samples in V2's judged window, pooled
V3_MIN_WINDOW_SMALL = 400  # offered samples in V3's windows, pooled
V3_MIN_WINDOW_LARGE = 20
DELAY_APPLIED_MIN_MS = 15.0  # d20 one-way p50 minus d1's (nominal 19 ms)
JITTER_APPLIED_MIN_MS = 4.0  # j5 p90-p10 one-way spread minus j0's (nominal 8 ms)
# rel_large's trace has one line per sample, stamped when the LAST of its units arrives: the maximum of ~723 jittered
# delays sits at the top of the jitter range, so its spread barely moves (T1: 0.24 ms j0, 0.52 ms j5). Its p50 does:
# a uniform +-5 ms jitter shifts the last unit's arrival by ~+5 ms (T1: +5.5 ms static).
JITTER_LARGE_SHIFT_MIN_MS = 3.0  # rel_large: j5 phase one-way p50 minus j0's
# rel_large copy rate denominator: the units (datagrams' worth of payload) of one sample at the bench's size over a
# 1500-byte link MTU (veth default; the harness sets none). Checked per run against the publisher's own first-send
# unit count (hybrid_run_units + hybrid_mtu_units = units x large_published), which moves with the size or the MTU.
LARGE_SAMPLE_BYTES = 1048576
LARGE_UNITS_PER_SAMPLE = 723
LOSS_REALIZED = (0.5, 1.5)  # a lossy static arm's realized unicast loss, as a share of its nominal
CLEAN_PHASE_LOSS_MAX = 0.005  # be_small in a tv arm's clean phase
CONTROL_BE_MIN = 0.999
SMALL_HZ = 100
WARMUP = SMALL_HZ  # wifi_link_node scores from seq 100

# Phase -> nominal (delay ms, loss share, jitter ms). D's background loss is read from the provenance.
PHASES = {"d1": (1, None, 0), "d20": (20, None, 0), "j0": (6, 0.0, 0), "j5": (6, 0.0, 5), "l0": (1, 0.0, 0),
          "lb5": (1, 0.05, 0), "lg5": (1, 0.05, 0), "ld2": (21, 0.10, 0)}
FAMILIES = {"D": ["d1", "d20"], "J": ["j0", "j5"], "LB": ["l0", "lb5"], "LG": ["l0", "lg5"],
            "LD": ["l0", "lg5", "ld2"]}
RULES = {"D": ["V1", "V2"], "J": ["V4"], "LB": ["V3"], "LG": ["V3"], "LD": ["V1", "V2", "V3"]}
COPY_CELLS = ("rel_small", "rel_large")
V3_CELLS = ("rel_small", "rel_large")


LAT_RE = re.compile(r"LAT index=(\d+) sent_ns=(\d+) arrived_ns=(\d+)$")
PEER_P_RE = re.compile(r"PEER_P before_sample=(\d+) code=0x([0-9a-fA-F]{4}) n_hist=[0-9/]+$")


def pct(x):
    return "n/a" if x is None else f"{100.0 * x:.2f}%"


# --- loading ---------------------------------------------------------------------------------------------------------


def read_phases(path):
    out = []
    for line in read_lines(path):
        if line.startswith("PHASE"):
            d = kv(line)
            out.append({"idx": int(d["idx"]), "key": d["key"], "t0": int(d["t_before_ns"]),
                        "t1": int(d["t_after_ns"]), "ok": d.get("ok", "1") == "1"})
    return out


def read_trace(path):
    rows = []
    for line in read_lines(path):
        parts = line.split()
        if len(parts) == 3 and all(p.isdigit() for p in parts):
            rows.append((int(parts[0]), int(parts[1]), int(parts[2])))
    return rows


def load_run(out, name, dur, nproc):
    parts = name.rsplit("-", 2)
    if len(parts) != 3:
        return None
    arm, cell, rep = parts
    d = os.path.join(out, "runs", name)
    r = {"arm": arm, "cell": cell, "rep": rep, "void": [], "lat_void": [], "tv": "_p" in arm}
    ping = read_lines(os.path.join(d, "ping.log"))
    echo = read_lines(os.path.join(d, "echo.log"))
    meta = kv(" ".join(read_lines(os.path.join(d, "meta.txt"))))
    if num(meta, "load") > nproc / 2:
        r["lat_void"].append(f"PC load {meta.get('load')} above nproc/2 at the start")
    r["phases"] = read_phases(os.path.join(d, "phases.txt"))
    r["counters"] = counters_of(os.path.join(d, "counters.txt"))
    r["rstats"] = {}
    for line in ping + echo:
        if line.startswith("RSTATS:") and "hist=" not in line:
            d2 = kv(line)
            for k, v in d2.items():
                r["rstats"][d2.get("role", "?") + "." + k] = v
    if cell == "rel_large":
        r["ping"] = result_of(ping, "RESULT: role=pub")
        r["echo"] = result_of(echo, "RESULT: role=sub")
        # LAT index sent_ns arrived_ns: one per delivered sample (-DR4_TRACE). Matched whole: in a log taken with
        # 2>&1 (the first smoke) a core warning on stderr can land inside a buffered stdout line and cut it; a cut
        # line is dropped and counts against the trace's length below. The harness now keeps stderr apart (.err).
        r["trace"] = []
        for line in echo:
            m = LAT_RE.match(line)
            if m:
                r["trace"].append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
        r["peer_p"] = []
        err = os.path.join(d, "ping.err")
        for line in read_lines(err) if os.path.exists(err) else ping:
            m = PEER_P_RE.match(line)
            if m:
                r["peer_p"].append((int(m.group(1)), int(m.group(2), 16)))
    else:
        r["ping"] = result_of(ping, "RESULT role=ping")
        r["echo"] = result_of(echo, "RESULT role=echo")
        r["trace"] = read_trace(os.path.join(d, "trace_echo.txt"))
    p, e = r["ping"], r["echo"]
    if p is None or e is None:
        r["void"].append("a RESULT line is missing")
        return r
    # A field absent is not a zero: a RESULT line cut by stderr (the first smoke's s_d20 rel_large lost its
    # large_duplicate=1364 that way, and read as 0 copies) or a build without RSTATS must void, not pass.
    large = cell == "rel_large"
    need = [(p, "published"), (e, "delivered" if large else "scored"),
            (e, "first_index" if large else "duplicates")] + ([(e, "large_duplicate")] if large else [])
    missing = [k for res, k in need if k not in res]
    if cell != "be_small":
        key = "sub.late_below_ack" if large else "echo.late_below_ack"
        missing += [key] if key not in r["rstats"] else []
    if missing:
        r["void"].append(f"field(s) missing from the node output: {', '.join(missing)}")
        return r
    if cell == "rel_large":
        r["first"] = int(num(e, "first_index"))
        r["published"] = int(num(p, "published"))
        r["offered"] = max(0, r["published"] - r["first"])
        r["delivered"] = int(num(e, "delivered"))
        r["copies"] = int(num(e, "large_duplicate")) + int(num(r["rstats"], "sub.late_below_ack")) + int(
            num(r["rstats"], "sub.duplicates"))
        # large_duplicate counts duplicate UNITS (datagrams of a sample), not samples: its denominator is the units
        # of the delivered samples, a fixed count per sample (LARGE_UNITS_PER_SAMPLE). Not the reader's hybrid_rx_loss
        # frames: that is a windowed estimate (halved at the window, reset by a stale forget, one frame per run in run
        # mode) and gave 24..723 units per sample across T1's s_l0 reps.
        size = int(num(p, "size", -1))
        have = all(k in p for k in ("hybrid_run_units", "hybrid_mtu_units"))
        first_sends = int(num(p, "hybrid_run_units")) + int(num(p, "hybrid_mtu_units")) if have else -1
        lp = int(num(p, "large_published", -1))
        if size != LARGE_SAMPLE_BYTES:
            r["void"].append(f"sample size {size}, not the {LARGE_SAMPLE_BYTES} LARGE_UNITS_PER_SAMPLE is for")
            return r
        if lp <= 0 or first_sends != LARGE_UNITS_PER_SAMPLE * lp:
            r["void"].append(f"publisher sent {first_sends} units for {lp} samples, not {LARGE_UNITS_PER_SAMPLE} each "
                             "(link MTU or unit size changed)")
            return r
        r["units_per_sample"] = LARGE_UNITS_PER_SAMPLE
        r["copy_den"] = r["delivered"] * r["units_per_sample"]
        r["retransmitted"] = int(num(p, "retransmitted", -1))
    else:
        r["first"] = WARMUP
        r["published"] = int(num(p, "published"))
        r["offered"] = max(0, r["published"] - WARMUP)
        r["delivered"] = int(num(e, "scored"))
        r["copies"] = int(num(r["rstats"], "echo.late_below_ack")) + int(num(r["rstats"], "echo.duplicates")) + int(
            num(e, "duplicates"))
        r["copy_den"] = r["delivered"]
        expected = (dur - 1) * SMALL_HZ
        if r["delivered"] < MIN_SCORED_FRAC * expected:
            r["void"].append(f"{r['delivered']} samples scored, under {MIN_SCORED_FRAC:.0%} of {expected:.0f}")
    if r["published"] == 0:
        r["void"].append("the Publisher never matched")
    if r["delivered"] > 0 and len(r["trace"]) < MIN_TRACE_FRAC * r["delivered"]:
        r["void"].append(f"trace short: {len(r['trace'])} lines for {r['delivered']} delivered")
    if not r["phases"]:
        r["void"].append("no phase log")
    elif not all(ph["ok"] for ph in r["phases"]):
        r["void"].append("a phase change failed (phases.txt ok=0)")
    if r["tv"] and r["trace"] and r["phases"]:
        lo = min(t[1] for t in r["trace"])
        hi = max(t[1] for t in r["trace"])
        inside = sum(1 for ph in r["phases"][1:] if lo <= ph["t1"] <= hi)
        if inside < MIN_PHASE_CHANGES:
            r["void"].append(f"{inside} phase changes inside the samples' send interval, under {MIN_PHASE_CHANGES}")
    return r


def load(out):
    nproc = os.cpu_count() or 1
    prov = kv(" ".join(read_lines(os.path.join(out, "provenance.txt"))))
    dur = num(prov, "dur", 30.0)
    bg = num(prov, "d_bg_loss", 2.0) / 100.0
    PHASES["d1"] = (1, bg, 0)
    PHASES["d20"] = (20, bg, 0)
    runs = []
    runs_dir = os.path.join(out, "runs")
    for name in sorted(os.listdir(runs_dir)) if os.path.isdir(runs_dir) else []:
        r = load_run(out, name, dur, nproc)
        if r is not None:
            runs.append(r)
    return runs, dur


# --- per-sample helpers -----------------------------------------------------------------------------------------------


def send_times(r):
    """seq -> send time for every offered sample: the trace's own for delivered ones, interpolated between delivered
    neighbours (the writer publishes on a fixed schedule) for lost ones."""
    known = sorted((s, t) for s, t, _ in r["trace"])
    if len(known) < 2:
        return {}
    seqs = [s for s, _ in known]
    times = [t for _, t in known]
    out = {}
    last = r["first"] + r["offered"]
    for seq in range(r["first"], last):
        i = bisect.bisect_left(seqs, seq)
        if i < len(seqs) and seqs[i] == seq:
            out[seq] = times[i]
            continue
        a = max(0, min(i - 1, len(seqs) - 2))
        s0, s1, t0, t1 = seqs[a], seqs[a + 1], times[a], times[a + 1]
        out[seq] = t0 + (seq - s0) * (t1 - t0) / max(1, s1 - s0)
    return out


def phase_at(phases, t):
    """(index into phases, key) of the phase a sample sent at t was sent in; None inside a change."""
    k = None
    for i, ph in enumerate(phases):
        if i > 0 and ph["t0"] <= t < ph["t1"]:
            return None
        if ph["t1"] <= t:
            k = i
    return k


def owd_ms(row):
    return (row[2] - row[1]) / 1e6


def static_runs(runs, phase, cell):
    return [r for r in runs if r["arm"] == "s_" + phase and r["cell"] == cell]


def usable(rs):
    return [r for r in rs if not r["void"]]


def pooled_owd(rs):
    return sorted(owd_ms(x) for r in rs for x in r["trace"])


def static_goodput(rs):
    off = sum(r["offered"] for r in rs)
    dl = sum(min(r["delivered"], r["offered"]) for r in rs)
    return (dl / off) if off else None


def halves_goodput(rs):
    """The static reference split in time: first and second half of each run's offered samples, pooled."""
    a_off = a_dl = b_off = b_dl = 0
    for r in rs:
        st = send_times(r)
        delivered = {s for s, _, _ in r["trace"]}
        seqs = sorted(st)
        mid = len(seqs) // 2
        for i, s in enumerate(seqs):
            got = s in delivered
            if i < mid:
                a_off, a_dl = a_off + 1, a_dl + got
            else:
                b_off, b_dl = b_off + 1, b_dl + got
    return (a_dl / a_off if a_off else None), (b_dl / b_off if b_off else None)


# --- treatment and control checks -------------------------------------------------------------------------------------


def realized_loss(r):
    """ping's unicast band: (sent skbs, dropped) per skb as netem drops (wifi_burst_read.py per_run)."""
    c = r["counters"].get("p", {})
    bands = c.get("bands", {})
    drop = bands.get("20", (0, 0))[1]
    sent = max(0, c.get("tx_packets", 0) - bands.get("10", (0, 0))[0])
    return (drop / (sent + drop)) if sent + drop else None


def control_resends(runs):
    """s_l0 rel_large must resend nothing: the clean path loses nothing, so a resend answers reordering (T1: run
    datagrams arriving behind a higher seq at 1 ms are NACKed at once, dup == retransmitted in every rep). Returns
    (reasons that void the rel_large verdicts, the finding line's per-rep counts)."""
    why, counts = [], []
    for r in sorted((r for r in runs if r["arm"] == "s_l0" and r["cell"] == "rel_large"), key=lambda r: r["rep"]):
        if r["void"]:
            continue  # control_void reports it
        rt = r.get("retransmitted", -1)
        dup = int(num(r["echo"], "large_duplicate"))
        counts.append(f"rep {r['rep']}: retransmitted {rt}, large_duplicate {dup}, "
                      f"{pct(dup / r['copy_den']) if r['copy_den'] else 'n/a'} of {r['copy_den']} units")
        if rt != 0:
            why.append(f"s_l0/rel_large/{r['rep']}: {'no retransmitted field' if rt < 0 else f'{rt} units resent'}")
    return why, counts


def control_void(runs):
    """The clean static arm s_l0 must lose nothing; otherwise the setup drops and nothing can be read."""
    ctl = [r for r in runs if r["arm"] == "s_l0"]
    if not ctl:
        return ["no clean control s_l0 in this OUT"]
    why = []
    for r in ctl:
        name = f"s_l0/{r['cell']}/{r['rep']}"
        if r["void"]:
            why.append(f"{name}: {'; '.join(r['void'])}")
            continue
        share = r["delivered"] / r["offered"] if r["offered"] else 0.0
        need = CONTROL_BE_MIN if r["cell"] == "be_small" else 1.0
        if share < need:
            why.append(f"{name}: delivered {pct(share)} (< {pct(need)})")
        drops = sum(x[1] for node in r["counters"].values() for x in node["bands"].values())
        if drops:
            why.append(f"{name}: {drops} tc drops")
    return why


def treatment_void(runs, family, cell):
    """Whether the family's factor is seen in the outcome, statics and tv runs. Returns a list of reasons."""
    why = []
    phases = FAMILIES[family]
    if family == "D" or family == "LD":
        lo_key, hi_key = ("d1", "d20") if family == "D" else ("l0", "ld2")
        lo = pooled_owd(usable(static_runs(runs, lo_key, "rel_small")))
        hi = pooled_owd(usable(static_runs(runs, hi_key, "rel_small")))
        if lo and hi:
            gap = quantile(hi, 0.5) - quantile(lo, 0.5)
            if gap < DELAY_APPLIED_MIN_MS:
                why.append(f"static s_{hi_key} one-way p50 only {gap:.1f} ms above s_{lo_key}")
    if family == "J":
        j0 = pooled_owd(usable(static_runs(runs, "j0", "rel_small")))
        j5 = pooled_owd(usable(static_runs(runs, "j5", "rel_small")))
        if j0 and j5:
            s0 = quantile(j0, 0.9) - quantile(j0, 0.1)
            s5 = quantile(j5, 0.9) - quantile(j5, 0.1)
            if s5 - s0 < JITTER_APPLIED_MIN_MS:
                why.append(f"s_j5 one-way p90-p10 spread {s5:.1f} ms, only {s5 - s0:.1f} ms above s_j0's")
    for ph in phases:
        loss = PHASES[ph][1]
        if not loss:
            continue
        for r in usable(static_runs(runs, ph, cell)):
            real = realized_loss(r)
            if real is None or not LOSS_REALIZED[0] * loss <= real <= LOSS_REALIZED[1] * loss:
                why.append(f"s_{ph}/{cell}/{r['rep']}: realized unicast loss {pct(real)} for nominal {pct(loss)}")
    return why


def tv_phase_check(r, family):
    """A tv run's own trace must show its phases: per-phase one-way p50 for delay families, per-phase loss on
    be_small for loss families (with no be_small, at least one tc drop)."""
    why = []
    if not r["trace"] or not r["phases"]:
        return why
    if family in ("D", "J") or (family == "LD" and r["cell"] != "be_small"):
        by = {}
        for row in r["trace"]:
            k = phase_at(r["phases"], row[1])
            if k is not None:
                by.setdefault(r["phases"][k]["key"], []).append(owd_ms(row))
        if family == "D" and "d1" in by and "d20" in by:
            gap = quantile(sorted(by["d20"]), 0.5) - quantile(sorted(by["d1"]), 0.5)
            if gap < DELAY_APPLIED_MIN_MS:
                why.append(f"tv trace: d20 phase one-way p50 only {gap:.1f} ms above d1's")
        if family == "J" and "j0" in by and "j5" in by and r["cell"] == "rel_large":
            gap = quantile(sorted(by["j5"]), 0.5) - quantile(sorted(by["j0"]), 0.5)
            if gap < JITTER_LARGE_SHIFT_MIN_MS:
                why.append(f"tv trace: j5 phase one-way p50 only {gap:.1f} ms above j0's "
                           f"(rel_large: last-unit arrival, judged by the shift)")
        elif family == "J" and "j0" in by and "j5" in by:
            s0 = quantile(sorted(by["j0"]), 0.9) - quantile(sorted(by["j0"]), 0.1)
            s5 = quantile(sorted(by["j5"]), 0.9) - quantile(sorted(by["j5"]), 0.1)
            if s5 - s0 < JITTER_APPLIED_MIN_MS:
                why.append(f"tv trace: j5 phase spread only {s5 - s0:.1f} ms above j0's")
        if family == "LD" and "l0" in by and "ld2" in by:
            gap = quantile(sorted(by["ld2"]), 0.5) - quantile(sorted(by["l0"]), 0.5)
            if gap < DELAY_APPLIED_MIN_MS:
                why.append(f"tv trace: ld2 phase one-way p50 only {gap:.1f} ms above l0's")
    if family in ("LB", "LG", "LD") and r["cell"] == "be_small":
        st = send_times(r)
        got = {s for s, _, _ in r["trace"]}
        off, dl = {}, {}
        for s, t in st.items():
            k = phase_at(r["phases"], t)
            if k is None:
                continue
            key = r["phases"][k]["key"]
            off[key] = off.get(key, 0) + 1
            dl[key] = dl.get(key, 0) + (s in got)
        for key, n in off.items():
            loss = 1 - dl[key] / n
            nominal = PHASES[key][1] or 0.0
            # GE bursts make a short phase's loss noisy: only the clean phase and a lossy phase showing none are judged
            if nominal == 0 and loss > CLEAN_PHASE_LOSS_MAX:
                why.append(f"tv be_small: clean phase {key} lost {pct(loss)}")
            if nominal > 0 and loss < LOSS_REALIZED[0] * nominal:
                why.append(f"tv be_small: lossy phase {key} lost only {pct(loss)} of nominal {pct(nominal)}")
    return why


# --- the rules -------------------------------------------------------------------------------------------------------


def copies_rule(rule, limit, runs, arm, family, cell, emit):
    tv = [r for r in runs if r["arm"] == arm and r["cell"] == cell]
    if not tv:
        return
    floor = MIN_DELIVERED if cell == "rel_small" else MIN_DELIVERED_LARGE
    why = [f"{r['rep']}: {v}" for r in tv for v in r["void"]]
    for r in usable(tv):
        why += [f"{r['rep']}: {v}" for v in tv_phase_check(r, family)]
    why += treatment_void(runs, family, cell)
    tvu = usable(tv)
    unit = "samples" if cell == "rel_small" else "units"
    t_dl = sum(r["copy_den"] for r in tvu)
    t_cp = sum(r["copies"] for r in tvu)
    if sum(r["delivered"] for r in tvu) < floor:
        why.append(f"tv delivered {sum(r['delivered'] for r in tvu)} samples, under {floor}")
    statics = {}
    for ph in FAMILIES[family]:
        su = usable(static_runs(runs, ph, cell))
        if sum(r["delivered"] for r in su) < floor:
            why.append(f"s_{ph} delivered {sum(r['delivered'] for r in su)} samples, under {floor}")
            continue
        dl = sum(r["copy_den"] for r in su)
        statics[ph] = (sum(r["copies"] for r in su) / dl, sum(r["copies"] for r in su), dl)
    if why:
        emit(rule, arm, cell, "VOID", "; ".join(why))
        return
    worst_ph = max(statics, key=lambda k: statics[k][0])
    worst = statics[worst_ph][0]
    tv_rate = t_cp / t_dl
    delta = tv_rate - worst
    text = (f"tv copies after delivery {t_cp}/{t_dl} {unit} = {pct(tv_rate)}; worst static s_{worst_ph} "
            f"{statics[worst_ph][1]}/{statics[worst_ph][2]} = {pct(worst)}; tv - worst = {pct(delta)} "
            f"(limit {pct(limit)}); statics: " + ", ".join(f"s_{k} {pct(v[0])}" for k, v in statics.items()))
    if worst > limit:
        emit(rule, arm, cell, "STATIC", text + f" - s_{worst_ph} already over {pct(limit)}")
    else:
        emit(rule, arm, cell, "PASS" if delta <= limit else "FAIL", text)


def down_steps(r, family):
    """Indices k of phase changes that reduce delay."""
    out = []
    for k in range(1, len(r["phases"])):
        a, b = r["phases"][k - 1]["key"], r["phases"][k]["key"]
        if PHASES[b][0] < PHASES[a][0]:
            out.append(k)
    return out


def v2(runs, arm, family, emit):
    cell = "rel_small"
    tv = [r for r in runs if r["arm"] == arm and r["cell"] == cell]
    if not tv:
        return
    phase_s = float(arm.rsplit("_p", 1)[1])
    dest = FAMILIES[family][0]  # every down step lands in the family's first (1 ms) phase
    # How long a change takes (t_before -> t_after: the harness's tc calls), every change of every rep.
    took = sorted((ph["t1"] - ph["t0"]) / 1e6 for r in tv for ph in r["phases"][1:])
    took_txt = (f"phase changes took {took[0]:.1f} / {quantile(took, 0.5):.1f} / {took[-1]:.1f} ms min/p50/max over "
                f"{len(took)}" if took else "no phase change logged")
    if not PHASES[dest][1]:
        emit("V2", arm, cell, "N/A", f"not applicable (down step lands in a lossless phase s_{dest}: nothing to "
             f"repair, no repair wait); {took_txt}")
        return
    why = [f"{r['rep']}: {v}" for r in tv for v in r["void"] + r["lat_void"]]
    for r in usable(tv):
        why += [f"{r['rep']}: {v}" for v in tv_phase_check(r, family)]
    why += treatment_void(runs, family, cell)
    ref = [r for r in usable(static_runs(runs, dest, cell)) if not r["lat_void"]]
    ref_owd = pooled_owd(ref)
    if len(ref_owd) < V2_MIN_WINDOW:
        why.append(f"static s_{dest}: {len(ref_owd)} unloaded samples, under {V2_MIN_WINDOW}")
    if why:
        emit("V2", arm, cell, "VOID", "; ".join(why))
        return
    base = quantile(ref_owd, 0.5)
    ref_p99 = quantile(ref_owd, 0.99) - base
    # The reference must reproduce itself within the rule's tolerance: its two halves (in arrival order).
    first, second = [], []
    for r in ref:
        rows = r["trace"]
        first += [owd_ms(x) - base for x in rows[: len(rows) // 2]]
        second += [owd_ms(x) - base for x in rows[len(rows) // 2:]]
    h1, h2 = quantile(sorted(first), 0.99), quantile(sorted(second), 0.99)
    transient, settled = [], []
    for r in usable(tv):
        if r["lat_void"]:
            continue
        steps = down_steps(r, family)
        for k in steps:
            # From the change's START: a sample sent while tc is being changed (60+ ms) is the one that waits behind
            # the old phase's queue; starting at t_after missed every such wait (T1 LD: 11 ms HOL waits unjudged).
            start = r["phases"][k]["t0"]
            end = r["phases"][k + 1]["t0"] if k + 1 < len(r["phases"]) else float("inf")
            for row in r["trace"]:
                if start <= row[1] < end:
                    w = owd_ms(row) - base
                    (transient if row[1] < start + V2_SETTLE_S * 1e9 else settled).append(w)
    transient.sort()
    settled.sort()
    tr = (f"transient (first {V2_SETTLE_S:.0f} s from the change start) p99 {quantile(transient, 0.99):.2f} ms, "
          f"max {transient[-1]:.2f} ms over {len(transient)}" if transient else "no transient samples")
    tr += f"; {took_txt}"
    if min(h1, h2) <= 0 or max(h1, h2) / min(h1, h2) > V2_WAIT_FACTOR:
        emit("V2", arm, cell, "VOID", f"static s_{dest} repair-wait p99 halves {h1:.2f} / {h2:.2f} ms differ by more "
             f"than {V2_WAIT_FACTOR}x: the reference cannot decide; {tr}")
        return
    if len(settled) < V2_MIN_WINDOW:
        emit("V2", arm, cell, "VOID", f"{len(settled)} samples sent >= {V2_SETTLE_S:.0f} s after a down step, under "
             f"{V2_MIN_WINDOW} (phase {phase_s:g} s); {tr}; static p99 {ref_p99:.2f} ms")
        return
    got = quantile(settled, 0.99)
    limit = V2_WAIT_FACTOR * ref_p99
    emit("V2", arm, cell, "PASS" if got <= limit else "FAIL",
         f"repair-wait p99 {got:.2f} ms over {len(settled)} settled samples vs limit {limit:.2f} ms "
         f"({V2_WAIT_FACTOR} x s_{dest} p99 {ref_p99:.2f} ms over its p50 {base:.2f} ms; halves {h1:.2f}/{h2:.2f}); "
         f"tv - static = {got - ref_p99:+.2f} ms; {tr}")


def decode_p(code):
    """core's loss code (src/tickle.c hybrid_loss_decode): 0 reads as p = 1."""
    if code == 0:
        return 1.0
    p = (256 + (code & 0xFF)) / 512.0
    return p * 0.5 ** (code >> 8)


def p_convergence(r, runs):
    """Report only: after each loss step, the samples until the publisher's p (PEER_P) is within P_CONVERGE_FACTOR of
    the destination static arm's final p (its last PEER_P line, median over reps), summarized per destination phase,
    with the median p held at the end of those phases. A clean destination's static p is core's "none seen" floor,
    which an estimate that has seen a loss event cannot reach until its window forgets it."""
    st = send_times(r)
    if not st or not r["peer_p"]:
        return "no PEER_P lines or no trace"
    per = {}
    for k in range(1, len(r["phases"])):
        key = r["phases"][k]["key"]
        finals = sorted(decode_p(s["peer_p"][-1][1]) for s in usable(static_runs(runs, key, "rel_large"))
                        if s.get("peer_p"))
        if not finals:
            continue
        target = finals[len(finals) // 2]
        t_step = r["phases"][k]["t1"]
        t_end = r["phases"][k + 1]["t0"] if k + 1 < len(r["phases"]) else float("inf")
        inside = sorted(s for s, t in st.items() if t_step <= t < t_end)
        if not inside:
            continue
        first, last = inside[0], inside[-1]

        def near(code, target=target):
            return target / P_CONVERGE_FACTOR <= decode_p(code) <= target * P_CONVERGE_FACTOR

        def held(seq):  # the code in force when sample seq was published
            codes = [code for idx, code in r["peer_p"] if idx <= seq]
            return codes[-1] if codes else None

        hit = first if held(first) is not None and near(held(first)) else None
        if hit is None:
            hit = next((idx for idx, code in r["peer_p"] if first < idx <= last and near(code)), None)
        d = per.setdefault(key, {"target": target, "steps": 0, "hits": [], "end_p": []})
        d["steps"] += 1
        if hit is not None:
            d["hits"].append(hit - first)
        if held(last) is not None:
            d["end_p"].append(decode_p(held(last)))
    out = []
    for key, d in per.items():
        hits = sorted(d["hits"])
        end_p = sorted(d["end_p"])
        text = f"->{key} (static p {d['target']:.2e}): within {P_CONVERGE_FACTOR:g}x in {len(hits)}/{d['steps']} steps"
        text += f", median {hits[len(hits) // 2]} samples" if hits else ""
        text += f", p at phase end median {end_p[len(end_p) // 2]:.2e}" if end_p else ""
        out.append(text)
    return "; ".join(out) if out else "no step with a static target"


def v3(runs, arm, family, cell, emit):
    tv = [r for r in runs if r["arm"] == arm and r["cell"] == cell]
    if not tv:
        return
    phase_s = float(arm.rsplit("_p", 1)[1])
    window = min(V3_WINDOW_S, phase_s) * 1e9
    why = [f"{r['rep']}: {v}" for r in tv for v in r["void"]]
    for r in usable(tv):
        why += [f"{r['rep']}: {v}" for v in tv_phase_check(r, family)]
    # be_small's per-phase loss is the tv arm's own treatment check
    for r in usable([x for x in runs if x["arm"] == arm and x["cell"] == "be_small"]):
        why += [f"be_small {r['rep']}: {v}" for v in tv_phase_check(r, family)]
    if not [x for x in runs if x["arm"] == arm and x["cell"] == "be_small"] and not any(
            sum(x[1] for x in r["counters"].get("p", {}).get("bands", {}).values()) for r in usable(tv)):
        why.append("no be_small run and no tc drop on ping: the loss steps are not seen")
    why += treatment_void(runs, family, cell)
    statics = {}
    for ph in FAMILIES[family]:
        su = usable(static_runs(runs, ph, cell))
        g = static_goodput(su)
        if g is None:
            why.append(f"s_{ph}: no usable run")
            continue
        statics[ph] = (g, su)
    if why:
        emit("V3", arm, cell, "VOID", "; ".join(why))
        return
    worst_ph = min(statics, key=lambda k: statics[k][0])
    worst, wruns = statics[worst_ph]
    h1, h2 = halves_goodput(wruns)
    off = dl = 0
    conv = []
    for r in usable(tv):
        st = send_times(r)
        got = {s for s, _, _ in r["trace"]}
        for k in range(1, len(r["phases"])):
            a, b = r["phases"][k - 1]["key"], r["phases"][k]["key"]
            if PHASES[a][1] == PHASES[b][1]:
                continue
            t1 = r["phases"][k]["t1"]
            for s, t in st.items():
                if t1 <= t < t1 + window:
                    off += 1
                    dl += s in got
        if cell == "rel_large":
            conv.append(f"rep {r['rep']}: {p_convergence(r, runs)}")
    floor = V3_MIN_WINDOW_SMALL if cell != "rel_large" else V3_MIN_WINDOW_LARGE
    if h1 is None or h2 is None or abs(h1 - h2) > V3_GOODPUT_DROP_MAX * worst:
        emit("V3", arm, cell, "VOID", f"static s_{worst_ph} goodput halves {pct(h1)} / {pct(h2)} differ by more than "
             f"{V3_GOODPUT_DROP_MAX:.0%} of {pct(worst)}: the reference cannot decide")
        return
    if off < floor:
        emit("V3", arm, cell, "VOID", f"{off} samples offered in the transition windows, under {floor}")
        return
    g = dl / off
    text = (f"transition goodput {dl}/{off} = {pct(g)} (first {window / 1e9:g} s after each loss step); worst static "
            f"s_{worst_ph} {pct(worst)}; tv - worst = {pct(g - worst)} (limit -{V3_GOODPUT_DROP_MAX:.0%} relative); "
            "statics: " + ", ".join(f"s_{k} {pct(v[0])}" for k, v in statics.items()))
    if cell == "rel_large":
        text += "; hybrid p convergence (report only; n* is not printed by any node, p is its only reader input): " + \
            " | ".join(conv)
    else:
        text += "; hybrid n*: not applicable to this cell (no fragmented samples)"
    emit("V3", arm, cell, "PASS" if g >= (1 - V3_GOODPUT_DROP_MAX) * worst else "FAIL", text)


def channel_line(runs, arm, family):
    """be_small per phase, printed: the channel the tv arm actually produced."""
    out = []
    for r in usable([x for x in runs if x["arm"] == arm and x["cell"] == "be_small"]):
        st = send_times(r)
        got = {s for s, _, _ in r["trace"]}
        per = {}
        for s, t in st.items():
            k = phase_at(r["phases"], t)
            if k is None:
                continue
            o, d = per.get(r["phases"][k]["key"], (0, 0))
            per[r["phases"][k]["key"]] = (o + 1, d + (s in got))
        out.append(f"rep {r['rep']}: " + ", ".join(f"{k} loss {pct(1 - d / o)} of {o}" for k, (o, d) in per.items()))
    return "; ".join(out)


def void_behind(rule, arm, cell, cr, emit, judge):
    """VOID for the control's resends, with what the rule reads past the control printed (not scored): the resends
    are a real effect, and the verdict they hide is part of the finding."""
    got = []
    judge(lambda *a: got.append(a))
    behind = "; ".join(f"{v} {t}" for _, _, _, v, t in got) or "nothing"
    emit(rule, arm, cell, "VOID", "the clean control resends: " + "; ".join(cr) + f" || read past the control (not "
         f"scored): {behind}")


def main():
    if len(sys.argv) != 2:
        print("usage: time_varying_read.py OUT", file=sys.stderr)
        return 2
    out = sys.argv[1]
    runs, dur = load(out)
    lines = []

    def emit(rule, arm, cell, verdict, text):
        lines.append((rule, arm, cell, verdict))
        print(f"{rule:3} {arm:7} {cell:9} {verdict:6} {text}")

    print(f"=== time_varying_read {out}: {len(runs)} runs, dur {dur:g} s; thresholds are a DRAFT pending the user")
    for r in runs:
        if r["void"]:
            print(f"VOID run {r['arm']}-{r['cell']}-{r['rep']}: {'; '.join(r['void'])}")
    cv = control_void(runs)
    cr, cr_counts = control_resends(runs)
    if cr_counts:
        print(f"CONTROL_RESENDS s_l0 rel_large ({'resends: rel_large verdicts VOID' if cr else 'none'}): "
              + "; ".join(cr_counts))
    tv_arms = sorted({r["arm"] for r in runs if r["tv"]}, key=lambda a: (list(FAMILIES).index(a.rsplit("_p", 1)[0])
                                                                         if a.rsplit("_p", 1)[0] in FAMILIES else 99,
                                                                         float(a.rsplit("_p", 1)[1])))
    for arm in tv_arms:
        family = arm.rsplit("_p", 1)[0]
        if family not in FAMILIES:
            print(f"VOID arm {arm}: unknown family")
            continue
        for rule in RULES[family]:
            if cv:
                for cell in (("rel_small",) if rule == "V2" else COPY_CELLS):
                    emit(rule, arm, cell, "VOID", "the clean control: " + "; ".join(cv))
                continue
            if rule in ("V1", "V4"):
                for cell in COPY_CELLS:
                    if cell == "rel_large" and cr:
                        void_behind(rule, arm, cell, cr, emit, lambda e, c=cell, rule=rule: copies_rule(
                            rule, V1_COPIES_MAX if rule == "V1" else V4_COPIES_MAX, runs, arm, family, c, e))
                        continue
                    copies_rule(rule, V1_COPIES_MAX if rule == "V1" else V4_COPIES_MAX, runs, arm, family, cell, emit)
            elif rule == "V2":
                v2(runs, arm, family, emit)
            elif rule == "V3":
                for cell in V3_CELLS:
                    if cell == "rel_large" and cr:
                        void_behind(rule, arm, cell, cr, emit, lambda e, c=cell: v3(runs, arm, family, c, e))
                        continue
                    v3(runs, arm, family, cell, emit)
        ch = channel_line(runs, arm, family)
        if ch:
            print(f"    {arm} channel (be_small per phase): {ch}")
    print("--- summary")
    for rule in ("V1", "V2", "V3", "V4"):
        got = [x for x in lines if x[0] == rule]
        if not got:
            print(f"{rule}: no tv arm of its families in this OUT")
            continue
        print(f"{rule}: " + ", ".join(f"{a}/{c} {v}" for _, a, c, v in got))
    print("V5 (service call retries): not measured by this harness (plan decision 3)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
