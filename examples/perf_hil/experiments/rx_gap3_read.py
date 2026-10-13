#!/usr/bin/env python3
"""rx_gap3_read.py - the rig A/B of ab/rx-gap3 against ab/rx-gap2 (481b9e3e) and origin/main (0c9f9a3b), read by rules
written 2026-10-13 before any run (~/rig_jobs/rig_queue_rx_gap3.sh). Nothing here is judged by hand: every rule below is
implemented in this file. The parsers come from rx_gap2_read.py; what differs is the arms, PRIMARY and NO HARM.

WHAT IS TESTED. B (the tip pushed as ab/rx-gap3) is A (481b9e3e, ab/rx-gap2's tip) plus:
  (1) the wait's deadline timer only after a wait that ended for a datagram (the publisher's CPU per MB was WORSE in
      both BEST_EFFORT cells of the rx-gap2 A/B: 1 MB +0.084, 4 MB +0.031 ms/MB; ~2.8 timerfd_settime(), ~2.8 read()
      and ~1 wake-up more per 1 MB sample on the PC bench publisher);
  (2) the rx line's idle_skips= (a counter only);
  (3) the hybrid reader's gap judged by its own sample's layout (a lost first run after an MTU sample read as 45 lost
      MTU units), and p forgetting a few events its clean frames outlived (the 1 MB RELIABLE r3 VOID: 393 samples
      below n_max at 0% loss).
  M (0c9f9a3b, origin/main) is the arm the BEST_EFFORT publisher regression was measured against.

INPUT. The four stage-1 invocations of rmw_keepall_rig.sh (OUT.runs/ each), cross-host at 0% loss, arms tickle@head
(B), tickle@pre (A, PRE_SHA), tickle@ack (M, ACK_SHA - the harness's second variant slot), fastdds, cyclonedds:
  s1_1m_rel  Array1m 30 Hz RELIABLE KEEP_LAST 10        s1_1m_be  Array1m 30 Hz BEST_EFFORT KEEP_LAST 5
  s1_4m_rel  Array4m 15 Hz RELIABLE KEEP_LAST 10        s1_4m_be  Array4m 15 Hz BEST_EFFORT KEEP_LAST 5

RULES
  VOID (the cell is not read), any of:
    - fewer than 3 usable runs of head, pre or main;
    - a RELIABLE cell where a head, pre or main run did not deliver every sample;
    - out_of_order > 0 on a head, pre or main subscriber's rmw_tickle delivery lines (summed);
    - TREATMENT (each arm reports its own; a line or field missing is "TREATMENT NOT READ"):
        head: the subscriber's "Node <n> rx:" line carries direct_reads=, empty_reads=, nonblock=, timer_waits=,
          idle_skips= and timer_plain=; direct_reads > 0, nonblock=1, timer_waits > 0, empty_reads per received sample
          at most 8 (1 MB) / 32 (4 MB); the PUBLISHER's rx line carries timer_plain= with timer_plain > 0 (the change
          engaged where the regression was) - else "TREATMENT NOT APPLIED: <which>"; the publisher's hybrid line carries
          n_hist= (absent: NOT READ).
        pre: the subscriber's rx line carries timer_waits= (A has the timer) and NOT timer_plain= (else pre is not A).
        main: the subscriber's rx line present and WITHOUT empty_reads= (M's line; else main is not M).
  FLOOR per cell, metric and pair: 2 x SE of the difference of means (Welch).
  PRIMARY:
    (a) BEST_EFFORT publisher, both BE cells: head's pub CPU per MB not WORSE than main's, d = mean(head) - mean(main)
        <= FLOOR. WORSE in either BE cell: MISSED.
    (b) n at 0% loss, every cell: no head run with more than 2 samples below n_max (the publisher's n_hist, buckets 1-6
        of 7). One such run: MISSED (it is the change's claim, not a treatment check: rx-gap2's r3 had 393).
  NO HARM per cell (all four), head against pre (A): sub CPU per MB WORSE iff d > FLOOR (the receive gain kept); pub
    CPU per MB WORSE iff d > FLOOR; latency median and p99 lower bound WORSE iff d > max(FLOOR, 0.1% of pre's mean);
    BEST_EFFORT delivered % WORSE iff it fell by more than 1 point.
  NO HARM per cell (all four), head against main (M): pub CPU per MB WORSE iff d > FLOOR.
  REPORTED, never deciding: every arm's means; head against fastdds and cyclonedds (and rx-gap2's PRIMARY (b), head
    within 5% of Fast DDS's sub CPU); samples below n_max per arm; the head subscriber's idle_skips and empty_reads per
    sample; the publisher's timer_fires / timer_plain per sample; page faults per MB.
  VERDICT, last line "RX GAP3 VERDICT: PASS|FAIL|NO VERDICT (reasons)": FAIL on any PRIMARY MISSED or NO HARM WORSE;
    NO VERDICT when a cell is missing, none is read, a BE cell is VOID (PRIMARY (a) needs both), or anything deciding
    is NOT READ; else PASS. Exit 0 / 1 / 2.
  --preflight (PC, REPS=1): the reading runs end to end; PREFLIGHT FAILED when a cell has no usable head, pre or main
    run, any treatment is NOT READ / NOT APPLIED / not its commit, or a RELIABLE run was incomplete. Last line
    "PREFLIGHT OK" (exit 0) or "PREFLIGHT FAILED: ..." (exit 1).

Usage: rx_gap3_read.py OUT [OUT ...] [--preflight]      rx_gap3_read.py --selftest
"""
import math
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rx_gap2_read import below_n_max, fin, mean_or_nan, out_of_order, pgf_per_mb, welch  # noqa: E402

