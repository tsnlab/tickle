# ros2_adapter offline test fixtures

Hand-written stand-ins for what a real ROS 2 install's `rosidl_generator_c`/`rosidl_runtime_c`
would generate/provide for `tests/fixtures_own/Arrays.msg` and `.../BoundedString.msg` - used by
`../test_ros2_adapter.py` to compile and round-trip `tickle_typesupport.ros2_adapter`'s generated
converter code entirely offline. This tool's own dev/test environment has no ROS 2 install at all
(see `ros2_adapter.py`'s own module docstring), so these exist specifically to let the *generator
logic* (field-by-field conversion, bounds checking) be regression-tested on every commit, without
waiting on a real `colcon build` (which only `rmw_tickle`'s own CI job, via `ros-tooling/
setup-ros`, currently exercises).

**Not vendored or real** - unlike `../fixtures_ros2/`'s real, unmodified `.msg` files, everything
here is written by hand against this tool's own recollection of rosidl_runtime_c's public,
long-stable API shape (`rosidl_runtime_c__String`, `rosidl_runtime_c__<T>__Sequence`) and the
`rosidl_generator_c` struct-naming/header-path convention (`pkg__msg__Type` / `pkg/msg/type.h`).
Treat a mismatch against a *real* ROS 2 install as this fixture being wrong, not the other way
around - it exists to catch regressions in `ros2_adapter.py`'s own logic between real ROS 2
verifications, not to define the contract.

| File | Stands in for |
|---|---|
| `rosidl_runtime_c/string.h` | `rosidl_runtime_c/string.h` |
| `rosidl_runtime_c/string_functions.h` | `rosidl_runtime_c/string_functions.h` (`__assign()`, plus `String__Sequence`/`__init()` for rmw_tickle/PLAN.md's Milestone 41 array-of-string conversion) |
| `rosidl_runtime_c/primitives_sequence_functions.h` | same header, just the two element types (`uint16`, `float32`) `Arrays.msg` needs |
| `test_msgs/msg/arrays.h` | what `rosidl_generator_c` would generate for `Arrays.msg` |
| `test_msgs/msg/bounded_string.h` | same, for `BoundedString.msg` |
| `test_msgs/msg/string_arrays.h` | same, for `StringArrays.msg` (Milestone 41's own array-of-string conversion) |
| `test_msgs/msg/leaf.h` / `test_msgs/msg/branch.h` | same, for `Leaf.msg`/`Branch.msg` (Milestone 38's own nested-message resolver test) |
