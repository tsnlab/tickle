#!/usr/bin/env python3
"""udp_csum0_read.py - the rig A/B of UDP checksum 0 (hal_linux.c "UDP checksum 0", TT_UDP_CSUM0=1), read by rules
written 2026-10-14 before any run (~/rig_jobs/rig_queue_udp_csum0.sh). Nothing here is judged by hand: every rule
below is implemented in this file.

WHAT IS TESTED. tickle@csum0 is tickle@head's own librmw_tickle.so (the tip pushed as ab/udp-csum0) with TT_UDP_CSUM0=1
at both ends: every sendmmsg() call that carries an IP-fragmented datagram (a hybrid run) goes with UDP checksum 0, so
the subscriber's kernel reads it with a plain copy instead of checking it first. tickle@head is the control: the same
binary, TT_UDP_CSUM0 unset.

EXPECTED, from the evidence before the build (rpi-0 kallsyms profile, ~/claude_reports/rx_perf_gap2_2026-10-13.md
section 9; ethtool -k on both Pis, 2026-10-14): the macb NIC checks the checksum of a datagram of one frame, never of an
IP fragment, so every run's checksum is made in software at the receiver - 0.31 / 0.30 ms/MB of the subscriber's kernel
time (1 / 4 MB), Fast DDS 0.24 / 0.22. With checksum 0 that pass goes, but the copy that follows it then reads the
data cold (the pass had brought it into the cache): the subscriber should save 0.05-0.15 ms/MB, not the whole pass.
The publisher makes the checksum of an IP-fragmented datagram as it copies from user with or without SO_NO_CHECK, and
pays a setsockopt() only where a call's kind changes (SO_NO_CHECK stays on between calls): no saving expected there.
The PC (udp_csum0_pc.sh, dbd74e7a): subscriber kernel instructions -0.355 M per MB in every cell, cycles HELD (x86
copies with one rep movsb, so instructions overstate); publisher instructions +0.07..+0.15% / -0.11% (one cell's
+0.15% at its floor).

INPUT. The four invocations of rmw_keepall_rig.sh (OUT.runs/ each), cross-host at 0% loss, arms tickle@csum0,
tickle@head, fastdds, cyclonedds:
  s1_1m_rel  Array1m 30 Hz RELIABLE KEEP_LAST 10        s1_1m_be  Array1m 30 Hz BEST_EFFORT KEEP_LAST 5
  s1_4m_rel  Array4m 15 Hz RELIABLE KEEP_LAST 10        s1_4m_be  Array4m 15 Hz BEST_EFFORT KEEP_LAST 5

RULES
  VOID (the cell is not read), any of:
    - fewer than 3 usable runs of csum0 or head;
    - a RELIABLE cell where a csum0 or head run did not deliver every sample;
    - out_of_order > 0 on a csum0 or head subscriber's rmw_tickle delivery lines (summed);
    - TREATMENT (each arm reports its own; a line or field missing is "TREATMENT NOT READ"): on both ends' "Node <n>
      rx:" lines, csum0: csum0=1 on every line and the publisher's csum0_datagrams at least the samples the subscriber
      received (a run per sample at the least) - else "TREATMENT NOT APPLIED"; head: csum0=0 and csum0_datagrams=0 on
      every line of both ends;
    - CONTROL (rig drift): fastdds's subscriber CPU per MB, the mean of its first two reps against its last two, moved
      by more than 0.03 ms/MB.
  FLOOR per cell, metric: 2 x SE (Welch) of the difference of means; D = mean(csum0) - mean(head).
  PRIMARY, subscriber CPU per MB:
    WIN        D <= -0.03 and D < -FLOOR in all four cells.
    FALSIFIED  D > -0.015 and |D| <= FLOOR in all four cells (the saving the evidence predicts is absent).
    NO WIN     anything else.
  NO HARM, every cell, csum0 against head: publisher CPU per MB WORSE iff D > FLOOR; latency median and p99 lower bound
    WORSE iff D > max(FLOOR, 0.1% of head's mean); BEST_EFFORT delivered % WORSE iff it fell by more than 1 point.
  REPORTED, never deciding: every arm's means; csum0 and head against fastdds and cyclonedds; csum0's datagrams with
    checksum 0 per sample received.
  VERDICT, last line "UDP CSUM0 VERDICT: WIN|NO WIN|FALSIFIED|FAIL|NO VERDICT (reasons)": FAIL on any NO HARM WORSE;
    NO VERDICT when a cell is missing or VOID, or anything deciding is NOT READ; else the PRIMARY's. Exit 0 (WIN, NO WIN,
    FALSIFIED) / 1 (FAIL) / 2 (NO VERDICT).
  --preflight (PC, REPS=1): the reading runs end to end; PREFLIGHT FAILED when a cell has no usable csum0 or head run,
    or a treatment is NOT READ / NOT APPLIED / wrong. Last line "PREFLIGHT OK" (exit 0) or "PREFLIGHT FAILED: ..." (1).

Usage: udp_csum0_read.py OUT [OUT ...] [--preflight]      udp_csum0_read.py --selftest
"""
import math
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rx_gap2_read import fin, mean_or_nan, out_of_order, welch  # noqa: E402

