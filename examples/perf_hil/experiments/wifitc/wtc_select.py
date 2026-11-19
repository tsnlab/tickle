#!/usr/bin/env python3
"""Stage C phase 1 selection (PLAN.md section 10): per class (M = M1+M2, S = S1+S2) the pacing arm with the lowest mean normalised median of
{p50, p99} over its two profiles x {128 KiB, 1 MiB}; qualifying = median delivery 15/15 and no run below 14/15 in every cell of the class.
The larger-burst arm replaces its rate-matched p90 only if its score is at least 10 % lower. Usage: wtc_select.py <Stage A dir>"""
import os, re, statistics, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wtc_read as R
root = sys.argv[1]
ARMS = ["tickle-p75", "tickle-p85", "tickle-p90", "tickle-p95", "tickle-p90-b24608"]
CLASS = {"M": ["M1", "M2"], "S": ["S1", "S2"]}
SIZES = {"rl128k": "128 KiB", "rl1m": "1 MiB"}
def runs(d, prof, fw):
    out = []
    ad = os.path.join(root, d, "rel_large", prof)
    for f in sorted(os.listdir(ad)):
        m = re.match(r"r(\d+)_(.+)_(A2?)\.done$", f)
        if m and m.group(2) == fw:
            x = R.run_metrics(os.path.join(ad, f[:-5]), "rel_large")
            if x: out.append((m.group(3), x))
    return out
def med(v): return statistics.median(v)
res = {}
for cls, profs in CLASS.items():
    cells = {}
    for d in SIZES:
        for p in profs:
            for fw in ARMS:
                rs = runs(d, p, fw)
                if fw.endswith("b24608") and d == "rl128k": continue
                cells[(d, p, fw)] = rs
    print(f"\n=== class {cls} ({', '.join(profs)}) ===")
    score = {}; qual = {}
    for fw in ARMS:
        tot = []; ok = True; notes = []
        for d in SIZES:
            if fw.endswith("b24608") and d == "rl128k": continue
            for p in profs:
                rs = cells[(d, p, fw)]
                n = len(rs)
                dl = [float(x["n"].split("/")[0]) for _, x in rs]
                if n < 8: notes.append(f"{d}/{p} only {n} runs")
                if not dl or med(dl) < 15 or min(dl) < 14: ok = False; notes.append(f"{d}/{p} delivery med {med(dl) if dl else 0:.0f} min {min(dl) if dl else 0:.0f}")
                for met in ("p50", "p99"):
                    allm = []
                    for a in ARMS:
                        if a.endswith("b24608") and d == "rl128k": continue
                        vs = [x[met] for _, x in cells[(d, p, a)]]
                        if vs: allm.append(med(vs))
                    mine = [x[met] for _, x in rs]
                    tot.append(med(mine) / min(allm))
        score[fw] = sum(tot) / len(tot) if tot else float("inf")
        qual[fw] = ok
        print(f"{fw:20s} score {score[fw]:.3f} qualifies={ok} {'; '.join(notes)}")
    cand = [a for a in ARMS if qual[a]]
    rate_arms = [a for a in cand if not a.endswith("b24608")]
    best = min(rate_arms, key=lambda a: (score[a], ARMS.index(a))) if rate_arms else None
    if best == "tickle-p90" and "tickle-p90-b24608" in cand and score["tickle-p90-b24608"] <= 0.9 * score["tickle-p90"]: best = "tickle-p90-b24608"
    print("CHOSEN", cls, best)
    res[cls] = best
    # oracle table: per profile and size, the arm with the lowest median p50 (information for the adaptive-design placeholders)
    for d in SIZES:
        for p in profs:
            best_o = None
            for a in ARMS:
                if a.endswith("b24608") and d == "rl128k": continue
                rs = cells[(d, p, a)]
                if not rs: continue
                dl = [float(x["n"].split("/")[0]) for _, x in rs]
                p50 = med([x["p50"] for _, x in rs]); p99 = med([x["p99"] for _, x in rs])
                if best_o is None or p50 < best_o[1]: best_o = (a, p50, p99, med(dl), min(dl), len(rs))
            print(f"  oracle {SIZES[d]} {p}: {best_o[0]} p50 {best_o[1]:.0f} ms p99 {best_o[2]:.0f} ms delivered med {best_o[3]:.0f} min {best_o[4]:.0f} ({best_o[5]} runs)")
            for a in ARMS:
                if a.endswith("b24608") and d == "rl128k": continue
                rs = cells[(d, p, a)]
                print(f"      {a:20s} p50 {med([x['p50'] for _, x in rs]):7.0f} p99 {med([x['p99'] for _, x in rs]):7.0f} deliv {[x['n'].split('/')[0] for _, x in rs]}")
print("\nSELECTED", res)
