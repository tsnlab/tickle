#!/usr/bin/env python3
"""C12 idle wake: send one empty UDP datagram to the pong's data socket LEAD ms before each ping (runs on the ping host).

Written 2026-10-31 for idle_wake_rig.sh, before any of its runs. The question (claude_reports/C12_PONG_RESIDUAL_2026-10-30.md,
side finding): a ping that reaches rmw_tickle's pong 26-47 ms after the thread read the peer's announcement is received
in 13.26 us (tap -> return of the receive), one after ~100 ms of idle in 15.50 us; CycloneDDS idles ~100 ms too and
reads 12.9 us. Is the idle receive path where TickLE's pong gap to CycloneDDS sits?

The treatment, and why it is the analogue of the announcement read: a datagram that arrives over eth0 from the ping
host and wakes the very thread that will receive the next ping, through the same socket, then is dropped by the stack
at once. The destination is the socket the ping's own samples are addressed to (learned from them, below), so for
both rmws it is the thread that receives the ping - for rmw_tickle the wait ring's owner, for CycloneDDS the receive
thread of its user-data port. The datagram is EMPTY: rmw_tickle takes a zero-length datagram as a doorbell and drops
it before any parsing or logging (tickle.c process_datagram_in), and a zero-length read is not an RTPS message either.
No rmw code is changed, so both rmws get the identical treatment.

How the ping is predicted: the ping's capture (rmw_crosshost_rtt.sh CAPTURE=1, tcpdump -U, so packet by packet) is
read as it grows. A client -> server packet that carries 8 little-endian bytes within 20 ms of (its pcap time - this
host's REALTIME - MONOTONIC offset) is a ping sample (the ping's send_ns, rmw_pcap_split.py's rule). The next ping is
due one period later (the median of the last 9 intervals between 50 and 200 ms, 100.2 ms until 3 are known); the
datagram goes out LEAD ms before that. Each send is logged with its target, so a late send is visible; whether the
treatment reached the pong is read on the pong side (idle_wake_read.py), not assumed from here.

LEAD "none" is the control arm: everything above runs (the capture is read, the pings are predicted) and nothing is
sent, so the only difference between arms is the datagram.

Output (stdout): "SEND <realtime_ns> target=<ns> late_us=<n> dport=<p>" per datagram, then always one line
  INJECT_END lead_ms=<x> pings=<n> sent=<n> dport=<p> period_us=<n> late_p50_us=<n> late_max_us=<n> reason=<why>
Exits by itself: DEADLINE_S after start, or IDLE_EXIT_S after the last ping once one was seen.

Usage: idle_wake_injector.py PCAP CLIENT_IP SERVER_IP LEAD_MS|none [DEADLINE_S=45] [IDLE_EXIT_S=3]
"""
import ctypes
import os
import socket
import struct
import sys
import time

SRC_PORT = 47999  # every injected datagram comes from this port: the pong-side reader finds them by it
KEY_WINDOW_NS = 20_000_000
MIN_MONO_NS = 10**9
DEFAULT_PERIOD_NS = 100_200_000


def ip4(s):
    return socket.inet_aton(s)


class PcapTail:
    """Reads a classic pcap that another process is still appending to; yields complete records only."""

    def __init__(self, path):
        self.path, self.buf, self.scale, self.f = path, b"", None, None

    def poll(self):
        if self.f is None:
            try:
                self.f = open(self.path, "rb")  # noqa: SIM115 - held open across polls on purpose
            except OSError:
                return []
        self.buf += self.f.read()
        out = []
        if self.scale is None:
            if len(self.buf) < 24:
                return out
            magic = struct.unpack("<I", self.buf[:4])[0]
            self.scale = 1 if magic == 0xA1B23C4D else 1000
            self.buf = self.buf[24:]
        while len(self.buf) >= 16:
            sec, frac, incl, _ = struct.unpack("<IIII", self.buf[:16])
            if len(self.buf) < 16 + incl:
                break
            pkt, self.buf = self.buf[16:16 + incl], self.buf[16 + incl:]
            out.append((sec * 10**9 + frac * self.scale, pkt))
        return out


def udp_of(pkt):
    """(src, dst, sport, dport, payload) of an Ethernet/IPv4/UDP frame, else None."""
    if len(pkt) < 42 or struct.unpack(">H", pkt[12:14])[0] != 0x0800:
        return None
    ip = pkt[14:]
    ihl = (ip[0] & 0x0F) * 4
    if ip[9] != 17:
        return None
    sport, dport = struct.unpack(">HH", ip[ihl:ihl + 4])
    return ip[12:16], ip[16:20], sport, dport, ip[ihl + 8:]


def ping_key(pay, mono):
    for k in range(0, len(pay) - 7):
        v = struct.unpack_from("<Q", pay, k)[0]
        if MIN_MONO_NS < v <= mono and mono - v < KEY_WINDOW_NS:
            return v
    return None


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 64
    pcap, client, server, lead_s = argv[1], ip4(argv[2]), ip4(argv[3]), argv[4]
    deadline_s = float(argv[5]) if len(argv) > 5 else 45.0
    idle_exit_s = float(argv[6]) if len(argv) > 6 else 3.0
    lead_ns = None if lead_s == "none" else int(float(lead_s) * 1e6)
    try:  # 1 us timer slack (PR_SET_TIMERSLACK), so a sleep ends near its target
        ctypes.CDLL(None, use_errno=True).prctl(29, 1000, 0, 0, 0)
    except OSError:
        pass
    off = time.clock_gettime_ns(time.CLOCK_REALTIME) - time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((argv[2], SRC_PORT))
    tail = PcapTail(pcap)
    t_start = time.time_ns()
    pings, sent, dport, last_v, last_tap, next_send = 0, 0, None, 0, None, None
    intervals, lates = [], []
    reason = "deadline"
    while True:
        now = time.time_ns()
        if now - t_start > deadline_s * 1e9:
            break
        if last_tap is not None and now - last_tap > idle_exit_s * 1e9:
            reason = "ping ended"
            break
        for t, pkt in tail.poll():
            u = udp_of(pkt)
            if not u or u[0] != client or u[2] == SRC_PORT:
                continue
            if u[1] != server:  # a broadcast: not the pong's data socket
                continue
            v = ping_key(u[4], t - off)
            if v is None or v < last_v + 50_000_000:
                continue
            if last_tap is not None and 50_000_000 < t - last_tap < 200_000_000:
                intervals = (intervals + [t - last_tap])[-9:]
            last_v, last_tap, dport = v, t, u[3]
            pings += 1
            period = sorted(intervals)[len(intervals) // 2] if len(intervals) >= 3 else DEFAULT_PERIOD_NS
            next_send = t + period - lead_ns if lead_ns is not None else None
        now = time.time_ns()
        if next_send is not None and dport is not None:
            wait = next_send - now
            if wait <= 0:
                sock.sendto(b"", (argv[3], dport))
                at = time.time_ns()
                lates.append((at - next_send) // 1000)
                print(f"SEND {at} target={next_send} late_us={(at - next_send) // 1000} dport={dport}", flush=True)
                sent += 1
                next_send = None
                continue
            if wait < 3_000_000:
                time.sleep(wait / 1e9)
                continue
        time.sleep(0.002)
    lates.sort()
    period = sorted(intervals)[len(intervals) // 2] if intervals else 0
    print(f"INJECT_END lead_ms={lead_s} pings={pings} sent={sent} dport={dport} period_us={period // 1000} "
          f"late_p50_us={lates[len(lates) // 2] if lates else 'na'} late_max_us={lates[-1] if lates else 'na'} "
          f"reason={reason} pid={os.getpid()}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