CELLS = {("Array1m", True, 30): "s1_1m_rel", ("Array1m", False, 30): "s1_1m_be",
         ("Array4m", True, 15): "s1_4m_rel", ("Array4m", False, 15): "s1_4m_be"}
CELL_ORDER = ("s1_1m_rel", "s1_1m_be", "s1_4m_rel", "s1_4m_be")
ON, OFF = "tickle@csum0", "tickle@head"
TICKLE = (ON, OFF)
VENDORS = ("fastdds", "cyclonedds")
MIN_RUNS = 3
WIN_D = -0.03
FALSIFIED_D = -0.015
DRIFT = 0.03
LAT_REL_FLOOR = 0.001
BE_DELIVERY_POINTS = 1.0
RX_RE = re.compile(r"Node \d+ rx: (.*)$", re.M)


def csum0_fields(text):
    """("no_line", None) without a "Node <n> rx:" line; ("no_field", None) when a line lacks csum0= or
    csum0_datagrams=; else ("ok", (the set of csum0 values, the sum of csum0_datagrams))."""
    lines = RX_RE.findall(text)
    if not lines:
        return "no_line", None
    states, sent = set(), 0
    for line in lines:
        on = re.search(r"\bcsum0=(-?\d+)", line)
        n = re.search(r"\bcsum0_datagrams=(\d+)", line)
        if on is None or n is None:
            return "no_field", None
        states.add(int(on.group(1)))
        sent += int(n.group(1))
    return "ok", (states, sent)


def treatment_problem(r):
    """'' when the run's treatment is as required, else why not (the text starts with the VOID class)."""
    for side in ("pub", "sub"):
        state, _ = r.get(f"{side}_csum0", ("no_line", None))
        if state != "ok":
            return f"TREATMENT NOT READ: the {side} log has {'no rx line' if state == 'no_line' else 'no csum0 fields'}"
    (p_states, p_sent), (s_states, _s_sent) = r["pub_csum0"][1], r["sub_csum0"][1]
    if r["arm"] == ON:
        bad = []
        if p_states != {1} or s_states != {1}:
            bad.append(f"csum0 pub {sorted(p_states)} sub {sorted(s_states)}")
        if p_sent < max(1, r.get("recv", 0)):
            bad.append(f"the publisher sent {p_sent} checksum-0 datagrams for {r.get('recv', 0)} samples received")
        return ("TREATMENT NOT APPLIED: " + ", ".join(bad)) if bad else ""
    if p_states != {0} or s_states != {0} or p_sent != 0:
        return f"TREATMENT: head has csum0 pub {sorted(p_states)} sub {sorted(s_states)}, {p_sent} sent"
    return ""


def compare(a, b):
    x, y = fin(a), fin(b)
    if len(x) < MIN_RUNS or len(y) < MIN_RUNS:
        return None
    return welch(x, y)


def harm(arms, metric, relative, out, label):
    c = compare([r.get(metric) for r in arms.get(ON, [])], [r.get(metric) for r in arms.get(OFF, [])])
    if c is None:
        out(f"  NO HARM {label}: NOT READ (fewer than 3 finite values)")
        return "NOT READ"
    d, se = c
    thr = 2 * se
    if relative:
        thr = max(thr, LAT_REL_FLOOR * abs(statistics.mean(fin(r.get(metric) for r in arms[OFF]))))
    v = "WORSE" if d > thr else "HELD"
    out(f"  NO HARM {label}: d {d:+.4f}, threshold {thr:.4f} -> {v}")
    return v


