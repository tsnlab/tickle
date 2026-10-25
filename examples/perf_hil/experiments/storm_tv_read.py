#!/usr/bin/env python3
"""storm_tv_read.py - the pre-registered reading of storm_tv_ab.sh (the tv repair storm, RESULTS `TVS`, 2026-10-25).

Written before the fix's first netns run; storm_tv_ab.sh's header states the hypothesis. Applied here, in code:

Per rep (rel_large, the writer's ping.log and the reader's echo.log, LARGE_RSTATS=1):
  resends     the writer's RESULT retransmitted
  collapse    releases by the repair timeout while the timeout stood under 1.024 ms (RSTATS hist=lro_timeout_value,
              bins lt1us..lt1024us): the mechanism. The path's round trip is >= 4 ms (2 ms netem delay each way), so a
              timeout under 1 ms stands on a sample no repair can have taken. A COLLAPSE rep has one or more.
  STORM       resends > 10 x BASE; NEAR resends > 2 x BASE, where BASE is the median rep's resends over the main trees
              (labels M and A) on the same arm - one bar for every tree, so a tree that resends less is not judged by
              its own lower median.
  reader-driven  the reader's timer ACKNACKs > 10 x the main trees' median rep: the reader's own retry estimate
              collapsed (R4K's K2 note), which the fix does not touch. Listed, and NEAR counted with and without them.
  latency     the reader's RESULT latency_median_us and latency_p99_us (150 samples a rep, p99 the 2nd largest).
  delivered   the harness's own pooled delivered % (summary.txt), which scores the samples offered after the match: a rep
              that matched late under loss (R4K: sample 17) is not a loss.
  loaded      meta.txt load > nproc / 2: listed; not left out (the storm needs no load), but counted per tree.

PHASE=tv verdicts (trees M, A = main twice, F = the fix; K, FK optional):
  CONTROL (A/A): M against A, COLLAPSE reps and NEAR reps, two-sided Fisher. p < 0.05 on either: VOID - two checkouts
    of one code differ, so nothing below can be read.
  MECHANISM: F SHOWN when F has 0 COLLAPSE reps and the one-sided Fisher of COLLAPSE reps (M+A against F) gives
    p < 0.05; FALSIFIED when F has any COLLAPSE rep (the fix does not remove the mechanism) or any STORM rep (the
    storm has another cause too); else UNDECIDED (main collapsed too rarely to tell).
  OUTCOME: NEAR reps that are not reader-driven, M+A against F, one-sided Fisher: SHOWN at p < 0.05, else UNDECIDED;
    F with more such reps than the main trees' rate * its reps + 2: WORSE.
  NOT WORSE on tv (each metric: F's value against LIMIT = max(M, A) + |M - A| + 5% of max(M, A), for a lower-is-better
    metric; delivered: F >= min(M, A) - 0.1 point): delivered % pooled, median rep p50, median rep p99.
  K / FK (when present): COLLAPSE and STORM reps of each, printed: "FK still storms" when FK has any STORM or COLLAPSE.
PHASE=arms (ge5b2 ge5b4 ge10b4 bc): per arm, F NOT WORSE when each of delivered, median rep p50, median rep p99 and
  median rep resends is within its LIMIT (resends: 10% instead of 5%); any one outside it is WORSE on that arm. The arms'
  pre-registered reading: no WORSE on any arm.
Usage: storm_tv_read.py BASE_OUT TAG PHASE LABEL...
"""
import math
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")


def kv(line):
    return dict(KV.findall(line))