CELLS = {("Array1m", True, 30): "s1_1m_rel", ("Array1m", False, 30): "s1_1m_be",
         ("Array4m", True, 15): "s1_4m_rel", ("Array4m", False, 15): "s1_4m_be"}
CELL_ORDER = ("s1_1m_rel", "s1_1m_be", "s1_4m_rel", "s1_4m_be")
BE_CELLS = ("s1_1m_be", "s1_4m_be")
HEAD, PRE, MAIN = "tickle@head", "tickle@pre", "tickle@ack"
TICKLE = (HEAD, PRE, MAIN)
VENDORS = ("fastdds", "cyclonedds")
MIN_RUNS = 3
FASTDDS_SLACK = 1.05
EMPTY_PER_SAMPLE = {"Array1m": 8.0, "Array4m": 32.0}
BELOW_N_MAX = 2
LAT_REL_FLOOR = 0.001
BE_DELIVERY_POINTS = 1.0
RX_RE = re.compile(r"Node \d+ rx: (.*)$", re.M)
SUB_FIELDS = ("direct_reads", "empty_reads", "nonblock", "timer_waits", "idle_skips", "timer_plain")
PUB_FIELDS = ("timer_waits", "timer_fires", "timer_plain")


def rx_fields(text, names):
    """(state, fields): ("no_line", {}) when no "Node <n> rx:" line; else ("ok", {name: the sum over the lines}) for
    each name every line carries (a field some line lacks is left out)."""
    lines = RX_RE.findall(text)
    if not lines:
        return "no_line", {}
    fields = {}
    for name in names:
        vals = [re.search(rf"\b{name}=(\d+)", x) for x in lines]
        if all(v is not None for v in vals):
            fields[name] = sum(int(v.group(1)) for v in vals)
    return "ok", fields


