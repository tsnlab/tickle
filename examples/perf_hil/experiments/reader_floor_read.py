#!/usr/bin/env python3
"""reader_floor_read.py - the pre-registered reading of reader_floor_ab.sh (RESULTS `RLF`, 2026-10-26).

Written and committed before the A/B's first run; this code, not this text, decides. Trees M (main), A (main again),
F (the floor). Labels M2/A2/F2 are second instances of the same trees and pool with them.

Per rel_large rep: timer = the reader's timer ACKNACKs (echo.log RSTATS acknack_tx timer=), resends = the writer's
retransmitted (ping.log RESULT), delivered / published, sub p50 / p99 (echo.log RESULT latency_median_us /
latency_p99_us), duplicates (sub large_duplicate), CPU (sub and pub cpu_ms). A rep missing either RESULT is VOID and
listed. Latency is read only from reps whose start load was <= nproc / 2 (the harness's own load rule).

COLLAPSE (reader_collapse_read.py's rule, verbatim): BASE = the median rep's timer over every rep of that profile + arm
  in this run (all trees pooled); COLLAPSE = timer > 5 x BASE and timer >= 500.
VOID (the run): on any census cell, a tree with fewer than 0.9 x the reps of the tree with the most (the trees ran
  side by side, so an imbalance means an instance failed).
CONTROL (A/A): A against M, COLLAPSE reps pooled over the five census cells, Fisher two-sided p >= 0.05 -> PASS;
  else VOID - two builds of the same source differing that much means the test cannot tell trees apart.
COLLAPSE VERDICT (primary, pooled over the five census cells: WIFI ge5b4 ge5b2 tv bc, ETHERNET ge0.1b1):
  F < M, Fisher one-sided p < 0.05 -> FIXED. F >= M with M >= 3 -> NOT FIXED (FALSIFIED). Otherwise UNDECIDED (M
  collapsed too rarely to tell). Printed beside it, not deciding: F against M + A pooled, and each cell alone.
NO WORSE, per cell (the five census cells and P0's ctl / ge10b4 rel_large, ge10b4 rel_small), F against M with the
  A/A spread as the band. Metrics, worse direction in brackets: delivered % pooled [lower]; median per-rep sub p50 and
  p99 [higher]; median writer resends [higher]; median sub duplicates [higher]; median sub cpu_ms and pub cpu_ms
  [higher]. band = max(|A - M|, floor). Floors, as fractions of M's value: p50 5%, p99 10%, resends 10%, duplicates
  25%, CPU 5%; delivered 0.5 points (rel_large), 0.3 points (rel_small); 1 for a count whose M median is under 20.
  WORSE when F is beyond M by more than the band in the worse direction; else NOT WORSE.
  rel_small (P0): delivered % (summary.txt) and median per-rep one-way p99 from owd_us.txt, same band rule.
  The floors were set before the run from a dry run of this reader on the census's main-only reps (rlc1 T1 / T2 / U as
  M / A / F): with 0.1 points and 5% everywhere it called main WORSE than main on ge5b4 delivered (one U rep short:
  99.53 against 99.98%) and tv duplicates (+19%).
R4 (P0): the harness's own R4 line per tree (summary.txt: ge10b4 rel_large pooled one-way p50 against ctl p50 +
  4 x base_rtt; base_rtt printed). F's p50 - M's <= max(|A - M|, 2 ms) -> NOT WORSE, else WORSE. PASS/FAIL of each
  tree's own line printed: main already fails it at times (k5d: M 107 ms against a 74 ms limit), so PASS is not
  required, NOT WORSE is.
LAND: CONTROL PASS, COLLAPSE FIXED, every NO WORSE row NOT WORSE, R4 NOT WORSE, no VOID. A WORSE row is listed with
  its numbers; the lead decides on it.
Usage: reader_floor_read.py OUT TAG
"""
import glob
import math
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
TREES = ("M", "A", "F")
CENSUS = [("wifi", "ge5b4"), ("wifi", "ge5b2"), ("wifi", "tv"), ("wifi", "bc"), ("ethernet", "ge0.1b1")]


def kv(line):
    return dict(KV.findall(line))


