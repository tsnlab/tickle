#!/usr/bin/env python3
"""rx_gap_read.py - the rig A/B of ab/rx-gap against its parent, read by rules written 2026-10-13 before any run
(~/rig_queue_rx_gap.sh). Nothing here is judged by hand: every rule below is implemented in this file.

WHAT IS TESTED. B (the tip pushed as ab/rx-gap) is A (370637f4, origin/ab/hybrid-v2) plus four changes that cut
rmw_tickle's receive CPU per MB on large samples (~/claude_reports/rx_perf_gap_2026-10-13.md: the Pi 5 subscriber
spends +0.37 ms/MB at 1 MB and +0.28 at 4 MB more than Fast DDS):
  (1) a hybrid run datagram is read straight into its large-sample assembly (no large_place copy);
  (2) one wait that returns data, instead of epoll_pwait2 + its timeout hrtimer + recvmsg + a trailing EAGAIN read;
  (3) trimmed per-datagram bookkeeping and clock reads;
  (4) the io_uring drain kept armed across consecutive large samples.

INPUT. The four stage-1 invocations of rmw_keepall_rig.sh (OUT.runs/ each), all cross-host at 0% loss, arms
tickle@head (B), tickle@pre (A, PRE_SHA), fastdds, cyclonedds, read through largemsg_rig_summary.load() (whole counts,
clock-corrected latency median lat_med and p99 lower bound p99_lb, CPU ms per MB each side, delivered %):
  s1_1m_rel  Array1m 30 Hz RELIABLE KEEP_LAST 10        s1_1m_be  Array1m 30 Hz BEST_EFFORT KEEP_LAST 5
  s1_4m_rel  Array4m 15 Hz RELIABLE KEEP_LAST 10        s1_4m_be  Array4m 15 Hz BEST_EFFORT KEEP_LAST 5
A run whose identity is VOID in the summary (the subscriber's maps do not show the arm's library) is not usable.

RULES
  VOID (the cell is not read), any of:
    - fewer than 3 usable runs of head or of pre;
    - a RELIABLE cell where a head or pre run did not deliver every sample (the summary's incomplete: lost > 0 or
      nothing received);
    - out_of_order > 0 on a head or pre subscriber's rmw_tickle "Subscriber <id> delivery:" lines (summed). perf_test
      and the summary report no torn or out-of-order count; core has no "torn" counter (a sample that cannot be whole
      is abandoned and counted, never delivered torn), so torn is printed as not reported and never voids;
    - TREATMENT: every head run's subscriber log has rmw_tickle core's "Node <n> rx:" line, every such line carries
      direct_runs=<v> (runs read straight into their assembly), and their sum is > 0. Every pre run's subscriber log
      has the "Node <n> rx:" line and its direct_runs= is absent or 0. A run with no "Node <n> rx:" line at all, or a
      head line without direct_runs=, is "TREATMENT NOT READ" - VOID, never PASS on an absent line. A head sum of 0 is
      "TREATMENT NOT APPLIED"; a pre value > 0 means pre is not A.
  FLOOR per cell and metric: 2 x SE of the head - pre difference of means (Welch). The vendors' run-to-run spread
    (max - min) is printed beside it for context and does not decide.
  PRIMARY per cell, sub CPU ms per MB (cpu_sub):
    (a) head BETTER than pre: d = mean(head) - mean(pre) < -FLOOR;
    (b) head within 5% of Fast DDS or better: mean(head) <= 1.05 x mean(fastdds) over fastdds's usable runs that
        received something. No such run: (b) is NOT READ (never PASS).
    Fewer than 3 finite values of head or pre: (a) is NOT READ.
  NO HARM per cell, head against pre:
    latency median and p99 lower bound: WORSE iff d > max(FLOOR, 0.1% of pre's mean);
    pub CPU ms per MB: WORSE iff d > FLOOR;
    BEST_EFFORT delivered % (of published): WORSE iff mean(pre) - mean(head) > 1 point.
    Fewer than 3 finite values on either side: that metric is NOT READ.
  CONTEXT, never deciding: head against fastdds and cyclonedds, sub / pub CPU per MB and latency median / p99.
  VERDICT (the last line, "RX GAP VERDICT: PASS|FAIL|NO VERDICT (reasons)"):
    FAIL        any non-VOID cell whose PRIMARY (a) or (b) is MISSED, or any NO HARM metric WORSE;
    NO VERDICT  otherwise, when any of the four cells is not in the input (or given twice), no cell is non-VOID, or a
                PRIMARY sub-criterion or NO HARM metric of a non-VOID cell is NOT READ;
    PASS        otherwise: every non-VOID cell meets (a) and (b) and nothing is WORSE (VOID cells are listed).
  Exit 0 on PASS, 1 on FAIL, 2 on NO VERDICT.
  --preflight (the PC preflight, REPS=1: every cell has fewer than 3 runs, so NO VERDICT is expected): the reading
    must run end to end, and it is a PREFLIGHT FAILURE when any cell has no usable head or pre run, when any head or
    pre run's treatment is NOT READ / NOT APPLIED / pre not A, or when a RELIABLE head or pre run was incomplete. Fast
    DDS may deliver nothing over the PC's veth namespaces (~/rig_queue_rx_perf.sh): (b) then reads NOT READ, which is
    not a failure here. The last line is "PREFLIGHT OK" (exit 0) or "PREFLIGHT FAILED: ..." (exit 1).

Usage: rx_gap_read.py OUT [OUT ...] [--preflight]     (each OUT.runs/ one invocation; the cell is named from meta.txt)
       rx_gap_read.py --selftest
"""
import math
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

