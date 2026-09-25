# The benchmark payload shape at P4, where TickLE fragments too (OPTIMIZATION_PLAN.md section 3).
# See p1/Bench.msg for why all four sizes share one message name.
#
# 2800 bytes exceeds one datagram for all three, so both sides of the comparison pay for
# fragmentation and the asymmetry of P3 is gone.
#
# **This size needs a second TickLE build**, `-Dtt_MAX_BUFFER_LENGTH=4096`, so that a 2828 byte
# datagram is allowed out at all; build.sh gives it its own install prefix because that flag sizes
# libtickle.a's own buffers, not just the example's. build.sh asserts the generated
# `BenchData_FITS_ONE_DATAGRAM` macro before compiling, which is (2800 <= tt_MAX_BUFFER_LENGTH):
# true under this shape's own 4096 build, false under the default 1472 one. So p4 built against
# the default prefix is a build failure rather than a silently wrong measurement - verified from
# both sides, the default prefix really does fail the assertion and the 4096 one really does pass.
#
# Note that "fits one datagram" here means TickLE will emit it as one UDP datagram; the IP layer
# still fragments it across two frames, which is the point of this size.
#
# One consequence, recorded so the memory figure is not over-read: tt_Node's tx/rx buffers are 2N
# each, so this build carries about 10 KB more node buffer than its siblings. Immaterial against a
# peak RSS in the megabytes, but the P4 memory figure is not strictly comparable to P1-P3's for
# TickLE.
#
# TickLE is expected to lose this size under loss (section 9), because a lost IP fragment costs it
# the whole datagram while RTPS retransmits one fragment. That is pre-registered, not a surprise.
#
# 8 + 4 + 2788 = 2800.
uint64 send_ns        # sender's own monotonic send timestamp (CLOCK_MONOTONIC, ns)
uint32 seq            # per-Publisher monotonic sample counter, starts at 1
uint8[2788] payload   # fixed filler, unused by the harness's own logic