def drift(arms):
    """fastdds's subscriber CPU per MB, first two reps against last two; None when it has fewer than 4."""
    fd = sorted((r for r in arms.get("fastdds", []) if r.get("recv", 0) > 0), key=lambda r: r["rep"])
    xs = [r.get("cpu_sub") for r in fd]
    if len(fin(xs)) < 4 or len(fin(xs)) != len(xs):
        return None
    return statistics.mean(xs[-2:]) - statistics.mean(xs[:2])


def judge_cell(name, reliable, arms, out=print):
    res = dict(name=name, void=[], treatment_bad=[], incomplete_bad=[], primary=None, harm={})
    out(f"=== {name}: {'RELIABLE' if reliable else 'BEST_EFFORT'}; usable runs: "
        + ", ".join(f"{a} {len(arms.get(a, []))}" for a in TICKLE + VENDORS) + " ===")
    counts = {a: len(arms.get(a, [])) for a in TICKLE}
    if min(counts.values()) < MIN_RUNS:
        res["void"].append(f"fewer than {MIN_RUNS} usable runs ({', '.join(f'{a} {n}' for a, n in counts.items())})")
    for r in [r for a in TICKLE for r in arms.get(a, [])]:
        tag = f"{r['arm']} r{r['rep']}"
        why = treatment_problem(r)
        p = r.get("pub_csum0", ("no_line", None))
        out(f"  treatment {tag}: pub {p[0]} {p[1]}, sub {r.get('sub_csum0', ('no_line', None))[0]}"
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
    dr = drift(arms)
    if dr is None:
        out("  control: fastdds drift NOT READ (fewer than 4 fastdds runs) - not a VOID by itself")
    else:
        out(f"  control: fastdds sub CPU last two reps - first two {dr:+.4f} ms/MB (allowed {DRIFT})")
        if abs(dr) > DRIFT:
            res["void"].append(f"CONTROL: fastdds drifted {dr:+.4f} ms/MB")
    # ---- context, always printed --------------------------------------------------------------------------------
    for metric in ("cpu_sub", "cpu_pub", "lat_med", "p99_lb", "delivered_pct"):
        line = [f"  {metric:13s}"]
        for a in TICKLE + VENDORS:
            xs = fin(r.get(metric) for r in arms.get(a, []))
            line.append(f"{a.replace('tickle@', '')} {mean_or_nan(xs):9.3f}")
        out(" ".join(line))
    for r in arms.get(ON, []):
        p = r.get("pub_csum0", ("no_line", None))
        if p[0] == "ok" and r.get("recv"):
            out(f"  csum0 r{r['rep']}: {p[1][1] / r['recv']:.1f} checksum-0 datagrams a sample received (not deciding)")
    for v in VENDORS:
        for a in TICKLE:
            am = mean_or_nan(fin(r.get("cpu_sub") for r in arms.get(a, [])))
            vm = mean_or_nan(fin(r.get("cpu_sub") for r in arms.get(v, []) if r.get("recv", 0) > 0))
            if not math.isnan(am) and not math.isnan(vm) and vm > 0:
                out(f"  context {a.replace('tickle@', '')} - {v}: sub CPU {am - vm:+.3f} ms/MB ({am / vm:.3f}x) "
                    "(not deciding)")
    if res["void"]:
        out(f"  VOID {name}: " + " | ".join(res["void"]))
        return res
    # ---- PRIMARY -------------------------------------------------------------------------------------------------
    c = compare([r.get("cpu_sub") for r in arms[ON]], [r.get("cpu_sub") for r in arms[OFF]])
    if c is None:
        res["primary"] = ("NOT READ", None, None)
        out("  PRIMARY sub CPU: NOT READ")
    else:
        d, se = c
        res["primary"] = ("READ", d, 2 * se)
        out(f"  PRIMARY sub CPU per MB: D {d:+.4f} ms/MB, floor {2 * se:.4f} (WIN needs D <= {WIN_D} and < -floor)")
    # ---- NO HARM -------------------------------------------------------------------------------------------------
    res["harm"]["cpu_pub"] = harm(arms, "cpu_pub", False, out, "cpu_pub")
    res["harm"]["lat_med"] = harm(arms, "lat_med", True, out, "lat_med")
    res["harm"]["p99_lb"] = harm(arms, "p99_lb", True, out, "p99_lb")
    if not reliable:
        on, off = fin(r.get("delivered_pct") for r in arms[ON]), fin(r.get("delivered_pct") for r in arms[OFF])
        if len(on) < MIN_RUNS or len(off) < MIN_RUNS:
            res["harm"]["delivered_pct"] = "NOT READ"
            out("  NO HARM delivered_pct: NOT READ")
        else:
            drop = statistics.mean(off) - statistics.mean(on)
            res["harm"]["delivered_pct"] = "WORSE" if drop > BE_DELIVERY_POINTS else "HELD"
            out(f"  NO HARM delivered_pct: csum0 {statistics.mean(on):.2f}% head {statistics.mean(off):.2f}% (drop "
                f"{drop:+.2f} points, allowed {BE_DELIVERY_POINTS}) -> {res['harm']['delivered_pct']}")
    return res


def verdict(results, problems, out=print):
    fail, unread = [], list(problems)
    prim = []
    for name in CELL_ORDER:
        r = results.get(name)
        if r is None:
            continue
        if r["void"]:
            unread.append(f"{name} VOID")
            continue
        if r["primary"][0] != "READ":
            unread.append(f"{name} PRIMARY NOT READ")
        else:
            prim.append((name, r["primary"][1], r["primary"][2]))
        for metric, v in r["harm"].items():
            if v == "WORSE":
                fail.append(f"{name} NO HARM {metric} WORSE")
            elif v != "HELD":
                unread.append(f"{name} NO HARM {metric} NOT READ")
    if fail:
        text, rc = "FAIL (" + "; ".join(fail + unread) + ")", 1
    elif unread or len(prim) != len(CELL_ORDER):
        text, rc = "NO VERDICT (" + "; ".join(unread or ["not every cell read"]) + ")", 2
    else:
        ds = ", ".join(f"{n} {d:+.3f}" for n, d, _ in prim)
        if all(d <= WIN_D and d < -fl for _, d, fl in prim):
            text = f"WIN (sub CPU per MB D: {ds})"
        elif all(d > FALSIFIED_D and abs(d) <= fl for _, d, fl in prim):
            text = f"FALSIFIED (sub CPU per MB D: {ds}; no cell shows the predicted saving)"
        else:
            text = f"NO WIN (sub CPU per MB D: {ds})"
        rc = 0
    out(f"UDP CSUM0 VERDICT: {text}")
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
            sub = (runs / f"{stem}_sub.log").read_text(errors="replace") if (runs / f"{stem}_sub.log").exists() else ""
            pub = (runs / f"{stem}_pub.log").read_text(errors="replace") if (runs / f"{stem}_pub.log").exists() else ""
            r["pub_csum0"] = csum0_fields(pub)
            r["sub_csum0"] = csum0_fields(sub)
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
        print(f"PREFLIGHT OK: {len(results)} cell(s) read end to end, treatment present on every csum0 and head run")
        return 0
    return rc


# ---- selftest --------------------------------------------------------------------------------------------------------
def _rec(arm, rep, cpu_sub=2.30, cpu_pub=3.90, lat=10.0, p99=12.0, dpct=100.0, recv=900):
    tickle = arm.startswith("tickle@")
    on = 1 if arm == ON else 0
    r = dict(arm=arm, rep=rep, cpu_sub=cpu_sub, cpu_pub=cpu_pub, lat_med=lat, p99_lb=p99, delivered_pct=dpct,
             recv=recv, lost=0 if dpct >= 100 else 5, incomplete=dpct < 100 or recv == 0)
    if tickle:
        r.update(pub_csum0=("ok", ({on}, 16 * recv * on)), sub_csum0=("ok", ({on}, 0)), ooo=0)
    return r


def _cell(on_sub=(2.20, 2.21, 2.19, 2.22, 2.20), off_sub=(2.30, 2.31, 2.29, 2.32, 2.30),
          on_pub=(3.90, 3.91, 3.89, 3.92, 3.90), off_pub=(3.90, 3.91, 3.89, 3.92, 3.90), fd=(2.10, 2.11, 2.10, 2.11, 2.10)):
    return {ON: [_rec(ON, i + 1, cpu_sub=s, cpu_pub=p) for i, (s, p) in enumerate(zip(on_sub, on_pub))],
            OFF: [_rec(OFF, i + 1, cpu_sub=s, cpu_pub=p) for i, (s, p) in enumerate(zip(off_sub, off_pub))],
            "fastdds": [_rec("fastdds", i + 1, cpu_sub=s) for i, s in enumerate(fd)],
            "cyclonedds": [_rec("cyclonedds", i + 1, cpu_sub=3.0) for i in range(5)]}


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
    line = "[I] Node 2 rx: socket_drops=0 drain=1 nonblock=1 csum0=1 csum0_calls=30 csum0_datagrams=480 csum0_singles=0\n"
    check("rx line", csum0_fields("x\n" + line + line) == ("ok", ({1}, 960)))
    check("no rx line", csum0_fields("nothing\n") == ("no_line", None))
    check("rx line without the fields", csum0_fields("[I] Node 2 rx: socket_drops=0\n")[0] == "no_field")
    print("case WIN / NO WIN / FALSIFIED")
    text, rc, _r, _o = _run(four())
    check(f"WIN exit 0 ({text[:50]})", rc == 0 and text.startswith("WIN"))
    small = _cell(on_sub=(2.28, 2.29, 2.27, 2.30, 2.28))
    text, rc, _r, _o = _run(four(s1_4m_be=small))
    check(f"one cell -0.02: NO WIN ({text[:50]})", rc == 0 and text.startswith("NO WIN"))
    same = _cell(on_sub=(2.30, 2.31, 2.29, 2.32, 2.30))
    text, rc, _r, _o = _run({n: _cell(on_sub=(2.30, 2.31, 2.29, 2.32, 2.30)) for n in CELL_ORDER})
    check(f"no saving anywhere: FALSIFIED ({text[:50]})", rc == 0 and text.startswith("FALSIFIED"))
    text, rc, _r, _o = _run(four(s1_1m_rel=same))
    check("one cell without the saving: NO WIN, not FALSIFIED", rc == 0 and text.startswith("NO WIN"))
    print("case NO HARM")
    pub = _cell(on_pub=(4.00, 4.01, 3.99, 4.02, 4.00))
    text, rc, _r, _o = _run(four(s1_1m_be=pub))
    check("publisher CPU worse -> FAIL", rc == 1 and "s1_1m_be NO HARM cpu_pub WORSE" in text)
    lat = _cell()
    for k in range(1, 6):
        lat = _with(lat, ON, k, lat_med=11.0 + 0.01 * k)
    text, rc, _r, _o = _run(four(s1_4m_rel=lat))
    check("latency worse -> FAIL", rc == 1 and "s1_4m_rel NO HARM lat_med WORSE" in text)
    be = _cell()
    for k in range(1, 6):
        be = _with(be, ON, k, delivered_pct=98.0)
    text, rc, _r, _o = _run(four(s1_4m_be=be))
    check("BE delivery 2 points lower -> FAIL", rc == 1 and "delivered_pct WORSE" in text)
    print("case VOID: treatments, control, incomplete")
    for what, arm, change, word in (
            ("csum0 run that sent no checksum-0 datagram", ON, {"pub_csum0": ("ok", ({1}, 0))}, "NOT APPLIED"),
            ("csum0 run with csum0=0 at the subscriber", ON, {"sub_csum0": ("ok", ({0}, 0))}, "NOT APPLIED"),
            ("csum0 run without csum0 fields", ON, {"pub_csum0": ("no_field", None)}, "NOT READ"),
            ("head run with csum0=1", OFF, {"pub_csum0": ("ok", ({1}, 500))}, "TREATMENT: head"),
            ("head run without an rx line", OFF, {"sub_csum0": ("no_line", None)}, "NOT READ")):
        bad = _with(_cell(), arm, 2, **change)
        text, rc, res, out = _run(four(s1_1m_rel=bad))
        check(f"{what}: VOID, NO VERDICT", bool(res["s1_1m_rel"]["void"]) and word in out and rc == 2)
    drifted = _cell(fd=(2.10, 2.10, 2.12, 2.15, 2.15))
    text, rc, res, out = _run(four(s1_1m_be=drifted))
    check("fastdds drift 0.05: VOID", rc == 2 and "CONTROL: fastdds drifted" in out)
    steady = _cell(fd=(2.10, 2.10, 2.12, 2.12, 2.12))
    _t, rc, _r, _o = _run(four(s1_1m_be=steady))
    check("fastdds drift 0.02 (control): read", rc == 0)
    inc = _with(_cell(), OFF, 1, incomplete=True, lost=3)
    _t, rc, res, _o = _run(four(s1_4m_rel=inc))
    check("RELIABLE head run incomplete -> VOID", rc == 2 and "delivered less" in " ".join(res["s1_4m_rel"]["void"]))
    few = _cell(on_sub=(2.20, 2.21))
    _t, rc, res, _o = _run(four(s1_4m_rel=few))
    check("2 csum0 runs -> VOID", rc == 2 and "fewer than 3" in " ".join(res["s1_4m_rel"]["void"]))
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
