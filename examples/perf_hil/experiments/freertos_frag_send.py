#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Linux-side sender for freertos_frag_q.sh. Runs inside the experiment's private netns.

Sends `count` UDP datagrams of n * 1446 bytes to the guest (broadcast, DF clear so the kernel fragments anything
above 1472 bytes), paced to `rate` bytes/s by an absolute schedule (no drift), then prints the qdisc/link counters
(the realized netem loss and any tap queue drop - the treatment and the backlog witness), then repeats an END datagram
every 200 ms until the guest's log shows its final line or `end_wait` seconds pass.

usage: freertos_frag_send.py <dev> <dst_ip> <cell> <n> <rate_Bps> <count> <guest_log> <end_wait_s>
"""

import socket
import struct
import subprocess
import sys
import time

PORT = 7650
UNIT = 1446
MAGIC = 0x46524147
IP_MTU_DISCOVER = 10
IP_PMTUDISC_DONT = 0


def counters(dev: str) -> str:
    q = subprocess.run(["tc", "-s", "qdisc", "show", "dev", dev], capture_output=True, text=True, check=False).stdout
    sent = dropped = -1
    for line in q.splitlines():
        line = line.strip()
        if line.startswith("Sent "):
            f = line.replace(",", " ").replace("(", " ").replace(")", " ").split()
            sent = int(f[3])
            dropped = int(f[f.index("dropped") + 1])
            break
    link = subprocess.run(["ip", "-s", "-s", "link", "show", "dev", dev], capture_output=True, text=True,
                          check=False).stdout.splitlines()
    tx_pk = tx_drop = -1
    for i, line in enumerate(link):
        if line.strip().startswith("TX:"):
            f = link[i + 1].split()
            tx_pk, tx_drop = int(f[1]), int(f[3])
            break
    return f"qdisc_sent={sent} qdisc_dropped={dropped} link_tx_packets={tx_pk} link_tx_dropped={tx_drop}"


def main() -> int:
    dev, dst, cell, n, rate, count, guest_log, end_wait = sys.argv[1:9]
    cell, n, rate, count, end_wait = int(cell), int(n), float(rate), int(count), float(end_wait)
    size = n * UNIT
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
    s.setsockopt(socket.IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_DONT)
    pad = bytes(size - 16)
    interval = size / rate
    print(f"send: start {counters(dev)}", flush=True)
    errors = 0
    t0 = time.perf_counter()
    for seq in range(count):
        target = t0 + seq * interval
        while True:
            now = time.perf_counter()
            if now >= target:
                break
            if target - now > 0.002:
                time.sleep(target - now - 0.001)
        try:
            s.sendto(struct.pack("<IIIHH", MAGIC, cell, seq, n, 0) + pad, (dst, PORT))
        except OSError:
            errors += 1
    elapsed = time.perf_counter() - t0
    print(f"send: done cell={cell} n={n} sent={count} errors={errors} elapsed_s={elapsed:.3f} "
          f"achieved_Bps={count * size / elapsed:.0f} target_Bps={rate:.0f}", flush=True)
    time.sleep(0.5)
    print(f"send: data {counters(dev)}", flush=True)
    end = struct.pack("<IIIHH", MAGIC, cell, count, n, 1)
    deadline = time.monotonic() + end_wait
    tries = 0
    while time.monotonic() < deadline:
        s.sendto(end, (dst, PORT))
        tries += 1
        time.sleep(0.2)
        with open(guest_log, "rb") as f:
            if b"frag_rx: END" in f.read():
                print(f"send: end_seen tries={tries}", flush=True)
                return 0
    print(f"send: end_timeout tries={tries}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
