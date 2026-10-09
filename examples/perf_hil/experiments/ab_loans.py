#!/usr/bin/env python3
"""ab_loans.py - the reading of ab_loans.sh: rmw loaned messages (build B) against main (build A), same-host.

What B changes, and what rclcpp does with it (read 2026-10-09 in jazzy's rcl and rclcpp, the rig's distribution, and
in lyrical's, this PC's; the PC preflight confirmed it): B sets can_loan_messages for every type whose wire bytes are
its message (Array1k, Array4k, the ping's Array1k). But
  - rclcpp's publish(const T &) never borrows a loan: only publish(LoanedMessage &&) does. perf_test and the ping
    publish by reference, so their publish path is rmw_publish() under A and B alike;
  - rcl leaves a subscription's disable_loaned_message TRUE unless ROS_DISABLE_LOANED_MESSAGES=0, so by default
    rclcpp's executor never calls rmw_take_loaned_message() either.
So B's DEFAULT path takes no loans; what reaches it there is B's other changes to the shared subscription code. Two
questions, each with its own rmw arm inside every phase (rmw_samehost.sh ARMS="tickle tickle_loans", order rotated):
  D (landing)   arm tickle, the shipped defaults:       B must not be WORSE than A  -> decides whether B lands
  L (opt-in)    arm tickle_loans, ROS_DISABLE_LOANED_MESSAGES=0, so rclcpp takes loans where can_loan is set:
                                                        B must not be WORSE than A  -> decides whether the opt-in may
                                                        be documented as costing nothing
                (A ignores the variable: its can_loan_messages is false, so rcl never takes a loan from it.)

Every rule below is implemented in this file; ab_loans.sh writes `prereg` to $OUTB.prereg.txt before the lock and logs
its sha256 under it.

THE RUNS. One rmw_samehost.sh run (one repetition per cell and arm) per PHASE, in PHASES order (default A B B A B A A B:
the A and B positions have equal sums and equal sums of squares, so a linear or quadratic drift of the Pi cancels out
of B - A). Each phase builds its commit on the Pi (rmw_samehost.sh's build), so B's typesupport regenerates the
interface packages and A's regenerates them back. Cells, both QoS each (best_effort KEEP_LAST 1, reliable KEEP_ALL):
    rtt  array1k block, array1k poll        loanable (Array1k: every field where the wire has it)
    tput Array1k, Array4k                   loanable
    tput RadarDetection                     CONTROL: not loanable (uint16 then a float64 struct: offset 8 in the C
                                            struct, 4 on the wire), so can_loan_messages is false under B too
Per run (VOID runs dropped, rmw_samehost_summary.py's rules): rtt p50 us; tput delivered/s and cpu us/sample.

THE UNIT IS A PHASE: each (rmw arm, cell, metric)'s mean over the phase. B phases against A phases, pooled-variance
Student t at df = phases - 2; drift between phases is variance, not a VOID.

PRIMARY, per question D and L (k = 12 each, Bonferroni within the question, two-sided t at df):
    rtt p50 of array1k best_effort/reliable x block/poll (4)         lower is better
    tput delivered/s of Array1k and Array4k best_effort/reliable (4)  higher is better
    tput cpu us/sample of the same four (4)                          lower is better
  WORSE when B is worse beyond the threshold, IMPROVED when better beyond it, else PASS - and beside every PASS the
  smallest worsening that would have read WORSE (threshold x SE, % of A): a PASS that cannot rule out 10% says WEAK.
CONTROL 1, the cell B cannot reach (RadarDetection best_effort and reliable, delivered/s and cpu us/sample, both rmw
  arms: k = 8): a move beyond the threshold EITHER way is VOID - the phases then differ by something other than B's
  loanable path (drift the order did not cancel, or B's changes to the shared subscription queue), so the loanable
  cells' difference cannot be read as that path's.
CONTROL 2, the treatment variable where it can do nothing: in A phases, arm tickle_loans against arm tickle, paired by
  phase (df = A phases - 1), the 12 primary metrics at Bonferroni k = 12: a move beyond the threshold is VOID (the
  arm order or the variable itself moves the figures, so L's difference is not the loans').
TREATMENT, counted by rmw_tickle at teardown (stderr; "loans_in_place= loans_copied=" per subscription that took a loan,
  "loans_published=" per publisher that published one), summed over every log of a run directory, and the variable as
  each process received it (the cell's "treat role=... env=" lines, read from /proc/PID/environ):
    every tickle_loans run       ROS_DISABLE_LOANED_MESSAGES=0 in every process, or VOID
    every tickle run             no ROS_DISABLE_LOANED_MESSAGES in any process, or VOID
    B, tickle_loans, loanable    taken (in_place + copied) > 0 in EVERY run, or VOID
    A, any run; control cell     no loan field at all, or VOID (a loan there means the wrong build or a wrong cell)
    B, tickle, loanable          information only: whether the default path took or published loans (expected: no)
  A run with no logs or no treat line is "could not look": NO VERDICT, never "0".

VERDICT per question: VOID > NO VERDICT > WORSE > IMPROVED > PASS. The VOIDs of the controls and the treatment apply
to both. B lands on D = PASS or IMPROVED; L is reported beside it.
Expectation: D PASS (the default path is A's apart from the subscription queue's bookkeeping); L PASS, perhaps a small
cpu/sample gain (a decoded shell lent instead of copied into the application's message). A D WORSE falsifies "B costs
the default path nothing" and B does not land as is.

MODE zcl (AB_LOANS_MODE=zcl; ab/zero-copy-loans, 2026-10-09). Both builds lend (main has had loans since 7a321d26), and
B adds the 8-aligned rx_buffer and the claimed-slot publish (core's tt_Publisher_claim(); rmw_tickle's loans built in
the ring slot with RMW_TICKLE_LOAN_PUBLISH_SLOTS=1). Arm tickle_loans then opts into every loan: loaned takes
(ROS_DISABLE_LOANED_MESSAGES=0), loaned publishes in the ping/pong nodes (PINGPONG_LOANED_PUBLISH=1: they borrow and
fill a loan where the rmw can lend the type; perf_test never borrows) and slot loans (RMW_TICKLE_LOAN_PUBLISH_SLOTS=1,
which A's rmw ignores). So L asks: with every loan opted into, does B's zero-copy path beat A's one-copy loans? D
asks what it always asks. The rules that change:
  TREATMENT  every tickle_loans run: the three variables as above in every process; every tickle run: none of them.
             A, any run: no loans_in_slot field (A's rmw cannot print one). B, tickle_loans, an rtt cell: loans_in_slot
             > 0 in EVERY run (the ping and pong built their messages in the slots), or VOID; a tput cell: taken > 0,
             as before. B, tickle: no loans_in_slot (slots are off by default), or VOID. The control cell: no loan
             field at all, as before.
  CONTROL 2  not applicable - A's tickle_loans arm borrows too, so it is no longer an arm the variable cannot touch -
             and printed as such. Control 1 (RadarDetection, both arms) is unchanged and is the only control.
Expectation in zcl: D PASS; L IMPROVED or PASS on the rtt cells' p50 (one 1 KB copy fewer per publish, both ends),
PASS on tput (perf_test borrows nothing; its takes read in place as on main, the ring path decodes as on main).

Usage:  ab_loans.py prereg | read A=OUT B=OUT ... | witness RUNS_DIR A|B | selftest
Exit status: 0 D PASS/IMPROVED, 1 D WORSE, 3 NO VERDICT, 4 VOID, 2 usage.
"""
import glob
import math
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ab_compare import bonferroni_t  # noqa: E402
from ab_drain import summary_tail, void_stems  # noqa: E402

