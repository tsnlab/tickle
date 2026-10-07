#!/usr/bin/env python3
"""ab_drain.py - the reading of ab_drain.sh's rmw steps (1 and 3). Step 2, the native A/B, is read by ab_samehost.py.

Every rule below is implemented in this file, not only stated; ab_drain.sh prints `prereg` into the driver log and
writes it to $OUTB.prereg.txt before the lock is taken.

STEP 1 - the stall fix confirmed (rmw_samehost.sh at A, every cell, rmw_tickle only). Read from <OUT>.txt (the summary
  rmw_samehost.sh appends after "=== runs done") and <OUT>.runs/rtt_*_tickle_r*/ping.log:
    VOID runs     every rmw_tickle run the summary lists as VOID, by cell; a block-wait rtt run among them is the stall
    STALE         "STALE: replies_after_deadline=N" in every rmw_tickle rtt run's ping.log (912eae0e prints it)
  CONFIRMED      no rmw_tickle block-wait rtt run is VOID, every rtt run printed its STALE line, and every one says 0
  NOT CONFIRMED  a block-wait rtt run is VOID, or a STALE count is > 0
  NO VERDICT     no "=== runs done" (the run did not finish), no block-wait rtt run at all, or a STALE line missing
                 ("could not look" is not "0")
  Other VOID rmw_tickle runs (poll or tput) are listed and counted; they do not decide this step.

STEP 3 - the rmw-level effect of C, in interleaved phases (since 2026-10-07 16:40; the first run's A1 C A2 bracket VOIDed
  on Array4k because A1 and A2 differed by 12% at t -2.95 while C was +480%, and its lat read WORSE 0.004 -> 0.006 ms
  with SE 0 because the summary printed lat to 1 us). ab_drain.sh runs rmw_samehost.sh, best_effort cells, rmw_tickle
  only, once per phase in RMW_PHASES order (default A C C A C A A C: balanced against a linear and a quadratic drift -
  the A and C positions have equal sums and equal sums of squares). Per rmw_tickle run, from the summary's per-run
  lines (VOID runs dropped):
    tput/<topic>/best_effort  delivered/s (higher better), lat_us (perf_test's mean latency of a delivered sample,
                              printed to 1 ns by rmw_samehost_summary.py since this change; lower better)
    rtt/<msg>/best_effort/<wait>  p50 us (lower)
  THE UNIT IS A PHASE: each metric's mean over that phase's runs. C phases against A phases, pooled-variance t with
  df = phases - 2. A drift of the Pi between phases is therefore in the variance the verdict is judged against, and
  the balanced order keeps a linear or quadratic drift out of the difference itself - so drift no longer VOIDs; it
  makes a small effect undecidable, as it should. The thresholds are Student t at df, not normal z.
  TREATMENT  C's counter expression (ab_drain.sh's TREATMENT_C_COUNTER, e.g. rx_shm_skipped_superseded>0), summed over
             every "name=N" in the subscriber's sub.log, must hold in EVERY C tput run, and must NOT hold in any A run
             (absent reads 0 there: A has no such counter). A C run whose sub.log has no such field -> NO VERDICT;
             a C run where it fails, or an A run where it holds -> VOID.
  PRIMARY, C phases against A phases:
     delivered/s of tput Array1k and Array4k best_effort    must be IMPROVED at the Bonferroni t (k = delivered + rtt)
     lat_us of tput Array1k and Array4k best_effort         must be HELD: not above A beyond the plain (k = 1) t (the
                                                            coordinator's rule for the overload trade-off; 132cd136
                                                            broke it, 0.003 -> 1.4 ms on the PC). A run printed without
                                                            lat_us (1 us resolution) is NO VERDICT for lat, not "held".
     rtt p50 of every best_effort rtt cell                  must not be WORSE at the Bonferroni t
  VERDICT  VOID > NO VERDICT (fewer than 2 phases with a value in an arm, treatment not checkable, lat too coarse) >
           WORSE (a lat above A, or an rtt p50 worse) > NO GAIN (nothing worse, a delivered/s not IMPROVED) > IMPROVED.
  Expectation (what would falsify the change): delivered/s rises several-fold (PC, 132cd136: 61k -> 367k/s; rig
  2026-10-07: +97% Array1k, +480% Array4k) with the latency held; a delivered/s that does not rise, or a lat that
  rises, falsifies "skip to newest" as the fix.

Usage:  ab_drain.py prereg | step1 OUT | step3 COUNTER_EXPR A=OUT C=OUT C=OUT A=OUT ... | selftest FIXTURE_OUT
        ab_drain.py pfwitness PREFLIGHT_DIR present|absent | s6counter PREFIX SHA8 NAME | check-counter EXPR
Exit status: 0 CONFIRMED/IMPROVED, 1 NOT CONFIRMED/WORSE/NO GAIN, 3 NO VERDICT, 4 VOID, 2 usage.
"""
import glob
import math
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ab_compare import bonferroni_t  # noqa: E402

