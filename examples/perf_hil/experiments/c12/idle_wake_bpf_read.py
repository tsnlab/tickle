#!/usr/bin/env python3
"""C12 idle wake, step 5: where an idle pong's receive spends the time an injected wake takes away (written 2026-10-31,
before its run). Reads pong_rx_ring_split.bt lines collected by rmw_crosshost_rtt.sh BPF_ARMS=on BPF_SCRIPT=<that>
INJECT_ARMS="none 50" into <OUT>.bpf/<stem>.txt.

Per burst (one ping, from NAPI's entry to the reply's send), consecutive segments in us:
  irq2napi            latest eth0 hard IRQ -> NAPI entry
  napi->enq           the stack up to UDP queueing the datagram
  enq->rd             the socket's data_ready
  rd->wk              data_ready -> the receiving thread is woken (io_uring's poll wake and task_work queueing on TickLE;
                      the socket's wait queue on CycloneDDS)
  wk->sw              woken -> switched in: the scheduler and, when the cpu was idle, its idle exit
  sw->rx              switched in -> the receive returns to user space (TickLE: the deferred task_work runs the recvmsg in
                      the waiter, posts the completion, io_uring_enter returns; CycloneDDS: recvmsg copies and returns)
  total               NAPI entry -> receive return
Only bursts whose first woken pong thread is the one that received are used. And per row the share of bursts whose woken cpu left an idle state during the burst, which state, and how long it had
been in it.

READING (diagnostic, decides nothing on its own; pre-registered so it cannot be fitted to the numbers):
  delta(seg) = median over none bursts - median over lead-50 bursts, per rmw.
  The segment(s) that carry >= 50% of TickLE's delta(total), while CycloneDDS's delta of the same segment is < 0.5 us,
  are where the idle cost sits. If the idle-exit share or depth differs between TickLE's arms and not CycloneDDS's,
  the cost is the cpu's idle state, not TickLE's code. Fewer than 100 bursts in an arm: NOT READ.

Usage: idle_wake_bpf_read.py <OUT file>
"""
import collections
import re
import statistics
import sys
from pathlib import Path

SEGS = ("irq2napi", "napi->enq", "enq->rd", "rd->wk", "wk->sw", "sw->rx", "total")
MIN_BURSTS = 100


def segs(r):
    g = {k: int(v) for k, v in r.items()}
    if min(g["enq"], g["rd"], g["wk"], g["sw"], g["rx"]) <= 0 or g["wtid"] != g["rxtid"]:
        return None
    return {"irq2napi": g["irq2napi"], "napi->enq": g["enq"], "enq->rd": g["rd"] - g["enq"], "rd->wk": g["wk"] - g["rd"],
            "wk->sw": g["sw"] - g["wk"], "sw->rx": g["rx"] - g["sw"], "total": g["rx"],
            "_idle": (g["ie"] > 0, g["istate"], g["idle_us"]), "_khz": g.get("khz", 0)}


def main():
    out = Path(sys.argv[1])
    groups = collections.defaultdict(list)
    for path in sorted(out.with_suffix(out.suffix + ".bpf").glob("*.txt")):
        m = re.match(r"(rmw_\S+?)_bench_best_effort_rep(\d+)_block.*_bpf_inj(\S+)$", path.stem)
        if not m:
            continue
        for line in path.read_text().splitlines():
            if line.startswith("S "):
                s = segs(dict(kv.split("=") for kv in line[2:].split()))
                if s:
                    groups[(m.group(1), m.group(3))].append(s)
    med = {}
    for key in sorted(groups):
        rows = groups[key]
        med[key] = {k: statistics.median(r[k] for r in rows) / 1000 for k in SEGS}
        idle = [r["_idle"] for r in rows]
        exits = [i for i in idle if i[0]]
        states = collections.Counter(i[1] for i in exits)
        idle_us = statistics.median(i[2] for i in exits) if exits else float("nan")
        note = "" if len(rows) >= MIN_BURSTS else f"  NOT READ (< {MIN_BURSTS} bursts)"
        print(f"== {key[0]} inject={key[1]}: {len(rows)} bursts{note}")
        print("   median us: " + ", ".join(f"{k} {med[key][k]:.2f}" for k in SEGS))
        print(f"   woken cpu left idle in the burst: {len(exits)}/{len(rows)}, states {dict(states)}, "
              f"time in it p50 {idle_us:.0f} us; woken cpu khz {dict(collections.Counter(r['_khz'] for r in rows))}")
    rmws = sorted({k[0] for k in med})
    deltas = {}
    for rmw in rmws:
        if (rmw, "none") in med and (rmw, "50") in med:
            deltas[rmw] = {k: med[(rmw, "none")][k] - med[(rmw, "50")][k] for k in SEGS}
            print(f"-- {rmw} none - 50: " + ", ".join(f"{k} {deltas[rmw][k]:+.2f}" for k in SEGS))
    t = next((r for r in deltas if r.startswith("rmw_tickle")), None)
    c = deltas.get("rmw_cyclonedds_cpp")
    if t and c:
        tot = deltas[t]["total"]
        carry = [k for k in SEGS[:-1] if tot > 0 and deltas[t][k] >= 0.5 * tot and abs(c[k]) < 0.5]
        print(f"WHERE: TickLE delta total {tot:+.2f} us; segment(s) carrying >= 50% with CycloneDDS < 0.5 us: "
              f"{', '.join(carry) if carry else 'none single'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