def treatment_problem(r):
    """'' when the run's treatment is as required, else why not (the text starts with the VOID class)."""
    arm, state, fields = r["arm"], r.get("rx_state", "no_line"), r.get("rx_fields", {})
    if state == "no_line":
        return "TREATMENT NOT READ: no 'Node <n> rx:' line in the subscriber log"
    if arm == MAIN:
        return "TREATMENT: main carries empty_reads= (not 0c9f9a3b)" if "empty_reads" in fields else ""
    if arm == PRE:
        if "timer_plain" in fields:
            return "TREATMENT: pre carries timer_plain= (not 481b9e3e)"
        if "timer_waits" not in fields:
            return "TREATMENT: pre lacks timer_waits= (not 481b9e3e)"
        return ""
    missing = [f for f in SUB_FIELDS if f not in fields]
    if missing:
        return "TREATMENT NOT READ: the subscriber's rx line lacks " + ", ".join(f + "=" for f in missing)
    pub_state, pub = r.get("pub_state", "no_line"), r.get("pub_fields", {})
    if pub_state == "no_line" or "timer_plain" not in pub:
        return "TREATMENT NOT READ: the publisher's rx line lacks timer_plain="
    if r.get("below") is None:
        return "TREATMENT NOT READ: no n_hist= on the publisher's hybrid line"
    bad = []
    if fields["direct_reads"] <= 0:
        bad.append("direct_reads=0")
    if fields["nonblock"] != 1:
        bad.append(f"nonblock={fields['nonblock']}")
    if fields["timer_waits"] <= 0:
        bad.append("timer_waits=0")
    limit = EMPTY_PER_SAMPLE.get(r.get("topic", "Array1m"), 8.0)
    recv = r.get("recv", 0)
    per = fields["empty_reads"] / recv if recv else float("inf")
    if per > limit:
        bad.append(f"empty_reads {per:.1f} a sample > {limit:g}")
    if pub["timer_plain"] <= 0:
        bad.append("the publisher's timer_plain=0")
    return ("TREATMENT NOT APPLIED: " + ", ".join(bad)) if bad else ""


def compare(a, b):
    """(d, se) of mean(a) - mean(b), or None with fewer than MIN_RUNS finite values either side."""
    x, y = fin(a), fin(b)
    if len(x) < MIN_RUNS or len(y) < MIN_RUNS:
        return None
    return welch(x, y)


def harm(arms, metric, against, relative, out, label):
    c = compare([r.get(metric) for r in arms.get(HEAD, [])], [r.get(metric) for r in arms.get(against, [])])
    if c is None:
        out(f"  NO HARM {label}: NOT READ (fewer than 3 finite values)")
        return "NOT READ"
    d, se = c
    thr = 2 * se
    if relative:
        thr = max(thr, LAT_REL_FLOOR * abs(statistics.mean(fin(r.get(metric) for r in arms[against]))))
    v = "WORSE" if d > thr else "HELD"
    out(f"  NO HARM {label}: d {d:+.4f}, threshold {thr:.4f} -> {v}")
    return v


