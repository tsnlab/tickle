#!/usr/bin/env python3
"""storm_tail_read.py - the ETHERNET tail check of the tv storm fix, read against its A/A (RESULTS `TVS`).

Reads large_repair_followups_netns.sh PHASE=tail's summary.txt (trees O = main's second checkout, P = main, F = the
fix) and applies the reading registered in storm_tv_ab.sh's second amendment, before the run it reads:
  BAND = max(N, |O - P|), N the harness's ctl band (|F - P| median rep p99 on ctl), |O - P| the A/A's own distance on
  the same arm; FALSIFIED on a STORM rep of F or fewer samples delivered by F than by P at ge0.1b1 or ge1b1; per arm
  COST when F's median rep p99 exceeds P's by more than BAND, GAIN when it is below by more than BAND, else NO COST.
Usage: storm_tail_read.py SUMMARY_TXT
"""
import re
import sys

rows = {}
n_band = None
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = re.match(r"^([OPF]) +(ctl|ge0\.1b1|ge1b1) +reps (\d+) storms (\d+) .* delivered (\d+)/(\d+) median p99 ([0-9.]+)",
                 line)
    if m:
        rows[(m.group(1), m.group(2))] = {"reps": int(m.group(3)), "storms": int(m.group(4)),
                                          "delivered": int(m.group(5)), "offered": int(m.group(6)),
                                          "p99": float(m.group(7))}
    m = re.match(r"^control noise band N = .* = ([0-9.]+) ms", line)
    if m:
        n_band = float(m.group(1))
if n_band is None:
    print("VOID: no control band in the summary")
    sys.exit(1)
for arm in ("ge0.1b1", "ge1b1"):
    o, p, f = rows.get(("O", arm)), rows.get(("P", arm)), rows.get(("F", arm))
    if not (o and p and f):
        print(f"VOID {arm}: a tree has no usable reps")
        continue
    band = max(n_band, abs(o["p99"] - p["p99"]))
    d = f["p99"] - p["p99"]
    if f["storms"] > 0 or f["delivered"] / f["offered"] < p["delivered"] / p["offered"]:
        verdict = "FALSIFIED"
    elif d > band:
        verdict = "COST"
    elif d < -band:
        verdict = "GAIN"
    else:
        verdict = "NO COST"
    print(f"TAIL {arm}: median rep p99 O {o['p99']} P {p['p99']} F {f['p99']} ms (reps {o['reps']}/{p['reps']}/"
          f"{f['reps']}); F - P {d:+.1f} against BAND {band:.2f} (N {n_band}, |O - P| {abs(o['p99'] - p['p99']):.1f}): "
          f"{verdict}")
