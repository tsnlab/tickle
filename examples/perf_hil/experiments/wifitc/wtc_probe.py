#!/usr/bin/env python3
"""Calibration probe of the emulated link (PLAN.md section 4): what the link carries, before any framework runs.
  wtc_probe.py serve IP PORT SECONDS      count datagrams per phase, reorder and gaps; prints one JSON line at the end
  wtc_probe.py flood IP PORT SIZE SECONDS offer as fast as the socket allows (phase 1): the sustained rate the link carries
  wtc_probe.py burst IP PORT SIZE N ROUNDS  N back-to-back datagrams, ROUNDS times, 1 s apart (phase 2+r): how many of each burst arrive
  wtc_probe.py rate IP PORT SIZE PPS SECONDS  paced offer at PPS (phase 100): loss and reorder of a gentle flow
Datagram: phase u32, seq u32, padding."""
import json, socket, struct, sys, time


def serve(ip, port, secs):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    s.bind((ip, port))
    s.settimeout(0.5)
    end = time.time() + secs
    ph = {}
    while time.time() < end:
        try:
            d, _ = s.recvfrom(2048)
        except socket.timeout:
            continue
        t = time.time()
        p, q = struct.unpack("!II", d[:8])
        r = ph.setdefault(p, {"n": 0, "first": t, "last": t, "max": -1, "reorder": 0, "bytes": 0})
        r["n"] += 1
        r["bytes"] += len(d) + 28
        r["last"] = t
        if q < r["max"]:
            r["reorder"] += 1
        r["max"] = max(r["max"], q)
    print(json.dumps({str(k): v for k, v in ph.items()}))


def client(ip, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
    s.connect((ip, port))
    return s


def main():
    m = sys.argv[1]
    ip, port = sys.argv[2], int(sys.argv[3])
    if m == "serve":
        serve(ip, port, float(sys.argv[4]))
    elif m == "flood":
        size, secs = int(sys.argv[4]), float(sys.argv[5])
        s = client(ip, port)
        pad = b"x" * (size - 8)
        end, q = time.time() + secs, 0
        while time.time() < end:
            try:
                s.send(struct.pack("!II", 1, q) + pad)
            except OSError:
                pass
            q += 1
        print(json.dumps({"offered": q}))
    elif m == "burst":
        size, n, rounds = int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
        s = client(ip, port)
        pad = b"x" * (size - 8)
        for r in range(rounds):
            for q in range(n):
                try:
                    s.send(struct.pack("!II", 2 + r, q) + pad)
                except OSError:
                    pass
            time.sleep(1.0)
        print(json.dumps({"bursts": rounds, "n": n}))
    elif m == "rate":
        size, pps, secs = int(sys.argv[4]), float(sys.argv[5]), float(sys.argv[6])
        s = client(ip, port)
        pad = b"x" * (size - 8)
        t0, q = time.time(), 0
        while time.time() - t0 < secs:
            s.send(struct.pack("!II", 100, q) + pad)
            q += 1
            while time.time() - t0 < q / pps:
                pass
        print(json.dumps({"offered": q}))


main()
