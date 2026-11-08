#!/usr/bin/env python3
"""x2_step_read.py OUT - the reader of x2_step_netns.sh (release stage 3, X2 "step response"; 2026-11-08).

PRE-REGISTERED RULES. Written before any X2 run; this code, not the harness header or the plan text, decides. Source:
~/claude_reports/TIME_VARYING_EXPERIMENT_PLAN_2026-11-03.md section 7 decision 2 (cost-based convergence criteria,
which replace the "20 RTT, +-10%" draft of section X2) and section 8 decision 2 (the A/A rule). One factor changes per
arm, one step each way in a run: delay 1 -> 20 -> 1 ms (family D, with a constant 2% background loss: a delay step
with nothing to repair exercises no repair rule), random loss 0 -> 5% -> 0 (family LB), jitter 0 -> +-5 ms -> 0
(family J). Every verdict is (tv value - worst static phase value), as time_varying_read.py; a static phase that
already fails the same number reads STATIC, not a step failure.

  1. CONVERGENCE TIME is REPORTED ONLY, never judged (section 7.2 last line, section 8.2). Per step and cell, from
     the estimator trace (ESTTRACE, every 100 ms): the time from the end of the change (t_after) until the signal
     stays within +-10% (CONV_BAND) of its own value at the end of that phase (the last CONV_TAIL of the phase), in ms
     and in units of the destination round trip. D: the data reader's recovery_srtt_ns; J: its recovery_rttvar_ns;
     LB: the large publisher's hybrid_p for the reader (rel_large only), within a FACTOR of 2 (CONV_P_FACTOR) as the
     section 7.2 "hybrid p within 2x" line, reported too. The writer-proxy of a run is the one whose signal moved most.
  2. COPIES AFTER DELIVERY (V1 on D's up step, V4 on J), rel_small and rel_large: copies / delivered, tv minus the
     worst static phase <= 1% (R7's limit). Computed by time_varying_read.copies_rule, the T0 code, unchanged.
  3. NO FALSE REPAIR (D up step, rel_small; section 7.2 "at most one fake copy per gap"): copies after delivery per
     gap the reader opened (RSTATS gaps_opened) <= 1.0 (X2_COPIES_PER_GAP_MAX).
  4. REPAIR WAIT p99 (V2, D down step, rel_small): the p99 of the one-way delay over its p50 for samples sent from
     2 s after the step to the next step <= 1.5 x the destination static arm's own p99 (time_varying_read.v2).
     DELAY DECREASE (section 7.2): deadline goodput (rel_small, 100 ms from creation) of the samples sent from the
     step's start to the end of the phase not below the preceding phase's (d20) static value (X2_DOWN_GOODPUT).
  5. LOSS STEP (V3, LB): deadline goodput of the samples sent in the 2 s after each step >= 90% of the worst static
     phase's (time_varying_read.v3, unchanged; hybrid p convergence is reported there and in rule 1).
  6. A/A NOISE RULE (section 8.2, "B-A over the A/A half-to-half difference"): V1, V2, V4, NOFALSE and DOWNGOOD
     FAIL only when over the limit AND tv - worst static exceeds the A/A spread: the same quantity on the odd reps
     against the even reps (interleaved in time), tv and static alike. Over the limit inside the spread reads NOISE
     (undecided, not a pass). "Spread" is that odd-vs-even difference in full (the T0 code's reading, kept for V1, V2,
     V4); a stricter reading, half of it, is X2_AA_FRACTION = 0.5. V3 keeps its own 10% relative margin.
  7. SAMPLE COUNT: D and LB need 8 usable reps in every cell, J 4 (plan X0c; AA_MIN_REPS*), else VOID. Plus all the
     VOID rules of time_varying_read.py (clean control s_l0 losing anything, treatment not seen, a missing RESULT
     line, short trace, under 2 phase changes inside the samples' interval, the PC loaded).

Output: one line per (rule, arm, cell) "<rule> <arm> <cell> PASS|FAIL|NOISE|STATIC|VOID|N/A text", then "X2 TABLE"
(rule x arm x cell verdicts), "X2 CONV" (the convergence times), "X2 SUMMARY".
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import time_varying_read as tv  # noqa: E402  (the T0 loader and rules; reused, not copied)
from wifi_burst_read import kv, num, quantile, read_lines  # noqa: E402

# --- thresholds (change only before a run, and say so in the commit) ----------------------------------------------
X2_COPIES_PER_GAP_MAX = 1.0  # rule 3: copies after delivery per reader gap
X2_DOWN_GOODPUT_DEADLINE_MS = 100.0  # rule 4b: the rel_small deadline of V3
X2_AA_FRACTION = 1.0  # rule 6: NOFALSE / DOWNGOOD read NOISE inside this share of the A/A spread (V1, V2, V4: T0, 1.0)
CONV_BAND = 0.10  # rule 1: +-10% of the phase-end value
CONV_TAIL = 0.30  # rule 1: the phase-end value is the median over the last 30% of the phase
CONV_P_FACTOR = 2.0  # rule 1: hybrid p within this factor
REPS_REQUIRED = {"D": 8, "LB": 8, "J": 4}
FAMILIES_X2 = ("D", "LB", "J")
FAMILY_RULES = {"D": ["V1", "NOFALSE", "V2", "DOWNGOOD"], "LB": ["V3"], "J": ["V4"]}
CONV_SIGNAL = {"D": ("wproxy", "recovery_srtt_ns"), "J": ("wproxy", "recovery_rttvar_ns"), "LB": ("peer", "hybrid_p")}


def pct(x):
    return tv.pct(x)


def split_reps(rs, parity):
    return [r for r in rs if int(r["rep"]) % 2 == parity]


def aa_spread(tvu, su, fn, fn_static=None):
    """|odd - even| of fn(tv reps) - fn_static(static reps): the A/A spread of tv - static."""
    d = []
    for par in (1, 0):
        a = fn(split_reps(tvu, par))
        b = (fn_static or fn)(split_reps(su, par))
        d.append((a or 0.0) - (b or 0.0))
    return abs(d[0] - d[1])


def reps_short(runs, arm, family, cell):
    need = REPS_REQUIRED[family]
    have = len({r["rep"] for r in tv.usable([x for x in runs if x["arm"] == arm and x["cell"] == cell])})
    return f"{have} usable reps in {cell}, under the {need} the plan asks for" if have < need else None


# --- rule 3: no false repair --------------------------------------------------------------------------------------


def copies_per_gap(rs):
    gaps = [tv.x1_rs(r, "gaps_opened") for r in rs]
    if not rs or None in gaps:
        return None
    g = sum(gaps)
    return sum(r["copies"] for r in rs) / g if g else None


def no_false_repair(runs, arm, family, emit):
    cell = "rel_small"
    mine = [r for r in runs if r["arm"] == arm and r["cell"] == cell]
    if not mine:
        return
    why = [f"{r['rep']}: {v}" for r in mine for v in r["void"]]
    short = reps_short(runs, arm, family, cell)
    if short:
        why.append(short)
    why += tv.treatment_void(runs, family, cell)
    tvu = tv.usable(mine)
    statics = {}
    for ph in tv.FAMILIES[family]:
        su = tv.usable(tv.static_runs(runs, ph, cell))
        v = copies_per_gap(su)
        if v is None:
            why.append(f"s_{ph}: no gaps_opened count (field absent) or no usable run")
        else:
            statics[ph] = (v, su)
    got = copies_per_gap(tvu)
    if got is None:
        why.append("tv: no gaps_opened count (field absent)")
    if why:
        emit("NOFALSE", arm, cell, "VOID", "; ".join(why))
        return
    worst_ph = max(statics, key=lambda k: statics[k][0])
    worst, wruns = statics[worst_ph]
    spread = aa_spread(tvu, wruns, copies_per_gap)
    text = (f"copies after delivery per reader gap {got:.2f} (limit {X2_COPIES_PER_GAP_MAX:g}); worst static "
            f"s_{worst_ph} {worst:.2f}; tv - worst {got - worst:+.2f}; A/A spread {spread:.2f}; statics: "
            + ", ".join(f"s_{k} {v[0]:.2f}" for k, v in statics.items()))
    if worst > X2_COPIES_PER_GAP_MAX:
        emit("NOFALSE", arm, cell, "STATIC", text + f" - s_{worst_ph} already over the limit")
    elif got <= X2_COPIES_PER_GAP_MAX:
        emit("NOFALSE", arm, cell, "PASS", text)
    elif got - worst <= X2_AA_FRACTION * spread:
        emit("NOFALSE", arm, cell, "NOISE", text + " - over the limit but inside the A/A spread: undecided")
    else:
        emit("NOFALSE", arm, cell, "FAIL", text)


# --- rule 4b: delay decrease, deadline goodput not below the preceding phase's static value --------------------------


def down_window_goodput(rs, deadline):
    """Deadline goodput of the samples sent from each down step's start to the next change (or the end)."""
    off = ok = 0
    for r in rs:
        st = tv.send_times(r)
        got = tv.on_time(r, deadline)
        for k in tv.down_steps(r, "D"):
            t0 = r["phases"][k]["t0"]
            t1 = r["phases"][k + 1]["t0"] if k + 1 < len(r["phases"]) else float("inf")
            for s, t in st.items():
                if t0 <= t < t1:
                    off += 1
                    ok += s in got
    return (ok / off) if off else None


