#!/usr/bin/env python3
"""The reading of debt2_remeasure.sh, pre-registered 2026-10-20 before any of its runs.

Two ROADMAP measurement-debt items, each read by rules written here, in code:

F  "DATA_FRAG under-loss throughput in the full campaign". The p4 (2-fragment) half was confirmed in the full
   cross-vendor campaign by RESULTS `D` (c6, 12 reps, d58873cf: TickLE 888-898 Mbps at 5% loss, frag_reassembled =
   sent). What was never re-measured is DATAFRAG_PLAN 14's 4-fragment figure, 527-536 Mbps at 5% loss (bceddf2a,
   3 reps, TickLE only: TICKLE_DATAGRAM_BYTES=800 has no vendor counterpart, by design of build.sh). Input:
   frag_count_scaling.sh's output, unchanged harness, REPS reps per arm.
M  "discovery join time (M3)". DISCOVERY_PLAN M3: a node started into a running set of N-1 = 7 learns every peer's
   endpoints; median over the joins, after <= before + 5 ms. Last measured at 3d3ed747 (CONTEXT_NODE_PLAN stage 3,
   2026-09-27): 2.0 ms at E = 4, 1.8 ms at E = 32. Input: discovery_join.sh join 8 E 10 files, the parent arm
   (3d3ed747, tool c828e19f^) and the main arm interleaved P C C P per E.

Usage: debt2_read.py [--frag FILE] [--m3 FILE ...] --main SHA8 | --prereg | --selftest
"""
import re
import statistics
import sys

# ---- F rules ---------------------------------------------------------------------------------------------------------
F_N_MIN = 6  # ok reps an arm needs (of REPS, default 8) before any value is read
F_ID_BANDS = {"frag2": (1.9, 2.2), "frag4": (3.8, 4.3)}  # loss0 client datagrams per sample: the arm is what it says
F_C1_LOSSLESS_MIN = 900.0  # frag2 loss0 median Mbps: bceddf2a 939.6, RESULTS row 10 (D) 943
F_D_C6 = (888.2, 897.7)  # RESULTS D, c6 TickLE send_mbps over 12 reps, same core as origin/main 73dfb833
F_C2_BAND = (0.9 * F_D_C6[0], 1.1 * F_D_C6[1])  # frag2 loss5 must land here, or the rig/harness is not comparable
F_PUB = (527.0, 536.0)  # DATAFRAG_PLAN 14, frag4 loss5, bceddf2a
F_CONFIRM = (0.95 * F_PUB[0], 1.05 * F_PUB[1])
F_PERDGRAM_MAX = 4.4  # model 4.211, measured 4.045 at bceddf2a
F_WHOLESAMPLE_MIN = 4.7  # model 4.911, measured 4.857-5.107 before per-datagram seq_no

# ---- M rules ---------------------------------------------------------------------------------------------------------
M_PARENT = "3d3ed747"
M_PUB = {4: 2.0, 32: 1.8}  # DISCOVERY_PLAN 10, stage 3 column, ms
M_BAR_MS = 5.0  # DISCOVERY_PLAN M3
M_N_MIN = 16  # ok joins an arm needs per cell (of 20)

PREREG = f"""PRE-REGISTERED READING (debt2_read.py, 2026-10-20)
F  4-fragment DATA_FRAG under 5% loss, TickLE only (frag_count_scaling.sh: frag2/frag4 x loss0/loss5)
   row ok: drained=acked and recv == sent; IDENTITY FAIL rows are not ok
   N   every arm needs >= {F_N_MIN} ok reps, else F VOID (counts printed before any value)
   ID  loss0 median wire_role_packets_per_sample: frag2 in {F_ID_BANDS['frag2']}, frag4 in {F_ID_BANDS['frag4']},
       else F VOID (the arm is not the fragment count it is named)
   C1  frag2 loss0 median send_mbps >= {F_C1_LOSSLESS_MIN}, else F VOID (link not at its lossless ceiling)
   C2  frag2 loss5 median send_mbps in [{F_C2_BAND[0]:.1f}, {F_C2_BAND[1]:.1f}] (RESULTS D c6 TickLE {F_D_C6} +-10%),
       else F VOID (this harness/rig does not reproduce the campaign's own 2-fragment figure at the same core)
   F1  frag4 loss5 median send_mbps vs published {F_PUB}: in [{F_CONFIRM[0]:.1f}, {F_CONFIRM[1]:.1f}] CONFIRMED;
       above HIGHER (the published figure keeps its provenance, the current build's is added beside it);
       below BELOW (a regression against the published figure, to Dev)
   F2  frag4 loss5 median wire_role_packets_per_sample <= {F_PERDGRAM_MAX} PER-DATAGRAM HOLDS; >= {F_WHOLESAMPLE_MIN}
       WHOLE-SAMPLE (per-datagram seq_no regressed); between AMBIGUOUS
   Not claimed: a cross-vendor 4-fragment cell. No vendor arm exists at 800 B datagrams; putting one there needs a
   non-default vendor fragment size, which is the user's decision (DDS default first).
M  discovery join time (M3), PC private netns, N=8, E in (4, 32), 10 joins x 2 runs per arm, parent {M_PARENT}
   N   every arm needs >= {M_N_MIN} ok joins per cell, else that cell VOID (counts printed first)
   C   the parent arm's median <= its published value + {M_BAR_MS} ms ({M_PUB}), else that cell VOID (PC loaded or
       tool broken: the control does not reproduce its own history)
   M3  main median <= parent median + {M_BAR_MS} ms PASS, else FAIL"""


