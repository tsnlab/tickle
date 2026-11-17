#!/usr/bin/env python3
"""CLOCK_MONOTONIC offset between the two Pis (they do not share a clock), for the one-way latencies of rel_large.
  t1pi_clock.py serve IP PORT   answers 'T' datagrams with its time.monotonic_ns() (runs until killed; exits after 5 s without a request)
  t1pi_clock.py probe IP PORT   prints "offset_us=<server - client>" from the minimum-RTT exchange of 300 (error about RTT/2)
A reported one-way latency (receiver clock minus sender clock) is true + offset when the receiver is the server."""
import socket, sys, time

def serve(ip, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((ip, port))
    s.settimeout(5)
    try:
        while True:
            data, addr = s.recvfrom(64)
            if data[:1] == b"T":
                s.sendto(b"%d" % time.monotonic_ns(), addr)
    except socket.timeout:
        pass

def probe(ip, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.2)
    best = None
    for _ in range(300):
        t0 = time.monotonic_ns()
        s.sendto(b"T", (ip, port))
        try:
            data, _ = s.recvfrom(64)
        except socket.timeout:
            continue
        t1 = time.monotonic_ns()
        rtt = t1 - t0
        off = int(data) - (t0 + t1) // 2
        if best is None or rtt < best[0]:
            best = (rtt, off)
    if best is None:
        print("offset_us=nan rtt_us=nan")
        return 1
    print("offset_us=%.1f rtt_us=%.1f" % (best[1] / 1e3, best[0] / 1e3))
    return 0

if __name__ == "__main__":
    mode, ip, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    sys.exit(serve(ip, port) or 0 if mode == "serve" else probe(ip, port))
