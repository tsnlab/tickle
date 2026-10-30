#!/usr/bin/env python3
"""C7 round 3 (the wire experiment, config.h tt_ACKNACK_ROOM): reads sweep.sh's three-arm output (base = main 75495519,
off = this branch with -Dtt_ACKNACK_ROOM=0, head = this branch). Registered 2026-10-30 before any round-3 sweep; the
slow-cell and control rules are round 2's (sweep_read.py), plus the off arm and the falsification below.

Cells: a "control" is a fast reader (service 0) with path loss - no slow reader, which the change cannot touch; every
other cell is a slow cell. Deterministic per cell: the band is 0 for delivery and max(1 ms, 2% of base) for the slowest.
  VOID: a run failed, a cell lacks an arm, or off differs from base in any cell (resent, drops, original drops,
    delivered, mean, slowest): the flag would not give main back, and head's difference could not be read as the room's.
  Controls: head identical to base (resent, drops, delivered, mean, slowest) and room_cut 0, else WORSE.
  Slow cell: WORSE when head delivers fewer than base or its slowest is later than base + band; pass when it resends
    no more than base (fewer where base's repair waste exceeds 10%); otherwise not shown.
  Verdict: WORSE if any cell is WORSE; PASS if every slow cell passes and every control is identical; else NOT SHOWN.
  "Receiver-advertised room is a fundamental fix" is FALSIFIED (sim) when the verdict is not PASS, or when any slow cell
    without path loss keeps head's repair waste (socket drops other than the first sends', over all socket drops) above
    10%: with the room known, a repair burst fits the socket by construction, so waste left is waste the room does not
    explain.
"""
import re
import sys

R = re.compile(r"(base|off|head) R paced (\d) svc (\d+) us cap (\d+) loss (\d+) seed (\d+) hz \d+: units \d+ resent "
               r"(\d+) drops (\d+) dup \d+ acks (\d+) held \d+ delivered (\d+)/(\d+) lat mean ([\d.]+) max ([\d.]+)")
X = re.compile(r"(base|off|head) X orig_drops (\d+) path_lost \d+(?: room_cut (\d+) room_sent (\d+) adverts (\d+) zero "
               r"(\d+) sum (\d+))?")
rows, cur, failed = {}, None, []
for line in open(sys.argv[1]):
    m = R.match(line)
    if m:
        cur = dict(arm=m.group(1), key=m.groups()[1:6], resent=int(m.group(7)), drops=int(m.group(8)),
                   acks=int(m.group(9)), dl=int(m.group(10)), n=int(m.group(11)), mean=float(m.group(12)),
                   mx=float(m.group(13)))
        continue
    m = X.match(line)
    if m and cur is not None and m.group(1) == cur["arm"]:
        cur["orig"] = int(m.group(2))
        cur["cut"] = int(m.group(3) or 0)
        cur["adverts"] = int(m.group(5) or 0)
        cur["zero"] = int(m.group(6) or 0)
        rows.setdefault(cur["key"], {})[cur["arm"]] = cur
        cur = None
    elif "FAILED" in line:
        failed.append(line.strip())


def waste(r):
    return 100.0 * (r["drops"] - r["orig"]) / r["drops"] if r["drops"] else 0.0


SAME = ("resent", "drops", "orig", "dl", "mean", "mx")
print("| paced | us/dgram | socket | loss | seed | base resent | base waste | base dl | base mean/max ms "
      "| head resent | head waste | head dl | head mean/max ms | room cut | ACKNACKs base/head | cell |")
print("|" + "---|" * 16)
worse, short, waste_left, n_slow, n_ctl = [], [], [], 0, 0
for key, v in rows.items():
    if any(a not in v for a in ("base", "off", "head")):
        failed.append(f"cell {key} missing an arm")
        continue
    b, o, h = v["base"], v["off"], v["head"]
    if any(b[k] != o[k] for k in SAME):
        failed.append(f"cell {key}: off differs from base")
    control = key[1] == "0"
    if control:
        n_ctl += 1
        same = all(b[k] == h[k] for k in ("resent", "drops", "dl", "mean", "mx")) and h["cut"] == 0
        mark = "control same" if same else "CONTROL DIFFERS"
        if not same:
            worse.append(f"control {key} differs")
    else:
        n_slow += 1
        band = max(1.0, 0.02 * b["mx"])
        if h["dl"] < b["dl"]:
            mark = "WORSE delivery"
            worse.append(f"{key} delivered {h['dl']} < {b['dl']}")
        elif h["mx"] > b["mx"] + band:
            mark = "WORSE slowest"
            worse.append(f"{key} slowest {h['mx']} > {b['mx']} + {band:.1f}")
        elif h["resent"] > b["resent"] or (waste(b) > 10.0 and h["resent"] >= b["resent"]):
            mark = "not shown (resends)"
            short.append(f"{key} resent {h['resent']} vs {b['resent']}")
        else:
            mark = "pass"
        if key[3] == "0" and waste(h) > 10.0:
            waste_left.append(f"{key} head waste {waste(h):.1f}%")
    print(f"| {' | '.join(key)} | {b['resent']} | {waste(b):.1f}% | {b['dl']}/{b['n']} | {b['mean']}/{b['mx']} "
          f"| {h['resent']} | {waste(h):.1f}% | {h['dl']}/{h['n']} | {h['mean']}/{h['mx']} | {h['cut']} "
          f"| {b['acks']}/{h['acks']} | {mark} |")
print(f"=== C7 round 3 sim reading: {n_slow} slow cells, {n_ctl} controls")
for f in failed:
    print("FAILED", f)
for w in worse:
    print("WORSE  ", w)
for s in short:
    print("SHORT  ", s)
for s in waste_left:
    print("WASTE  ", s)
if failed:
    verdict = "VOID"
elif worse:
    verdict = "WORSE"
elif short:
    verdict = "NOT SHOWN"
else:
    verdict = "PASS"
print(f"VERDICT {verdict}")
if verdict != "VOID":
    print("FUNDAMENTAL FIX (sim):", "FALSIFIED" if verdict != "PASS" or waste_left else "NOT FALSIFIED")
