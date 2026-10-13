#!/usr/bin/env python3
"""rx_gap2_read.py - the rig A/B of ab/rx-gap2 against origin/main 0c9f9a3b, read by rules written 2026-10-13 before
any run (~/rig_queue_rx_gap2.sh). Nothing here is judged by hand: every rule below is implemented in this file.
Derived from rx_gap_read.py (the ab/rx-gap A/B); what differs is the treatment, PRIMARY (b) and the page faults.

WHAT IS TESTED. B (the tip pushed as ab/rx-gap2) is A (0c9f9a3b, origin/main) plus five changes to rmw_tickle's
receive CPU per MB on large samples (~/claude_reports/rx_perf_gap2_2026-10-13.md: +0.26 / +0.28 ms/MB behind Fast DDS):
  (1) a run read in place with receive offload on too, and the empty read cut after ANY run read;
  (n) both sockets O_NONBLOCK, so the wait no longer checksums each datagram before the read (udp_poll);
  (2) the wait's deadline a timerfd moved only earlier: a wait that ends for data starts no hrtimer;
  (3) at 0% loss n at n_max from the reader's first clean sample: no slow start, "none seen" announced at once;
  (4) the copy target: page faults measured (PGFAULTS=1) - none expected; reported per arm, never deciding.

INPUT. The four stage-1 invocations of rmw_keepall_rig.sh (OUT.runs/ each), cross-host at 0% loss, arms tickle@head
(B), tickle@pre (A), fastdds, cyclonedds, PGFAULTS=1, read through largemsg_rig_summary.load():
  s1_1m_rel  Array1m 30 Hz RELIABLE KEEP_LAST 10        s1_1m_be  Array1m 30 Hz BEST_EFFORT KEEP_LAST 5
  s1_4m_rel  Array4m 15 Hz RELIABLE KEEP_LAST 10        s1_4m_be  Array4m 15 Hz BEST_EFFORT KEEP_LAST 5

RULES
  VOID (the cell is not read), any of:
    - fewer than 3 usable runs of head or of pre;
    - a RELIABLE cell where a head or pre run did not deliver every sample;
    - out_of_order > 0 on a head or pre subscriber's rmw_tickle delivery lines (summed);
    - TREATMENT, head (every run; each arm reports its treatment):
        the subscriber's "Node <n> rx:" line carries direct_reads=, empty_reads=, nonblock= and timer_waits= - a line
        or field missing is "TREATMENT NOT READ";
        direct_reads > 0 (runs read in place, at 1 MB too), nonblock=1, timer_waits > 0, and empty_reads per received
        sample at most half the runs a sample (8 at 1 MB, 32 at 4 MB: A made about one per run) - else "TREATMENT NOT
        APPLIED: <which>";
        the publisher's "Node <n> hybrid:" line carries n_hist=, and at most 2 samples of the run went below n_max
        (buckets 1..6 of 7; A sent ~77-150 a run there, the ramp) - absent: NOT READ; more: NOT APPLIED.
      pre (every run): the "Node <n> rx:" line present and WITHOUT empty_reads= (A's line); carrying it means pre is
      not A.
  FLOOR per cell and metric: 2 x SE of the head - pre difference of means (Welch).
  PRIMARY, sub CPU ms per MB (cpu_sub):
    (a) per cell: head BETTER than pre, d = mean(head) - mean(pre) < -FLOOR. Every non-VOID cell must meet it.
    (b) over the cells: head within 5% of Fast DDS - mean(head) <= 1.05 x mean(fastdds over its runs that received
        something) - in AT LEAST TWO of the four cells. MET in 2+: met; MET plus NOT READ below 2: MISSED; else NOT
        READ.
  NO HARM per cell, head against pre: latency median and p99 lower bound WORSE iff d > max(FLOOR, 0.1% of pre's mean);
    pub CPU per MB WORSE iff d > FLOOR; BEST_EFFORT delivered % WORSE iff it fell by more than 1 point.
  REPORTED, never deciding: each arm's subscriber page faults per MB over the steady window (from <stem>_sub.pgf: the
    first sample at least 10 s after the subscriber started to the last one; MB = rate x size x seconds), and head
    against fastdds and cyclonedds.
  VERDICT, last line "RX GAP2 VERDICT: PASS|FAIL|NO VERDICT (reasons)": FAIL on any (a) MISSED, (b) MISSED or NO
    HARM WORSE; NO VERDICT when a cell is missing, none is read, or anything deciding is NOT READ; else PASS.
    Exit 0 / 1 / 2.
  --preflight (PC, REPS=1): the reading runs end to end; PREFLIGHT FAILED when a cell has no usable head or pre run,
    a head or pre treatment is NOT READ / NOT APPLIED / pre not A, or a RELIABLE head or pre run was incomplete. Last
    line "PREFLIGHT OK" (exit 0) or "PREFLIGHT FAILED: ..." (exit 1).

Usage: rx_gap2_read.py OUT [OUT ...] [--preflight]      rx_gap2_read.py --selftest
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
FASTDDS_CELLS = 2         # PRIMARY (b): in at least this many cells
EMPTY_PER_SAMPLE = {"Array1m": 8.0, "Array4m": 32.0}  # TREATMENT: empty reads per received sample, at most
BELOW_N_MAX = 2           # TREATMENT: samples of a run sent below n_max, at most
SAMPLE_MB = {"Array1m": 1048576 / 1e6, "Array4m": 4194304 / 1e6}
PGF_SKIP_S = 10.0         # the page faults' steady window starts this long after the subscriber
LAT_REL_FLOOR = 0.001     # NO HARM latency: 0.1% of pre's mean
BE_DELIVERY_POINTS = 1.0  # NO HARM BEST_EFFORT delivered %
RX_RE = re.compile(r"Node \d+ rx: (.*)$", re.M)
RX_FIELDS = ("direct_reads", "empty_reads", "nonblock", "timer_waits")
NHIST_RE = re.compile(r"Node \d+ hybrid: .*?\bn_hist=([\d/]+)")
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
    """(state, fields) from one subscriber log: ("no_line", {}) when no "Node <n> rx:" line; else ("ok", {field: the
    sum over the lines}) for each of RX_FIELDS a line carries (a field some line lacks is left out)."""
    lines = RX_RE.findall(text)
    if not lines:
        return "no_line", {}
    fields = {}
    for name in RX_FIELDS:
        vals = [re.search(rf"\b{name}=(\d+)", x) for x in lines]
        if all(v is not None for v in vals):
            fields[name] = sum(int(v.group(1)) for v in vals)
    return "ok", fields


def below_n_max(text):
    """Samples the publisher sent below n_max (n_hist's buckets but the last), summed over its hybrid lines; None when
    it printed none."""
    vals = NHIST_RE.findall(text)
    if not vals:
        return None
    return sum(sum(int(x) for x in v.split("/")[:-1]) for v in vals)


def pgf_per_mb(path, topic, rate):
    """Page faults (minor + major) per MB over the steady window of one .pgf file, or nan."""
    try:
        rows = [tuple(int(x) for x in line.split()) for line in Path(path).read_text().splitlines() if line.strip()]
    except (OSError, ValueError):
        return float("nan")
    rows = [r for r in rows if len(r) == 3]
    if len(rows) < 3 or topic not in SAMPLE_MB or rate <= 0:
        return float("nan")
    t0 = rows[0][0] + PGF_SKIP_S * 1e9
    window = [r for r in rows if r[0] >= t0]
    if len(window) < 2:
        return float("nan")
    seconds = (window[-1][0] - window[0][0]) / 1e9
    faults = (window[-1][1] + window[-1][2]) - (window[0][1] + window[0][2])
    mb = rate * SAMPLE_MB[topic] * seconds
    return faults / mb if mb > 0 else float("nan")


def out_of_order(text):
    """Sum of out_of_order over rmw_tickle's "Subscriber <id> delivery:" lines, or None when there are none."""
    vals = OOO_RE.findall(text)
    return sum(int(v) for v in vals) if vals else None


def treatment_problem(arm, state, fields, recv=0, below=None, topic="Array1m"):
    """'' when the run's treatment is as required, else why not (the text starts with the VOID class)."""
    if state == "no_line":
        return "TREATMENT NOT READ: no 'Node <n> rx:' line in the subscriber log"
    if arm != HEAD:
        if "empty_reads" in fields:
            return "TREATMENT: pre carries empty_reads= (A is not the parent)"
        return ""
    missing = [f for f in RX_FIELDS if f not in fields]
    if missing:
        return "TREATMENT NOT READ: the rx line lacks " + ", ".join(f + "=" for f in missing)
    if below is None:
        return "TREATMENT NOT READ: no n_hist= on the publisher's hybrid line"
    bad = []
    if fields["direct_reads"] <= 0:
        bad.append("direct_reads=0")
    if fields["nonblock"] != 1:
        bad.append(f"nonblock={fields['nonblock']}")
    if fields["timer_waits"] <= 0:
        bad.append("timer_waits=0")
    limit = EMPTY_PER_SAMPLE.get(topic, 8.0)
    per = fields["empty_reads"] / recv if recv else float("inf")
    if per > limit:
        bad.append(f"empty_reads {per:.1f} a sample > {limit:g}")
    if below > BELOW_N_MAX:
        bad.append(f"{below} samples below n_max > {BELOW_N_MAX}")
    return ("TREATMENT NOT APPLIED: " + ", ".join(bad)) if bad else ""


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
        why = treatment_problem(r["arm"], r.get("rx_state", "no_line"), r.get("rx_fields", {}), r.get("recv", 0),
                                r.get("below"), r.get("topic", "Array1m"))
        out(f"  treatment {tag}: rx line {r.get('rx_state', 'no_line')} {r.get('rx_fields', {})}, "
            f"below n_max {r.get('below')}" + (f" -> {why}" if why else " -> ok"))
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
    for metric in ("cpu_sub", "cpu_pub", "lat_med", "p99_lb", "delivered_pct", "pgf_per_mb"):
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
            f"(fastdds {fm:.4f}, head/fastdds {h_sub / fm:.3f}): {res['b']} (needed in {FASTDDS_CELLS} of 4 cells)")
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
        if r["a"] == "MISSED":
            fail.append(f"{name} PRIMARY (a) MISSED")
        elif r["a"] != "MET":
            unread.append(f"{name} PRIMARY (a) NOT READ")
        for metric, v in r["harm"].items():
            if v == "WORSE":
                fail.append(f"{name} NO HARM {metric} WORSE")
            elif v != "HELD":
                unread.append(f"{name} NO HARM {metric} NOT READ")
    read = [n for n in CELL_ORDER if n in results and not results[n]["void"]]
    if not read:
        unread.append("no cell was read")
    b_met = [n for n in read if results[n]["b"] == "MET"]
    b_open = [n for n in read if results[n]["b"] not in ("MET", "MISSED")] + [n for n in CELL_ORDER if n not in read]
    if len(b_met) >= FASTDDS_CELLS:
        pass
    elif len(b_met) + len(b_open) < FASTDDS_CELLS:
        fail.append(f"PRIMARY (b) MISSED: within 5% of Fast DDS in {len(b_met)} cell(s) "
                    f"({', '.join(b_met) or 'none'}), {FASTDDS_CELLS} needed")
    else:
        unread.append(f"PRIMARY (b) NOT READ: met in {len(b_met)}, {len(b_open)} cell(s) not read")
    if fail:
        text, rc = "FAIL (" + "; ".join(fail + unread + voids) + ")", 1
    elif unread:
        text, rc = "NO VERDICT (" + "; ".join(unread + voids) + ")", 2
    else:
        text, rc = "PASS (" + "; ".join([f"{', '.join(read)} met (a), (b) in {', '.join(b_met)}, nothing WORSE"]
                                        + voids) + ")", 0
    out(f"RX GAP2 VERDICT: {text}")
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
        r["topic"] = r["topic"] if "topic" in r else ""
        r["pgf_per_mb"] = pgf_per_mb(runs / f"{stem}_sub.pgf", r["topic"], rate)
        if r["arm"].startswith("tickle@"):
            text = (runs / f"{stem}_sub.log").read_text(errors="replace") if (runs / f"{stem}_sub.log").exists() else ""
            r["rx_state"], r["rx_fields"] = rx_treatment(text)
            r["ooo"] = out_of_order(text)
            pub = (runs / f"{stem}_pub.log").read_text(errors="replace") if (runs / f"{stem}_pub.log").exists() else ""
            r["below"] = below_n_max(pub)
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
HEAD_FIELDS = {"direct_reads": 5000, "empty_reads": 300, "nonblock": 1, "timer_waits": 9000}