EXIT = {"CONFIRMED": 0, "IMPROVED": 0, "NOT CONFIRMED": 1, "WORSE": 1, "NO GAIN": 1, "NO VERDICT": 3, "VOID": 4}
ORDER = ("VOID", "NO VERDICT", "WORSE", "NO GAIN", "IMPROVED")


def summary_tail(out):
    """The part of <OUT>.txt the real run's summary wrote (after '=== runs done'), or None if it never got there."""
    try:
        text = open(out + ".txt", errors="replace").read()
    except OSError:
        return None
    i = text.rfind("=== runs done")
    return None if i < 0 else text[i:]


def void_stems(tail):
    stems = {}
    sec = re.search(r"^VOID runs:\n((?:  .*\n?)*)", tail, re.M)
    if sec:
        for line in sec.group(1).splitlines():
            m = re.match(r"\s+(\S+): (.*)", line)
            if m:
                stems[m.group(1)] = m.group(2)
    return stems


RUN_LINE = re.compile(r"^(?P<kind>rtt|tput)/(?P<cell>\S+) (?P<arm>\S+)\s+r(?P<rep>\d+) (?P<rest>.*)$", re.M)


def runs(tail, arm="tickle"):
    """[(stem, kind, cell, rep, {metric: value})] for one arm's per-run lines; VOID runs dropped."""
    voids = void_stems(tail)
    got = []
    for m in RUN_LINE.finditer(tail):
        if m["arm"] != arm:
            continue
        kind, cell, rest = m["kind"], m["cell"], m["rest"]
        stem = f"{kind}_{cell.replace('/', '_')}_{arm}_r{m['rep']}"
        if stem in voids:
            continue
        vals = {}
        if kind == "tput":
            d = re.search(r"delivered ([\d,]+)/s", rest)
            lat = re.search(r"\blat ([\d.]+) ms", rest)
            lat_us = re.search(r"\blat_us ([\d.]+)", rest)
            if d:
                vals["delivered_per_s"] = float(d.group(1).replace(",", ""))
            if lat:
                vals["lat_ms"] = float(lat.group(1))
            if lat_us:
                vals["lat_us"] = float(lat_us.group(1))
        else:
            p50 = re.search(r"\bp50 ([\d.]+)", rest)
            if p50:
                vals["p50_us"] = float(p50.group(1))
        got.append((stem, kind, cell, int(m["rep"]), vals))
    return got


