#!/usr/bin/env python3
"""c11_param_read.py - the reading c11_param_ab.sh pre-registers (its header holds the rules; this applies them).

Usage: c11_param_read.py OUTB TAG  - reads OUTB/TAG_{M,A,F,O}/ (wifi_burst_loss_netns.sh outputs).
"""
import os
import re
import sys

from c11_reveal_read import (LARGE_ARMS, SMALL_ARMS, MIN_UNLOADED, FLOORS, read, kv, med, large_cell, summarize,
                             small_cell, rules, rule_line, band, worse_rows)

TREES = "MAFO"
O_C11AB_P50 = 56.5  # O's ge10b4 median per-rep p50, c11ab.read.txt (d80877cf, 2026-10-30)
METRICS = ["delivered", "p50", "p99", "resends", "duplicates", "cpu"]


def rep_dirs(base, tag, lab, cell):
    runs = os.path.join(base, f"{tag}_{lab}", "runs")
    for name in sorted(os.listdir(runs)) if os.path.isdir(runs) else []:
        arm, c, rep = name.rsplit("-", 2)
        if c == cell:
            yield arm, rep, os.path.join(runs, name)


def treatment(base, tag, lab):
    """Per rep: the REVEAL: values every node log reported, and the reveal ACKNACKs all its readers sent."""
    out = []
    for cell in ("rel_large", "rel_small"):
        for arm, rep, d in rep_dirs(base, tag, lab, cell):
            modes, reveals = [], 0
            for log in ("echo.log", "ping.log"):
                for line in read(os.path.join(d, log)):
                    if line.startswith("REVEAL:"):
                        modes.append(int(kv(line).get("repair_reveal", -1)))
                    if line.startswith("RSTATS:") and "acknack_tx" in line:
                        reveals += int(kv(line).get("reveal", 0))
            out.append((cell, arm, rep, modes, reveals))
    return out


def off_floor_rows(X, M, label):
    rows = []
    for arm in LARGE_ARMS:
        x, m = X.get(arm), M.get(arm)
        if not (x and m):
            rows.append(f"{label} {arm}: missing")
            continue
        for metric in METRICS:
            xv, mv = x[metric], m[metric]
            if xv != xv or mv != mv:
                continue
            fl = band(metric, mv, mv)  # the floor alone
            if abs(xv - mv) > fl:
                rows.append(f"{label} {arm} {metric}: {xv:.2f} against M {mv:.2f} (floor {fl:.2f})")
    return rows


def small_extra(base, tag, lab, arm):
    """rel_small: writer resends (both nodes publish), duplicates the readers counted, reveals a rep."""
    resends, dups, reveals = [], [], []
    for a, rep, d in rep_dirs(base, tag, lab, "rel_small"):
        if a != arm:
            continue
        r = u = v = 0
        for log in ("echo.log", "ping.log"):
            for line in read(os.path.join(d, log)):
                if line.startswith("RESULT"):
                    k = kv(line)
                    r += int(k.get("retransmitted", 0))
                    u += int(k.get("duplicates", 0))
                if line.startswith("RSTATS:") and "acknack_tx" in line:
                    v += int(kv(line).get("reveal", 0))
        resends.append(r)
        dups.append(u)
        reveals.append(v)
    return resends, dups, reveals


