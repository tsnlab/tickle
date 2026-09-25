# The benchmark payload shape at P3, the deliberately asymmetric size (OPTIMIZATION_PLAN.md
# section 3). See p1/Bench.msg for why all four sizes share one message name.
#
# 1424 bytes fits one TickLE frame and forces both DDS vendors to split. That asymmetry is the
# point of this size - it is one of the user's four cases - but it makes this size a boundary
# measurement and nothing more.
#
# The window is narrow and both edges are measured (see p2/Bench.msg for the model and its
# validation): a sample fits one frame up to 1443.4 on TickLE and 1409.9 on CycloneDDS, so this
# size has to land between them. It was 1440, which worked, but sat 3.4 bytes below TickLE's own
# limit - so any future growth in TickLE's 28-byte DATA framing would have flipped the cell
# silently, and it would have read as a TickLE regression on packet count rather than as a test
# that had lost its premise. 1424 splits the window instead: 19.4 bytes of room below TickLE's
# limit and 14.1 above CycloneDDS's. check_bench_shapes.sh gates both margins.
#
# **A "TickLE wins at large messages" claim must not be read off P3.** Under loss a two-fragment
# datagram is lost about 9.75% of the time where a one-fragment datagram is lost 5%, so TickLE
# would show roughly half the effective sample loss here from the payload size alone. P2 is the
# like-for-like large size. What P3 measures is the cost of the boundary itself, which is a real
# property worth a number - not a protocol win.
#
# 8 + 4 + 1412 = 1424.
uint64 send_ns        # sender's own monotonic send timestamp (CLOCK_MONOTONIC, ns)
uint32 seq            # per-Publisher monotonic sample counter, starts at 1
uint8[1412] payload   # fixed filler, unused by the harness's own logic