def med(xs):
    return statistics.median(xs) if xs else None


def first(key, text):
    m = re.search(r"(?:^| )" + re.escape(key) + r"=([^ ]+)", text)
    return m.group(1) if m else None


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def read_frag(lines):
    """Returns (verdict lines, summary verdict)."""
    arms = {}
    row_re = re.compile(r"^(frag[24]) (loss[05]) rep(\d+) \| (.*)$")
    for line in lines:
        m = row_re.match(line.rstrip("\n"))
        if not m:
            continue
        arm = (m.group(1), m.group(2))
        rest = m.group(4)
        a = arms.setdefault(arm, {"rows": 0, "ok": [], "bad": []})
        a["rows"] += 1
        if rest.startswith("IDENTITY FAIL"):
            a["bad"].append(rest)
            continue
        sent, recv = first("sent", rest), first("recv", rest)
        mbps, pk = num(first("send_mbps", rest)), num(first("wire_role_packets_per_sample", rest))
        if first("drained", rest) != "acked" or sent is None or sent != recv or mbps is None or pk is None:
            a["bad"].append(rest[:120])
            continue
        a["ok"].append((mbps, pk))
    out = []
    names = [(f, l) for f in ("frag2", "frag4") for l in ("loss0", "loss5")]
    out.append("F  N: " + ", ".join(f"{f} {l} {len(arms.get((f, l), {}).get('ok', []))} ok of "
                                    f"{arms.get((f, l), {}).get('rows', 0)}" for f, l in names))
    if any(len(arms.get(k, {}).get("ok", [])) < F_N_MIN for k in names):
        out.append(f"F  VOID(N): an arm has fewer than {F_N_MIN} ok reps")
        return out, "VOID"
    st = {}
    for k in names:
        ok = arms[k]["ok"]
        st[k] = (med([x for x, _ in ok]), min(x for x, _ in ok), max(x for x, _ in ok), med([p for _, p in ok]))
        out.append(f"F  {k[0]} {k[1]}: send_mbps median {st[k][0]:.1f} (range {st[k][1]:.1f}-{st[k][2]:.1f}), "
                   f"datagrams/sample median {st[k][3]:.3f}")
    for f, (lo, hi) in F_ID_BANDS.items():
        p = st[(f, "loss0")][3]
        if not lo <= p <= hi:
            out.append(f"F  VOID(ID): {f} loss0 reads {p:.3f} datagrams/sample, outside {lo}-{hi}")
            return out, "VOID"
    out.append("F  ID ok: loss0 datagrams/sample match 2 and 4 fragments")
    if st[("frag2", "loss0")][0] < F_C1_LOSSLESS_MIN:
        out.append(f"F  VOID(C1): frag2 loss0 {st[('frag2', 'loss0')][0]:.1f} < {F_C1_LOSSLESS_MIN}")
        return out, "VOID"
    c2 = st[("frag2", "loss5")][0]
    if not F_C2_BAND[0] <= c2 <= F_C2_BAND[1]:
        out.append(f"F  VOID(C2): frag2 loss5 {c2:.1f} outside {F_C2_BAND[0]:.1f}-{F_C2_BAND[1]:.1f} (D c6)")
        return out, "VOID"
    out.append(f"F  C1, C2 PASS: lossless {st[('frag2', 'loss0')][0]:.1f}, 2-fragment 5% loss {c2:.1f} (D c6 {F_D_C6})")
    v = st[("frag4", "loss5")][0]
    f1 = "CONFIRMED" if F_CONFIRM[0] <= v <= F_CONFIRM[1] else ("HIGHER" if v > F_CONFIRM[1] else "BELOW")
    out.append(f"F1 {f1}: frag4 loss5 median {v:.1f} Mbps against published {F_PUB[0]:.0f}-{F_PUB[1]:.0f} "
               f"(band {F_CONFIRM[0]:.1f}-{F_CONFIRM[1]:.1f})")
    p = st[("frag4", "loss5")][3]
    f2 = "PER-DATAGRAM HOLDS" if p <= F_PERDGRAM_MAX else ("WHOLE-SAMPLE" if p >= F_WHOLESAMPLE_MIN else "AMBIGUOUS")
    out.append(f"F2 {f2}: frag4 loss5 {p:.3f} datagrams/sample (per-datagram model 4.211, whole-sample 4.911)")
    return out, f1 + "/" + f2


