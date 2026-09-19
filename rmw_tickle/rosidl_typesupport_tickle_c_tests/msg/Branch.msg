# A genuinely *cross-package* nested message field - rosidl_typesupport_tickle_c_tests_dep is a
# completely separate ament package from this one, unlike Simple.msg's own trivial scalar field.
# Exercises rosidl_typesupport_tickle_c_generate_interfaces.cmake's own cross-package -I wiring
# (rmw_tickle/PLAN.md's own Milestone for it) - a same-package reference (like test_msgs/
# Nested.msg's own BasicTypes field, Milestone 40) never reaches that code path at all.
int32 tag
rosidl_typesupport_tickle_c_tests_dep/Leaf leaf_value
