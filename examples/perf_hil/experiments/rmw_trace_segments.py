#!/usr/bin/env python3
"""Per-segment latency of a ping-pong responder, from an RMW_TICKLE_TRACE_FILE dump (rmw_init.c).

rmw_tickle/RMW_PERF_PLAN.md H2. The dump holds stamps from a build with -DRMW_TICKLE_TRACE=ON, oldest first:
    stamp <ns> <thread> <point>
with points 1=rx_wake 2=rx_datagram 3=deliver 4=signaled 5=exec_wake 6=taken 7=publish 8=tx_done.

Each deliver (3) anchors one message. Walking back from it: the last rx_datagram (2) and the last rx_wake
(1) before it on the same thread. Walking forward: the first signaled (4) on that thread, then the first
exec_wake (5), taken (6), publish (7) and tx_done (8) after it on any thread, each after the one before.
A message missing any of them is skipped and counted. Segments are reported as median, p90 and max, in
microseconds, over the messages that had all eight - with the lock counters line printed as it is.

Usage: rmw_trace_segments.py <dump file>
"""

import statistics
import sys

NAMES = {1: "rx_wake", 2: "rx_datagram", 3: "deliver", 4: "signaled", 5: "exec_wake", 6: "taken", 7: "publish",
         8: "tx_done"}


def main(path):
    stamps = []
    with open(path) as dump:
        for line in dump:
            fields = line.split()
            if fields and fields[0] == "lock":
                print(line.rstrip())
            elif fields and fields[0] == "stamp":
                stamps.append((int(fields[1]), int(fields[2]), int(fields[3])))
    segments = {k: [] for k in range(1, 8)}
    skipped = 0
    for i, (ns, thread, point) in enumerate(stamps):
        if point != 3:
            continue
        chain = {3: ns}
        for back in range(i - 1, -1, -1):
            b_ns, b_thread, b_point = stamps[back]
            if b_thread != thread:
                continue
            if b_point == 2 and 2 not in chain:
                chain[2] = b_ns
            elif b_point == 1 and 2 in chain:
                chain[1] = b_ns
                break
            elif b_point == 3:
                break  # the previous message: its wake is not this one's
        want = 4
        for fwd in range(i + 1, len(stamps)):
            f_ns, f_thread, f_point = stamps[fwd]
            if f_point != want:
                continue
            if want == 4 and f_thread != thread:
                continue
            chain[want] = f_ns
            want += 1
            if want > 8:
                break
        if len(chain) != 8:
            skipped += 1
            continue
        for k in range(1, 8):
            segments[k].append((chain[k + 1] - chain[k]) / 1000.0)
    complete = len(segments[1])
    print(f"messages: {complete} complete, {skipped} skipped")
    if complete == 0:
        return 1
    total = []
    for k in range(1, 8):
        values = sorted(segments[k])
        p90 = values[min(len(values) - 1, int(len(values) * 0.9))]
        print(f"{NAMES[k]:>11} -> {NAMES[k + 1]:<11} median {statistics.median(values):8.1f} us  "
              f"p90 {p90:8.1f}  max {values[-1]:8.1f}")
    for m in range(complete):
        total.append(sum(segments[k][m] for k in range(1, 8)))
    print(f"{'rx_wake':>11} -> {'tx_done':<11} median {statistics.median(total):8.1f} us")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
