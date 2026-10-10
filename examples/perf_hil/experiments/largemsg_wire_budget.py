#!/usr/bin/env python3
"""largemsg_wire_budget.py - how much of a large sample's cross-host latency is the wire, per framework.

Why (2026-10-11): in the rig's I4 cell (Array4m, 4,194,320 B, 15 Hz, Pi 5 to Pi 5 over 1 GbE; A4 stage 3b
RELIABLE KEEP_LAST 10, 3c BEST_EFFORT KEEP_LAST 5) rmw_tickle's latency median was 38.69 ms against Fast DDS's 37.50
and CycloneDDS's 38.60. A one-way latency of a sample this size is mostly the time its frames take to cross the
link, and each framework cuts the sample into frames differently. This prints, per framework, the bytes its frames
put on the wire for one sample, their time at 1 Gb/s, and what is left of a measured median once that is taken away:
the work the two ends do that the wire does not hide (encoding before the first frame, decoding after the last,
wake-ups).

The frame model (no capture was taken on the rig; the counts are checked against what the runs counted - see below):
  every Ethernet frame costs its IP packet + 14 (header) + 4 (FCS) + 8 (preamble) + 12 (inter-frame gap) = +38 B;
  rmw_tickle  one UDP datagram per frame: 4 B single header + 12 B FRAG_CONT_L + 1,452 B of CDR (fragment 0: 18 B
              FRAG_FIRST_L, 1,446 B), + 8 UDP + 20 IP -> 1,496 B packets (DESIGN.md section 8); the CDR is the
              sample plus rmw_tickle's 4-byte psn. Checked: the publisher's tx_datagrams (2,609,322 for 902 samples,
              A4 3b r1) is 2,889 a sample plus 3.8 a sample of HEARTBEATs and repairs.
  Fast DDS    shipped profile: one RTPS message per UDP datagram of up to 65,500 B (DATA_FRAG, about 68 B of RTPS
              headers each), which the kernel splits into IP fragments of 1,480 B of payload (20 B IP header each).
  CycloneDDS  defaults (FragmentSize 1,344 B, MaxMessageSize 14,720 B): ten DATA_FRAGs of 1,344 B (36 B submessage
              header each) in one RTPS message of about 13,840 B, IP-fragmented the same way.
The two DDS models are approximate to a few headers per datagram, which is microseconds per sample.

Usage: largemsg_wire_budget.py [--size BYTES] [--median ARM=MS ...]
       with no --median, the A4 3b/3c medians (largemsg_rig_summary.py, whole counts) are used.
"""
import argparse
import math

ETH = 38  # 14 header + 4 FCS + 8 preamble + 12 inter-frame gap
IP = 20
UDP = 8
MTU_IP_PAYLOAD = 1480


def frames_ip_fragmented(udp_payload):
    """Wire bytes of one UDP datagram the kernel splits into IP fragments."""
    ip_payload = udp_payload + UDP
    full, rest = divmod(ip_payload, MTU_IP_PAYLOAD)
    wire = full * (MTU_IP_PAYLOAD + IP + ETH)
    if rest:
        wire += rest + IP + ETH
    return wire, full + (1 if rest else 0)


def tickle(size):
    cdr = size + 4  # rmw_tickle's psn ahead of the CDR
    first, cont = 1446, 1452
    count = 1 + math.ceil((cdr - first) / cont)
    last = cdr - first - (count - 2) * cont
    wire = (first + 4 + 18 + UDP + IP + ETH) + (count - 2) * (cont + 4 + 12 + UDP + IP + ETH)
    wire += last + 4 + 12 + UDP + IP + ETH
    return wire, count, count


def fastdds(size, datagram=65500, rtps=68):
    per = datagram - rtps
    count = math.ceil(size / per)
    wire = frames = 0
    left = size
    for _ in range(count):
        chunk = min(per, left)
        left -= chunk
        w, f = frames_ip_fragmented(chunk + rtps)
        wire += w
        frames += f
    return wire, frames, count


def cyclonedds(size, frag=1344, per_message=10, submessage=36, rtps=32):
    frags = math.ceil(size / frag)
    wire = frames = datagrams = 0
    left = frags
    remaining = size
    while left:
        n = min(per_message, left)
        left -= n
        payload = min(n * frag, remaining)
        remaining -= payload
        w, f = frames_ip_fragmented(rtps + n * submessage + payload)
        wire += w
        frames += f
        datagrams += 1
    return wire, frames, datagrams


MODELS = {"tickle": tickle, "fastdds": fastdds, "cyclonedds": cyclonedds}
# A4 3b (RELIABLE KEEP_LAST 10) and 3c (BEST_EFFORT KEEP_LAST 5), Array4m cross-host: largemsg_rig_summary.py's
# medians over usable reps, in that order.
A4_MEDIANS = {"tickle": (38.69, 38.68), "fastdds": (37.50, 37.46), "cyclonedds": (38.60, 38.58)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=4194320, help="sample bytes (perf_test Array4m: 4,194,320)")
    ap.add_argument("--median", action="append", default=[], help="ARM=ms, a measured latency median")
    args = ap.parse_args()
    medians = {k: v for k, v in A4_MEDIANS.items()}
    for item in args.median:
        arm, _, ms = item.partition("=")
        medians[arm] = (float(ms),)
    print(f"sample {args.size} B; 1 Gb/s; frame = IP packet + {ETH} B")
    print(f"{'arm':12} {'datagrams':>9} {'frames':>7} {'wire B':>10} {'wire ms':>8} {'B/frame':>8}"
          f" {'median ms':>16} {'left ms':>14}")
    left_of = {}
    for arm, model in MODELS.items():
        wire, frames, datagrams = model(args.size)
        ms = wire * 8 / 1e9 * 1e3
        meds = medians.get(arm, ())
        left = [m - ms for m in meds]
        left_of[arm] = left
        print(f"{arm:12} {datagrams:9d} {frames:7d} {wire:10d} {ms:8.2f} {wire / frames:8.1f}"
              f" {'/'.join(f'{m:.2f}' for m in meds):>16} {'/'.join(f'{x:.2f}' for x in left):>14}")
    t_wire = tickle(args.size)[0]
    f_wire = fastdds(args.size)[0]
    print(f"tickle - fastdds on the wire: {(t_wire - f_wire) * 8 / 1e6:.2f} ms "
          f"({t_wire - f_wire} B: per frame {UDP} B UDP header + 16 B TickLE framing, "
          f"against Fast DDS's ~{68 * 1e3 / 65432:.1f} B of RTPS per 1,000 B)")
    if left_of.get("tickle") and left_of.get("fastdds"):
        d = [t - f for t, f in zip(left_of["tickle"], left_of["fastdds"])]
        print(f"tickle - fastdds off the wire: {'/'.join(f'{x:.2f}' for x in d)} ms")


if __name__ == "__main__":
    main()