EXIT = {"PASS": 0, "IMPROVED": 0, "WORSE": 1, "NO VERDICT": 3, "VOID": 4}
ORDER = ("VOID", "NO VERDICT", "WORSE", "IMPROVED", "PASS")
CONTROL_TOPIC = "RadarDetection"
RMW_ARMS = ("tickle", "tickle_loans")
QUESTION = {"tickle": "D", "tickle_loans": "L"}
QOSES = ("best_effort", "reliable")
PRIMARY = ([(f"rtt/array1k/{q}/{w}", "p50_us") for q in QOSES for w in ("block", "poll")] +
           [(f"tput/{t}/{q}", m) for t in ("Array1k", "Array4k") for q in QOSES for m in ("delivered_per_s", "cpu_us")])
CONTROL = [(f"tput/{CONTROL_TOPIC}/{q}", m) for q in QOSES for m in ("delivered_per_s", "cpu_us")]
HIGHER_BETTER = {"delivered_per_s"}
WEAK_PCT = 10.0
ENV_VAR = "ROS_DISABLE_LOANED_MESSAGES"
ZCL = os.environ.get("AB_LOANS_MODE", "") == "zcl"
# The variables arm tickle_loans sets in mode zcl, besides ENV_VAR, and the value it sets them to.
ZCL_VARS = {"RMW_TICKLE_LOAN_PUBLISH_SLOTS": "1", "PINGPONG_LOANED_PUBLISH": "1"}

