#!/usr/bin/env python3
"""Summarises pong_rx_split.bt output per rmw (2026-09-26, RMW_PERF_PLAN.md section 8.2).

Usage: rmw_bpf_split.py <OUT file of rmw_crosshost_rtt.sh BPF_ARMS="... on">   (reads <OUT>.bpf/<stem>.txt)

Per (message, qos, wait mode, rmw), over every ping line of every repetition: the median time, in us after the
NIC's hard IRQ, of each point on the pong's receive path, and how the threads relate. For each ping:
- is the first thread woken the one whose receive returned (the receiver woken directly)?
- is the thread woken after the receive the one that sends the reply (a handoff to an executor)?
- does the receiving thread send the reply itself (no handoff)?
A point a given rmw never reaches (a wait call, for a thread blocked in recv) is reported as n/a.
"""
import collections
import re
import statistics
import sys
from pathlib import Path

POINTS = ("napi", "enq", "rd", "wk", "sw", "wait", "rx", "wk2", "sw2", "send")


def main():
    out = Path(sys.argv[1])
    bpf_dir = out.with_suffix(out.suffix + ".bpf")
    groups = collections.defaultdict(list)
    for path in sorted(bpf_dir.glob("*.txt")):
        m = re.match(r"(rmw_\w+?)_(bench|array1k)_(best_effort|reliable)_rep\d+(?:_(poll|block))?.*_bpf$", path.stem)
        if not m:
            continue
        rmw, msg, qos, mode = m.groups()
        for line in path.read_text().splitlines():
            if line.startswith("S "):
                groups[(msg, qos, mode or "poll", rmw)].append(dict(kv.split("=") for kv in line[2:].split()))
    for key in sorted(groups):
        rows = groups[key]
        print(f"== {' '.join(key)}: {len(rows)} pings")
        parts = []
        for p in POINTS:
            xs = [int(r[p]) for r in rows if int(r[p]) > 0]
            parts.append(f"{p} {statistics.median(xs) / 1000:.1f}" if xs else f"{p} n/a")
        print("   median us after IRQ: " + ", ".join(parts))
        n = len(rows)
        direct = sum(r["wktid"] == r["rxtid"] for r in rows)
        handoff = sum(r["wk2tid"] != "0" and r["wk2tid"] == r["sendtid"] for r in rows)
        self_send = sum(r["rxtid"] == r["sendtid"] for r in rows)
        print(f"   receiver woken first {direct}/{n}; handoff to the sending thread {handoff}/{n}; "
              f"receiver sends the reply itself {self_send}/{n}")


if __name__ == "__main__":
    main()
