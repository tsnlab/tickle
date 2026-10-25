#!/usr/bin/env python3
"""storm_tv_extra.py - per-tree aggregates of a storm_tv_ab.sh run, beside its verdicts (not a verdict itself):
writer resends (median, max, sum), timeout releases (median, max), RACK releases, held requests, the reader's
duplicates and timer ACKNACKs, the writer egress's netem drops (loss model plus overflow), and samples delivered.
Usage: storm_tv_extra.py BASE_OUT TAG LABEL...
"""
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")


def kv(line):
    return dict(KV.findall(line))


def lines(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []


base, tag, labels = sys.argv[1], sys.argv[2], sys.argv[3:]
for lab in labels:
    runs = os.path.join(base, f"{tag}_{lab}", "runs")
    by_arm = {}
    for name in sorted(os.listdir(runs)):
        arm, cell, rep = name.rsplit("-", 2)
        d = os.path.join(runs, name)
        ping, echo = lines(os.path.join(d, "ping.log")), lines(os.path.join(d, "echo.log"))
        r = {}
        for x in ping:
            if x.startswith("RESULT"):
                r["resends"] = int(kv(x).get("retransmitted", 0))
            if x.startswith("RSTATS: role=pub lro "):
                k = kv(x)
                r["timeout"] = int(k.get("released_timeout", 0))
                r["rack"] = int(k.get("rack", 0))
            if "resend_held=" in x and "large:" in x:
                r["held"] = int(kv(x).get("resend_held", 0))
        for x in echo:
            if x.startswith("RESULT"):
                k = kv(x)
                r["delivered"] = int(k.get("delivered", 0))
                r["dup"] = int(k.get("large_duplicate", 0))
            if x.startswith("RSTATS: role=sub acknack_tx"):
                r["timer"] = int(kv(x).get("timer", 0))
        c = lines(os.path.join(d, "counters.txt"))
        drops = [int(m.group(1)) for m in (re.search(r"dropped (\d+)", x) for x in c) if m]
        r["tc"] = drops[0] if drops else -1  # the first qdisc line: the writer's prio root
        by_arm.setdefault(arm, []).append(r)
    for arm, rows in sorted(by_arm.items()):
        def col(k):
            return [x.get(k, 0) for x in rows]
        def st(k):
            v = col(k)
            return f"{statistics.median(v):.0f}/{max(v)}"
        print(f"{lab:3} {arm:7} reps {len(rows):2} resends med/max {st('resends')} sum {sum(col('resends'))} "
              f"timeout_rel {st('timeout')} rack {st('rack')} held {st('held')} dup {st('dup')} "
              f"timer_acknacks {st('timer')} tc_drops {st('tc')} delivered sum {sum(col('delivered'))}")
