# Test-only fixture (not a real TickLE example) - M6's array default values: a fixed array's
# default must supply exactly its declared length (no wire length prefix to fall back to), a
# variable array's default may supply fewer elements than its capacity (the rest is what
# _init()'s own memset(0) already left behind) and also sets its own _count.
uint8[4] fixed_with_default [1, 2, 3, 4]
uint16[<=4] bounded_with_default [10, 20]
