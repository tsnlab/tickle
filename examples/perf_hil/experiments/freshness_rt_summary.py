#!/usr/bin/env python3
"""KEEP_LAST freshness by ROUND TRIP: the reading of keep_last_freshness_rt.sh (rig) and freshness_rt_pc.sh (PC).

The user's decision D3 (2026-10-18): a KEEP_LAST cell is scored first by FRESHNESS - the age of a sample when the
reader application takes it (p50, p99) and the share of the sender's bursts whose newest sample was taken; a superseded
sample is not a loss. The user's rule of 2026-10-19: across hosts only ROUND TRIPS are timed (the hosts' clocks drift
and Ethernet PTP's error is large), so the age is read on the SENDER's clock alone (tickle/common/BenchFreshRt.h):

    round-trip age = t_echo_rx - t_create      (both CLOCK_MONOTONIC on the sender)

where t_create is the burst's due time and t_echo_rx the arrival of the reader's ECHO, sent over a plain side UDP
socket (not the middleware, the same code for all three frameworks) the moment the application took the sample. The
side channel's own delay is measured in the same run by a PING/PONG on the same socket pair (every 20 ms).

Written 2026-10-19 BEFORE any run, enforced by the code below (--selftest exercises every rule); --prereg prints this.

  R0  SAMPLE COUNTS FIRST, printed before any value. A run is VOID unless it has the client's and the server's RESULT
      lines, both with fresh_rt=on, the sender saw its peer (fresh_rt_peer=yes) and no foreign datagram
      (fresh_rt_bad=0), and the reader's own accounting closes (echo_sent + echo_fail + echo_skipped = fresh_taken).
      ECHO LOSS: lost = reader echo_sent - sender echoes. VOID if echo_fail > 0, echo_skipped > 0, lost < 0, or
      lost > floor(ECHO_LOSS_FRAC x echo_sent) (0.1%: on an unshaped link no echo should be lost; a 160-take run may lose
      none). Then per metric, per run:
        age p50 read only with >= MIN_AGES_P50 (50) in-window echoes;
        age p99 read only with >= MIN_AGES_P99 (300): (1 - 0.01)^n is the chance that no sample of the run lies beyond
          the true p99, and (0.99)^300 = 0.049 < 0.05 - with the published 160 samples it is 0.20, so rows 43/44 as
          published are scored on p50 and newest share, and p99 is read from the b8/b16 cells (800/1600 samples);
        newest share read only over >= MIN_BURSTS (100) scheduled bursts (a resolution of 1%);
        the side channel read only with >= MIN_PONGS (100) pongs, and pongs = pings - (at most 2 in flight at the end).
      A cell x framework is read with >= MIN_REPS readable runs (rig 3, --pc 2).
  R0b THE TICKLE READER'S STALL (added 2026-10-20 with the reshaped server, before its rig run): TickLE's server now
      matches, stalls consumption for -p into a KEEP_LAST 8 cache, then takes, as the twins' readers; its RESULT line
      reports stalled, depth and evicted. A TickLE run is VOID unless stalled=1 (it saw a sample and its stall ended),
      lost = evicted (every seq it did not take was evicted by the cache, none missing otherwise) and, in h44, evicted
      > 0 (a 3 s stall at 20 samples/s that evicts nothing did not stall; KEEP_LAST 8 predicts 61 - 8 = 53). Until then
      TickLE's h44 "taken of 160" was when its Subscriber joined, not the depth (history_depth_pc_check.sh); it now
      reads as the twins' do, the newest 8 of the stall plus everything after it. The taken counts are printed as
      COUNT lines (not scored).
  R1  THE CONTROL (the side channel, framework-independent: no middleware on its path). Per cell, over every readable
      run of every framework: spread50 = max - min of side RTT p50, spread99 = the same for side p99. A difference
      between two frameworks' ages is attributed to the frameworks only when the gap between their rep ranges exceeds
      the spread of the control in that cell (spread50 for age p50, spread99 for age p99); a gap that does not is VOID
      (the return path moved as much as the difference), never WIN or LOSE.
  R2  THE FAULT ARM, `dly`: the h43 cell with `tc netem delay 1ms` on the sender's eth0 (the data and the PINGs leave
      through it; the ECHOes and PONGs do not). Each run's realized qdisc is recorded beside it (QDISC lines): a dly run
      whose qdisc does not read "netem ... delay 1ms", or any other run whose qdisc has netem, is VOID (an arm that does
      not report its treatment is not a result). Per framework, with the median over readable runs:
        d_age  = med(age p50, dly) - med(age p50, h43)     d_side = med(side p50, dly) - med(side p50, h43)
      PASS iff |d_side - 1000 us| <= FAULT_TOL_US (250) and |d_age - 1000 us| <= FAULT_TOL_US and
      |d_age - d_side| <= FAULT_TOL_US: the instrument moves by a known path change, and the control moves identically.
      Every framework must PASS, or the session is NOT READ (an instrument that cannot see a 1 ms change measures
      nothing). No dly cell, or too few readable dly/h43 runs: NOT READ.
  R3  THE METRICS, per cell and framework over readable runs: round-trip age p50 and p99 (us, lower is fresher), newest
      share = sender fresh_rt_newest / sender fresh_bursts (higher is fresher). DIAGNOSTIC ONLY, never scored, printed
      labelled: age p50 - side RTT p50 / 2 (a one-way estimate), the reader's own one-way fresh_age_p50_us (same host
      only), and goodput (server recv / client sent).
  R4  THE VERDICT per cell, metric and vendor (FastDDS, CycloneDDS), docs/TESTING.md section 4 by rep ranges: WIN when
      every TickLE run is better than every run of the vendor and the gap exceeds the control's spread (R1); LOSE in
      the mirror case; VOID when the ranges are disjoint but the gap is within the spread; TIE when the ranges touch or
      overlap. newest share has no control (a count, not a time): WIN/LOSE/TIE by ranges alone. Per cell and metric:
      LOSE if any vendor's is LOSE; else VOID if any is VOID; else WIN if every scored vendor's is WIN; else TIE. A
      vendor NOT READ is listed and left out; TickLE NOT READ because it took nothing in the window is LOSE, NOT READ
      otherwise. Each verdict line carries the realized treatment: the echoes received per run.
  FALSIFIED (the instrument, not a framework): the fault arm fails (R2).

Usage:  freshness_rt_summary.py [--pc] <results file>      freshness_rt_summary.py --selftest | --prereg
Exit status: 0 when the session is READ (fault arm passed for every framework), 1 when NOT READ, 2 on bad input.
"""
import argparse
import math
import re
import statistics
import sys