RUN_LINE = re.compile(r"^(?P<kind>rtt|tput)/(?P<cell>\S+) (?P<arm>\S+)\s+r(?P<rep>\d+) (?P<rest>.*)$", re.M)
LOAN_FIELDS = ("loans_in_place", "loans_copied", "loans_published", "loans_in_slot")


def runs(tail):
    """[(rmw arm, 'kind/cell', {metric: value})] for the rmw_tickle arms' per-run lines; VOID runs dropped."""
    voids = void_stems(tail)
    got = []
    for m in RUN_LINE.finditer(tail):
        if m["arm"] not in RMW_ARMS:
            continue
        kind, cell, rest = m["kind"], m["cell"], m["rest"]
        if f"{kind}_{cell.replace('/', '_')}_{m['arm']}_r{m['rep']}" in voids:
            continue
        vals = {}
        if kind == "tput":
            d = re.search(r"delivered ([\d,]+)/s", rest)
            c = re.search(r"\bcpu ([\d.]+) us/sample", rest)
            if d:
                vals["delivered_per_s"] = float(d.group(1).replace(",", ""))
            if c:
                vals["cpu_us"] = float(c.group(1))
        else:
            p50 = re.search(r"\bp50 ([\d.]+)", rest)
            if p50:
                vals["p50_us"] = float(p50.group(1))
        got.append((m["arm"], f"{kind}/{cell}", vals))
    return got


def loan_counts(run_dir):
    """{field: sum} over every *.log of one run directory, or None when it holds no log (could not look). A field that
    appears nowhere is absent: rmw_tickle prints its line only when a count is > 0."""
    logs = sorted(glob.glob(os.path.join(run_dir, "*.log")))
    if not logs:
        return None
    sums = {}
    for f in logs:
        text = open(f, errors="replace").read()
        for name in LOAN_FIELDS:
            for v in re.findall(r"\b" + name + r"=(\d+)", text):
                sums[name] = sums.get(name, 0) + int(v)
    return sums


def env_values(run_dir, var=ENV_VAR):
    """[the variable's value or None, per process the cell read], or None when there is no treat line."""
    try:
        meta = open(os.path.join(run_dir, "meta.txt"), errors="replace").read()
    except OSError:
        return None
    lines = re.findall(r"^treat role=\S+ env=(.*)$", meta, re.M)
    if not lines:
        return None
    out = []
    for line in lines:
        m = re.search(r"(?:^|;)" + var + r"=([^;]*)", line)
        out.append(m.group(1) if m else None)
    return out


def zcl_env_problem(run_dir, arm):
    """Mode zcl: the variables beside ENV_VAR as each process received them - None when as the arm sets them, else
    what was wrong (a missing treat line is env_values()' None, reported by the caller already)."""
    for var, want in ZCL_VARS.items():
        got = env_values(run_dir, var) or []
        expect = want if arm == "tickle_loans" else None
        if any(v != expect for v in got):
            return f"{var} as received {got}, want {expect} in every process"
    return None


def rmw_arm_of(stem):
    m = re.search(r"_(tickle_loans|tickle)_r\d+$", stem)
    return m.group(1) if m else None


