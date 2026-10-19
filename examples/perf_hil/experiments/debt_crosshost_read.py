#!/usr/bin/env python3
"""The reading of debt_crosshost_remeasure.sh (ROADMAP "Measurement debt", written 2026-10-19 before any of its runs).

Usage: debt_crosshost_read.py <A.txt> <T.txt>          (campaign_sweep.sh output files, FastDDS shipped / 1472)
       debt_crosshost_read.py --selftest

It decides nothing the rows' own reading decides: every WIN/DRAW/LOSE/VOID is campaign_summary.py's (instrument=ok, the
boundary gate, VOID, delivery, goodput G1-G6, win only outside the reps' spread), and the row numbers are
fair_crosshost_rows.py's, both run by the driver on the same two files. This adds, per cell, what those do not print:

  N  Sample counts first: for each framework the reps with an ok row and the VOID ones. A cell with no row at all for a
     framework is "NOT MEASURED", never an empty line.
  O  The realized framework order of every rep, in the order the rows were written, and how often each framework ran
     first. campaign_sweep.sh rotates by one per cell and per rep; ORDER NOT ROTATED is printed when a framework never
     ran first in a cell with at least 3 reps (the rotation did not happen, whatever the code says).
  P  Placement (the ◊ debt, COMPARISON to-do 14 of 2026-09-23: eth0's IRQ is served on CPU 0 of both Pis, and a
     sender that lands there read ~15% low; the cure then was taskset -c 1-3, the rule now is unpinned). Per framework:
     n0 = reps whose client reported cpu_main=0, n1 = the others, and the median goodput (server.win_recv_mbps, the
     figure campaign_summary scores) of each group.
       P1 "both modes" when n0 >= 1 and n1 >= 1; otherwise "one mode only" with the counts.
       P2 with n0 >= 2 and n1 >= 2: COIN FLIP SEEN when every CPU-0 rep is below every other rep; REVERSED when every
          CPU-0 rep is above; NOT SEEN otherwise. With fewer: NOT READ (n0, n1) - too few to say either way.
       P3 Placement never re-scores a cell: the headline is the verdict over every rep, unpinned, as anyone runs it.
          A COIN FLIP SEEN is reported beside it.
  R  c6's `retransmitted` figure (COMPARISON 2.8, 2026-09-25: "the prediction, written before the run, is
     retransmitted at about 46x sent and gap_abandoned = 0"). Per TickLE rep: sent, retransmitted, their ratio,
     gap_abandoned and gap_evicted (server), and sample_path.
       R1 The 46x prediction: CONFIRMED if the median ratio is within 2x of 46 (23-92), REFUTED below 4.6 or above
          460, NEITHER otherwise. It was made for one IP-fragmented datagram per sample (a lost fragment costs the whole
          datagram); sample_path= says which path this build took.
       R2 gap_abandoned = 0 and gap_evicted = 0 in every TickLE rep: HOLDS, or FALSIFIED with the reps that broke it.
          A rep without the field is NOT READ for R2, never counted as 0.
       R3 Against TickLE's own previous figure (3ae721aa, 2026-10-06, the c6 the T rows publish: ratios 0.184, 0.194,
          0.215; read from fair_crosshost_3ae721aa_T6.T_free.txt): CHANGED UP / CHANGED DOWN when every rep now lies
          above / below that range, NO CHANGE SHOWN otherwise. Informational: retransmitted is TickLE's own counter and
          never scored against a vendor.
"""
import re
import statistics
import sys

LINE = re.compile(r"^c(?P<num>\d+)\s+(?P<shape>[TL])\s+(?P<payload>p\d)\s+(?P<qos>Q\d)\s+(?P<net>.+?)\s+"
                  r"(?P<fw>tickle|cyclonedds|fastdds)\s+rep(?P<rep>\d+)\s+\|\s+(?P<verdict>[^|]+?)\s+\|\s*(?P<fields>.*)$")
FWS = ("tickle", "fastdds", "cyclonedds")
PREV_C6_RATIOS = (0.184, 0.194, 0.215)   # 3ae721aa T6: 41947/194821, 38052/195914, 34714/188839 (see R3)


def roles(fields):
    """The concatenated RESULT lines split back into {role: {key: str value}} (the client's chunk comes first)."""
    out = {}
    for chunk in re.split(r"(?=role=)", fields):
        m = re.match(r"role=(\w+)", chunk)
        if not m:
            continue
        kv = out.setdefault(m.group(1), {})
        for k, v in re.findall(r"([A-Za-z_]\w*)=(\S+)", chunk):
            kv.setdefault(k, v)   # the first occurrence of a key in its role's line, as printed
    return out


def num(d, key):
    try:
        return float(d[key])
    except (KeyError, ValueError):
        return None


def parse(path):
    """{cell: {"head": str, "rows": [(rep, fw, ok, roles)]}} in file order."""
    cells = {}
    for line in open(path, encoding="utf-8"):
        m = LINE.match(line.rstrip())
        if not m:
            continue
        c = cells.setdefault(int(m["num"]), {"head": f"c{m['num']} {m['shape']} {m['payload']} {m['qos']} "
                                             f"[{m['net'].strip()}]", "rows": []})
        c["rows"].append((int(m["rep"]), m["fw"], m["verdict"].startswith("ok"), roles(m["fields"])))
    return cells


