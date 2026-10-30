#!/usr/bin/env python3
"""C7 round 3 (the wire experiment, config.h tt_ACKNACK_ROOM) netns reading, applied to a netns_slow_ab.sh OUT directory
run with PHASE=order (the slow cell and the amp cells). P = main (75495519), F = this branch. Registered 2026-10-30
before any round-3 run; the cells, bands and slow/path-loss rules are round 2's (netns_read.py), with the window's
counters replaced by the room's ("Node N room: adverts= zero= sum= cut= sent=" in each log).

  VOID per rep: no RESULT line; load above nproc/2 at its start; a slow treatment rep (W = SLOW_W) of P with no socket
    drop (the treatment did not apply); a slow treatment rep of F whose writer never cut an answer to the room
    (room cut=0: the change never acted).
  Controls the change cannot touch - slow W = 0 (no slow reader) and amp ctl (no loss): both trees resend 0, else the
    whole run is VOID. They must also EQUAL main: F's median delivered == P's, and |F's median p99 - P's| <= max(P's rep
    range, 1 ms); otherwise the control cell is WORSE (the change touched what it cannot).
  Band per cell: max(the range of P's usable reps in this run, round 1's P range where measured - slow treatment cell:
    4.6 points of delivery, 81 ms of p99).
  Slow cell PASS: F's median delivered >= P's median - band, F's resends (summed) < P's, F's median p99 <= P's median
    + band. WORSE: F's median delivered < P's median - band. Otherwise NOT SHOWN.
  Path-loss cells (amp ge0.1b1, ge1b1: a reader that keeps up) NOT WORSE: F's median delivered >= P's - band, F's
    median p99 <= P's + band, and room cut 0 in every F rep (a reader with room must never be throttled: a cut there
    means the advertisement is wrong). Otherwise WORSE.
  Wire cost: the room rides in the ACKNACK's reserved 16 bits, so an ACKNACK's length is unchanged (0 bytes added by
    construction). What can change is how many the reader sends: per cell, the reader's tx_datagrams / the writer's
    tx_datagrams, summed over usable reps, both trees - reported, and the change in the reader's datagrams per sample
    delivered on the normal cells (ctl, slow W = 0, ge0.1b1, ge1b1) beyond 5% is listed as a wire cost.
  Verdict: PASS when the slow cell PASSes, both path-loss cells are NOT WORSE and both controls EQUAL; WORSE when any
    cell is WORSE; else NOT SHOWN.
  "Receiver-advertised room is a fundamental fix" is FALSIFIED (netns) when the slow cell is not PASS, or F's median
    delivered in the slow cell is below P's (a fix must not trade delivery for resends), or any normal cell is WORSE.
    The wire rule (a wire change must leave every test better) is reported per cell as BETTER / EQUAL / WORSE:
    slow cell BETTER on PASS with F's median delivered above P's + band or resends below half of P's; normal cells
    EQUAL when NOT WORSE / EQUAL and BETTER only when F's median p99 is below P's - band.
"""
import os
import pathlib
import re
import statistics
import sys

out = pathlib.Path(sys.argv[1])
slow_w = os.environ.get("SLOW_W", "31000")
NPROC = os.cpu_count() or 1
KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
ROUND1 = {("slow", slow_w): (0.046, 81000.0)}  # delivered fraction, p99 us


def line_fields(path, prefix):
    d = {}
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            if line.startswith(prefix):
                d.update(KV.findall(line))
    return d


def room(path):
    """Sums of the room counters over every "Node N room:" line of a log (a tree without them reads None)."""
    if not path.exists():
        return None
    found = None
    for m in re.finditer(r"room: adverts=(\d+) zero=(\d+) sum=(\d+) cut=(\d+) sent=(\d+)", path.read_text(errors="replace")):
        found = found or [0, 0, 0, 0, 0]
        found = [a + int(b) for a, b in zip(found, m.groups())]
    return found


