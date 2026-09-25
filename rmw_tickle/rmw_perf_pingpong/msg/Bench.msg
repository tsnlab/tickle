# rmw-layer counterpart of examples/perf_hil/idl/pN/Bench.idl - identical field shape on purpose
# (rmw_tickle/COMPARISON.MD's own "Design: rmw_perf_pingpong" section), so a later "how much does
# the rmw_tickle wrapper itself cost" question can compare this tool's own numbers against that
# native (no-rmw) one directly, on the same message, instead of needing a second design. The
# native side's small shape is p1.
#
# `send_ns` comes before `seq` since 2026-09-25, and must stay that way, for the reason the native
# files record: standard CDR aligns a 64-bit field to 8 while TickLE's CDR-4 caps alignment at 4,
# so with `seq` first the same source is 76 bytes on one wire and 80 on the other. That byte-
# identity is precisely what this file's own comment above claims, so reordering the native side
# without this one would have quietly falsified it.
uint64 send_ns      # sender's own monotonic send timestamp (CLOCK_MONOTONIC, ns)
uint32 seq          # per-Publisher monotonic sample counter, starts at 1
uint8[64] payload   # fixed filler, unused by this tool's own logic
