#!/usr/bin/env python3
"""Stress table of DESIGN section 27: candidates across link slotting (the link serves its queue only at slot boundaries) and
receiver time stamp jitter. usage: stress_table.py <res dir (slot 0)> <res_slot1000> <res_slot2000> <res_slot4000>"""
import glob, math, os, re, sys, collections
INF = float("inf")
dirs = sys.argv[1:]
names = ["clean", "slot 1 ms, jitter 0.2 ms", "slot 2 ms, jitter 0.5 ms", "slot 4 ms, jitter 1 ms"]
WLS = {"P7", "N1", "N2", "N3", "N4", "M1", "S1", "RS", "RSm", "RSd", "H100"}
def load(d):
    runs = collections.defaultdict(dict)
    for p in glob.glob(f"{d}/*.txt"):
        m = re.match(r"(.+?)_(\d+)_(.+)_([ab])_(\d+)\.txt$", os.path.basename(p))
        if not m or m.group(1) not in WLS or os.path.getsize(p) == 0:
            continue
        wl, units, cand, fb, seed = m.groups()
        line = open(p).read().strip()
        head, _, lat = line.partition(" lat=")
        r = {k: float(v) for k, v in (kv.split("=") for kv in head.split())}
        r["lat"] = [float(x) for x in lat.split(",") if x]
        runs[(wl, int(units), cand, fb)][int(seed)] = r
    return runs
def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q / 100.0))] if v else INF
def lat(rl, q):
    allv = []
    for r in rl.values():
        allv += r["lat"] + [INF] * int(r["undeliv"])
    return pct(allv, q)
data = [load(d) for d in dirs]
cands = [("oracle", "a"), ("fixed", "a"), ("adapt", "a"), ("adapt_hi", "a"), ("bbr3", "b"), ("nada", "b"), ("gcc", "b"), ("train", "b"), ("train1", "b"), ("train1t", "b"), ("traint", "b")]
def gm(xs):
    xs = [x for x in xs if x not in (INF,) and x > 0 and x == x]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")
for units, label in ((89, "128 KiB"), (723, "1 MiB")):
    print(f"\n#### Stress, {label} (workloads P7 N1 N2 N3 N4 M1 S1 RS RSm RSd H100; 6 seeds): in +-30% of the capacity / p50 x oracle / p99 x oracle / over 3x\n")
    print("| candidate | " + " | ".join(names) + " |")
    print("|---|" + "---|" * len(names))
    for c, f in cands:
        cells = []
        for runs in data:
            in30, r50, r99, o3 = [], [], [], []
            for wl in WLS:
                rl = runs.get((wl, units, c, f)); orc = runs.get((wl, units, "oracle", "a"))
                if not rl or not orc:
                    continue
                in30.append(sum(r["in30"] for r in rl.values()) / len(rl))
                o3.append(sum(r["over3"] for r in rl.values()) / len(rl))
                a50, a99, b50, b99 = lat(rl, 50), lat(rl, 99), lat(orc, 50), lat(orc, 99)
                r50.append(a50 / b50 if b50 > 0 else float("nan")); r99.append(a99 / b99 if b99 > 0 else float("nan"))
            if not in30:
                cells.append("-")
            else:
                cells.append(f"{sum(in30)/len(in30):.0f}% / {gm(r50):.2f} / {gm(r99):.2f} / {sum(o3)/len(o3):.1f}%")
        print(f"| {c if c in ('oracle','fixed') else c + '-' + f} | " + " | ".join(cells) + " |")
