# rmw-layer counterpart of examples/perf_hil/idl/Bench.idl - identical field shape on purpose
# (rmw_tickle/comparison.md's own "Design: rmw_perf_pingpong" section), so a later "how much does
# the rmw_tickle wrapper itself cost" question can compare this tool's own numbers against that
# native (no-rmw) one directly, on the same message, instead of needing a second design.
uint32 seq          # per-Publisher monotonic sample counter, starts at 1
uint64 send_ns      # sender's own monotonic send timestamp (CLOCK_MONOTONIC, ns)
uint8[64] payload   # fixed filler, unused by this tool's own logic