def judge_cell(name, reliable, arms, out=print):
    head = arms.get(HEAD, [])
    res = dict(name=name, void=[], treatment_bad=[], incomplete_bad=[], a=None, b=None, harm={})
    out(f"=== {name}: {'RELIABLE' if reliable else 'BEST_EFFORT'}; usable runs: "
        + ", ".join(f"{a} {len(arms.get(a, []))}" for a in TICKLE + VENDORS) + " ===")
    counts = {a: len(arms.get(a, [])) for a in TICKLE}
    if min(counts.values()) < MIN_RUNS:
        res["void"].append(f"fewer than {MIN_RUNS} usable runs ({', '.join(f'{a} {n}' for a, n in counts.items())})")
    for r in [r for a in TICKLE for r in arms.get(a, [])]:
        tag = f"{r['arm']} r{r['rep']}"
        why = treatment_problem(r)
        out(f"  treatment {tag}: rx {r.get('rx_state', 'no_line')} {r.get('rx_fields', {})}, pub rx "
            f"{r.get('pub_fields', {})}, below n_max {r.get('below')}" + (f" -> {why}" if why else " -> ok"))
        if why:
            res["treatment_bad"].append(f"{tag} {why}")
        if reliable and r.get("incomplete"):
            res["incomplete_bad"].append(f"{tag} recv {r.get('recv')} lost {r.get('lost')}")
        if (r.get("ooo") or 0) > 0:
            res["void"].append(f"{tag} out_of_order={r['ooo']}")
    if res["treatment_bad"]:
        res["void"].append("; ".join(res["treatment_bad"]))
    if res["incomplete_bad"]:
        res["void"].append("RELIABLE run(s) delivered less than published: " + "; ".join(res["incomplete_bad"]))
    # ---- context, always printed --------------------------------------------------------------------------------
    for metric in ("cpu_sub", "cpu_pub", "lat_med", "p99_lb", "delivered_pct", "pgf_per_mb", "below"):
        line = [f"  {metric:13s}"]
        for a in TICKLE + VENDORS:
            xs = fin(r.get(metric) for r in arms.get(a, []))
            line.append(f"{a.replace('tickle@', '').replace('ack', 'main')} {mean_or_nan(xs):9.3f}")
        out(" ".join(line))
    for r in head:
        recv = r.get("recv") or 0
        f, p = r.get("rx_fields", {}), r.get("pub_fields", {})
        if recv:
            out(f"  head r{r['rep']}: idle_skips {f.get('idle_skips', float('nan')) / recv:.2f} and empty_reads "
                f"{f.get('empty_reads', float('nan')) / recv:.2f} a sample; publisher timer_fires "
                f"{p.get('timer_fires', float('nan')) / recv:.2f}, timer_plain {p.get('timer_plain', float('nan')) / recv:.2f}"
                " a sample (not deciding)")
    for v in VENDORS:
        parts = []
        for metric in ("cpu_sub", "cpu_pub", "lat_med", "p99_lb"):
            hm = mean_or_nan(fin(r.get(metric) for r in head))
            vm = mean_or_nan(fin(r.get(metric) for r in arms.get(v, []) if r.get("recv", 0) > 0))
            parts.append(f"{metric} {hm - vm:+.3f}")
        out(f"  context head - {v}: " + ", ".join(parts) + " (not deciding)")
    fd = fin(r.get("cpu_sub") for r in arms.get("fastdds", []) if r.get("recv", 0) > 0)
    h_sub = mean_or_nan(fin(r.get("cpu_sub") for r in head))
    if fd and not math.isnan(h_sub):
        out(f"  context head sub CPU / fastdds {h_sub / statistics.mean(fd):.3f} (rx-gap2's (b) at "
            f"{FASTDDS_SLACK}; not deciding)")
    if res["void"]:
        out(f"  VOID {name}: " + " | ".join(res["void"]))
        return res
    # ---- PRIMARY -------------------------------------------------------------------------------------------------
    if name in BE_CELLS:
        v = harm(arms, "cpu_pub", MAIN, False, out, "(PRIMARY (a)) pub CPU head vs main")
        res["a"] = {"HELD": "MET", "WORSE": "MISSED"}.get(v, "NOT READ")
        out(f"  PRIMARY (a) BEST_EFFORT pub CPU not worse than main: {res['a']}")
    stalled = [f"r{r['rep']} {r['below']}" for r in head if (r.get("below") or 0) > BELOW_N_MAX]
    res["b"] = "MISSED" if stalled else "MET"
    out(f"  PRIMARY (b) no head run above {BELOW_N_MAX} samples below n_max: {res['b']}"
        + (f" ({', '.join(stalled)})" if stalled else ""))
    # ---- NO HARM -------------------------------------------------------------------------------------------------
    res["harm"]["cpu_sub vs pre"] = harm(arms, "cpu_sub", PRE, False, out, "cpu_sub vs pre")
    res["harm"]["cpu_pub vs pre"] = harm(arms, "cpu_pub", PRE, False, out, "cpu_pub vs pre")
    res["harm"]["cpu_pub vs main"] = harm(arms, "cpu_pub", MAIN, False, out, "cpu_pub vs main")
    res["harm"]["lat_med vs pre"] = harm(arms, "lat_med", PRE, True, out, "lat_med vs pre")
    res["harm"]["p99_lb vs pre"] = harm(arms, "p99_lb", PRE, True, out, "p99_lb vs pre")
    if not reliable:
        hd, pd = fin(r.get("delivered_pct") for r in head), fin(r.get("delivered_pct") for r in arms.get(PRE, []))
        if len(hd) < MIN_RUNS or len(pd) < MIN_RUNS:
            res["harm"]["delivered_pct vs pre"] = "NOT READ"
            out("  NO HARM delivered_pct vs pre: NOT READ")
        else:
            drop = statistics.mean(pd) - statistics.mean(hd)
            res["harm"]["delivered_pct vs pre"] = "WORSE" if drop > BE_DELIVERY_POINTS else "HELD"
            out(f"  NO HARM delivered_pct vs pre: head {statistics.mean(hd):.2f}% pre {statistics.mean(pd):.2f}% "
                f"(drop {drop:+.2f} points, allowed {BE_DELIVERY_POINTS}) -> {res['harm']['delivered_pct vs pre']}")
    return res