def treatment_of_run(build, stem, counts, envs, zcl_env=None):
    """(state ok|VOID|NO VERDICT|info, text) for one run of build A or B. zcl_env: zcl_env_problem()'s answer."""
    arm = rmw_arm_of(stem)
    if counts is None or envs is None:
        return "NO VERDICT", "no logs or no treat line (could not look)"
    want_env = "0" if arm == "tickle_loans" else None
    if any(v != want_env for v in envs):
        return "VOID", f"{ENV_VAR} as received {envs}, want {want_env} in every process"
    if ZCL and zcl_env:
        return "VOID", zcl_env
    taken = counts.get("loans_in_place", 0) + counts.get("loans_copied", 0)
    published = counts.get("loans_published", 0)
    in_slot = counts.get("loans_in_slot", 0)
    shown = (f"taken {taken} (in_place {counts.get('loans_in_place', 0)} copied {counts.get('loans_copied', 0)}) "
             f"published {published}{f' in_slot {in_slot}' if ZCL else ''}; env {envs}")
    if f"_{CONTROL_TOPIC}_" in stem or (build == "A" and not ZCL):
        return ("VOID" if counts else "ok"), f"{shown} (want no loan field)"
    if ZCL:
        if build == "A":
            return ("VOID" if "loans_in_slot" in counts else "ok"), f"{shown} (want no loans_in_slot field)"
        if arm == "tickle":
            return ("VOID" if in_slot else "info"), f"{shown} (default path: want no slot loan)"
        if stem.startswith("rtt_"):
            return ("ok" if in_slot > 0 else "VOID"), f"{shown} (want in_slot > 0)"
    if arm == "tickle_loans":
        return ("ok" if taken > 0 else "VOID"), f"{shown} (want taken > 0)"
    return "info", f"{shown} (default path; information)"


def phase_t(a, b):
    ma, mb = statistics.fmean(a), statistics.fmean(b)
    sp2 = ((len(a) - 1) * statistics.variance(a) + (len(b) - 1) * statistics.variance(b)) / (len(a) + len(b) - 2)
    se = math.sqrt(sp2 * (1 / len(a) + 1 / len(b)))
    d = mb - ma
    t = d / se if se else (0.0 if d == 0 else math.copysign(math.inf, d))
    return ma, mb, d, se, t


def paired_t(xs, ys):
    ds = [y - x for x, y in zip(xs, ys)]
    md = statistics.fmean(ds)
    se = statistics.stdev(ds) / math.sqrt(len(ds)) if len(ds) > 1 else 0.0
    t = md / se if se else (0.0 if md == 0 else math.copysign(math.inf, md))
    return md, se, t


