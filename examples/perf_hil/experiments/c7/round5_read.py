#!/usr/bin/env python3
"""C7 round 5 (variant 4, an echoed grant: config.h tt_ACKNACK_ROOM_GRANT 2) - the reading added to the unchanged
sweep_read_r3.py and netns_read_r3.py, registered 2026-10-31 before any round-5 run. Usage:
    round5_read.py SIM_READ_TXT NETNS_OUT_DIR
SIM_READ_TXT is sweep_read_r3.py's output, NETNS_OUT_DIR a netns_slow_ab.sh OUT whose summary.txt ends with
netns_read_r3.py's reading. Against main 3178006c (P / base).

  VOID: either reading VOID, or a file missing. PASS: both readings PASS and the round-4 cause is shown gone (below).
    WORSE: either reading WORSE. Otherwise NOT SHOWN.
  Round 4's netns VOID (F resent 45 / 3 units in two no-loss control reps) came from variant 3's reader trimming its
    in-flight requests on a guess (a small room) while the writer had cut nothing (room cut 0): the trimmed units,
    still on their way, were asked again and resent as duplicates. Variant 4 trims only on a writer's GRANT, which a
    writer sends only when it cut. SHOWN GONE requires, in every usable F control rep (slow W = 0, amp ctl): the
    writer's room cut 0 and GRANTs sent 0, the reader's GRANT trims 0, resent 0 and large_duplicate 0. A control rep
    with any of them non-zero is a FAIL of this check (and the whole round is not PASS).
  "A coherent grant is a fundamental fix" is FALSIFIED when: the sim reading is not PASS, or it prints FUNDAMENTAL
    FIX (sim): FALSIFIED (a lossless slow cell keeps repair waste above 10%); or the netns reading is not PASS (VOID
    included), or prints FALSIFIED; or the round-4 cause is not shown gone.
"""
import pathlib
import re
import sys

sim_txt = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
summary = out / "summary.txt"
problems = []


def verdict(text, tag):
    m = re.findall(r"^VERDICT (\w+(?: \w+)?)", text, re.M)
    return m[-1] if m else None


sim = sim_txt.read_text() if sim_txt.exists() else ""
net = summary.read_text() if summary.exists() else ""
sim_v = verdict(sim, "sim") if sim else None
net_v = verdict(net, "netns") if net else None
sim_falsified = "FUNDAMENTAL FIX (sim): FALSIFIED" in sim
net_falsified = "FUNDAMENTAL FIX (netns): FALSIFIED" in net
print("=== C7 round 5 reading")
print(f"sim verdict: {sim_v}{' (fundamental: FALSIFIED)' if sim_falsified else ''}")
print(f"netns verdict: {net_v}{' (fundamental: FALSIFIED)' if net_falsified else ''}")


def log_counters(path, label):
    text = path.read_text(errors="replace") if path.exists() else ""
    total = {}
    for m in re.finditer(label + r": ((?:[a-z_]+=\d+ ?)+)", text):
        for k, v in re.findall(r"([a-z_]+)=(\d+)", m.group(1)):
            total[k] = total.get(k, 0) + int(v)
    return total


cause = []
reps = 0
for sub, arm in (("slow_w0", "ctl"), ("amp", "ctl")):
    base = out / "F" / sub / "runs"
    for run in sorted(base.glob(f"{arm}-*")) if base.exists() else []:
        echo, ping = run / "echo.log", run / "ping.log"
        e = " ".join(l for l in echo.read_text(errors="replace").splitlines() if l.startswith("RESULT")) \
            if echo.exists() else ""
        p = " ".join(l for l in ping.read_text(errors="replace").splitlines() if l.startswith("RESULT")) \
            if ping.exists() else ""
        if not e or not p:
            continue  # VOID by netns_read_r3.py's rule; not counted here
        reps += 1
        room_w = log_counters(ping, "room")
        grant_w = log_counters(ping, "grant")
        grant_r = log_counters(echo, "grant")
        resent = int(re.search(r"retransmitted=(\d+)", p).group(1))
        dup = int(re.search(r"large_duplicate=(\d+)", e).group(1))
        vals = dict(cut=room_w.get("cut", -1), grants_sent=grant_w.get("sent", -1), trims=grant_r.get("trims", -1),
                    resent=resent, duplicate=dup)
        bad = {k: v for k, v in vals.items() if v != 0}
        print(f"   control F/{sub}/{run.name}: {vals}{'  <- FAIL' if bad else ''}")
        if bad:
            cause.append(f"F/{sub}/{run.name} {bad}")
if reps == 0:
    problems.append("no usable F control rep")
shown_gone = reps > 0 and not cause
print(f"round-4 cause shown gone: {'YES' if shown_gone else 'NO'} ({reps} control reps)")
for c in cause:
    print("   ", c)
if sim_v is None or net_v is None:
    v = "VOID (a reading is missing)"
elif "VOID" in (sim_v, net_v):
    v = "VOID"
elif "WORSE" in (sim_v, net_v):
    v = "WORSE"
elif sim_v == "PASS" and net_v == "PASS" and shown_gone:
    v = "PASS"
else:
    v = "NOT SHOWN"
print(f"ROUND 5 VERDICT {v}")
falsified = (sim_v != "PASS" or sim_falsified or net_v != "PASS" or net_falsified or not shown_gone)
print("COHERENT GRANT AS A FUNDAMENTAL FIX:", "FALSIFIED" if falsified else "NOT FALSIFIED")
