#!/usr/bin/env python3
"""udp_csum0_pc_read.py - reads udp_csum0_pc.sh's results by the rules written in its header before the first run.

Usage: udp_csum0_pc_read.py RESULTS   (the last "=== udp_csum0_pc" session in the file)
       udp_csum0_pc_read.py --selftest
Exit 0 when every cell has a subscriber verdict, 2 when one could not be read.
"""
import math
import statistics
import sys

ARMS = ("OFF", "ON", "OFF2")
CELLS = ("be1m", "rel1m", "be4m", "rel4m")
PRIMARY = "sub_insn_per_mb"
METRICS = (PRIMARY, "sub_insn_k_per_mb", "sub_cyc_per_mb", "pub_insn_per_mb", "pub_insn_k_per_mb", "pub_cyc_per_mb")
MIN_RUNS = 2


def kv(text):
    return dict(x.split("=", 1) for x in text.split() if "=" in x)


def parse_lines(lines):
    starts = [i for i, line in enumerate(lines) if line.startswith("=== udp_csum0_pc")]
    if not starts:
        return None, []
    runs = []
    for line in lines[starts[-1]:]:
        if not line.startswith("RUN "):
            continue
        parts = line.split(" | ")
        head = kv(parts[0])
        if len(parts) < 3:
            runs.append((head, None, None))
            continue
        runs.append((head, kv(parts[1]), kv(parts[2])))
    return lines[starts[-1]], runs


def treatment_problem(head, pub):
    """'' when the arm's treatment is as required, else why not."""
    if "pub_csum0" not in head or "pub_csum0_datagrams" not in head:
        return "TREATMENT NOT READ: the publisher's rx line lacks csum0= / csum0_datagrams="
    on, sent = head["pub_csum0"], int(head["pub_csum0_datagrams"])
    if head["arm"] == "ON":
        published = int(pub.get("published", "0"))
        if on != "1" or sent < max(1, published):
            return f"TREATMENT NOT APPLIED: csum0={on} csum0_datagrams={sent} for {published} published"
        return ""
    if on != "0" or sent != 0:
        return f"TREATMENT: control {head['arm']} has csum0={on} csum0_datagrams={sent}"
    return ""


def void_reason(head, pub, sub):
    if pub is None or sub is None:
        return "no RESULT"
    published, delivered = int(pub["published"]), int(sub["delivered"])
    if int(sub["torn"]) or int(sub["out_of_order"]):
        return "torn/out_of_order"
    if int(sub["reliable"]) and delivered < published:
        return f"RELIABLE delivered {delivered} < published {published}"
    if not int(sub["reliable"]) and delivered < 0.98 * published:
        return f"BEST_EFFORT delivered {delivered} < 98% of {published}"
    if delivered == 0 or published == 0:
        return "nothing delivered"
    for side in ("sub", "pub"):
        for key in ("instructions_u", "instructions_k", "cycles_u", "cycles_k"):
            if not head.get(f"{side}_{key}", "none").isdigit():
                return f"no perf count {side}_{key}"
    return treatment_problem(head, pub) or None


def values(head, pub, sub):
    size = int(sub["size"])
    got = int(sub["delivered"]) * size / 1e6
    sent = int(pub["published"]) * size / 1e6
    out = {}
    for side, mb in (("sub", got), ("pub", sent)):
        iu, ik = int(head[f"{side}_instructions_u"]), int(head[f"{side}_instructions_k"])
        out[f"{side}_insn_per_mb"] = (iu + ik) / mb
        out[f"{side}_insn_k_per_mb"] = ik / mb
        out[f"{side}_cyc_per_mb"] = (int(head[f"{side}_cycles_u"]) + int(head[f"{side}_cycles_k"])) / mb
    out["csum0_per_sample"] = int(head["pub_csum0_datagrams"]) / int(pub["published"])
    return out


def mean_se(xs):
    m = statistics.mean(xs)
    se = statistics.stdev(xs) / math.sqrt(len(xs)) if len(xs) > 1 else float("inf")
    return m, se


def judge(cells, out):
    """{cell: {arm: [values]}} -> {cell: {metric: verdict}} (None: the cell not read)."""
    verdicts = {}
    for cell in CELLS:
        arms = cells.get(cell, {})
        if any(len(arms.get(a, [])) < MIN_RUNS for a in ARMS):
            out(f"CELL {cell} NOT READ: runs " + ", ".join(f"{a} {len(arms.get(a, []))}" for a in ARMS)
                + f" (fewer than {MIN_RUNS})")
            verdicts[cell] = None
            continue
        verdicts[cell] = {}
        for metric in METRICS:
            off = [v[metric] for v in arms["OFF"]]
            on = [v[metric] for v in arms["ON"]]
            off2 = [v[metric] for v in arms["OFF2"]]
            m_off, s_off = mean_se(off)
            m_on, s_on = mean_se(on)
            m_off2, _ = mean_se(off2)
            floor = max(abs(m_off2 - m_off), 2 * math.sqrt(s_off * s_off + s_on * s_on))
            delta = m_on - m_off
            verdict = "LOWER" if delta < -floor else "HIGHER" if delta > floor else "HELD"
            verdicts[cell][metric] = verdict
            out(f"CELL {cell} {metric}: OFF {m_off:,.0f} OFF2 {m_off2:,.0f} ON {m_on:,.0f} delta {delta:+,.0f} "
                f"({100 * delta / m_off:+.2f}%) floor {floor:,.0f} -> {verdict}")
        per = statistics.mean(v["csum0_per_sample"] for v in arms["ON"])
        out(f"CELL {cell} treatment: ON sent {per:.1f} checksum-0 datagrams a sample (context)")
    return verdicts