CELLS = {("Array1m", True, 30): "s1_1m_rel", ("Array1m", False, 30): "s1_1m_be",
         ("Array4m", True, 15): "s1_4m_rel", ("Array4m", False, 15): "s1_4m_be"}
CELL_ORDER = ("s1_1m_rel", "s1_1m_be", "s1_4m_rel", "s1_4m_be")
HEAD, PRE = "tickle@head", "tickle@pre"
VENDORS = ("fastdds", "cyclonedds")
MIN_RUNS = 3
FASTDDS_SLACK = 1.05      # PRIMARY (b): head <= 1.05 x fastdds
LAT_REL_FLOOR = 0.001     # NO HARM latency: 0.1% of pre's mean
BE_DELIVERY_POINTS = 1.0  # NO HARM BEST_EFFORT delivered %
RX_RE = re.compile(r"Node \d+ rx: (.*)$", re.M)
DIRECT_RE = re.compile(r"\bdirect_runs=(\d+)")
OOO_RE = re.compile(r"Subscriber \d+ delivery: .*?\bout_of_order=(\d+)")


def fin(xs):
    return [x for x in xs if isinstance(x, (int, float)) and not math.isnan(x)]


def mean_or_nan(xs):
    return statistics.mean(xs) if xs else float("nan")


def welch(a, b):
    d = statistics.mean(a) - statistics.mean(b)
    se = math.sqrt(statistics.variance(a) / len(a) + statistics.variance(b) / len(b))
    return d, se


def rx_treatment(text):
    """(state, value) from one subscriber log: ("no_line", None) when no "Node <n> rx:" line; ("no_field", the sum of
    the values that are there, 0 if none) when a line lacks direct_runs=; else ("ok", the sum over the lines). A pre
    line without the field is A's own line (treatment_problem)."""
    lines = RX_RE.findall(text)
    if not lines:
        return "no_line", None
    vals = [DIRECT_RE.search(x) for x in lines]
    if any(v is None for v in vals):
        return "no_field", sum(int(v.group(1)) for v in vals if v is not None)
    return "ok", sum(int(v.group(1)) for v in vals)


def out_of_order(text):
    """Sum of out_of_order over rmw_tickle's "Subscriber <id> delivery:" lines, or None when there are none."""
    vals = OOO_RE.findall(text)
    return sum(int(v) for v in vals) if vals else None


def treatment_problem(arm, state, value):
    """'' when the run's treatment is as required, else why not (the text starts with the VOID class)."""
    if state == "no_line":
        return "TREATMENT NOT READ: no 'Node <n> rx:' line in the subscriber log"
    if arm == HEAD:
        if state == "no_field":
            return "TREATMENT NOT READ: a 'Node <n> rx:' line without direct_runs="
        if value <= 0:
            return "TREATMENT NOT APPLIED: direct_runs=0"
        return ""
    if value:  # pre: absent field or 0 is A
        return f"TREATMENT: pre carries direct_runs={value} (A is not the parent)"
    return ""


def compare(head, pre):
    """(d, se) of mean(head) - mean(pre), or None with fewer than MIN_RUNS finite values either side."""
    h, p = fin(head), fin(pre)
    if len(h) < MIN_RUNS or len(p) < MIN_RUNS:
        return None
    return welch(h, p)


