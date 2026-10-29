#!/usr/bin/env python3
"""c11_reveal_read.py - the reading c11_reveal_ab.sh pre-registers (its header holds the rules; this applies them).

Usage: c11_reveal_read.py OUTB TAG  - reads OUTB/TAG_{M,A,O}/ (wifi_burst_loss_netns.sh outputs) and OUTB/TAG.trace.txt.
"""
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
NPROC = os.cpu_count() or 1
LARGE_ARMS = ["ctl", "ge5b2", "ge5b4", "ge10b4", "tv"]
SMALL_ARMS = ["ctl", "ge5b4", "tv"]
MIN_UNLOADED = 4
FLOORS = {"delivered": ("abs", 0.5), "p50": ("rel", 0.05), "p99": ("rel", 0.10), "resends": ("rel", 0.10),
          "duplicates": ("rel", 0.25), "cpu": ("rel", 0.05)}


def read(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def kv(line):
    return dict(KV.findall(line))


def med(v):
    return statistics.median(v) if v else float("nan")


def large_cell(base, tag, lab, arm):
    """Per-rep figures of one rel_large cell of one tree."""
    reps = []
    runs = os.path.join(base, f"{tag}_{lab}", "runs")
    for name in sorted(os.listdir(runs)) if os.path.isdir(runs) else []:
        a, cell, rep = name.rsplit("-", 2)
        if cell != "rel_large" or a != arm:
            continue
        d = os.path.join(runs, name)
        load = float(kv(" ".join(read(os.path.join(d, "meta.txt")))).get("load", 0))
        echo = read(os.path.join(d, "echo.log"))
        ping = read(os.path.join(d, "ping.log"))
        sub = [kv(x) for x in echo if x.startswith("RESULT: role=sub")]
        pub = [kv(x) for x in ping if x.startswith("RESULT: role=pub")]
        tx = [kv(x) for x in echo if x.startswith("RSTATS: role=sub acknack_tx")]
        r = {"rep": rep, "load": load, "ok": bool(sub and pub)}
        if r["ok"]:
            s, p = sub[0], pub[0]
            r.update(delivered=int(s.get("delivered", 0)), published=int(p.get("published", 0)),
                     p50=float(s.get("latency_median_us", "nan")) / 1000,
                     p99=float(s.get("latency_p99_us", "nan")) / 1000,
                     resends=int(p.get("retransmitted", 0)), duplicates=int(s.get("large_duplicate", 0)),
                     cpu=float(s.get("cpu_ms", 0)) + float(p.get("cpu_ms", 0)),
                     reveal=int(tx[0]["reveal"]) if tx and "reveal" in tx[0] else None)
        reps.append(r)
    return reps


def summarize(reps):
    good = [r for r in reps if r["ok"] and r["delivered"] >= 100]
    unloaded = [r for r in good if r["load"] <= NPROC / 2]
    pub = sum(r["published"] for r in good)
    return {"n": len(reps), "good": len(good), "unloaded": len(unloaded),
            "delivered": 100.0 * sum(r["delivered"] for r in good) / pub if pub else float("nan"),
            "p50": med([r["p50"] for r in unloaded]), "p99": med([r["p99"] for r in unloaded]),
            "resends": med([r["resends"] for r in good]), "duplicates": med([r["duplicates"] for r in good]),
            "cpu": med([r["cpu"] for r in unloaded]),
            "reveal": [r["reveal"] for r in good], "resend_list": [r["resends"] for r in good]}


def small_cell(base, tag, lab, arm):
    p99s, deliv, off = [], 0, 0
    runs = os.path.join(base, f"{tag}_{lab}", "runs")
    for name in sorted(os.listdir(runs)) if os.path.isdir(runs) else []:
        a, cell, rep = name.rsplit("-", 2)
        if cell != "rel_small" or a != arm:
            continue
        d = os.path.join(runs, name)
        load = float(kv(" ".join(read(os.path.join(d, "meta.txt")))).get("load", 0))
        e = [kv(x) for x in read(os.path.join(d, "echo.log")) if x.startswith("RESULT")]
        if e:
            deliv += int(e[0].get("scored", 0))
            off += int(e[0].get("max_seq", 0)) + 1 - int(e[0].get("warmup", 0))
        owd = sorted(int(x) for x in read(os.path.join(d, "owd_us.txt")) if x.strip().isdigit())
        if owd and load <= NPROC / 2:
            p99s.append(owd[min(len(owd) - 1, int(0.99 * len(owd)))] / 1000)
    return {"p99": med(p99s), "delivered": 100.0 * deliv / off if off else float("nan"), "reps": len(p99s)}


def rules(base, tag, lab):
    out = {}
    for line in read(os.path.join(base, f"{tag}_{lab}", "summary.txt")):
        m = re.match(r"^(PASS|FAIL|VOID)\s+(R\d[^:]*):\s*(.*)$", line)
        if m:
            out.setdefault(m.group(2), []).append((m.group(1), line.strip()))
    return out


def rule_line(R, rule, arm):
    for v, line in R.get(rule, []):
        if f" {arm} " in line:
            return v, line
    return None, ""


def table_rev(base, tag, lab, arm):
    for line in read(os.path.join(base, f"{tag}_{lab}", "summary.txt")):
        f = line.split()
        if len(f) > 12 and f[0] == "rel_large" and f[1] == arm:
            try:
                return float(f[12])
            except ValueError:
                return float("nan")
    return float("nan")


def band(metric, m, a):
    kind, floor = FLOORS[metric]
    fl = floor if kind == "abs" else floor * abs(m)
    if metric == "duplicates":
        fl = max(fl, 5.0)
    return max(abs(a - m), fl)


def worse_rows(X, M, A, label):
    rows = []
    for arm in LARGE_ARMS:
        x, m, a = X.get(arm), M.get(arm), A.get(arm)
        if not (x and m and a):
            rows.append(f"{label} {arm}: missing")
            continue
        for metric in ["delivered", "p50", "p99", "resends", "duplicates", "cpu"]:
            xv, mv, av = x[metric], m[metric], a[metric]
            if any(v != v for v in (xv, mv, av)):
                continue
            b = band(metric, mv, av)
            d = (mv - xv) if metric == "delivered" else (xv - mv)
            verdict = "WORSE" if d > b else "NOT WORSE"
            print(f"  {label} {arm:7} {metric:10} M {mv:9.2f} A {av:9.2f} {label} {xv:9.2f}  {label}-M {xv - mv:+9.2f} "
                  f"band {b:8.2f} -> {verdict}")
            if verdict == "WORSE":
                rows.append(f"{label} {arm} {metric}: {xv:.2f} against M {mv:.2f} (band {b:.2f})")
        mres = med(m["resend_list"])
        storms_x = sum(1 for v in x["resend_list"] if mres > 0 and v > 10 * mres)
        storms_m = sum(1 for v in m["resend_list"] if mres > 0 and v > 10 * mres)
        if storms_x > storms_m:
            rows.append(f"{label} {arm}: {storms_x} STORM reps against M's {storms_m}")
    return rows


def main(base, tag):
    L = {lab: {arm: summarize(large_cell(base, tag, lab, arm)) for arm in LARGE_ARMS} for lab in "MAO"}
    S = {lab: {arm: small_cell(base, tag, lab, arm) for arm in SMALL_ARMS} for lab in "MAO"}
    R = {lab: rules(base, tag, lab) for lab in "MAO"}
    print(f"=== c11_reveal_read {base} {tag}")
    void = []
    for lab in "MAO":
        for arm in LARGE_ARMS:
            c = L[lab][arm]
            print(f"{lab} rel_large {arm:7} reps {c['n']} good {c['good']} unloaded {c['unloaded']} delivered "
                  f"{c['delivered']:.2f}% p50 {c['p50']:.1f} p99 {c['p99']:.1f} ms resends {c['resends']:.0f} dups "
                  f"{c['duplicates']:.0f} cpu {c['cpu']:.0f} ms reveal/rep "
                  f"{med([x for x in c['reveal'] if x is not None]) if any(x is not None for x in c['reveal']) else '-'}")
            if c["unloaded"] < MIN_UNLOADED:
                void.append(f"{lab} {arm}: {c['unloaded']} unloaded reps")
        for arm in SMALL_ARMS:
            c = S[lab][arm]
            print(f"{lab} rel_small {arm:7} reps {c['reps']} delivered {c['delivered']:.2f}% median per-rep p99 "
                  f"{c['p99']:.1f} ms")
    print("--- every tree's harness rules (R1-R7)")
    for lab in "MAO":
        for rule in sorted(R[lab]):
            if "as-registered" in rule or rule.endswith("-all") or "falsifier" in rule:
                continue
            for v, line in R[lab][rule]:
                print(f"  {lab}: {line[:220]}")
    # T
    o10 = [x for x in L["O"]["ge10b4"]["reveal"] if x is not None]
    octl = [x for x in L["O"]["ctl"]["reveal"] if x is not None]
    applied = bool(o10) and med(o10) >= 10
    print(f"T  O's reveal ACKNACKs a rep at ge10b4 {o10} (median {med(o10)}) -> {'APPLIED' if applied else 'NOT APPLIED'}")
    if any(x != 0 for x in octl):
        print(f"BUG O revealed at ctl (no loss): {octl}")
    # C
    m10, a10, o10p = (L[x]["ge10b4"]["p50"] for x in "MAO")
    spread = abs(a10 - m10)
    cvoid = not (spread == spread) or spread > 0.15 * m10
    print(f"C  ge10b4 median per-rep p50: M {m10:.1f} A {a10:.1f} O {o10p:.1f} ms; A/A spread S {spread:.1f} ms "
          f"({100 * spread / m10:.1f}% of M) -> {'VOID (over 15%)' if cvoid else 'PASS'}")
    print("C  A vs M under the NO WORSE rule (noise rows):")
    noise = worse_rows(L["A"], L["M"], L["M"], "A")
    for n in noise:
        print(f"  NOISE {n}")
    # P1
    need = max(2 * spread, 0.05 * m10)
    v, line = rule_line(R["O"], "R4", "ge10b4")
    fall = m10 - o10p
    if v == "PASS" and fall > need:
        p1 = "R4 FIXED"
    elif v == "PASS":
        p1 = "UNDECIDED (PASS, fall within the control's resolution)"
    elif v == "FAIL":
        lim = re.search(r"<= ([0-9.]+) ms", line)
        p1 = f"NOT FIXED ({o10p - float(lim.group(1)):+.1f} ms over the limit)" if lim else "NOT FIXED"
    else:
        p1 = "VOID (no R4 line for O at ge10b4)"
    print(f"P1 O's R4 ge10b4: {v} ({line}); fall {fall:.1f} ms against max(2 S, 5% of M) {need:.1f} -> {p1}")
    mv, mline = rule_line(R["M"], "R4", "ge10b4")
    print(f"STATE main R4 ge10b4: {mv} ({mline})")
    # P2
    ov5, ol5 = rule_line(R["O"], "R4", "ge5b4")
    mv5, _ = rule_line(R["M"], "R4", "ge5b4")
    p2 = "WORSE" if ov5 == "FAIL" and mv5 == "PASS" else ("PASS" if ov5 == "PASS" else f"{ov5}")
    print(f"P2 O's R4 ge5b4: {ov5} ({ol5}); M {mv5} -> {p2}")
    # NO WORSE
    print("NO WORSE O vs M (band = max(|A-M|, floor)):")
    worse = worse_rows(L["O"], L["M"], L["A"], "O")
    for arm in SMALL_ARMS:
        o, m = S["O"][arm], S["M"][arm]
        print(f"  (compiled out, noise floor) rel_small {arm}: delivered O {o['delivered']:.2f} M {m['delivered']:.2f}; "
              f"median per-rep p99 O {o['p99']:.1f} M {m['p99']:.1f}")
    print("NO WORSE: " + ("0 rows" if not worse else f"{len(worse)} row(s): " + "; ".join(worse)))
    # reverse bytes
    rev = table_rev(base, tag, "O", "ge10b4")
    print(f"REV O ge10b4 reverse bytes {rev:.2f}% of forward (M {table_rev(base, tag, 'M', 'ge10b4'):.2f}%) -> "
          f"{'over 2%: the ACKNACK would read as material' if rev > 2 else 'within 2%'}")
    # R7, main's state
    for lab in "MAO":
        v7 = R[lab].get("R7", [])
        print(f"R7 {lab}: {v7[0][1] if v7 else 'missing'}")
    # FALSIFIER
    if applied and not cvoid and fall <= need:
        print("FALSIFIER: FALSIFIED - the reveal applied and the ge10b4 p50 did not fall beyond the control's "
              "resolution: an order-revealed re-ask is not what holds R4 here")
    elif applied and not cvoid:
        print("FALSIFIER: not falsified")
    else:
        print("FALSIFIER: cannot be read (treatment or control void)")
    if void:
        print("VOID cells: " + "; ".join(void))
    trace = read(os.path.join(base, f"{tag}.trace.txt"))
    if trace:
        print("--- P5 trace (descriptive)")
        for line in trace:
            if any(k in line for k in ("P5 trace", "samples near", "reask_wait", "re-requests", "lat ", "retry timer")):
                print("  " + line.strip()[:200])
    land = applied and not cvoid and p1 == "R4 FIXED" and p2 == "PASS" and not worse and rev <= 2 and not void
    print(f"LAND (recommendation): {'screen passed: run the landing census' if land else 'do not land'}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
