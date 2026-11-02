#!/usr/bin/env python3
"""ethernet_seed_ab_read.py RES - the reader of ethernet_seed_ab.sh (2026-11-02).

Implements that driver's pre-registered rules IN CODE (its header states them; this file decides). RES holds one
directory per arm label - A (the base tree), B (base + the seed fix), A2 (the base tree again, the A/A) - and under each
the OUTs of the harnesses it ran: tv (time_varying_netns.sh, ETHERNET), wbe_small / wbe_large (wifi_burst_loss_netns.sh,
ETHERNET; rel_large with TT_UDP_OFFLOAD=0), wbw_small / wbw_large (the same on the WIFI profile: the control - the fix
only changes the branch a non-repair_timing link takes, so nothing on WIFI can move). RES/identity.txt is the driver's
record of the trees.

Prints one line per check and rule, then "ETHSEED_VERDICT <PASS|MIXED|FAIL|FALSIFIED|VOID>" last.
"""

import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wifi_burst_read import kv, num, per_run, quantile, read_lines  # noqa: E402
from wifi_burst_read import load_runs as wb_load_runs  # noqa: E402

LABELS = ("A", "B", "A2")
GROUPS = {"tv": "ethernet", "wbe_small": "ethernet", "wbe_large": "ethernet", "wbw_small": "wifi", "wbw_large": "wifi"}
# The paths A..B may change. ETHSEED_EXPECTED_DIFF (comma-separated) names them for another B: the Karn's-rule B
# (2026-11-03) also re-pins two tests' deterministic counts and a wire golden. Set before the run, like a threshold.
EXPECTED_DIFF = os.environ.get("ETHSEED_EXPECTED_DIFF", "src/tickle.c,tests/test_reliable_pubsub.c").split(",")

# --- pre-registered thresholds (ethernet_seed_ab.sh header). Change only before a run, and say so there. ---
P1_B_COPIES_MAX = 0.03  # s_d20 rel_small copies after delivery / delivered, every B rep
P1_B_TIMER_PER_GAP_MAX = 2.0  # s_d20 rel_small timer ACKNACKs per gap opened, B pooled, both directions
P1_FALSIFIED_COPIES = 0.10  # B pooled above this: the seed is not the mechanism
A_PHENOMENON_MIN = 0.10  # A's s_d20 copies must reproduce the smoke's ~30%, else nothing was exercised: VOID
AA_DECISION_MAX = 0.05  # |A - A2| on s_d20 rel_small copies above this: A does not reproduce itself: VOID
DOSE_A_TIMER_MIN = 2.5  # s_d3 rel_small: A timer per gap (predicted ~4) ...
DOSE_B_TIMER_MAX = 1.5  # ... and B's (predicted ~0-1): the mechanism's dose-response, report only
P2_P99_SLACK_MS = 40.0  # s_d20 rel_small round-trip p99: B <= A + one base round trip
CONTROL_MOVED_MAX_SHARE = 0.10  # control comparisons outside the A/A tolerance: more than this share (and > 1) VOID
MIN_REPS = 2  # valid reps per label for any judged cell
R1 = {"ge5b2": 0.995, "ge5b4": 0.995}  # wifi_burst R1, rel_small delivered
R3 = {"ge5b4": 0.95, "ge10b4": 0.95}  # wifi_burst R3, rel_large delivered
# Absolute floors of the B-vs-A tolerance, per metric: tol = max(2 x |A - A2|, 2 x SE of A and A2 reps, floor).
FLOORS = {"delivered": lambda a: 0.005, "copies": lambda a: 0.005, "owd_p99": lambda a: max(0.10 * a, 1.0),
          "rtt_p99": lambda a: max(0.10 * a, 1.0), "p99": lambda a: max(0.10 * a, 1.0),
          "retx": lambda a: 0.10 * a + 5.0, "timer_per_gap": lambda a: 0.5, "timer": lambda a: 0.10 * a + 5.0}
WORSE_IF_LOWER = ("delivered",)
LATENCY = ("owd_p99", "rtt_p99", "p99")

REGRESSION = [("tv", "s_d1", "rel_small"), ("tv", "s_d1", "rel_large")] + [
    (g, arm, cell) for g, cell in (("wbe_small", "rel_small"), ("wbe_large", "rel_large"))
    for arm in ("ge1b2", "ge5b2", "ge5b4", "ge10b4", "tv")]
