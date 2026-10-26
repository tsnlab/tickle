#!/usr/bin/env python3
"""eth_storm_fix_read.py - the pre-registered reading of eth_storm_fix_ab.sh (RESULTS `EWS`, 2026-10-26).

Written and committed before the A/B's first run; this code, not this text, decides. Trees M (main), A (main again),
and two candidates C in (F, G): F = main + the per-unit back-off, G = the branch (the timeout held while the reader
shows progress). Labels M2 / A2 / F2 / G2 are second instances of the same trees and pool with them. Phases: x =
forced (a whole sample lost every 3 s), p = forced + the timeout pinned at ~1.5 ms, r4 = R4's cells, w<n> = Wi-Fi
rounds, e<n> = ETHERNET amp rounds.

Per rel_large rep: resends = the writer's retransmitted (ping.log RESULT role=pub), delivered / published, sub p50 /
p99 (echo.log RESULT latency_median_us / latency_p99_us), duplicates (sub large_duplicate), CPU (sub and pub cpu_ms),
the start load (meta.txt), forced losses (ping.log "EWS forced loss" lines), the reader's "Still waiting" warnings
(echo.log). A rep missing either RESULT is VOID and listed. Latency is read only from reps whose start load was <=
nproc / 2 (the harness's own load rule).

STORM (EWS's census rule): resends >= 100,000.
VOID (the run): in any cell, a tree with fewer than 0.9 x the reps of the tree with the most (the trees ran side by
  side, so an imbalance means an instance failed). TREATMENT: a forced arm (x, p) whose reps carry no "EWS forced
  loss" line is VOID (the drop never happened), and every verdict resting on it with it.
CONTROL (A/A), two-sided Fisher p >= 0.05 -> PASS, else FAIL (the verdict it controls is VOID):
  C1 natural: A against M, STORM reps in the e rounds. C2 pinned: A against M, STORM reps in phase p.
LOOP (phase p): M >= 3 STORM reps -> REACHABLE; else NOT REACHED (the provocation failed; LOOP verdicts UNDECIDED).
  Per candidate, read only when REACHABLE and C2 PASS: C's STORM reps fewer than M's with one-sided Fisher p <= 0.05
  -> BREAKS IT; else NOT SHOWN.
NATURAL (e rounds), per candidate, read only when C1 PASS: against M + A pooled (C's n against M's + A's n),
  one-sided Fisher p <= 0.05 -> FEWER SHOWN; M + A >= 5 and C's rate not below theirs -> NOT FEWER (FALSIFIED);
  otherwise UNDECIDED (main stormed too rarely to tell). Printed beside it, not deciding: counts by start-load band
  (< 2, 2-4, >= 4).
NO WORSE, per candidate and cell (e: ETHERNET ge0.1b1; x: ETHERNET forced; w: ge5b4 ge5b2 tv bc; r4: ctl and ge10b4
  rel_large), C against M with the A/A spread as the band. Metrics, worse direction in brackets: delivered % pooled
  [lower]; median per-rep sub p50 and p99 [higher]; median writer resends [higher]; median sub duplicates [higher];
  median sub cpu_ms and pub cpu_ms [higher]. band = max(|A - M|, floor). Floors, as fractions of M's value: p50 5%,
  p99 10%, resends 10%, duplicates 25%, CPU 5%; delivered 0.5 points (rel_large), 0.3 points (rel_small); 1 for a
  count whose M median is under 20 (RLF's floors, set there from a dry run of main against main). WORSE when C is
  beyond M by more than the band in the worse direction; else NOT WORSE. rel_small (r4 ge10b4): delivered % (the
  harness's summary.txt) and the median per-rep one-way p99 from owd_us.txt, same band rule. Beside it, not deciding:
  reps with a "Still waiting" warning per tree.
R4 (P0): the harness's own R4 line per tree (summary.txt). C's p50 - M's <= max(|A - M|, 2 ms) -> NOT WORSE, else
  WORSE. Each tree's own PASS/FAIL printed (main itself fails it at times).
LAND, per candidate: C1 and C2 PASS, LOOP BREAKS IT, NATURAL FEWER SHOWN or UNDECIDED (not FALSIFIED), every NO WORSE
  row NOT WORSE, R4 NOT WORSE, no VOID. A WORSE row is listed with its numbers; the lead decides on it.
PREFER: both land -> G (it keeps the slow reader's repairs and the warning test, which F fails in the sim); one lands
  -> that one; neither -> none.
Usage: eth_storm_fix_read.py OUT TAG
"""
import glob
import math
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
TREES = ("M", "A", "F", "G")
CANDIDATES = ("F", "G")
STORM = 100_000


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
    if n1 == 0 or n2 == 0:
        return float("nan")
    tot = math.comb(n, k)
    return sum(math.comb(n1, x) * math.comb(n2, k - x) / tot for x in range(max(0, k - n2), min(a, n1) + 1))


