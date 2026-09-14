# A large variable-length payload - the same shape as examples/linux/perf/Bulk.c's hand-written
# BulkData (see perf_client/perf_server's throughput benchmark), used here to exercise M2's array
# support end to end. `payload` deliberately carries neither a ROS 2 upper bound nor an explicit
# `@capacity` annotation, so its capacity comes from the lowest-priority rule in PLAN.md's
# resolution order: auto-derived from what's left of a single datagram once `seq` and the framing
# overhead are accounted for.
#
# The auto-generated codec below is not yet wire-compatible with examples/linux/perf/Bulk.c (that
# hand-written version reuses BulkData.size as the array's own length instead of carrying a
# separate wire count, to avoid storing the length twice) - reconciling the two, and replacing
# the hand-written version with this generated one, is M4/M5's job (see PLAN.md's milestone
# table), not M2's.
uint32 seq
uint8[] payload
