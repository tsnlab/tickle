#!/usr/bin/env python3
"""The collapse test's false-positive rate and power, exactly (2026-10-17), for each option collapse_fp_patch.py builds.

One window of R runs of s frames, against an MTU baseline of F frames. Under plain frame loss p: E ~ Binomial(F, p)
MTU events, X ~ Binomial(R, 1 - (1 - p)^s) runs lost, independent. A window collapses when the option's rule says so
given (E, F, X, R, s); P(false) = sum over E of P(E) P(X >= x*(E)). confirm2 needs two fresh windows on the same
baseline: sum P(E) P(X >= x*)^2. Power: X ~ Binomial(R, L_c) with L_c the collapse's run loss, the baseline as before.

  collapse_fp_model.py            prints the tables the report quotes
"""
import math

from scipy.stats import binom, chi2, poisson

Z95, Z999 = 1.6449, 3.0902


def upper(k, conf):
    return chi2.ppf(conf, 2 * k + 2) / 2.0


OPTS = {
    "0 main": dict(conf=0.95, z=3.09, minev=0, confirm=False),
    "1 bound999": dict(conf=0.999, z=3.09, minev=0, confirm=False),
    "2 minev3": dict(conf=0.95, z=3.09, minev=3, confirm=False),
    "3 z4": dict(conf=0.95, z=4.0, minev=0, confirm=False),
    "4 confirm2": dict(conf=0.95, z=3.09, minev=0, confirm=True),
    "5 bound999+confirm2": dict(conf=0.999, z=3.09, minev=0, confirm=True),
    "7 z4.5": dict(conf=0.95, z=4.5, minev=0, confirm=False),
    "8 surv": dict(conf=0.95, z=3.09, minev=0, confirm=False, surv=True),
    "9 b999+surv": dict(conf=0.999, z=3.09, minev=0, confirm=False, surv=True),
    "10 b999+surv+z4": dict(conf=0.999, z=4.0, minev=0, confirm=False, surv=True),
    "11 z@999,far@95+surv": dict(conf=0.999, z=3.09, minev=0, confirm=False, surv=True, far_conf=0.95),
    "12 11+z4": dict(conf=0.999, z=4.0, minev=0, confirm=False, surv=True, far_conf=0.95),
}


def threshold(o, e, f, r, s):
    """The smallest X the option calls a collapse at, or None."""
    if e < o["minev"]:
        return None
    p_hi = min(1.0, upper(e, o["conf"]) / f)
    l0 = 1.0 - (1.0 - p_hi) ** s
    if l0 <= 0.0 or l0 >= 1.0:
        return None
    sd = math.sqrt(r * l0 * (1 - l0))
    l0_far = l0
    if "far_conf" in o:  # the size of the excess judged at another bound than its significance
        l0_far = 1.0 - (1.0 - min(1.0, upper(e, o["far_conf"]) / f)) ** s
    far = 2 * l0_far
    if o.get("surv"):
        far = min(far, (1 + l0_far) / 2)  # q <= q0 / 2 where 2 L0 > 1
    x = max(4, math.ceil(r * l0 + 0.5 + o["z"] * sd - 1e-9), math.ceil(far * r - 1e-9))
    return x if x <= r else None


def p_collapse(o, p_frame, f, r, s, lc=None):
    """P(a window is called a collapse): under plain loss p_frame (lc None) or with run loss lc."""
    run_loss = lc if lc is not None else 1.0 - (1.0 - p_frame) ** s
    total = 0.0
    lo, hi = int(binom.ppf(1e-12, f, p_frame)), int(binom.isf(1e-12, f, p_frame)) + 1
    for e in range(max(0, lo), hi + 1):
        pe = binom.pmf(e, f, p_frame)
        if pe < 1e-15:
            continue
        x = threshold(o, e, f, r, s)
        if x is None:
            continue
        px = binom.sf(x - 1, r, run_loss)
        total += pe * (px * px if o["confirm"] else px)
    return total