CONTROLS = [("tv", "s_l0", "rel_small"), ("tv", "s_l0", "rel_large"), ("wbe_small", "ctl", "rel_small"),
            ("wbe_large", "ctl", "rel_large")] + [
    (g, arm, cell) for g, cell in (("wbw_small", "rel_small"), ("wbw_large", "rel_large"))
    for arm in ("ctl", "ge1b2", "ge5b2", "ge5b4", "ge10b4", "tv")]
CLEAN_ARMS = ("s_l0", "ctl")
SMALL_METRICS = ("delivered", "owd_p99", "copies", "retx")
LARGE_METRICS = ("delivered", "p99", "copies", "retx")


def rstats_of(run_dir):
    """role.key -> value over every RSTATS line (histograms skipped) of ping.log and echo.log."""
    out = {}
    for name in ("ping.log", "echo.log"):
        for line in read_lines(os.path.join(run_dir, name)):
            if line.startswith("RSTATS:") and "hist=" not in line:
                d = kv(line)
                for k, v in d.items():
                    out[d.get("role", "?") + "." + k] = v
    return out


def load_group(res, label, group):
    """Per (arm, cell, rep): a dict of metrics, or None with the reason it is unusable; plus the OUT's provenance."""
    out = os.path.join(res, label, group)
    prov = kv(" ".join(read_lines(os.path.join(out, "provenance.txt"))))
    rows = {}
    if not os.path.isdir(os.path.join(out, "runs")):
        return rows, prov
    dur = num(prov, "dur", 30.0)
    for r in wb_load_runs(out, GROUPS[group]):
        key = (r["arm"], r["cell"], r["rep"])
        if r["cell"] == "be_small":
            continue
        m = per_run(r, dur)
        why = list(r["void"]) + list(r.get("treat_void") or [])
        if m is None:
            rows[key] = {"void": why or ["no metrics"]}
            continue
        rs = rstats_of(r["dir"])
        m["void"] = why
        m["delivered"] = (min(m["delivered"], m["offered"]) / m["offered"]) if m["offered"] else None
        if r["cell"] == "rel_small":
            # As time_varying_read.py's V1: the echo reader's copies after delivery over its delivered samples.
            dl = int(num(r["echo"], "scored"))
            m["copies"] = m["dups"] / dl if dl else None
            m["owd_p99"] = quantile(r["owd"], 0.99) / 1000.0 if r["owd"] else None
            m["rtt_p99"] = quantile(r["rtt"], 0.99) / 1000.0 if r["rtt"] else None
            m["timer_sum"] = int(num(rs, "echo.timer")) + int(num(rs, "ping.timer"))
            m["gaps_sum"] = int(num(rs, "echo.gaps_opened")) + int(num(rs, "ping.gaps_opened"))
            if "echo.timer" not in rs or "ping.timer" not in rs:
                m["void"].append("RSTATS acknack_tx missing (a build without tt_RELIABLE_STATS?)")
        else:
            dl = int(num(r["echo"], "delivered"))
            m["copies"] = m["dups"] / dl if dl else None  # duplicate units per delivered sample
            m["p99"] = m["p99"] / 1000.0 if m.get("p99") else None
            m["timer"] = int(num(rs, "sub.timer")) if "sub.timer" in rs else None
        m["loaded"] = r["loaded"]
        rows[key] = m
    return rows, prov


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def stdev(xs):
    xs = [x for x in xs if x is not None]
    if len(xs) < 2:
        return 0.0
    mu = sum(xs) / len(xs)
    return math.sqrt(sum((x - mu) ** 2 for x in xs) / (len(xs) - 1))


class Data:
    def __init__(self, res):
        self.res = res
        self.rows = {}  # (label, group) -> {(arm, cell, rep): metrics}
        self.prov = {}
        for label in LABELS:
            for group in GROUPS:
                self.rows[(label, group)], self.prov[(label, group)] = load_group(res, label, group)
        self.control_void = set()  # (label, group, rep): its clean arm lost something

    def valid(self, label, group, arm, cell):
        """The usable per-rep metric dicts of one cell: not void, and its rep's control clean."""
        out = []
        for (a, c, rep), m in sorted(self.rows[(label, group)].items()):
            if a == arm and c == cell and not m.get("void") and (label, group, rep) not in self.control_void:
                out.append(m)
        return out

    def values(self, label, group, arm, cell, metric):
        ms = self.valid(label, group, arm, cell)
        if metric in LATENCY:
            ms = [m for m in ms if not m["loaded"]]  # a loaded rep's latency is void, its delivery kept
        return [m.get(metric) for m in ms if m.get(metric) is not None]


