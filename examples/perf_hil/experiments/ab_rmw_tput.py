#!/usr/bin/env python3
"""ab_rmw_tput.py - the throughput reading of an rmw_tickle rig A/B (ab_rmw_regress.sh with TPUT_TOPICS set).

Written 2026-10-18, before any run of it, for the QoS redesign (step 3: rmw_tickle creates its endpoints with core's
QoS profile). ab_rmw_regress.py reads the RTT cells; no rule existed for the throughput cells (RESULTS R13-R21), so
this is it. ab_rmw_regress.sh copies this docstring to $OUTB.tput_prereg.txt before the rig lock is taken.

INPUT. Phases A B B A B A A B (balanced against linear and quadratic drift), each one rmw_samehost.sh run at its own
commit with arms tickle and cyclonedds. Per phase, the per-run lines rmw_samehost_summary.py prints after
"=== runs done" ("tput/<topic>/<qos> <arm> r<rep> delivered <n>/s (sent <n>/s) over <s> s  lost <n>  cpu <x>
us/sample ... rss pub/sub <p>/<s> kB ... witness <transport> ..."). Runs the summary voided are not in those lines;
they are counted from its "VOID runs:" list and printed.

CELLS (every tput cell of the phases; the default ones are R14-R17): <topic>/<qos>, topic Array1k / Array4k, qos
reliable (KEEP_ALL) / best_effort (KEEP_LAST 1).
METRICS per run: rate = delivered/s (higher is better, R14-R17), cpu = us per delivered sample, publisher +
subscriber (lower, R18-R21), rss = the larger process's peak RSS, kB (lower, R13).

RULES, in this order. Every one is code below; the reading is the printout of this file and nothing else.
  N  SAMPLES FIRST. A run counts only if its measured window is at least MIN_WINDOW_S seconds, it delivered at least
     MIN_DELIVERED samples in that window (rate x seconds), and - for arm tickle - its witness reads shm (the
     segment carried it; the summary already voids a tickle run off its segment, this re-checks the outcome). A run
     that fails is listed with why, never averaged.
  T  TREATMENT. Each phase's log names a build at its own commit ("built on ... at <sha7>"), and has "=== runs done".
     Every cell has >= MIN_REPS counted runs per arm in every phase, and each build has >= 2 phases. Otherwise VOID.
  C  CONTROL (cyclonedds: no rmw_tickle commit can move it). Per cell, rate and cpu, B phases against A phases (Welch
     on the phase means, Bonferroni over the control comparisons): |t| beyond the threshold -> VOID, the rig moved
     between the A and B phases.
  D  DELIVERY (RELIABLE cells). perf_test's lost after the first delivered second is 0 for rmw_tickle on main. A B
     run with lost > 0, or perf_test's "failed to publish" (REFUSED), where no A run of that cell had it -> WORSE
     (docs/TESTING.md: an incomplete rmw_tickle delivery LOSES every metric of the cell).
  P  PRIMARY. Per cell and metric (cells x 3 comparisons, k of them), tickle B - A over phase means, Welch t with
     Welch-Satterthwaite df, threshold ab_compare.bonferroni_t(k, df) (family alpha 0.0455). A comparison is WORSE
     only if it is beyond the threshold in its bad direction AND its relative change is larger than the control's
     relative change in the same cell and metric (rss: the control's rss) - our delta against its delta, never our
     value against a historical point. BETTER likewise in the good direction; everything else is held.
  VERDICT: VOID (N/T/C), WORSE (any D, or any P WORSE), otherwise PASS, with each P row's difference and 95%
  interval printed so the size the run could have seen is on record.
FALSIFIED - the claim "step 3 changed no throughput cell for the worse" is refuted by a WORSE verdict.

Exit status: 0 PASS, 1 WORSE, 3 VOID, 2 usage. The last line is always "VERDICT: ...".
Usage: ab_rmw_tput.py prereg | ab_rmw_tput.py read <A_sha> <B_sha> A=<phase stem> B=<stem> ... | ab_rmw_tput.py
--selftest
"""
import math
import pathlib
import re
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ab_compare import bonferroni_t  # noqa: E402

MIN_WINDOW_S = 10      # rmw_samehost.sh's TPUT_DUR 20 - WARM_S 2 - COOL_S 2 = 16 s expected
MIN_DELIVERED = 10000  # samples in the window
MIN_REPS = 2           # counted runs per arm, cell and phase
ARMS = ("tickle", "cyclonedds")
CONTROL = "cyclonedds"
METRICS = (("rate", True), ("cpu", False), ("rss", False))  # (name, higher is better)
LINE = re.compile(r"^tput/(\w+)/(\w+) (\w+)\s+r(\d+) delivered ([\d,]+)/s \(sent ([\d,]+)/s\) over (\d+) s\s+lost (\d+)"
                  r"\s+cpu ([\d.]+) us/sample.*?rss pub/sub (\d+)/(\d+) kB.*?(REFUSED )?witness (\S+)")


