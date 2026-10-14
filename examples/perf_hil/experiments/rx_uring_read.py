#!/usr/bin/env python3
"""rx_uring_read.py - the rig A/B of ab/rx-uring-wait: tt_receive()'s wait on io_uring against the same binary with
TT_RX_URING_WAIT=0, read by rules written 2026-10-14 before any run (~/rig_jobs/rig_queue_rx_uring.sh; the design's
section 4, ~/claude_reports/RX_URING_WAIT_DESIGN_2026-10-13.md). Nothing here is judged by hand: every rule below is
implemented in this file. The parsers come from rx_gap2_read.py and rx_gap3_read.py.

WHAT IS TESTED. tickle@head (B) waits on io_uring while large samples arrive (C1-C4: one-shot recvmsg per socket and one
io_uring_enter() per wait, receive offload's merged reads, the aimed read, the deadline as an IORING_OP_TIMEOUT).
tickle@nouring (A) is head's own librmw_tickle.so with TT_RX_URING_WAIT=0 at both ends: the epoll wait as before.
fastdds is the control the change provably cannot touch; cyclonedds is reported.

INPUT. The four stage-1 invocations of rmw_keepall_rig.sh (OUT.runs/ each), cross-host at 0% loss, arms tickle@head,
tickle@nouring, fastdds, cyclonedds, reps rotated by the harness:
  s1_1m_rel  Array1m 30 Hz RELIABLE KEEP_LAST 10        s1_1m_be  Array1m 30 Hz BEST_EFFORT KEEP_LAST 5
  s1_4m_rel  Array4m 15 Hz RELIABLE KEEP_LAST 10        s1_4m_be  Array4m 15 Hz BEST_EFFORT KEEP_LAST 5

RULES
  VOID (the cell is not read), any of:
    - fewer than 3 usable runs of head or nouring;
    - a RELIABLE cell where a head or nouring run did not deliver every sample;
    - out_of_order > 0 on a head or nouring subscriber's rmw_tickle delivery lines (summed);
    - head's subscriber rx line shows drain_truncated > 0 (the drain's handover broke);
    - TREATMENT (each arm reports its own; a line or field missing is "TREATMENT NOT READ"):
        nouring: every subscriber rx line uring_wait=0 and uring_waits=0 (the harness VOIDs a run that is not, and
          its runs then fall short of 3);
        head: the subscriber's rx line carries uring_wait=1 and uring_switches >= 1 (the wait went on the ring).
    - CONTROL (rig drift): fastdds's sub CPU per MB, first half of its reps against the second half, moved by more
      than 0.03 ms/MB.
  MECHANISM (head, per cell, from the subscriber's rx and traffic lines): share = uring_recvs / rx_datagrams.
    ENGAGED when share >= 0.5 and uring_fallbacks <= 0.1 x uring_waits; PART when share >= 0.1; else NOT ENGAGED.
    A cell not ENGAGED is VOID for the claim (not a loss): the design's rule names strace counts; the rig's harness
    has none, and the HAL's own counters say the same thing per run (stated deviation).
  FLOOR per cell and metric: 2 x SE of the difference of means (Welch). D = mean(head) - mean(nouring).
  PRIMARY (WIN), sub CPU per MB (ms/MB, perf_test cpu_sub): D <= -0.02 in both 1 MB cells and D <= -0.03 in both 4 MB
    cells, each with D < -FLOOR (ranges separate).
  FALSIFIED: in the ENGAGED cells, D > -0.015 with |D| <= FLOOR at both 1 MB and 4 MB (in at least one cell of each).
  NO HARM per cell, head against nouring: latency median and p99 lower bound WORSE iff D > max(FLOOR, 0.1% of
    nouring's mean); pub CPU per MB WORSE iff D > FLOOR (the env reaches the publisher too); BEST_EFFORT delivered %
    WORSE iff it fell by more than 1 point.
  REPORTED, never deciding: every arm's means; head against fastdds and cyclonedds; the head subscriber's uring
    counters per sample; page faults per MB.
  VERDICT, last line "RX URING VERDICT: WIN|NO WIN|FALSIFIED|FAIL|NO VERDICT (reasons)": FAIL on any NO HARM WORSE;
    NO VERDICT when a cell is missing or VOID, or anything deciding is NOT READ; FALSIFIED by the rule above; WIN when
    PRIMARY holds in all four cells; else NO WIN. Exit 0 WIN, 1 FAIL/FALSIFIED/NO WIN, 2 NO VERDICT.
  --preflight (PC, REPS=1): the reading runs end to end; PREFLIGHT FAILED when a cell has no usable head or nouring
    run, a treatment is NOT READ or wrong, or a RELIABLE run was incomplete. Last line "PREFLIGHT OK" (exit 0) or
    "PREFLIGHT FAILED: ..." (exit 1).

Usage: rx_uring_read.py OUT [OUT ...] [--preflight]      rx_uring_read.py --selftest
"""
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rx_gap2_read import fin, mean_or_nan, out_of_order, pgf_per_mb, welch  # noqa: E402
from rx_gap3_read import rx_fields  # noqa: E402

