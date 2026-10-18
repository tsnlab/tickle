#!/usr/bin/env python3
"""KEEP_LAST freshness: the reading of keep_last_freshness.sh (and of s6_transport_cells.sh files run with --fresh).

The user's decision D3 (2026-10-18): in a KEEP_LAST cell freshness is the primary score and goodput a diagnostic.
Freshness is the AGE of a sample when the application takes it (take - creation, p50 and p99) and the NEWEST SHARE
(bursts whose newest sample the application took / bursts the sender scheduled). Definitions, in code, for all three
frameworks: examples/perf_hil/tickle/common/BenchFreshness.h. A superseded sample is not lost.

THE READING RULES, in the order they are applied (each is code below; --selftest exercises every one):

  R0  SAMPLE COUNT FIRST. A run needs the client's and the server's RESULT line, both with fresh=on, no unstamped
      sample (fresh_no_stamp=0) and the same burst size on both sides; otherwise it is VOID. Its ages are read only
      from at least MIN_AGES in-window takes, its newest share only over at least MIN_BURSTS scheduled bursts; below
      that the metric is NOT READ for that run. A cell x framework is read only with MIN_REPS readable runs (the fault
      arm: 1). The counts are printed before any value.
  R1  ONE CLOCK. Same host: one clock, offset 0; a minimum age below -NEG_TOL_US is a broken stamp: VOID.
      Cross host: the offset (receiver - sender) is the mean of the probes before and after the run, both PROBE ok=1;
      its uncertainty U = |after - before| / 2 + max(delay_min) / 2 (a probe's own error is at most half its best
      round trip). With --fresh-dump the server prints every age with its take time; each age is then corrected by
      the offset interpolated linearly at its own take time, and U = INTERP_ALLOW x |after - before| / 2 +
      max(delay_min) / 2: the Pis' clocks drift ~30 us/s against each other, so over a ~20 s run the whole-run term
      alone would exceed every transit-sized age. VOID when a probe is missing or failed, when U exceeds the run's corrected age p50 (the offset is
      not known well enough to read the age), or when the corrected minimum age is below -(U + NEG_TOL_US).
  R2  THE CONTROL. The `ctl` cell (same host, paced, the reader present) must read a transit for every framework:
      minimum age in [-NEG_TOL_US, TRANSIT_MAX_US] and age p50 <= TRANSIT_MAX_US in every run. The `fault` cell (the
      same with --fresh-skew-us on the sender) must FAIL that test in every run: a control that passes a deliberately
      wrong clock cannot fail, and proves nothing. Either failure, or no control cell at all, leaves every verdict of
      the session NOT READ.
  R3  THE METRICS, per cell and framework over its readable runs: age p50 and p99 (corrected, us; lower is fresher)
      and newest share (higher is fresher). Goodput (server recv / client sent) is printed beside them, never scored.
      A cell whose sender has no schedule (fresh_creation=stamp, a max-rate throughput client: S1-S3) is scored on
      age alone: with no pause after a burst nothing marks a newest sample, and the share restates delivery.
  R4  THE VERDICT, docs/TESTING.md section 4, per metric: WIN when every TickLE run is better than every run of every
      scored vendor (FastDDS, CycloneDDS); LOSE when some vendor's every run is better than every TickLE run; DRAW
      otherwise (ranges touch or overlap). A vendor whose metric is NOT READ is listed and left out; TickLE NOT READ
      because it took nothing in the window is LOSE (it delivered nothing to be fresh), NOT READ otherwise.

Input: keep_last_freshness.sh output (FRESH lines), and optionally s6_transport_cells.sh output files (the headline
arm=ON client and server RESULT lines, same host) via --s6. Usage:
  freshness_summary.py <keep_last_freshness output> [--s6 <s6 file> ...]
  freshness_summary.py --selftest
Exit status: 0 when the session is READ (control passed, fault caught), 1 when it is NOT READ, 2 on bad input.
"""
import argparse
import re
import sys

MIN_AGES = 50
MIN_BURSTS = 100
MIN_REPS = 3
MIN_REPS_FAULT = 1
NEG_TOL_US = 20.0
TRANSIT_MAX_US = 5000.0
INTERP_ALLOW = 0.1  # share of the probes' drift kept in U when each age is corrected at its own take time
SCORED_VENDORS = ("fastdds", "cyclonedds")
FRAMEWORKS = ("tickle",) + SCORED_VENDORS
CONTROL_CELL = "ctl"
FAULT_CELL = "fault"
NO_SCHEDULE = "no schedule (max rate): it equals the delivered share, diagnostic only"
METRICS = (("age_p50_us", "lower"), ("age_p99_us", "lower"), ("newest_share", "higher"))