def judge_cell(name, reliable, arms, out=print):
    """Read one cell. arms: arm -> usable run records (largemsg_rig_summary.run_record() plus 'rx_state', 'direct',
    'ooo'). Returns dict(void=[...], primary=MET|MISSED|NOT READ per sub, harm={metric: WORSE|HELD|NOT READ},
    treatment_bad=[...], incomplete_bad=[...])."""
    head, pre = arms.get(HEAD, []), arms.get(PRE, [])
    res = dict(name=name, void=[], treatment_bad=[], incomplete_bad=[], a=None, b=None, harm={})
    out(f"=== {name}: {'RELIABLE' if reliable else 'BEST_EFFORT'}; usable runs: "
        + ", ".join(f"{a} {len(arms.get(a, []))}" for a in (HEAD, PRE) + VENDORS) + " ===")
    # ---- VOID ----------------------------------------------------------------------------------------------------
    if len(head) < MIN_RUNS or len(pre) < MIN_RUNS:
        res["void"].append(f"fewer than {MIN_RUNS} usable runs (head {len(head)}, pre {len(pre)})")
    for r in head + pre:
        tag = f"{r['arm']} r{r['rep']}"
        why = treatment_problem(r["arm"], r.get("rx_state", "no_line"), r.get("direct"))
        out(f"  treatment {tag}: rx line {r.get('rx_state', 'no_line')}, direct_runs={r.get('direct')}"
            + (f" -> {why}" if why else " -> ok"))
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
    ooo_known = [r for r in head + pre if r.get("ooo") is not None]
    out(f"  out_of_order: {'read from ' + str(len(ooo_known)) + ' subscriber logs' if ooo_known else 'not reported'}"
        "; torn: not reported (no such counter; perf_test and the summary have none)")
    # ---- context, always printed --------------------------------------------------------------------------------
    for metric in ("cpu_sub", "cpu_pub", "lat_med", "p99_lb", "delivered_pct"):
        line = [f"  {metric:13s}"]
        for a in (HEAD, PRE) + VENDORS:
            xs = fin(r.get(metric) for r in arms.get(a, []))
            line.append(f"{a.replace('tickle@', '')} {mean_or_nan(xs):9.3f}")
        spreads = []
        for v in VENDORS:
            xs = fin(r.get(metric) for r in arms.get(v, []))
            spreads.append(f"{v} {max(xs) - min(xs):.3f}" if len(xs) >= 2 else f"{v} -")
        line.append("| vendor spread " + ", ".join(spreads))
        out(" ".join(line))
    h_sub = mean_or_nan(fin(r.get("cpu_sub") for r in head))
    for v in VENDORS:
        parts = []
        for metric in ("cpu_sub", "cpu_pub", "lat_med", "p99_lb"):
            hm = mean_or_nan(fin(r.get(metric) for r in head))
            vm = mean_or_nan(fin(r.get(metric) for r in arms.get(v, []) if r.get("recv", 0) > 0))
            parts.append(f"{metric} {hm - vm:+.3f}")
        out(f"  context head - {v}: " + ", ".join(parts) + " (not deciding)")
    if res["void"]:
        out(f"  VOID {name}: " + " | ".join(res["void"]))
        return res
    # ---- PRIMARY -------------------------------------------------------------------------------------------------
    c = compare([r.get("cpu_sub") for r in head], [r.get("cpu_sub") for r in pre])
    if c is None:
        res["a"] = "NOT READ"
        out("  PRIMARY (a) sub CPU head vs pre: NOT READ (fewer than 3 finite values)")
    else:
        d, se = c
        res["a"] = "MET" if d < -2 * se else "MISSED"
        out(f"  PRIMARY (a) sub CPU head vs pre: d {d:+.4f} ms/MB, floor (2SE) {2 * se:.4f} -> "
            f"{'BETTER' if d < -2 * se else 'not better'}: {res['a']}")
    fd = fin(r.get("cpu_sub") for r in arms.get("fastdds", []) if r.get("recv", 0) > 0)
    if not fd or math.isnan(h_sub):
        res["b"] = "NOT READ"
        out("  PRIMARY (b) sub CPU head <= 1.05 x fastdds: NOT READ (fastdds delivered nothing / no runs, or head has "
            "no value)")
    else:
        fm = statistics.mean(fd)
        res["b"] = "MET" if h_sub <= FASTDDS_SLACK * fm else "MISSED"
        out(f"  PRIMARY (b) sub CPU head {h_sub:.4f} vs 1.05 x fastdds {FASTDDS_SLACK * fm:.4f} "
            f"(fastdds {fm:.4f}, head/fastdds {h_sub / fm:.3f}): {res['b']}")
    # ---- NO HARM -------------------------------------------------------------------------------------------------
    for metric in ("lat_med", "p99_lb", "cpu_pub"):
        c = compare([r.get(metric) for r in head], [r.get(metric) for r in pre])
        if c is None:
            res["harm"][metric] = "NOT READ"
            out(f"  NO HARM {metric}: NOT READ (fewer than 3 finite values)")
            continue
        d, se = c
        thr = 2 * se
        if metric != "cpu_pub":
            thr = max(thr, LAT_REL_FLOOR * abs(statistics.mean(fin(r.get(metric) for r in pre))))
        res["harm"][metric] = "WORSE" if d > thr else "HELD"
        out(f"  NO HARM {metric}: d {d:+.4f}, threshold {thr:.4f} -> {res['harm'][metric]}")
    if not reliable:
        hd, pd = fin(r.get("delivered_pct") for r in head), fin(r.get("delivered_pct") for r in pre)
        if len(hd) < MIN_RUNS or len(pd) < MIN_RUNS:
            res["harm"]["delivered_pct"] = "NOT READ"
            out("  NO HARM delivered_pct: NOT READ")
        else:
            drop = statistics.mean(pd) - statistics.mean(hd)
            res["harm"]["delivered_pct"] = "WORSE" if drop > BE_DELIVERY_POINTS else "HELD"
            out(f"  NO HARM delivered_pct: head {statistics.mean(hd):.2f}% pre {statistics.mean(pd):.2f}% "
                f"(drop {drop:+.2f} points, allowed {BE_DELIVERY_POINTS}) -> {res['harm']['delivered_pct']}")
    return res