def verdict(results, problems, out=print):
    fail, unread, voids = [], list(problems), []
    for name in CELL_ORDER:
        r = results.get(name)
        if r is None:
            continue
        if r["void"]:
            voids.append(f"{name} VOID")
            if name in BE_CELLS:
                unread.append(f"PRIMARY (a) NOT READ: {name} VOID")
            continue
        for key, label in (("a", "PRIMARY (a)"), ("b", "PRIMARY (b)")):
            if r[key] == "MISSED":
                fail.append(f"{name} {label} MISSED")
            elif r[key] is not None and r[key] != "MET":
                unread.append(f"{name} {label} NOT READ")
        for metric, v in r["harm"].items():
            if v == "WORSE":
                fail.append(f"{name} NO HARM {metric} WORSE")
            elif v != "HELD":
                unread.append(f"{name} NO HARM {metric} NOT READ")
    read = [n for n in CELL_ORDER if n in results and not results[n]["void"]]
    if not read:
        unread.append("no cell was read")
    if fail:
        text, rc = "FAIL (" + "; ".join(fail + unread + voids) + ")", 1
    elif unread:
        text, rc = "NO VERDICT (" + "; ".join(unread + voids) + ")", 2
    else:
        text, rc = "PASS (" + f"{', '.join(read)} read, PRIMARY (a) and (b) met, nothing WORSE" + ")", 0
    out(f"RX GAP3 VERDICT: {text}")
    return text, rc


def read_out(out_path):
    """(cell name, reliable, arms) of one invocation, or (None, reason, None)."""
    from largemsg_rig_summary import load
    runs = Path(f"{out_path}.runs")
    if not (runs / "index.txt").exists():
        return None, f"{runs}/index.txt missing", None
    meta, samehost, recs = load(runs)
    reliable = "--reliable" in meta.get("qos_args", "")
    rate = int(meta.get("pub_rate", "0") or 0)
    topics = {r["topic"] for r in recs}
    losses = {r["loss"] for r in recs}
    if samehost or len(topics) != 1 or losses != {0}:
        return None, f"{runs}: not one cross-host 0% cell (topics {sorted(topics)}, losses {sorted(losses)})", None
    name = CELLS.get((topics.pop(), reliable, rate))
    if name is None:
        return None, f"{runs}: not one of the four s1 cells (qos [{meta.get('qos_args')}], {rate} Hz)", None
    arms = {}
    for r in recs:
        stem = f"{r['arm']}_{r['topic']}_l{r['loss']}_r{r['rep']}"
        if r["void"]:
            print(f"  VOID run {stem}: {r['void']}")
            continue
        r["pgf_per_mb"] = pgf_per_mb(runs / f"{stem}_sub.pgf", r["topic"], rate)
        if r["arm"].startswith("tickle@"):
            sub = (runs / f"{stem}_sub.log").read_text(errors="replace") if (runs / f"{stem}_sub.log").exists() else ""
            pub = (runs / f"{stem}_pub.log").read_text(errors="replace") if (runs / f"{stem}_pub.log").exists() else ""
            r["rx_state"], r["rx_fields"] = rx_fields(sub, SUB_FIELDS)
            r["pub_state"], r["pub_fields"] = rx_fields(pub, PUB_FIELDS)
            r["ooo"] = out_of_order(sub)
            r["below"] = below_n_max(pub)
        arms.setdefault(r["arm"], []).append(r)
    return name, reliable, arms