KV = re.compile(r"(\w+)=(\S+)")


def fields(line):
    return dict(KV.findall(line))


def num(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


# ------------------------------------------------------------------------------------------------------- parsing ----

def parse_fresh_log(text):
    """keep_last_freshness.sh output -> list of runs (dicts)."""
    runs = {}
    order = []
    for line in text.splitlines():
        m = re.match(r"FRESH run=(\d+) (.*)$", line)
        if not m:
            continue
        rid, rest = m.group(1), m.group(2)
        if rest.startswith("cell="):
            f = fields(rest)
            runs[rid] = {"id": rid, "cell": f.get("cell"), "fw": f.get("fw"), "rep": f.get("rep"),
                         "topo": f.get("topo"), "leftover": f.get("leftover_before", "none"),
                         "client": None, "server": None, "probe": {}, "note": []}
            order.append(rid)
            continue
        run = runs.get(rid)
        if run is None:
            continue
        pm = re.match(r"probe=(before|after) (PROBE .*)$", rest)
        if pm:
            run["probe"][pm.group(1)] = fields(pm.group(2))
        elif rest.startswith("RESULT:"):
            f = fields(rest)
            run[f.get("role", "unknown")] = f
        elif rest.startswith("NO RESULT LINE"):
            run["note"].append("no RESULT line")
    return [runs[r] for r in order]


def parse_s6(text, name):
    """s6_transport_cells.sh output -> runs of the headline arm (ON), same host, one per client/server pair."""
    head = re.search(r"scen=(\S+) size=(\S+)", text)
    cell = f"s6_{head.group(1)}_{head.group(2)}" if head else f"s6_{name}"
    runs = []
    pending = {}
    rep = {}
    for line in text.splitlines():
        m = re.match(r"\s*arm=ON (RESULT: .*)$", line)
        if not m:
            continue
        f = fields(m.group(1))
        fw, role = f.get("framework"), f.get("role")
        if role == "client":
            pending[fw] = f
        elif role == "server" and fw in pending:
            rep[fw] = rep.get(fw, 0) + 1
            runs.append({"id": f"{name}:{fw}:{rep[fw]}", "cell": cell, "fw": fw, "rep": str(rep[fw]), "topo": "same",
                         "leftover": "none", "client": pending.pop(fw), "server": f, "probe": {}, "note": []})
    return runs


# ------------------------------------------------------------------------------------------------------- reading ----

def read_run(run):
    """R0 and R1 for one run: fills run['void'] (str or None) and the corrected metrics (None = NOT READ)."""
    run.update({"void": None, "age_p50_us": None, "age_p99_us": None, "age_min_us": None, "newest_share": None,
                "goodput": None, "taken": 0, "bursts": 0, "offset_us": None, "u_us": None, "age_why": "",
                "share_why": "", "dump": False})
    cli, srv = run.get("client"), run.get("server")
    if cli is None or srv is None:
        run["void"] = "missing " + ("client" if cli is None else "server") + " RESULT line"
        return run
    if run.get("leftover", "none") != "none":
        run["void"] = f"leftover guard fired ({run['leftover']})"
        return run
    if cli.get("fresh") != "on" or srv.get("fresh") != "on":
        run["void"] = "no freshness fields (was --fresh given to both sides?)"
        return run
    if int(srv.get("fresh_no_stamp", "1")) != 0:
        run["void"] = f"fresh_no_stamp={srv.get('fresh_no_stamp')}"
        return run
    taken = int(srv.get("fresh_in_window", "0"))
    if taken > 0 and srv.get("fresh_burst") != cli.get("fresh_burst"):
        run["void"] = f"burst size client {cli.get('fresh_burst')} server {srv.get('fresh_burst')}"
        return run
    sent, recv = num(cli.get("sent")), num(srv.get("recv"))
    if sent:
        run["goodput"] = (recv or 0.0) / sent
    run["taken"] = taken
    run["bursts"] = int(cli.get("fresh_bursts", "0"))
    if cli.get("fresh_creation") == "stamp":
        # R3: a sender with no schedule (max rate) never pauses after a burst, so nothing marks a sample as the newest
        # the reader could have had; the share then only restates the delivered share.
        run["share_why"] = NO_SCHEDULE
    elif run["bursts"] >= MIN_BURSTS:
        run["newest_share"] = int(srv.get("fresh_newest", "0")) / run["bursts"]
    else:
        run["share_why"] = f"{run['bursts']} bursts < {MIN_BURSTS}"
    if taken < MIN_AGES:
        run["age_why"] = f"{taken} in-window takes < {MIN_AGES}"
        return run
    p50, p99, amin = (num(srv.get(k)) for k in ("fresh_age_p50_us", "fresh_age_p99_us", "fresh_age_min_us"))
    if p50 is None or p99 is None or amin is None:
        run["void"] = "age fields unreadable"
        return run
    if run["topo"] == "same":
        offset, u = 0.0, 0.0
    else:
        b, a = run["probe"].get("before"), run["probe"].get("after")
        if not b or not a or b.get("ok") != "1" or a.get("ok") != "1":
            run["void"] = "clock probe missing or failed"
            return run
        ob, oa = int(b["offset_ns"]) / 1e3, int(a["offset_ns"]) / 1e3
        offset = (ob + oa) / 2
        half_delay = max(int(b["delay_min_ns"]), int(a["delay_min_ns"])) / 2e3
        u = abs(oa - ob) / 2 + half_delay
        pairs = parse_dump(srv.get("fresh_dump"))
        if pairs is not None:
            # Per take: the offset interpolated at the take's own time (sender clock = receiver clock - offset), so
            # the drift between the probes no longer counts in full; INTERP_ALLOW of it stays in U for a drift that
            # is not linear over the run.
            if len(pairs) != int(srv.get("fresh_kept", "-1")):
                run["void"] = f"fresh_dump holds {len(pairs)} ages, fresh_kept={srv.get('fresh_kept')}"
                return run
            wb, wa = int(b["wall_ns"]), int(a["wall_ns"])
            slope = (oa - ob) / (wa - wb) if wa != wb else 0.0
            ages = sorted(age - (ob + slope * (t - offset * 1e3 - wb)) for t, age in pairs)
            p50, p99, amin = rank(ages, 0.50), rank(ages, 0.99), ages[0]
            run["offset_us"], run["u_us"] = offset, INTERP_ALLOW * abs(oa - ob) / 2 + half_delay
            u = run["u_us"]
            run["dump"] = True
    if run["offset_us"] is None:
        run["offset_us"], run["u_us"] = offset, u
        p50, p99, amin = p50 - offset, p99 - offset, amin - offset
    run["age_min_us"] = amin
    if amin < -(u + NEG_TOL_US):
        run["void"] = f"corrected minimum age {amin:.1f} us < -(U {u:.1f} + {NEG_TOL_US:.0f}) us: the clocks disagree"
        return run
    if run["topo"] != "same" and u > p50:
        run["void"] = f"offset uncertainty {u:.1f} us exceeds the age p50 {p50:.1f} us"
        return run
    run["age_p50_us"], run["age_p99_us"] = p50, p99
    return run


def parse_dump(text):
    """fresh_dump=<t0>:<dt>/<age>,... -> [(take time ns, raw age us)], None when absent or unreadable."""
    if not text or text == "unavailable" or ":" not in text:
        return None
    head, _, body = text.partition(":")
    try:
        t0 = int(head)
        out = []
        for item in body.split(","):
            if item:
                dt, _, age = item.partition("/")
                out.append((t0 + int(dt) * 1000, float(age)))
        return out
    except ValueError:
        return None


def rank(sorted_values, q):
    """Nearest-rank quantile, as BenchFreshness.h's."""
    r = int(q * len(sorted_values) + 0.999999)
    return sorted_values[min(max(r, 1), len(sorted_values)) - 1]


def control_ok(run):
    """R2's test for one run: a transit on one clock."""
    if run["age_min_us"] is None or run["age_p50_us"] is None:
        return False
    return -NEG_TOL_US <= run["age_min_us"] <= TRANSIT_MAX_US and run["age_p50_us"] <= TRANSIT_MAX_US


def fault_failed(run):
    """R2 for the fault arm: the skewed clock must not pass as a transit. A run voided by R1 has failed it too."""
    if run["void"] is not None:
        return run["void"].startswith("corrected minimum age")
    return not control_ok(run)


def judge_control(runs):
    """R2 -> (ok, lines)."""
    lines = []
    ctl = [r for r in runs if r["cell"] == CONTROL_CELL]
    flt = [r for r in runs if r["cell"] == FAULT_CELL]
    ok = True
    if not ctl:
        return False, ["CONTROL MISSING: no ctl cell, so nothing shows the clock reads a transit"]
    for fw in FRAMEWORKS:
        mine = [r for r in ctl if r["fw"] == fw]
        bad = [r for r in mine if r["void"] is not None or not control_ok(r)]
        if len(mine) < MIN_REPS or bad:
            ok = False
            why = "; ".join(f"run {r['id']}: " + (r["void"] or f"min {r['age_min_us']} p50 {r['age_p50_us']}")
                            for r in bad) or f"{len(mine)} runs < {MIN_REPS}"
            lines.append(f"CONTROL FAIL {fw}: {why}")
        else:
            lines.append(f"control pass {fw}: {len(mine)} runs, min age "
                         f"{min(r['age_min_us'] for r in mine):.1f}..{max(r['age_min_us'] for r in mine):.1f} us, p50 "
                         f"{min(r['age_p50_us'] for r in mine):.1f}..{max(r['age_p50_us'] for r in mine):.1f} us")
    if not flt:
        ok = False
        lines.append("FAULT ARM MISSING: nothing shows the control can fail")
    for fw in FRAMEWORKS:
        mine = [r for r in flt if r["fw"] == fw]
        if not flt:
            break
        caught = [r for r in mine if fault_failed(r)]
        if len(mine) < MIN_REPS_FAULT or len(caught) != len(mine):
            ok = False
            lines.append(f"FAULT NOT CAUGHT {fw}: {len(caught)} of {len(mine)} skewed runs failed the control")
        else:
            lines.append(f"fault caught {fw}: {len(caught)} of {len(mine)} skewed runs failed the control (as they must)")
    return ok, lines


def metric_values(runs, metric):
    return [r[metric] for r in runs if r["void"] is None and r[metric] is not None]


def verdict(values, sense):
    """R4 for one metric: values = {fw: [per-run values]} with TickLE's present."""
    t = values["tickle"]
    better = (lambda x, y: x < y) if sense == "lower" else (lambda x, y: x > y)
    t_worst = max(t) if sense == "lower" else min(t)
    t_best = min(t) if sense == "lower" else max(t)
    vendors = [v for v in SCORED_VENDORS if values.get(v)]
    if not vendors:
        return "NOT READ (no scored vendor read)"
    for v in vendors:
        v_best = min(values[v]) if sense == "lower" else max(values[v])
        v_worst = max(values[v]) if sense == "lower" else min(values[v])
        if better(v_worst, t_best):
            return f"LOSE (against {v})"
    if all(better(t_worst, min(values[v]) if sense == "lower" else max(values[v])) for v in vendors):
        return "WIN"
    return "DRAW"


def summarise(runs, out=print):
    for r in runs:
        read_run(r)
    out("== R0: sample counts (runs; in-window takes; scheduled bursts) ==")
    cells = []
    for r in runs:
        if r["cell"] not in cells:
            cells.append(r["cell"])
    for c in cells:
        for fw in FRAMEWORKS:
            mine = [r for r in runs if r["cell"] == c and r["fw"] == fw]
            if not mine:
                continue
            voids = [r for r in mine if r["void"]]
            out(f"  {c:28} {fw:10} runs {len(mine)} (VOID {len(voids)}) takes "
                f"{','.join(str(r['taken']) for r in mine)} bursts {','.join(str(r['bursts']) for r in mine)}")
            for r in voids:
                out(f"      VOID run {r['id']}: {r['void']}")
    out("== R1/R2: clock and control ==")
    for r in runs:
        if r["topo"] != "same" and r["offset_us"] is not None:
            out(f"  run {r['id']:>4} {r['cell']:6} {r['fw']:10} offset {r['offset_us']:+.1f} us, U {r['u_us']:.1f} us")
    ok, lines = judge_control(runs)
    for line in lines:
        out("  " + line)
    out("== R3/R4: freshness per cell (us; share 0..1); goodput diagnostic ==")
    verdicts = {}
    for c in cells:
        if c in (CONTROL_CELL, FAULT_CELL):
            continue
        out(f"  -- {c}")
        need = MIN_REPS
        per = {}
        for fw in FRAMEWORKS:
            mine = [r for r in runs if r["cell"] == c and r["fw"] == fw]
            if not mine:
                continue
            per[fw] = {}
            gp = [r["goodput"] for r in mine if r["void"] is None and r["goodput"] is not None]
            parts = []
            for metric, _ in METRICS:
                vals = metric_values(mine, metric)
                if len(vals) >= need:
                    per[fw][metric] = vals
                    parts.append(f"{metric} {min(vals):.4g}..{max(vals):.4g}")
                else:
                    whys = {r["age_why"] if metric.startswith("age") else r["share_why"] for r in mine} - {""}
                    per[fw][metric] = None
                    per[fw][metric + "_nothing"] = all(r["void"] is None and r["taken"] == 0 for r in mine) and \
                        metric.startswith("age")
                    nvoid = sum(1 for r in mine if r["void"])
                    why = "; ".join(sorted(whys)) or (f"{nvoid} VOID" if nvoid else f"{len(mine)} runs < {need}")
                    parts.append(f"{metric} NOT READ ({len(vals)} of {need} runs; {why})")
            gps = f"goodput {min(gp):.3f}..{max(gp):.3f}" if gp else "goodput -"
            out(f"     {fw:10} " + " | ".join(parts) + f" | {gps}")
        if "tickle" not in per:
            continue
        for metric, sense in METRICS:
            t = per["tickle"].get(metric)
            if metric == "newest_share" and all(r["share_why"] == NO_SCHEDULE for r in runs if r["cell"] == c):
                v = "NOT SCORED (" + NO_SCHEDULE + ")"
            elif not ok:
                v = "NOT READ (control)"
            elif t is None:
                v = "LOSE (TickLE took nothing in the window)" if per["tickle"].get(metric + "_nothing") else \
                    "NOT READ (TickLE)"
            else:
                vals = {"tickle": t}
                skipped = []
                for vend in SCORED_VENDORS:
                    if vend in per and per[vend].get(metric) is not None:
                        vals[vend] = per[vend][metric]
                    elif vend in per:
                        skipped.append(vend)
                v = verdict(vals, sense) + (f"; not scored: {', '.join(skipped)}" if skipped else "")
            verdicts[(c, metric)] = v
            out(f"     {metric:14} {v}")
    out("SESSION " + ("READ" if ok else "NOT READ: see R2 above"))
    return ok, verdicts


# ------------------------------------------------------------------------------------------------------ selftest ----

def _client(fw, burst=1, bursts=160, sent=160, fresh=True):
    f = {"framework": fw, "role": "client", "sent": str(sent)}
    if fresh:
        f.update({"fresh": "on", "fresh_burst": str(burst), "fresh_bursts": str(bursts)})
    return f


def _server(fw, p50, p99, amin, taken=160, newest=160, burst=1, recv=None, no_stamp=0):
    return {"framework": fw, "role": "server", "recv": str(recv if recv is not None else taken), "fresh": "on",
            "fresh_in_window": str(taken), "fresh_no_stamp": str(no_stamp), "fresh_burst": str(burst),
            "fresh_newest": str(newest), "fresh_age_p50_us": str(p50), "fresh_age_p99_us": str(p99),
            "fresh_age_min_us": str(amin)}


def _probe(offset_ns, delay_ns=60000):
    return {"ok": "1", "offset_ns": str(offset_ns), "delay_min_ns": str(delay_ns)}


def _run(cell, fw, rep, topo, srv, cli=None, probes=None):
    return {"id": f"{cell}{fw}{rep}", "cell": cell, "fw": fw, "rep": str(rep), "topo": topo, "leftover": "none",
            "client": cli or _client(fw), "server": srv, "probe": probes or {}, "note": []}


def _session(cell_rows, ctl=True, fault=True, ctl_p50=80, fault_skew=-5000):
    """cell_rows: {(cell, fw): [(p50, p99, min, newest, taken, offset_before_ns, offset_after_ns), ...]}"""
    runs = []
    if ctl:
        for fw in FRAMEWORKS:
            for rep in (1, 2, 3):
                runs.append(_run(CONTROL_CELL, fw, rep, "same", _server(fw, ctl_p50, ctl_p50 * 2, 40)))
    if fault:
        for fw in FRAMEWORKS:
            runs.append(_run(FAULT_CELL, fw, 1, "same",
                             _server(fw, 80 + fault_skew, 160 + fault_skew, 40 + fault_skew)))
    for (cell, fw), reps in cell_rows.items():
        for i, (p50, p99, amin, newest, taken, ob, oa) in enumerate(reps, 1):
            runs.append(_run(cell, fw, i, "cross", _server(fw, p50, p99, amin, taken=taken, newest=newest),
                             probes={"before": _probe(ob), "after": _probe(oa)}))
    return runs


def selftest():
    failures = []
    checked = []

    def check(name, cond):
        checked.append(name)
        if not cond:
            failures.append(name)

    quiet = [].append
    off = 1_000_000  # the receiver's clock 1 ms ahead: raw ages read 1000 us too old
    good = {("h43", "tickle"): [(1100, 1300, 1050, 160, 160, off, off)] * 3,
            ("h43", "fastdds"): [(1400, 1900, 1200, 160, 160, off, off)] * 3,
            ("h43", "cyclonedds"): [(1300, 1700, 1150, 160, 160, off, off)] * 3}
    ok, v = summarise(_session(good), out=quiet)
    check("WIN on ages", ok and v[("h43", "age_p50_us")] == "WIN" and v[("h43", "age_p99_us")] == "WIN")
    check("equal newest share is DRAW", v[("h43", "newest_share")] == "DRAW")

    lose = dict(good)
    lose[("h43", "fastdds")] = [(1050, 1100, 1020, 160, 160, off, off)] * 3
    _, v = summarise(_session(lose), out=quiet)
    check("LOSE when a vendor's every run is better", v[("h43", "age_p50_us")].startswith("LOSE"))

    overlap = dict(good)
    overlap[("h43", "fastdds")] = [(1050, 1900, 1020, 160, 160, off, off), (1400, 1900, 1200, 160, 160, off, off),
                                   (1400, 1900, 1200, 160, 160, off, off)]
    _, v = summarise(_session(overlap), out=quiet)
    check("DRAW when the ranges overlap", v[("h43", "age_p50_us")] == "DRAW")

    share = dict(good)
    share[("h43", "fastdds")] = [(1400, 1900, 1200, 109, 109, off, off)] * 3
    share[("h43", "cyclonedds")] = [(1300, 1700, 1150, 120, 120, off, off)] * 3
    _, v = summarise(_session(share), out=quiet)
    check("newest share WIN", v[("h43", "newest_share")] == "WIN")

    few = dict(good)
    few[("h43", "tickle")] = [(1100, 1300, 1050, 160, 160, off, off)] * 2
    _, v = summarise(_session(few), out=quiet)
    check("two runs are NOT READ", v[("h43", "age_p50_us")].startswith("NOT READ"))

    thin = dict(good)
    thin[("h43", "cyclonedds")] = [(1300, 1700, 1150, 30, 30, off, off)] * 3
    _, v = summarise(_session(thin), out=quiet)
    check("a vendor under MIN_AGES is left out", "not scored: cyclonedds" in v[("h43", "age_p50_us")])

    nothing = dict(good)
    nothing[("h43", "tickle")] = [(0, 0, 0, 0, 0, off, off)] * 3
    _, v = summarise(_session(nothing), out=quiet)
    check("TickLE taking nothing is LOSE", v[("h43", "age_p50_us")].startswith("LOSE"))

    # R1: offset uncertainty above the age voids the run. Offsets 0 and 3 ms: U = 1.5 ms + 30 us > corrected p50.
    unc = dict(good)
    unc[("h43", "tickle")] = [(2600, 2800, 2550, 160, 160, 0, 3_000_000)] * 3
    runs = _session(unc)
    summarise(runs, out=quiet)
    tv = [r for r in runs if r["cell"] == "h43" and r["fw"] == "tickle"]
    check("U > age voids", all(r["void"] and "uncertainty" in r["void"] for r in tv))

    # Drift: offsets 0 and 3 ms over 10 s, true age 400 us. One offset for the run voids it (U 1.5 ms); the dump,
    # corrected at each take time, reads ~400 us with U 0.1 x 1.5 ms + 30 us = 180 us.
    def drift_run(fw, rep, dump):
        pairs = [(1_000_000_000 + k * 62_500_000, 400.0 + 3000.0 * k / 160) for k in range(160)]
        raw = sorted(a for _, a in pairs)
        srv = _server(fw, rank(raw, 0.5), rank(raw, 0.99), raw[0], taken=160, newest=160)
        srv["fresh_kept"] = "160"
        if dump:
            srv["fresh_dump"] = f"{pairs[0][0]}:" + ",".join(f"{(t - pairs[0][0]) // 1000}/{a:.1f}" for t, a in pairs)
        return _run("d", fw, rep, "cross", srv, probes={"before": {"ok": "1", "offset_ns": "0", "delay_min_ns": "60000",
                                                                    "wall_ns": str(1_000_000_000)},
                                                         "after": {"ok": "1", "offset_ns": "3000000",
                                                                   "delay_min_ns": "60000",
                                                                   "wall_ns": str(1_000_000_000 + 160 * 62_500_000)}})
    r = read_run(drift_run("tickle", 1, False))
    check("drift without the dump voids", r["void"] is not None and "uncertainty" in r["void"])
    r = read_run(drift_run("tickle", 1, True))
    check("drift with the dump reads the true age", r["void"] is None and r["dump"]
          and abs(r["age_p50_us"] - 400.0) < 30 and abs(r["u_us"] - 180.0) < 1e-6)
    r = drift_run("tickle", 1, True)
    r["server"]["fresh_kept"] = "161"
    check("a truncated dump voids", read_run(r)["void"].startswith("fresh_dump holds"))

    neg = dict(good)
    neg[("h43", "tickle")] = [(1100, 1300, 700, 160, 160, off, off)] * 3  # corrected min -300 us
    runs = _session(neg)
    summarise(runs, out=quiet)
    tv = [r for r in runs if r["cell"] == "h43" and r["fw"] == "tickle"]
    check("a negative corrected age voids", all(r["void"] and "minimum age" in r["void"] for r in tv))

    noprobe = _session(good)
    for r in noprobe:
        if r["cell"] == "h43" and r["fw"] == "fastdds":
            r["probe"] = {"before": _probe(off)}
    summarise(noprobe, out=quiet)
    check("a missing probe voids", all(r["void"] == "clock probe missing or failed"
                                       for r in noprobe if r["cell"] == "h43" and r["fw"] == "fastdds"))

    # R2.
    ok, v = summarise(_session(good, ctl=False), out=quiet)
    check("no control: NOT READ", not ok and v[("h43", "age_p50_us")].startswith("NOT READ"))
    ok, _ = summarise(_session(good, fault=False), out=quiet)
    check("no fault arm: NOT READ", not ok)
    ok, _ = summarise(_session(good, ctl_p50=9000), out=quiet)
    check("a control reading 9 ms is not a transit", not ok)
    ok, _ = summarise(_session(good, fault_skew=0), out=quiet)
    check("an uncaught fault leaves the session NOT READ", not ok)
    ok, _ = summarise(_session(good, fault_skew=+20000), out=quiet)
    check("a fault skewed the other way (ages 20 ms too old) is caught too", ok)

    # R0 voids.
    r = read_run(_run("h43", "tickle", 1, "same", _server("tickle", 1, 2, 1, no_stamp=3)))
    check("no_stamp voids", r["void"] and "no_stamp" in r["void"])
    r = read_run(_run("h43", "tickle", 1, "same", _server("tickle", 1, 2, 1), cli=_client("tickle", fresh=False)))
    check("missing --fresh voids", r["void"] and "freshness" in r["void"])
    r = read_run(_run("h43", "tickle", 1, "same", _server("tickle", 1, 2, 1, burst=8)))
    check("burst mismatch voids", r["void"] and "burst" in r["void"])
    r = read_run(_run("h43", "tickle", 1, "same", _server("tickle", 1, 2, 1)) | {"leftover": "pi:tickle:123"})
    check("leftover voids", r["void"] and "leftover" in r["void"])
    r = read_run(_run("h43", "tickle", 1, "same", _server("tickle", 1, 2, 1), cli=_client("tickle", bursts=40)))
    check("40 bursts: share NOT READ", r["newest_share"] is None and r["age_p50_us"] is not None)

    # Parsers.
    log = "\n".join([
        "FRESH run=1 cell=h43 fw=tickle rep=1 topo=cross leftover_before=none args=-p 0 --fresh",
        "FRESH run=1 probe=before PROBE ok=1 wall_ns=1 offset_ns=1000 delay_min_ns=50000 used=16 n=64",
        "FRESH run=1 RESULT: framework=tickle scenario=history_depth_burst_loss role=client sent=160 fresh=on "
        "fresh_burst=1 fresh_bursts=160 fresh_stamped=160 fresh_creation=due fresh_skew_us=0.0 fresh_window=all",
        "FRESH run=1 RESULT: framework=tickle scenario=history_depth_burst_loss role=server pause_s=0.0 recv=160 lost=0 "
        "loss_pct=0.000000 fresh=on fresh_taken=160 fresh_no_stamp=0 fresh_in_window=160 fresh_burst=1 "
        "fresh_newest=160 fresh_newest_late=0 fresh_kept=160 fresh_age_p50_us=120.0 fresh_age_p99_us=300.0 "
        "fresh_age_min_us=90.0 fresh_age_max_us=400.0",
        "FRESH run=1 probe=after PROBE ok=1 wall_ns=2 offset_ns=3000 delay_min_ns=40000 used=16 n=64",
        "FRESH run=2 cell=h43 fw=fastdds rep=1 topo=cross leftover_before=none args=-p 0 --fresh",
        "FRESH run=2 NO RESULT LINE",
    ])
    runs = parse_fresh_log(log)
    check("parse: two runs", len(runs) == 2)
    r = read_run(runs[0])
    check("parse: offset is the mean, corrected", r["void"] is None and abs(r["offset_us"] - 2.0) < 1e-9
          and abs(r["age_p50_us"] - 118.0) < 1e-9 and abs(r["u_us"] - 26.0) < 1e-9)
    check("parse: no RESULT voids", read_run(runs[1])["void"].startswith("missing client"))
    s6 = "\n".join([
        "=== S6 transport cells x sha=1 scen=best_effort_throughput size=p2 dur=5s reps=1 ===",
        "  arm=ON RESULT: framework=tickle scenario=best_effort_throughput role=client sent=100 fresh=on "
        "fresh_burst=1 fresh_bursts=100",
        "  arm=OFF RESULT: framework=tickle scenario=best_effort_throughput role=client sent=7",
        "  arm=ON RESULT: framework=tickle scenario=best_effort_throughput role=server recv=100 fresh=on "
        "fresh_in_window=100 fresh_no_stamp=0 fresh_burst=1 fresh_newest=100 fresh_age_p50_us=10.0 "
        "fresh_age_p99_us=20.0 fresh_age_min_us=5.0",
    ])
    runs = parse_s6(s6, "x")
    check("s6: the ON pair only", len(runs) == 1 and runs[0]["cell"] == "s6_best_effort_throughput_p2"
          and runs[0]["client"]["sent"] == "100")
    check("s6: read on one clock", read_run(runs[0])["age_p50_us"] == 10.0)
    stamp = _session({})
    for fw in FRAMEWORKS:
        for rep in (1, 2, 3):
            cli = _client(fw, bursts=5000, sent=5000) | {"fresh_creation": "stamp"}
            stamp.append(_run("s6_p2", fw, rep, "same", _server(fw, 5 if fw == "tickle" else 50, 80, 2, taken=900,
                                                                  newest=900), cli=cli))
    _, v = summarise(stamp, out=quiet)
    check("max rate: share NOT SCORED, age scored", v[("s6_p2", "newest_share")].startswith("NOT SCORED")
          and v[("s6_p2", "age_p50_us")] == "WIN")

    if failures:
        print("SELFTEST FAIL: " + "; ".join(failures))
        return 1
    print(f"SELFTEST PASS ({len(checked)} checks)")
    return 0


def main():
    global FRAMEWORKS, SCORED_VENDORS  # --frameworks narrows the session's required set
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?")
    ap.add_argument("--s6", nargs="*", default=[])
    ap.add_argument("--frameworks", default=" ".join(FRAMEWORKS),
                    help="the frameworks the session must hold (default all three; the control is required of each). "
                         "A PC dry run without FastDDS's code generator passes 'tickle cyclonedds'.")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    FRAMEWORKS = tuple(args.frameworks.split())
    if not FRAMEWORKS or FRAMEWORKS[0] != "tickle":
        ap.error("--frameworks must start with tickle")
    SCORED_VENDORS = tuple(f for f in FRAMEWORKS if f in ("fastdds", "cyclonedds"))
    if not args.log:
        ap.error("a keep_last_freshness.sh output is required (it holds the control)")
    try:
        with open(args.log, encoding="utf-8", errors="replace") as fh:
            runs = parse_fresh_log(fh.read())
        for path in args.s6:
            with open(path, encoding="utf-8", errors="replace") as fh:
                runs += parse_s6(fh.read(), path.rsplit("/", 1)[-1])
    except OSError as e:
        print(f"cannot read input: {e}")
        return 2
    if not runs:
        print("no runs found: nothing to read (NOT READ)")
        return 1
    ok, _ = summarise(runs)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