def verdict(results, problems, out=print):
    """(text, exit status) over the cells' results; problems: input-level reasons (missing / duplicate cells)."""
    fail, unread, voids = [], list(problems), []
    for name in CELL_ORDER:
        r = results.get(name)
        if r is None:
            continue
        if r["void"]:
            voids.append(f"{name} VOID")
            continue
        for sub in ("a", "b"):
            if r[sub] == "MISSED":
                fail.append(f"{name} PRIMARY ({sub}) MISSED")
            elif r[sub] != "MET":
                unread.append(f"{name} PRIMARY ({sub}) NOT READ")
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
        text, rc = "PASS (" + "; ".join([f"{', '.join(read)} met (a) and (b), nothing WORSE"] + voids) + ")", 0
    out(f"RX GAP VERDICT: {text}")
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
        if r["arm"].startswith("tickle@"):
            text = (runs / f"{stem}_sub.log").read_text(errors="replace") if (runs / f"{stem}_sub.log").exists() else ""
            r["rx_state"], r["direct"] = rx_treatment(text)
            r["ooo"] = out_of_order(text)
        arms.setdefault(r["arm"], []).append(r)
    return name, reliable, arms


def preflight_check(results):
    bad = []
    for name, r in results.items():
        if r.get("_head_n", 0) == 0 or r.get("_pre_n", 0) == 0:
            bad.append(f"{name}: no usable head or pre run (head {r.get('_head_n', 0)}, pre {r.get('_pre_n', 0)})")
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
        res["_head_n"], res["_pre_n"] = len(arms.get(HEAD, [])), len(arms.get(PRE, []))
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
        print(f"PREFLIGHT OK: {len(results)} cell(s) read end to end, treatment present on every head and pre run")
        return 0
    return rc


# ---- selftest --------------------------------------------------------------------------------------------------------
def _rec(arm, rep, cpu_sub, cpu_pub=1.0, lat=10.0, p99=12.0, dpct=100.0, direct=None, rx="ok", ooo=0, recv=900):
    if direct is None:
        direct = 500 if arm == HEAD else 0
    return dict(arm=arm, rep=rep, cpu_sub=cpu_sub, cpu_pub=cpu_pub, lat_med=lat, p99_lb=p99, delivered_pct=dpct,
                recv=recv, lost=0 if dpct >= 100 else 5, incomplete=dpct < 100 or recv == 0, rx_state=rx,
                direct=direct if arm.startswith("tickle@") else None, ooo=ooo if arm.startswith("tickle@") else None)