def fisher_two(a, n1, b, n2):
    k, n = a + b, n1 + n2
    if n1 == 0 or n2 == 0:
        return float("nan")
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
    rows, void = [], []
    nproc = os.cpu_count() or 1
    for d in sorted(glob.glob(os.path.join(out, f"{tag}_*", "runs", "*"))):
        run = os.path.basename(os.path.dirname(os.path.dirname(d)))[len(tag) + 1:]  # x_M, r4_G, w3_A, e5_F2
        phase, label = run.split("_", 1)
        tree = tree_of(label)
        if tree not in TREES:
            continue
        kind = phase if phase in ("x", "p", "r4") else phase[0]
        prof = "wifi" if kind in ("w", "r4") else "ethernet"
        arm, cell, rep = os.path.basename(d).rsplit("-", 2)
        meta = kv(" ".join(lines(os.path.join(d, "meta.txt"))))
        load = float(meta.get("load", 0) or 0)
        r = {"kind": kind, "prof": prof, "tree": tree, "label": label, "arm": arm, "cell": cell,
             "rep": f"{run}-{rep}", "load": load, "loaded": load > nproc / 2}
        if cell == "rel_large":
            ping = lines(os.path.join(d, "ping.log"))
            echo = lines(os.path.join(d, "echo.log"))
            pub = [kv(x) for x in ping if x.startswith("RESULT: role=pub")]
            sub = [kv(x) for x in echo if x.startswith("RESULT: role=sub")]
            if not pub or not sub:
                void.append(f"{kind} {prof} {arm} {r['rep']}: missing RESULT")
                continue
            p, s = pub[0], sub[0]
            r.update(resends=int(p.get("retransmitted", 0)), published=int(p.get("published", 0)),
                     delivered=int(s.get("delivered", 0)), dup=int(s.get("large_duplicate", 0)),
                     cpu_sub=float(s.get("cpu_ms", "nan")), cpu_pub=float(p.get("cpu_ms", "nan")),
                     forced=sum(1 for x in ping if "EWS forced loss" in x),
                     waiting=sum(1 for x in echo if "Still waiting" in x))
            r["storm"] = r["resends"] >= STORM
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


def band_check(name, worse_higher, m, a, c, floor, cname):
    if any(x is None or (isinstance(x, float) and math.isnan(x)) for x in (m, a, c)):
        return f"{name:14} M {m} A {a} {cname} {c}: VOID (no value)", "VOID"
    band = max(abs(a - m), floor)
    d = c - m
    worse = d > band if worse_higher else -d > band
    v = "WORSE" if worse else "NOT WORSE"
    return (f"{name:14} M {m:10.2f} A {a:10.2f} {cname} {c:10.2f}  {cname}-M {d:+9.2f} band {band:8.2f} -> {v}", v)


def med(v):
    v = [x for x in v if x is not None and not (isinstance(x, float) and math.isnan(x))]
    return statistics.median(v) if v else None


def storms(sel, t):
    n = [r for r in sel if r["tree"] == t]
    return sum(1 for r in n if r["storm"]), len(n)