def down_goodput(runs, arm, family, emit):
    cell = "rel_small"
    mine = [r for r in runs if r["arm"] == arm and r["cell"] == cell]
    if not mine:
        return
    prev = tv.FAMILIES[family][-1]  # the phase the down step leaves (d20)
    why = [f"{r['rep']}: {v}" for r in mine for v in r["void"] + r["lat_void"]]
    short = reps_short(runs, arm, family, cell)
    if short:
        why.append(short)
    why += tv.treatment_void(runs, family, cell)
    tvu = [r for r in tv.usable(mine) if not r["lat_void"]]
    su = [r for r in tv.usable(tv.static_runs(runs, prev, cell)) if not r["lat_void"]]
    dl = X2_DOWN_GOODPUT_DEADLINE_MS
    ref = tv.static_goodput(su, dl)
    got = down_window_goodput(tvu, dl)
    if ref is None or got is None:
        why.append("no goodput to compare (no usable static or no down-step samples)")
    if why:
        emit("DOWNGOOD", arm, cell, "VOID", "; ".join(why))
        return
    spread = aa_spread(tvu, su, lambda rs: down_window_goodput(rs, dl), lambda rs: tv.static_goodput(rs, dl))
    text = (f"deadline goodput ({dl:g} ms) after the down step {pct(got)} vs the preceding static s_{prev} {pct(ref)}; "
            f"tv - static {pct(got - ref)}; A/A spread {pct(spread)}")
    if got >= ref:
        emit("DOWNGOOD", arm, cell, "PASS", text)
    elif ref - got <= X2_AA_FRACTION * spread:
        emit("DOWNGOOD", arm, cell, "NOISE", text + " - below the static but inside the A/A spread: undecided")
    else:
        emit("DOWNGOOD", arm, cell, "FAIL", text)


