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

STEP 3 - the rmw-level effect of C, bracketed: A1 (step 1's best_effort cells), C, A2 (A rebuilt and re-run after C), all
  on the same Pi with the same cells and reps. Per rmw_tickle run, from the summary's per-run lines (VOID runs dropped):
    tput/<topic>/best_effort  delivered/s (higher better), lat ms (perf_test's mean age of a delivered sample; lower)
    rtt/<msg>/best_effort/<wait>  p50 us (lower)
  TREATMENT  C's counter expression (ab_drain.sh's TREATMENT_C_COUNTER, e.g. rx_shm_skipped_superseded>0), summed over
             every "name=N" in the subscriber's sub.log, must hold in EVERY C tput run, and must NOT hold in any A run
             (absent reads 0 there: A has no such counter). A C run whose sub.log has no such field -> NO VERDICT;
             a C run where it fails, or an A run where it holds -> VOID.
  CONTROL    A1 against A2, the same build twice: if any primary moves beyond plain 2 x SE between them the Pi drifted
             and that metric's verdict is VOID (its C delta cannot be told from the drift).
  PRIMARY, C against A (A1 and A2 pooled), t = (C - A) / sqrt(SE_A^2 + SE_C^2):
     delivered/s of tput Array1k and Array4k best_effort    must be IMPROVED at the Bonferroni z for k = #primaries
     lat ms of tput Array1k and Array4k best_effort         must be HELD: within plain 2 x SE of A (the coordinator's
                                                            rule for the overload trade-off; 132cd136 broke it,
                                                            0.003 -> 1.4 ms on the PC)
     rtt p50 of every best_effort rtt cell                  must not be WORSE at the Bonferroni z
  VERDICT  VOID > NO VERDICT (fewer than 2 runs in an arm, treatment not checkable) > WORSE (a lat beyond 2 x SE above
           A, or an rtt p50 worse) > NO GAIN (nothing worse, a delivered/s not IMPROVED) > IMPROVED (all three hold).
  Expectation (what would falsify the change): delivered/s rises several-fold (PC, 132cd136: 61k -> 367k/s) with the
  latency held; a delivered/s that does not rise, or a lat that rises, falsifies "skip to newest" as the fix.

Usage:  ab_drain.py prereg | step1 OUT | step3 A1_OUT C_OUT A2_OUT COUNTER_EXPR | selftest FIXTURE_OUT
        ab_drain.py pfwitness PREFLIGHT_DIR present|absent | s6counter PREFIX SHA8 NAME | check-counter EXPR
Exit status: 0 CONFIRMED/IMPROVED, 1 NOT CONFIRMED/WORSE/NO GAIN, 3 NO VERDICT, 4 VOID, 2 usage.
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ab_compare import bonferroni_z, judge, stats  # noqa: E402

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
            if d:
                vals["delivered_per_s"] = float(d.group(1).replace(",", ""))
            if lat:
                vals["lat_ms"] = float(lat.group(1))
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


def step3(a1, c, a2, expr):
    name, holds = counter_check(expr)
    tails = {k: summary_tail(o) for k, o in (("A1", a1), ("C", c), ("A2", a2))}
    if any(t is None for t in tails.values()):
        print(f"STEP3 VERDICT: NO VERDICT - an arm did not finish: {[k for k, t in tails.items() if t is None]}")
        return "NO VERDICT"
    data = {}  # (cell, metric) -> {arm: [values]}
    for arm, tail in tails.items():
        for _stem, kind, cell, _rep, vals in runs(tail):
            if "/best_effort" not in cell:
                continue
            for met, v in vals.items():
                data.setdefault((f"{kind}/{cell}", met), {}).setdefault(arm, []).append(v)
    # Treatment, every tput best_effort run of every arm.
    treat = "ok"
    for arm, out in (("A1", a1), ("C", c), ("A2", a2)):
        for d in sorted(glob.glob(out + ".runs/tput_*_best_effort_tickle_r*")):
            n = counter_sum(d, name)
            shown = "absent" if n is None else n
            if arm == "C":
                if n is None:
                    print(f"  TREATMENT C {os.path.basename(d)}: {name} absent - could not look")
                    treat = "NO VERDICT" if treat == "ok" else treat
                elif not holds(n):
                    print(f"  TREATMENT C {os.path.basename(d)}: {name}={n} fails {expr} - the change did not act")
                    treat = "VOID"
                else:
                    print(f"  treatment C {os.path.basename(d)}: {name}={n} ({expr} holds)")
            elif holds(n or 0):
                print(f"  TREATMENT {arm} {os.path.basename(d)}: {name}={shown} holds {expr} in the base arm")
                treat = "VOID"
    if not any(glob.glob(c + ".runs/tput_*_best_effort_tickle_r*")):
        print("  TREATMENT C: no tput best_effort run directory - could not look")
        treat = "NO VERDICT" if treat == "ok" else treat
    prim = [k for k in sorted(data) if k[1] in ("delivered_per_s", "lat_ms", "p50_us")]
    z = bonferroni_z(len(prim))
    print(f"step 3: C against A1+A2 pooled; {len(prim)} primaries, Bonferroni |t| > {z:.2f}; lat held at plain 2 x SE;"
          f" control A2 - A1 at plain 2 x SE")
    verdicts = []
    for key in prim:
        cell, met = key
        arms = data[key]
        s1, s2, sc = stats(arms.get("A1", [])), stats(arms.get("A2", [])), stats(arms.get("C", []))
        if not s1 or not s2 or not sc or min(s1[2], s2[2], sc[2]) < 2:
            print(f"  {cell} {met}: NO VERDICT - n A1/C/A2 = {[len(arms.get(x, [])) for x in ('A1', 'C', 'A2')]}")
            verdicts.append("NO VERDICT")
            continue
        hi = met == "delivered_per_s"
        _, tc, _ = judge(s1, s2, hi, 2.0)
        sa = stats(arms["A1"] + arms["A2"])
        diff, t, how = judge(sa, sc, hi, z if met != "lat_ms" else 2.0)
        line = (f"  {cell} {met}: A1 {s1[0]:.4g} A2 {s2[0]:.4g} (control t {tc:+.2f})  C {sc[0]:.4g}  "
                f"C-A {diff:+.4g} t {t:+.2f}")
        if abs(tc) > 2.0:
            v = "VOID"
            line += "  -> VOID (A1 and A2 differ: the Pi drifted)"
        elif met == "delivered_per_s":
            v = "IMPROVED" if how == "better" else ("WORSE" if how == "WORSE" else "NO GAIN")
            line += f"  -> {v}"
        elif met == "lat_ms":
            v = "WORSE" if how == "WORSE" else "IMPROVED"
            line += "  -> " + ("WORSE (above A beyond 2 x SE)" if v == "WORSE" else "HELD")
        else:
            v = "WORSE" if how == "WORSE" else "IMPROVED"
            line += "  -> " + ("WORSE" if v == "WORSE" else "HELD")
        print(line)
        verdicts.append(v)
    if treat != "ok":
        verdicts.append(treat)
    if not any(k[1] == "delivered_per_s" for k in prim) or not any(k[1] == "lat_ms" for k in prim):
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
    v3 = step3(fixture, fixture, fixture, "rx_shm_skipped_superseded>0")
    ok &= v3 == "NO VERDICT"
    # A treatment the fixture DOES carry (rx_shm=, the subscriber's traffic line): then the base arms hold it too,
    # which must VOID - the A-side guard deciding.
    v3b = step3(fixture, fixture, fixture, "rx_shm>0")
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
    if len(argv) == 5 and argv[0] == "step3":
        return EXIT[step3(*argv[1:5])]
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
