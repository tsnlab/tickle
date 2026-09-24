# Test-only fixture (not a real TickLE example) - the simplest possible nested-message case: one
# field of another package's type (std_msgs/Header, which itself nests builtin_interfaces/Time -
# two levels of nesting from a single field), resolved from tests/fixtures_ros2/ via -I.
std_msgs/Header header
uint32 value