# --- rule 1: convergence time (ESTTRACE), reported only -----------------------------------------------------------------


def esttrace(path, obj, key):
    """{(group): [(t_ns, value)]} of one obj's lines; group is its sub/pub id."""
    out = {}
    for line in read_lines(path):
        if not line.startswith("ESTTRACE ") or f" obj={obj} " not in line:
            continue
        d = kv(line)
        if d.get("reliable") == "0" or key not in d:
            continue
        try:
            out.setdefault(d.get("sub") or d.get("pub") or "?", []).append((int(d["t_ns"]), float(d[key])))
        except ValueError:
            continue
    return out


def pick_series(groups):
    """The writer proxy / reader entry whose signal moved most in the run (the others are builtin or idle)."""
    best = None
    for rows in groups.values():
        vals = [v for _, v in rows if v > 0]
        if len(vals) >= 10:
            span = max(vals) / min(vals)
            if best is None or span > best[0]:
                best = (span, rows)
    return best[1] if best else None


def converge_step(rows, t_after, t_end, family):
    """(ms from t_after until the signal stays in the band of its phase-end value, phase-end value) or (None, v)."""
    win = [(t, v) for t, v in rows if t_after <= t < t_end]
    if len(win) < 5:
        return None, None
    tail = sorted(v for t, v in win if t >= t_end - CONV_TAIL * (t_end - t_after))
    if not tail:
        return None, None
    target = tail[len(tail) // 2]
    if target <= 0:
        return None, target

    def inside(v):
        if family == "LB":
            return target / CONV_P_FACTOR <= v <= target * CONV_P_FACTOR
        return abs(v - target) <= CONV_BAND * target

    last_out = None
    for t, v in win:
        if not inside(v):
            last_out = t
    if last_out is None:
        return 0.0, target
    after = [t for t, _ in win if t > last_out]
    return ((after[0] - t_after) / 1e6 if after else None), target


def convergence(out, runs, family, arm, cell):
    """Per step: list of (ms, target) over the reps of one tv arm and cell."""
    obj, key = CONV_SIGNAL[family]
    if obj == "peer" and cell != "rel_large":
        return None  # hybrid p exists only with fragmented samples
    per_step = {}
    for r in tv.usable([x for x in runs if x["arm"] == arm and x["cell"] == cell]):
        d = os.path.join(out, "runs", f"{arm}-{cell}-{r['rep']}")
        path = os.path.join(d, "ping.err" if obj == "peer" else "echo.err")
        series = pick_series(esttrace(path, obj, key))
        if series is None:
            continue
        ph = r["phases"]
        for k in range(1, len(ph)):
            t_end = ph[k + 1]["t0"] if k + 1 < len(ph) else series[-1][0] + 1
            ms, target = converge_step(series, ph[k]["t1"], t_end, family)
            per_step.setdefault(k, []).append((ms, target, ph[k]["key"]))
    return per_step


def conv_lines(out, runs, tv_arms):
    lines = []
    for arm in tv_arms:
        family = arm.rsplit("_p", 1)[0]
        for cell in tv.ALL_CELLS:
            per = convergence(out, runs, family, arm, cell)
            if per is None or not per:
                lines.append(f"X2 CONV {arm:7} {cell:9} n/a (no trace of {CONV_SIGNAL[family][1]} for this cell)")
                continue
            for k, vals in sorted(per.items()):
                got = sorted(v[0] for v in vals if v[0] is not None)
                key = vals[0][2]
                tg = sorted(v[1] for v in vals if v[1])
                if not got:
                    lines.append(f"X2 CONV {arm:7} {cell:9} step {k} ->{key}: not within the band by the phase end in "
                                 f"any of {len(vals)} reps (report only)")
                    continue
                med = quantile(got, 0.5)
                rtt = ""
                if family == "D" and tg:
                    rtt = f" = {med / (quantile(tg, 0.5) / 1e6):.1f} x the phase-end srtt {quantile(tg, 0.5) / 1e6:.2f} ms"
                lines.append(f"X2 CONV {arm:7} {cell:9} step {k} ->{key}: {CONV_SIGNAL[family][1]} settled after "
                             f"{med:.0f} ms median ({got[0]:.0f}..{got[-1]:.0f}, {len(got)}/{len(vals)} reps){rtt} "
                             "(report only)")
    return lines


# --- main ------------------------------------------------------------------------------------------------------------


def main():
    if len(sys.argv) != 2:
        print("usage: x2_step_read.py OUT", file=sys.stderr)
        return 2
    out = sys.argv[1]
    runs, dur = tv.load(out)
    rows = []

    def emit(rule, arm, cell, verdict, text):
        rows.append((rule, arm, cell, verdict))
        print(f"{rule:8} {arm:7} {cell:9} {verdict:6} {text}")

    print(f"=== x2_step_read {out}: {len(runs)} runs, dur {dur:g} s; rules are in this file's docstring")
    for r in runs:
        if r["void"]:
            print(f"VOID run {r['arm']}-{r['cell']}-{r['rep']}: {'; '.join(r['void'])}")
    cv = tv.control_void(runs)
    cr, cr_counts = tv.control_resends(runs)
    if cr_counts:
        print(f"CONTROL_RESENDS s_l0 rel_large ({'resends: rel_large verdicts VOID' if cr else 'none'}): "
              + "; ".join(cr_counts))
    tv_arms = sorted({r["arm"] for r in runs if r["tv"] and r["arm"].rsplit("_p", 1)[0] in FAMILIES_X2},
                     key=lambda a: (FAMILIES_X2.index(a.rsplit("_p", 1)[0]), a))
    for arm in tv_arms:
        family = arm.rsplit("_p", 1)[0]
        for rule in FAMILY_RULES[family]:
            cells = ("rel_small",) if rule in ("V2", "NOFALSE", "DOWNGOOD") else tv.COPY_CELLS
            if cv:
                for cell in cells:
                    emit(rule, arm, cell, "VOID", "the clean control: " + "; ".join(cv))
                continue
            for cell in cells:
                short = reps_short(runs, arm, family, cell)
                if rule in ("V1", "V4", "V3", "V2") and short:
                    emit(rule, arm, cell, "VOID", short)
                    continue
                if rule == "V2":
                    tv.v2(runs, arm, family, emit)
                elif rule == "NOFALSE":
                    no_false_repair(runs, arm, family, emit)
                elif rule == "DOWNGOOD":
                    down_goodput(runs, arm, family, emit)
                elif rule == "V3":
                    judge = lambda e, c=cell: tv.v3(runs, arm, family, c, e)  # noqa: E731
                    if cell == "rel_large" and cr:
                        tv.void_behind(rule, arm, cell, cr, emit, judge)
                    else:
                        judge(emit)
                else:  # V1, V4
                    limit = tv.V1_COPIES_MAX if rule == "V1" else tv.V4_COPIES_MAX
                    judge = lambda e, c=cell, rule=rule, limit=limit: tv.copies_rule(  # noqa: E731
                        rule, limit, runs, arm, family, c, e)
                    if cell == "rel_large" and cr:
                        tv.void_behind(rule, arm, cell, cr, emit, judge)
                    else:
                        judge(emit)
        ch = tv.channel_line(runs, arm, family)
        if ch:
            print(f"    {arm} channel (be_small per phase): {ch}")
    print("--- X2 TABLE (rule x arm x cell)")
    for rule, arm, cell, verdict in rows:
        print(f"X2 TABLE {rule:8} {arm:7} {cell:9} {verdict}")
    print("--- X2 CONV (convergence time, reported only)")
    for line in conv_lines(out, runs, tv_arms):
        print(line)
    print("--- X2 SUMMARY")
    for rule in ("V1", "NOFALSE", "V2", "DOWNGOOD", "V3", "V4"):
        got = [x for x in rows if x[0] == rule]
        if got:
            print(f"X2 SUMMARY {rule}: " + ", ".join(f"{a}/{c} {v}" for _, a, c, v in got))
    return 0


if __name__ == "__main__":
    sys.exit(main())