def read(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []


def fisher_one_sided(a, n1, b, n2):
    """P(X >= a) for X the count in group 1 under the hypergeometric of a + b events over n1 + n2 reps."""
    k = a + b
    if k == 0:
        return 1.0
    total = math.comb(n1 + n2, k)
    return sum(math.comb(n1, x) * math.comb(n2, k - x) for x in range(a, min(k, n1) + 1)) / total


def fisher_two_sided(a, n1, b, n2):
    k = a + b
    total = math.comb(n1 + n2, k)
    probs = {x: math.comb(n1, x) * math.comb(n2, k - x) / total for x in range(max(0, k - n2), min(k, n1) + 1)}
    p0 = probs.get(a, 0.0)
    return min(1.0, sum(p for p in probs.values() if p <= p0 * (1 + 1e-9)))


def rep_rows(base, tag, lab):
    out = os.path.join(base, f"{tag}_{lab}", "runs")
    rows = []
    nproc = os.cpu_count() or 1
    for name in sorted(os.listdir(out)) if os.path.isdir(out) else []:
        arm, cell, rep = name.rsplit("-", 2)
        if cell != "rel_large":
            continue
        d = os.path.join(out, name)
        ping, echo = read(os.path.join(d, "ping.log")), read(os.path.join(d, "echo.log"))
        pres = [kv(x) for x in ping if x.startswith("RESULT")]
        sres = [kv(x) for x in echo if x.startswith("RESULT")]
        if not pres or not sres:
            rows.append({"arm": arm, "rep": rep, "void": "no RESULT"})
            continue
        collapse = 0
        for x in ping:
            if x.startswith("RSTATS: role=pub hist=lro_timeout_value"):
                for k, v in kv(x).items():
                    m = re.match(r"lt(\d+)us$", k)
                    if m and int(m.group(1)) <= 1024:
                        collapse += int(v)
        timer = 0
        for x in echo:
            if x.startswith("RSTATS: role=sub acknack_tx"):
                timer = int(kv(x).get("timer", 0))
        meta = kv(" ".join(read(os.path.join(d, "meta.txt"))))
        rows.append({"arm": arm, "rep": rep, "resends": int(pres[0].get("retransmitted", 0)),
                     "published": int(pres[0].get("published", 0)), "delivered": int(sres[0].get("delivered", 0)),
                     "p50": float(sres[0].get("latency_median_us", "nan")) / 1000,
                     "p99": float(sres[0].get("latency_p99_us", "nan")) / 1000, "collapse": collapse,
                     "timer": timer, "loaded": float(meta.get("load", 0)) > nproc / 2})
    return rows


def summary_delivered(base, tag, lab):
    out = {}
    for line in read(os.path.join(base, f"{tag}_{lab}", "summary.txt")):
        f = line.split()
        if len(f) > 3 and f[0] == "rel_large":
            try:
                out[f[1]] = float(f[3])
            except ValueError:
                pass
    return out


def med(v):
    v = [x for x in v if x == x]
    return statistics.median(v) if v else float("nan")


def limit(m, a, frac):
    hi = max(m, a)
    return hi + abs(m - a) + frac * hi


def main():
    base, tag, phase, labels = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    data = {lab: [r for r in rep_rows(base, tag, lab) if "void" not in r] for lab in labels}
    for lab in labels:
        voids = [r for r in rep_rows(base, tag, lab) if "void" in r]
        for r in voids:
            print(f"VOID rep {lab} {r['arm']}-{r['rep']}: {r['void']}")
    deliv = {lab: summary_delivered(base, tag, lab) for lab in labels}
    arms = sorted({r["arm"] for rows in data.values() for r in rows})
    if "M" not in data or "A" not in data or "F" not in data:
        print("VOID: trees M, A and F are required")
        return 1
    for arm in arms:
        by = {lab: [r for r in data[lab] if r["arm"] == arm] for lab in labels}
        main_rows = by["M"] + by["A"]
        if not main_rows:
            continue
        base_r = med([r["resends"] for r in main_rows])
        base_t = med([r["timer"] for r in main_rows])
        print(f"=== arm {arm}: BASE (main trees' median rep resends) {base_r:.0f}, reader timer ACKNACKs median {base_t:.0f}")
        stats = {}
        for lab in labels:
            rows = by[lab]
            n = len(rows)
            col = [r for r in rows if r["collapse"] > 0]
            storm = [r for r in rows if r["resends"] > 10 * base_r]
            near = [r for r in rows if r["resends"] > 2 * base_r]
            rdrv = [r for r in rows if base_t > 0 and r["timer"] > 10 * base_t]
            near_nr = [r for r in near if r not in rdrv]
            stats[lab] = {"n": n, "col": len(col), "storm": len(storm), "near": len(near), "near_nr": len(near_nr),
                          "deliv": deliv[lab].get(arm, float("nan")), "p50": med([r["p50"] for r in rows]),
                          "p99": med([r["p99"] for r in rows]), "resends": med([r["resends"] for r in rows])}
            print(f"  {lab:3} reps {n:3} (loaded {sum(r['loaded'] for r in rows)}) COLLAPSE {len(col):2} STORM "
                  f"{len(storm):2} NEAR {len(near):2} (not reader-driven {len(near_nr):2}) reader-driven {len(rdrv):2} "
                  f"delivered {stats[lab]['deliv']:.2f}% median rep p50 {stats[lab]['p50']:.1f} ms p99 "
                  f"{stats[lab]['p99']:.1f} ms resends {stats[lab]['resends']:.0f}")
            print(f"      resends {[r['resends'] for r in rows]}")
            if col:
                print(f"      COLLAPSE reps {[(r['rep'], r['collapse'], r['resends']) for r in col]}")
        m, a, f = stats["M"], stats["A"], stats["F"]
        if phase == "tv":
            p_col = fisher_two_sided(m["col"], m["n"], a["col"], a["n"])
            p_near = fisher_two_sided(m["near"], m["n"], a["near"], a["n"])
            void = p_col < 0.05 or p_near < 0.05
            print(f"CONTROL A/A {arm}: COLLAPSE M {m['col']}/{m['n']} A {a['col']}/{a['n']} p {p_col:.3f}; NEAR "
                  f"M {m['near']}/{m['n']} A {a['near']}/{a['n']} p {p_near:.3f}: {'VOID' if void else 'PASS'}")
            mc, mn = m["col"] + a["col"], m["n"] + a["n"]
            p_mech = fisher_one_sided(mc, mn, f["col"], f["n"])
            if void:
                mech = "VOID (the control failed)"
            elif f["col"] > 0 or f["storm"] > 0:
                mech = f"FALSIFIED (F COLLAPSE {f['col']}, STORM {f['storm']})"
            elif p_mech < 0.05:
                mech = "SHOWN"
            else:
                mech = "UNDECIDED"
            print(f"MECHANISM {arm}: COLLAPSE main {mc}/{mn} F {f['col']}/{f['n']} one-sided p {p_mech:.4f}: {mech}")
            mnr = m["near_nr"] + a["near_nr"]
            p_out = fisher_one_sided(mnr, mn, f["near_nr"], f["n"])
            if f["near_nr"] > (mnr / mn) * f["n"] + 2:
                out = "WORSE"
            elif p_out < 0.05:
                out = "SHOWN"
            else:
                out = "UNDECIDED"
            print(f"OUTCOME {arm}: NEAR not reader-driven main {mnr}/{mn} F {f['near_nr']}/{f['n']} one-sided p "
                  f"{p_out:.4f}: {out}")
        worse = []
        if f["deliv"] < min(m["deliv"], a["deliv"]) - 0.1:
            worse.append(f"delivered {f['deliv']:.2f}% < {min(m['deliv'], a['deliv']):.2f}% - 0.1")
        for key in ("p50", "p99") + (("resends",) if phase != "tv" else ()):
            frac = 0.10 if key == "resends" else 0.05
            lim = limit(m[key], a[key], frac)
            if f[key] > lim:
                worse.append(f"{key} {f[key]:.1f} > limit {lim:.1f}")
        print(f"NOT-WORSE {arm}: {'WORSE (' + '; '.join(worse) + ')' if worse else 'NOT WORSE'}")
        for lab in ("K", "FK"):
            if lab in stats:
                s = stats[lab]
                tail = " - FK still storms" if lab == "FK" and (s["storm"] or s["col"]) else ""
                print(f"READING {lab} {arm}: COLLAPSE {s['col']}/{s['n']} STORM {s['storm']}/{s['n']}{tail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
