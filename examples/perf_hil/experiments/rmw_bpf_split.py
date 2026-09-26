#!/usr/bin/env python3
"""Summarises pong_rx_split.bt output per rmw (2026-09-26, RMW_PERF_PLAN.md section 8.2).

Usage: rmw_bpf_split.py <OUT file of rmw_crosshost_rtt.sh BPF_ARMS="... on">   (reads <OUT>.bpf/<stem>.txt)

Per (message, qos, wait mode, rmw), over every ping line of every repetition: the median time, in us after NAPI
handed the frame to the stack, of each point on the pong's receive path (irq2napi is the time before it), and how
often the receiving thread and the executor were woken within the burst. A point a given rmw never reaches (a wait
call, for a thread blocked in recv; an executor wake, when the receiver is already the executor) is n/a.
"""
import collections
import re
import statistics
import sys
from pathlib import Path

POINTS = ("irq2napi", "enq", "rd", "wk_rx", "sw_rx", "wait", "rx", "wk_ex", "sw_ex", "send")


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
        print("   median us after NAPI: " + ", ".join(parts))
        n = len(rows)
        woke_rx = sum(int(r["wk_rx"]) > 0 for r in rows)
        woke_ex = sum(int(r["wk_ex"]) > 0 for r in rows)
        print(f"   receiving thread woken in the burst {woke_rx}/{n}; executor (main thread) woken {woke_ex}/{n}")


if __name__ == "__main__":
    main()
