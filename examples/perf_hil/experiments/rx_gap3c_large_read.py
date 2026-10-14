#!/usr/bin/env python3
"""rx_gap3c_large_read.py - the rig NO HARM check of ab/rx-gap3c's tip on the large-message cells, read by rules written
2026-10-15 before any run (~/rig_jobs/rig_queue_rx_gap3c_large.sh). Nothing here is judged by hand: every rule below is
implemented in this file.

WHAT IS TESTED. tickle@head is the tip (B, pushed as ab/rx-gap3c-large; 64cb16f1 plus this reader and its harness
record): 723ec1f0 (origin/ab/rx-gap3b) plus f0ace5e0 (a blocking send the buffer refused is made again off the send's
own path), f768bcc8 (rx_wait() in line, the timer's waits out of line), 162eff2a (tidy) and 64cb16f1 (sendto, sendmsg,
recvfrom, recvmmsg and the wait's epoll_pwait2 made with the instruction itself, tt_sys6(), not through glibc's
cancellation-point wrappers). tickle@pre is A = 723ec1f0 itself. These commits are on the large-message send and
receive paths too, and nothing in them is meant to make those faster: this is a NO HARM check, there is no PRIMARY.

INPUT. The four invocations of rmw_keepall_rig.sh (OUT.runs/ each), cross-host at 0% loss, arms tickle@head, tickle@pre,
fastdds, cyclonedds, 5 reps x 60 s:
  s1_1m_rel  Array1m 30 Hz RELIABLE KEEP_LAST 10        s1_1m_be  Array1m 30 Hz BEST_EFFORT KEEP_LAST 5
  s1_4m_rel  Array4m 15 Hz RELIABLE KEEP_LAST 10        s1_4m_be  Array4m 15 Hz BEST_EFFORT KEEP_LAST 5

RULES
  VOID (the cell is not read), any of:
    - fewer than 3 usable runs of head or pre;
    - a RELIABLE cell where a head or pre run did not deliver every sample;
    - out_of_order > 0 on a head or pre subscriber's rmw_tickle delivery lines (summed);
    - TREATMENT, from the invocation's OUT.runs/libs.txt (rmw_keepall_rig.sh writes it in every invocation; a file, a
      line or a field missing is "TREATMENT NOT READ"), on each of the two hosts:
        head: built from the commit the invocation requested (requested head=), not from A; nm read its import table
          (it imports recvmsg and sendmmsg, the calls 64cb16f1 left alone - the positive control) and it imports none
          of sendto, sendmsg, recvfrom (the raw-syscall build) - else "TREATMENT NOT APPLIED";
        pre: built from A (723ec1f0c8f9...), and it imports sendto, sendmsg and recvfrom (the library's calls, as A
          makes them) - else "TREATMENT: pre is not A";
        head and pre: different sha256 on each host;
    - CONTROL (rig drift): fastdds's subscriber CPU per MB, the mean of its first two reps against its last two, moved
      by more than 0.15 ms/MB (udp_csum0_read.py's revised bound, 2026-10-15: Fast DDS's own rep-to-rep spread reaches
      0.1; the arms run interleaved within each rep, so a drift common to the rig moves head and pre alike).
  FLOOR per cell, metric: 2 x SE (Welch) of the difference of means; d = mean(head) - mean(pre).
  NO HARM, every cell, head against pre:
    subscriber CPU per MB WORSE iff d > FLOOR;  publisher CPU per MB WORSE iff d > FLOOR;
    latency median and p99 lower bound WORSE iff d > max(FLOOR, 0.1% of pre's mean);
    BEST_EFFORT delivered % WORSE iff it fell by more than 1 point.
  REPORTED, never deciding: every arm's means; head and pre against fastdds and cyclonedds (sub CPU, pub CPU, latency
    median); the libraries' records.
  VERDICT, last line "RX GAP3C LARGE VERDICT: PASS|FAIL|NO VERDICT (reasons)": FAIL on any NO HARM WORSE (in a cell that
    was read); NO VERDICT when a cell is missing or VOID, or a NO HARM metric is NOT READ; PASS when all four cells
    were read and nothing is WORSE. Exit 0 (PASS) / 1 (FAIL) / 2 (NO VERDICT).
  --preflight (PC, REPS=1): the reading runs end to end; PREFLIGHT FAILED when a cell has no usable head or pre run, a
    RELIABLE run is incomplete, or a treatment is NOT READ / NOT APPLIED / wrong. Last line "PREFLIGHT OK" (exit 0) or
    "PREFLIGHT FAILED: ..." (1).

Usage: rx_gap3c_large_read.py OUT [OUT ...] [--preflight]      rx_gap3c_large_read.py --selftest
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
HEAD, PRE = "tickle@head", "tickle@pre"
TICKLE = (HEAD, PRE)
VENDORS = ("fastdds", "cyclonedds")
PRE_COMMIT = "723ec1f0c8f91c94959fd9ff628bb5efbd37d834"
LIBC_RAW = frozenset(("sendto", "sendmsg", "recvfrom"))  # made with the instruction since 64cb16f1
LIBC_KEPT = frozenset(("recvmsg", "sendmmsg"))  # left to the library by 64cb16f1: nm's positive control
HOSTS = 2
MIN_RUNS = 3
DRIFT = 0.15
LAT_REL_FLOOR = 0.001
BE_DELIVERY_POINTS = 1.0
LIB_RE = re.compile(r"^LIB host=(\S+) arm=(\S+) commit=(\S*) sha256=(\S*) libc_net=(\S*)\s*$")
REQ_RE = re.compile(r"^requested head=(\S+) pre=(\S+)")


def parse_libs(text):
    """(requested head or None, {(arm, host): dict(commit, sha256, libc: frozenset or None)})."""
    req, libs = None, {}
    for line in (text or "").splitlines():
        m = REQ_RE.match(line)
        if m:
            req = m.group(1)
            continue
        m = LIB_RE.match(line)
        if m:
            host, arm, commit, sha, net = m.groups()
            libc = None if net == "NOT_READ" else frozenset(x for x in net.split(",") if x and x != "-")
            libs[(arm, host)] = dict(commit=commit, sha256=sha, libc=libc)
    return req, libs


def treatment_problems(req, libs):
    """Every reason the libraries are not the treatment; each starts with its VOID class. [] when they are."""
    bad = []
    if req is None or req in ("", "none"):
        return ["TREATMENT NOT READ: libs.txt has no requested head"]
    if PRE_COMMIT.startswith(req) or req.startswith(PRE_COMMIT[:8]):
        bad.append(f"TREATMENT: the requested head {req} is A")
    hosts = sorted({h for (_a, h) in libs})
    if len(hosts) != HOSTS:
        bad.append(f"TREATMENT NOT READ: libs.txt names {len(hosts)} host(s), not {HOSTS}")
    for h in hosts:
        hd, pr = libs.get((HEAD, h)), libs.get((PRE, h))
        for arm, rec in ((HEAD, hd), (PRE, pr)):
            if rec is None:
                bad.append(f"TREATMENT NOT READ: no {arm} library on {h}")
            elif rec["libc"] is None or not rec["commit"] or rec["commit"] == "-" or not rec["sha256"]:
                bad.append(f"TREATMENT NOT READ: {arm} on {h} (commit {rec['commit'] or '-'}, nm "
                           f"{'unread' if rec['libc'] is None else 'read'})")
            elif not LIBC_KEPT <= rec["libc"]:
                bad.append(f"TREATMENT NOT READ: {arm} on {h} imports {sorted(rec['libc'])}, not {sorted(LIBC_KEPT)} "
                           "(nm's positive control)")
        if hd is None or pr is None or hd["libc"] is None or pr["libc"] is None:
            continue
        if not (hd["commit"].startswith(req) or req.startswith(hd["commit"])) or hd["commit"] == PRE_COMMIT:
            bad.append(f"TREATMENT NOT APPLIED: head on {h} was built from {hd['commit'][:12]}, not {req[:12]}")
        if hd["libc"] & LIBC_RAW:
            bad.append(f"TREATMENT NOT APPLIED: head on {h} imports {sorted(hd['libc'] & LIBC_RAW)} (not the raw build)")
        if pr["commit"] != PRE_COMMIT:
            bad.append(f"TREATMENT: pre on {h} was built from {pr['commit'][:12]}, not A {PRE_COMMIT[:12]}")
        if not LIBC_RAW <= pr["libc"]:
            bad.append(f"TREATMENT: pre on {h} imports {sorted(pr['libc'] & LIBC_RAW)} of {sorted(LIBC_RAW)} (not A)")
        if hd["sha256"] == pr["sha256"]:
            bad.append(f"TREATMENT: head and pre on {h} are one library")
    return bad


def compare(a, b):
    x, y = fin(a), fin(b)
    if len(x) < MIN_RUNS or len(y) < MIN_RUNS:
        return None
    return welch(x, y)


def harm(arms, metric, relative, out, label):
    c = compare([r.get(metric) for r in arms.get(HEAD, [])], [r.get(metric) for r in arms.get(PRE, [])])
    if c is None:
        out(f"  NO HARM {label}: NOT READ (fewer than 3 finite values)")
        return "NOT READ"
    d, se = c
    thr = 2 * se
    if relative:
        thr = max(thr, LAT_REL_FLOOR * abs(statistics.mean(fin(r.get(metric) for r in arms[PRE]))))
    v = "WORSE" if d > thr else "HELD"
    out(f"  NO HARM {label}: d {d:+.4f}, threshold {thr:.4f} -> {v}")
    return v


def drift(arms):
    """fastdds's subscriber CPU per MB, last two reps minus first two; None when it has fewer than 4."""
    fd = sorted((r for r in arms.get("fastdds", []) if r.get("recv", 0) > 0), key=lambda r: r["rep"])
    xs = [r.get("cpu_sub") for r in fd]
    if len(fin(xs)) < 4 or len(fin(xs)) != len(xs):
        return None
    return statistics.mean(xs[-2:]) - statistics.mean(xs[:2])