def num(s):
    return float(s.replace(",", ""))


def phase(stem, sha):
    """(runs {(cell, arm): [rec]}, excluded [str], voided [str]) or (None, why)."""
    txt = pathlib.Path(str(stem) + ".txt")
    if not txt.exists():
        return None, f"{txt} missing"
    lines = txt.read_text(errors="replace").splitlines()
    built = [l for l in lines if l.startswith("built on ")]
    if not built or not sha.startswith(built[-1].split(" at ")[-1].strip()):
        return None, f"{txt.name}: no build at {sha[:8]} ({built[-1] if built else 'no build line'})"
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith("=== runs done"))
    except StopIteration:
        return None, f"{txt.name}: no '=== runs done'"
    runs, excluded, voided, in_void = {}, [], [], False
    for l in lines[start:]:
        if l.startswith("VOID runs:"):
            in_void = True
            continue
        if in_void:
            if l.startswith("  tput_"):
                voided.append(l.strip())
                continue
            if not l.startswith("  "):
                in_void = False
        m = LINE.match(l)
        if not m:
            continue
        cell, arm = f"{m.group(1)}/{m.group(2)}", m.group(3)
        if arm not in ARMS:
            continue
        rate, secs = num(m.group(5)), int(m.group(7))
        rec = {"rep": int(m.group(4)), "rate": rate, "secs": secs, "lost": int(m.group(8)), "cpu": float(m.group(9)),
               "rss": max(int(m.group(10)), int(m.group(11))), "refused": bool(m.group(12)), "witness": m.group(13)}
        why = []
        if secs < MIN_WINDOW_S:
            why.append(f"window {secs} s < {MIN_WINDOW_S}")
        if rate * secs < MIN_DELIVERED:
            why.append(f"{rate * secs:.0f} delivered < {MIN_DELIVERED}")
        if arm == "tickle" and rec["witness"] != "shm":
            why.append(f"witness {rec['witness']}, not shm")
        if why:
            excluded.append(f"{txt.name} {cell} {arm} r{rec['rep']}: " + "; ".join(why))
            continue
        runs.setdefault((cell, arm), []).append(rec)
    return (runs, excluded, voided), None


def mean(xs):
    return sum(xs) / len(xs)


def welch(a, b):
    """(B - A, se, t, df) over two lists of phase means."""
    ma, mb = mean(a), mean(b)
    va = sum((x - ma) ** 2 for x in a) / (len(a) - 1)
    vb = sum((x - mb) ** 2 for x in b) / (len(b) - 1)
    qa, qb = va / len(a), vb / len(b)
    se = math.sqrt(qa + qb)
    d = mb - ma
    if se == 0:
        return d, 0.0, (0.0 if d == 0 else math.copysign(math.inf, d)), len(a) + len(b) - 2
    df = (qa + qb) ** 2 / ((qa ** 2 / (len(a) - 1) if qa else 0) + (qb ** 2 / (len(b) - 1) if qb else 0))
    return d, se, d / se, df


