#!/usr/bin/env python3
"""Split an rmw ping-pong round trip into ping send, wire, pong turnaround and ping receive, from
the pcaps rmw_crosshost_rtt.sh CAPTURE=1 records on both Pis (RMW_PERF_PLAN.md section 6).

Usage: rmw_pcap_split.py <OUT file of rmw_crosshost_rtt.sh>   (pcaps are read from <OUT>.pcaps/)

Matching needs no knowledge of any wire format. The ping's Bench sample starts with send_ns, the
ping's CLOCK_MONOTONIC at publish, and the pong echoes the sample back unchanged. On the ping host a
client->server packet is a ping when it carries, at any offset, 8 little-endian bytes within 50 ms of
(pcap time - client REALTIME-MONOTONIC offset). Those 8 bytes are then the key for the same sample in
every other packet, on both hosts and in both directions, so RTPS and TickLE are matched alike. A
retransmission repeats its key; only the first copy per host and direction counts.

Per sample, with P/R the ping host's departure/arrival and A/D the pong host's arrival/departure
(tcpdump's tap: after the driver on receive, before it on send):
  ping_send   P - (send_ns + client offset)     ping process: publish to the tap, one clock
  pong_turn   D - A                             pong host: tap to tap, kernel receive, wake, rmw, echo
  wire        (R - P) - (D - A)                 both links, NICs and drivers below the tap
  ping_recv   rtt_avg_ms - mean(R - send_ns')   mean only: the ping reports no per-sample RTT

HOW TO READ IT (written before the first capture, 2026-09-26). rmw_tickle's RTT is ~0.08 ms above
CycloneDDS's, and ~240 us of each round trip is outside rmw_tickle's stamps (RMW_PERF_PLAN.md 5).
  - CONTROL: wire must agree across the three rmw implementations within ~10 us, since it is the
    same link carrying packets of nearly the same size. If it does not, the split is measuring
    something other than what is labelled, and no other column is to be read.
  - If rmw_tickle's pong_turn exceeds CycloneDDS's by most of the deficit (>= 60 us), the deficit
    is in the pong process; RMW_PERF_PLAN's next step is inside that span (rx_wake vs tap).
  - If ping_send + ping_recv carry it instead, it is on the ping side, which the rmw trace never
    stamped, and the pong-side hypotheses (H1-H3) were refuted for that reason.
  - If it spreads with no segment above ~30 us, no single fix is indicated; report the split as is.
"""
import re
import statistics
import struct
import sys
from pathlib import Path

WINDOW_NS = 50_000_000
MIN_MONO_NS = 10**9


def packets(path):
    """Yields (time_ns, src_ip, dst_ip, udp_payload) for every IPv4/UDP packet of a classic pcap."""
    with open(path, "rb") as f:
        head = f.read(24)
        magic = struct.unpack("<I", head[:4])[0]
        if magic == 0xA1B23C4D:
            scale = 1
        elif magic == 0xA1B2C3D4:
            scale = 1000
        else:
            sys.exit(f"{path}: not a little-endian classic pcap")
        linktype = struct.unpack("<I", head[20:24])[0]
        if linktype != 1:
            sys.exit(f"{path}: linktype {linktype}, expected Ethernet")
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            sec, frac, incl, _ = struct.unpack("<IIII", rec)
            buf = f.read(incl)
            if struct.unpack(">H", buf[12:14])[0] != 0x0800:
                continue
            ip = buf[14:]
            ihl = (ip[0] & 0x0F) * 4
            if ip[9] != 17:
                continue
            yield sec * 10**9 + frac * scale, ip[12:16], ip[16:20], ip[ihl + 8:]


