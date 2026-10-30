#!/usr/bin/env python3
"""C7 round 2's netns reading (design section 7), applied to a netns_slow_ab.sh OUT directory (PHASE=order: the slow
cell and the amp path-loss cells). Registered before the run:

  VOID per rep: no RESULT line; load above nproc/2 at its start; a slow treatment rep (W = SLOW_W) of P with no socket
    drop (the treatment did not apply); a slow treatment rep of F whose writer's window_held is 0 (the window never
    engaged). The controls the change cannot touch - slow W = 0 and amp ctl - must resend 0 on both trees, else the
    whole run is VOID.
  Band per cell: max(the range of P's usable reps in this run, round 1's P range where measured - slow treatment cell:
    4.6 points of delivery, 81 ms of p99).
  Slow cell PASS: F's median delivered >= P's median - band, F's resends (summed) < P's, F's median p99 <= P's median
    + band. WORSE: F's median delivered < P's median - band. Otherwise NOT SHOWN.
  Path-loss cells (amp ge0.1b1, ge1b1) NOT WORSE: F's median delivered >= P's - band, F's median p99 <= P's + band,
    and window_held 0 in every F rep. Otherwise WORSE.
  Verdict: PASS when the slow cell PASSes and both path-loss cells are NOT WORSE; WORSE when any cell is WORSE; else
    NOT SHOWN.
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


def window_held(path):
    if not path.exists():
        return 0
    m = re.findall(r"window_held=(\d+)", path.read_text(errors="replace"))
    return sum(int(x) for x in m)


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
            r.update(delivered=float(e.get("delivered", "nan")), published=float(p.get("published", "nan")),
                     p99=float(e.get("latency_p99_us", "nan")), drops=float(e.get("socket_drops", "0")),
                     resent=float(p.get("retransmitted", "0")), held=window_held(run / "ping.log"))
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


print("=== C7 round 2 netns reading")
void_run = []
for sub, arm in (("slow_w0", "ctl"), ("amp", "ctl")):
    for t in ("P", "F"):
        for r in usable(reps(t, sub, arm)):
            if r["resent"] != 0:
                void_run.append(f"control {r['name']} resent {r['resent']:.0f}")
verdicts = {}
cells = [("slow", f"slow_w{slow_w}", "ctl"), ("ge0.1b1", "amp", "ge0.1b1"), ("ge1b1", "amp", "ge1b1")]
for cell, sub, arm in cells:
    P, F = reps("P", sub, arm), reps("F", sub, arm)
    if cell == "slow":
        for r in P:
            if "void" not in r and r["drops"] == 0:
                r["void"] = "no socket drop in P's treatment rep"
        for r in F:
            if "void" not in r and r["held"] == 0:
                r["void"] = "the window never engaged"
    for r in P + F:
        if "void" in r:
            print(f"VOID  {r['name']}: {r['void']}")
    pu, fu = usable(P), usable(F)
    if not pu or not fu:
        verdicts[cell] = "VOID"
        print(f"{cell}: VOID (usable reps P {len(pu)} F {len(fu)})")
        continue
    bd, bp = band(pu, "delivered", ("slow", slow_w) if cell == "slow" else None), band(
        pu, "p99", ("slow", slow_w) if cell == "slow" else None)
    pd, fd, pp, fp = med(pu, "delivered"), med(fu, "delivered"), med(pu, "p99"), med(fu, "p99")
    prs, frs = sum(r["resent"] for r in pu), sum(r["resent"] for r in fu)
    held = [int(r["held"]) for r in fu]
    print(f"{cell}: reps P {len(pu)} F {len(fu)}; delivered median P {pd:.0f} F {fd:.0f} (band {bd:.1f}); p99 median "
          f"P {pp / 1000:.1f} F {fp / 1000:.1f} ms (band {bp / 1000:.1f}); resent P {prs:.0f} F {frs:.0f}; socket drops "
          f"P {sum(r['drops'] for r in pu):.0f} F {sum(r['drops'] for r in fu):.0f}; F window_held {held}")
    for r in pu + fu:
        print(f"   {r['name']}: delivered {r['delivered']:.0f}/{r['published']:.0f} p99 {r['p99'] / 1000:.1f} ms "
              f"resent {r['resent']:.0f} drops {r['drops']:.0f} window_held {r['held']}")
    if cell == "slow":
        if fd < pd - bd:
            verdicts[cell] = "WORSE"
        elif frs < prs and fp <= pp + bp:
            verdicts[cell] = "PASS"
        else:
            verdicts[cell] = "NOT SHOWN"
    else:
        ok = fd >= pd - bd and fp <= pp + bp and all(h == 0 for h in held)
        verdicts[cell] = "NOT WORSE" if ok else "WORSE"
    print(f"   -> {verdicts[cell]}")
if void_run:
    for v in void_run:
        print("VOID ", v)
    print("VERDICT VOID")
elif any(v == "WORSE" for v in verdicts.values()):
    print("VERDICT WORSE")
elif verdicts.get("slow") == "PASS" and all(verdicts.get(c) == "NOT WORSE" for c in ("ge0.1b1", "ge1b1")):
    print("VERDICT PASS")
else:
    print("VERDICT NOT SHOWN", verdicts)