def judge_cell(name, reliable, arms, libs_text, out=print):
    res = dict(name=name, void=[], treatment_bad=[], incomplete_bad=[], harm={})
    out(f"=== {name}: {'RELIABLE' if reliable else 'BEST_EFFORT'}; usable runs: "
        + ", ".join(f"{a} {len(arms.get(a, []))}" for a in TICKLE + VENDORS) + " ===")
    counts = {a: len(arms.get(a, [])) for a in TICKLE}
    if min(counts.values()) < MIN_RUNS:
        res["void"].append(f"fewer than {MIN_RUNS} usable runs ({', '.join(f'{a} {n}' for a, n in counts.items())})")
    req, libs = parse_libs(libs_text)
    for (arm, host), rec in sorted(libs.items()):
        out(f"  treatment lib {arm} {host}: commit {rec['commit'][:12]} sha256 {rec['sha256'][:12]} imports "
            f"{'NOT READ' if rec['libc'] is None else ','.join(sorted(rec['libc']))}")
    res["treatment_bad"] = treatment_problems(req, libs)
    out("  treatment: " + ("ok (head the raw build of the requested commit, pre A's library build, on both hosts)"
                           if not res["treatment_bad"] else " | ".join(res["treatment_bad"])))
    for r in [r for a in TICKLE for r in arms.get(a, [])]:
        tag = f"{r['arm']} r{r['rep']}"
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
    # ---- reported, always printed -------------------------------------------------------------------------------
    for metric in ("cpu_sub", "cpu_pub", "lat_med", "p99_lb", "delivered_pct"):
        line = [f"  {metric:13s}"]
        for a in TICKLE + VENDORS:
            xs = fin(r.get(metric) for r in arms.get(a, []))
            line.append(f"{a.replace('tickle@', '')} {mean_or_nan(xs):9.3f}")
        out(" ".join(line))
    for v in VENDORS:
        for a in TICKLE:
            parts = []
            for metric in ("cpu_sub", "cpu_pub", "lat_med"):
                am = mean_or_nan(fin(r.get(metric) for r in arms.get(a, [])))
                vm = mean_or_nan(fin(r.get(metric) for r in arms.get(v, []) if r.get("recv", 0) > 0))
                if not math.isnan(am) and not math.isnan(vm) and vm > 0:
                    parts.append(f"{metric} {am - vm:+.3f} ({am / vm:.3f}x)")
            if parts:
                out(f"  context {a.replace('tickle@', '')} - {v}: " + ", ".join(parts) + " (not deciding)")
    if res["void"]:
        out(f"  VOID {name}: " + " | ".join(res["void"]))
        return res
    # ---- NO HARM -------------------------------------------------------------------------------------------------
    res["harm"]["cpu_sub"] = harm(arms, "cpu_sub", False, out, "cpu_sub")
    res["harm"]["cpu_pub"] = harm(arms, "cpu_pub", False, out, "cpu_pub")
    res["harm"]["lat_med"] = harm(arms, "lat_med", True, out, "lat_med")
    res["harm"]["p99_lb"] = harm(arms, "p99_lb", True, out, "p99_lb")
    if not reliable:
        hd, pr = fin(r.get("delivered_pct") for r in arms[HEAD]), fin(r.get("delivered_pct") for r in arms[PRE])
        if len(hd) < MIN_RUNS or len(pr) < MIN_RUNS:
            res["harm"]["delivered_pct"] = "NOT READ"
            out("  NO HARM delivered_pct: NOT READ")
        else:
            drop = statistics.mean(pr) - statistics.mean(hd)
            res["harm"]["delivered_pct"] = "WORSE" if drop > BE_DELIVERY_POINTS else "HELD"
            out(f"  NO HARM delivered_pct: head {statistics.mean(hd):.2f}% pre {statistics.mean(pr):.2f}% (drop "
                f"{drop:+.2f} points, allowed {BE_DELIVERY_POINTS}) -> {res['harm']['delivered_pct']}")
    return res