def read(phases):
    """phases: [(build A or B, OUT stem)] in the order they ran. Returns (D verdict, L verdict)."""
    tails = [(build, out, summary_tail(out)) for build, out in phases]
    unfinished = [f"{b}:{os.path.basename(o)}" for b, o, t in tails if t is None]
    if unfinished:
        print(f"VERDICT D: NO VERDICT\nVERDICT L: NO VERDICT - phase(s) did not finish: {unfinished}")
        return "NO VERDICT", "NO VERDICT"
    data, order, a_pairs = {}, {}, {}
    treat = "ok"
    default_loans = []
    for i, (build, out, tail) in enumerate(tails, 1):
        per = {}
        for arm, cell, vals in runs(tail):
            for met, v in vals.items():
                per.setdefault((arm, cell, met), []).append(v)
        for key, vs in per.items():
            m = statistics.fmean(vs)
            data.setdefault(key, {}).setdefault(build, []).append(m)
            order.setdefault(key, []).append(f"{build}{i} {m:.4g}")
        if build == "A":
            for (arm, cell, met), vs in per.items():
                a_pairs.setdefault((cell, met), {}).setdefault(i, {})[arm] = statistics.fmean(vs)
        dirs = sorted(d for d in glob.glob(out + ".runs/*_r*") if rmw_arm_of(os.path.basename(d)))
        if not dirs:
            print(f"  !!treatment {build}{i} {os.path.basename(out)}: no run directory (could not look)")
            treat = "NO VERDICT" if treat == "ok" else treat
        for d in dirs:
            stem = os.path.basename(d)
            counts = loan_counts(d)
            state, text = treatment_of_run(build, stem, counts, env_values(d), zcl_env_problem(d, rmw_arm_of(stem)))
            if state == "info":
                default_loans.append(bool(counts))
            print(f"  {'  ' if state in ('ok', 'info') else '!!'}treatment {build}{i} {stem}: {text}")
            if state not in ("ok", "info") and (treat == "ok" or state == "VOID"):
                treat = state
    n_a = sum(1 for b, _ in phases if b == "A")
    n_b = sum(1 for b, _ in phases if b == "B")
    df = n_a + n_b - 2
    t_p = bonferroni_t(len(PRIMARY), df) if df > 0 else math.inf
    t_c1 = bonferroni_t(len(CONTROL) * len(RMW_ARMS), df) if df > 0 else math.inf
    t_c2 = bonferroni_t(len(PRIMARY), n_a - 1) if n_a > 1 else math.inf
    print(f"phases {' '.join(b for b, _ in phases)}; unit = phase mean; B phases against A phases, pooled t, df {df}: "
          f"primary |t| > {t_p:.2f} (k {len(PRIMARY)} per question), control 1 |t| > {t_c1:.2f}; control 2 paired over "
          f"A phases, df {n_a - 1}, |t| > {t_c2:.2f}")
    print(f"B's default path (arm tickle) took or published loans in {sum(default_loans)} of {len(default_loans)} "
          "runs (expected 0: rcl disables loaned takes, rclcpp's publish(const T &) borrows none)")
    print(f"mode {'zcl (both builds lend; B builds rtt loans in the slot)' if ZCL else 'loans (A lends nothing)'}")

    def judge(key, thresh, control):
        arm, cell, met = key
        got = data.get(key, {})
        a, b = got.get("A", []), got.get("B", [])
        if len(a) < 2 or len(b) < 2:
            print(f"  {arm} {cell} {met}: NO VERDICT - phases with a value A {len(a)} B {len(b)} (2 each needed)")
            return "NO VERDICT"
        ma, mb, d, se, t = phase_t(a, b)
        tb = t if met in HIGHER_BETTER else -t  # > 0: B better
        line = (f"  {arm} {cell} {met}: A {ma:.4g} B {mb:.4g}  B-A {d:+.4g} ({100 * d / ma if ma else math.nan:+.2f}%)"
                f" t {t:+.2f}  [{' '.join(order[key])}]")
        if control:
            v = "VOID" if abs(t) > thresh else "PASS"
            line += "  -> CONTROL MOVED (VOID)" if v == "VOID" else "  -> control held"
        else:
            v = "IMPROVED" if tb > thresh else ("WORSE" if tb < -thresh else "PASS")
            line += f"  -> {v}"
            if v == "PASS":
                mde = 100 * thresh * se / ma if ma else math.nan
                line += f" (rules out a worsening > {mde:.1f}%{', WEAK' if mde > WEAK_PCT else ''})"
        print(line)
        return v

    common = [treat] if treat != "ok" else []
    print("CONTROL 1 (RadarDetection, not loanable; both rmw arms):")
    for arm in RMW_ARMS:
        for cell, met in CONTROL:
            common.append(judge((arm, cell, met), t_c1, True))
    print("CONTROL 2 (A phases: tickle_loans against tickle, which the variable cannot touch under A):")
    for cell, met in ([] if ZCL else PRIMARY):
        pairs = [(v["tickle"], v["tickle_loans"]) for v in a_pairs.get((cell, met), {}).values()
                 if "tickle" in v and "tickle_loans" in v]
        if len(pairs) < 2:
            print(f"  {cell} {met}: NO VERDICT - {len(pairs)} A phases with both arms (2 needed)")
            common.append("NO VERDICT")
            continue
        md, _se, t = paired_t([p[0] for p in pairs], [p[1] for p in pairs])
        base = statistics.fmean(p[0] for p in pairs)
        v = "VOID" if abs(t) > t_c2 else "PASS"
        print(f"  {cell} {met}: tickle_loans - tickle {md:+.4g} ({100 * md / base if base else math.nan:+.2f}%) "
              f"t {t:+.2f}  -> {'MOVED (VOID)' if v == 'VOID' else 'held'}")
        common.append(v)
    if ZCL:
        print("  not applicable in mode zcl: A's tickle_loans arm borrows too (control 1 is the control)")
    result = {}
    for arm in RMW_ARMS:
        q = QUESTION[arm]
        print(f"PRIMARY {q} (arm {arm}):")
        verdicts = list(common)
        for cell, met in PRIMARY:
            verdicts.append(judge((arm, cell, met), t_p, False))
        result[q] = next((v for v in ORDER if v in verdicts), "NO VERDICT")
    print(f"VERDICT D (landing, shipped defaults): {result['D']} (treatment {treat})")
    print(f"VERDICT L (loans opted into): {result['L']} (treatment {treat})")
    return result["D"], result["L"]