def main():
    # Windows a stream sees: (label, s, R per 1 s window). Best effort runs at the cap (45; 11 for a 16 KB sample);
    # RELIABLE at n* (13, 4, 2 at 0.01, 0.1, 0.5%; 1 at 1% - no runs). R: 1 MB at 30 Hz is 723 units a second.
    streams = [("1MB BE n=45", 45, 480), ("16KB BE n=11", 11, 100), ("1MB REL n=13", 13, 1668),
               ("1MB REL n=4", 4, 5400), ("probe n=3", 3, 7230)]
    ps = [1e-4, 1e-3, 1e-2, 5e-2]
    fs = [600, 3000, 30000, 300000]
    print("P(false collapse) per 1 s window, plain frame loss")
    for label, s, r in streams:
        for p in ps:
            for f in fs:
                row = [p_collapse(o, p, f, r, s) for o in OPTS.values()]
                print("%-14s p=%-6g F=%-6d " % (label, p, f) + " ".join("%9.2e" % v for v in row))
    print("options:", list(OPTS))
    # Power: the PC's collapses (DESIGN 8): survival q against q0. At 0.1%, n = 45: q ~ 3.6/45 q0 (lost ~92%); a
    # milder one, lost 38% where frame loss allows 4%; at 0.01%, n = 45 lost 38% (q ~ 28/45 q0).
    print("P(detect) per 1 s window, a real collapse")
    for label, s, r, p, lc in [("BE45 0.1% lost92%", 45, 480, 1e-3, 0.92), ("BE45 0.1% lost38%", 45, 480, 1e-3, 0.38),
                               ("BE45 0.01% lost38%", 45, 480, 1e-4, 0.38), ("BE45 1% lost50%", 45, 480, 1e-2, 0.5),
                               ("BE45 1% lost70%", 45, 480, 1e-2, 0.7), ("n=4 0.1% lost3%", 4, 5400, 1e-3, 0.03),
                               ("n=16 0.1% lost10%", 16, 1350, 1e-3, 0.10), ("BE45 0.1% R=30 lost50%", 45, 30, 1e-3, 0.5)]:
        for f in fs:
            row = [p_collapse(o, p, f, r, s, lc) for o in OPTS.values()]
            print("%-22s F=%-6d " % (label, f) + " ".join("%9.3f" % v for v in row))



U, H = 1454.0, 26.0  # unit payload and MTU-unit overhead on 8/9 (DESIGN 8, Cost(n))


def cost(n, p):
    return (U + H / n) + (1.0 - (1.0 - p) ** n) * (U + H)


def drop(n):
    return max(1, int(n * 0.7 + 0.5))


def false_collapse_cost(n, p, units_per_s=723.0, tau=30.0):
    """Extra wire bytes and their share over 3 tau after a false collapse at n (RELIABLE: n = n*): tau at drop(n),
    tau at the plateau n - 1, then a probe and n again; a second the same as the clean link costs Cost(n)."""
    base = cost(n, p)
    extra = tau * units_per_s * ((cost(drop(n), p) - base) + (cost(max(1, n - 1), p) - base))
    return extra, extra / (3 * tau * units_per_s * base)


def report_cost():
    print("Cost of one false collapse, 1 MB at 30 Hz (723 units/s), tau 30 s, RELIABLE at n*")
    for p, n in [(1e-4, 13), (1e-3, 4), (5e-3, 2), (1e-4, 45), (1e-3, 45)]:
        extra, share = false_collapse_cost(n, p)
        print("p=%-6g n=%-2d drop=%-2d Cost(n)/u=%.4f Cost(drop)/u=%.4f extra=%.0f B over 90 s (%.3f%%)" %
              (p, n, drop(n), cost(n, p) / U, cost(drop(n), p) / U, extra, 100 * share))


if __name__ == "__main__":
    main()
    report_cost()
