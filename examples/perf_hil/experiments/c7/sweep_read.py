#!/usr/bin/env python3
"""Reads sweep.sh's output: one row per cell, base against head - resends, the share of the socket's drops that were
not a publish's own (repair waste), samples delivered, mean and slowest publish-to-delivery, and head's window - then
applies the round-2 rules registered in the C7 design (section 7) and prints a verdict.

Cells: a "control" is a fast reader (service 0) with path loss; every other cell is a slow cell. The sim is
deterministic per cell, so the band is 0 for delivery and max(1 ms, 2% of base) for the slowest sample.
PASS: every slow cell delivers no fewer than base, resends no more (and fewer where base's waste exceeds 10%), slowest
no later than base + band; every control identical. WORSE: a slow cell delivering fewer, or slower beyond the band,
or a control not identical. Otherwise NOT SHOWN."""
import re
import sys

R = re.compile(r"(base|head) R paced (\d) svc (\d+) us cap (\d+) loss (\d+) seed (\d+) hz \d+: units \d+ resent (\d+) "
               r"drops (\d+) dup \d+ acks \d+ held \d+ delivered (\d+)/(\d+) lat mean ([\d.]+) max ([\d.]+)")
X = re.compile(r"(base|head) X orig_drops (\d+) path_lost \d+(?: window_held (\d+) W (\d+))?")
rows, cur, failed = {}, None, []
for line in open(sys.argv[1]):
    m = R.match(line)
    if m:
        cur = dict(arm=m.group(1), key=m.groups()[1:6], resent=int(m.group(7)), drops=int(m.group(8)),
                   dl=int(m.group(9)), n=int(m.group(10)), mean=float(m.group(11)), mx=float(m.group(12)))
        continue
    m = X.match(line)
    if m and cur is not None:
        cur["orig"] = int(m.group(2))
        cur["held"] = int(m.group(3) or 0)
        cur["W"] = int(m.group(4) or 0)
        rows.setdefault(cur["key"], {})[cur["arm"]] = cur
    elif "FAILED" in line:
        failed.append(line.strip())


def waste(r):
    return 100.0 * (r["drops"] - r["orig"]) / r["drops"] if r["drops"] else 0.0


print("| paced | us/dgram | socket | loss | seed | base resent | base waste | base dl | base mean/max ms "
      "| head resent | head waste | head dl | head mean/max ms | window_held | W | cell |")
print("|" + "---|" * 16)
worse, short, n_slow, n_ctl = [], [], 0, 0
for key, v in rows.items():
    if "base" not in v or "head" not in v:
        failed.append(f"cell {key} missing an arm")
        continue
    b, h = v["base"], v["head"]
    control = key[1] == "0"
    if control:
        n_ctl += 1
        same = all(b[k] == h[k] for k in ("resent", "drops", "dl", "mean", "mx"))
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
    print(f"| {' | '.join(key)} | {b['resent']} | {waste(b):.1f}% | {b['dl']}/{b['n']} | {b['mean']}/{b['mx']} "
          f"| {h['resent']} | {waste(h):.1f}% | {h['dl']}/{h['n']} | {h['mean']}/{h['mx']} | {h['held']} | {h['W']} "
          f"| {mark} |")
print(f"=== C7 sim reading: {n_slow} slow cells, {n_ctl} controls")
for f in failed:
    print("FAILED", f)
for w in worse:
    print("WORSE  ", w)
for s in short:
    print("SHORT  ", s)
if failed:
    print("VERDICT VOID (a run failed or a cell is missing)")
elif worse:
    print("VERDICT WORSE")
elif short:
    print("VERDICT NOT SHOWN")
else:
    print("VERDICT PASS")
