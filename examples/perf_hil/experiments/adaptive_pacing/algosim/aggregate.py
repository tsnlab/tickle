#!/usr/bin/env python3
"""Aggregates the algosim result lines into the tables of DESIGN section 27. usage: aggregate.py <res dir> [markdown out] [csv out]"""
import collections, glob, math, os, re, statistics, sys

res, md_out = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else None)
csv_out = sys.argv[3] if len(sys.argv) > 3 else None
FAM = [("Varying bandwidth (P3 P4 P5a P5b P6 P7)", ["P3", "P4", "P5a", "P5b", "P6", "P7"]),
       ("Rate step 30 -> 4 -> 30 Mbit/s (RS aligned with the 4 s sample grid, RSm not, RSd steps down inside a transfer)", ["RS", "RSm", "RSd"]),
       ("Static links (M1 M2 S1 S2)", ["M1", "M2", "S1", "S2"]),
       ("5G-like (N1 N2 N3 N4)", ["N1", "N2", "N3", "N4"]),
       ("High-rate static, startup (H100 H200)", ["H100", "H200"])]
SIZE = {89: "128 KiB", 723: "1 MiB"}
INF = float("inf")

def parse(path):
    line = open(path).read().strip()
    head, _, lat = line.partition(" lat=")
    d = {k: float(v) for k, v in (kv.split("=") for kv in head.split())}
    d["lat"] = [float(x) for x in lat.split(",") if x]
    return d

runs = collections.defaultdict(dict)  # (wl, units, cand, fb) -> seed -> dict
for p in glob.glob(f"{res}/*.txt"):
    m = re.match(r"(.+?)_(\d+)_(.+)_([ab])_(\d+)\.txt$", os.path.basename(p))
    if not m or os.path.getsize(p) == 0:
        continue
    wl, units, cand, fb, seed = m.groups()
    runs[(wl, int(units), cand, fb)][int(seed)] = parse(p)

def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q / 100.0))] if v else INF

def lat_stats(rl):
    """pooled over seeds; an undelivered sample counts as infinite."""
    allv = []
    for r in rl.values():
        allv += r["lat"] + [INF] * int(r["undeliv"])
    return pct(allv, 50), pct(allv, 95), pct(allv, 99)