def lines(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def fisher_less(a, n1, b, n2):
    """One-sided P(X <= a) for group 1's count, hypergeometric."""
    k, n = a + b, n1 + n2
    tot = math.comb(n, k)
    return sum(math.comb(n1, x) * math.comb(n2, k - x) / tot for x in range(max(0, k - n2), min(a, n1) + 1))


def fisher_two(a, n1, b, n2):
    k, n = a + b, n1 + n2
    tot = math.comb(n, k)
    pr = {x: math.comb(n1, x) * math.comb(n2, k - x) / tot for x in range(max(0, k - n2), min(k, n1) + 1)}
    p0 = pr.get(a, 0.0)
    return min(1.0, sum(v for v in pr.values() if v <= p0 * (1 + 1e-9)))


def q(v, f):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * f))] if v else float("nan")


def tree_of(label):
    return label.rstrip("0123456789")


def collect(out, tag):
    """Every rep: dict with phase, prof, tree, arm, cell, rep and metrics."""
    rows, void = [], []
    nproc = os.cpu_count() or 1
    for d in sorted(glob.glob(os.path.join(out, f"{tag}_*", "runs", "*"))):
        run = os.path.basename(os.path.dirname(os.path.dirname(d)))[len(tag) + 1:]  # r4_M, w3_A2, e5_F
        phase, label = run.split("_", 1)
        tree = tree_of(label)
        if tree not in TREES:
            continue
        prof = "ethernet" if phase.startswith("e") else "wifi"
        arm, cell, rep = os.path.basename(d).rsplit("-", 2)
        meta = kv(" ".join(lines(os.path.join(d, "meta.txt"))))
        r = {"phase": phase[0], "prof": prof, "tree": tree, "label": label, "arm": arm, "cell": cell,
             "rep": f"{run}-{rep}", "loaded": float(meta.get("load", 0) or 0) > nproc / 2}
        if cell == "rel_large":
            pub = [kv(x) for x in lines(os.path.join(d, "ping.log")) if x.startswith("RESULT: role=pub")]
            sub = [kv(x) for x in lines(os.path.join(d, "echo.log")) if x.startswith("RESULT: role=sub")]
            timer = [kv(x) for x in lines(os.path.join(d, "echo.log")) if x.startswith("RSTATS: role=sub acknack_tx")]
            if not pub or not sub or not timer:
                void.append(f"{prof} {arm} {r['rep']}: missing RESULT/RSTATS")
                continue
            p, s = pub[0], sub[0]
            published = int(p.get("published", 0))
            r.update(timer=int(timer[-1].get("timer", 0)), resends=int(p.get("retransmitted", 0)),
                     published=published, delivered=int(s.get("delivered", 0)),
                     dup=int(s.get("large_duplicate", 0)), cpu_sub=float(s.get("cpu_ms", "nan")),
                     cpu_pub=float(p.get("cpu_ms", "nan")))
            for key, f in (("p50", "latency_median_us"), ("p99", "latency_p99_us")):
                v = s.get(f, "nan")
                r[key] = float(v) / 1000 if v != "nan" and not r["loaded"] else None
        elif cell == "rel_small":
            owd = [int(x) for x in lines(os.path.join(d, "owd_us.txt")) if x.strip().isdigit()]
            r["owd_p99"] = q(owd, 0.99) / 1000 if owd and not r["loaded"] else None
        else:
            continue
        rows.append(r)
    return rows, void


def band_check(name, worse_higher, m, a, f, floor):
    if any(x is None or (isinstance(x, float) and math.isnan(x)) for x in (m, a, f)):
        return f"{name:18} M {m} A {a} F {f}: VOID (no value)", "VOID"
    band = max(abs(a - m), floor)
    d = f - m
    worse = d > band if worse_higher else -d > band
    v = "WORSE" if worse else "NOT WORSE"
    return f"{name:18} M {m:10.2f} A {a:10.2f} F {f:10.2f}  F-M {d:+9.2f} band {band:8.2f} -> {v}", v


def med(v):
    v = [x for x in v if x is not None]
    return statistics.median(v) if v else None


