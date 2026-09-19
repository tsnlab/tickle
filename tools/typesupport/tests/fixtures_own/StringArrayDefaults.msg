# Test-only fixture (not a real TickLE example) - array-of-string default values: a fixed
# array's default must supply exactly its declared length (mirrors ArrayDefaults.msg's own
# fixed_with_default, just with string elements), a variable array's default may supply fewer
# elements than its capacity (the rest is what _init()'s own memset(0) already left behind, same
# as a scalar variable array) and also sets its own _count.
string[2] fixed_with_default ["", "hello"]
string[<=4] bounded_with_default ["a", "bb"]
