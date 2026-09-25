# The benchmark payload shape at P2, the like-for-like large size (OPTIMIZATION_PLAN.md section 3).
#
# See p1/Bench.msg for why all four sizes share one message name and why `send_ns` comes first.
#
# 1292 bytes is sized to the *most constrained* framing of the three, not to TickLE's, so that all
# three send exactly one datagram and the comparison at this size is like for like.
#
# It was 1388, and 1388 was wrong. I derived that from the RTPS spec - header 20 + INFO_TS 12 +
# DATA submessage 24 + encapsulation 4, call it 60 to 76 bytes - and said at the time that spec
# arithmetic is not a measurement and had to be gated rather than trusted. The gate then caught
# it: FastDDS sent 2.000 datagrams per sample at 1388 in the 2026-09-25 campaign. Its real
# per-sample framing is 209.6 bytes, nearly three times the estimate.
#
# The sizes below now come from measurement. Per-sample framing at the sizes where every
# framework sent exactly one datagram, which includes Ethernet + IP + UDP (42) as well as each
# protocol's own headers and its amortised ACK/heartbeat traffic:
#
#     tickle 70.6    cyclonedds 104.1    fastdds 209.6
#
# TickLE's 70.6 is the check on that decomposition rather than another datapoint: 42 + 28 = 70,
# and 28 is what DESIGN.md says TickLE's own DATA framing is. The model `sample + base <= 1514`
# (1500 MTU + 14 bytes of Ethernet, since the interface counters are L2) predicted all twelve
# observed packet counts with no mismatches. It gives largest-sample-in-one-frame figures of
# 1443.4 / 1409.9 / 1304.4, so the most constrained is FastDDS at 1304.
#
# 8 + 4 + 1280 = 1292, which leaves 12.4 bytes of margin on FastDDS. Still gated rather than
# trusted - wire_packets_per_sample must read 1.0 for all three at N0, or this size's
# cross-vendor comparison is void - because the numbers above are one rig and one set of library
# versions, and a vendor upgrade can move them.
uint64 send_ns        # sender's own monotonic send timestamp (CLOCK_MONOTONIC, ns)
uint32 seq            # per-Publisher monotonic sample counter, starts at 1
uint8[1280] payload   # fixed filler, unused by the harness's own logic
