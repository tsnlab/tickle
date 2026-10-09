#!/usr/bin/env python3
"""ab_rmw_regress.py - the reading of ab_rmw_regress.sh (2026-10-09). Written, and its sha256 logged, before the lock.

QUESTION. RESULTS R2 / R9 (rmw_tickle, rtt bench RELIABLE block, on one rig Pi) rose from a7e02807 to 06dd78d8:
RTT mean 36.9 -> 40.0 us, CPU 63.3 -> 68.1 us per round trip, 3 reps each, R9's reps not overlapping; CycloneDDS in
the same cell +0.2 / +0.7. The PC (rmw_rtt_bisect.sh, 8 interleaved reps x 5 commits) could not decide it: its
rep-to-rep scatter (SE ~2.5 us) is as large as the effect. Is it real on the rig?

DESIGN. Phases A B B A B A A B (A = a7e02807, B = 06dd78d8; balanced against linear and quadratic drift), each one
rmw_samehost.sh run at its commit: arms tickle and cyclonedds, rtt bench block, RELIABLE and BEST_EFFORT, REPS reps.

PER PHASE (from that phase's rmw_samehost_summary.py "per run" lines after "=== runs done", i.e. the published
window: 4096 round trips dropped at each end): for each arm and cell, the mean over its reps of the run's RTT mean and
CPU us per round trip. Page faults: minor faults per round trip, ping + pong, over the same window, from the cell
script's sampler F lines (rmw_rtt_bisect.py's reading).

RULES, in this order:
  T  TREATMENT: each phase's log names a build at its own commit ("built on ... at <sha7>"); every tickle and
     cyclonedds run of the two cells is present and not VOID in the summary; >= 2 reps per arm and cell per phase.
     Otherwise VOID.
  C  CONTROL: cyclonedds bench/reliable/block, RTT mean and CPU per rt, B phases against A phases (Welch, phase
     means): |t| > T_CRIT -> VOID (the rig moved between A and B phases in a way no rmw_tickle commit can cause).
  P  PRIMARY: tickle bench/reliable/block RTT mean and CPU per rt (R2 and R9), B - A over phases, Welch t.
     T_CRIT = 3.29 (two-sided 0.05, Bonferroni over the 2 primaries, df ~6). B - A > 0 with t > T_CRIT on either ->
     REAL REGRESSION. Otherwise NOT REAL at this power, with the difference and its 95% interval printed, so the size
     the run could have seen is on record.
  S  SECONDARY (printed, not scored): tickle bench/best_effort/block, same test - a REAL in reliable with nothing in
     best_effort points at the RELIABLE path (783bb5d5), in both at the context / allocation (718220d8).
  F  LAZY FAULTS: tickle bench/reliable minor faults per rt, B - A. Below 0.01 per round trip -> the hypothesis that
     718220d8's lazily faulted context costs microseconds per round trip is FALSIFIED (one extra fault per 100 round
     trips cannot); otherwise its size and t are printed.
"""
import math
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rmw_rtt_bisect as rb  # noqa: E402

T_CRIT = 3.29
CELLS = ("bench/reliable/block", "bench/best_effort/block")
LINE = re.compile(r"^rtt/(\w+)/(\w+)/(\w+) (\w+)\s+r(\d+) n=(\d+) p50 ([\d.]+) p99 ([\d.]+) mean ([\d.]+) us\s+cpu ([\d.]+) us/rt")


def phase(path, sha):
    txt = pathlib.Path(str(path) + ".txt")
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
    out = {}
    for l in lines[start:]:
        if l.startswith("VOID"):
            return None, f"{txt.name}: {l}"
        m = LINE.match(l)
        if not m:
            continue
        cell = f"{m.group(1)}/{m.group(2)}/{m.group(3)}"
        arm, rep = m.group(4), int(m.group(5))
        rec = {"rtt": float(m.group(9)), "cpu": float(m.group(10))}
        if arm == "tickle":
            d = pathlib.Path(str(path) + ".runs") / f"rtt_{m.group(1)}_{m.group(2)}_{m.group(3)}_tickle_r{rep}"
            try:
                st = rb.stamps(d)
                w = st[rb.WARM:len(st) - rb.COOL]
                _, f = rb.series(d)
                tot = 0
                for role in ("ping", "pong"):
                    tot += rb.delta(f, role, 0, w[0][0], w[-1][1])
                rec["minflt"] = tot / len(w)
            except (OSError, TypeError, IndexError, ValueError):
                rec["minflt"] = None
        out.setdefault((cell, arm), []).append(rec)
    for c in CELLS:
        for a in ("tickle", "cyclonedds"):
            if len(out.get((c, a), [])) < 2:
                return None, f"{txt.name}: {c} {a} has {len(out.get((c, a), []))} reps"
    return out, None