def _cell(head_sub=(2.20, 2.21, 2.19, 2.22, 2.20), pre_sub=(2.60, 2.61, 2.59, 2.62, 2.60),
          fd_sub=(2.20, 2.25, 2.18), **kw):
    """A cell where head is BETTER than pre and within 5% of fastdds; kw overrides per arm via callables."""
    arms = {HEAD: [_rec(HEAD, i + 1, x) for i, x in enumerate(head_sub)],
            PRE: [_rec(PRE, i + 1, x) for i, x in enumerate(pre_sub)],
            "fastdds": [_rec("fastdds", i + 1, x) for i, x in enumerate(fd_sub)],
            "cyclonedds": [_rec("cyclonedds", i + 1, x) for i, x in enumerate((2.5, 2.6, 2.55))]}
    for arm, fn in kw.items():
        arms[arm.replace("_", "@") if arm.startswith("tickle") else arm] = fn(arms)
    return arms


def _run(cells, problems=()):
    quiet = []
    results = {n: judge_cell(n, n.endswith("_rel"), a, out=quiet.append) for n, a in cells.items()}
    text, rc = verdict(results, list(problems) + [f"{n} not in the input" for n in CELL_ORDER if n not in cells],
                       out=quiet.append)
    return text, rc, results, "\n".join(quiet)


