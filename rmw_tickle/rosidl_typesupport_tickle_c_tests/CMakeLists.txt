cmake_minimum_required(VERSION 3.8)
# C CXX, not just C (unlike rmw_tickle/rmw_tickle/CMakeLists.txt's own C-only project(), which
# never calls rosidl_generate_interfaces() at all): several of the *other* typesupport generators
# rosidl_generate_interfaces() below invokes (rosidl_typesupport_c, rosidl_typesupport_fastrtps_c/
# _cpp, rosidl_typesupport_introspection_cpp) generate .cpp sources regardless of this test
# package's own C-only test/test_dispatch.c - without CXX enabled, CMake can't determine a link
# language for any of their generated library targets at all ("Cannot determine link language"),
# unrelated to our own rosidl_typesupport_tickle_c extension (C-only, and unaffected).
project(rosidl_typesupport_tickle_c_tests C CXX)

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
# Pulling this in (and find_package()-ing it, directly or transitively, before calling
# rosidl_generate_interfaces() below) is what makes rosidl_generate_interfaces() below pick up
# rosidl_typesupport_tickle_c automatically - see its own CMakeLists.txt's ament_index_register_
# resource("rosidl_typesupport_c") and rmw_tickle/PLAN.md's Milestone 1(c).
find_package(rosidl_typesupport_tickle_c REQUIRED)
find_package(rosidl_runtime_c REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Simple.msg"
)

if(BUILD_TESTING)
  # A plain add_executable() run as a manual CI step (check-all.yml), not ament_add_test() - this
  # package exists solely to prove rmw_tickle/PLAN.md's Milestone 1(c) actually works end to end,
  # not as a long-lived part of the test suite that needs colcon test's full ament_cmake_test
  # machinery.
  add_executable(test_dispatch test/test_dispatch.c)
  target_link_libraries(test_dispatch
    "${PROJECT_NAME}__rosidl_generator_c"
    "${PROJECT_NAME}__rosidl_typesupport_c"
    rosidl_typesupport_tickle_c::rosidl_typesupport_tickle_c
    rosidl_runtime_c::rosidl_runtime_c
  )
endif()

ament_package()