CELLS = {("Array1m", True, 30): "s1_1m_rel", ("Array1m", False, 30): "s1_1m_be",
         ("Array4m", True, 15): "s1_4m_rel", ("Array4m", False, 15): "s1_4m_be"}
CELL_ORDER = ("s1_1m_rel", "s1_1m_be", "s1_4m_rel", "s1_4m_be")
WIN_BELOW = {"s1_1m_rel": -0.02, "s1_1m_be": -0.02, "s1_4m_rel": -0.03, "s1_4m_be": -0.03}
FALSIFY_ABOVE = -0.015
HEAD, NOURING = "tickle@head", "tickle@nouring"
TICKLE = (HEAD, NOURING)
VENDORS = ("fastdds", "cyclonedds")
MIN_RUNS = 3
CONTROL_DRIFT = 0.03
ENGAGED_SHARE = 0.5
PART_SHARE = 0.1
FALLBACK_SHARE = 0.1
LAT_REL_FLOOR = 0.001
BE_DELIVERY_POINTS = 1.0
SUB_FIELDS = ("uring_wait", "uring_switches", "uring_waits", "uring_recvs", "uring_fallbacks", "drain_truncated")
TRAFFIC_RE = re.compile(r"traffic: .*?\brx_datagrams=(\d+)")


def rx_datagrams(text):
    vals = [int(x) for x in TRAFFIC_RE.findall(text)]
    return sum(vals) if vals else None


def treatment_problem(r):
    """'' when the run's treatment is as required, else why not."""
    state, fields = r.get("rx_state", "no_line"), r.get("rx_fields", {})
    if state == "no_line":
        return "TREATMENT NOT READ: no 'Node <n> rx:' line in the subscriber log"
    if "uring_wait" not in fields or "uring_waits" not in fields:
        return "TREATMENT NOT READ: the subscriber's rx line lacks uring_wait= / uring_waits="
    if r["arm"] == NOURING:
        if fields["uring_wait"] != 0 or fields["uring_waits"] != 0:
            return f"TREATMENT: nouring uring_wait={fields['uring_wait']} uring_waits={fields['uring_waits']}"
        return ""
    if fields["uring_wait"] < 1 or fields.get("uring_switches", 0) < 1:
        return (f"TREATMENT NOT APPLIED: head uring_wait={fields['uring_wait']} "
                f"uring_switches={fields.get('uring_switches')}")
    if fields.get("drain_truncated", 0) > 0:
        return f"drain_truncated={fields['drain_truncated']}"
    return ""


def mechanism(head):
    """(label, share, fallback share) over the head runs."""
    recvs = sum(r.get("rx_fields", {}).get("uring_recvs", 0) for r in head)
    waits = sum(r.get("rx_fields", {}).get("uring_waits", 0) for r in head)
    falls = sum(r.get("rx_fields", {}).get("uring_fallbacks", 0) for r in head)
    dgrams = [r.get("rx_datagrams") for r in head]
    if not head or any(d is None for d in dgrams) or sum(dgrams) == 0:
        return "NOT READ", float("nan"), float("nan")
    share = recvs / sum(dgrams)
    fshare = falls / waits if waits else float("inf")
    if share >= ENGAGED_SHARE and fshare <= FALLBACK_SHARE:
        return "ENGAGED", share, fshare
    return ("PART" if share >= PART_SHARE else "NOT ENGAGED"), share, fshare