def preflight_check(results):
    bad = []
    for name, r in results.items():
        for a in TICKLE:
            if r.get("_n", {}).get(a, 0) == 0:
                bad.append(f"{name}: no usable {a} run")
        bad += [f"{name}: {x}" for x in r["treatment_bad"]]
        bad += [f"{name}: RELIABLE incomplete {x}" for x in r["incomplete_bad"]]
    return bad


def main(argv):
    pre_mode = "--preflight" in argv
    outs = [a for a in argv if not a.startswith("--")]
    if not outs:
        print(__doc__)
        return 2
    results, problems = {}, []
    for o in outs:
        name, reliable, arms = read_out(o)
        if name is None:
            print(f"NOT READ {o}: {reliable}")
            problems.append(f"{o} not read")
            continue
        if name in results:
            problems.append(f"{name} given twice")
            continue
        res = judge_cell(name, reliable, arms)
        res["_n"] = {a: len(arms.get(a, [])) for a in TICKLE}
        results[name] = res
    problems += [f"{n} not in the input" for n in CELL_ORDER if n not in results]
    _text, rc = verdict(results, problems)
    if pre_mode:
        bad = preflight_check(results) + [p for p in problems if "not read" in p]
        if not results:
            bad.append("no cell read")
        if bad:
            print("PREFLIGHT FAILED: " + " | ".join(bad))
            return 1
        print(f"PREFLIGHT OK: {len(results)} cell(s) read end to end, treatment present on every head, pre and main run")
        return 0
    return rc


# ---- selftest --------------------------------------------------------------------------------------------------------
HEAD_SUB = {"direct_reads": 5000, "empty_reads": 300, "nonblock": 1, "timer_waits": 9000, "idle_skips": 800,
            "timer_plain": 40}
HEAD_PUB = {"timer_waits": 3000, "timer_fires": 20, "timer_plain": 3000}


def _rec(arm, rep, cpu_sub=2.3, cpu_pub=3.9, lat=10.0, p99=12.0, dpct=100.0, recv=900, below=None):
    tickle = arm.startswith("tickle@")
    sub = {HEAD: dict(HEAD_SUB), PRE: {"direct_reads": 4000, "empty_reads": 290, "nonblock": 1, "timer_waits": 9000,
                                       "idle_skips": 790}, MAIN: {"direct_reads": 3000}}.get(arm, {})
    pub = {HEAD: dict(HEAD_PUB), PRE: {"timer_waits": 3000, "timer_fires": 2000}, MAIN: {}}.get(arm, {})
    if below is None:
        below = {HEAD: 1, PRE: 1, MAIN: 150}.get(arm)
    return dict(arm=arm, rep=rep, cpu_sub=cpu_sub, cpu_pub=cpu_pub, lat_med=lat, p99_lb=p99, delivered_pct=dpct,
                recv=recv, lost=0 if dpct >= 100 else 5, incomplete=dpct < 100 or recv == 0, topic="Array1m",
                pgf_per_mb=0.0, rx_state="ok" if tickle else None, rx_fields=sub, pub_state="ok" if tickle else None,
                pub_fields=pub, below=below if tickle else None, ooo=0 if tickle else None)