# --------------------------------------------------------------------------------------------------- step 1
def step1(out):
    tail = summary_tail(out)
    if tail is None:
        print(f"STEP1 VERDICT: NO VERDICT - {out}.txt has no '=== runs done': the run did not finish")
        return "NO VERDICT"
    voids = {s: r for s, r in void_stems(tail).items() if re.search(r"_tickle_r\d+$", s)}
    block_voids = {s: r for s, r in voids.items() if s.startswith("rtt_") and "_block_" in s}
    rtt_dirs = sorted(glob.glob(out + ".runs/rtt_*_tickle_r*"))
    block_dirs = [d for d in rtt_dirs if "_block_" in os.path.basename(d)]
    stale, missing = {}, []
    for d in rtt_dirs:
        m = None
        try:
            m = re.search(r"STALE: replies_after_deadline=(\d+)", open(os.path.join(d, "ping.log"), errors="replace").read())
        except OSError:
            pass
        if m:
            stale[os.path.basename(d)] = int(m.group(1))
        else:
            missing.append(os.path.basename(d))
    print(f"step 1: {len(rtt_dirs)} rmw_tickle rtt runs ({len(block_dirs)} block-wait); {len(voids)} rmw_tickle runs VOID")
    for s, r in sorted(voids.items()):
        print(f"  VOID {s}: {r}")
    for s, v in sorted(stale.items()):
        if v:
            print(f"  STALE {s}: replies_after_deadline={v}")
    for s in missing:
        print(f"  NO STALE LINE {s} (could not look)")
    if block_voids or any(stale.values()):
        v = "NOT CONFIRMED"
        why = f"{len(block_voids)} block-wait rtt runs VOID, {sum(1 for x in stale.values() if x)} runs with late replies"
    elif not block_dirs or missing:
        v = "NO VERDICT"
        why = "no block-wait rtt run" if not block_dirs else f"{len(missing)} rtt runs printed no STALE line"
    else:
        v = "CONFIRMED"
        why = f"0 of {len(block_dirs)} block-wait rtt runs VOID, replies_after_deadline=0 in all {len(stale)} rtt runs"
    print(f"STEP1 VERDICT: {v} - {why}")
    return v


# --------------------------------------------------------------------------------------------------- step 3
def counter_check(expr):
    m = re.fullmatch(r"\s*([A-Za-z_]\w*)\s*(>=|<=|==|!=|>|<)\s*(-?[\d.]+)\s*", expr)
    if not m:
        raise SystemExit(f"COUNTER_EXPR {expr!r}: want name<op>number, e.g. rx_shm_skipped_superseded>0")
    name, op, num = m.group(1), m.group(2), float(m.group(3))
    ops = {">": lambda a: a > num, ">=": lambda a: a >= num, "<": lambda a: a < num, "<=": lambda a: a <= num,
           "==": lambda a: a == num, "!=": lambda a: a != num}
    return name, ops[op]


def counter_sum(run_dir, name):
    """The sum of every name=N in the subscriber's log, or None if the field appears nowhere (could not look)."""
    try:
        text = open(os.path.join(run_dir, "sub.log"), errors="replace").read()
    except OSError:
        return None
    vals = [int(v) for v in re.findall(r"\b" + re.escape(name) + r"=(\d+)", text)]
    return sum(vals) if vals else None


