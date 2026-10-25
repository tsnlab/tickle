"""k5_floor_read.py - k5_floor_ab.sh's pre-registered reading (the rules are in that script's header; this applies them).

Usage: k5_floor_read.py OUT TAG REPS1 REPS2 REPS3
"""
import math
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
TREES = ("M", "A", "K", "KF")


def kv(line):
    return dict(KV.findall(line))


def lines(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def reps(out, tag, tree):
    """{(arm, cell): [rep dict]} with resends, timer ACKNACKs, sub p50."""
    res = {}
    base = os.path.join(out, f"{tag}_{tree}", "runs")
    if not os.path.isdir(base):
        return res
    for name in sorted(os.listdir(base)):
        arm, cell, rep = name.rsplit("-", 2)
        d = os.path.join(base, name)
        r = {"rep": int(rep), "retx": None, "timer": None, "p50": None, "p99": None}
        for ln in lines(os.path.join(d, "ping.log")):
            if ln.startswith("RESULT") and r["retx"] is None:
                r["retx"] = int(kv(ln).get("retransmitted", -1))
        for ln in lines(os.path.join(d, "echo.log")):
            if ln.startswith("RSTATS: role=sub acknack_tx"):
                r["timer"] = int(kv(ln).get("timer", -1))
            if ln.startswith("RESULT: role=sub"):
                v = kv(ln).get("latency_median_us")
                r["p50"] = float(v) / 1000 if v not in (None, "nan") else None
                v = kv(ln).get("latency_p99_us")
                r["p99"] = float(v) / 1000 if v not in (None, "nan") else None
        if r["retx"] is not None:
            res.setdefault((arm, cell), []).append(r)
    return res


def fisher(a, n1, b, n2, one_sided_less=True):
    """P-value for a/n1 vs b/n2. one-sided: P(X <= a) for the first group's count under the null; else two-sided."""
    k = a + b
    n = n1 + n2

    def pmf(x):
        if x < max(0, k - n2) or x > min(k, n1):
            return 0.0
        return math.comb(n1, x) * math.comb(n2, k - x) / math.comb(n, k)

    if one_sided_less:
        return sum(pmf(x) for x in range(0, a + 1))
    p0 = pmf(a)
    return min(1.0, sum(pmf(x) for x in range(0, min(k, n1) + 1) if pmf(x) <= p0 * (1 + 1e-9)))


def main():
    out, tag = sys.argv[1], sys.argv[2]
    planned = {"p1": int(sys.argv[3]), "p2": int(sys.argv[4]), "p3": int(sys.argv[5])}
    verdicts = {}
    void = []
    # ---- P1: HIGH / COLLAPSE / STORM, pooled over ge5b2 and ge5b4
    p1 = {t: reps(out, f"{tag}_p1", t) for t in TREES}
    extra = os.environ.get("K5_P1_EXTRA", "").split()  # the P1 extension (k5_floor_ab.sh header), pooled
    for x in extra:
        for t in TREES:
            for key, rs in reps(out, f"{x}_p1", t).items():
                p1[t].setdefault(key, []).extend(rs)
        planned["p1"] += int(sys.argv[3])
    if extra:
        print(f"P1 POOLED with {extra} (the extension's second look; the 40-rep reading is the registered one)")
    counts = {t: {"HIGH": 0, "COLLAPSE": 0, "STORM": 0, "n": 0} for t in TREES}
    print("P1 per arm (resends median; HIGH > 1.5 x M median, COLLAPSE timer > 5 x M median, STORM > 10 x):")
    for arm in ("ge5b2", "ge5b4"):
        mr = [r for r in p1["M"].get((arm, "rel_large"), [])]
        if not mr:
            void.append(f"P1 {arm}: no M reps")
            continue
        ref_retx = statistics.median(r["retx"] for r in mr)
        ref_timer = statistics.median(r["timer"] for r in mr if r["timer"] is not None)
        for t in TREES:
            rs = p1[t].get((arm, "rel_large"), [])
            if len(rs) < 0.8 * planned["p1"]:
                void.append(f"P1 {arm} {t}: {len(rs)} reps of {planned['p1']}")
            high = [r for r in rs if r["retx"] > 1.5 * ref_retx]
            coll = [r for r in rs if r["timer"] is not None and r["timer"] > 5 * ref_timer]
            storm = [r for r in rs if r["retx"] > 10 * ref_retx]
            counts[t]["HIGH"] += len(high)
            counts[t]["COLLAPSE"] += len(coll)
            counts[t]["STORM"] += len(storm)
            counts[t]["n"] += len(rs)
            med = statistics.median(r["retx"] for r in rs) if rs else float("nan")
            print(f"  {arm} {t:2} reps {len(rs)} resends median {med:.0f} (M ref {ref_retx:.0f}, timer ref "
                  f"{ref_timer:.0f}) HIGH {len(high)} {[(r['rep'], r['retx']) for r in high]} COLLAPSE {len(coll)} "
                  f"{[(r['rep'], r['timer']) for r in coll]} STORM {len(storm)}")
    for t in TREES:
        c = counts[t]
        print(f"P1 pooled {t:2}: n {c['n']} HIGH {c['HIGH']} COLLAPSE {c['COLLAPSE']} STORM {c['STORM']}")
    cm, ca, ck, cf = counts["M"], counts["A"], counts["K"], counts["KF"]
    p_ctl_h = fisher(ca["HIGH"], ca["n"], cm["HIGH"], cm["n"], one_sided_less=False)
    p_ctl_c = fisher(ca["COLLAPSE"], ca["n"], cm["COLLAPSE"], cm["n"], one_sided_less=False)
    verdicts["CONTROL"] = "PASS" if p_ctl_h >= 0.05 and p_ctl_c >= 0.05 else "VOID"
    print(f"CONTROL A vs M: HIGH p {p_ctl_h:.3f}, COLLAPSE p {p_ctl_c:.3f} -> {verdicts['CONTROL']}")
    p_mech = fisher(cf["COLLAPSE"], cf["n"], ck["COLLAPSE"], ck["n"])
    if cf["COLLAPSE"] < ck["COLLAPSE"] and p_mech < 0.05:
        verdicts["MECHANISM"] = "SHOWN"
    elif cf["COLLAPSE"] >= ck["COLLAPSE"] and ck["COLLAPSE"] >= 3:
        verdicts["MECHANISM"] = "FALSIFIED"
    else:
        verdicts["MECHANISM"] = "UNDECIDED"
    print(f"MECHANISM KF {cf['COLLAPSE']}/{cf['n']} vs K {ck['COLLAPSE']}/{ck['n']} COLLAPSE, one-sided p "
          f"{p_mech:.4f} -> {verdicts['MECHANISM']}")
    p_out = fisher(cf["HIGH"], cf["n"], ck["HIGH"], ck["n"])
    main_high = max(cm["HIGH"], ca["HIGH"])
    if cf["HIGH"] < ck["HIGH"] and p_out < 0.05 and cf["HIGH"] <= main_high + 1:
        verdicts["OUTCOME"] = "FIXED"
    elif cf["HIGH"] > main_high + 1:
        verdicts["OUTCOME"] = "NOT FIXED"
    else:
        verdicts["OUTCOME"] = "UNDECIDED"
    print(f"OUTCOME KF {cf['HIGH']}/{cf['n']} vs K {ck['HIGH']}/{ck['n']} HIGH (M {cm['HIGH']}, A {ca['HIGH']}), "
          f"one-sided p {p_out:.4f} -> {verdicts['OUTCOME']}")
    print("  history (not pooled): K ge5b2 2x reps 4/23, M+A 0/46 (R4K)")
    if any(counts[t]["STORM"] for t in ("KF",)):
        verdicts["NO WORSE"] = "WORSE (KF STORM)"
    # ---- P2: R4
    summ = lines(os.path.join(out, f"{tag}_p2_KF", "summary.txt"))
    r4 = [ln for ln in summ if re.match(r"^(PASS|FAIL|VOID) +R4: ge10b4", ln)]
    base = [ln for ln in summ if "BASE: base_rtt" in ln]
    print(f"R4 (KF, the harness's line): {r4[0] if r4 else 'none'}; {base[0].strip() if base else 'no base_rtt'}")
    verdicts["R4"] = r4[0].split()[0] if r4 else "VOID"
    p2 = {t: reps(out, f"{tag}_p2", t) for t in TREES}
    med = {}
    for t in TREES:
        rs = p2[t].get(("ge10b4", "rel_large"), [])
        if len(rs) < 0.8 * planned["p2"]:
            void.append(f"P2 ge10b4 {t}: {len(rs)} reps of {planned['p2']}")
        v = [r["p50"] for r in rs if r["p50"] is not None]
        med[t] = statistics.median(v) if v else float("nan")
        tl = [ln for ln in lines(os.path.join(out, f"{tag}_p2_{t}", "summary.txt"))
              if re.match(r"^(PASS|FAIL|VOID) +R4: ge10b4", ln)]
        print(f"  ge10b4 rel_large {t:2}: per-rep p50 {[round(x, 1) for x in v]} median {med[t]:.1f}; "
              f"{tl[0] if tl else 'no R4 line'}")
    band = max(2.0, abs(med["A"] - med["M"]))
    keeps = med["KF"] - med["K"] <= band
    verdicts["R4 KEEPS"] = "KEEPS" if keeps else "LOSES (FALSIFIED)"
    print(f"R4 KF - K = {med['KF'] - med['K']:+.1f} ms against band {band:.1f} -> {verdicts['R4 KEEPS']}")
    # ---- NO WORSE: wifi_repair_ab VERDICT lines and resends
    worse = []
    for ph in ("p1", "p2", "p3"):
        ab = lines(os.path.join(out, f"{tag}_{ph}.ab.txt"))
        aw = {ln.split(":")[0].split()[-1] for ln in ab if ln.startswith("VERDICT A vs M") and "WORSE (" in ln}
        for ln in ab:
            if ln.startswith("VERDICT KF vs M") and "WORSE (" in ln:
                cellarm = ln.split(":")[0].split()[-1]
                (print(f"  noise ({ph}, A shows it too): {ln}") if cellarm in aw else worse.append(f"{ph}: {ln}"))
        pr = {t: reps(out, f"{tag}_{ph}", t) for t in TREES}
        for key in sorted(pr["KF"]):
            if key[1] != "rel_large":
                continue
            mm = [statistics.median(r["retx"] for r in pr[t].get(key, [])) for t in ("M", "A") if pr[t].get(key)]
            kf = statistics.median(r["retx"] for r in pr["KF"][key])
            if mm and kf > 1.10 * max(mm):
                worse.append(f"{ph} {key}: KF resends median {kf:.0f} > 1.10 x {max(mm):.0f}")
            print(f"  resends {ph} {key[0]:6} " + " ".join(
                f"{t} {statistics.median(r['retx'] for r in pr[t][key]):.0f}" for t in TREES if pr[t].get(key)))
        if ph == "p3":
            for key in pr["KF"]:
                rs = pr["M"].get(key, [])
                if rs:
                    ref = statistics.median(r["retx"] for r in rs)
                    st = [r["retx"] for r in pr["KF"][key] if r["retx"] > 10 * ref]
                    if st:
                        worse.append(f"p3 {key}: KF STORM {st}")
    verdicts.setdefault("NO WORSE", "WORSE: " + "; ".join(worse) if worse else "NOT WORSE")
    # ---- TAIL (registered 2026-10-26 04:53 for the discard run k5d, before it; post hoc for k5ab's clamp)
    tail_loses = []
    for ph, arms in (("p1", ("ge5b2", "ge5b4")), ("p3", ("bc", "tv"))):
        pr = {t: reps(out, f"{tag}_{ph}", t) for t in TREES}
        if ph == "p1":
            for x in extra:
                for t in TREES:
                    for key, rs in reps(out, f"{x}_p1", t).items():
                        pr[t].setdefault(key, []).extend(rs)
        for arm in arms:
            key = (arm, "rel_large")
            m99 = {}
            for t in TREES:
                v = [r["p99"] for r in pr[t].get(key, []) if r["p99"] is not None]
                m99[t] = statistics.median(v) if v else float("nan")
            band = 0.10 * m99["K"] + abs(m99["A"] - m99["M"])
            ok = m99["KF"] <= m99["K"] + band
            print(f"TAIL {arm}: median per-rep p99 M {m99['M']:.1f} A {m99['A']:.1f} K {m99['K']:.1f} KF {m99['KF']:.1f}"
                  f" ms; KF <= K + {band:.1f} -> {'KEEPS' if ok else 'LOSES'}")
            if not ok:
                tail_loses.append(arm)
    verdicts["TAIL"] = "KEEPS K's" if not tail_loses else "LOSES K's at " + " ".join(tail_loses)
    for v in void:
        print("VOID: " + v)
    print("VERDICTS: " + "; ".join(f"{k} {v}" for k, v in verdicts.items()))
    land = (not void and verdicts["CONTROL"] == "PASS" and verdicts["MECHANISM"] == "SHOWN" and
            verdicts["OUTCOME"] == "FIXED" and verdicts["R4"] == "PASS" and verdicts["R4 KEEPS"] == "KEEPS" and
            verdicts["NO WORSE"] == "NOT WORSE" and verdicts["TAIL"] == "KEEPS K's")
    print("READING: " + ("LAND the floor with K (after a rig core gate)" if land else "do not land as registered"))


main()