def witness(runs_dir, build):
    """The PC preflight's treatment check: every rmw_tickle run of a rmw_samehost.sh preflight runs/ directory, read
    as build A or B with treatment_of_run; B must also have a tickle_loans loanable run that took a loan."""
    dirs = sorted(d for d in glob.glob(os.path.join(runs_dir, "*_r*")) if rmw_arm_of(os.path.basename(d)))
    ok = bool(dirs)
    loaned = 0
    for d in dirs:
        stem = os.path.basename(d)
        state, text = treatment_of_run(build, stem, loan_counts(d), env_values(d), zcl_env_problem(d, rmw_arm_of(stem)))
        ok &= state in ("ok", "info")
        loaned += 1 if (build == "B" and rmw_arm_of(stem) == "tickle_loans" and state == "ok"
                        and f"_{CONTROL_TOPIC}_" not in stem) else 0
        print(f"  {'ok  ' if state in ('ok', 'info') else 'FAIL'} {stem}: {text}")
    if build == "B" and not loaned:
        print("  FAIL no tickle_loans loanable run took a loan")
        ok = False
    if not dirs:
        print(f"  FAIL no rmw_tickle runs under {runs_dir} (could not look)")
    return 0 if ok else 1


def selftest():
    """The guards answer, not the defaults: synthetic phases in a temporary directory, in both modes."""
    global ZCL
    import tempfile
    results = []
    with tempfile.TemporaryDirectory() as tmp:
        def phase(name, build, base, factor=1.0, loans=True, a_loans=False, env_ok=True, ctl_loans=False,
                  slots=True, a_slots=False):
            out = os.path.join(tmp, name)
            lines = ["=== runs done", "--- per run ---"]
            for arm in RMW_ARMS:
                for key, met in PRIMARY + CONTROL:
                    kind, rest = key.split("/", 1)
                    stem = f"{kind}_{rest.replace('/', '_')}_{arm}_r1"
                    ctl = CONTROL_TOPIC in key
                    d = f"{out}.runs/{stem}"
                    os.makedirs(d, exist_ok=True)
                    env = f"{ENV_VAR}=0;" if arm == "tickle_loans" and env_ok else ""
                    if ZCL and arm == "tickle_loans":
                        env += "".join(f"{k}={v};" for k, v in ZCL_VARS.items())
                    with open(f"{d}/meta.txt", "w") as f:
                        f.write(f"treat role=sub env=RMW_IMPLEMENTATION=rmw_tickle;{env}\n")
                    lend = ((build == "B" and arm == "tickle_loans" and loans and not ctl) or
                            (build == "A" and a_loans and not ctl) or (ctl and ctl_loans and build == "B"))
                    with open(f"{d}/sub.log", "w") as f:
                        f.write("Node 1 traffic: tx_datagrams=1\n")
                        if lend:
                            f.write("rmw_tickle: subscription /t loans_in_place=0 loans_copied=5\n")
                        if ZCL and kind == "rtt" and arm == "tickle_loans" and not ctl:
                            slot = (build == "B" and slots) or (build == "A" and a_slots)
                            f.write(f"rmw_tickle: publisher /t loans_published=5 loans_in_slot={5 if slot else 0}\n"
                                    if slot else "rmw_tickle: publisher /t loans_published=5\n")
                    sc = base if ctl else base * factor
                    if met == "p50_us":
                        lines.append(f"{key} {arm:10} r1 n=9000 p50 {100 * sc:.1f} p99 1 mean 1 us  cpu 1 us/rt")
                    elif met == "delivered_per_s":
                        lines.append(f"{key} {arm:10} r1 delivered {100000 / sc:,.0f}/s (sent 1/s) over 16 s  lost 0  "
                                     f"cpu {10 * sc:.2f} us/sample  rss pub/sub 1/1 kB  lat 0.001 ms")
            with open(out + ".txt", "w") as f:
                f.write("\n".join(lines) + "\n")
            return (build, out)

        jit = [1.000, 1.004, 0.997, 1.002, 0.999, 1.003, 0.998, 1.001]
        cases = [
            ("same", {}, ("PASS", "PASS")),
            ("B 2% worse", {"B": {"factor": 1.02}}, ("WORSE", "WORSE")),
            ("all 2% worse in B (control moves)", {"Bctl": True}, ("VOID", "VOID")),
            ("B tickle_loans takes no loan", {"B": {"loans": False}}, ("VOID", "VOID")),
            ("A shows loans", {"A": {"a_loans": True}}, ("VOID", "VOID")),
            ("variable not received", {"B": {"env_ok": False}}, ("VOID", "VOID")),
            ("control cell lends", {"B": {"ctl_loans": True}}, ("VOID", "VOID")),
        ]
        zcl_cases = [
            ("zcl: same", {}, ("PASS", "PASS")),
            ("zcl: A lends (loans_published, no slot field)", {"A": {"a_loans": True}}, ("PASS", "PASS")),
            ("zcl: B builds no rtt loan in a slot", {"B": {"slots": False}}, ("VOID", "VOID")),
            ("zcl: A shows a slot loan", {"A": {"a_slots": True}}, ("VOID", "VOID")),
            ("zcl: B 2% worse", {"B": {"factor": 1.02}}, ("WORSE", "WORSE")),
            ("zcl: variable not received", {"B": {"env_ok": False}}, ("VOID", "VOID")),
        ]
        saved = ZCL
        for zcl, table in ((False, cases), (True, zcl_cases)):
            ZCL = zcl
            run_cases(table, phase, jit, results)
        ZCL = saved
    ok = all(r.endswith("OK") for r in results)
    print("SELFTEST " + ("PASS" if ok else "FAIL") + ":\n  " + "\n  ".join(results))
    return 0 if ok else 1