def compare(a, b):
    """(d, se) of mean(a) - mean(b), or None with fewer than MIN_RUNS finite values either side."""
    x, y = fin(a), fin(b)
    if len(x) < MIN_RUNS or len(y) < MIN_RUNS:
        return None
    return welch(x, y)


def control_drift(arms):
    """fastdds's sub CPU, first half of its reps against the second: the drift, or None when not readable."""
    fd = sorted((r for r in arms.get("fastdds", []) if r.get("recv", 0) > 0), key=lambda r: r["rep"])
    xs = fin(r.get("cpu_sub") for r in fd)
    if len(xs) < 2:
        return None
    half = len(xs) // 2
    return statistics.mean(xs[half:]) - statistics.mean(xs[:half])


def harm(arms, metric, relative, out, label):
    c = compare([r.get(metric) for r in arms.get(HEAD, [])], [r.get(metric) for r in arms.get(NOURING, [])])
    if c is None:
        out(f"  NO HARM {label}: NOT READ (fewer than 3 finite values)")
        return "NOT READ"
    d, se = c
    thr = 2 * se
    if relative:
        thr = max(thr, LAT_REL_FLOOR * abs(statistics.mean(fin(r.get(metric) for r in arms[NOURING]))))
    v = "WORSE" if d > thr else "HELD"
    out(f"  NO HARM {label}: d {d:+.4f}, threshold {thr:.4f} -> {v}")
    return v


def judge_cell(name, reliable, arms, out=print):
    head = arms.get(HEAD, [])
    res = dict(name=name, void=[], treatment_bad=[], incomplete_bad=[], primary=None, d=None, floor=None,
               mech=None, harm={})
    out(f"=== {name}: {'RELIABLE' if reliable else 'BEST_EFFORT'}; usable runs: "
        + ", ".join(f"{a} {len(arms.get(a, []))}" for a in TICKLE + VENDORS) + " ===")
    counts = {a: len(arms.get(a, [])) for a in TICKLE}
    if min(counts.values()) < MIN_RUNS:
        res["void"].append(f"fewer than {MIN_RUNS} usable runs ({', '.join(f'{a} {n}' for a, n in counts.items())})")
    for r in [r for a in TICKLE for r in arms.get(a, [])]:
        tag = f"{r['arm']} r{r['rep']}"
        why = treatment_problem(r)
        out(f"  treatment {tag}: rx {r.get('rx_state', 'no_line')} {r.get('rx_fields', {})}, rx_datagrams "
            f"{r.get('rx_datagrams')}" + (f" -> {why}" if why else " -> ok"))
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
    drift = control_drift(arms)
    if drift is None:
        res["void"].append("CONTROL NOT READ: fewer than 2 fastdds runs")
    elif abs(drift) > CONTROL_DRIFT:
        res["void"].append(f"CONTROL: fastdds sub CPU drifted {drift:+.4f} ms/MB between its halves")
    out(f"  control fastdds drift (second half - first): {drift if drift is not None else float('nan'):+.4f} ms/MB "
        f"(allowed {CONTROL_DRIFT})")
    mech, share, fshare = mechanism(head)
    res["mech"] = mech
    out(f"  MECHANISM {mech}: uring_recvs / rx_datagrams {share:.3f}, uring_fallbacks / uring_waits {fshare:.3f}")
    # ---- context, always printed --------------------------------------------------------------------------------
    for metric in ("cpu_sub", "cpu_pub", "lat_med", "p99_lb", "delivered_pct", "pgf_per_mb"):
        line = [f"  {metric:13s}"]
        for a in TICKLE + VENDORS:
            xs = fin(r.get(metric) for r in arms.get(a, []))
            line.append(f"{a.replace('tickle@', '')} {mean_or_nan(xs):9.3f}")
        out(" ".join(line))
    for r in head:
        recv, f = r.get("recv") or 0, r.get("rx_fields", {})
        if recv:
            out(f"  head r{r['rep']}: a sample - uring_waits {f.get('uring_waits', 0) / recv:.1f}, uring_recvs "
                f"{f.get('uring_recvs', 0) / recv:.1f}, uring_fallbacks {f.get('uring_fallbacks', 0) / recv:.1f} "
                "(not deciding)")
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
    c = compare([r.get("cpu_sub") for r in head], [r.get("cpu_sub") for r in arms.get(NOURING, [])])
    if c is None:
        res["primary"] = "NOT READ"
    else:
        d, se = c
        res["d"], res["floor"] = d, 2 * se
        met = d <= WIN_BELOW[name] and d < -2 * se
        res["primary"] = "MET" if met else "MISSED"
        out(f"  PRIMARY sub CPU head - nouring: d {d:+.4f} ms/MB, floor {2 * se:.4f}, needs <= {WIN_BELOW[name]} and "
            f"< -floor -> {res['primary']}")
    # ---- NO HARM -------------------------------------------------------------------------------------------------
    res["harm"]["lat_med"] = harm(arms, "lat_med", True, out, "lat_med")
    res["harm"]["p99_lb"] = harm(arms, "p99_lb", True, out, "p99_lb")
    res["harm"]["cpu_pub"] = harm(arms, "cpu_pub", False, out, "cpu_pub")
    if not reliable:
        hd, nd = fin(r.get("delivered_pct") for r in head), fin(r.get("delivered_pct") for r in arms.get(NOURING, []))
        if len(hd) < MIN_RUNS or len(nd) < MIN_RUNS:
            res["harm"]["delivered_pct"] = "NOT READ"
            out("  NO HARM delivered_pct: NOT READ")
        else:
            drop = statistics.mean(nd) - statistics.mean(hd)
            res["harm"]["delivered_pct"] = "WORSE" if drop > BE_DELIVERY_POINTS else "HELD"
            out(f"  NO HARM delivered_pct: head {statistics.mean(hd):.2f}% nouring {statistics.mean(nd):.2f}% (drop "
                f"{drop:+.2f} points, allowed {BE_DELIVERY_POINTS}) -> {res['harm']['delivered_pct']}")
    return res