def read(a_sha, b_sha, args, out=print):
    """Returns (verdict word, exit status)."""
    phases = []
    for arg in args:
        lab, path = arg.split("=", 1)
        if lab not in ("A", "B"):
            out(f"VERDICT: VOID - treatment: phase label '{lab}' is not A or B")
            return "VOID", 3
        data, why = phase(path, a_sha if lab == "A" else b_sha)
        if why:
            out(f"VERDICT: VOID - treatment: {why}")
            return "VOID", 3
        phases.append((lab, pathlib.Path(path).name, data))
    for lab in ("A", "B"):
        if sum(1 for l, _, _ in phases if l == lab) < 2:
            out(f"VERDICT: VOID - treatment: fewer than 2 {lab} phases")
            return "VOID", 3
    cells = sorted({c for _, _, (runs, _, _) in phases for (c, _) in runs})
    for _, name, (runs, excluded, voided) in phases:
        for e in excluded:
            out(f"N excluded: {e}")
        for v in voided:
            out(f"N summary VOID: {name}: {v}")
    if not cells:
        out("VERDICT: VOID - treatment: no tput cell in any phase")
        return "VOID", 3
    for lab, name, (runs, _, _) in phases:
        for c in cells:
            for arm in ARMS:
                n = len(runs.get((c, arm), []))
                if n < MIN_REPS:
                    out(f"VERDICT: VOID - samples: {name} ({lab}) {c} {arm} has {n} counted run(s) < {MIN_REPS}")
                    return "VOID", 3

    def series(lab, c, arm, m):
        return [mean([r[m] for r in runs[(c, arm)]]) for l, _, (runs, _, _) in phases if l == lab]

    out("per phase (rep means): " + "  ".join(lab for lab, _, _ in phases))
    for c in cells:
        for arm in ARMS:
            for m, _ in METRICS:
                vals = " ".join(f"{l}:{mean([r[m] for r in runs[(c, arm)]]):.2f}" for l, _, (runs, _, _) in phases)
                out(f"  {c:22s} {arm:10s} {m:4s} {vals}")

    # C - the control
    kc = len(cells) * 2
    void = None
    for c in cells:
        for m in ("rate", "cpu"):
            d, se, t, df = welch(series("A", c, CONTROL, m), series("B", c, CONTROL, m))
            thr = bonferroni_t(kc, df)
            out(f"C control {CONTROL} {c} {m}: B-A {d:+.3f} (SE {se:.3f}, t {t:+.2f}, df {df:.1f}, |t| > {thr:.2f}"
                f" voids)")
            if abs(t) > thr and void is None:
                void = f"the control moved ({c} {m}, t {t:+.2f} beyond {thr:.2f})"
    if void:
        out(f"VERDICT: VOID - {void}")
        return "VOID", 3

    # D - delivery on RELIABLE cells
    worse = []
    for c in cells:
        if not c.endswith("/reliable"):
            continue
        for what, key in (("lost > 0", "lost"), ("REFUSED", "refused")):
            a_bad = [r for l, _, (runs, _, _) in phases if l == "A" for r in runs[(c, "tickle")] if r[key]]
            b_bad = [r for l, _, (runs, _, _) in phases if l == "B" for r in runs[(c, "tickle")] if r[key]]
            out(f"D tickle {c} {what}: A {len(a_bad)} run(s), B {len(b_bad)} run(s)")
            if b_bad and not a_bad:
                worse.append(f"D {c} {what}")

    # P - the primaries
    k = len(cells) * len(METRICS)
    out(f"P primaries: {len(cells)} cells x {len(METRICS)} metrics = k {k}")
    better = []
    for c in cells:
        for m, hi in METRICS:
            a, b = series("A", c, "tickle", m), series("B", c, "tickle", m)
            d, se, t, df = welch(a, b)
            thr = bonferroni_t(k, df)
            ma = mean(a)
            rel = d / ma if ma else 0.0
            ca, cb = series("A", c, CONTROL, m), series("B", c, CONTROL, m)
            crel = (mean(cb) - mean(ca)) / mean(ca) if mean(ca) else 0.0
            bad = (t < -thr) if hi else (t > thr)
            good = (t > thr) if hi else (t < -thr)
            beyond_control = abs(rel) > abs(crel)
            word = "WORSE" if bad and beyond_control else "BETTER" if good and beyond_control else "held"
            out(f"P {word:6s} tickle {c} {m}: A {ma:.2f} -> B {mean(b):.2f}, B-A {d:+.2f} ({100 * rel:+.2f}%; control "
                f"{100 * crel:+.2f}%), 95% CI [{d - 2.45 * se:+.2f}, {d + 2.45 * se:+.2f}], t {t:+.2f} df {df:.1f} "
                f"(|t| > {thr:.2f})")
            if word == "WORSE":
                worse.append(f"P {c} {m}")
            elif word == "BETTER":
                better.append(f"{c} {m}")
    if worse:
        out(f"VERDICT: WORSE ({', '.join(worse)})")
        return "WORSE", 1
    out(f"VERDICT: PASS (no WORSE of {k} primaries; better: {', '.join(better) or 'none'})")
    return "PASS", 0


# ---- selftest: synthetic phases in rmw_samehost.sh's own format -----------------------------------------------------
def _write_phase(d, name, sha, rows, voids=()):
    lines = [f"built on tickle-testbed-rpi-0 at {sha[:8]}", "--- rep 1/3 ---", "=== runs done 2026-10-18T00:00:00 ===",
             "", "--- per run ---"]
    for (topic, qos, arm, rep, rate, secs, lost, cpu, rss, refused, wit) in rows:
        lines.append(f"tput/{topic}/{qos} {arm:10} r{rep} delivered {rate:,.0f}/s (sent {rate:,.0f}/s) over {secs} s  "
                     f"lost {lost}  cpu {cpu:.2f} us/sample  rss pub/sub {rss - 100}/{rss} kB  lat 0.010 ms (lat_us "
                     f"10.000) {'REFUSED ' if refused else ''}witness {wit} (bytes/sample 0, pkts/sample 0.00, ratio "
                     f"0.00) tx_shm share 1.000 roudi 0")
    if voids:
        lines += ["", "VOID runs:"] + [f"  {v}" for v in voids]
    lines += ["", "--- table ---"]
    (d / f"{name}.txt").write_text("\n".join(lines) + "\n")
    return str(d / name)