def welch(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    va = sum((x - ma) ** 2 for x in a) / (len(a) - 1)
    vb = sum((x - mb) ** 2 for x in b) / (len(b) - 1)
    se = math.sqrt(va / len(a) + vb / len(b))
    return mb - ma, se, (mb - ma) / se if se > 0 else float("inf")


def main():
    if sys.argv[1] == "prereg":
        print(__doc__)
        print(f"T_CRIT={T_CRIT} CELLS={CELLS}")
        return 0
    a_sha, b_sha = sys.argv[2], sys.argv[3]
    phases = []
    for arg in sys.argv[4:]:
        lab, path = arg.split("=", 1)
        data, why = phase(path, a_sha if lab == "A" else b_sha)
        if why:
            print(f"VERDICT: VOID - treatment: {why}")
            return 1
        phases.append((lab, data))
    print("per phase (rep means): " + "  ".join(lab for lab, _ in phases))

    def series(lab, cell, arm, metric):
        v = []
        for l, d in phases:
            if l != lab:
                continue
            xs = [r[metric] for r in d[(cell, arm)] if r.get(metric) is not None]
            if xs:
                v.append(sum(xs) / len(xs))
        return v

    for c in CELLS:
        for arm in ("tickle", "cyclonedds"):
            for m in ("rtt", "cpu") + (("minflt",) if arm == "tickle" else ()):
                row = []
                for l, d in phases:
                    xs = [r[m] for r in d[(c, arm)] if r.get(m) is not None]
                    row.append(f"{l}:{sum(xs) / len(xs):.3f}" if xs else f"{l}:-")
                print(f"  {c:24s} {arm:10s} {m:6s} " + " ".join(row))
    verdict = None
    for m in ("rtt", "cpu"):
        dd, se, t = welch(series("A", CELLS[0], "cyclonedds", m), series("B", CELLS[0], "cyclonedds", m))
        print(f"C control cyclonedds {CELLS[0]} {m}: B-A {dd:+.3f} (SE {se:.3f}, t {t:+.2f})")
        if abs(t) > T_CRIT:
            verdict = f"VOID - the control moved ({m}, t {t:+.2f})"
    real = []
    for c, tag in ((CELLS[0], "P"), (CELLS[1], "S")):
        for m in ("rtt", "cpu"):
            a, b = series("A", c, "tickle", m), series("B", c, "tickle", m)
            dd, se, t = welch(a, b)
            ma = sum(a) / len(a)
            print(f"{tag} tickle {c} {m}: A {ma:.3f} -> B {sum(b) / len(b):.3f}, B-A {dd:+.3f} ({100 * dd / ma:+.1f}%), "
                  f"95% CI [{dd - 2.45 * se:+.3f}, {dd + 2.45 * se:+.3f}], t {t:+.2f}")
            if tag == "P" and dd > 0 and t > T_CRIT:
                real.append(m)
    fa, fb = series("A", CELLS[0], "tickle", "minflt"), series("B", CELLS[0], "tickle", "minflt")
    if len(fa) >= 2 and len(fb) >= 2:
        dd, se, t = welch(fa, fb)
        print(f"F lazy faults: minflt per rt B-A {dd:+.4f} (t {t:+.2f}) -> " + ("FALSIFIED" if dd < 0.01 else "STANDS"))
    else:
        print("F lazy faults: NO VERDICT (fewer than 2 phases per build with fault counts)")
    if verdict is None:
        verdict = (f"REAL REGRESSION ({', '.join(real)})" if real else "NOT REAL at this power (see the intervals)")
    print(f"VERDICT: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
