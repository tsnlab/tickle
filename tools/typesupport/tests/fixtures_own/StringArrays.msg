# Test-only fixture (not a real TickLE example) - exercises the array-of-string wire shape
# (DESIGN.md's "Variable/Fixed arrays" rule composed with its own "Strings" rule, one string per
# element - see rmw_tickle/PLAN.md's Milestone entry for the design): a fixed-count array of
# plain unbounded strings (no length prefix - N is part of the wire format itself, mirrored by
# Arrays.msg's own fixed_bytes/fixed_ints for scalar elements), a ROS 2 upper-bounded variable
# array of strings (capacity_source "bounded"), and an unbounded variable array of strings
# pinned by an explicit `@capacity` annotation (capacity_source "annotation").
string[3] fixed_names
string[<=4] bounded_names
string[] tagged_names  # @capacity 5