MIN_AGES_P50 = 50
MIN_AGES_P99 = 300
MIN_BURSTS = 100
MIN_PONGS = 100
PONGS_IN_FLIGHT = 2
MIN_REPS = 3
MIN_REPS_PC = 2
ECHO_LOSS_FRAC = 0.001
FAULT_DELAY_US = 1000.0
FAULT_TOL_US = 250.0
SCORED_CELLS = ("h43", "h44", "b8", "b16")
FAULT_CELL = "dly"
FAULT_BASE_CELL = "h43"
STALL_CELL = "h44"
SCORED_VENDORS = ("fastdds", "cyclonedds")
FRAMEWORKS = ("tickle",) + SCORED_VENDORS
# metric, direction, the control field whose spread a time gap must exceed (None: no control)
METRICS = (("age_p50", "lower", "side_p50"), ("age_p99", "lower", "side_p99"), ("newest_share", "higher", None))

KV = re.compile(r"(\w+)=(\S+)")


def fields(line):
    return dict(KV.findall(line))


def num(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def parse(text):
    """FRESH/QDISC lines -> {run id: run dict}."""
    runs = {}
    for line in text.splitlines():
        m = re.match(r"FRESH run=(\d+) cell=(\S+) fw=(\w+) rep=(\d+)", line)
        if m:
            rid = int(m.group(1))
            runs[rid] = {"id": rid, "cell": m.group(2), "fw": m.group(3), "rep": int(m.group(4)),
                         "client": None, "server": None, "qdisc": None}
            continue
        m = re.match(r"QDISC run=(\d+) (?!after )(.*)", line)
        if m and int(m.group(1)) in runs:
            runs[int(m.group(1))]["qdisc"] = m.group(2)
            continue
        m = re.match(r"FRESH run=(\d+) RESULT: (.*)", line)
        if m and int(m.group(1)) in runs:
            f = fields(m.group(2))
            role = f.get("role")
            if role in ("client", "server"):
                runs[int(m.group(1))][role] = f
    return runs


def qdisc_ok(run):
    """R2's realized treatment: (ok, why)."""
    q = run["qdisc"]
    if q is None:
        return False, "no QDISC line"
    if run["cell"] == FAULT_CELL:
        if "netem" in q and re.search(r"delay 1(\.0+)?ms", q):
            return True, ""
        return False, f"dly run without netem delay 1ms ({q.strip()})"
    if "netem" in q or "tbf" in q:
        return False, f"shaped link ({q.strip()})"
    return True, ""


def tickle_stall_void(cell, s):
    """R0b: why a TickLE server line is not the reshaped reader's stall, or None."""
    if s.get("stalled") != "1":
        return f"TickLE reader did not stall (stalled={s.get('stalled')})"
    lost, evicted = num(s.get("lost")), num(s.get("evicted"))
    if lost is None or evicted is None or lost != evicted:
        return f"TickLE lost {s.get('lost')} != evicted {s.get('evicted')}"
    if cell == STALL_CELL and evicted <= 0:
        return f"TickLE {STALL_CELL} evicted nothing: the stall did not bite"
    return None


def read_run(run):
    """R0 -> the run with 'void' (reason or None) and the readable metrics."""
    c, s = run["client"], run["server"]
    run["metrics"] = {}
    run["void"] = None
    run["echo_lost"] = None
    if c is None or s is None:
        run["void"] = "missing RESULT line(s)"
        return run
    if c.get("fresh_rt") != "on" or s.get("fresh_rt") != "on":
        run["void"] = f"fresh_rt not on (client {c.get('fresh_rt')}, server {s.get('fresh_rt')})"
        return run
    if c.get("fresh_rt_peer") != "yes":
        run["void"] = "the sender never heard from the reader"
        return run
    if num(c.get("fresh_rt_bad")) != 0:
        run["void"] = f"fresh_rt_bad={c.get('fresh_rt_bad')}"
        return run
    sent = num(s.get("fresh_rt_echo_sent"))
    fail = num(s.get("fresh_rt_echo_fail"))
    skipped = num(s.get("fresh_rt_echo_skipped"))
    taken = num(s.get("fresh_taken"))
    got = num(c.get("fresh_rt_echoes"))
    if None in (sent, fail, skipped, taken, got):
        run["void"] = "echo counters missing"
        return run
    if sent + fail + skipped != taken:
        run["void"] = f"reader accounting does not close: sent {sent:.0f} + fail {fail:.0f} + skipped {skipped:.0f} != taken {taken:.0f}"
        return run
    lost = sent - got
    run["echo_lost"] = lost
    if fail > 0 or skipped > 0:
        run["void"] = f"echo_fail={fail:.0f} echo_skipped={skipped:.0f}"
        return run
    if lost < 0 or lost > math.floor(ECHO_LOSS_FRAC * sent):
        run["void"] = f"echoes lost {lost:.0f} of {sent:.0f} (allowed {math.floor(ECHO_LOSS_FRAC * sent)})"
        return run
    ok, why = qdisc_ok(run)
    if not ok:
        run["void"] = why
        return run
    if run["fw"] == "tickle":
        why = tickle_stall_void(run["cell"], s)
        if why:
            run["void"] = why
            return run
    n_age = num(c.get("fresh_rt_in_window")) or 0
    pings, pongs = num(c.get("fresh_rt_pings")) or 0, num(c.get("fresh_rt_pongs")) or 0
    bursts = num(c.get("fresh_bursts")) or 0
    m = run["metrics"]
    if n_age >= MIN_AGES_P50:
        m["age_p50"] = num(c.get("fresh_rt_age_p50_us"))
    if n_age >= MIN_AGES_P99:
        m["age_p99"] = num(c.get("fresh_rt_age_p99_us"))
    if bursts >= MIN_BURSTS:
        m["newest_share"] = (num(c.get("fresh_rt_newest")) or 0) / bursts
    if pongs >= MIN_PONGS and pings - pongs <= PONGS_IN_FLIGHT:
        m["side_p50"] = num(c.get("fresh_rt_side_p50_us"))
        m["side_p99"] = num(c.get("fresh_rt_side_p99_us"))
    else:
        run["void"] = f"control not read: pings {pings:.0f} pongs {pongs:.0f}"
        run["metrics"] = {}
        return run
    m = {k: v for k, v in m.items() if v is not None}
    run["metrics"] = m
    run["n_age"], run["bursts"], run["pongs"], run["echoes"] = n_age, bursts, pongs, got
    if "age_p50" in m and "side_p50" in m:
        run["diag_oneway"] = m["age_p50"] - m["side_p50"] / 2
    run["diag_reader_oneway"] = num(s.get("fresh_age_p50_us"))
    run["goodput"] = (num(s.get("recv")) or 0) / max(1.0, num(c.get("sent")) or 0)
    run["taken"], run["sent"], run["evicted"] = num(s.get("recv")), num(c.get("sent")), num(s.get("evicted"))
    run["took_nothing"] = n_age == 0
    return run


def values(runs, cell, fw, metric):
    return [r["metrics"][metric] for r in runs if r["cell"] == cell and r["fw"] == fw and r["void"] is None
            and metric in r["metrics"]]


def compare(t_vals, v_vals, direction, spread):
    """R4 for one vendor: WIN / LOSE / TIE / VOID(control)."""
    better = (lambda a, b: a < b) if direction == "lower" else (lambda a, b: a > b)
    t_best, t_worst = (min(t_vals), max(t_vals)) if direction == "lower" else (max(t_vals), min(t_vals))
    v_best, v_worst = (min(v_vals), max(v_vals)) if direction == "lower" else (max(v_vals), min(v_vals))
    if better(t_worst, v_best):
        gap = abs(v_best - t_worst)
        return "WIN" if spread is None or gap > spread else "VOID(control)"
    if better(v_worst, t_best):
        gap = abs(t_best - v_worst)
        return "LOSE" if spread is None or gap > spread else "VOID(control)"
    return "TIE"


def fault_arm(runs, min_reps):
    """R2 -> (passed for every framework, lines)."""
    lines, all_pass = [], True
    for fw in FRAMEWORKS:
        a_dly, a_base = values(runs, FAULT_CELL, fw, "age_p50"), values(runs, FAULT_BASE_CELL, fw, "age_p50")
        s_dly, s_base = values(runs, FAULT_CELL, fw, "side_p50"), values(runs, FAULT_BASE_CELL, fw, "side_p50")
        if min(len(a_dly), len(a_base), len(s_dly), len(s_base)) < min_reps:
            lines.append(f"FAULT fw={fw} NOT READ: readable runs dly {len(a_dly)} h43 {len(a_base)} (need {min_reps})")
            all_pass = False
            continue
        d_age = statistics.median(a_dly) - statistics.median(a_base)
        d_side = statistics.median(s_dly) - statistics.median(s_base)
        ok = (abs(d_side - FAULT_DELAY_US) <= FAULT_TOL_US and abs(d_age - FAULT_DELAY_US) <= FAULT_TOL_US
              and abs(d_age - d_side) <= FAULT_TOL_US)
        all_pass = all_pass and ok
        lines.append(f"FAULT fw={fw} {'PASS' if ok else 'FAIL'}: d_age={d_age:.1f} us d_side={d_side:.1f} us "
                     f"(expected {FAULT_DELAY_US:.0f} +- {FAULT_TOL_US:.0f}, |d_age - d_side| <= {FAULT_TOL_US:.0f}) "
                     f"runs dly {len(a_dly)} h43 {len(a_base)}")
    return all_pass, lines


def summarize(text, pc=False, out=print):
    runs_by_id = parse(text)
    if not runs_by_id:
        out("BAD INPUT: no FRESH run lines")
        return 2
    min_reps = MIN_REPS_PC if pc else MIN_REPS
    runs = [read_run(r) for r in runs_by_id.values() if not r["cell"].startswith("idn")]
    out(f"== R0 sample counts ({len(runs)} runs; min reps {min_reps}) ==")
    for r in sorted(runs, key=lambda x: x["id"]):
        if r["void"]:
            out(f"RUN {r['id']:3d} {r['cell']:4s} {r['fw']:10s} VOID: {r['void']}")
            continue
        out(f"RUN {r['id']:3d} {r['cell']:4s} {r['fw']:10s} echoes={r['echoes']:.0f} lost={r['echo_lost']:.0f} "
            f"in_window={r['n_age']:.0f} bursts={r['bursts']:.0f} pongs={r['pongs']:.0f} "
            f"read={','.join(sorted(r['metrics']))}")
    cells = [c for c in SCORED_CELLS + (FAULT_CELL,) if any(r["cell"] == c for r in runs)]

    out("== R1 the control: side channel RTT (framework-independent), per cell ==")
    spreads = {}
    for c in cells:
        sp = {}
        for ctl in ("side_p50", "side_p99"):
            vals = [r["metrics"][ctl] for r in runs if r["cell"] == c and r["void"] is None and ctl in r["metrics"]]
            sp[ctl] = (max(vals) - min(vals)) if vals else None
            per_fw = "  ".join(f"{fw} {min(v):.1f}-{max(v):.1f}" for fw in FRAMEWORKS
                               if (v := values(runs, c, fw, ctl)))
            out(f"CONTROL cell={c} {ctl}_us: {per_fw or 'none'}  spread={sp[ctl] if sp[ctl] is None else round(sp[ctl], 1)}")
        spreads[c] = sp

    out("== R2 the fault arm (netem delay 1 ms on the sender's eth0) ==")
    fault_pass, lines = fault_arm(runs, min_reps)
    for line in lines:
        out(line)

    out("== R3 values per cell and framework (rep ranges; us) ==")
    for c in cells:
        for fw in FRAMEWORKS:
            rs = [r for r in runs if r["cell"] == c and r["fw"] == fw and r["void"] is None]
            if not rs:
                continue
            parts = []
            for metric, _, _ in METRICS + (("side_p50", "", None),):
                v = values(runs, c, fw, metric)
                parts.append(f"{metric}=" + (f"{min(v):.3f}-{max(v):.3f}" if metric == "newest_share" and v else
                                            f"{min(v):.1f}-{max(v):.1f}" if v else "NOT_READ"))
            echoes = ",".join(f"{r['echoes']:.0f}" for r in rs)
            out(f"VALUE cell={c} fw={fw} n={len(rs)} " + " ".join(parts) + f" echoes/run={echoes}")
            diag = [r["diag_oneway"] for r in rs if "diag_oneway" in r]
            rdr = [r["diag_reader_oneway"] for r in rs if r.get("diag_reader_oneway") is not None]
            good = [r["goodput"] for r in rs]
            taken = ",".join(f"{r['taken']:.0f}/{r['sent']:.0f}" if r["taken"] is not None and r["sent"] is not None
                             else "?" for r in rs)
            ev = ",".join(f"{r['evicted']:.0f}" for r in rs if r.get("evicted") is not None)
            out(f"  COUNT (not scored) cell={c} fw={fw} taken/sent per run={taken}" + (f" evicted={ev}" if ev else ""))
            out(f"  DIAG (not scored) cell={c} fw={fw} age_p50-side_p50/2="
                + (f"{min(diag):.1f}-{max(diag):.1f}" if diag else "none")
                + " reader_one_way_p50(same-clock only)=" + (f"{min(rdr):.1f}-{max(rdr):.1f}" if rdr else "none")
                + f" goodput={min(good):.3f}-{max(good):.3f}")

    out("== R4 verdicts (TickLE vs each vendor; WIN/LOSE need disjoint rep ranges and a gap beyond the control) ==")
    for c in [c for c in cells if c in SCORED_CELLS]:
        for metric, direction, ctl in METRICS:
            t_vals = values(runs, c, "tickle", metric)
            t_runs = [r for r in runs if r["cell"] == c and r["fw"] == "tickle" and r["void"] is None]
            echoes = ",".join(f"{r['echoes']:.0f}" for r in t_runs) or "-"
            per, listed = {}, []
            if len(t_vals) < min_reps:
                took_nothing = t_runs and all(r["took_nothing"] for r in t_runs)
                verdict = "LOSE (TickLE took nothing)" if took_nothing else f"NOT READ (TickLE {len(t_vals)} runs)"
                out(f"VERDICT cell={c} metric={metric} {verdict} tickle_echoes/run={echoes}")
                continue
            for v in SCORED_VENDORS:
                v_vals = values(runs, c, v, metric)
                if len(v_vals) < min_reps:
                    listed.append(f"{v} NOT READ ({len(v_vals)} runs)")
                    continue
                spread = None if ctl is None else spreads[c].get(ctl)
                if ctl is not None and spread is None:
                    listed.append(f"{v} NOT READ (no control)")
                    continue
                per[v] = compare(t_vals, v_vals, direction, spread)
            if not per:
                verdict = "NOT READ (no vendor readable)"
            elif "LOSE" in per.values():
                verdict = "LOSE"
            elif any(x.startswith("VOID") for x in per.values()):
                verdict = "VOID"
            elif all(x == "WIN" for x in per.values()):
                verdict = "WIN"
            else:
                verdict = "TIE"
            detail = " ".join(f"vs_{v}={x}" for v, x in per.items())
            out(f"VERDICT cell={c} metric={metric} {verdict} {detail} {'; '.join(listed)} "
                f"tickle_echoes/run={echoes}".rstrip())
    if fault_pass:
        out("SESSION READ: the fault arm passed for every framework")
        return 0
    out("SESSION NOT READ: the fault arm did not pass for every framework (R2) - no verdict above stands")
    return 1


# ---------------------------------------------------------------------------------------------------- selftest ----

def _client(fw, age50, age99, side50, side99, n=160, bursts=160, newest=160, echoes=None, pings=700, pongs=700,
            peer="yes", bad=0):
    echoes = n if echoes is None else echoes
    return (f"RESULT: framework={fw} scenario=history_depth_burst_loss role=client sent={bursts} fresh=on "
            f"fresh_bursts={bursts} fresh_rt=on fresh_rt_peer={peer} fresh_rt_echoes={echoes} fresh_rt_in_window={n} "
            f"fresh_rt_newest={newest} fresh_rt_bad={bad} fresh_rt_age_p50_us={age50} fresh_rt_age_p99_us={age99} "
            f"fresh_rt_pings={pings} fresh_rt_pongs={pongs} fresh_rt_side_p50_us={side50} fresh_rt_side_p99_us={side99}")


def _server(fw, n=160, sent=None, fail=0, skipped=0, stalled=1, lost=0, evicted=None):
    sent = n if sent is None else sent
    evicted = lost if evicted is None else evicted
    stall = f"lost={lost} depth=8 evicted={evicted} stalled={stalled} " if fw == "tickle" else ""
    return (f"RESULT: framework={fw} scenario=history_depth_burst_loss role=server recv={n} {stall}fresh=on fresh_taken="
            f"{sent + fail + skipped} fresh_age_p50_us=100 fresh_rt=on fresh_rt_echo_sent={sent} "
            f"fresh_rt_echo_fail={fail} fresh_rt_echo_skipped={skipped}")


def _session(spec, reps=3, dly=True, mutate=None):
    """spec: {(cell, fw): (age50, age99, side50, side99, n)}; mutate(run_id, cell, fw, rep) -> dict of overrides."""
    lines, rid = [], 0
    cells = sorted({c for c, _ in spec})
    for rep in range(1, reps + 1):
        for c in cells + (["dly"] if dly else []):
            for fw in FRAMEWORKS:
                key = (c if c != "dly" else "h43", fw)
                if key not in spec:
                    continue
                a50, a99, s50, s99, n = spec[key]
                kw = {"n": n, "bursts": n}
                q = "qdisc noqueue 0: root refcnt 2"
                if c == "dly":
                    a50, a99, s50, s99 = a50 + 1000, a99 + 1000, s50 + 1000, s99 + 1000
                    q = "qdisc netem 8001: root refcnt 2 limit 1000 delay 1ms"
                rid += 1
                over = mutate(rid, c, fw, rep) if mutate else {}
                q = over.pop("qdisc", q)
                srv = over.pop("server", {})
                lines.append(f"FRESH run={rid} cell={c} fw={fw} rep={rep} topo=cross args=x")
                lines.append(f"QDISC run={rid} {q}")
                cargs = dict(kw)
                cargs.update(over)
                lines.append(f"FRESH run={rid} " + _client(fw, a50, a99, s50, s99, **cargs))
                if srv is not None:
                    sargs = {"n": n}
                    sargs.update(srv)
                    lines.append(f"FRESH run={rid} " + _server(fw, **sargs))
    return "\n".join(lines)


def selftest():
    checks = []

    def run(text, pc=False):
        buf = []
        rc = summarize(text, pc=pc, out=buf.append)
        return rc, "\n".join(buf)

    def check(name, cond):
        checks.append((name, bool(cond)))

    base = {("h43", "tickle"): (300, 600, 150, 300, 160), ("h43", "fastdds"): (900, 1500, 152, 305, 160),
            ("h43", "cyclonedds"): (800, 1400, 151, 302, 160),
            ("b16", "tickle"): (400, 700, 150, 300, 1600), ("b16", "fastdds"): (1200, 2500, 152, 305, 1600),
            ("b16", "cyclonedds"): (1100, 2400, 151, 302, 1600)}
    rc, out = run(_session(base))
    check("clean session is READ", rc == 0)
    check("h43 p50 WIN", "VERDICT cell=h43 metric=age_p50 WIN" in out)
    check("h43 p99 NOT READ with 160 samples", "cell=h43 metric=age_p99 NOT READ" in out)
    check("b16 p99 read and WIN", "VERDICT cell=b16 metric=age_p99 WIN" in out)
    check("newest share TIE when equal", "VERDICT cell=h43 metric=newest_share TIE" in out)
    check("fault arm PASS printed", out.count("FAULT fw=") == 3 and "FAIL" not in out)
    check("realized treatment printed", "tickle_echoes/run=160,160,160" in out)
    check("diagnostic is labelled", "DIAG (not scored)" in out and "age_p50-side_p50/2=225.0-225.0" in out)
    check("sample counts come before values", out.index("== R0") < out.index("== R3"))

    lose = dict(base)
    lose[("h43", "tickle")] = (2000, 3000, 150, 300, 160)
    rc, out = run(_session(lose))
    check("LOSE when a vendor is better in every run", "VERDICT cell=h43 metric=age_p50 LOSE" in out)

    # Control spread: the side channel moved 700 us between runs; a 100 us age gap must be VOID, not WIN.
    ctl = dict(base)
    ctl[("h43", "fastdds")] = (800, 1500, 850, 900, 160)
    rc, out = run(_session(ctl))
    check("gap within the control spread is VOID", "VERDICT cell=h43 metric=age_p50 VOID" in out)

    # Equal ranges: TIE.
    tie = dict(base)
    tie[("h43", "fastdds")] = (300, 600, 150, 300, 160)
    rc, out = run(_session(tie))
    check("equal ranges are TIE", "vs_fastdds=TIE" in out)

    # Echo loss beyond the threshold voids a run.
    def lose_echo(rid, c, fw, rep):
        return {"echoes": 158} if (fw == "tickle" and c == "h43" and rep == 1) else {}
    rc, out = run(_session(base, mutate=lose_echo))
    check("lost echoes VOID the run", "VOID: echoes lost 2 of 160" in out)
    check("a voided TickLE rep leaves h43 NOT READ", "cell=h43 metric=age_p50 NOT READ (TickLE 2 runs)" in out)

    def fail_echo(rid, c, fw, rep):
        return {"server": {"fail": 3, "sent": 157}, "echoes": 157} if (fw == "fastdds" and rep == 2) else {}
    rc, out = run(_session(base, mutate=fail_echo))
    check("echo send failures VOID the run", "VOID: echo_fail=3" in out)
    check("a vendor below min reps is listed", "fastdds NOT READ (2 runs)" in out)

    def no_server(rid, c, fw, rep):
        return {"server": None} if rid == 1 else {}
    rc, out = run(_session(base, mutate=no_server))
    check("missing RESULT line VOID", "VOID: missing RESULT line(s)" in out)

    def no_peer(rid, c, fw, rep):
        return {"peer": "no"} if rid == 2 else {}
    rc, out = run(_session(base, mutate=no_peer))
    check("sender without peer VOID", "VOID: the sender never heard from the reader" in out)

    def bad_dgram(rid, c, fw, rep):
        return {"bad": 1} if rid == 3 else {}
    rc, out = run(_session(base, mutate=bad_dgram))
    check("foreign datagram VOID", "VOID: fresh_rt_bad=1" in out)

    def few_pongs(rid, c, fw, rep):
        return {"pongs": 50, "pings": 50} if rid == 4 else {}
    rc, out = run(_session(base, mutate=few_pongs))
    check("too few pongs: control not read, VOID", "VOID: control not read" in out)

    def lost_pongs(rid, c, fw, rep):
        return {"pongs": 600} if rid == 5 else {}
    rc, out = run(_session(base, mutate=lost_pongs))
    check("pongs missing beyond in-flight VOID", "VOID: control not read: pings 700 pongs 600" in out)

    # The fault arm: no netem on the dly runs -> VOID -> NOT READ.
    def no_netem(rid, c, fw, rep):
        return {"qdisc": "qdisc noqueue 0: root refcnt 2"} if c == "dly" else {}
    rc, out = run(_session(base, mutate=no_netem))
    check("dly without its qdisc is VOID", "VOID: dly run without netem delay 1ms" in out)
    check("... and the session NOT READ", rc == 1 and "SESSION NOT READ" in out)

    def shaped(rid, c, fw, rep):
        return {"qdisc": "qdisc netem 8001: root delay 1ms"} if c == "h43" and rep == 1 and fw == "tickle" else {}
    rc, out = run(_session(base, mutate=shaped))
    check("a scored run on a shaped link is VOID", "VOID: shaped link" in out)

    # The fault arm sees the side channel move but not the age (e.g. data over shared memory): FAIL.
    text = _session(base)
    text = re.sub(r"(cell=dly fw=tickle[^\n]*\n[^\n]*\n[^\n]*fresh_rt_age_p50_us=)1300", r"\g<1>300", text)
    rc, out = run(text)
    check("fault arm FAIL when the age does not move", "FAULT fw=tickle FAIL" in out and rc == 1)

    rc, out = run(_session(base, dly=False))
    check("no dly cell: NOT READ", rc == 1 and "FAULT fw=tickle NOT READ" in out)

    def took_nothing(rid, c, fw, rep):
        return {"n": 0, "echoes": 0, "server": {"n": 0, "sent": 0}} if fw == "tickle" and c == "b16" else {}
    rc, out = run(_session(base, mutate=took_nothing))
    check("TickLE took nothing: LOSE", "VERDICT cell=b16 metric=age_p50 LOSE (TickLE took nothing)" in out)

    def few_bursts(rid, c, fw, rep):
        return {"bursts": 50} if c == "h43" else {}
    rc, out = run(_session(base, mutate=few_bursts))
    check("newest share NOT READ under 100 bursts", "cell=h43 metric=newest_share NOT READ" in out)

    def share(rid, c, fw, rep):
        return {"newest": 100} if fw == "fastdds" and c == "h43" else {}
    rc, out = run(_session(base, mutate=share))
    check("newest share has no control: WIN on ranges", "cell=h43 metric=newest_share WIN" not in out
          and "vs_fastdds=WIN" in out)

    # R0b: the reshaped TickLE reader must report its stall, and every seq it did not take must be an eviction.
    def no_stall(rid, c, fw, rep):
        return {"server": {"stalled": 0}} if fw == "tickle" and c == "h43" and rep == 1 else {}
    rc, out = run(_session(base, mutate=no_stall))
    check("TickLE without its stall is VOID", "VOID: TickLE reader did not stall (stalled=0)" in out)

    def lost_not_evicted(rid, c, fw, rep):
        return {"server": {"lost": 5, "evicted": 3}} if fw == "tickle" and c == "b16" and rep == 2 else {}
    rc, out = run(_session(base, mutate=lost_not_evicted))
    check("TickLE lost beyond eviction is VOID", "VOID: TickLE lost 5 != evicted 3" in out)

    stall = dict(base)
    for fw in FRAMEWORKS:
        stall[("h44", fw)] = (300 if fw == "tickle" else 900, 600, 150, 300, 107)

    def evicts(rid, c, fw, rep):
        return {"server": {"lost": 53, "evicted": 53, "n": 107}, "bursts": 160} if c == "h44" else {}
    rc, out = run(_session(stall, mutate=evicts))
    check("h44 with evictions is read", "VERDICT cell=h44 metric=age_p50 WIN" in out)
    check("COUNT line carries taken and evicted", "COUNT (not scored) cell=h44 fw=tickle taken/sent per run="
          "107/160,107/160,107/160 evicted=53,53,53" in out)

    def evicts_none(rid, c, fw, rep):
        return {"bursts": 160} if c == "h44" else {}
    rc, out = run(_session(stall, mutate=evicts_none))
    check("h44 evicting nothing is VOID for TickLE", "VOID: TickLE h44 evicted nothing" in out)
    check("... and not for a vendor", "h44 fastdds    VOID" not in out)

    rc, out = run(_session(base, reps=2), pc=True)
    check("--pc reads 2 reps", rc == 0 and "VERDICT cell=h43 metric=age_p50 WIN" in out)
    rc, out = run(_session(base, reps=2))
    check("rig mode needs 3 reps", rc == 1)

    rc, out = run("nothing here")
    check("bad input exit 2", rc == 2)

    check("(1-0.01)^MIN_AGES_P99 < 0.05", (1 - 0.01) ** MIN_AGES_P99 < 0.05)
    check("(1-0.01)^160 > 0.05: rows 43/44 p99 not read", (1 - 0.01) ** 160 > 0.05)

    failed = [n for n, ok in checks if not ok]
    for n, ok in checks:
        print(f"{'ok  ' if ok else 'FAIL'} {n}")
    print(f"SELFTEST {'PASS' if not failed else 'FAIL'} {len(checks) - len(failed)}/{len(checks)}")
    return 0 if not failed else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("results", nargs="?")
    ap.add_argument("--pc", action="store_true", help="the PC dry run: 2 reps per cell instead of 3")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--prereg", action="store_true", help="print the reading rules written before any run")
    a = ap.parse_args()
    if a.prereg:
        print(__doc__)
        return 0
    if a.selftest:
        return selftest()
    if not a.results:
        ap.error("a results file, --selftest or --prereg")
    with open(a.results, encoding="utf-8", errors="replace") as f:
        return summarize(f.read(), pc=a.pc)


if __name__ == "__main__":
    sys.exit(main())
