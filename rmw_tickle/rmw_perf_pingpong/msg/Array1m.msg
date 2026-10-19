# Byte-identical field layout to apex performance_test's Array1m (performance_test/src/msg/Array1m.msg), the message
# docs/RESULTS.md's L4 cells I1/I1L publish, so large_rtt_tx/large_rtt_rx (the L4 round trip) put the same bytes on the
# wire as perf_test did. `time` carries the sender's creation (due) time and `id` the sequence id; the sender also keeps
# both locally and computes the round trip from its own clock alone.
byte[1048576] array
int64 time
uint64 id