def med(xs):
    return f"{statistics.median(xs):.1f}" if xs else "-"


def read_cell(cnum, cell, out):
    rows = cell["rows"]
    out(f"\n== {cell['head']}")
    # N: counts before values.
    counts = []
    for fw in FWS:
        mine = [r for r in rows if r[1] == fw]
        if not mine:
            counts.append(f"{fw} NOT MEASURED")
        else:
            counts.append(f"{fw} {sum(r[2] for r in mine)} ok / {sum(not r[2] for r in mine)} VOID")
    out("   N  reps: " + "  ".join(counts))
    # O: realized order, per rep, as written.
    order = {}
    for rep, fw, _ok, _r in rows:
        order.setdefault(rep, []).append(fw)
    out("   O  order: " + "  ".join(f"rep{rep} {','.join(f[:3] for f in fws)}" for rep, fws in sorted(order.items())))
    first = {fw: sum(1 for fws in order.values() if fws and fws[0] == fw) for fw in FWS}
    note = ""
    if len(order) >= 3 and any(first[fw] == 0 for fw in FWS if any(r[1] == fw for r in rows)):
        note = "  ORDER NOT ROTATED"
    out("      first: " + "  ".join(f"{fw} {n}" for fw, n in first.items()) + note)
    # P: placement of the sender against eth0's IRQ core.
    if all(r[3].get("server", {}).get("win_recv_mbps") is None and r[3].get("server", {}).get("recv_mbps") is None
           for r in rows):
        out("   P  placement: no goodput field in this cell (not a throughput cell)")
    for fw in FWS:
        g0, g1, unknown = [], [], 0
        for _rep, f, ok, r in rows:
            if f != fw or not ok:
                continue
            cpu = num(r.get("client", {}), "cpu_main")
            gp = num(r.get("server", {}), "win_recv_mbps")
            if gp is None:
                gp = num(r.get("server", {}), "recv_mbps")
            if cpu is None or gp is None:
                unknown += 1
                continue
            (g0 if cpu == 0 else g1).append(gp)
        if not any(r[1] == fw for r in rows):
            continue   # already NOT MEASURED under N
        if not g0 and not g1:
            out(f"   P  {fw:<10} NOT READ (no rep with both cpu_main and a goodput figure; {unknown} without)")
            continue
        p1 = "both modes" if g0 and g1 else f"one mode only (n0 {len(g0)}, n1 {len(g1)})"
        if len(g0) >= 2 and len(g1) >= 2:
            if max(g0) < min(g1):
                p2 = "COIN FLIP SEEN"
            elif min(g0) > max(g1):
                p2 = "REVERSED"
            else:
                p2 = "NOT SEEN"
        else:
            p2 = f"NOT READ (n0 {len(g0)}, n1 {len(g1)})"
        out(f"   P  {fw:<10} client on CPU0 n0={len(g0)} goodput med {med(g0)}  elsewhere n1={len(g1)} med "
            f"{med(g1)} Mbps  -> {p1}; {p2}" + (f"  ({unknown} rep(s) without cpu_main)" if unknown else ""))
    if cnum != 6:
        return
    # R: c6's retransmitted figure (TickLE only).
    ratios, broke, unread = [], [], []
    for rep, fw, ok, r in rows:
        if fw != "tickle" or not ok:
            continue
        cl, sv = r.get("client", {}), r.get("server", {})
        sent, rtx = num(cl, "sent"), num(cl, "retransmitted")
        ga, ge = num(sv, "gap_abandoned"), num(sv, "gap_evicted")
        ratio = rtx / sent if sent and rtx is not None else None
        if ratio is not None:
            ratios.append(ratio)
        if ga is None or ge is None:
            unread.append(rep)
        elif ga or ge:
            broke.append(f"rep{rep} abandoned {ga:g} evicted {ge:g}")
        out(f"   R  tickle rep{rep}: sent {cl.get('sent', '?')} retransmitted {cl.get('retransmitted', '?')} ratio "
            f"{'-' if ratio is None else f'{ratio:.3f}'} gap_abandoned {sv.get('gap_abandoned', '?')} gap_evicted "
            f"{sv.get('gap_evicted', '?')} sample_path {cl.get('sample_path', '?')}")
    if not ratios:
        out("   R1 NOT READ: no TickLE rep with sent= and retransmitted=")
        out("   R3 NOT READ")
    else:
        m = statistics.median(ratios)
        r1 = ("CONFIRMED" if 23 <= m <= 92 else "REFUTED" if m < 4.6 or m > 460 else "NEITHER")
        out(f"   R1 46x prediction: median ratio {m:.3f} over {len(ratios)} rep(s) -> {r1}")
        lo, hi = min(PREV_C6_RATIOS), max(PREV_C6_RATIOS)
        r3 = ("CHANGED UP" if min(ratios) > hi else "CHANGED DOWN" if max(ratios) < lo else "NO CHANGE SHOWN")
        out(f"   R3 against 3ae721aa ({lo:.3f}-{hi:.3f}): now {min(ratios):.3f}-{max(ratios):.3f} -> {r3}")
    if broke:
        out(f"   R2 FALSIFIED: {'; '.join(broke)}")
    elif unread and len(unread) == sum(1 for r in rows if r[1] == "tickle" and r[2]):
        out("   R2 NOT READ: no TickLE server line carries gap_abandoned= and gap_evicted=")
    else:
        out("   R2 HOLDS: gap_abandoned = gap_evicted = 0 in every TickLE rep read"
            + (f" (not read: reps {unread})" if unread else ""))


