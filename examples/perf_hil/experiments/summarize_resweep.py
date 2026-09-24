#!/usr/bin/env python3
"""Summarize a comparison_resweep.sh output file: per (cell, framework), each key RESULT field as
min / mean / max over reps, with client and server fields prefixed c./s. so they cannot collide."""
import re
import statistics
import sys
from collections import OrderedDict, defaultdict

KEEP = {"loss_pct", "rtt_avg_ms", "sent", "recv", "lost", "send_mbps", "recv_mbps", "write_fail", "received",
        "backlog_delivery_ms", "writer_misses", "reader_misses", "offered_missed_total", "requested_missed_total",
        "detect_latency_ms", "drained"}
cells = OrderedDict()
for line in open(sys.argv[1], encoding="utf-8"):
    m = re.match(r"^([ABC]) (\S+)(?: (\S+))? rep(\d+) (tickle|cyclonedds|fastdds) \| (.*)$", line.rstrip())
    if not m:
        m2 = re.match(r"^([ABC]) (\S+) (\S+) rep(\d+) (\S+) \| (.*)$", line.rstrip())
        if not m2:
            continue
        m = m2
    part, label, arm, _rep, fw, rest = m.groups()
    key = (part, label + ("/" + arm if arm and not arm.startswith("rep") else ""), fw)
    d = cells.setdefault(key, defaultdict(list))
    if "CONTAMINATED" in rest or "NO RESULT LINE" in rest:
        d["_bad"].append(1)
        continue
    for seg in rest.split("RESULT:")[1:]:
        role = "s" if "role=server" in seg else "c"
        for k, v in re.findall(r"(\w+)=(\S+)", seg):
            if k in KEEP:
                try:
                    d[f"{role}.{k}"].append(float(v))
                except ValueError:
                    d[f"{role}.{k}"].append(v)
for (part, label, fw), d in cells.items():
    out = []
    for k, vals in d.items():
        if k == "_bad":
            out.append(f"BAD={len(vals)}")
        elif all(isinstance(v, float) for v in vals):
            lo, hi, mean = min(vals), max(vals), statistics.mean(vals)
            fmt = (lambda x: f"{x:.3g}") if max(abs(lo), abs(hi)) < 1000 else (lambda x: f"{x:.0f}")
            out.append(f"{k}={fmt(mean)}" + (f"[{fmt(lo)}..{fmt(hi)}]" if len(vals) > 1 and lo != hi else ""))
        else:
            out.append(f"{k}={'/'.join(sorted(set(map(str, vals))))}")
    print(f"{part} {label:<24} {fw:<10} n={max((len(v) for v in d.values()), default=0)} " + " ".join(out))