def _cell(head_pub=(3.90, 3.91, 3.89, 3.92, 3.90), pre_pub=(4.00, 4.01, 3.99, 4.02, 4.00),
          main_pub=(3.93, 3.92, 3.94, 3.91, 3.93), head_sub=(2.30, 2.31, 2.29, 2.32, 2.30),
          pre_sub=(2.31, 2.30, 2.32, 2.29, 2.31)):
    arms = {HEAD: [_rec(HEAD, i + 1, cpu_sub=s, cpu_pub=p) for i, (s, p) in enumerate(zip(head_sub, head_pub))],
            PRE: [_rec(PRE, i + 1, cpu_sub=s, cpu_pub=p) for i, (s, p) in enumerate(zip(pre_sub, pre_pub))],
            MAIN: [_rec(MAIN, i + 1, cpu_sub=2.5, cpu_pub=p) for i, p in enumerate(main_pub)],
            "fastdds": [_rec("fastdds", i + 1, cpu_sub=2.2) for i in range(3)],
            "cyclonedds": [_rec("cyclonedds", i + 1, cpu_sub=3.0) for i in range(3)]}
    return arms


def _run(cells, problems=()):
    quiet = []
    results = {n: judge_cell(n, n.endswith("_rel"), a, out=quiet.append) for n, a in cells.items()}
    text, rc = verdict(results, list(problems) + [f"{n} not in the input" for n in CELL_ORDER if n not in cells],
                       out=quiet.append)
    return text, rc, results, "\n".join(quiet)


def _with(arms, arm, rep, **change):
    arms[arm] = [dict(r, **change) if r["rep"] == rep else r for r in arms[arm]]
    return arms