def read(lines, out=print):
    header, runs = parse_lines(lines)
    if header is None:
        out("READING: NOT READ (no '=== udp_csum0_pc' session)")
        return 2
    out(header)
    cells = {}
    for head, pub, sub in runs:
        reason = void_reason(head, pub, sub)
        if reason:
            out(f"VOID {head.get('cell')} {head.get('arm')} block {head.get('block')}: {reason}")
            continue
        cells.setdefault(head["cell"], {}).setdefault(head["arm"], []).append(values(head, pub, sub))
    verdicts = judge(cells, out)
    if any(verdicts[c] is None for c in CELLS):
        out("READING: NOT READ (a cell has no verdict)")
        return 2
    sub = [verdicts[c][PRIMARY] for c in CELLS]
    if all(v == "LOWER" for v in sub):
        text = "VISIBLE on the PC - fewer subscriber instructions per MB in every cell"
    elif any(v == "HIGHER" for v in sub):
        text = "REFUTED on the PC - more subscriber instructions per MB in a cell"
    elif all(v == "HELD" for v in sub):
        text = "NOT VISIBLE on the PC (not evidence about the rig)"
    else:
        text = "MIXED - " + ", ".join(f"{c} {v}" for c, v in zip(CELLS, sub))
    pub_harm = [c for c in CELLS if verdicts[c]["pub_insn_per_mb"] == "HIGHER"]
    if pub_harm:
        text += "; PUBLISHER HARM in " + ", ".join(pub_harm)
    out("READING: " + text)
    return 0


def selftest():
    def line(cell, arm, block, sub_k, pub_k=3_000_000, csum0=None, sent0=None, delivered=300, torn=0):
        on = arm == "ON"
        csum0 = ("1" if on else "0") if csum0 is None else csum0
        sent0 = (16 * 300 if on else 0) if sent0 is None else sent0
        rel = 1 if cell.startswith("rel") else 0
        counts = (f"sub_instructions_u=1000000 sub_instructions_k={sub_k + block * 1000} sub_cycles_u=1 sub_cycles_k=1 "
                  f"pub_instructions_u=1000000 pub_instructions_k={pub_k + block * 1000} pub_cycles_u=1 pub_cycles_k=1 ")
        tx = f"pub_csum0={csum0} pub_csum0_datagrams={sent0} " if csum0 != "absent" else ""
        return (f"RUN cell={cell} arm={arm} block={block} {counts}{tx}| role=pub size=1048576 published=300 | "
                f"role=sub size=1048576 reliable={rel} delivered={delivered} torn={torn} out_of_order=0")

    def session(**kw):
        sub_on = kw.get("sub_on", 2_700_000)
        pub_on = kw.get("pub_on", 3_000_000)
        rows = ["=== udp_csum0_pc test ==="]
        for cell in CELLS:
            for block in (1, 2, 3):
                for arm in ARMS:
                    extra = kw.get("change", {}).get((cell, arm, block), {})
                    rows.append(line(cell, arm, block, sub_on if arm == "ON" else 3_000_000,
                                     pub_on if arm == "ON" else 3_000_000, **extra))
        return rows

    checks = [
        ("lower everywhere", session(), 0, "READING: VISIBLE"),
        ("held everywhere", session(sub_on=3_000_000), 0, "READING: NOT VISIBLE"),
        ("higher", session(sub_on=3_300_000), 0, "REFUTED"),
        ("publisher harm", session(pub_on=3_300_000), 0, "PUBLISHER HARM"),
        ("ON without the treatment: its runs VOID, cell not read",
         session(change={("be1m", "ON", b): {"sent0": 0} for b in (1, 2, 3)}), 2, "NOT READ"),
        ("ON's field absent: VOID",
         session(change={("rel4m", "ON", b): {"csum0": "absent"} for b in (1, 2)}), 2, "TREATMENT NOT READ"),
        ("control with the treatment: VOID",
         session(change={("be4m", "OFF", b): {"csum0": "1"} for b in (1, 2)}), 2, "TREATMENT: control"),
        ("RELIABLE incomplete: VOID", session(change={("rel1m", "OFF", b): {"delivered": 299} for b in (1, 2)}), 2,
         "RELIABLE delivered"),
        ("no session", [], 2, "NOT READ"),
    ]
    fails = 0
    for name, rows, want_rc, want in checks:
        got = []
        rc = read(rows, out=got.append)
        ok = rc == want_rc and any(want in g for g in got)
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: rc {rc}, '{want}' {'found' if ok else 'missing'}")
        fails += 0 if ok else 1
    print("SELFTEST OK" if not fails else f"SELFTEST FAILED: {fails}")
    return 1 if fails else 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        return read(f.read().splitlines())


if __name__ == "__main__":
    sys.exit(main())
