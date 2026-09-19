# Test-only fixture (not a real TickLE example) - exercises the array-of-nested-message-type
# wire shape (DESIGN.md's "Variable/Fixed arrays" rule composed with its own "Nested messages"
# rule - see rmw_tickle/PLAN.md's Milestone entry for the design): a fixed-count array of a
# nested type deliberately shaped to need a real inter-element alignment gap (OddAlign.msg, see
# its own comment), a ROS 2 upper-bounded variable array of the same nested type, and an
# unbounded variable array pinned by an explicit `@capacity` annotation.
OddAlign[2] fixed_items
OddAlign[<=3] bounded_items
OddAlign[] tagged_items  # @capacity 4
