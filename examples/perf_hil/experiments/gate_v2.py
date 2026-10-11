#!/usr/bin/env python3
"""Gate v2: how a TickLE-vs-TickLE A/B on the rig decides (the user's decision of 2026-10-11, TESTING.md 6.1).

Shared by every comparator that reads such an A/B (ab_compare.py campaign; the same-host and rmw readers), so one rule
is applied to every run. Written before any gate v2 run; every rule below is implemented here, not only stated.

WHY. The old gate decided on CPU time, rate and latency at 2 x SE (Bonferroni over the pre-registered primaries). On
the Pi 5 it read +2% WORSE three times for a change whose executed instructions are equal (2e9e861c vs a5747a85), and
the gap vanished when both arms were built with -falign-functions=64: the gate was more sensitive than the build is
reproducible. So:

ARMS. A (the parent), B (the change), and P = A' - A's commit rebuilt with TICKLE_PLACEMENT_CFLAGS
(-falign-functions=128, gate_v2_lib.sh): the same code with every function moved by a multiple of 64 bytes, which is
what a change does to the code it does not touch once every build is layout-stable (A1). All three are built with the
same base flags (TICKLE_LAYOUT_CFLAGS) and interleaved in mirrored blocks (A P B B P A) by the harness.

FAMILIES.
  PRIMARY   executed instructions per sample (per round trip on latency cells), user and kernel, per process -
            insn_u_per_sample and insn_k_per_sample, every role and cell unless the harness pre-registers a subset.
  WITNESS   CPU time, rate and latency (the harness names them; ab_compare.py: cpu_s_per_Msample, send_mbps,
            recv_mbps, rtt_avg_ms). They still decide: a witness that regresses beyond its threshold AND its
            placement floor is WORSE, as before, but now net of what placement alone does to it.
  secondary everything else; printed at plain 2 x SE, never decides.
THRESHOLD. k = the number of deciding (PRIMARY + WITNESS) comparisons. t = (B - A) / sqrt(SE_A^2 + SE_B^2), judged
  against the two-sided Student t at FAMILY_ALPHA / k with Welch's degrees of freedom - not the normal z, which at
  n = 6 per arm lets a family of 14 cross by noise about one run in five (the first fixture check of this file did).
  A harness whose unit is a block passes its own threshold instead.
PLACEMENT FLOOR (A3). Per (cell, metric): FLOOR = |mean(P) - mean(A)|, measured in the same run.
VERDICT per comparison:
  WORSE   (B - A) in the bad direction, |t| > threshold, AND |B - A| > FLOOR
  better  (B - A) in the good direction, |t| > threshold, AND |B - A| > FLOOR
  held    otherwise (beyond the threshold but inside the floor is printed as "held(placement-sized)")
OVERALL:
  NO VERDICT  no PRIMARY comparison at all (no counts: "could not look" must not pass), a pre-registered item that
              selects nothing, or a deciding comparison with no P arm (a floor not measured is not zero)
  WORSE       any deciding comparison WORSE
  PASS        otherwise
Also printed, never deciding: P vs A per deciding metric (how large placement is this session), and a NOTE when a
PRIMARY moves between A and P beyond the threshold - instructions should not move with placement (layout_flags_cost.sh
Q1/Q2), so that is run-to-run variation in the work itself (retransmissions, timer paths), and it widens the floor.
Exit status of report(): 0 PASS, 1 WORSE, 3 NO VERDICT.
"""
import math
import statistics

FAMILY_ALPHA = 0.0455   # two-sided P(|z| > 2), the 2 x SE rule's own false-positive rate
SECONDARY_Z = 2.0
INSN_METRICS = ("insn_u_per_sample", "insn_k_per_sample")


def stats(vals):
    """(mean, SE, n) of a list, or None when empty; SE is nan for n = 1."""
    n = len(vals)
    if n == 0:
        return None
    mean = statistics.fmean(vals)
    se = statistics.stdev(vals) / math.sqrt(n) if n > 1 else float("nan")
    return mean, se, n


