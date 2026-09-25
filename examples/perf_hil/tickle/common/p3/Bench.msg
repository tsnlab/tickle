# The benchmark payload shape at P3, the deliberately asymmetric size (OPTIMIZATION_PLAN.md
# section 3). See p1/Bench.msg for why all four sizes share one message name.
#
# 1440 bytes fills one TickLE datagram (28 + 1440 = 1468 of 1472, four bytes to spare) and forces
# both DDS vendors to split, since RTPS framing puts them at ~1500-1516. That asymmetry is the
# point of this size - it is one of the user's four cases - but it makes this size a boundary
# measurement and nothing more.
#
# **A "TickLE wins at large messages" claim must not be read off P3.** Under loss a two-fragment
# datagram is lost about 9.75% of the time where a one-fragment datagram is lost 5%, so TickLE
# would show roughly half the effective sample loss here from the payload size alone. P2 is the
# like-for-like large size. What P3 measures is the cost of the boundary itself, which is a real
# property worth a number - not a protocol win.
#
# 8 + 4 + 1428 = 1440.
uint64 send_ns        # sender's own monotonic send timestamp (CLOCK_MONOTONIC, ns)
uint32 seq            # per-Publisher monotonic sample counter, starts at 1
uint8[1428] payload   # fixed filler, unused by the harness's own logic