def step3(expr, phases):
    """phases: [(arm letter A or C, OUT stem)] in the order they ran. Read as the module docstring says."""
    name, holds = counter_check(expr)
    tails = [(arm, out, summary_tail(out)) for arm, out in phases]
    unfinished = [f"{arm}:{os.path.basename(out)}" for arm, out, t in tails if t is None]
    if unfinished:
        print(f"STEP3 VERDICT: NO VERDICT - phase(s) did not finish: {unfinished}")
        return "NO VERDICT"
    # (cell, metric) -> {arm: [phase means]}, and the per-phase means in run order for the drift line.
    data, order = {}, {}
    lat_coarse = set()
    for i, (arm, _out, tail) in enumerate(tails, 1):
        per = {}
        for _stem, kind, cell, _rep, vals in runs(tail):
            if "/best_effort" not in cell:
                continue
            if "lat_ms" in vals and "lat_us" not in vals:
                lat_coarse.add(f"{kind}/{cell}")
            for met, v in vals.items():
                if met != "lat_ms":
                    per.setdefault((f"{kind}/{cell}", met), []).append(v)
        for key, vs in per.items():
            m = statistics.fmean(vs)
            data.setdefault(key, {}).setdefault(arm, []).append(m)
            order.setdefault(key, []).append(f"{arm}{i} {m:.4g}")
    # Treatment, every tput best_effort run of every phase.
    treat = "ok"
    for arm, out, _ in tails:
        for d in sorted(glob.glob(out + ".runs/tput_*_best_effort_tickle_r*")):
            n = counter_sum(d, name)
            shown = "absent" if n is None else n
            if arm == "C":
                if n is None:
                    print(f"  TREATMENT C {os.path.basename(out)}/{os.path.basename(d)}: {name} absent - could not look")
                    treat = "NO VERDICT" if treat == "ok" else treat
                elif not holds(n):
                    print(f"  TREATMENT C {os.path.basename(out)}/{os.path.basename(d)}: {name}={n} fails {expr}")
                    treat = "VOID"
                else:
                    print(f"  treatment C {os.path.basename(out)}/{os.path.basename(d)}: {name}={n} ({expr} holds)")
            elif holds(n or 0):
                print(f"  TREATMENT {arm} {os.path.basename(out)}/{os.path.basename(d)}: {name}={shown} holds {expr} "
                      "in the base arm")
                treat = "VOID"
    for arm, out, _ in tails:
        if arm == "C" and not glob.glob(out + ".runs/tput_*_best_effort_tickle_r*"):
            print(f"  TREATMENT C {os.path.basename(out)}: no tput best_effort run directory - could not look")
            treat = "NO VERDICT" if treat == "ok" else treat
    n_a = sum(1 for a, _ in phases if a == "A")
    n_c = sum(1 for a, _ in phases if a == "C")
    df = n_a + n_c - 2
    prim = [k for k in sorted(data) if k[1] in ("delivered_per_s", "lat_us", "p50_us")]
    k_dec = sum(1 for k in prim if k[1] != "lat_us")
    t_dec, t_lat = (bonferroni_t(k_dec, df), bonferroni_t(1, df)) if df > 0 else (math.inf, math.inf)
    print(f"step 3: phases {' '.join(a for a, _ in phases)}; the unit is a phase (its runs' mean), C phases against A "
          f"phases, pooled-variance t with df = {df}: delivered/s and rtt p50 at Bonferroni |t| > {t_dec:.2f} "
          f"(k = {k_dec}), lat at |t| > {t_lat:.2f}; drift between phases is in the variance, not a VOID")
    verdicts = []
    for key in prim:
        cell, met = key
        arms = data[key]
        a, c = arms.get("A", []), arms.get("C", [])
        if len(a) < 2 or len(c) < 2:
            print(f"  {cell} {met}: NO VERDICT - phases with a value A {len(a)} C {len(c)} (2 each needed)")
            verdicts.append("NO VERDICT")
            continue
        ma, mc = statistics.fmean(a), statistics.fmean(c)
        sp2 = ((len(a) - 1) * statistics.variance(a) + (len(c) - 1) * statistics.variance(c)) / (len(a) + len(c) - 2)
        se = math.sqrt(sp2 * (1 / len(a) + 1 / len(c)))
        diff = mc - ma
        t = diff / se if se else (0.0 if diff == 0 else math.copysign(math.inf, diff))
        line = (f"  {cell} {met}: A {ma:.4g} C {mc:.4g}  C-A {diff:+.4g} ({100 * diff / ma if ma else float('nan'):+.1f}%)"
                f" t {t:+.2f}  [phases: {' '.join(order[key])}]")
        if met == "delivered_per_s":
            v = "IMPROVED" if t > t_dec else ("WORSE" if t < -t_dec else "NO GAIN")
            line += f"  -> {v}"
        elif met == "lat_us":
            v = "WORSE" if t > t_lat else "IMPROVED"
            line += "  -> " + ("WORSE (above A)" if v == "WORSE" else "HELD")
        else:
            v = "WORSE" if t > t_dec else "IMPROVED"
            line += "  -> " + ("WORSE" if v == "WORSE" else "HELD")
        print(line)
        verdicts.append(v)
    for cell in sorted(lat_coarse):
        print(f"  {cell} lat: NO VERDICT - the summary printed lat at 1 us only (no lat_us): too coarse to hold")
        verdicts.append("NO VERDICT")
    if treat != "ok":
        verdicts.append(treat)
    if not any(k[1] == "delivered_per_s" for k in prim) or (not any(k[1] == "lat_us" for k in prim)
                                                           and not lat_coarse):
        verdicts.append("NO VERDICT")
    overall = next((v for v in ORDER if v in verdicts), "NO VERDICT")
    print(f"STEP3 VERDICT: {overall} (treatment {treat})")
    return overall