def verdict(results, problems, out=print):
    fail, unread = [], list(problems)
    for name in CELL_ORDER:
        r = results.get(name)
        if r is None:
            continue
        if r["void"]:
            unread.append(f"{name} VOID")
            continue
        for metric, v in r["harm"].items():
            if v == "WORSE":
                fail.append(f"{name} NO HARM {metric} WORSE")
            elif v != "HELD":
                unread.append(f"{name} NO HARM {metric} NOT READ")
    if fail:
        text, rc = "FAIL (" + "; ".join(fail + unread) + ")", 1
    elif unread or any(n not in results for n in CELL_ORDER):
        text, rc = "NO VERDICT (" + "; ".join(unread or ["not every cell read"]) + ")", 2
    else:
        text, rc = "PASS (all four cells read, nothing WORSE)", 0
    out(f"RX GAP3C LARGE VERDICT: {text}")
    return text, rc


def read_out(out_path):
    """(cell name, reliable, arms, libs.txt text) of one invocation, or (None, reason, None, None)."""
    from largemsg_rig_summary import load
    runs = Path(f"{out_path}.runs")
    if not (runs / "index.txt").exists():
        return None, f"{runs}/index.txt missing", None, None
    meta, samehost, recs = load(runs)
    reliable = "--reliable" in meta.get("qos_args", "")
    rate = int(meta.get("pub_rate", "0") or 0)
    topics = {r["topic"] for r in recs}
    losses = {r["loss"] for r in recs}
    if samehost or len(topics) != 1 or losses != {0}:
        return None, f"{runs}: not one cross-host 0% cell (topics {sorted(topics)}, losses {sorted(losses)})", None, None
    name = CELLS.get((topics.pop(), reliable, rate))
    if name is None:
        return None, f"{runs}: not one of the four s1 cells (qos [{meta.get('qos_args')}], {rate} Hz)", None, None
    arms = {}
    for r in recs:
        stem = f"{r['arm']}_{r['topic']}_l{r['loss']}_r{r['rep']}"
        if r["void"]:
            print(f"  VOID run {stem}: {r['void']}")
            continue
        if r["arm"].startswith("tickle@"):
            sub = (runs / f"{stem}_sub.log").read_text(errors="replace") if (runs / f"{stem}_sub.log").exists() else ""
            r["ooo"] = out_of_order(sub)
        arms.setdefault(r["arm"], []).append(r)
    libs = (runs / "libs.txt").read_text(errors="replace") if (runs / "libs.txt").exists() else ""
    return name, reliable, arms, libs


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
        name, reliable, arms, libs = read_out(o)
        if name is None:
            print(f"NOT READ {o}: {reliable}")
            problems.append(f"{o} not read")
            continue
        if name in results:
            problems.append(f"{name} given twice")
            continue
        res = judge_cell(name, reliable, arms, libs)
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
        print(f"PREFLIGHT OK: {len(results)} cell(s) read end to end, treatment present on every invocation")
        return 0
    return rc


