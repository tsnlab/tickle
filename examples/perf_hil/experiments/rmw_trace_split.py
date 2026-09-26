#!/usr/bin/env python3
"""Where a ping-pong responder's receive-to-reply time goes, above and below core (RMW_PERF_PLAN.md 9, 2026-09-27).

Reads an RMW_TICKLE_TRACE_FILE dump from a build with -DRMW_TICKLE_TRACE=ON (rmw_init.c), stamps oldest first:
    stamp <ns> <thread> <point>
Points (include/tickle/trace.h): 1 rx_wake, 2 rx_datagram, 3 deliver, 12 decoded, 13 converted, 4 signaled,
14 release, 15 timer_set, 5 exec_wake, 16 take_enter, 6 taken, 7 publish, 9 serialized, 10 encoded, 11 tx_start,
8 tx_done.

Each deliver (3) anchors one message. Back from it on its own thread: the last rx_datagram (2), then the last rx_wake
(1) before that. Forward: each point of the chain, the first after the one before and before the next deliver, on
any thread. A message missing any point is skipped and counted; a chain longer than 5 ms is skipped too (an idle gap,
not a round trip). Each segment's median and p90 over the messages kept, in microseconds.

Usage: rmw_trace_split.py <dump file>
"""

import statistics
import sys

NAMES = {1: "rx_wake", 2: "rx_datagram", 3: "deliver", 12: "decoded", 13: "converted", 4: "signaled", 14: "release",
         15: "timer_set", 5: "exec_wake", 16: "take_enter", 6: "taken", 7: "publish", 9: "serialized", 10: "encoded",
         11: "tx_start", 8: "tx_done"}
FORWARD = [12, 13, 4, 14, 15, 5, 16, 6, 7, 9, 10, 11, 8]
CHAIN = [1, 2, 3] + FORWARD
LOOK_BACK_NS = 2_000_000
LONGEST_NS = 5_000_000
P90 = 0.9


def read_stamps(path):
    stamps = []
    with open(path) as dump:
        for line in dump:
            fields = line.split()
            if fields and fields[0] == "stamp":
                stamps.append((int(fields[1]), int(fields[2]), int(fields[3])))
    return stamps


def chain_at(stamps, index, end):
    ns, thread, _ = stamps[index]
    chain = {3: ns}
    for back in range(index - 1, -1, -1):
        b_ns, b_thread, b_point = stamps[back]
        if ns - b_ns > LOOK_BACK_NS:
            break
        if b_thread != thread:
            continue
        if b_point == 2 and 2 not in chain:
            chain[2] = b_ns
        elif b_point == 1 and 2 in chain:
            chain[1] = b_ns
            break
    position = index
    for point in FORWARD:
        for forward in range(position + 1, end):
            if stamps[forward][2] == point:
                chain[point] = stamps[forward][0]
                position = forward
                break
    return chain


def main(path):
    stamps = read_stamps(path)
    delivers = [i for i, stamp in enumerate(stamps) if stamp[2] == 3]
    chains, skipped = [], 0
    for k, index in enumerate(delivers):
        end = delivers[k + 1] if k + 1 < len(delivers) else len(stamps)
        chain = chain_at(stamps, index, end)
        if all(point in chain for point in CHAIN) and chain[8] - chain[1] < LONGEST_NS:
            chains.append(chain)
        else:
            skipped += 1
    print(f"messages {len(chains)} (skipped {skipped})")
    if not chains:
        return 1
    for first, second in zip(CHAIN, CHAIN[1:]):
        spans = sorted((chain[second] - chain[first]) / 1000 for chain in chains)
        print(f"{NAMES[first]:>12} -> {NAMES[second]:<12} median {statistics.median(spans):7.2f} us"
              f"  p90 {spans[int(len(spans) * P90)]:7.2f}")
    whole = sorted((chain[8] - chain[1]) / 1000 for chain in chains)
    print(f"{'rx_wake':>12} -> {'tx_done':<12} median {statistics.median(whole):7.2f} us"
          f"  p90 {whole[int(len(whole) * P90)]:7.2f}  (the whole responder)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