def main(paths, out=print):
    for label, path in zip(("A (FastDDS shipped)", "T (FastDDS maxMessageSize 1472)"), paths):
        cells = parse(path)
        out(f"\n######## {label}: {path}")
        if not cells:
            out("   NO CAMPAIGN ROWS in this file - the campaign did not run, or printed nothing")
            continue
        for n in sorted(cells):
            read_cell(n, cells[n], out)


def selftest():
    """Each rule on fabricated rows, with the answer it must give."""
    import tempfile

    def row(fw, rep, cpu, gp, extra_c="", extra_s="", num_=4, verdict="ok"):
        return (f"c{num_}  T p4 Q0 loss 5%   {fw} rep{rep} | {verdict} | role=client sent=1000 cpu_main={cpu} "
                f"{extra_c} role=server recv=1000 win_recv_mbps={gp} {extra_s}\n")

    def run(text):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write(text)
        got = []
        main([f.name, f.name], got.append)
        return "\n".join(got)

    flip = "".join(row("tickle", i, 0 if i <= 2 else 2, 80 if i <= 2 else 95) for i in range(1, 6))
    mixed = "".join(row("tickle", i, 0 if i <= 2 else 2, (80, 99, 90, 95, 85)[i - 1]) for i in range(1, 6))
    c6 = lambda rtx, ga: "".join(row("tickle", i, 1, 90, f"retransmitted={rtx} sample_path=frag",  # noqa: E731
                                     f"gap_abandoned={ga} gap_evicted=0", num_=6) for i in (1, 2, 3))
    rot = "".join(row(fw, rep, 1, 90) for rep in (1, 2, 3) for fw in
                  (("tickle", "cyclonedds", "fastdds"), ("cyclonedds", "fastdds", "tickle"),
                   ("fastdds", "tickle", "cyclonedds"))[rep - 1])
    norot = "".join(row(fw, rep, 1, 90) for rep in (1, 2, 3) for fw in ("tickle", "cyclonedds", "fastdds"))
    cases = [
        ("P2 coin flip seen", lambda: "COIN FLIP SEEN" in run(flip)),
        ("P2 overlapping groups: not seen", lambda: "NOT SEEN" in run(mixed) and "COIN FLIP" not in run(mixed)),
        ("P2 one CPU0 rep: not read", lambda: "NOT READ (n0 1, n1 4)" in run(
            "".join(row("tickle", i, 0 if i == 1 else 2, 90) for i in range(1, 6)))),
        ("P1 one mode only", lambda: "one mode only (n0 0, n1 3)" in run(
            "".join(row("tickle", i, 2, 90) for i in (1, 2, 3)))),
        ("N a framework with no row is NOT MEASURED", lambda: "fastdds NOT MEASURED" in run(flip)),
        ("N a VOID rep is counted as VOID", lambda: "tickle 1 ok / 1 VOID" in run(
            row("tickle", 1, 2, 90) + row("tickle", 2, 2, 90, verdict="VOID(x)"))),
        ("O rotated order is not flagged", lambda: "ORDER NOT ROTATED" not in run(rot)),
        ("O fixed order is flagged", lambda: "ORDER NOT ROTATED" in run(norot)),
        ("R1 46x confirmed", lambda: "-> CONFIRMED" in run(c6(46000, 0))),
        ("R1 0.2x refuted, R3 no change", lambda: "-> REFUTED" in run(c6(200, 0))
         and "NO CHANGE SHOWN" in run(c6(200, 0))),
        ("R3 changed down", lambda: "CHANGED DOWN" in run(c6(100, 0))),
        ("R2 holds", lambda: "R2 HOLDS" in run(c6(200, 0))),
        ("R2 falsified", lambda: "R2 FALSIFIED" in run(c6(200, 3))),
        ("R2 absent field is not read, not 0", lambda: "R2 NOT READ" in run(
            "".join(row("tickle", i, 1, 90, "retransmitted=200", num_=6) for i in (1, 2, 3)))),
        ("R only in c6", lambda: "R1" not in run(flip)),
        ("an empty file says so", lambda: "NO CAMPAIGN ROWS" in run("nothing\n")),
    ]
    fails = 0
    for name, check in cases:
        try:
            ok = check()
        except Exception as err:  # noqa: BLE001 - a case that raises has not given its answer
            ok, name = False, f"{name} (raised {err!r})"
        fails += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name}")
    print(f"SELFTEST {'OK' if not fails else 'FAILED'} ({len(cases) - fails}/{len(cases)})")
    return 1 if fails else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(selftest())
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1:])
