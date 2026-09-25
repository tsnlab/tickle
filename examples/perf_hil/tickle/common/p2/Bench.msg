# The benchmark payload shape at P2, the like-for-like large size (OPTIMIZATION_PLAN.md section 3).
#
# See p1/Bench.msg for why all four sizes share one message name and why `send_ns` comes first.
#
# 1388 bytes is sized to the *most constrained* framing of the three, not to TickLE's, so that all
# three send exactly one datagram and the comparison at this size is like for like. RTPS costs 60
# bytes before the CDR body (header 20 + INFO_TS 12 + DATA submessage 24 + encapsulation 4), or 76
# with an INFO_DST: 1472 - 76 = 1396, less 8 bytes of headroom, gives 1388. TickLE's own DATA
# framing is 28 bytes (DESIGN.md), so it uses 1416 of its 1472 here - comfortably inside.
#
# The number is spec arithmetic, not a measurement, so it is checked rather than trusted:
# wire_packets_per_sample must read 1.0 for all three at N0, or this size's cross-vendor comparison
# is void (OPTIMIZATION_PLAN.md section 9).
#
# 8 + 4 + 1376 = 1388.
uint64 send_ns        # sender's own monotonic send timestamp (CLOCK_MONOTONIC, ns)
uint32 seq            # per-Publisher monotonic sample counter, starts at 1
uint8[1376] payload   # fixed filler, unused by the harness's own logic
