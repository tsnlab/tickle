# Test-only fixture (not a real TickLE example) - exercises every array shape M2 adds in one
# message: a fixed array of 1-byte elements (the memcpy fast path), a fixed array of a wider
# scalar (the per-element byte-swap loop), a ROS 2 upper-bounded variable array (capacity_source
# "bounded"), and an unbounded variable array pinned by an explicit `@capacity` annotation
# (capacity_source "annotation") whose element type is a float, to cover the union-based bit
# reinterpretation path for arrays too.
uint8[4] fixed_bytes
int32[3] fixed_ints
uint16[<=8] bounded_values
float32[] samples  # @capacity 16