def student_t_cdf(x, df):
    """P(T <= x) for Student's t with df degrees of freedom (Simpson's rule on the density; no scipy on the rig)."""
    if math.isinf(df):
        return statistics.NormalDist().cdf(x)
    if x < 0:
        return 1.0 - student_t_cdf(-x, df)
    c = math.exp(math.lgamma((df + 1) / 2) - math.lgamma(df / 2)) / math.sqrt(df * math.pi)
    steps = 2000
    h = x / steps
    acc = 0.0
    for i in range(steps + 1):
        w = 1 if i in (0, steps) else (4 if i % 2 else 2)
        acc += w * (1 + (i * h) ** 2 / df) ** (-(df + 1) / 2)
    return 0.5 + c * acc * h / 3


def threshold(k, df=math.inf):
    """The two-sided threshold at FAMILY_ALPHA / k: Student t with df degrees of freedom (normal when df is inf)."""
    p = 1 - FAMILY_ALPHA / max(k, 1) / 2
    if math.isinf(df):
        return statistics.NormalDist().inv_cdf(p)
    lo, hi = 0.0, 1000.0
    for _ in range(60):
        mid = (lo + hi) / 2
        if student_t_cdf(mid, df) < p:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def welch_df(a, b):
    """Welch-Satterthwaite degrees of freedom of (B - A) from two (mean, SE, n); inf when both SE are 0, and the
    smaller arm's n - 1 when one SE is unknown (n = 1 gives df 1 at most)."""
    va = 0 if math.isnan(a[1]) else a[1] ** 2
    vb = 0 if math.isnan(b[1]) else b[1] ** 2
    if va + vb == 0:
        return math.inf
    den = (va ** 2 / (a[2] - 1) if a[2] > 1 else 0) + (vb ** 2 / (b[2] - 1) if b[2] > 1 else 0)
    if den == 0:
        return max(1, min(a[2], b[2]) - 1)
    return max(1.0, (va + vb) ** 2 / den)


def tstat(a, b):
    """(B - A, t) from two (mean, SE, n); SE 0 in both arms makes any difference infinite."""
    diff = b[0] - a[0]
    se = math.sqrt(sum(0 if math.isnan(x[1]) else x[1] ** 2 for x in (a, b)))
    if se == 0:
        return diff, (0.0 if diff == 0 else math.copysign(math.inf, diff))
    return diff, diff / se


def judge(a, b, p, hi_better, z):
    """One comparison under gate v2 at threshold z. Returns (diff, t, floor, verdict); floor is None without a P arm,
    and the verdict is then reported as 'nofloor-...' so no caller can mistake it for a gate v2 one."""
    diff, t = tstat(a, b)
    floor = abs(p[0] - a[0]) if p else None
    good = (t > z) if hi_better else (t < -z)
    bad = (t < -z) if hi_better else (t > z)
    if floor is None:
        return diff, t, None, "nofloor-WORSE" if bad else "nofloor-better" if good else "nofloor-held"
    if bad and abs(diff) > floor:
        return diff, t, floor, "WORSE"
    if good and abs(diff) > floor:
        return diff, t, floor, "better"
    if bad or good:
        return diff, t, floor, "held(placement-sized)"
    return diff, t, floor, "held"


def _line(tag, verdict, cell, metric, a, b, diff, t, z, floor):
    pct = 100 * diff / a[0] if a[0] else float("nan")
    fl = f"  floor {floor:.4g} ({100 * floor / a[0]:.2f}%)" if floor is not None and a[0] else ""
    print(f"{tag:9s} {verdict:22s} {cell:40s} {metric:30s} A {a[0]:12.4f} (se {a[1]:.4f}, n{a[2]})  "
          f"B {b[0]:12.4f} (se {b[1]:.4f}, n{b[2]})  {pct:+6.2f}%  t {t:+.1f} (> {z:.2f}){fl}")