def _scenario(d, tag, a_sha, b_sha, tweak):
    """Eight phases A B B A B A A B; tweak(lab, cell, arm, rep, rec dict) may change a run."""
    args = []
    jitter = [0.0, 0.004, -0.003, 0.002, -0.004, 0.001, 0.003, -0.002]
    for i, lab in enumerate("A B B A B A A B".split()):
        rows = []
        for topic, base in (("Array1k", 180000.0), ("Array4k", 85000.0)):
            for qos in ("reliable", "best_effort"):
                for arm, scale in (("tickle", 1.0), ("cyclonedds", 0.37)):
                    for rep in (1, 2, 3):
                        j = 1 + jitter[(i + rep) % 8]
                        rec = dict(rate=base * scale * j, secs=16, lost=0, cpu=(6.5 / scale) / j,
                                   rss=15000 + int(2000 * scale) + rep * 4, refused=False,
                                   wit="shm" if arm == "tickle" else "kernel")
                        tweak(lab, f"{topic}/{qos}", arm, rep, rec)
                        rows.append((topic, qos, arm, rep, rec["rate"], rec["secs"], rec["lost"], rec["cpu"],
                                     rec["rss"], rec["refused"], rec["wit"]))
        args.append(f"{lab}=" + _write_phase(d, f"{tag}_p{i + 1}_{lab}", a_sha if lab == "A" else b_sha, rows))
    return args


def selftest():
    a_sha, b_sha = "a1e8e887" + "0" * 32, "9018e2de" + "0" * 32
    cases = [
        ("identical builds", "PASS", lambda lab, c, arm, rep, r: None),
        ("B tickle Array1k/reliable rate -8%", "WORSE",
         lambda lab, c, arm, rep, r: r.update(rate=r["rate"] * 0.92) if (lab, c, arm) == ("B", "Array1k/reliable",
                                                                                           "tickle") else None),
        ("B tickle Array4k/best_effort cpu +10%", "WORSE",
         lambda lab, c, arm, rep, r: r.update(cpu=r["cpu"] * 1.10) if (lab, c, arm) == ("B", "Array4k/best_effort",
                                                                                         "tickle") else None),
        ("B tickle rss +6 MB in every cell", "WORSE",
         lambda lab, c, arm, rep, r: r.update(rss=r["rss"] + 6000) if (lab, arm) == ("B", "tickle") else None),
        ("B tickle loses samples on Array1k/reliable", "WORSE",
         lambda lab, c, arm, rep, r: r.update(lost=12) if (lab, c, arm, rep) == ("B", "Array1k/reliable", "tickle",
                                                                                  2) else None),
        ("B tickle rate +8% (better is not worse)", "PASS",
         lambda lab, c, arm, rep, r: r.update(rate=r["rate"] * 1.08) if (lab, arm) == ("B", "tickle") else None),
        ("control cyclonedds rate -10% in B phases", "VOID",
         lambda lab, c, arm, rep, r: r.update(rate=r["rate"] * 0.90) if (lab, arm) == ("B", "cyclonedds") else None),
        ("two short-window tickle runs in one phase", "VOID",
         lambda lab, c, arm, rep, r: r.update(secs=4) if (lab, c, arm) == ("A", "Array1k/best_effort", "tickle")
         and rep < 3 else None),
        ("tickle off its segment (witness kernel) in B", "VOID",
         lambda lab, c, arm, rep, r: r.update(wit="kernel") if (lab, arm) == ("B", "tickle") else None),
    ]
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        for i, (what, want, tweak) in enumerate(cases):
            args = _scenario(d, f"s{i}", a_sha, b_sha, tweak)
            got, rc = read(a_sha, b_sha, args, out=lambda s: None)
            ok = got == want and rc == {"PASS": 0, "WORSE": 1, "VOID": 3}[want]
            fails += not ok
            print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got} (want {want})")
        # treatment: a phase built at the wrong commit
        args = _scenario(d, "tw", a_sha, a_sha, lambda *x: None)
        got, _ = read(a_sha, b_sha, args, out=lambda s: None)
        fails += got != "VOID"
        print(f"  {'ok  ' if got == 'VOID' else 'FAIL'} B phases built at A's commit: {got} (want VOID)")
    print(f"SELFTEST {'OK' if not fails else 'FAILED (' + str(fails) + ')'}")
    return 0 if not fails else 1


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "prereg":
        print(__doc__)
        print(f"MIN_WINDOW_S={MIN_WINDOW_S} MIN_DELIVERED={MIN_DELIVERED} MIN_REPS={MIN_REPS} ARMS={ARMS} "
              f"CONTROL={CONTROL} METRICS={METRICS}")
        return 0
    if sys.argv[1] == "--selftest":
        return selftest()
    if sys.argv[1] == "read" and len(sys.argv) >= 6:
        return read(sys.argv[2], sys.argv[3], sys.argv[4:])[1]
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