def falsified(results):
    """True when the ENGAGED cells show no saving at both sizes (the design's section 3)."""
    flat = {"1m": False, "4m": False}
    for name, r in results.items():
        if r["void"] or r["mech"] != "ENGAGED" or r["d"] is None:
            continue
        if r["d"] > FALSIFY_ABOVE and abs(r["d"]) <= r["floor"]:
            flat["1m" if "_1m_" in name else "4m"] = True
    return flat["1m"] and flat["4m"]


def verdict(results, problems, out=print):
    fail, unread, voids, missed = [], list(problems), [], []
    for name in CELL_ORDER:
        r = results.get(name)
        if r is None:
            continue
        if r["void"]:
            voids.append(f"{name} VOID")
            continue
        if r["mech"] != "ENGAGED":
            voids.append(f"{name} mechanism {r['mech']}")
        if r["primary"] == "MISSED":
            missed.append(f"{name} PRIMARY MISSED")
        elif r["primary"] != "MET":
            unread.append(f"{name} PRIMARY NOT READ")
        for metric, v in r["harm"].items():
            if v == "WORSE":
                fail.append(f"{name} NO HARM {metric} WORSE")
            elif v != "HELD":
                unread.append(f"{name} NO HARM {metric} NOT READ")
    if fail:
        text, rc = "FAIL (" + "; ".join(fail + missed + unread + voids) + ")", 1
    elif unread or voids:
        text, rc = "NO VERDICT (" + "; ".join(unread + voids + missed) + ")", 2
    elif falsified(results):
        text, rc = "FALSIFIED (no saving at 1 MB and 4 MB in engaged cells; " + "; ".join(missed) + ")", 1
    elif missed:
        text, rc = "NO WIN (" + "; ".join(missed) + ")", 1
    else:
        text, rc = "WIN (all four cells: sub CPU down beyond the threshold and the floor, nothing WORSE)", 0
    out(f"RX URING VERDICT: {text}")
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
            r["rx_state"], r["rx_fields"] = rx_fields(sub, SUB_FIELDS)
            r["rx_datagrams"] = rx_datagrams(sub)
            r["ooo"] = out_of_order(sub)
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
        print(f"PREFLIGHT OK: {len(results)} cell(s) read end to end, treatment present on every head and nouring run")
        return 0
    return rc