def pfwitness(pf_dir, expect, name="rx_drain_ring_turns"):
    """The PC preflight's treatment check (before the lock): in every job of a rig_preflight.sh output, the sum of
    name=N over client.log and server.log must be > 0 (expect=present: the arm carries the ring-turn rule and its
    witness) or the field must appear nowhere (expect=absent: the base). A job with no logs at all is "could not
    look" and fails either way."""
    jobs = sorted(glob.glob(os.path.join(pf_dir, "jobs", "*")))
    ok = bool(jobs)
    for j in jobs:
        texts = []
        for role in ("client.log", "server.log"):
            try:
                texts.append(open(os.path.join(j, role), errors="replace").read())
            except OSError:
                pass
        vals = [int(v) for t in texts for v in re.findall(r"\b" + re.escape(name) + r"=(\d+)", t)]
        if len(texts) < 2:
            good, what = False, "logs missing (could not look)"
        elif expect == "present":
            good, what = bool(vals) and sum(vals) > 0, f"{name} sum {sum(vals)} over {len(vals)} lines"
        else:
            good, what = not vals, f"{len(vals)} {name} fields"
        ok &= good
        print(f"  {'ok  ' if good else 'FAIL'} {os.path.basename(j)}: {what} (want {expect})")
    if not jobs:
        print(f"  FAIL no jobs under {pf_dir} (could not look)")
    return 0 if ok else 1


def s6counter(prefix, sha8, name):
    """Information, not a verdict: the C counter summed per cell over the native A/B's C files (server and client
    traffic lines). The native bench has no KEEP_LAST queue to supersede, so 0 there is expected and says only that
    the native cells measure C's cost, not its effect - which step 3 measures."""
    for f in sorted(glob.glob(f"{prefix}.b*_{sha8}_*.txt")):
        vals = [int(v) for v in re.findall(r"\b" + re.escape(name) + r"=(\d+)", open(f, errors="replace").read())]
        print(f"  {os.path.basename(f)}: {name} " + (f"sum {sum(vals)} over {len(vals)} fields" if vals else "absent"))
    return 0


def prereg():
    print(__doc__.split("Usage:")[0].rstrip())


def selftest(fixture):
    """On a finished pre-fix run (rmw_samehost 7e6fe171): step 1 must say NOT CONFIRMED (block VOIDs, no STALE line
    since that ping predates it), and step 3 with that run as all three arms must say NO VERDICT for C's treatment
    (the counter does not exist there) - the guards answer, not the defaults."""
    ok = True
    v1 = step1(fixture)
    ok &= v1 == "NOT CONFIRMED"
    phases = [(a, fixture) for a in "ACCA"]
    v3 = step3("rx_shm_skipped_superseded>0", phases)
    ok &= v3 == "NO VERDICT"
    # A treatment the fixture DOES carry (rx_shm=, the subscriber's traffic line): then the base arms hold it too,
    # which must VOID - the A-side guard deciding.
    v3b = step3("rx_shm>0", phases)
    ok &= v3b == "VOID"
    print(f"SELFTEST {'PASS' if ok else 'FAIL'}: step1 {v1} (want NOT CONFIRMED), step3 {v3} (want NO VERDICT),"
          f" step3 rx_shm {v3b} (want VOID)")
    return 0 if ok else 1


def main(argv):
    if len(argv) >= 1 and argv[0] == "prereg":
        prereg()
        return 0
    if len(argv) == 2 and argv[0] == "step1":
        return EXIT[step1(argv[1])]
    if len(argv) >= 6 and argv[0] == "step3":
        phases = []
        for spec in argv[2:]:
            arm, sep, out = spec.partition("=")
            if not sep or arm not in ("A", "C"):
                print(f"step3: {spec!r} is not A=<OUT> or C=<OUT>", file=sys.stderr)
                return 2
            phases.append((arm, out))
        return EXIT[step3(argv[1], phases)]
    if len(argv) == 2 and argv[0] == "selftest":
        return selftest(argv[1])
    if len(argv) == 3 and argv[0] == "pfwitness" and argv[2] in ("present", "absent"):
        return pfwitness(argv[1], argv[2])
    if len(argv) == 4 and argv[0] == "s6counter":
        return s6counter(*argv[1:4])
    if len(argv) == 2 and argv[0] == "check-counter":
        name, _ = counter_check(argv[1])
        print(name)
        return 0
    print(__doc__.split("Usage:")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