def main():
    out, tag = sys.argv[1], sys.argv[2]
    rows, void = collect(out, tag)
    for v in void:
        print("VOID rep", v)
    large = [r for r in rows if r["cell"] == "rel_large"]
    run_void = []
    # ---- balance and treatment
    for kind in ("x", "p", "e", "w", "r4"):
        for arm in sorted({r["arm"] for r in large if r["kind"] == kind}):
            sel = [r for r in large if r["kind"] == kind and r["arm"] == arm]
            n = {t: sum(1 for r in sel if r["tree"] == t) for t in TREES}
            if max(n.values()) > 0 and min(n.values()) < 0.9 * max(n.values()):
                run_void.append(f"{kind} {arm}: reps {n}")
    treat_void = set()
    for kind in ("x", "p"):
        for t in TREES:
            sel = [r for r in large if r["kind"] == kind and r["tree"] == t]
            if not sel or sum(r["forced"] for r in sel) == 0:
                treat_void.add(kind)
                run_void.append(f"{kind} {t}: no forced loss (treatment absent) or no reps")
            else:
                print(f"TREATMENT {kind} {t}: {len(sel)} reps, forced losses a rep "
                      f"{sorted(set(r['forced'] for r in sel))}")
    for v in run_void:
        print("VOID run:", v)
    # ---- storms
    print("STORMS (resends >= 100,000), per phase:")
    for kind in ("x", "p", "e", "w"):
        sel = [r for r in large if r["kind"] == kind]
        if not sel:
            continue
        print(f"  {kind}: " + "  ".join("{} {}/{}".format(t, *storms(sel, t)) for t in TREES))
        for r in sorted(sel, key=lambda x: -x["resends"]):
            if r["storm"]:
                print(f"    STORM {r['tree']} {r['rep']} resends {r['resends']} delivered {r['delivered']}/"
                      f"{r['published']} load {r['load']:.1f}")
    nat = [r for r in large if r["kind"] == "e"]
    pin = [r for r in large if r["kind"] == "p"]
    km, nm = storms(nat, "M")
    ka, na = storms(nat, "A")
    p1 = fisher_two(ka, na, km, nm)
    c1 = "PASS" if p1 >= 0.05 else "FAIL"
    print(f"CONTROL C1 natural: A {ka}/{na} vs M {km}/{nm}, two-sided p {p1:.4f} -> {c1}")
    pkm, pnm = storms(pin, "M")
    pka, pna = storms(pin, "A")
    p2 = fisher_two(pka, pna, pkm, pnm)
    c2 = "PASS" if p2 >= 0.05 and "p" not in treat_void else "FAIL"
    print(f"CONTROL C2 pinned: A {pka}/{pna} vs M {pkm}/{pnm}, two-sided p {p2:.4f} -> {c2}")
    loop = "REACHABLE" if pkm >= 3 and "p" not in treat_void else "NOT REACHED"
    print(f"LOOP {loop} (pinned M {pkm} of {pnm})")
    verdict = {c: {} for c in CANDIDATES}
    for c in CANDIDATES:
        kc, nc = storms(pin, c)
        pl = fisher_less(kc, nc, pkm, pnm)
        if loop == "REACHABLE" and c2 == "PASS":
            lv = "BREAKS IT" if pl <= 0.05 else "NOT SHOWN"
        else:
            lv = "UNDECIDED"
        verdict[c]["loop"] = lv
        print(f"LOOP {c}: {kc}/{nc} against M {pkm}/{pnm}, one-sided p {pl:.4f} -> {lv}")
    for c in CANDIDATES:
        kc, nc = storms(nat, c)
        pn = fisher_less(kc, nc, km + ka, nm + na)
        if c1 != "PASS":
            nv = "VOID (control)"
        elif pn <= 0.05:
            nv = "FEWER SHOWN"
        elif km + ka >= 5 and nc and kc / nc >= (km + ka) / max(1, nm + na):
            nv = "NOT FEWER (FALSIFIED)"
        else:
            nv = "UNDECIDED"
        verdict[c]["natural"] = nv
        print(f"NATURAL {c}: {kc}/{nc} against M + A {km + ka}/{nm + na}, one-sided p {pn:.4f} -> {nv}")
    for lo, hi in ((0, 2), (2, 4), (4, 1e9)):
        sel = [r for r in nat if lo <= r["load"] < hi]
        print(f"  load [{lo}, {hi if hi < 1e9 else 'inf'}): " +
              "  ".join("{} {}/{}".format(t, *storms(sel, t)) for t in TREES))
    # ---- NO WORSE
    print("NO WORSE per cell (band = max(|A-M|, floor)):")
    cells = [("e", "ge0.1b1"), ("x", "ge0.1b1"), ("w", "ge5b4"), ("w", "ge5b2"), ("w", "tv"), ("w", "bc"),
             ("r4", "ctl"), ("r4", "ge10b4")]
    worse = {c: [] for c in CANDIDATES}
    for kind, arm in cells:
        sel = [r for r in large if r["kind"] == kind and r["arm"] == arm]
        if not sel:
            print(f"  {kind} {arm}: no reps")
            for c in CANDIDATES:
                worse[c].append(f"{kind} {arm}: no reps")
            continue
        by = {t: [r for r in sel if r["tree"] == t] for t in TREES}
        print(f"  {kind} {arm} rel_large reps " + " ".join(f"{t} {len(by[t])}" for t in TREES) +
              "   reps with a 'Still waiting' warning " +
              " ".join(f"{t} {sum(1 for r in by[t] if r['waiting'])}" for t in TREES))
        deliv = {t: 100.0 * sum(r["delivered"] for r in by[t]) / max(1, sum(r["published"] for r in by[t]))
                 for t in TREES}
        metrics = [("delivered %", False, deliv, 0.5)]
        for key, name in (("p50", "p50 ms"), ("p99", "p99 ms"), ("resends", "resends"), ("dup", "duplicates"),
                          ("cpu_sub", "cpu sub ms"), ("cpu_pub", "cpu pub ms")):
            vals = {t: med([r[key] for r in by[t]]) for t in TREES}
            share = {"p99": 0.10, "resends": 0.10, "dup": 0.25}.get(key, 0.05)
            floor = share * vals["M"] if vals["M"] is not None else 0
            if key in ("resends", "dup") and vals["M"] is not None and vals["M"] < 20:
                floor = 1
            metrics.append((name, True, vals, floor))
        for name, wh, vals, floor in metrics:
            for c in CANDIDATES:
                line, v = band_check(name, wh, vals["M"], vals["A"], vals[c], floor, c)
                print("    " + line)
                if v != "NOT WORSE":
                    worse[c].append(f"{kind} {arm} {name}: {line.strip()}")
    small = [r for r in rows if r["cell"] == "rel_small" and r["arm"] == "ge10b4"]
    if small:
        by = {t: [r for r in small if r["tree"] == t] for t in TREES}
        print("  r4 ge10b4 rel_small reps " + " ".join(f"{t} {len(by[t])}" for t in TREES))
        vals = {t: med([r["owd_p99"] for r in by[t]]) for t in TREES}
        dl = {}
        for t in TREES:
            for ln in lines(os.path.join(out, f"{tag}_r4_{t}", "summary.txt")):
                fl = ln.split()
                if len(fl) > 3 and fl[0] == "rel_small" and fl[1] == "ge10b4":
                    dl[t] = float(fl[3])
        for c in CANDIDATES:
            line, v = band_check("one-way p99", True, vals["M"], vals["A"], vals[c],
                                 0.10 * vals["M"] if vals["M"] else 0, c)
            print("    " + line)
            if v != "NOT WORSE":
                worse[c].append(f"r4 ge10b4 rel_small p99: {line.strip()}")
            if len(dl) == 4:
                line, v = band_check("delivered %", False, dl["M"], dl["A"], dl[c], 0.3, c)
                print("    " + line)
                if v != "NOT WORSE":
                    worse[c].append(f"r4 ge10b4 rel_small delivered: {line.strip()}")
            else:
                worse[c].append("r4 ge10b4 rel_small delivered: missing from summary.txt")
    else:
        for c in CANDIDATES:
            worse[c].append("r4 ge10b4 rel_small: no reps")
    for c in CANDIDATES:
        print(f"NO WORSE {c}: {'every row NOT WORSE' if not worse[c] else str(len(worse[c])) + ' row(s) WORSE or missing'}")
        for w in worse[c]:
            print("   ", w)
    # ---- R4
    r4 = {}
    for t in TREES:
        for ln in lines(os.path.join(out, f"{tag}_r4_{t}", "summary.txt")):
            if " R4: ge10b4 rel_large one-way p50" in ln:
                mm = re.search(r"p50 ([0-9.]+) ms \(<= ([0-9.]+) ms\)", ln)
                if mm:
                    r4[t] = (float(mm.group(1)), float(mm.group(2)), ln.split()[0])
    r4v = {}
    if len(r4) == 4:
        for t in TREES:
            print(f"R4 {t}: p50 {r4[t][0]:.2f} ms, limit {r4[t][1]:.2f} ms, own line {r4[t][2]}")
        band = max(abs(r4["A"][0] - r4["M"][0]), 2.0)
        for c in CANDIDATES:
            d = r4[c][0] - r4["M"][0]
            r4v[c] = "WORSE" if d > band else "NOT WORSE"
            print(f"R4 {c}-M {d:+.2f} ms, band {band:.2f} -> {r4v[c]}")
    else:
        print(f"R4 VOID: R4 line missing for {[t for t in TREES if t not in r4]}")
        r4v = {c: "VOID" for c in CANDIDATES}
    land = {}
    for c in CANDIDATES:
        v = verdict[c]
        land[c] = (c1 == "PASS" and c2 == "PASS" and v["loop"] == "BREAKS IT" and
                   v["natural"] in ("FEWER SHOWN", "UNDECIDED") and not worse[c] and r4v[c] == "NOT WORSE" and
                   not run_void)
        print(f"LAND {c}: {'yes' if land[c] else 'no'} (C1 {c1}; C2 {c2}; LOOP {v['loop']}; NATURAL {v['natural']}; "
              f"NO WORSE {'all' if not worse[c] else len(worse[c])}; R4 {r4v[c]}; VOID {len(run_void)})")
    pick = "G" if land["G"] else "F" if land["F"] else "none"
    print(f"PREFER: {pick}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