def read_m3(files_lines, main8):
    cells = {}  # (E, core8) -> {"rows": n, "ok": [ms]}
    head_re = re.compile(r"^=== discovery join, .* core ([0-9a-f]{8}), .*N=(\d+) E=(\d+) count=(\d+)")
    row_re = re.compile(r"^join \d+ \| (ok|VOID[^|]*) \| (.*)$")
    for lines in files_lines:
        key = None
        for line in lines:
            h = head_re.match(line)
            if h:
                key = (int(h.group(3)), h.group(1))
                cells.setdefault(key, {"rows": 0, "ok": []})
                continue
            r = row_re.match(line.rstrip("\n"))
            if r and key:
                cells[key]["rows"] += 1
                ms = num(first("reached_ms", r.group(2)))
                if r.group(1) == "ok" and ms is not None and ms >= 0:
                    cells[key]["ok"].append(ms)
    out, verdicts = [], []
    for e in sorted(M_PUB):
        p = cells.get((e, M_PARENT), {"rows": 0, "ok": []})
        c = cells.get((e, main8), {"rows": 0, "ok": []})
        out.append(f"M  E={e} N: parent {M_PARENT} {len(p['ok'])} ok of {p['rows']}, main {main8} "
                   f"{len(c['ok'])} ok of {c['rows']}")
        if len(p["ok"]) < M_N_MIN or len(c["ok"]) < M_N_MIN:
            out.append(f"M  E={e} VOID(N): an arm has fewer than {M_N_MIN} ok joins")
            verdicts.append("VOID")
            continue
        pm, cm = med(p["ok"]), med(c["ok"])
        out.append(f"M  E={e}: median reached_ms parent {pm:.2f} (published {M_PUB[e]}), main {cm:.2f}; "
                   f"p90 parent {sorted(p['ok'])[int(0.9 * (len(p['ok']) - 1))]:.2f}, "
                   f"main {sorted(c['ok'])[int(0.9 * (len(c['ok']) - 1))]:.2f}")
        if pm > M_PUB[e] + M_BAR_MS:
            out.append(f"M  E={e} VOID(C): the parent reads {pm:.2f} ms, above its published {M_PUB[e]} + {M_BAR_MS}")
            verdicts.append("VOID")
            continue
        v = "PASS" if cm <= pm + M_BAR_MS else "FAIL"
        out.append(f"M3 E={e} {v}: main {cm:.2f} ms against parent {pm:.2f} + {M_BAR_MS} ms")
        verdicts.append(v)
    return out, ",".join(verdicts)


