#!/usr/bin/env python3
"""r4_short_read.py - the pre-registered reading of r4_short_ab.sh (RESULTS `EWB`, 2026-10-27).

Written and committed before the run; this code, not this text, decides. Trees M (G), A (G again), S (the stall
back-off); arms ctl (control) and ge10b4, cell rel_large (Wi-Fi profile, 150 samples a rep).

Per rep: delivered and published (sub / pub RESULT), "gap too large to track" lines (echo.log), the writer's timeout
releases with a wait >= 65 ms (ping.log hist=lro_timeout_wait ge65536us), the reader's latency max (latency_max_us),
start load (meta.txt). A rep missing a RESULT, or started at load > 8, is VOID: listed, not counted.
SHORT: a rep delivering fewer than 150 samples, or with any "gap too large to track" line.
CONTROL: ctl has no SHORT rep in any tree -> PASS; else FAIL (the run cannot tell the change from the bench; the verdict
  is VOID).
SHORT VERDICT (ge10b4), k_t SHORT reps of n_t counted:
  S WORSE when k_S / n_S is above (k_M + k_A) / (n_M + n_A) with one-sided Fisher p < 0.05;
  NOT WORSE when k_S = 0, or k_S <= max(k_M, k_A) (not above the A/A's own count);
  otherwise UNDECIDED.
VOID (the run): a tree with fewer than 0.9 x the counted ge10b4 reps of the one with the most, or fewer than 20.
Printed beside it, not deciding: per tree the median and maximum of the >= 65 ms timeout releases and of latency max.
Usage: r4_short_read.py OUT TAG
"""
import glob
import math
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
TREES = ("M", "A", "S")


def kv(line):
    return dict(KV.findall(line))


def lines(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def fisher_more(a, n1, b, n2):
    """One-sided P(X >= a) for group 1's count, hypergeometric."""
    k, n = a + b, n1 + n2
    if n1 == 0 or n2 == 0:
        return float("nan")
    tot = math.comb(n, k)
    return sum(math.comb(n1, x) * math.comb(n2, k - x) / tot for x in range(a, min(k, n1) + 1))


def collect(out, tag):
    reps = {(t, a): [] for t in TREES for a in ("ctl", "ge10b4")}
    for t in TREES:
        for d in sorted(glob.glob(os.path.join(out, f"{tag}_{t}", "runs", "*-rel_large-*"))):
            arm = os.path.basename(d).split("-")[0]
            if arm not in ("ctl", "ge10b4"):
                continue
            meta = kv(" ".join(lines(os.path.join(d, "meta.txt"))))
            load = float(meta.get("load", 0) or 0)
            ping, echo = lines(os.path.join(d, "ping.log")), lines(os.path.join(d, "echo.log"))
            pub = [kv(x) for x in ping if x.startswith("RESULT: role=pub")]
            sub = [kv(x) for x in echo if x.startswith("RESULT: role=sub")]
            if not pub or not sub or load > 8:
                print(f"VOID rep {t} {os.path.basename(d)}: {'load %.1f' % load if load > 8 else 'missing RESULT'}")
                continue
            hist = [kv(x) for x in ping if "hist=lro_timeout_wait" in x]
            r = {"rep": os.path.basename(d), "load": load, "delivered": int(sub[0].get("delivered", 0)),
                 "published": int(pub[0].get("published", 0)),
                 "jumps": sum(1 for x in echo if "gap too large to track" in x),
                 "late": int(hist[0].get("ge65536us", 0)) if hist else 0,
                 "lat_max": float(sub[0].get("latency_max_us", "nan")) / 1000}
            r["short"] = r["delivered"] < 150 or r["jumps"] > 0
            reps[(t, arm)].append(r)
    return reps


def main():
    out, tag = sys.argv[1], sys.argv[2]
    reps = collect(out, tag)
    for arm in ("ctl", "ge10b4"):
        for t in TREES:
            rs = reps[(t, arm)]
            k = sum(r["short"] for r in rs)
            late = [r["late"] for r in rs]
            lm = [r["lat_max"] for r in rs if r["lat_max"] == r["lat_max"]]
            print(f"TREE {t} {arm}: counted {len(rs)}, SHORT {k}, timeout releases >= 65 ms median "
                  f"{statistics.median(late) if late else float('nan')} max {max(late) if late else 'nan'}, latency max "
                  f"median {statistics.median(lm) if lm else float('nan'):.1f} ms, worst {max(lm) if lm else float('nan'):.1f} ms")
            for r in rs:
                if r["short"]:
                    print(f"   SHORT {t} {r['rep']}: delivered {r['delivered']}/{r['published']} jumps {r['jumps']} "
                          f"late {r['late']} latency max {r['lat_max']:.1f} ms load {r['load']:.1f}")
    ctl_short = sum(r["short"] for t in TREES for r in reps[(t, "ctl")])
    control = "PASS" if ctl_short == 0 and all(reps[(t, "ctl")] for t in TREES) else "FAIL"
    print(f"CONTROL ctl: SHORT reps {ctl_short} -> {control}")
    n = {t: len(reps[(t, "ge10b4")]) for t in TREES}
    k = {t: sum(r["short"] for r in reps[(t, "ge10b4")]) for t in TREES}
    most = max(n.values())
    if any(n[t] < 0.9 * most or n[t] < 20 for t in TREES):
        print(f"VOID run: counted ge10b4 reps {n}")
        print("SHORT VERDICT: VOID")
        return 0
    p = fisher_more(k["S"], n["S"], k["M"] + k["A"], n["M"] + n["A"])
    if control != "PASS":
        v = "VOID (control)"
    elif k["S"] / n["S"] > (k["M"] + k["A"]) / (n["M"] + n["A"]) and p < 0.05:
        v = "S WORSE"
    elif k["S"] == 0 or k["S"] <= max(k["M"], k["A"]):
        v = "NOT WORSE"
    else:
        v = "UNDECIDED"
    print(f"SHORT ge10b4: S {k['S']}/{n['S']} against M + A {k['M'] + k['A']}/{n['M'] + n['A']} (M {k['M']}, "
          f"A {k['A']}), one-sided p {p:.4f}")
    print(f"SHORT VERDICT: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