def run_cases(cases, phase, jit, results):
    """One selftest table: each case's phases A B B A B A A B, read, and compared with what it must give."""
    for n, (label, mods, want) in enumerate(cases):
        ph = []
        for i, b in enumerate("ABBABAAB"):
            base = jit[i] * (1.02 if mods.get("Bctl") and b == "B" else 1.0)
            ph.append(phase(f"{'z' if ZCL else 'c'}{n}_{i}", b, base, **mods.get(b, {})))
        got = read(ph)
        results.append(f"{label}: {got} (want {want}) {'OK' if got == want else 'MISMATCH'}")


def main(argv):
    if argv[:1] == ["prereg"]:
        print(__doc__.split("Usage:")[0].rstrip())
        return 0
    if len(argv) >= 5 and argv[0] == "read":
        phases = []
        for spec in argv[1:]:
            build, sep, out = spec.partition("=")
            if not sep or build not in ("A", "B"):
                print(f"read: {spec!r} is not A=<OUT> or B=<OUT>", file=sys.stderr)
                return 2
            phases.append((build, out))
        return EXIT[read(phases)[0]]
    if len(argv) == 3 and argv[0] == "witness" and argv[2] in ("A", "B"):
        return witness(argv[1], argv[2])
    if argv[:1] == ["selftest"]:
        return selftest()
    print(__doc__.split("Usage:")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