def selftest():
    fails = []

    def frow(f, l, r, mbps, pk, extra=""):
        return (f"{f} {l} rep{r} | sent=100 send_mbps={mbps} drained=acked wire_role_packets_per_sample={pk} "
                f"recv=100 frag_abandoned=0 {extra}")

    def fset(f4l5=530.0, f4pk=4.05, f2l5=890.0, f2l0=940.0, f4l0pk=4.02, n=8):
        ls = []
        for r in range(1, n + 1):
            ls.append(frow("frag2", "loss0", r, f2l0, 2.02))
            ls.append(frow("frag2", "loss5", r, f2l5, 2.08))
            ls.append(frow("frag4", "loss0", r, 889, f4l0pk))
            ls.append(frow("frag4", "loss5", r, f4l5, f4pk))
        return ls

    cases = [
        (fset(), "CONFIRMED/PER-DATAGRAM HOLDS"),
        (fset(f4l5=600), "HIGHER/PER-DATAGRAM HOLDS"),
        (fset(f4l5=300, f4pk=4.9), "BELOW/WHOLE-SAMPLE"),
        (fset(f4pk=4.5), "CONFIRMED/AMBIGUOUS"),
        (fset(n=5), "VOID"),
        (fset(f4l0pk=2.02), "VOID"),  # the 800 B build silently not applied
        (fset(f2l5=700), "VOID"),  # control C2
        (fset(f2l0=850), "VOID"),  # control C1
    ]
    for i, (ls, want) in enumerate(cases):
        got = read_frag(ls)[1]
        if got != want:
            fails.append(f"frag case {i}: want {want}, got {got}")
    # a row that did not drain, or lost samples, is not ok
    ls = fset()
    ls = [x.replace("drained=acked", "drained=timeout") if x.startswith("frag4 loss5 rep1 ") or
          x.startswith("frag4 loss5 rep2 ") or x.startswith("frag4 loss5 rep3 ") else x for x in ls]
    if read_frag(ls)[1] != "VOID":
        fails.append("frag: 3 undrained rows of 8 should void the arm")
    ls = [x.replace("recv=100", "recv=99") for x in fset()]
    if read_frag(ls)[1] != "VOID":
        fails.append("frag: recv != sent should not count")
    ls = fset() + ["frag4 loss5 rep9 | IDENTITY FAIL: not datagram_bytes=800"]
    if "of 9" not in read_frag(ls)[0][0]:
        fails.append("frag: identity-failed rows must be counted as rows")

    def mfile(core, e, vals, void=0):
        ls = [f"=== discovery join, 2026-10-20T04:00:00+09:00, core {core}, tool x, N=8 E={e} count={len(vals)} ==="]
        ls += [f"join {j + 1} | ok | node=8 endpoints={e} seconds=8 expected={7 * e} reached_ms={v} dips=0"
               for j, v in enumerate(vals)]
        ls += [f"join {len(vals) + k + 1} | VOID(never reached) | expected={7 * e} reached_ms=-1" for k in range(void)]
        return ls

    m = "73dfb833"
    ok10 = [1.8] * 10
    base = [mfile(M_PARENT, 4, ok10), mfile(m, 4, ok10), mfile(m, 4, ok10), mfile(M_PARENT, 4, ok10),
            mfile(M_PARENT, 32, ok10), mfile(m, 32, ok10), mfile(m, 32, ok10), mfile(M_PARENT, 32, ok10)]
    mcases = [
        (base, "PASS,PASS"),
        (base[:1] + [mfile(m, 4, [9.0] * 10)] * 2 + base[3:], "FAIL,PASS"),
        ([mfile(M_PARENT, 4, [8.0] * 10)] * 2 + base[1:3] + base[4:], "VOID,PASS"),  # control
        ([mfile(M_PARENT, 4, [1.8] * 7, void=3)] * 2 + base[1:3] + base[4:], "VOID,PASS"),  # N: 14 of 20
        (base[:4], "PASS,VOID"),  # E=32 never ran
    ]
    for i, (fl, want) in enumerate(mcases):
        got = read_m3(fl, m)[1]
        if got != want:
            fails.append(f"m3 case {i}: want {want}, got {got}")
    for f in fails:
        print("SELFTEST FAIL:", f)
    print("SELFTEST OK" if not fails else f"SELFTEST FAILED ({len(fails)})")
    return 0 if not fails else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if "--prereg" in argv:
        print(PREREG)
        return 0
    frag, m3, main8 = None, [], None
    i = 0
    while i < len(argv):
        if argv[i] == "--frag":
            frag = argv[i + 1]
            i += 2
        elif argv[i] == "--main":
            main8 = argv[i + 1][:8]
            i += 2
        elif argv[i] == "--m3":
            i += 1
            while i < len(argv) and not argv[i].startswith("--"):
                m3.append(argv[i])
                i += 1
        else:
            print(f"unknown argument {argv[i]}", file=sys.stderr)
            return 2
    print(PREREG)
    print()
    if frag:
        try:
            with open(frag, encoding="utf-8", errors="replace") as fh:
                lines, v = read_frag(fh.readlines())
        except OSError as e:
            lines, v = [f"F  NOT READ: {e}"], "NOT READ"
        print("\n".join(lines))
        print(f"F  verdict: {v}")
    else:
        print("F  NOT RUN")
    if m3:
        if not main8:
            print("--main is required with --m3", file=sys.stderr)
            return 2
        fl = []
        for p in m3:
            try:
                with open(p, encoding="utf-8", errors="replace") as fh:
                    fl.append(fh.readlines())
            except OSError as e:
                print(f"M  NOT READ: {p}: {e}")
        lines, v = read_m3(fl, main8)
        print("\n".join(lines))
        print(f"M  verdict (E=4,E=32): {v}")
    else:
        print("M  NOT RUN")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