def _rec(arm, rep, cpu_sub, cpu_pub=1.0, lat=10.0, p99=12.0, dpct=100.0, rx="ok", ooo=0, recv=900, fields=None,
         below=None):
    tickle = arm.startswith("tickle@")
    if fields is None:
        fields = dict(HEAD_FIELDS) if arm == HEAD else {"direct_reads": 4000}
    if below is None:
        below = 1 if arm == HEAD else 77
    return dict(arm=arm, rep=rep, cpu_sub=cpu_sub, cpu_pub=cpu_pub, lat_med=lat, p99_lb=p99, delivered_pct=dpct,
                recv=recv, lost=0 if dpct >= 100 else 5, incomplete=dpct < 100 or recv == 0, topic="Array1m",
                pgf_per_mb=0.01, rx_state=rx if tickle else None, rx_fields=fields if tickle else {},
                below=below if tickle else None, ooo=ooo if tickle else None)


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


def _head(change):
    return lambda a: [change(dict(r)) for r in a[HEAD]]


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
    line = ("[I] Node 2 rx: socket_drops=0 direct=1 direct_reads=17 direct_runs=16 nonblock=1 empty_reads=4 gro_on=0 "
            "timer_waits=99 timer_sets=3 timer_fires=2\n")
    check("rx line fields", rx_treatment("x\n" + line) == ("ok", {"direct_reads": 17, "empty_reads": 4,
                                                                    "nonblock": 1, "timer_waits": 99}))
    check("A's rx line: no empty_reads=", "empty_reads" not in rx_treatment("Node 2 rx: direct_reads=5 drain=1\n")[1])
    check("no rx line at all", rx_treatment("Node 2 traffic: tx_datagrams=1\n") == ("no_line", {}))
    check("n_hist below n_max", below_n_max("Node 3 hybrid: runs=9 n_hist=1/16/15/15/15/15/525 cap=2\n") == 77)
    check("no hybrid line", below_n_max("nothing\n") is None)
    with tempfile.TemporaryDirectory() as tmp:
        f = Path(tmp) / "x.pgf"
        # 30 s at 0.5 s steps; 2 faults a second after 10 s: Array1m 30 Hz is 31.46 MB/s -> 0.0636 faults/MB.
        f.write_text("".join(f"{int(i * 0.5e9)} {1000 + (0 if i < 20 else 2 * (i - 20) // 2)} 0\n" for i in range(60)))
        v = pgf_per_mb(f, "Array1m", 30)
        check(f"page faults per MB over the steady window ({v:.4f})", abs(v - 2 / 31.457) < 0.01)
        check("no file: nan", math.isnan(pgf_per_mb(Path(tmp) / "none.pgf", "Array1m", 30)))
    check("pre with A's line is ok", treatment_problem(PRE, "ok", {"direct_reads": 3}) == "")
    check("out_of_order summed", out_of_order("Subscriber 1 delivery: delivered=9 out_of_order=2 x=1\n"
                                              "Subscriber 7 delivery: delivered=0 out_of_order=0\n") == 2)

    print("case PASS: four cells, head better than pre and within 5% of fastdds, nothing worse")
    text, rc, _r, _o = _run(four())
    check(f"PASS exit 0 ({text[:60]})", rc == 0 and text.startswith("PASS"))

    print("case FAIL (a): head equal to pre in one cell")
    eq = _cell(head_sub=(2.60, 2.61, 2.59, 2.62, 2.60))
    text, rc, _r, _o = _run(four(s1_1m_rel=eq))
    check(f"FAIL exit 1 naming (a) ({text[:70]})", rc == 1 and "s1_1m_rel PRIMARY (a) MISSED" in text)

    print("case (b): 10% above fastdds in two cells still PASSes (two met); in three cells FAILs")
    hi = _cell(fd_sub=(2.00, 2.01, 1.99))
    text, rc, res, _o = _run(four(s1_4m_be=hi, s1_4m_rel=hi))
    check("two MISSED cells, two MET: PASS", rc == 0 and res["s1_4m_be"]["b"] == "MISSED")
    text, rc, _r, _o = _run(four(s1_4m_be=hi, s1_4m_rel=hi, s1_1m_be=hi))
    check(f"three MISSED: FAIL (b) ({text[:70]})", rc == 1 and "PRIMARY (b) MISSED" in text)

    print("case (b) NOT READ: fastdds delivered nothing in two cells, met in one, missed in one")
    nofd = _cell(fastdds=lambda a: [dict(r, recv=0, cpu_sub=float("nan")) for r in a["fastdds"]])
    text, rc, res, _o = _run(four(s1_4m_be=nofd, s1_4m_rel=nofd, s1_1m_be=hi))
    check("NO VERDICT, not PASS", rc == 2 and "PRIMARY (b) NOT READ" in text)
    text, rc, res, _o = _run(four(s1_4m_be=nofd))
    check("one cell NOT READ, three met: PASS", rc == 0)

    print("case NO HARM: latency median worse by 0.5%; control 0.05%")
    lat = _cell(tickle_head=lambda a: [dict(r, lat_med=10.05 + 0.001 * r["rep"]) for r in a[HEAD]])
    text, rc, _r, _o = _run(four(s1_1m_be=lat))
    check(f"NO HARM lat_med WORSE -> FAIL ({text[:70]})", rc == 1 and "s1_1m_be NO HARM lat_med WORSE" in text)
    lat2 = _cell(tickle_head=lambda a: [dict(r, lat_med=10.005) for r in a[HEAD]])
    text, rc, res, _o = _run(four(s1_1m_be=lat2))
    check("HELD, PASS", res["s1_1m_be"]["harm"]["lat_med"] == "HELD" and rc == 0)

    print("case NO HARM: pub CPU worse; BEST_EFFORT delivered % lower by 2 points (control 0.5)")
    pub = _cell(tickle_head=lambda a: [dict(r, cpu_pub=1.2 + 0.001 * r["rep"]) for r in a[HEAD]])
    text, rc, _r, _o = _run(four(s1_4m_rel=pub))
    check("cpu_pub WORSE -> FAIL", rc == 1 and "s1_4m_rel NO HARM cpu_pub WORSE" in text)
    be = _cell(tickle_head=lambda a: [dict(r, delivered_pct=98.0) for r in a[HEAD]])
    text, rc, _r, _o = _run(four(s1_4m_be=be))
    check("delivered_pct WORSE -> FAIL", rc == 1 and "s1_4m_be NO HARM delivered_pct WORSE" in text)
    be2 = _cell(tickle_head=lambda a: [dict(r, delivered_pct=99.5) for r in a[HEAD]])
    _t, rc, _r, _o = _run(four(s1_4m_be=be2))
    check("control: 0.5 points lower is HELD -> PASS", rc == 0)

    print("case VOID: each treatment the head arm must report")
    nr = _cell(tickle_head=_head(lambda r: dict(r, rx_state="no_line", rx_fields={})))
    text, rc, res, out = _run({n: nr for n in CELL_ORDER})
    check("no rx line anywhere: every cell VOID, NOT READ, NO VERDICT (never PASS)",
          all(r["void"] for r in res.values()) and "TREATMENT NOT READ" in out and rc == 2)
    for what, change, word in (
            ("a field missing (timer_waits=)",
             lambda r: dict(r, rx_fields={k: v for k, v in HEAD_FIELDS.items() if k != "timer_waits"}), "NOT READ"),
            ("no n_hist on the publisher", lambda r: dict(r, below=None), "NOT READ"),
            ("direct_reads=0", lambda r: dict(r, rx_fields=dict(HEAD_FIELDS, direct_reads=0)), "NOT APPLIED"),
            ("nonblock=0", lambda r: dict(r, rx_fields=dict(HEAD_FIELDS, nonblock=0)), "NOT APPLIED"),
            ("timer_waits=0", lambda r: dict(r, rx_fields=dict(HEAD_FIELDS, timer_waits=0)), "NOT APPLIED"),
            ("9 empty reads a sample at 1 MB", lambda r: dict(r, rx_fields=dict(HEAD_FIELDS, empty_reads=8100)),
             "NOT APPLIED"),
            ("3 samples below n_max", lambda r: dict(r, below=3), "NOT APPLIED")):
        bad = _cell(tickle_head=lambda a, change=change: [change(dict(r)) if r["rep"] == 2 else r for r in a[HEAD]])
        text, rc, res, out = _run(four(s1_1m_rel=bad))
        check(f"head {what}: s1_1m_rel VOID ({word}), the rest PASS",
              bool(res["s1_1m_rel"]["void"]) and f"TREATMENT {word}" in out and rc == 0 and "s1_1m_rel VOID" in text)
    ok8 = _cell(tickle_head=_head(lambda r: dict(r, rx_fields=dict(HEAD_FIELDS, empty_reads=7200), below=2)))
    _t, rc, res, _o = _run(four(s1_1m_rel=ok8))
    check("control: 8.0 empty reads a sample and 2 below n_max are within the limits", not res["s1_1m_rel"]["void"])
    pa = _cell(tickle_pre=lambda a: [dict(r, rx_fields={"empty_reads": 3}) for r in a[PRE]])
    _t, _rc, res, out = _run(four(s1_1m_rel=pa))
    check("pre carrying empty_reads= voids (not A)", bool(res["s1_1m_rel"]["void"]) and "A is not the parent" in out)

    print("case VOID: fewer than 3 runs; RELIABLE incomplete; out_of_order")
    few = _cell(head_sub=(2.2, 2.21))
    _t, _rc, res, _o = _run(four(s1_4m_rel=few))
    check("2 head runs -> VOID", "fewer than 3" in " ".join(res["s1_4m_rel"]["void"]))
    inc = _cell(tickle_pre=lambda a: [dict(r, incomplete=True, lost=3) if r["rep"] == 1 else r for r in a[PRE]])
    _t, _rc, res, _o = _run(four(s1_4m_rel=inc))
    check("RELIABLE pre run incomplete -> VOID", "delivered less" in " ".join(res["s1_4m_rel"]["void"]))
    _t, _rc, res, _o = _run(four(s1_4m_be=inc))
    check("control: the same in a BEST_EFFORT cell does not void", not res["s1_4m_be"]["void"])
    oo = _cell(tickle_head=lambda a: [dict(r, ooo=1) if r["rep"] == 3 else r for r in a[HEAD]])
    _t, _rc, res, _o = _run(four(s1_1m_be=oo))
    check("out_of_order=1 -> VOID", "out_of_order=1" in " ".join(res["s1_1m_be"]["void"]))

    print("case NO VERDICT: a cell missing; FAIL dominates")
    cells = four()
    del cells["s1_4m_rel"]
    text, rc, _r, _o = _run(cells)
    check("missing cell -> NO VERDICT", rc == 2 and "s1_4m_rel not in the input" in text)
    text, rc, _r, _o = _run(dict(cells, s1_1m_rel=eq))
    check("FAIL exit 1 with the missing cell listed", rc == 1 and "not in the input" in text)

    print("case end to end through largemsg_rig_summary.load (fixture s3b: Array4m 15 Hz RELIABLE)")
    fx = Path(__file__).resolve().parent / "largemsg_rig_fixture" / "s3b.runs"
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp) / "s1_4m_rel.runs"
        shutil.copytree(fx, t)
        index = (t / "index.txt").read_text().splitlines()
        new = list(index)
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
            extra = "\n[I] Node 2 rx: socket_drops=0 drain=1 direct=1 direct_reads=900 direct_runs=890"
            if f.name.startswith(HEAD):
                extra += " nonblock=1 empty_reads=10 gro_on=0 timer_waits=500 timer_sets=4 timer_fires=3"
            f.write_text(f.read_text(errors="replace") + extra + "\n")
        for f in t.glob("tickle@*_pub.log"):
            hist = "0/0/0/0/0/1/600" if f.name.startswith(HEAD) else "1/16/15/15/15/15/525"
            f.write_text(f.read_text(errors="replace") + f"\n[I] Node 3 hybrid: runs=9 n_hist={hist} cap=45\n")
        for f in t.glob("*_sub.start"):
            stem = f.name[:-len("_sub.start")]
            (t / f"{stem}_sub.pgf").write_text("".join(f"{int(i * 0.5e9)} {500 + i // 10} 0\n" for i in range(140)))
        out_base = str(t)[:-len(".runs")]
        name, reliable, arms = read_out(out_base)
        check(f"named {name}, RELIABLE {reliable}, 3 head + 3 pre runs",
              name == "s1_4m_rel" and reliable and len(arms.get(HEAD, [])) == 3 and len(arms.get(PRE, [])) == 3)
        check("head fields and n_hist read from the logs; pre's line has no empty_reads=",
              all(r["rx_fields"].get("timer_waits") == 500 and r["below"] == 1 for r in arms[HEAD])
              and all("empty_reads" not in r["rx_fields"] and r["below"] == 77 for r in arms[PRE]))
        check("page faults per MB read for every arm",
              all(not math.isnan(r["pgf_per_mb"]) for a in arms.values() for r in a))
        log = []
        res = judge_cell(name, reliable, arms, out=log.append)
        check("identical head and pre: not VOID, (a) MISSED (a FAIL path through real files)",
              not res["void"] and res["a"] == "MISSED")
        rc = main([out_base, "--preflight"])
        check("--preflight on it: PREFLIGHT OK (exit 0)", rc == 0)
        lf = t / "tickle@head_Array4m_l0_r2_sub.log"
        lf.write_text(lf.read_text().replace(" timer_waits=500", ""))
        rc = main([out_base, "--preflight"])
        check("--preflight with one head line lacking timer_waits=: PREFLIGHT FAILED (exit 1)", rc == 1)
        rc = main([out_base])
        check("and the reading: NO VERDICT (exit 2)", rc == 2)
    print(f"SELFTEST {'FAILED: ' + '; '.join(fails) if fails else 'OK'}")
    return 1 if fails else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    sys.exit(main(sys.argv[1:]))
