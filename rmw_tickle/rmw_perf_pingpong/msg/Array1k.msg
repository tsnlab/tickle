# Byte-identical field layout to buildfarm_perf_tests' own performance_test/msg/Array1k.msg - a
# deliberate copy, not a package dependency (that upstream package pulls in its own build-only
# Boost/Java/Maven toolchain for its own reporting tooling, none of which this tool needs just to
# reuse two small message shapes) - so this tool's own Array1k numbers stay directly comparable
# against COMPARISON.MD's own existing "Performance comparison" section (same wire layout, same
# CDR size) without inheriting that weight. `time`/`id` (not this package's own Bench.msg
# `send_ns`/`seq` naming) match performance_test's own field names exactly, for the same reason.
byte[1024] array
int64 time
uint64 id