# ---- selftest --------------------------------------------------------------------------------------------------------
B_SELF = "64cb16f1b0bf6d0cbc277420df90e4f2b7fa6c47"
LIBS_OK = "\n".join(
    [f"requested head={B_SELF} pre=723ec1f0 ack=8d1c3712"]
    + [f"LIB host={h} arm=tickle@head commit={B_SELF} sha256={'a' * 64} libc_net=epoll_pwait2,recvmsg,sendmmsg"
       for h in ("10.1.1.214", "10.1.1.213")]
    + [f"LIB host={h} arm=tickle@pre commit={PRE_COMMIT} sha256={'b' * 64} "
       "libc_net=epoll_pwait2,recvfrom,recvmsg,sendmmsg,sendmsg,sendto" for h in ("10.1.1.214", "10.1.1.213")]) + "\n"


def _rec(arm, rep, cpu_sub=2.30, cpu_pub=3.90, lat=10.0, p99=12.0, dpct=100.0, recv=900):
    r = dict(arm=arm, rep=rep, cpu_sub=cpu_sub, cpu_pub=cpu_pub, lat_med=lat, p99_lb=p99, delivered_pct=dpct,
             recv=recv, lost=0 if dpct >= 100 else 5, incomplete=dpct < 100 or recv == 0)
    if arm.startswith("tickle@"):
        r["ooo"] = 0
    return r