def reps(tree, sub, arm):
    base = out / tree / sub / "runs"
    rows = []
    for run in sorted(base.glob(f"{arm}-*")) if base.exists() else []:
        e = line_fields(run / "echo.log", "RESULT")
        p = line_fields(run / "ping.log", "RESULT")
        meta = (run / "meta.txt").read_text() if (run / "meta.txt").exists() else ""
        m = re.search(r"load=([\d.]+)", meta)
        load = float(m.group(1)) if m else float("nan")
        r = dict(name=f"{tree}/{sub}/{run.name}", load=load)
        if not e or not p:
            r["void"] = "no RESULT line"
        elif load > NPROC / 2:
            r["void"] = f"load {load} > nproc/2"
        else:
            wr, rr = room(run / "ping.log"), room(run / "echo.log")
            r.update(delivered=float(e.get("delivered", "nan")), published=float(p.get("published", "nan")),
                     p99=float(e.get("latency_p99_us", "nan")), drops=float(e.get("socket_drops", "0")),
                     resent=float(p.get("retransmitted", "0")), sub_tx=float(e.get("tx_datagrams", "0")),
                     pub_tx=float(p.get("tx_datagrams", "0")), cut=wr[3] if wr else 0, sent=wr[4] if wr else 0,
                     adverts=rr[0] if rr else 0, zero=rr[1] if rr else 0, room_sum=rr[2] if rr else 0)
        rows.append(r)
    return rows


def usable(rows):
    return [r for r in rows if "void" not in r]


def med(rows, k):
    return statistics.median(r[k] for r in rows)


def band(prows, key, cell):
    rng = max(r[key] for r in prows) - min(r[key] for r in prows) if prows else 0.0
    floor = ROUND1.get(cell)
    if floor is None:
        return rng
    return max(rng, floor[0] * prows[0]["published"] if key == "delivered" else floor[1])


print("=== C7 round 3 (wire: receiver-advertised room) netns reading")
void_run = []
for sub, arm in (("slow_w0", "ctl"), ("amp", "ctl")):
    for t in ("P", "F"):
        for r in usable(reps(t, sub, arm)):
            if r["resent"] != 0:
                void_run.append(f"control {r['name']} resent {r['resent']:.0f}")
verdicts, wire, costs = {}, {}, []
cells = [("slow", f"slow_w{slow_w}", "ctl"), ("ge0.1b1", "amp", "ge0.1b1"), ("ge1b1", "amp", "ge1b1"),
         ("ctl_w0", "slow_w0", "ctl"), ("ctl_amp", "amp", "ctl")]