def report(rows, z=None, title=""):
    """rows: dicts with cell, metric, family ('primary' | 'witness' | 'secondary' | 'unmatched'), a, b, p (stats
    tuples; p may be None), hi_better. A row of family 'unmatched' names (in metric) a pre-registered item that selected
    nothing. z: a fixed threshold (block designs); None = Bonferroni Student t at Welch df per comparison.
    Prints the table and the verdict; returns 0 PASS, 1 WORSE, 3 NO VERDICT."""
    unmatched = [r["metric"] for r in rows if r["family"] == "unmatched"]
    rows = [r for r in rows if r["family"] != "unmatched"]
    deciding = [r for r in rows if r["family"] in ("primary", "witness")]
    k = len(deciding)

    def zz(a, b):
        return z if z is not None else threshold(k, welch_df(a, b))

    if title:
        print(f"--- {title}")
    rule = f"|t| > {z:.2f}" if z is not None else f"|t| > Student t at alpha {FAMILY_ALPHA}/{k} (Welch df)"
    print(f"--- gate v2: {sum(r['family'] == 'primary' for r in rows)} primary (instructions) + "
          f"{sum(r['family'] == 'witness' for r in rows)} witness comparison(s), k = {k}; WORSE needs {rule} "
          "and |B - A| beyond the placement floor |P - A|")
    counts, sized, nofloor = {"better": 0, "WORSE": 0, "held": 0}, 0, 0
    for fam in ("primary", "witness"):
        for r in (x for x in rows if x["family"] == fam):
            thr = zz(r["a"], r["b"])
            diff, t, floor, v = judge(r["a"], r["b"], r["p"], r["hi_better"], thr)
            if v.startswith("nofloor"):
                nofloor += 1
            elif v.startswith("held"):
                counts["held"] += 1
                sized += v != "held"
            else:
                counts[v] += 1
            _line(fam.upper(), v, r["cell"], r["metric"], r["a"], r["b"], diff, t, thr, floor)
    for r in (x for x in deciding if x["p"]):
        thr = zz(r["a"], r["p"])
        diff, t = tstat(r["a"], r["p"])
        moved = abs(t) > thr
        _line("PLACEMENT", "P vs A " + ("beyond" if moved else "within"), r["cell"], r["metric"], r["a"], r["p"],
              diff, t, thr, None)
        if moved and r["family"] == "primary":
            print(f"NOTE: {r['cell']} {r['metric']} moved between A and P beyond the threshold: instructions do not "
                  "move with placement, so this is run-to-run variation in the work; its floor is that wide")
    sec = {"better": 0, "WORSE": 0, "held": 0}
    for r in (x for x in rows if x["family"] == "secondary"):
        diff, t = tstat(r["a"], r["b"])
        bad = (t < -SECONDARY_Z) if r["hi_better"] else (t > SECONDARY_Z)
        good = (t > SECONDARY_Z) if r["hi_better"] else (t < -SECONDARY_Z)
        v = "WORSE" if bad else "better" if good else "held"
        sec[v] += 1
        _line("secondary", v, r["cell"], r["metric"], r["a"], r["b"], diff, t, SECONDARY_Z, None)
    print(f"--- secondary (never decide, 2 x SE): {sec['better']} better, {sec['held']} held, {sec['WORSE']} WORSE of "
          f"{sum(sec.values())}")
    print(f"--- deciding: {counts['better']} better, {counts['held']} held ({sized} beyond the threshold but inside "
          f"the placement floor), {counts['WORSE']} WORSE of {k}")
    if unmatched:
        print(f"NO VERDICT: pre-registered item(s) {', '.join(unmatched)} matched no non-void comparison in both arms")
        return 3
    if not any(r["family"] == "primary" for r in rows):
        print("NO VERDICT: no instruction counts (PRIMARY) in both arms - the gate could not look")
        return 3
    if nofloor:
        print(f"NO VERDICT: {nofloor} deciding comparison(s) have no placement arm P, so no floor was measured")
        return 3
    if counts["WORSE"]:
        print(f"VERDICT: WORSE ({counts['WORSE']} of {k} deciding comparison(s) beyond the threshold and their floor)")
        return 1
    print(f"VERDICT: PASS (no deciding comparison of {k} worse beyond the threshold and its placement floor)")
    return 0