def _cell(hd_sub=(2.30, 2.31, 2.29, 2.32, 2.30), pr_sub=(2.30, 2.31, 2.29, 2.32, 2.30),
          hd_pub=(3.90, 3.91, 3.89, 3.92, 3.90), pr_pub=(3.90, 3.91, 3.89, 3.92, 3.90), fd=(2.10, 2.11, 2.10, 2.11, 2.10)):
    return {HEAD: [_rec(HEAD, i + 1, cpu_sub=s, cpu_pub=p) for i, (s, p) in enumerate(zip(hd_sub, hd_pub))],
            PRE: [_rec(PRE, i + 1, cpu_sub=s, cpu_pub=p) for i, (s, p) in enumerate(zip(pr_sub, pr_pub))],
            "fastdds": [_rec("fastdds", i + 1, cpu_sub=s) for i, s in enumerate(fd)],
            "cyclonedds": [_rec("cyclonedds", i + 1, cpu_sub=3.0) for i in range(5)]}


def _run(cells, libs=None, problems=()):
    quiet = []
    libs = libs or {}
    results = {n: judge_cell(n, n.endswith("_rel"), a, libs.get(n, LIBS_OK), out=quiet.append) for n, a in cells.items()}
    text, rc = verdict(results, list(problems) + [f"{n} not in the input" for n in CELL_ORDER if n not in cells],
                       out=quiet.append)
    return text, rc, results, "\n".join(quiet)