def ping_keys(pcap, offset_ns):
    """Finds the ping samples on the ping host: returns ({key: P}, client_ip, server_ip)."""
    found = {}
    ends = None
    for t, src, dst, pay in packets(pcap):
        mono = t - offset_ns
        for k in range(0, len(pay) - 7):
            v = struct.unpack_from("<Q", pay, k)[0]
            if MIN_MONO_NS < v <= mono and mono - v < WINDOW_NS:
                key = pay[k:k + 8]
                if key not in found:
                    found[key] = (t, v)
                ends = ends or (src, dst)
                break
    return found, ends


def first_by_key(pcap, keys, src_ip):
    """{key: first time} for packets from src_ip carrying a key."""
    out = {}
    for t, src, _, pay in packets(pcap):
        if src != src_ip:
            continue
        for key in keys:
            if key not in out and key in pay:
                out[key] = t
                break
    return out


def stats(xs):
    xs = sorted(xs)
    return (f"mean {statistics.fmean(xs) / 1000:7.1f}  p50 {xs[len(xs) // 2] / 1000:7.1f}  "
            f"p90 {xs[int(len(xs) * 0.9)] / 1000:7.1f} us")


def main():
    out = Path(sys.argv[1])
    text = out.read_text()
    offsets = {}
    for m in re.finditer(r"clock_offset_ns stem=(\S+) before: client=(-?\d+) server=(-?\d+) "
                         r"after: client=(-?\d+) server=(-?\d+)", text):
        c0, s0, c1, s1 = (int(x) for x in m.groups()[1:])
        offsets[m.group(1)] = ((c0 + c1) // 2, abs(c1 - c0), abs(s1 - s0))
    rtts = {}
    for m in re.finditer(r"^(\S+) (\S+) (\S+) rep(\d+)\S* \| (\S+) \|.*?rtt_avg_ms=([\d.]+)", text, re.M):
        rtts[f"{m.group(1)}_{m.group(2)}_{m.group(3)}_rep{m.group(4)}"] = (m.group(5), float(m.group(6)))
    for stem, (coff, cdrift, sdrift) in offsets.items():
        ping, pong = out.with_suffix(out.suffix + ".pcaps") / f"{stem}_ping.pcap", \
            out.with_suffix(out.suffix + ".pcaps") / f"{stem}_pong.pcap"
        verdict, rtt = rtts.get(stem, ("missing", float("nan")))
        print(f"== {stem}  (row {verdict}, app rtt_avg {rtt:.3f} ms, offset drift client {cdrift} ns "
              f"server {sdrift} ns)")
        if not ping.exists() or not pong.exists():
            print("   VOID: pcap missing")
            continue
        sent, ends = ping_keys(ping, coff)
        if not sent:
            print("   VOID: no ping sample found in the ping host's capture")
            continue
        cli, srv = ends
        back = first_by_key(ping, sent, srv)
        arr = first_by_key(pong, sent, cli)
        dep = first_by_key(pong, sent, srv)
        full = [k for k in sent if k in back and k in arr and k in dep]
        print(f"   matched {len(full)} of {len(sent)} pings (echo seen {len(back)}, "
              f"pong host in {len(arr)} out {len(dep)})")
        if not full:
            print("   VOID: nothing matched on both hosts")
            continue
        send = [sent[k][0] - sent[k][1] - coff for k in full]
        turn = [dep[k] - arr[k] for k in full]
        wire = [(back[k] - sent[k][0]) - (dep[k] - arr[k]) for k in full]
        tap_rtt = [back[k] - sent[k][1] - coff for k in full]
        bad = sum(1 for x in send + turn + wire if x < 0)
        print(f"   ping_send  {stats(send)}")
        print(f"   pong_turn  {stats(turn)}")
        print(f"   wire       {stats(wire)}")
        recv = rtt * 1e6 - statistics.fmean(tap_rtt)
        print(f"   ping_recv  mean {recv / 1000:7.1f} us   (app rtt_avg minus send_ns-to-echo-at-tap)")
        if bad:
            print(f"   WARNING: {bad} negative segments - offsets or matching are wrong for this row")


if __name__ == "__main__":
    main()
