#!/usr/bin/env python3
"""Reads the T1-Pi pacing sweep (t1pi_fw.sh with TickLE variants, PACE_PLAN.md sections 2-3) and prints the per-cell table, the best of each
(size, arm), the in-band test, the recommended rate band, the score and the stage-2 choice. Rules are the registered ones; nothing here is tuned
after seeing data. Usage: t1pi_pacread.py [--stage2] LABEL=DIR [LABEL=DIR ...]   (DIR holds rel_large/<arm>/r<n>_<cfg>_<A|A2>.done)
Simulation only; relative figures inside the hwsim model."""
import os, re, sys, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from t1pi_fwread import run_metrics, med, INF  # noqa: E402

ARMS = ["ctl", "o60", "o200"]


def cfg_rate(cfg):
    m = re.match(r"tickle(?:-r(\d+|off))?(?:-b\d+)?(?:-f\w+)?$", cfg)
    if not m:
        return None
    return m.group(1)


def load(dirs):
    data = {}  # (size, arm, cfg) -> {"A": [m], "A2": [m]}
    for label, d in dirs:
        for arm in ARMS:
            ad = os.path.join(d, "rel_large", arm)
            if not os.path.isdir(ad):
                continue
            for f in sorted(os.listdir(ad)):
                mt = re.match(r"r(\d+)_([\w-]+?)_(A2?)\.done$", f)
                if not mt:
                    continue
                base = os.path.join(ad, f[:-5])
                m = run_metrics(base, "rel_large")
                if m is None:
                    continue
                near = {}
                for line in open(base + ".near", errors="replace"):
                    if line.startswith("RESULT"):
                        for tok in line.split():
                            if "=" in tok:
                                k, v = tok.split("=", 1)
                                near[k] = v
                m["deliv"] = int(m["n"].split("/")[0])
                m["retx"] = float(near.get("retransmitted", 0))
                m["waits"] = float(near.get("large_send_waits", 0))
                m["pwaits"] = float(near.get("pacing_waits", 0))
                m["pdef"] = float(near.get("pacing_repairs_deferred", 0))
                data.setdefault((label, arm, mt.group(2)), {}).setdefault(mt.group(3), []).append(m)
    return data


def stats(runs):
    allr = runs.get("A", []) + runs.get("A2", [])
    d = [r["deliv"] for r in allr]
    p50 = [r["p50"] for r in allr]
    p99 = [r["p99"] for r in allr]
    def void(key):
        a = [r[key] for r in runs.get("A", [])]
        b = [r[key] for r in runs.get("A2", [])]
        if not a or not b:
            return True
        ma, mb = med(a), med(b)
        if ma == INF and mb == INF:
            return False
        if ma == INF or mb == INF:
            return True
        return abs(mb - ma) > 0.10 * abs(ma) if ma else mb != 0
    return {"n": len(allr), "d": d, "med_d": med(d), "min_d": min(d) if d else 0, "p50": p50, "med_p50": med(p50),
            "min_p50": min(p50) if p50 else INF, "max_p50": max(p50) if p50 else INF, "med_p99": med(p99),
            "void_p50": void("p50"), "void_d": void("deliv"), "retx": med([r["retx"] for r in allr]),
            "pw": med([r["pwaits"] for r in allr]), "pd": med([r["pdef"] for r in allr])}


def ft(v):
    return "none" if v == INF else (f"{v:.0f}" if v >= 100 else f"{v:.2f}")