def main():
    out, tag = sys.argv[1], sys.argv[2]
    rows, void = collect(out, tag)
    verdicts = []
    for v in void:
        print("VOID rep", v)
    large = [r for r in rows if r["cell"] == "rel_large"]
    # ---- COLLAPSE, the census rule
    base_of = {}
    for key in {(r["prof"], r["arm"]) for r in large if r["phase"] != "r"}:
        base_of[key] = statistics.median(r["timer"] for r in large if (r["prof"], r["arm"]) == key and r["phase"] != "r")
    census = [r for r in large if r["phase"] != "r"]
    for r in census:
        b = base_of[(r["prof"], r["arm"])]
        r["collapse"] = r["timer"] > 5 * b and r["timer"] >= 500
    print("OCCURRENCE (rel_large, COLLAPSE: timer > 5 x BASE and >= 500; BASE over all trees):")
    run_void = []
    tot = {t: [0, 0] for t in TREES}
    for prof, arm in CENSUS:
        cell = [r for r in census if (r["prof"], r["arm"]) == (prof, arm)]
        n = {t: sum(1 for r in cell if r["tree"] == t) for t in TREES}
        k = {t: sum(1 for r in cell if r["tree"] == t and r["collapse"]) for t in TREES}
        for t in TREES:
            tot[t][0] += k[t]
            tot[t][1] += n[t]
        if n and max(n.values()) > 0 and min(n.values()) < 0.9 * max(n.values()):
            run_void.append(f"{prof} {arm}: reps {n}")
        b = base_of.get((prof, arm), float("nan"))
        p = fisher_less(k["F"], n["F"], k["M"], n["M"]) if n["F"] and n["M"] else float("nan")
        print(f"  {prof:8} {arm:8} BASE {b:7.0f}  " + "  ".join(f"{t} {k[t]}/{n[t]}" for t in TREES) +
              f"  (F<M one-sided p {p:.4f}, not deciding)")
        for r in sorted(cell, key=lambda x: -x["timer"]):
            if r["collapse"]:
                print(f"    COLLAPSE {r['tree']} {r['rep']} timer {r['timer']} resends {r['resends']} dup {r['dup']}")
    if not census:
        run_void.append("no census reps")
    for v in run_void:
        print("VOID run (unbalanced):", v)
    (km, nm), (ka, na), (kf, nf) = tot["M"], tot["A"], tot["F"]
    pc = fisher_two(ka, na, km, nm) if na and nm else float("nan")
    control = "PASS" if pc >= 0.05 else "FAIL (VOID)"
    print(f"CONTROL A {ka}/{na} vs M {km}/{nm}: two-sided p {pc:.4f} -> {control}")
    pf = fisher_less(kf, nf, km, nm) if nf and nm else float("nan")
    if pf < 0.05:
        cv = "FIXED"
    elif kf >= km and km >= 3:
        cv = "NOT FIXED (FALSIFIED)"
    else:
        cv = "UNDECIDED"
    pma = fisher_less(kf, nf, km + ka, nm + na) if nf and nm + na else float("nan")
    print(f"COLLAPSE F {kf}/{nf} vs M {km}/{nm}: one-sided p {pf:.2e} -> {cv}   (beside it: F vs M+A "
          f"{km + ka}/{nm + na}, p {pma:.2e})")
    # ---- NO WORSE
    print("NO WORSE per cell (band = max(|A-M|, floor)):")
    worse = []
    cells = [(p, a, "rel_large") for p, a in CENSUS] + [("wifi", "ctl", "rel_large"), ("wifi", "ge10b4", "rel_large")]
    for prof, arm, cellname in cells:
        sel = [r for r in large if (r["prof"], r["arm"]) == (prof, arm) and
               (r["phase"] == "r") == (arm in ("ctl", "ge10b4"))]
        if not sel:
            print(f"  {prof} {arm} {cellname}: no reps")
            worse.append(f"{prof} {arm}: no reps")
            continue
        by = {t: [r for r in sel if r["tree"] == t] for t in TREES}
        print(f"  {prof} {arm} {cellname} reps " + " ".join(f"{t} {len(by[t])}" for t in TREES))
        deliv = {t: 100.0 * sum(r["delivered"] for r in by[t]) / max(1, sum(r["published"] for r in by[t]))
                 for t in TREES}
        checks = [("delivered %", False, deliv["M"], deliv["A"], deliv["F"], 0.5)]
        for key, name in (("p50", "p50 ms"), ("p99", "p99 ms"), ("resends", "resends"), ("dup", "duplicates"),
                          ("cpu_sub", "cpu sub ms"), ("cpu_pub", "cpu pub ms")):
            m, a, f = (med([r[key] for r in by[t]]) for t in TREES)
            share = {"p99": 0.10, "resends": 0.10, "dup": 0.25}.get(key, 0.05)
            floor = share * m if m is not None else 0
            if key in ("resends", "dup") and m is not None and m < 20:
                floor = 1
            checks.append((name, True, m, a, f, floor))
        for name, wh, m, a, f, floor in checks:
            line, v = band_check(name, wh, m, a, f, floor)
            print("    " + line)
            if v != "NOT WORSE":
                worse.append(f"{prof} {arm} {name}: {line.strip()}")
    small = [r for r in rows if r["cell"] == "rel_small" and r["arm"] == "ge10b4"]
    if small:
        by = {t: [r for r in small if r["tree"] == t] for t in TREES}
        print("  wifi ge10b4 rel_small reps " + " ".join(f"{t} {len(by[t])}" for t in TREES))
        m, a, f = (med([r["owd_p99"] for r in by[t]]) for t in TREES)
        line, v = band_check("one-way p99 ms", True, m, a, f, 0.10 * m if m else 0)
        print("    " + line)
        if v != "NOT WORSE":
            worse.append(f"wifi ge10b4 rel_small p99: {line.strip()}")
        dl = {}
        for t in TREES:
            for ln in lines(os.path.join(out, f"{tag}_r4_{t}", "summary.txt")):
                fl = ln.split()
                if len(fl) > 3 and fl[0] == "rel_small" and fl[1] == "ge10b4":
                    dl[t] = float(fl[3])
        if len(dl) == 3:
            line, v = band_check("delivered %", False, dl["M"], dl["A"], dl["F"], 0.3)
            print("    " + line)
            if v != "NOT WORSE":
                worse.append(f"wifi ge10b4 rel_small delivered: {line.strip()}")
        else:
            worse.append("wifi ge10b4 rel_small delivered: missing from summary.txt")
    else:
        worse.append("wifi ge10b4 rel_small: no reps")
    print(f"NO WORSE: {'every row NOT WORSE' if not worse else str(len(worse)) + ' row(s) WORSE or missing'}")
    for w in worse:
        print("  ", w)
    # ---- R4
    r4 = {}
    for t in TREES:
        for ln in lines(os.path.join(out, f"{tag}_r4_{t}", "summary.txt")):
            if " R4: ge10b4 rel_large one-way p50" in ln:
                mm = re.search(r"p50 ([0-9.]+) ms \(<= ([0-9.]+) ms\)", ln)
                if mm:
                    r4[t] = (float(mm.group(1)), float(mm.group(2)), ln.split()[0])
            if ln.startswith("info  BASE:"):
                print(f"R4 {t} {ln.strip()}")
    if len(r4) == 3:
        for t in TREES:
            print(f"R4 {t}: p50 {r4[t][0]:.2f} ms, limit {r4[t][1]:.2f} ms, own line {r4[t][2]}")
        d = r4["F"][0] - r4["M"][0]
        band = max(abs(r4["A"][0] - r4["M"][0]), 2.0)
        r4v = "WORSE" if d > band else "NOT WORSE"
        print(f"R4 F-M {d:+.2f} ms, band {band:.2f} -> {r4v}")
    else:
        r4v = "VOID"
        print(f"R4 VOID: R4 line missing for {[t for t in TREES if t not in r4]}")
    land = control == "PASS" and cv == "FIXED" and not worse and r4v == "NOT WORSE" and not run_void
    print(f"LAND: {'yes' if land else 'no'} (CONTROL {control}; COLLAPSE {cv}; NO WORSE "
          f"{'all' if not worse else len(worse)}; R4 {r4v}; VOID {len(run_void)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