for cell, sub, arm in cells:
    P, F = reps("P", sub, arm), reps("F", sub, arm)
    if cell == "slow":
        for r in P:
            if "void" not in r and r["drops"] == 0:
                r["void"] = "no socket drop in P's treatment rep"
        for r in F:
            if "void" not in r and r["cut"] == 0:
                r["void"] = "the room never cut an answer"
    for r in P + F:
        if "void" in r:
            print(f"VOID  {r['name']}: {r['void']}")
    pu, fu = usable(P), usable(F)
    if not pu or not fu:
        verdicts[cell] = "VOID"
        print(f"{cell}: VOID (usable reps P {len(pu)} F {len(fu)})")
        continue
    key = ("slow", slow_w) if cell == "slow" else None
    bd, bp = band(pu, "delivered", key), band(pu, "p99", key)
    pd, fd, pp, fp = med(pu, "delivered"), med(fu, "delivered"), med(pu, "p99"), med(fu, "p99")
    prs, frs = sum(r["resent"] for r in pu), sum(r["resent"] for r in fu)
    ratio = {t: sum(r["sub_tx"] for r in rows) / max(1.0, sum(r["pub_tx"] for r in rows)) for t, rows in
             (("P", pu), ("F", fu))}
    per_dl = {t: sum(r["sub_tx"] for r in rows) / max(1.0, sum(r["delivered"] for r in rows)) for t, rows in
              (("P", pu), ("F", fu))}
    cuts = [int(r["cut"]) for r in fu]
    print(f"{cell}: reps P {len(pu)} F {len(fu)}; delivered median P {pd:.0f} F {fd:.0f} (band {bd:.1f}); p99 median "
          f"P {pp / 1000:.1f} F {fp / 1000:.1f} ms (band {bp / 1000:.1f}); resent P {prs:.0f} F {frs:.0f}; socket drops "
          f"P {sum(r['drops'] for r in pu):.0f} F {sum(r['drops'] for r in fu):.0f}; F room cut {cuts}")
    print(f"   wire: reader/writer datagrams P {100 * ratio['P']:.3f}% F {100 * ratio['F']:.3f}%; reader datagrams per "
          f"delivered sample P {per_dl['P']:.2f} F {per_dl['F']:.2f}; ACKNACK bytes added per ACKNACK 0")
    for r in pu + fu:
        mean_room = r["room_sum"] / r["adverts"] if r["adverts"] else 0.0
        print(f"   {r['name']}: delivered {r['delivered']:.0f}/{r['published']:.0f} p99 {r['p99'] / 1000:.1f} ms "
              f"resent {r['resent']:.0f} drops {r['drops']:.0f} room cut {r['cut']} sent {r['sent']} adverts "
              f"{r['adverts']} zero {r['zero']} mean room {mean_room:.0f}")
    if cell == "slow":
        if fd < pd - bd:
            verdicts[cell] = "WORSE"
        elif frs < prs and fp <= pp + bp:
            verdicts[cell] = "PASS"
        else:
            verdicts[cell] = "NOT SHOWN"
        wire[cell] = ("BETTER" if verdicts[cell] == "PASS" and (fd > pd + bd or frs < 0.5 * prs) else
                      "WORSE" if verdicts[cell] == "WORSE" or fd < pd - bd else "EQUAL")
    elif cell.startswith("ctl"):
        bpc = max(bp, 1000.0)
        ok = fd == pd and abs(fp - pp) <= bpc
        verdicts[cell] = "EQUAL" if ok else "WORSE"
        wire[cell] = "EQUAL" if ok else "WORSE"
    else:
        ok = fd >= pd - bd and fp <= pp + bp and all(c == 0 for c in cuts)
        verdicts[cell] = "NOT WORSE" if ok else "WORSE"
        wire[cell] = "WORSE" if not ok else "BETTER" if fp < pp - bp else "EQUAL"
    if cell != "slow" and per_dl["P"] > 0 and abs(per_dl["F"] / per_dl["P"] - 1.0) > 0.05:
        costs.append(f"{cell}: reader datagrams per delivered sample P {per_dl['P']:.2f} F {per_dl['F']:.2f}")
    print(f"   -> {verdicts[cell]} (wire rule: {wire[cell]})")
for c in costs:
    print("WIRE COST", c)
if void_run:
    for v in void_run:
        print("VOID ", v)
    print("VERDICT VOID")
elif any(v == "WORSE" for v in verdicts.values()):
    print("VERDICT WORSE")
elif verdicts.get("slow") == "PASS" and all(verdicts.get(c) == "NOT WORSE" for c in ("ge0.1b1", "ge1b1")) and all(
        verdicts.get(c) == "EQUAL" for c in ("ctl_w0", "ctl_amp")):
    print("VERDICT PASS")
else:
    print("VERDICT NOT SHOWN", verdicts)
falsified = (verdicts.get("slow") != "PASS" or any(verdicts.get(c) == "WORSE" for c, _, _ in cells[1:]))
if not void_run and "slow" in verdicts and verdicts["slow"] != "VOID":
    pu, fu = usable(reps("P", f"slow_w{slow_w}", "ctl")), usable(reps("F", f"slow_w{slow_w}", "ctl"))
    if pu and fu and med(fu, "delivered") < med(pu, "delivered"):
        falsified = True
print("FUNDAMENTAL FIX (netns):", "FALSIFIED" if falsified else "NOT FALSIFIED")
print("WIRE RULE per cell:", ", ".join(f"{c} {w}" for c, w in wire.items()),
      "->", "every cell better or equal" if wire and all(w != "WORSE" for w in wire.values()) else "some cell WORSE")
