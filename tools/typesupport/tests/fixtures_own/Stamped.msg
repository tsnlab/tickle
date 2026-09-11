# Test-only fixture (not a real TickLE example) - the simplest possible nested-message case: one
# field of a type the generator resolves without any -I search path at all, via
# tickle_typesupport.builtins's fallback (std_msgs/Header, which itself nests
# builtin_interfaces/Time - two levels of nesting from a single field).
std_msgs/Header header
uint32 value
