#!/usr/bin/env python3
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""dseed_ab_read.py OUT | --self-test - the reader of dseed_ab.sh (discovery-seeded retry interval A/B, 2026-11-04).

OUT holds one time_varying_netns.sh OUT per arm and round, OUT/<A|B|A2>-r<round>/. Every run is re-read from its own
files with time_varying_read.py's loader and X1 metric functions (the summaries print only a median and a range per
point, not the per-rep values the A/B needs); each OUT's summary.txt is read for its tree and its completion line.

Applies the rules pre-registered in dseed_ab.sh's header IN CODE (the constants below are the pre-registration,
written 2026-11-04 before any run; change them only before a run, and say so in the harness header):
  sample counts first (a point with under MIN_REPS usable reps in any arm is VOID), P1 (B's rel_small TIMER_PER_GAP at
  the treatment points), P2 (B no worse than A on OWD p99, RTT p99 and RELIABLE delivery at every point, against the
  A/A distance), the A/A control check (|A2 - A| over AA_VOID_REL of A voids that metric at that point), and the trace
  evidence (the data reader's recovery srtt 0 with a non-zero seed, field SEED_FIELD).

Prints the per-OUT provenance, the sample counts, the P1 table, the trace evidence, the P2 comparisons with every
WORSE and every noise VOID, then "VERDICT P1=<PASS|PARTIAL|FAIL|VOID> P2=<PASS|FAIL|VOID>" and a "HYPOTHESIS" line.
Exit 0 once OUT is read (the verdict is in the text), 2 when it cannot be read. --self-test runs the rules on
synthetic inputs (every PASS / PARTIAL / FAIL / VOID / WORSE path) and the trace parser on synthetic lines; it exits
1 if any expectation is not met.
"""

import os
import re
import shutil
import statistics
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import time_varying_read as tvr  # noqa: E402
from wifi_burst_read import kv, num, read_lines  # noqa: E402

# --- the pre-registration (dseed_ab.sh header, 2026-11-04, before any run) --------------------------------------------
SEED_FIELD = "discovery_seed_ns"  # the treatment's ESTTRACE field for the discovery seed (its name is not final)
SRTT_FIELD = "recovery_srtt_ns"  # the data reader's recovery estimate, 0 until a requested sample is recovered
RETRY_FIELD = "retry_interval_ns"  # report only: the retry interval the writer proxy uses
# The data reader's view of the data writer: echo (ctx 2; rel_large's sub) prints one "obj=wproxy" line per writer
# proxy, and the data writer is ping / pub, ctx 1. (ping's own obj=wproxy, writer_ctx=2, is the return stream.)
TRACE_FILE = "echo.err"
TRACE_OBJ = "wproxy"
TRACE_WRITER_CTX = "1"
ARMS = ("A", "B", "A2")  # A the base, B the treatment, A2 the base again: the A/A control
BASE, TREAT, CONTROL = "A", "B", "A2"
TREAT_POINTS = [(50, 1), (50, 2), (50, 5), (50, 10), (100, 1), (100, 2), (100, 5), (100, 10)]
CONTROL_POINTS = [(1, 1), (1, 10), (5, 1), (5, 10), (20, 1), (20, 10)]
CELLS = ("rel_small", "rel_large")
MIN_REPS = 2  # usable reps a point needs in every arm before any value of it is read
P1_CELL = "rel_small"
P1_METRIC = "TIMER_PER_GAP"
P1_PASS_MAX = 2.0  # B's median TIMER_PER_GAP at or under this at every treatment point -> PASS
P1_FAIL_MIN = 5.0  # over this at any treatment point while B's trace shows a non-zero seed -> FAIL
P2_METRICS = {"rel_small": ("OWD_P99", "RTT_P99", "DELIVERY"), "rel_large": ("OWD_P99", "DELIVERY")}
P2_REL_TOL = 0.10  # latency WORSE: median(B) - median(A) > max(|A2 - A|, this x median(A))
P2_DELIVERY_TOL = 0.001  # DELIVERY WORSE: A - B > max(|A2 - A|, 0.1 point)
AA_VOID_REL = 0.10  # |median(A2) - median(A)| over this x |median(A)|: the metric at the point is noise, VOID
P2_MAX_NOISE_VOID_FRAC = 0.5  # over this share of P2's comparisons voided as noise: P2 itself is VOID
LATENCY_METRICS = ("OWD_P99", "RTT_P99")
OUT_RE = re.compile(r"^(A2|A|B)-r([0-9]+)$")
FMT = {"TIMER_PER_GAP": "f2", "OWD_P99": "ms", "RTT_P99": "ms", "DELIVERY": "pct"}
METRIC_FN = {m[0]: m[2] for m in tvr.X1_METRICS}
METRIC_CELLS = {m[0]: m[1] for m in tvr.X1_METRICS}


def fmt(v, metric):
    return tvr.x1_fmt(v, FMT.get(metric, "f2"))


def pname(p):
    return f"{p[0]}ms/{p[1]}%"


# --- trace ------------------------------------------------------------------------------------------------------------


def trace_counts(lines):
    """The data reader's writer-proxy ESTTRACE lines: how many, how many with recovery srtt 0, how many of those carry
    a non-zero seed, whether the seed field is printed at all, and the retry intervals on the srtt-0 lines."""
    c = {"lines": 0, "srtt0": 0, "seeded": 0, "field": False, "retry": []}
    for line in lines:
        if not line.startswith("ESTTRACE "):
            continue
        d = kv(line)
        if d.get("obj") != TRACE_OBJ or d.get("writer_ctx") != TRACE_WRITER_CTX or SRTT_FIELD not in d:
            continue
        c["lines"] += 1
        if SEED_FIELD in d:
            c["field"] = True
        if num(d, SRTT_FIELD, -1) == 0:
            c["srtt0"] += 1
            if num(d, SEED_FIELD, 0) > 0:
                c["seeded"] += 1
            if RETRY_FIELD in d:
                c["retry"].append(num(d, RETRY_FIELD))
    return c


def pool_traces(ts):
    out = {"lines": 0, "srtt0": 0, "seeded": 0, "field": False, "retry": [], "runs": 0}
    for t in ts:
        if t is None:
            continue
        out["runs"] += 1
        for k in ("lines", "srtt0", "seeded"):
            out[k] += t[k]
        out["field"] = out["field"] or t["field"]
        out["retry"] += t["retry"]
    return out


def trace_text(t):
    if not t["runs"] or not t["lines"]:
        return "no trace lines"
    retry = f", retry_interval p50 {statistics.median(t['retry']) / 1e6:.2f} ms on srtt-0 lines" if t["retry"] else ""
    if not t["field"]:
        return f"absent ({SEED_FIELD} not printed; {t['srtt0']}/{t['lines']} lines srtt 0{retry})"
    share = 100.0 * t["seeded"] / t["lines"]
    return f"{t['seeded']}/{t['lines']} = {share:.1f}% srtt 0 with a non-zero seed ({t['srtt0']} srtt 0{retry})"


# --- loading ----------------------------------------------------------------------------------------------------------


def new_data():
    return {"reps": {}, "dropped": {}, "traces": {}, "outs": [], "void": []}


def load_out(data, arm, rnd, path):
    """Reads one time_varying OUT of arm `arm`, round `rnd`, into data."""
    prov = kv(" ".join(read_lines(os.path.join(path, "provenance.txt"))))
    summ = read_lines(os.path.join(path, "summary.txt"))
    complete = any(x.startswith("=== time_varying_netns ended") for x in summ)
    runs, _ = tvr.load(path)
    cv = tvr.control_void(runs)
    cr, _ = tvr.control_resends(runs)
    data["outs"].append({"arm": arm, "round": rnd, "path": path, "tree": prov.get("tree", "?"),
                         "dirty": prov.get("dirty_paths", "?"), "est_trace": prov.get("est_trace", "0"),
                         "complete": complete, "runs": len(runs), "cv": cv, "cr": cr})
    base50 = {c: tvr.x1_median([tvr.x1_owd(r, 0.5) for r in tvr.usable(tvr.static_runs(runs, "l0", c))])
              for c in CELLS}
    for r in runs:
        k = tvr.x1_key(r["arm"])
        if k is None or k == (1, 0) or r["cell"] not in CELLS:
            continue
        key = (arm, r["cell"], k)
        why = tvr.x1_rep_why(r, k[0], k[1], base50[r["cell"]])
        if cv:
            why.append("its invocation's s_l0 control lost: " + "; ".join(cv))
        if r["cell"] == "rel_large" and cr:
            why.append("its invocation's s_l0 rel_large resent: " + "; ".join(cr))
        tag = f"{arm}-r{rnd}/{r['arm']}-{r['cell']}-{r['rep']}"
        if why:
            data["dropped"].setdefault(key, []).append(f"{tag}: {'; '.join(why)}")
            continue
        vals = {m: METRIC_FN[m](r) for m in set(P2_METRICS[r["cell"]]) | {P1_METRIC} if r["cell"] in METRIC_CELLS[m]}
        err = os.path.join(path, "runs", f"{r['arm']}-{r['cell']}-{r['rep']}", TRACE_FILE)
        tr = trace_counts(read_lines(err)) if prov.get("est_trace") == "1" else None
        data["reps"].setdefault(key, []).append({"tag": tag, "vals": vals, "lat_void": bool(r["lat_void"]),
                                                 "trace": tr})


def load_all(out):
    data = new_data()
    names = sorted(os.listdir(out)) if os.path.isdir(out) else []
    for name in names:
        m = OUT_RE.match(name)
        if m and os.path.isdir(os.path.join(out, name, "runs")):
            load_out(data, m.group(1), int(m.group(2)), os.path.join(out, name))
    if not data["outs"]:
        return None
    return data


def tree_void(data):
    """The arms must be what they name: one tree each, A2 the same tree as A, B another."""
    why = []
    trees = {a: {o["tree"] for o in data["outs"] if o["arm"] == a} for a in ARMS}
    for a in ARMS:
        if len(trees[a]) != 1:
            why.append(f"arm {a} ran {len(trees[a])} trees: {sorted(trees[a])}")
    if trees[BASE] != trees[CONTROL]:
        why.append(f"A ({sorted(trees[BASE])}) and A2 ({sorted(trees[CONTROL])}) are not the same tree: no A/A control")
    if trees[TREAT] and trees[TREAT] == trees[BASE]:
        why.append(f"B runs the base tree {sorted(trees[TREAT])}: nothing is tested")
    return why


# --- the rules --------------------------------------------------------------------------------------------------------


def values(data, arm, cell, point, metric):
    rs = data["reps"].get((arm, cell, point), [])
    lat = metric in LATENCY_METRICS
    return [r["vals"].get(metric) for r in rs if r["vals"].get(metric) is not None and not (lat and r["lat_void"])]


def n_usable(data, arm, cell, point):
    return len(data["reps"].get((arm, cell, point), []))


def sample_void(data, cell, point):
    """Reasons the point has too few usable reps in some arm (empty: enough everywhere)."""
    return [f"{a} {n_usable(data, a, cell, point)} usable" for a in ARMS if n_usable(data, a, cell, point) < MIN_REPS]


def p1(data, emit):
    """P1 on rel_small at the treatment points. Returns the verdict."""
    emit(f"--- P1 (treatment works): {P1_CELL} {P1_METRIC} median per arm at the treatment points; PASS <= "
         f"{P1_PASS_MAX:g} everywhere, FAIL > {P1_FAIL_MIN:g} with a seed in B's trace")
    states = []
    for p in TREAT_POINTS:
        sv = sample_void(data, P1_CELL, p)
        med = {a: tvr.x1_median(values(data, a, P1_CELL, p, P1_METRIC)) for a in ARMS}
        tr = pool_traces(r["trace"] for r in data["reps"].get((TREAT, P1_CELL, p), []))
        txt = " ".join(f"{a}={fmt(med[a], P1_METRIC)}" for a in ARMS)
        if sv:
            st, why = "VOID", "sample count: " + ", ".join(sv)
        elif med[TREAT] is None:
            st, why = "VOID", f"B's {P1_METRIC} n/a (under {tvr.X1_MIN_GAPS} gaps a rep)"
        elif med[TREAT] <= P1_PASS_MAX:
            st, why = "PASS", f"B <= {P1_PASS_MAX:g}"
        elif med[TREAT] > P1_FAIL_MIN and tr["seeded"] > 0:
            st, why = "FAIL", f"B > {P1_FAIL_MIN:g} with {tr['seeded']} seeded trace lines"
        elif med[TREAT] > P1_FAIL_MIN:
            st, why = "VOID", f"B > {P1_FAIL_MIN:g} but no seeded trace line: the treatment is not seen"
        else:
            st, why = "PARTIAL", f"{P1_PASS_MAX:g} < B <= {P1_FAIL_MIN:g}"
        states.append(st)
        emit(f"P1 {pname(p):10} {st:7} {txt} ({why})")
    if "FAIL" in states:
        return "FAIL"
    if "VOID" in states:
        return "VOID"
    return "PARTIAL" if "PARTIAL" in states else "PASS"


def trace_report(data, emit):
    emit(f"--- TRACE (data reader {TRACE_FILE} obj={TRACE_OBJ} writer_ctx={TRACE_WRITER_CTX}, {SRTT_FIELD} 0 with "
         f"{SEED_FIELD} > 0), per arm and treatment point, pooled over usable reps")
    for p in TREAT_POINTS:
        for cell in CELLS:
            for a in ARMS:
                tr = pool_traces(r["trace"] for r in data["reps"].get((a, cell, p), []))
                emit(f"TRACE {pname(p):10} {cell:9} {a:2} {trace_text(tr)}")


def p2(data, emit):
    """P2 at every point, both cells. Returns (verdict, WORSE list, noise-VOID list)."""
    emit(f"--- P2 (no cost): B WORSE when its median is worse than A's by more than max(|A2 - A|, "
         f"{P2_REL_TOL:.0%} of A) (latency) / max(|A2 - A|, {100 * P2_DELIVERY_TOL:g} point) (DELIVERY); "
         f"|A2 - A| > {AA_VOID_REL:.0%} of A voids the metric there")
    worse, noise, svoid, compared = [], [], [], 0
    for p in TREAT_POINTS + CONTROL_POINTS:
        for cell in CELLS:
            sv = sample_void(data, cell, p)
            if sv:
                svoid.append((p, cell))
                emit(f"P2 {pname(p):10} {cell:9} VOID   sample count: {', '.join(sv)}")
                continue
            for m in P2_METRICS[cell]:
                vs = {a: values(data, a, cell, p, m) for a in ARMS}
                short = [f"{a} {len(vs[a])}" for a in ARMS if len(vs[a]) < MIN_REPS]
                if short:
                    svoid.append((p, cell))
                    emit(f"P2 {pname(p):10} {cell:9} {m:9} VOID   sample count (values): {', '.join(short)}")
                    continue
                ma, mb, mc = (tvr.x1_median(vs[a]) for a in (BASE, TREAT, CONTROL))
                aa = abs(mc - ma)
                n = "/".join(str(len(vs[a])) for a in (BASE, TREAT, CONTROL))
                txt = f"A={fmt(ma, m)} B={fmt(mb, m)} A2={fmt(mc, m)} n(A/B/A2)={n}"
                compared += 1
                if aa > AA_VOID_REL * abs(ma):
                    noise.append((p, cell, m, txt))
                    emit(f"P2 {pname(p):10} {cell:9} {m:9} VOID   noise: |A2 - A| {aa:.4g} > {AA_VOID_REL:.0%} of A; "
                         f"{txt}")
                    continue
                if m == "DELIVERY":
                    tol = max(aa, P2_DELIVERY_TOL)
                    bad = ma - mb > tol
                    delta = f"B - A {100 * (mb - ma):+.2f} points, tolerance {100 * tol:.2f}"
                else:
                    tol = max(aa, P2_REL_TOL * ma)
                    bad = mb - ma > tol
                    delta = f"B - A {mb - ma:+.2f} ms, tolerance {tol:.2f}"
                st = "WORSE" if bad else "ok"
                if bad:
                    worse.append((p, cell, m, f"{txt}; {delta}"))
                emit(f"P2 {pname(p):10} {cell:9} {m:9} {st:6} {txt}; {delta}")
    for p, cell, m, txt in worse:
        emit(f"P2 WORSE {pname(p)} {cell} {m}: {txt}")
    if worse:
        verdict = "FAIL"
    elif svoid or not compared or len(noise) > P2_MAX_NOISE_VOID_FRAC * compared:
        verdict = "VOID"
    else:
        verdict = "PASS"
    emit(f"P2 summary: {compared} comparisons, {len(worse)} WORSE, {len(noise)} noise VOID, "
         f"{len(set(svoid))} (point, cell) sample VOID -> {verdict}")
    return verdict, worse, noise


def evaluate(data):
    """Applies every rule; returns (lines, P1, P2)."""
    lines = []
    emit = lines.append
    for o in data["outs"]:
        emit(f"OUT {o['arm']}-r{o['round']}: tree={o['tree']} dirty_paths={o['dirty']} est_trace={o['est_trace']} "
             f"runs={o['runs']} {'complete' if o['complete'] else 'INCOMPLETE (no ended line in summary.txt)'}"
             + (f"; s_l0 control VOID: {'; '.join(o['cv'])}" if o["cv"] else "")
             + (f"; s_l0 rel_large resent: {'; '.join(o['cr'])}" if o["cr"] else ""))
    emit("--- sample counts (usable reps per arm; a point needs " + str(MIN_REPS) + " in every arm)")
    for cell in CELLS:
        for p in TREAT_POINTS + CONTROL_POINTS:
            n = " ".join(f"{a}={n_usable(data, a, cell, p)}" for a in ARMS)
            emit(f"COUNT {cell:9} {pname(p):10} {n}{'  VOID' if sample_void(data, cell, p) else ''}")
    for key in sorted(data["dropped"]):
        for why in data["dropped"][key]:
            emit(f"DROPPED rep {why}")
    gv = data["void"] + tree_void(data)
    if gv:
        emit("VOID (every verdict): " + "; ".join(gv))
        r1, r2 = "VOID", "VOID"
    else:
        r1 = p1(data, emit)
        trace_report(data, emit)
        r2, _, _ = p2(data, emit)
    emit(f"VERDICT P1={r1} P2={r2}")
    claim = ("seeding the retry interval from a discovery round trip ends the retry-timer storm at 50/100 ms "
             "(P1) at no cost in latency or delivery anywhere (P2)")
    if r1 == "FAIL" or r2 == "FAIL":
        h = "FALSIFIED"
    elif r1 == "PASS" and r2 == "PASS":
        h = "SUPPORTED"
    elif r1 == "PARTIAL" and r2 == "PASS":
        h = "PARTLY SUPPORTED (the storm is reduced, not removed)"
    else:
        h = "UNDECIDED (a VOID)"
    emit(f"HYPOTHESIS {h}: {claim}")
    return lines, r1, r2


# --- self-test --------------------------------------------------------------------------------------------------------


def st_trace(seeded):
    """A synthetic trace count: 20 lines, all srtt 0; seeded=None -> the field absent (a base build)."""
    return {"lines": 20, "srtt0": 20, "seeded": seeded or 0, "field": seeded is not None, "retry": [1e6] * 20}


def st_data(tweak=None, reps=3):
    """Every arm, cell and point with `reps` usable reps; the base storms (TIMER_PER_GAP 30 at 50/100 ms), B does not
    (0.5); latencies and delivery equal across arms with a small spread. tweak(arm, cell, point, i, rep) edits a rep;
    returning False drops it."""
    data = new_data()
    for a in ARMS:
        data["outs"].append({"arm": a, "round": 1, "path": "-", "tree": "b" if a == TREAT else "a", "dirty": "0",
                             "est_trace": "1", "complete": True, "runs": 0, "cv": [], "cr": []})
        for cell in CELLS:
            for p in TREAT_POINTS + CONTROL_POINTS:
                for i in range(reps):
                    storm = p[0] >= 50 and a != TREAT
                    rep = {"tag": f"{a}/{cell}/{pname(p)}/{i}", "lat_void": False,
                           "trace": st_trace(18 if a == TREAT else None),
                           "vals": {"TIMER_PER_GAP": 30.0 if storm else 0.5,
                                    "OWD_P99": 2.0 * p[0] + 10 + 0.1 * i, "DELIVERY": 0.99 - 0.0001 * i}}
                    if cell == "rel_small":
                        rep["vals"]["RTT_P99"] = 4.0 * p[0] + 10 + 0.1 * i
                    if tweak is None or tweak(a, cell, p, i, rep) is not False:
                        data["reps"].setdefault((a, cell, p), []).append(rep)
    return data


def self_test():
    cases = []

    def case(name, data, p1_want, p2_want, extra=None):
        cases.append((name, data, p1_want, p2_want, extra))

    case("baseline: B removes the storm, no cost", st_data(), "PASS", "PASS")

    def partial(a, cell, p, i, rep):
        if a == TREAT and p == (100, 1):
            rep["vals"]["TIMER_PER_GAP"] = 3.0
    case("PARTIAL: one treatment point between 2 and 5", st_data(partial), "PARTIAL", "PASS",
         lambda lines: any(x.startswith("HYPOTHESIS PARTLY") for x in lines))

    def fail(a, cell, p, i, rep):
        if a == TREAT and p == (50, 10):
            rep["vals"]["TIMER_PER_GAP"] = 12.0
    case("FAIL: a treatment point stays > 5 with a seeded trace", st_data(fail), "FAIL", "PASS",
         lambda lines: any(x.startswith("HYPOTHESIS FALSIFIED") for x in lines))

    def unseen(a, cell, p, i, rep):
        if a == TREAT and p == (50, 10):
            rep["vals"]["TIMER_PER_GAP"] = 12.0
            rep["trace"] = st_trace(0)
    case("VOID: > 5 with no seeded line (treatment not seen)", st_data(unseen), "VOID", "PASS",
         lambda lines: any("treatment is not seen" in x for x in lines))

    def few(a, cell, p, i, rep):
        return not (a == CONTROL and cell == "rel_small" and p == (100, 5) and i > 0)
    case("VOID: one arm with 1 usable rep at a treatment point", st_data(few), "VOID", "VOID",
         lambda lines: any(x.startswith("COUNT rel_small 100ms/5%") and x.endswith("VOID") for x in lines))

    def owd(a, cell, p, i, rep):
        if a == TREAT and cell == "rel_large" and p == (5, 10):
            rep["vals"]["OWD_P99"] *= 1.3
    case("WORSE: B's OWD p99 30% over A at a control point", st_data(owd), "PASS", "FAIL",
         lambda lines: any(x.startswith("P2 WORSE 5ms/10% rel_large OWD_P99") for x in lines))

    def deliv(a, cell, p, i, rep):
        if a == TREAT and cell == "rel_small" and p == (50, 2):
            rep["vals"]["DELIVERY"] -= 0.005
    case("WORSE: B's delivery 0.5 point under A", st_data(deliv), "PASS", "FAIL",
         lambda lines: any(x.startswith("P2 WORSE 50ms/2% rel_small DELIVERY") for x in lines))

    def small_d(a, cell, p, i, rep):
        if a == TREAT and cell == "rel_small" and p == (50, 2):
            rep["vals"]["DELIVERY"] -= 0.0005
    case("delivery 0.05 point under A is inside the 0.1 point tolerance", st_data(small_d), "PASS", "PASS")

    def noisy(a, cell, p, i, rep):
        if cell == "rel_small" and p == (20, 1):
            if a == CONTROL:
                rep["vals"]["RTT_P99"] *= 1.2
            if a == TREAT:
                rep["vals"]["RTT_P99"] *= 1.5
    case("noise: |A2 - A| > 10% voids the metric there, so B's 50% is not WORSE", st_data(noisy), "PASS", "PASS",
         lambda lines: any(x.startswith("P2 20ms/1%") and "RTT_P99" in x and "noise" in x for x in lines)
         and not any(x.startswith("P2 WORSE") for x in lines))

    def aa_wide(a, cell, p, i, rep):
        if cell == "rel_small" and p == (50, 1) and a in (CONTROL, TREAT):
            rep["vals"]["OWD_P99"] *= 1.08  # A2 8% over A (not noise): B 8% over A is inside max(|A2 - A|, 10%)
    case("B as far from A as A2 is: not WORSE", st_data(aa_wide), "PASS", "PASS")

    def noise_all(a, cell, p, i, rep):
        if a == CONTROL:
            for m in rep["vals"]:
                if m != "TIMER_PER_GAP":
                    rep["vals"][m] *= 1.5
    case("VOID: most P2 comparisons are noise", st_data(noise_all), "PASS", "VOID")

    d = st_data()
    d["outs"][2]["tree"] = "c"
    case("VOID: A2 is not A's tree", d, "VOID", "VOID",
         lambda lines: any("no A/A control" in x for x in lines))

    def lat(a, cell, p, i, rep):
        if a == BASE and cell == "rel_large" and p == (20, 10) and i > 0:
            rep["lat_void"] = True
    case("VOID: a loaded PC leaves A one latency value (DELIVERY still read)", st_data(lat), "PASS", "VOID",
         lambda lines: any(x.startswith("P2 20ms/10%") and "OWD_P99" in x and "values" in x for x in lines)
         and any(x.startswith("P2 20ms/10%") and "DELIVERY" in x and " ok " in x for x in lines))

    failed = total = 0
    for i, (name, data, w1, w2, extra) in enumerate(cases):
        lines, r1, r2 = evaluate(data)
        print(f"--- self-test {i + 1}: {name}")
        checks = [(f"P1={w1}", r1 == w1), (f"P2={w2}", r2 == w2)]
        if extra is not None:
            checks.append(("case's line check", extra(lines)))
        for label, ok in checks:
            total += 1
            failed += not ok
            print(f"  {'ok  ' if ok else 'FAIL'} {label} (got P1={r1} P2={r2})")
        if os.environ.get("DSEED_SELFTEST_VERBOSE"):
            print("\n".join("    | " + x for x in lines))

    # The trace parser on synthetic lines, through a file as the loader reads it.
    root = tempfile.mkdtemp(prefix="dseed_ab_selftest.")
    try:
        path = os.path.join(root, TRACE_FILE)
        base = (f"ESTTRACE t_ns=1 ctx=2 obj={TRACE_OBJ} sub=d1 reliable=1 writer_ctx={TRACE_WRITER_CTX} writer=w "
                f"{SRTT_FIELD}=%d recovery_rttvar_ns=0 {RETRY_FIELD}=%d")
        with open(path, "w", encoding="utf-8") as f:
            f.write("[INFO] Node open\n")
            f.write(base % (0, 1000000) + f" {SEED_FIELD}=300000000\n")
            f.write(base % (0, 1000000) + f" {SEED_FIELD}=0\n")
            f.write(base % (5000000, 2000000) + f" {SEED_FIELD}=300000000\n")
            f.write(f"ESTTRACE t_ns=1 ctx=2 obj=peer pub=p {SEED_FIELD}=300000000\n")
            f.write(f"ESTTRACE t_ns=1 ctx=1 obj={TRACE_OBJ} writer_ctx=2 {SRTT_FIELD}=0 {SEED_FIELD}=3\n")
        t = trace_counts(read_lines(path))
        tb = trace_counts([base % (0, 1000000)])
        checks = [("3 data-reader lines (peer and the return stream's wproxy ignored)", t["lines"] == 3),
                  ("2 with srtt 0, 1 of them seeded", t["srtt0"] == 2 and t["seeded"] == 1),
                  ("seed field seen", t["field"]),
                  ("a base line reads absent", "absent" in trace_text(pool_traces([tb])))]
        print("--- self-test trace parser")
        for label, ok in checks:
            total += 1
            failed += not ok
            print(f"  {'ok  ' if ok else 'FAIL'} {label}")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print(f"self-test: {total - failed} of {total} expectations met")
    return 1 if failed else 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] in ("--self-test", "--selftest"):
        return self_test()
    if len(sys.argv) != 2:
        print("usage: dseed_ab_read.py OUT | dseed_ab_read.py --self-test", file=sys.stderr)
        return 2
    data = load_all(sys.argv[1])
    if data is None:
        print(f"dseed_ab_read: no <A|B|A2>-r<round>/runs in {sys.argv[1]}", file=sys.stderr)
        return 2
    lines, _, _ = evaluate(data)
    print(f"=== dseed_ab_read {sys.argv[1]}: {len(data['outs'])} OUTs")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