def main():
    args = sys.argv[1:]
    dirs = [tuple(a.split("=", 1)) for a in args if "=" in a]
    data = load(dirs)
    sizes = sorted({k[0] for k in data})
    cfgs = sorted({k[2] for k in data}, key=lambda c: (c != "tickle", c))
    print("# T1-Pi pacing sweep, read by t1pi_pacread.py (simulation only; relative, not Wi-Fi performance)\n")
    st = {k: stats(v) for k, v in data.items()}
    for size in sizes:
        for arm in ARMS:
            print(f"\n## {size} / {arm}\n")
            print("| config | n | delivered median [min] | p50 ms median [min..max] | p99 ms median | retx median | pacing_waits | deferred | void p50/deliv |")
            print("|---|---:|---|---|---:|---:|---:|---:|---|")
            for c in cfgs:
                s = st.get((size, arm, c))
                if not s:
                    continue
                print(f"| {c} | {s['n']} | {s['med_d']:.0f} [{s['min_d']}] | {ft(s['med_p50'])} [{ft(s['min_p50'])}..{ft(s['max_p50'])}] | {ft(s['med_p99'])} | "
                      f"{s['retx']:.0f} | {s['pw']:.0f} | {s['pd']:.0f} | {'V' if s['void_p50'] else '-'}/{'V' if s['void_d'] else '-'} |")
    # best per (size, arm) and band over the stage-1 rate configs only (no -b, -f variants)
    rate_cfgs = [c for c in cfgs if re.match(r"tickle(-roff)?$|tickle-r\d+$", c)]
    best = {}
    for size in sizes:
        for arm in ARMS:
            cand = [(st[(size, arm, c)]["med_p50"], c) for c in rate_cfgs if (size, arm, c) in st
                    and not st[(size, arm, c)]["void_p50"] and st[(size, arm, c)]["med_d"] >= 14 and st[(size, arm, c)]["min_d"] >= 12]
            best[(size, arm)] = min(cand) if cand else None
    print("\n## Best of each (size, arm): non-VOID p50, median delivered >= 14, worst run >= 12\n")
    for k, v in best.items():
        print(f"- {k[0]}/{k[1]}: " + (f"{v[1]} at p50 {ft(v[0])} ms" if v else "NONE qualifies"))
    inband = {}
    print("\n## In-band test per rate (p50 <= best + max(10% of best, half the best cell's range); delivered median >= best median - 1; worst run >= 12)\n")
    print("| rate | " + " | ".join(f"{s}/{a}" for s in sizes for a in ARMS) + " | in band (all) | score |")
    print("|---|" + "---|" * (len(sizes) * len(ARMS) + 2))
    scores = {}
    for c in rate_cfgs:
        row, ok_all, sc, nsc = [], True, 0.0, 0
        for size in sizes:
            for arm in ARMS:
                s = st.get((size, arm, c))
                b = best[(size, arm)]
                if not s or not b:
                    row.append("n/a"); ok_all = False; continue
                bs = st[(size, arm, b[1])]
                limit = bs["med_p50"] + max(0.10 * bs["med_p50"], 0.5 * (bs["max_p50"] - bs["min_p50"]))
                ok = s["med_p50"] <= limit and s["med_d"] >= bs["med_d"] - 1 and s["min_d"] >= 12
                row.append(("ok" if ok else "NO") + f" ({ft(s['med_p50'])})")
                ok_all = ok_all and ok
                ratio = s["med_p50"] / bs["med_p50"] if s["med_p50"] != INF else 1e9
                if s["med_d"] < 14:
                    ratio *= 10
                sc += ratio; nsc += 1
        inband[c] = ok_all
        scores[c] = sc / nsc if nsc else INF
        print(f"| {c} | " + " | ".join(row) + f" | {'YES' if ok_all else 'no'} | {scores[c]:.2f} |")
    rates = [c for c in rate_cfgs if re.match(r"tickle-r\d+$", c)]
    rates.sort(key=lambda c: int(c.split("-r")[1]))
    runs_, cur = [], []
    for c in rates:
        if inband.get(c):
            cur.append(c)
        else:
            if cur:
                runs_.append(cur); cur = []
    if cur:
        runs_.append(cur)
    print("\n## Recommended band (largest contiguous run of in-band rates)\n")
    if runs_:
        longest = max(runs_, key=len)
        print("- in-band groups: " + "; ".join(",".join(x.replace("tickle-r", "") for x in g) + " Mbit/s" for g in runs_))
        print("- recommended: " + longest[0].replace("tickle-r", "") + " to " + longest[-1].replace("tickle-r", "") + " Mbit/s" + (" (in-band set has gaps)" if len(runs_) > 1 else ""))
    else:
        print("- NO rate is in band for all six (size, arm) pairs")
    ranked = sorted(rates, key=lambda c: (scores[c], int(c.split("-r")[1])))
    print("\n## Stage-2 choice (two lowest scores, ties to the lower rate): " + ", ".join(f"{c} ({scores[c]:.2f})" for c in ranked[:2]))


if __name__ == "__main__":
    main()
