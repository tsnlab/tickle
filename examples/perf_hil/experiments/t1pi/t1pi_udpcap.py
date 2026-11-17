#!/usr/bin/env python3
"""Plain UDP capacity probe for the T1-Pi link (WIFI_DIAG_PLAN.md section 2). SIMULATION ONLY (runs inside t1p_* namespaces).
  tx: sends bursts of B datagrams back to back, one burst every B/R seconds, for DUR seconds; payload carries (cell id, seq, send ns).
  rx: counts datagrams of the cell for RXDUR seconds, per-second arrival bins (relative to the first arrival), per-burst arrival counts.
Prints one JSON line."""
import argparse, json, os, socket, struct, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("mode", choices=["tx", "rx"])
ap.add_argument("--bind", required=True)
ap.add_argument("--dst", default="")
ap.add_argument("--port", type=int, default=9999)
ap.add_argument("--size", type=int, default=1472)
ap.add_argument("--burst", type=int, default=1)
ap.add_argument("--rate", type=float, default=100.0)
ap.add_argument("--dur", type=float, default=5.0)
ap.add_argument("--rxdur", type=float, default=7.0)
ap.add_argument("--cell", type=int, default=0)
a = ap.parse_args()
H = struct.Struct("!IIQ")
pc = time.perf_counter

if a.mode == "tx":
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    s.bind((a.bind, 0))
    buf = bytearray(max(a.size, H.size))
    interval = a.burst / a.rate
    nb = max(1, int(a.dur / interval))
    sent = errs = 0
    c0 = time.process_time()
    e0 = time.time()
    t0 = pc()
    last = t0
    for k in range(nb):
        target = t0 + k * interval
        while True:
            d = target - pc()
            if d <= 0:
                break
            if d > 0.003:
                time.sleep(d - 0.002)
        for _ in range(a.burst):
            H.pack_into(buf, 0, a.cell, sent, time.monotonic_ns())
            try:
                s.sendto(buf, (a.dst, a.port))
            except OSError:
                errs += 1
            sent += 1
        last = pc()
    print(json.dumps({"mode": "tx", "sent": sent, "errs": errs, "bursts": nb, "took": last - t0, "t_epoch0": e0, "t_epoch1": time.time(), "cpu_s": time.process_time() - c0}))
else:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    s.bind((a.bind, a.port))
    s.settimeout(0.2)
    seen = {}
    foreign = dups = 0
    first = None
    bins = {}
    end = pc() + a.rxdur
    e0 = time.time()
    c0 = time.process_time()
    while pc() < end:
        try:
            d = s.recv(2048)
        except socket.timeout:
            continue
        if len(d) < H.size:
            continue
        cell, seq, _ = H.unpack_from(d)
        if cell != a.cell:
            foreign += 1
            continue
        t = pc()
        if seq in seen:
            dups += 1
            continue
        seen[seq] = 1
        if first is None:
            first = t
        b = int(t - first)
        bins[b] = bins.get(b, 0) + 1
    nbin = (max(bins) + 1) if bins else 0
    per_burst = {}
    for q in seen:
        per_burst[q // a.burst] = per_burst.get(q // a.burst, 0) + 1
    print(json.dumps({"mode": "rx", "got": len(seen), "dups": dups, "foreign": foreign, "bins": [bins.get(i, 0) for i in range(nbin)],
                      "bursts_hit": len(per_burst),
                      "burst_mean_frac": (sum(per_burst.values()) / (len(per_burst) * a.burst)) if per_burst else 0.0,
                      "t_epoch0": e0, "cpu_s": time.process_time() - c0}))
