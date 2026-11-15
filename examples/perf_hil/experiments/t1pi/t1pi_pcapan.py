#!/usr/bin/env python3
"""Burst/arrival analysis of a T1-Pi diagnosis run (PCAP=1): tcpdump text of the sender-side (cli.pcap on hwsta) and receiver-side (srv.pcap
on t1pgs) captures. Per 1 s bin: big datagrams (>= 1000 B UDP payload, pub -> sub) seen at the sender and at the receiver, small datagrams
sub -> pub (NACK/ACK) seen at the receiver side; prints the arrival fraction per bin and totals. Usage: t1pi_pcapan.py RUNBASE (without .cli.pcap)"""
import re, subprocess, sys, collections
base = sys.argv[1]
PUB, SUB = "10.90.0.2", "10.90.0.3"
def read(path):
    out = subprocess.run(["tcpdump", "-tt", "-nn", "-r", path], capture_output=True, text=True, errors="replace").stdout
    pk = []
    for l in out.splitlines():
        m = re.match(r"(\d+\.\d+) IP (\d+\.\d+\.\d+\.\d+)\.\d+ > (\d+\.\d+\.\d+\.\d+)\.\d+: UDP, length (\d+)", l)
        if m:
            pk.append((float(m.group(1)), m.group(2), m.group(3), int(m.group(4))))
    return pk
c, s = read(base + ".cli.pcap"), read(base + ".srv.pcap")
if not c: sys.exit("no packets")
t0 = c[0][0]
bins = collections.defaultdict(lambda: [0, 0, 0, 0])
for t, a, b, n in c:
    if a == PUB and b == SUB and n >= 1000: bins[int(t - t0)][0] += 1
    if a == SUB and b == PUB and n < 1000: bins[int(t - t0)][3] += 1
for t, a, b, n in s:
    if a == PUB and b == SUB and n >= 1000: bins[int(t - t0)][1] += 1
    if a == SUB and b == PUB and n < 1000: bins[int(t - t0)][2] += 1
print("sec  big_sent  big_recv  frac   ctl_sub->pub(sent_by_sub, seen_at_pub)")
tot = [0, 0, 0, 0]
for k in sorted(bins):
    a, b, cc, d = bins[k]
    tot = [x + y for x, y in zip(tot, bins[k])]
    print(f"{k:4d} {a:8d} {b:9d} {b/a if a else 0:6.2f} {cc:6d} {d:6d}")
print(f"TOTAL big_sent={tot[0]} big_recv={tot[1]} frac={tot[1]/tot[0] if tot[0] else 0:.2f} ctl_sent={tot[2]} ctl_seen_at_pub={tot[3]}")