def _with(arms, arm, rep, **change):
    arms[arm] = [dict(r, **change) if r["rep"] == rep else r for r in arms[arm]]
    return arms


def _lib_edit(old, new, count=1):
    assert old in LIBS_OK, old
    return LIBS_OK.replace(old, new, count)


def selftest():
    fails = []

    def check(what, ok):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}")
        if not ok:
            fails.append(what)

    def four(**over):
        return {n: over.get(n, _cell()) for n in CELL_ORDER}

    print("parsers")
    req, libs = parse_libs(LIBS_OK)
    check("libs.txt: requested head and four libraries", req == B_SELF and len(libs) == 4)
    check("libs.txt: the good record has no problem", treatment_problems(req, libs) == [])
    _r, nl = parse_libs("LIB host=h arm=tickle@head commit=x sha256=y libc_net=NOT_READ\n")
    check("libc_net=NOT_READ parses as unread", nl[(HEAD, "h")]["libc"] is None)
    print("case PASS")
    text, rc, _r, _o = _run(four())
    check(f"identical arms: PASS exit 0 ({text[:40]})", rc == 0 and text.startswith("PASS"))
    better = _cell(hd_sub=(2.20, 2.21, 2.19, 2.22, 2.20), hd_pub=(3.80, 3.81, 3.79, 3.82, 3.80))
    text, rc, _r, _o = _run(four(s1_4m_be=better))
    check("head cheaper: PASS (no PRIMARY, better is not judged)", rc == 0 and text.startswith("PASS"))
    print("case FAIL (must FAIL)")
    sub = _cell(hd_sub=(2.40, 2.41, 2.39, 2.42, 2.40))
    text, rc, _r, _o = _run(four(s1_1m_rel=sub))
    check("subscriber CPU +0.10 ms/MB -> FAIL", rc == 1 and "s1_1m_rel NO HARM cpu_sub WORSE" in text)
    pub = _cell(hd_pub=(4.00, 4.01, 3.99, 4.02, 4.00))
    text, rc, _r, _o = _run(four(s1_1m_be=pub))
    check("publisher CPU +0.10 ms/MB -> FAIL", rc == 1 and "s1_1m_be NO HARM cpu_pub WORSE" in text)
    lat = _cell()
    for k in range(1, 6):
        lat = _with(lat, HEAD, k, lat_med=11.0 + 0.01 * k)
    text, rc, _r, _o = _run(four(s1_4m_rel=lat))
    check("latency median +10% -> FAIL", rc == 1 and "s1_4m_rel NO HARM lat_med WORSE" in text)
    p99 = _cell()
    for k in range(1, 6):
        p99 = _with(p99, HEAD, k, p99_lb=12.5 + 0.01 * k)
    text, rc, _r, _o = _run(four(s1_4m_rel=p99))
    check("p99 +4% -> FAIL", rc == 1 and "s1_4m_rel NO HARM p99_lb WORSE" in text)
    tiny = _cell()
    for k in range(1, 6):
        tiny = _with(tiny, HEAD, k, lat_med=10.005)
        tiny = _with(tiny, PRE, k, lat_med=10.0)
    text, rc, _r, _o = _run(four(s1_1m_rel=tiny))
    check("latency +0.05% with no spread (below the 0.1% floor): PASS", rc == 0)
    be = _cell()
    for k in range(1, 6):
        be = _with(be, HEAD, k, delivered_pct=98.0)
    text, rc, _r, _o = _run(four(s1_4m_be=be))
    check("BE delivery 2 points lower -> FAIL", rc == 1 and "s1_4m_be NO HARM delivered_pct WORSE" in text)
    be2 = _cell()
    for k in range(1, 6):
        be2 = _with(be2, HEAD, k, delivered_pct=99.5)
    _t, rc, _r, _o = _run(four(s1_4m_be=be2))
    check("BE delivery 0.5 point lower: PASS", rc == 0)
    print("case VOID (must VOID): treatment, control, incomplete, out of order, runs")
    for what, libs_text, word in (
            ("head imports sendto (the library build)",
             _lib_edit("libc_net=epoll_pwait2,recvmsg,sendmmsg", "libc_net=epoll_pwait2,recvmsg,sendmmsg,sendto"),
             "TREATMENT NOT APPLIED"),
            ("head built from another commit", _lib_edit(f"arm=tickle@head commit={B_SELF}",
                                                         f"arm=tickle@head commit={'c' * 40}"), "TREATMENT NOT APPLIED"),
            ("head nm unread", _lib_edit("libc_net=epoll_pwait2,recvmsg,sendmmsg", "libc_net=NOT_READ"),
             "TREATMENT NOT READ"),
            ("head nm read an empty list (no positive control)",
             _lib_edit("libc_net=epoll_pwait2,recvmsg,sendmmsg", "libc_net=-"), "positive control"),
            ("pre without sendto (not A)", _lib_edit("recvmsg,sendmmsg,sendmsg,sendto", "recvmsg,sendmmsg,sendmsg"),
             "TREATMENT: pre"),
            ("pre built from another commit", _lib_edit(f"commit={PRE_COMMIT}", f"commit={B_SELF}"), "TREATMENT: pre"),
            ("one host only", "\n".join(x for x in LIBS_OK.splitlines() if "10.1.1.213" not in x), "1 host"),
            ("no libs.txt", "", "no requested head"),
            ("head and pre one library", LIBS_OK.replace("b" * 64, "a" * 64), "one library"),
            ("requested head is A", LIBS_OK.replace(f"requested head={B_SELF}", "requested head=723ec1f0"), "is A")):
        text, rc, res, out = _run(four(), libs={"s1_1m_rel": libs_text})
        check(f"{what}: VOID, NO VERDICT", bool(res["s1_1m_rel"]["void"]) and word in out and rc == 2)
    drifted = _cell(fd=(2.10, 2.10, 2.20, 2.30, 2.30))
    text, rc, res, out = _run(four(s1_1m_be=drifted))
    check("fastdds drift 0.20: VOID", rc == 2 and "CONTROL: fastdds drifted" in out)
    steady = _cell(fd=(2.10, 2.10, 2.12, 2.15, 2.15))
    _t, rc, _r, _o = _run(four(s1_1m_be=steady))
    check("fastdds drift 0.05 (control): read, PASS", rc == 0)
    inc = _with(_cell(), PRE, 1, incomplete=True, lost=3)
    _t, rc, res, _o = _run(four(s1_4m_rel=inc))
    check("RELIABLE pre run incomplete -> VOID", rc == 2 and "delivered less" in " ".join(res["s1_4m_rel"]["void"]))
    inc_be = _with(_cell(), PRE, 1, incomplete=True, lost=3)
    _t, rc, res, _o = _run(four(s1_4m_be=inc_be))
    check("BEST_EFFORT run incomplete: not a VOID", rc == 0)
    ooo = _with(_cell(), HEAD, 3, ooo=2)
    _t, rc, res, _o = _run(four(s1_1m_be=ooo))
    check("out_of_order on head -> VOID", rc == 2 and "out_of_order=2" in " ".join(res["s1_1m_be"]["void"]))
    few = _cell(hd_sub=(2.30, 2.31))
    _t, rc, res, _o = _run(four(s1_4m_rel=few))
    check("2 head runs -> VOID", rc == 2 and "fewer than 3" in " ".join(res["s1_4m_rel"]["void"]))
    print("case FAIL beside a VOID cell")
    text, rc, _r, _o = _run(four(s1_1m_rel=sub, s1_4m_rel=few))
    check("a WORSE in a read cell is FAIL even with another VOID", rc == 1 and "s1_4m_rel VOID" in text)
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