def compare(d, group, arm, cell, metric):
    """(verdict, text): 'worse' / 'better' / 'same' / 'void', B against A with the A/A tolerance."""
    a = d.values("A", group, arm, cell, metric)
    a2 = d.values("A2", group, arm, cell, metric)
    b = d.values("B", group, arm, cell, metric)
    if min(len(a), len(a2), len(b)) < MIN_REPS:
        return "void", f"{group}/{arm}/{cell} {metric}: reps A {len(a)}, A2 {len(a2)}, B {len(b)} (need {MIN_REPS})"
    ma, ma2, mb = mean(a), mean(a2), mean(b)
    se = stdev(a + a2) / math.sqrt(len(b))
    tol = max(2 * abs(ma - ma2), 2 * se, FLOORS[metric](abs(ma)))
    delta = mb - ma
    lower_worse = metric in WORSE_IF_LOWER
    if (delta < -tol) if lower_worse else (delta > tol):
        v = "worse"
    elif (delta > tol) if lower_worse else (delta < -tol):
        v = "better"
    else:
        v = "same"
    return v, (f"{group}/{arm}/{cell} {metric}: A {ma:.4g} A2 {ma2:.4g} B {mb:.4g} delta {delta:+.4g} tol {tol:.4g}"
               f" -> {v}")


def identity_checks(d, emit):
    why = []
    ident = kv(" ".join(read_lines(os.path.join(d.res, "identity.txt"))))
    if not ident:
        return ["no identity.txt"]
    if ident.get("b_parent") != ident.get("a_sha"):
        why.append(f"B's parent {ident.get('b_parent')} is not A {ident.get('a_sha')}: B is not A + the fix alone")
    if ident.get("a2_sha") != ident.get("a_sha"):
        why.append("A2 is not the same tree as A")
    if sorted(ident.get("diff_paths", "").split(",")) != sorted(EXPECTED_DIFF):
        why.append(f"A..B changes {ident.get('diff_paths')}, not exactly {','.join(EXPECTED_DIFF)}")
    short = {"A": ident.get("a_short"), "B": ident.get("b_short"), "A2": ident.get("a_short")}
    for (label, group), prov in sorted(d.prov.items()):
        if not d.rows[(label, group)]:
            why.append(f"{label}/{group}: no runs")
            continue
        if prov.get("tree") != short[label] or prov.get("dirty_paths") != "0":
            why.append(f"{label}/{group}: provenance tree={prov.get('tree')} dirty={prov.get('dirty_paths')}, "
                       f"expected {short[label]} clean")
    emit("CHECK identity " + ("OK" if not why else "VOID: " + "; ".join(why)))
    return why