# ---- selftest --------------------------------------------------------------------------------------------------------
def _rec(arm, rep, cpu_sub, cpu_pub=3.9, lat=10.0, p99=12.0, dpct=100.0, recv=900, fields=None, dgrams=16000):
    if fields is None:
        fields = ({"uring_wait": 0, "uring_switches": 0, "uring_waits": 0, "uring_recvs": 0, "uring_fallbacks": 0,
                   "drain_truncated": 0} if arm == NOURING else
                  {"uring_wait": 1, "uring_switches": 1, "uring_waits": 15000, "uring_recvs": 14000,
                   "uring_fallbacks": 10, "drain_truncated": 0})
    return dict(arm=arm, rep=rep, cpu_sub=cpu_sub, cpu_pub=cpu_pub, lat_med=lat, p99_lb=p99, delivered_pct=dpct,
                recv=recv, lost=0, incomplete=False, ooo=0, rx_state="ok", rx_fields=dict(fields),
                rx_datagrams=dgrams if arm.startswith("tickle@") else None, pgf_per_mb=0.0)


def _cell(head=(2.20, 2.21, 2.19, 2.22, 2.20), nouring=(2.30, 2.31, 2.29, 2.32, 2.30),
          fastdds=(2.10, 2.11, 2.09, 2.10, 2.11)):
    arms = {HEAD: [_rec(HEAD, i + 1, x) for i, x in enumerate(head)],
            NOURING: [_rec(NOURING, i + 1, x) for i, x in enumerate(nouring)],
            "fastdds": [_rec("fastdds", i + 1, x) for i, x in enumerate(fastdds)]}
    return arms


def _run(cells):
    sink = []
    results = {}
    for name, arms in cells.items():
        results[name] = judge_cell(name, name.endswith("rel"), arms, out=sink.append)
    return verdict(results, [], out=sink.append)


def selftest():
    names = CELL_ORDER
    text, rc = _run({n: _cell() for n in names})
    assert rc == 0 and text.startswith("WIN"), text
    # a saving below the 4 MB threshold (-0.02 at 4 MB): NO WIN
    cells = {n: _cell() for n in names}
    cells["s1_4m_rel"] = _cell(head=(2.28, 2.29, 2.27, 2.30, 2.28))
    text, rc = _run(cells)
    assert rc == 1 and text.startswith("NO WIN"), text
    # no saving anywhere: FALSIFIED
    flat = (2.30, 2.31, 2.29, 2.32, 2.30)
    text, rc = _run({n: _cell(head=flat) for n in names})
    assert rc == 1 and text.startswith("FALSIFIED"), text
    # latency WORSE: FAIL
    cells = {n: _cell() for n in names}
    for r in cells["s1_1m_be"][HEAD]:
        r["lat_med"] = 12.0
    text, rc = _run(cells)
    assert rc == 1 and text.startswith("FAIL"), text
    # nouring that waited on the ring: VOID -> NO VERDICT
    cells = {n: _cell() for n in names}
    cells["s1_1m_rel"][NOURING][0]["rx_fields"]["uring_waits"] = 5
    text, rc = _run(cells)
    assert rc == 2 and "s1_1m_rel VOID" in text, text
    # head never on the ring: VOID
    cells = {n: _cell() for n in names}
    cells["s1_4m_be"][HEAD][0]["rx_fields"]["uring_switches"] = 0
    text, rc = _run(cells)
    assert rc == 2 and "s1_4m_be VOID" in text, text
    # mechanism only part engaged: NO VERDICT, not a WIN
    cells = {n: _cell() for n in names}
    for r in cells["s1_1m_be"][HEAD]:
        r["rx_fields"]["uring_recvs"] = 3000
    text, rc = _run(cells)
    assert rc == 2 and "mechanism PART" in text, text
    # rig drift on the control: VOID
    cells = {n: _cell() for n in names}
    cells["s1_1m_rel"] = _cell(fastdds=(2.10, 2.10, 2.20, 2.20, 2.20))
    text, rc = _run(cells)
    assert rc == 2 and "s1_1m_rel VOID" in text, text
    # drain_truncated on head: VOID
    cells = {n: _cell() for n in names}
    cells["s1_4m_rel"][HEAD][1]["rx_fields"]["drain_truncated"] = 1
    text, rc = _run(cells)
    assert rc == 2 and "s1_4m_rel VOID" in text, text
    # a missing cell: NO VERDICT
    text, rc = _run({n: _cell() for n in names[:3]})
    assert rc == 0 or rc == 2, text  # verdict() is given no "not in the input" problem here; main() adds it
    print("rx_uring_read selftest: PASS")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    sys.exit(main(sys.argv[1:]))
