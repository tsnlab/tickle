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
# Not rosidl_default_generators (what a real interface package normally depends on) - see this
# package's own package.xml for why. rosidl_cmake provides the rosidl_generate_interfaces() macro
# itself (rosidl_default_generators only pulls it in transitively - dropping that means finding it
# directly). rosidl_generator_c is what produces the message struct this package's own
# test_dispatch.c includes; rosidl_typesupport_c is ROSIDL_GET_MSG_TYPE_SUPPORT()'s own standard
# dispatch entry point.
find_package(rosidl_cmake REQUIRED)
find_package(rosidl_generator_c REQUIRED)
find_package(rosidl_typesupport_c REQUIRED)
# Pulling this in (and find_package()-ing it, directly or transitively, before calling
# rosidl_generate_interfaces() below) is what makes rosidl_generate_interfaces() below pick up
# rosidl_typesupport_tickle_c automatically - see its own CMakeLists.txt's ament_index_register_
# resource("rosidl_typesupport_c") and rmw_tickle/PLAN.md's Milestone 1(c).
find_package(rosidl_typesupport_tickle_c REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
# Same reasoning as rosidl_typesupport_tickle_c above, one dispatch level up - pulling these in
# before rosidl_generate_interfaces() is what makes it also generate this message's C++-level
# dispatch entry (rosidl_typesupport_cpp) with rosidl_typesupport_tickle_cpp as one of its
# candidates, and rosidl_typesupport_tickle_cpp's own per-message wrapper on top of that - see
# rosidl_typesupport_tickle_cpp's own package.xml doc comment.
find_package(rosidl_generator_cpp REQUIRED)
find_package(rosidl_typesupport_cpp REQUIRED)
find_package(rosidl_typesupport_tickle_cpp REQUIRED)
# msg/Branch.msg's own cross-package nested field - find_package()-ing the dependency before
# rosidl_generate_interfaces() (and naming it in DEPENDENCIES below) is what makes rosidl_
# typesupport_tickle_c_generate_interfaces.cmake's own DEPENDENCY_PACKAGE_NAMES loop (the -I/
# include-dir/link-library wiring this whole package exists to exercise) see it at all.
find_package(rosidl_typesupport_tickle_c_tests_dep REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Simple.msg"
  "msg/Branch.msg"
  "srv/SimpleService.srv"
  DEPENDENCIES rosidl_typesupport_tickle_c_tests_dep
)

if(BUILD_TESTING)
  # A plain add_executable() run as a manual CI step (check-all.yml), not ament_add_test() - this
  # package exists solely to prove rmw_tickle/PLAN.md's Milestone 1(c)/4 actually work end to end,
  # not as a long-lived part of the test suite that needs colcon test's full ament_cmake_test
  # machinery.
  add_executable(test_dispatch test/test_dispatch.c)
  add_executable(test_dispatch_srv test/test_dispatch_srv.c)
  add_executable(test_dispatch_nested test/test_dispatch_nested.c)
  foreach(_test_target test_dispatch test_dispatch_srv test_dispatch_nested)
    target_link_libraries(${_test_target}
      "${PROJECT_NAME}__rosidl_generator_c"
      "${PROJECT_NAME}__rosidl_typesupport_c"
      rosidl_typesupport_tickle_c::rosidl_typesupport_tickle_c
      rosidl_runtime_c::rosidl_runtime_c
    )
  endforeach()
  # No extra target_link_libraries() for test_dispatch_nested's own cross-package Leaf struct:
  # "${PROJECT_NAME}__rosidl_generator_c" above already PUBLIC-links
  # rosidl_typesupport_tickle_c_tests_dep's own __rosidl_generator_c target (rosidl_generator_c_
  # generate_interfaces.cmake's own DEPENDENCY_PACKAGE_NAMES loop, confirmed by reading it
  # directly), and "${PROJECT_NAME}__rosidl_typesupport_c" likewise PUBLIC-links its own
  # __rosidl_typesupport_tickle_c dependency (this same milestone's own new wiring) - both
  # propagate transitively to every executable linking this package's own targets already.

  # See test/test_dispatch_cpp.cpp's own doc comment for why this is a separate executable from
  # test_dispatch above, not just an addition to it.
  add_executable(test_dispatch_cpp test/test_dispatch_cpp.cpp)
  target_link_libraries(test_dispatch_cpp
    "${PROJECT_NAME}__rosidl_generator_c"
    "${PROJECT_NAME}__rosidl_generator_cpp"
    "${PROJECT_NAME}__rosidl_typesupport_cpp"
    rosidl_typesupport_tickle_c::rosidl_typesupport_tickle_c
    rosidl_typesupport_tickle_cpp::rosidl_typesupport_tickle_cpp
    rosidl_runtime_c::rosidl_runtime_c
  )
  set_target_properties(test_dispatch_cpp PROPERTIES CXX_STANDARD 17)

  # See test/test_dispatch_srv_cpp.cpp's own doc comment - the .srv counterpart to test_dispatch_
  # cpp.cpp above, one dispatch level up from test_dispatch_srv.c's own C-level entry point.
  add_executable(test_dispatch_srv_cpp test/test_dispatch_srv_cpp.cpp)
  target_link_libraries(test_dispatch_srv_cpp
    "${PROJECT_NAME}__rosidl_generator_c"
    "${PROJECT_NAME}__rosidl_generator_cpp"
    "${PROJECT_NAME}__rosidl_typesupport_cpp"
    rosidl_typesupport_tickle_c::rosidl_typesupport_tickle_c
    rosidl_typesupport_tickle_cpp::rosidl_typesupport_tickle_cpp
    rosidl_runtime_c::rosidl_runtime_c
  )
  set_target_properties(test_dispatch_srv_cpp PROPERTIES CXX_STANDARD 17)
endif()

ament_package()
