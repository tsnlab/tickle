# The benchmark payload shape at P1, the small size (OPTIMIZATION_PLAN.md section 3).
#
# One of four: common/p1..p4/Bench.msg. All four are named Bench.msg and generate the same
# `struct BenchData` / `BenchTopic` / `BenchData_*` API, so the harness sources compile unchanged
# at every size and the size is chosen purely by include path (build.sh's second argument). Four
# differently-named types would have put a `#if` ladder over six function names into every
# client.c and server.c, and the harness would then no longer be the same code at the four sizes -
# which is the property this whole comparison rests on.
#
# Field order is `send_ns` before `seq` deliberately, and must stay that way. TickLE's CDR-4 caps
# alignment at 4, standard CDR aligns a 64-bit field to 8; with `seq` first the DDS pair inserts 4
# bytes of padding that TickLE does not, so the same source produced 76 bytes here and 80 on the
# wire there. `send_ns` first is 8 + 4 + N under both rules, with no padding under either.
#
# 8 + 4 + 64 = 76 bytes, which is what every figure published in COMPARISON.MD so far was measured
# at, and what the generated `_Static_assert` pins.
#
# Replaces a hand-written codec (deleted 2026-09-25, the user's decision): the benchmark was
# measuring an encoder no user of TickLE has, and it was the only side of a three-way byte-for-byte
# comparison able to drift silently - both DDS vendors generate from one shared idl/pN/Bench.idl by
# construction. tools/scripts/check_bench_shapes.sh now checks the .msg against the .idl.
uint64 send_ns      # sender's own monotonic send timestamp (CLOCK_MONOTONIC, ns)
uint32 seq          # per-Publisher monotonic sample counter, starts at 1
uint8[64] payload   # fixed filler, unused by the harness's own logic