def control_clean(d, emit):
    """A rep whose clean arm (s_l0, ctl) lost anything - RELIABLE under 100% or a tc drop - voids that rep's group."""
    for (label, group), rows in sorted(d.rows.items()):
        for (arm, cell, rep), m in sorted(rows.items()):
            if arm not in CLEAN_ARMS:
                continue
            bad = []
            if m.get("void"):
                bad.append("void: " + "; ".join(m["void"]))
            else:
                if m.get("delivered") is None or m["delivered"] < 1.0:
                    bad.append(f"delivered {m.get('delivered')}")
                if m.get("all_drop", 0) > 0:
                    bad.append(f"{m['all_drop']} tc drops")
            if bad:
                d.control_void.add((label, group, rep))
                emit(f"CHECK control {label}/{group}/{arm}/{cell}/rep{rep} VOID (rep excluded): {', '.join(bad)}")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    res = sys.argv[1]
    lines = []

    def emit(s):
        print(s, flush=True)
        lines.append(s)

    d = Data(res)
    emit(f"=== ethernet_seed_ab_read {res}")
    voids = identity_checks(d, emit)
    control_clean(d, emit)

    # Runs unusable for their own reasons, listed.
    for (label, group), rows in sorted(d.rows.items()):
        for (arm, cell, rep), m in sorted(rows.items()):
            if m.get("void"):
                emit(f"RUN   {label}/{group}/{arm}-{cell}-{rep} VOID: {'; '.join(m['void'])}")
            elif m.get("loaded"):
                emit(f"RUN   {label}/{group}/{arm}-{cell}-{rep} latency void: PC load above nproc/2 at its start")

    # --- the A/A on the decision cell, and the phenomenon itself ---
    dec = ("tv", "s_d20", "rel_small")
    ca = d.values("A", *dec, "copies")
    ca2 = d.values("A2", *dec, "copies")
    cb = d.values("B", *dec, "copies")
    if min(len(ca), len(ca2), len(cb)) < MIN_REPS:
        voids.append(f"decision cell reps A {len(ca)} A2 {len(ca2)} B {len(cb)}, need {MIN_REPS}")
    else:
        if abs(mean(ca) - mean(ca2)) > AA_DECISION_MAX:
            voids.append(f"A/A on s_d20 rel_small copies: A {mean(ca):.2%} A2 {mean(ca2):.2%}, over "
                         f"{AA_DECISION_MAX:.0%} apart")
        if min(mean(ca), mean(ca2)) < A_PHENOMENON_MIN:
            voids.append(f"A's s_d20 rel_small copies {mean(ca):.2%} / A2 {mean(ca2):.2%}: the smoke's ~30% not "
                         f"reproduced (under {A_PHENOMENON_MIN:.0%}): nothing was exercised")
    emit("CHECK A/A + phenomenon " + ("OK" if not voids else "see VOID list"))

    # --- the control: nothing the fix cannot touch may move beyond the A/A tolerance ---
    moved, judged = [], 0
    for group, arm, cell in CONTROLS:
        for metric in (SMALL_METRICS if cell == "rel_small" else LARGE_METRICS):
            v, text = compare(d, group, arm, cell, metric)
            if v == "void":
                continue
            judged += 1
            if v != "same":
                moved.append(text)
    emit(f"CHECK control: {len(moved)} of {judged} comparisons outside the A/A tolerance")
    for t in moved:
        emit("      " + t)
    if judged == 0:
        voids.append("no control comparison could be judged")
    elif len(moved) > 1 and len(moved) > CONTROL_MOVED_MAX_SHARE * judged:
        voids.append(f"controls moved: {len(moved)} of {judged} (over {CONTROL_MOVED_MAX_SHARE:.0%}): the noise is "
                     "larger than the A/A spread shows")

    # --- P1: the decision cell ---
    p1 = "VOID"
    if min(len(ca), len(cb)) >= MIN_REPS:
        tb = d.valid("B", *dec)
        ta = d.valid("A", *dec)
        tpg_b = sum(m["timer_sum"] for m in tb) / max(1, sum(m["gaps_sum"] for m in tb))
        tpg_a = sum(m["timer_sum"] for m in ta) / max(1, sum(m["gaps_sum"] for m in ta))
        every = all(c <= P1_B_COPIES_MAX for c in cb)
        if mean(cb) > P1_FALSIFIED_COPIES:
            p1 = "FALSIFIED"
        elif every and tpg_b <= P1_B_TIMER_PER_GAP_MAX:
            p1 = "PASS"
        else:
            p1 = "FAIL"
        emit(f"P1    s_d20 rel_small {p1}: copies after delivery A {[f'{c:.2%}' for c in ca]} B "
             f"{[f'{c:.2%}' for c in cb]} (B every rep <= {P1_B_COPIES_MAX:.0%}; FALSIFIED if B mean > "
             f"{P1_FALSIFIED_COPIES:.0%}); timer ACKNACKs per gap A {tpg_a:.1f} B {tpg_b:.1f} (B <= "
             f"{P1_B_TIMER_PER_GAP_MAX})")
    else:
        emit("P1    VOID: too few reps")

    # --- P1d: dose-response at s_d3, report only ---
    da, db = d.valid("A", "tv", "s_d3", "rel_small"), d.valid("B", "tv", "s_d3", "rel_small")
    if len(da) >= MIN_REPS and len(db) >= MIN_REPS:
        ta = sum(m["timer_sum"] for m in da) / max(1, sum(m["gaps_sum"] for m in da))
        tb = sum(m["timer_sum"] for m in db) / max(1, sum(m["gaps_sum"] for m in db))
        ok = ta >= DOSE_A_TIMER_MIN and tb <= DOSE_B_TIMER_MAX
        emit(f"P1d   s_d3 rel_small {'CONFIRMED' if ok else 'NOT_CONFIRMED'} (report only): timer per gap A {ta:.2f}"
             f" (predicted ~4, >= {DOSE_A_TIMER_MIN}) B {tb:.2f} (<= {DOSE_B_TIMER_MAX})")
    else:
        emit("P1d   s_d3 VOID: too few reps")

    # --- P2: what the longer interval costs at s_d20 ---
    p2 = "VOID"
    bdl = d.values("B", *dec, "delivered")
    ra, rb = d.values("A", *dec, "rtt_p99"), d.values("B", *dec, "rtt_p99")
    if len(bdl) >= MIN_REPS and len(ra) >= MIN_REPS and len(rb) >= MIN_REPS:
        p2 = "PASS" if all(x >= 1.0 for x in bdl) and mean(rb) <= mean(ra) + P2_P99_SLACK_MS else "FAIL"
        emit(f"P2    s_d20 rel_small {p2}: B delivered {[f'{x:.2%}' for x in bdl]} (100% each); round-trip p99 A "
             f"{mean(ra):.1f} B {mean(rb):.1f} ms (B <= A + {P2_P99_SLACK_MS:.0f})")
    else:
        emit(f"P2    VOID: unloaded reps A {len(ra)} B {len(rb)}, delivered reps B {len(bdl)}")

    # --- P3: regressions on the 1 ms ETHERNET set ---
    worse, judged3, void3 = [], 0, []
    for group, arm, cell in REGRESSION:
        for metric in (SMALL_METRICS if cell == "rel_small" else LARGE_METRICS):
            v, text = compare(d, group, arm, cell, metric)
            if v == "void":
                void3.append(text)
                continue
            judged3 += 1
            if v == "worse":
                worse.append(text)
        thr = (R1 if cell == "rel_small" else R3).get(arm) if group.startswith("wbe") else None
        if thr is not None:
            a, b = d.values("A", group, arm, cell, "delivered"), d.values("B", group, arm, cell, "delivered")
            if a and b and mean(a) >= thr > mean(b):
                worse.append(f"{group}/{arm}/{cell}: delivered A {mean(a):.2%} meets {thr:.1%}, B {mean(b):.2%} not")
    p3 = "VOID" if judged3 == 0 or len(void3) > judged3 else ("FAIL" if worse else "PASS")
    emit(f"P3    regression {p3}: {len(worse)} worse of {judged3} judged, {len(void3)} void")
    for t in worse:
        emit("      worse " + t)
    for t in void3:
        emit("      void  " + t)

    # --- P4: report only - does B also help after a delay step up (it should not: the seed is a cold start) ---
    for group, arm in (("tv", "D_p2"), ("wbe_small", "tv")):
        a, b = d.values("A", group, arm, "rel_small", "copies"), d.values("B", group, arm, "rel_small", "copies")
        v, text = compare(d, group, arm, "rel_small", "copies")
        note = ""
        if v == "better" and a and b and mean(b) < 0.5 * mean(a):
            note = " - B helped a step up too: the cold-start-only reading is WRONG"
        emit(f"P4    (report) {text}{note}")

    # --- P5: rel_large at s_d20 ---
    p5parts = []
    for metric in ("timer", "retx"):
        v, text = compare(d, "tv", "s_d20", "rel_large", metric)
        p5parts.append((v, text))
    if any(v == "void" for v, _ in p5parts):
        p5 = "VOID"
    else:
        p5 = "FAIL" if any(v == "worse" for v, _ in p5parts) else "PASS"
    emit(f"P5    s_d20 rel_large {p5}: " + " | ".join(t for _, t in p5parts))

    # --- verdict ---
    for v in voids:
        emit("VOID  " + v)
    if voids or p1 == "VOID":
        verdict = "VOID"
    elif p1 == "FALSIFIED":
        verdict = "FALSIFIED"
    elif p1 == "PASS" and p2 == "PASS" and p3 == "PASS":
        verdict = "PASS" if p5 == "PASS" else "MIXED"
    elif p1 == "PASS":
        verdict = "MIXED" if p2 != "FAIL" and p3 != "FAIL" else "FAIL"
    else:
        verdict = "FAIL"
    emit(f"SUMMARY P1 {p1}  P2 {p2}  P3 {p3}  P5 {p5}  voids {len(voids)}")
    emit(f"ETHSEED_VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