def selftest():
    import shutil
    import tempfile
    fails = []

    def check(what, ok):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}")
        if not ok:
            fails.append(what)

    def four(**over):
        return {n: over.get(n, _cell()) for n in CELL_ORDER}

    print("parsers")
    check("rx line with direct_runs", rx_treatment("x\n[I] Node 2 rx: socket_drops=0 direct_runs=17 drain=1\n")
          == ("ok", 17))
    check("rx line without direct_runs", rx_treatment("Node 2 rx: socket_drops=0 drain=1\n")[0] == "no_field")
    check("no rx line at all", rx_treatment("Node 2 traffic: tx_datagrams=1\n") == ("no_line", None))
    check("two rx lines summed", rx_treatment("Node 2 rx: direct_runs=3\nNode 4 rx: direct_runs=4\n") == ("ok", 7))
    check("out_of_order summed", out_of_order("Subscriber 1 delivery: delivered=9 out_of_order=2 x=1\n"
                                              "Subscriber 7 delivery: delivered=0 out_of_order=0\n") == 2)
    check("pre without the field is ok/0 treatment", treatment_problem(PRE, "no_field", None) == "")

    print("case PASS: four cells, head better than pre and within 5% of fastdds, nothing worse")
    text, rc, _r, _o = _run(four())
    check(f"PASS exit 0 ({text[:60]})", rc == 0 and text.startswith("PASS"))

    print("case FAIL (a): head equal to pre")
    eq = _cell(head_sub=(2.60, 2.61, 2.59, 2.62, 2.60))
    text, rc, _r, _o = _run(four(s1_1m_rel=eq))
    check(f"FAIL exit 1 naming (a) ({text[:70]})", rc == 1 and "s1_1m_rel PRIMARY (a) MISSED" in text)

    print("case FAIL (b): head better than pre but 10% above fastdds")
    hi = _cell(fd_sub=(2.00, 2.01, 1.99))
    text, rc, res, _o = _run(four(s1_4m_be=hi))
    check("(a) MET and (b) MISSED both printed", res["s1_4m_be"]["a"] == "MET" and res["s1_4m_be"]["b"] == "MISSED")
    check("FAIL exit 1", rc == 1 and "s1_4m_be PRIMARY (b) MISSED" in text)

    print("case NO HARM: latency median worse by 0.5% (beyond 0.1% and 2 SE)")
    lat = _cell(tickle_head=lambda a: [dict(r, lat_med=10.05 + 0.001 * r["rep"]) for r in a[HEAD]])
    text, rc, _r, _o = _run(four(s1_1m_be=lat))
    check(f"NO HARM lat_med WORSE -> FAIL ({text[:70]})", rc == 1 and "s1_1m_be NO HARM lat_med WORSE" in text)

    print("control: latency worse by 0.05% (inside the 0.1% floor) is HELD")
    lat2 = _cell(tickle_head=lambda a: [dict(r, lat_med=10.005) for r in a[HEAD]])
    text, rc, res, _o = _run(four(s1_1m_be=lat2))
    check("HELD, PASS", res["s1_1m_be"]["harm"]["lat_med"] == "HELD" and rc == 0)

    print("case NO HARM: pub CPU worse beyond 2 SE")
    pub = _cell(tickle_head=lambda a: [dict(r, cpu_pub=1.2 + 0.001 * r["rep"]) for r in a[HEAD]])
    text, rc, _r, _o = _run(four(s1_4m_rel=pub))
    check("cpu_pub WORSE -> FAIL", rc == 1 and "s1_4m_rel NO HARM cpu_pub WORSE" in text)

    print("case NO HARM: BEST_EFFORT delivered % lower by 2 points")
    be = _cell(tickle_head=lambda a: [dict(r, delivered_pct=98.0) for r in a[HEAD]])
    text, rc, _r, _o = _run(four(s1_4m_be=be))
    check("delivered_pct WORSE -> FAIL", rc == 1 and "s1_4m_be NO HARM delivered_pct WORSE" in text)
    be2 = _cell(tickle_head=lambda a: [dict(r, delivered_pct=99.5) for r in a[HEAD]])
    _t, rc, _r, _o = _run(four(s1_4m_be=be2))
    check("control: 0.5 points lower is HELD -> PASS", rc == 0)

    print("case VOID: treatment not read on head (no rx line) in every cell")
    nr = _cell(tickle_head=lambda a: [dict(r, rx_state="no_line", direct=None) for r in a[HEAD]])
    text, rc, res, out = _run({n: nr for n in CELL_ORDER})
    check("every cell VOID with TREATMENT NOT READ", all(r["void"] for r in res.values())
          and "TREATMENT NOT READ" in out)
    check(f"NO VERDICT exit 2, never PASS ({text[:60]})", rc == 2 and text.startswith("NO VERDICT"))

    print("case VOID: head line present without direct_runs= in one run; the other three cells pass")
    nf = _cell(tickle_head=lambda a: [dict(r, rx_state="no_field") if r["rep"] == 2 else r for r in a[HEAD]])
    text, rc, res, _o = _run(four(s1_1m_rel=nf))
    check("s1_1m_rel VOID", bool(res["s1_1m_rel"]["void"]))
    check("PASS over the non-VOID cells, the VOID one listed", rc == 0 and "s1_1m_rel VOID" in text)

    print("case VOID: head direct_runs=0 (not applied); pre carrying direct_runs > 0")
    na = _cell(tickle_head=lambda a: [dict(r, direct=0) for r in a[HEAD]])
    _t, _rc, res, out = _run(four(s1_1m_rel=na))
    check("NOT APPLIED voids", bool(res["s1_1m_rel"]["void"]) and "TREATMENT NOT APPLIED" in out)
    pa = _cell(tickle_pre=lambda a: [dict(r, direct=9) for r in a[PRE]])
    _t, _rc, res, out = _run(four(s1_1m_rel=pa))
    check("pre not A voids", bool(res["s1_1m_rel"]["void"]) and "A is not the parent" in out)

    print("case VOID: fewer than 3 runs; RELIABLE incomplete; out_of_order")
    few = _cell(head_sub=(2.2, 2.21))
    _t, _rc, res, _o = _run(four(s1_4m_rel=few))
    check("2 head runs -> VOID", "fewer than 3" in " ".join(res["s1_4m_rel"]["void"]))
    inc = _cell(tickle_pre=lambda a: [dict(r, incomplete=True, lost=3) if r["rep"] == 1 else r for r in a[PRE]])
    _t, _rc, res, _o = _run(four(s1_4m_rel=inc))
    check("RELIABLE pre run incomplete -> VOID", "delivered less" in " ".join(res["s1_4m_rel"]["void"]))
    _t, _rc, res, _o = _run(four(s1_4m_be=inc))
    check("control: the same incompleteness in a BEST_EFFORT cell does not void", not res["s1_4m_be"]["void"])
    oo = _cell(tickle_head=lambda a: [dict(r, ooo=1) if r["rep"] == 3 else r for r in a[HEAD]])
    _t, _rc, res, _o = _run(four(s1_1m_be=oo))
    check("out_of_order=1 -> VOID", "out_of_order=1" in " ".join(res["s1_1m_be"]["void"]))

    print("case NOT READ: fastdds delivered nothing in one cell")
    nofd = _cell(fastdds=lambda a: [dict(r, recv=0, cpu_sub=float("nan")) for r in a["fastdds"]])
    text, rc, res, _o = _run(four(s1_4m_be=nofd))
    check("(b) NOT READ, (a) MET", res["s1_4m_be"]["b"] == "NOT READ" and res["s1_4m_be"]["a"] == "MET")
    check("NO VERDICT, not PASS", rc == 2 and "s1_4m_be PRIMARY (b) NOT READ" in text)

    print("case NO VERDICT: a cell missing from the input")
    cells = four()
    del cells["s1_4m_rel"]
    text, rc, _r, _o = _run(cells)
    check("missing cell -> NO VERDICT", rc == 2 and "s1_4m_rel not in the input" in text)
    print("case FAIL dominates NOT READ")
    text, rc, _r, _o = _run(dict(cells, s1_1m_rel=eq))
    check("FAIL exit 1 with the missing cell listed", rc == 1 and "not in the input" in text)

    print("case end to end through largemsg_rig_summary.load (fixture s3b: Array4m 15 Hz RELIABLE)")
    fx = Path(__file__).resolve().parent / "largemsg_rig_fixture" / "s3b.runs"
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp) / "s1_4m_rel.runs"
        shutil.copytree(fx, t)
        index = (t / "index.txt").read_text().splitlines()
        new = list(index)
        # pre = head's runs under another name; a third rep of each from r1; fastdds = cyclonedds's runs renamed with
        # the library it would map.
        for line in index:
            m = re.match(r"RUN (\S+)_Array4m_l0_r(\d) ", line)
            arm, rep = m.group(1), m.group(2)
            jobs = []
            if arm == HEAD:
                jobs += [(PRE, rep)] + ([(HEAD, "3"), (PRE, "3")] if rep == "1" else [])
            if arm == "cyclonedds":
                jobs += [("fastdds", rep)]
            for to_arm, to_rep in jobs:
                stem_from, stem_to = f"{arm}_Array4m_l0_r{rep}", f"{to_arm}_Array4m_l0_r{to_rep}"
                for f in t.glob(f"{stem_from}_*"):
                    shutil.copy(f, t / f.name.replace(stem_from, stem_to))
                ln = line.replace(stem_from, stem_to, 1)
                if to_arm == "fastdds":
                    ln = ln.replace("cyclonedds", "fastrtps")
                new.append(ln)
        (t / "index.txt").write_text("\n".join(new) + "\n")
        for f in t.glob("tickle@*_sub.log"):
            val = 120 if f.name.startswith(HEAD) else None
            extra = "\n[I] Node 2 rx: socket_drops=0 well_known_drops=0 data_drops=0 drain=1" + \
                (f" direct_runs={val}" if val is not None else "") + "\n"
            f.write_text(f.read_text(errors="replace") + extra)
        out_base = str(t)[:-len(".runs")]
        name, reliable, arms = read_out(out_base)
        check(f"named {name}, RELIABLE {reliable}, 3 head + 3 pre runs",
              name == "s1_4m_rel" and reliable and len(arms.get(HEAD, [])) == 3 and len(arms.get(PRE, [])) == 3)
        check("head direct_runs read from the log, pre's line has none",
              all(r["direct"] == 120 for r in arms[HEAD]) and all(r["direct"] == 0 for r in arms[PRE]))
        check("out_of_order read from rmw_tickle's delivery lines", all(r["ooo"] == 0 for r in arms[HEAD]))
        log = []
        res = judge_cell(name, reliable, arms, out=log.append)
        check("identical head and pre: not VOID, (a) MISSED (a FAIL path through real files)",
              not res["void"] and res["a"] == "MISSED")
        rc = main([out_base, "--preflight"])
        check("--preflight on it: PREFLIGHT OK (exit 0)", rc == 0)
        lf = t / "tickle@head_Array4m_l0_r2_sub.log"
        lf.write_text(lf.read_text().replace(" direct_runs=120", ""))
        rc = main([out_base, "--preflight"])
        check("--preflight with one head line lacking direct_runs=: PREFLIGHT FAILED (exit 1)", rc == 1)
        rc = main([out_base])
        check("and the reading: NO VERDICT (exit 2)", rc == 2)
    print(f"SELFTEST {'FAILED: ' + '; '.join(fails) if fails else 'OK'}")
    return 1 if fails else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    sys.exit(main(sys.argv[1:]))