def selftest():
    fails = []

    def check(what, ok):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}")
        if not ok:
            fails.append(what)

    def four(**over):
        return {n: over.get(n, _cell()) for n in CELL_ORDER}

    print("parsers")
    line = ("[I] Node 2 rx: socket_drops=0 direct_reads=17 nonblock=1 empty_reads=4 idle_skips=9 gro_on=0 "
            "timer_waits=99 timer_sets=3 timer_fires=2 timer_plain=5\n")
    check("sub fields", rx_fields("x\n" + line, SUB_FIELDS) == ("ok", {"direct_reads": 17, "empty_reads": 4,
                                                                         "nonblock": 1, "timer_waits": 99,
                                                                         "idle_skips": 9, "timer_plain": 5}))
    check("no rx line", rx_fields("nothing\n", SUB_FIELDS) == ("no_line", {}))
    print("case PASS: head's pub CPU below main's and pre's, sub CPU as pre's, no stall")
    text, rc, _r, _o = _run(four())
    check(f"PASS exit 0 ({text[:60]})", rc == 0 and text.startswith("PASS"))
    print("case PRIMARY (a): head's BE pub CPU worse than main's in one BE cell; the same in a RELIABLE cell is NO HARM")
    worse = _cell(head_pub=(4.00, 4.01, 3.99, 4.02, 4.00), pre_pub=(4.00, 4.01, 3.99, 4.02, 4.00))
    text, rc, _r, _o = _run(four(s1_4m_be=worse))
    check(f"FAIL naming (a) ({text[:70]})", rc == 1 and "s1_4m_be PRIMARY (a) MISSED" in text)
    text, rc, _r, _o = _run(four(s1_4m_rel=worse))
    check("RELIABLE: FAIL by NO HARM cpu_pub vs main, not (a)",
          rc == 1 and "s1_4m_rel NO HARM cpu_pub vs main WORSE" in text and "PRIMARY (a)" not in text)
    print("case NO HARM vs pre: sub CPU worse (the receive gain given back); pub CPU worse than pre only")
    sub = _cell(head_sub=(2.40, 2.41, 2.39, 2.42, 2.40))
    text, rc, _r, _o = _run(four(s1_1m_rel=sub))
    check("sub CPU vs pre WORSE -> FAIL", rc == 1 and "s1_1m_rel NO HARM cpu_sub vs pre WORSE" in text)
    pubpre = _cell(head_pub=(3.95, 3.96, 3.94, 3.97, 3.95), pre_pub=(3.90, 3.91, 3.89, 3.92, 3.90),
                   main_pub=(4.00, 4.01, 3.99, 4.02, 4.00))
    text, rc, _r, _o = _run(four(s1_1m_be=pubpre))
    check("pub CPU vs pre WORSE (main fine) -> FAIL", rc == 1 and "s1_1m_be NO HARM cpu_pub vs pre WORSE" in text
          and "PRIMARY (a) MISSED" not in text)
    print("case PRIMARY (b): one head run 393 samples below n_max; control 2")
    st = _with(_cell(), HEAD, 3, below=393)
    text, rc, _r, _o = _run(four(s1_1m_rel=st))
    check("FAIL naming (b)", rc == 1 and "s1_1m_rel PRIMARY (b) MISSED" in text)
    ok2 = _with(_cell(), HEAD, 3, below=2)
    _t, rc, _r, _o = _run(four(s1_1m_rel=ok2))
    check("control: 2 below n_max passes", rc == 0)
    print("case VOID: treatments")
    for what, arm, change, word in (
            ("head without idle_skips=", HEAD, {"rx_fields": {k: v for k, v in HEAD_SUB.items() if k != "idle_skips"}},
             "NOT READ"),
            ("head publisher without timer_plain=", HEAD, {"pub_fields": {"timer_waits": 9}}, "NOT READ"),
            ("head publisher timer_plain=0", HEAD, {"pub_fields": dict(HEAD_PUB, timer_plain=0)}, "NOT APPLIED"),
            ("head nonblock=0", HEAD, {"rx_fields": dict(HEAD_SUB, nonblock=0)}, "NOT APPLIED"),
            ("pre carrying timer_plain=", PRE, {"rx_fields": dict(HEAD_SUB)}, "not 481b9e3e"),
            ("main carrying empty_reads=", MAIN, {"rx_fields": {"empty_reads": 3}}, "not 0c9f9a3b"),
            ("no rx line on main", MAIN, {"rx_state": "no_line", "rx_fields": {}}, "NOT READ")):
        bad = _with(_cell(), arm, 2, **change)
        text, rc, res, out = _run(four(s1_1m_rel=bad))
        check(f"{what}: s1_1m_rel VOID ({word}), the rest read, NO VERDICT only if a BE cell",
              bool(res["s1_1m_rel"]["void"]) and word in out and rc == 0)
        text, rc, res, out = _run(four(s1_1m_be=bad))
        check(f"{what} in a BE cell: NO VERDICT ((a) needs both)", rc == 2 and "PRIMARY (a) NOT READ" in text)
    print("case VOID: fewer than 3 main runs; RELIABLE incomplete main")
    few = _cell(main_pub=(3.9, 3.91))
    _t, _rc, res, _o = _run(four(s1_4m_rel=few))
    check("2 main runs -> VOID", "fewer than 3" in " ".join(res["s1_4m_rel"]["void"]))
    inc = _with(_cell(), MAIN, 1, incomplete=True, lost=3)
    _t, _rc, res, _o = _run(four(s1_4m_rel=inc))
    check("RELIABLE main run incomplete -> VOID", "delivered less" in " ".join(res["s1_4m_rel"]["void"]))
    print("case NO VERDICT: a cell missing")
    cells = four()
    del cells["s1_4m_rel"]
    text, rc, _r, _o = _run(cells)
    check("missing cell -> NO VERDICT", rc == 2 and "s1_4m_rel not in the input" in text)
    print(f"SELFTEST {'FAILED: ' + '; '.join(fails) if fails else 'OK'}")
    return 1 if fails else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    sys.exit(main(sys.argv[1:]))