def seed_p50(rl):
    out = {}
    for s, r in rl.items():
        v = sorted(r["lat"] + [INF] * int(r["undeliv"]))
        out[s] = v[len(v) // 2] if v else INF
    return out

cands = sorted({(c, f) for (_, _, c, f) in runs}, key=lambda x: (x[0] not in ("oracle", "fixed"), x[0], x[1]))
def label(c, f):
    return c if c in ("fixed", "oracle") else f"{c}-{f}"

def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else float("nan")

def geomean(xs):
    xs = [x for x in xs if x not in (None, INF) and x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")

def verdict(a, b):
    """paired per seed: W if a < b in at least 5 of 6 (with a 5 % margin), L if the reverse, else T"""
    w = sum(1 for s in a if s in b and a[s] < 0.95 * b[s])
    l = sum(1 for s in a if s in b and b[s] < 0.95 * a[s])
    n = len([s for s in a if s in b])
    need = max(1, n - 1)
    return "W" if w >= need else ("L" if l >= need else "T")

out = []
csv = ["family,size,wl,cand,fb,p50,p95,p99,in30,over13,under07,q100,retx,undeliv_late_pct,up,down,startup,gp,fb_Bps,slow10,over3"]
for fam, wls in FAM:
    for units in (89, 723):
        out.append(f"\n#### {fam}, {SIZE[units]}\n")
        hdr = "| candidate | in +-30% | over 1.3x | under 0.7x | q>100ms | p50 x | p95 x | p99 x | undeliv+late | retx | recov up / down s | startup s | slow<10% | over>3x | fb B/s |"
        out.append(hdr)
        out.append("|" + "---|" * 15)
        for c, f in cands:
            rows = []
            for wl in wls:
                rl = runs.get((wl, units, c, f))
                orc = runs.get((wl, units, "oracle", "a"))
                if not rl or not orc:
                    continue
                p50, p95, p99 = lat_stats(rl)
                o50, o95, o99 = lat_stats(orc)
                started = sum(r["started"] for r in rl.values())
                bad = sum(r["undeliv"] + r["late"] for r in rl.values())
                ups = [r["up"] for r in rl.values() if r["up"] >= 0]
                downs = [r["down"] for r in rl.values() if r["down"] >= 0]
                rows.append({
                    "in30": mean([r["in30"] for r in rl.values()]), "over": mean([r["over13"] for r in rl.values()]),
                    "under": mean([r["under07"] for r in rl.values()]), "q": mean([r["q100"] for r in rl.values()]),
                    "r50": p50 / o50 if o50 > 0 else None, "r95": p95 / o95 if o95 > 0 else None, "r99": p99 / o99 if o99 > 0 else None,
                    "bad": 100.0 * bad / started if started else 0.0, "retx": mean([r["retx"] for r in rl.values()]),
                    "up": statistics.median(ups) if ups else None, "down": statistics.median(downs) if downs else None,
                    "startup": statistics.median([r["startup"] for r in rl.values()]), "slow": mean([r["slow10"] for r in rl.values()]),
                    "over3": mean([r["over3"] for r in rl.values()]), "fb": mean([r["fb"] for r in rl.values()]),
                })
                csv.append(",".join(str(x) for x in [fam.split(" (")[0], SIZE[units], wl, c, f, p50, p95, p99,
                    rows[-1]["in30"], rows[-1]["over"], rows[-1]["under"], rows[-1]["q"], rows[-1]["retx"], rows[-1]["bad"],
                    rows[-1]["up"], rows[-1]["down"], rows[-1]["startup"], mean([r["gp"] for r in rl.values()]), rows[-1]["fb"], rows[-1]["slow"], rows[-1]["over3"]]))
            if not rows:
                continue
            def col(k, fmt="{:.0f}"):
                v = mean([r[k] for r in rows if r[k] is not None])
                return fmt.format(v) if v == v else "-"
            def gm(k):
                v = geomean([r[k] for r in rows])
                return f"{v:.2f}" if v == v else "-"
            upv = [r["up"] for r in rows if r["up"] is not None]
            dnv = [r["down"] for r in rows if r["down"] is not None]
            rec = (f"{statistics.median(upv):.1f}" if upv else "-") + " / " + (f"{statistics.median(dnv):.1f}" if dnv else "-")
            stv = [r["startup"] for r in rows]
            out.append(f"| {label(c, f)} | {col('in30')}% | {col('over')}% | {col('under')}% | {col('q', '{:.1f}')}% | {gm('r50')} | {gm('r95')} | {gm('r99')} | {col('bad', '{:.1f}')}% | {col('retx', '{:.1f}')}% | {rec} | {statistics.median(stv):.1f} | {col('slow')}% | {col('over3', '{:.1f}')}% | {col('fb')} |")

# verdicts against the current ADAPTIVE on the current feedback (adapt-a) and against the oracle, all 34 cells
cells = sorted({(wl, u) for (wl, u, _, _) in runs})
out.append(f"\n#### Verdicts per candidate over all {len(cells)} (workload, size) cells (per-seed p50 latency, W/L need 5 of 6 seeds better by 5 %)\n")
out.append("| candidate | vs adapt-a W/T/L | vs fixed 95% of mean W/T/L | vs oracle W/T/L |")
out.append("|---|---|---|---|")
for c, f in cands:
    if c in ("oracle", "fixed"):
        continue
    t = {"adapt": collections.Counter(), "fixed": collections.Counter(), "oracle": collections.Counter()}
    for wl, u in cells:
        a = runs.get((wl, u, c, f))
        if not a:
            continue
        sp = seed_p50(a)
        for name, key in (("adapt", ("adapt", "a")), ("fixed", ("fixed", "a")), ("oracle", ("oracle", "a"))):
            ref = runs.get((wl, u) + key)
            if ref:
                t[name][verdict(sp, seed_p50(ref))] += 1
    fmt = lambda k: f"{t[k]['W']}/{t[k]['T']}/{t[k]['L']}"
    out.append(f"| {label(c, f)} | {fmt('adapt')} | {fmt('fixed')} | {fmt('oracle')} |")

text = "\n".join(out)
print(text)
if md_out:
    open(md_out, "w").write(text + "\n")
if csv_out:
    open(csv_out, "w").write("\n".join(csv) + "\n")
