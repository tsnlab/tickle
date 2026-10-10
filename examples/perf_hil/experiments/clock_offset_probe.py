#!/usr/bin/env python3
"""The wall-clock offset between two hosts, measured NTP-style over the test link, for one-clock latency.

Why (2026-10-10, ~/rig_queue_largemsg_A4.sh): perf_test stamps a sample with the publisher's system_clock and takes
its latency on the subscriber's, so a cross-host latency is the true latency plus the offset between the two Pis'
clocks. The A3 queue saw that offset move by ~11 ms over one L3 call (the per-run floor went 8.4 -> 19.6 ms) and about
2 ms within one 60 s run. rmw_keepall_rig.sh (CLOCK_PROBE=1) runs this before and after every run and the summary
subtracts the offset, interpolated linearly in time, so every latency it reports is on the publisher's clock.

  serve  --bind IP --port P [--idle 3] [--max 15]   on the subscriber's host: answers each probe with its receive and
                                                    send times (CLOCK_REALTIME, the clock perf_test stamps with on the
                                                    rig), exits on the client's goodbye (3 sent), after --idle s
                                                    without a probe, or after --max s in all
  query  --server IP --port P [--count 64]          on the publisher's host: --count probes 5 ms apart; prints ONE line
                                                    PROBE ok=1 wall_ns=... offset_ns=... delay_min_ns=... used=... n=...

offset_ns = server clock - client clock, the median of theta = ((t2 - t1) + (t3 - t4)) / 2 over the quarter of the
replies with the smallest round trip (at least 8); delay_min_ns is the smallest round trip seen. With fewer than 16
replies it prints PROBE ok=0 and exits 1. The reading rules (when an offset is trusted) are the summary's
(largemsg_rig_summary.py); on one host the offset must read 0 to within the probe's own error, which is the PC
preflight's control.
"""
import argparse
import socket
import statistics
import struct
import sys
import time

FMT = "!Iqqq"  # seq, t1, t2, t3
BYE = 0xFFFFFFFF  # the client's last datagrams: the server exits at once, so the next run's server can bind the port


def serve(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((args.bind, args.port))
    start = time.monotonic()
    last = start
    while True:
        now = time.monotonic()
        if now - start > args.max or (now - last > args.idle):
            return 0
        s.settimeout(0.2)
        try:
            data, peer = s.recvfrom(64)
        except socket.timeout:
            continue
        t2 = time.time_ns()
        if len(data) < struct.calcsize(FMT):
            continue
        seq, t1, _, _ = struct.unpack(FMT, data[:struct.calcsize(FMT)])
        if seq == BYE:
            return 0
        last = time.monotonic()
        s.sendto(struct.pack(FMT, seq, t1, t2, time.time_ns()), peer)


def query(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.2)
    samples = []
    for seq in range(args.count):
        t1 = time.time_ns()
        s.sendto(struct.pack(FMT, seq, t1, 0, 0), (args.server, args.port))
        try:
            while True:
                data, _ = s.recvfrom(64)
                t4 = time.time_ns()
                rseq, rt1, t2, t3 = struct.unpack(FMT, data[:struct.calcsize(FMT)])
                if rseq == seq and rt1 == t1:
                    break
        except socket.timeout:
            continue
        samples.append((((t2 - t1) + (t3 - t4)) / 2, (t4 - t1) - (t3 - t2), (t1 + t4) // 2))
        time.sleep(0.005)
    for _ in range(3):
        s.sendto(struct.pack(FMT, BYE, 0, 0, 0), (args.server, args.port))
    if len(samples) < 16:
        print(f"PROBE ok=0 n={len(samples)} of {args.count} replies")
        return 1
    best = sorted(samples, key=lambda x: x[1])[:max(8, len(samples) // 4)]
    offset = statistics.median(x[0] for x in best)
    wall = statistics.median(x[2] for x in best)
    spread = max(x[0] for x in best) - min(x[0] for x in best)
    print(f"PROBE ok=1 wall_ns={int(wall)} offset_ns={int(offset)} delay_min_ns={int(best[0][1])} "
          f"delay_used_max_ns={int(best[-1][1])} theta_spread_ns={int(spread)} used={len(best)} n={len(samples)}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    a = sub.add_parser("serve")
    a.add_argument("--bind", required=True)
    a.add_argument("--port", type=int, required=True)
    a.add_argument("--idle", type=float, default=3.0)
    a.add_argument("--max", type=float, default=15.0)
    b = sub.add_parser("query")
    b.add_argument("--server", required=True)
    b.add_argument("--port", type=int, required=True)
    b.add_argument("--count", type=int, default=64)
    args = ap.parse_args()
    return serve(args) if args.mode == "serve" else query(args)


if __name__ == "__main__":
    sys.exit(main())