def main(base, tag):
    L = {lab: {arm: summarize(large_cell(base, tag, lab, arm)) for arm in LARGE_ARMS} for lab in TREES}
    S = {lab: {arm: small_cell(base, tag, lab, arm) for arm in SMALL_ARMS} for lab in TREES}
    R = {lab: rules(base, tag, lab) for lab in TREES}
    print(f"=== c11_param_read {base} {tag}")
    void = []
    for lab in TREES:
        for arm in LARGE_ARMS:
            c = L[lab][arm]
            rv = [x for x in c["reveal"] if x is not None]
            print(f"{lab} rel_large {arm:7} reps {c['n']} good {c['good']} unloaded {c['unloaded']} delivered "
                  f"{c['delivered']:.2f}% p50 {c['p50']:.1f} p99 {c['p99']:.1f} ms resends {c['resends']:.0f} dups "
                  f"{c['duplicates']:.0f} cpu {c['cpu']:.0f} ms reveal/rep {med(rv) if rv else '-'}")
            if c["unloaded"] < MIN_UNLOADED:
                void.append(f"{lab} {arm}: {c['unloaded']} unloaded reps")
        for arm in SMALL_ARMS:
            c = S[lab][arm]
            res, dup, rev = small_extra(base, tag, lab, arm)
            print(f"{lab} rel_small {arm:7} reps {c['reps']} delivered {c['delivered']:.2f}% median per-rep p99 "
                  f"{c['p99']:.1f} ms; resends/rep {res} duplicates/rep {dup} reveals/rep {rev}")
    print("--- every tree's harness rules (R1-R7)")
    for lab in TREES:
        for rule in sorted(R[lab]):
            if "as-registered" in rule or rule.endswith("-all") or "falsifier" in rule:
                continue
            for _, line in R[lab][rule]:
                print(f"  {lab}: {line[:220]}")
    # TREATMENT REPORTED and T_OFF
    want = {"F": 1, "O": 3}
    t_off = True
    for lab, mode in want.items():
        for cell, arm, rep, modes, reveals in treatment(base, tag, lab):
            if not modes or any(x != mode for x in modes):
                void.append(f"{lab} {cell} {arm} rep {rep}: REVEAL lines {modes}, not repair_reveal={mode}")
            if lab == "F" and reveals != 0:
                t_off = False
                print(f"BUG F (OFF) sent {reveals} reveal ACKNACKs in {cell} {arm} rep {rep}")
    print(f"TREATMENT reported: {'every F rep OFF, every O rep ALL_MISSING' if not any('REVEAL lines' in v for v in void) else 'NOT as named (VOID rows below)'}")
    print(f"T_OFF F sent no reveal: {'HOLDS' if t_off else 'BUG'}")
    o10 = [x for x in L["O"]["ge10b4"]["reveal"] if x is not None]
    applied = bool(o10) and med(o10) >= 10
    print(f"T_ALL O's reveals a rep at ge10b4 {o10} (median {med(o10)}) -> {'APPLIED' if applied else 'NOT APPLIED'}")
    # C
    m10, a10, f10, o10p = (L[x]["ge10b4"]["p50"] for x in "MAFO")
    spread = abs(a10 - m10)
    cvoid = not (spread == spread) or spread > 0.15 * m10
    print(f"C  ge10b4 median per-rep p50: M {m10:.1f} A {a10:.1f} F {f10:.1f} O {o10p:.1f} ms; A/A spread S "
          f"{spread:.1f} ms ({100 * spread / m10:.1f}% of M) -> {'VOID (over 15%)' if cvoid else 'PASS'}")
    # IDENTITY
    a_rows = off_floor_rows(L["A"], L["M"], "A")
    f_rows = off_floor_rows(L["F"], L["M"], "F")
    for r in a_rows:
        print(f"  OFF-FLOOR {r}")
    for r in f_rows:
        print(f"  OFF-FLOOR {r}")
    f_void = [v for v in void if v.startswith("F ") or v.startswith("M ") or v.startswith("A ")]
    identity = t_off and not f_void and len(f_rows) <= len(a_rows) + 1
    print(f"IDENTITY (OFF == main): F {len(f_rows)} off-floor rows, A {len(a_rows)} -> "
          f"{'HOLDS' if identity else 'NOT SHOWN'}{' (VOID cells: ' + '; '.join(f_void) + ')' if f_void else ''}")
    for arm in SMALL_ARMS:
        f, m, a = S["F"][arm], S["M"][arm], S["A"][arm]
        print(f"  rel_small {arm}: delivered M {m['delivered']:.2f} A {a['delivered']:.2f} F {f['delivered']:.2f}; "
              f"median per-rep p99 M {m['p99']:.1f} A {a['p99']:.1f} F {f['p99']:.1f} ms")
    # REPRODUCE
    need = max(2 * spread, 0.05 * m10)
    v, line = rule_line(R["O"], "R4", "ge10b4")
    fall = m10 - o10p
    fixed = v == "PASS" and fall > need and applied and not cvoid
    near = abs(o10p - O_C11AB_P50) <= 0.20 * O_C11AB_P50
    rep_verdict = ("REPRODUCED" if fixed and near else
                   "FIXED, NOT THE SAME FIGURE" if fixed else "NOT REPRODUCED")
    print(f"REPRODUCE O's R4 ge10b4: {v} ({line}); fall {fall:.1f} ms against max(2 S, 5% of M) {need:.1f}; p50 "
          f"{o10p:.1f} against c11ab's {O_C11AB_P50} (+-20%) -> {rep_verdict}")
    print("COST: NO WORSE O vs M (band = max(|A-M|, floor)):")
    worse = worse_rows(L["O"], L["M"], L["A"], "O")
    print("COST: " + ("0 rows" if not worse else f"{len(worse)} row(s): " + "; ".join(worse)))
    # SMALL
    print("SMALL (descriptive): ALL_MISSING on small samples, O against M")
    for arm in SMALL_ARMS:
        o, m = S["O"][arm], S["M"][arm]
        ores, odup, orev = small_extra(base, tag, "O", arm)
        mres, mdup, _ = small_extra(base, tag, "M", arm)
        print(f"  rel_small {arm}: delivered O {o['delivered']:.2f} M {m['delivered']:.2f}; median per-rep p99 O "
              f"{o['p99']:.1f} M {m['p99']:.1f} ms; resends/rep median O {med(ores)} M {med(mres)}; duplicates/rep "
              f"median O {med(odup)} M {med(mdup)}; reveals/rep median O {med(orev)}")
    for lab in "MO":
        v7 = R[lab].get("R7", [])
        print(f"  R7 {lab}: {v7[0][1] if v7 else 'missing'}")
    if void:
        print("VOID: " + "; ".join(void))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
