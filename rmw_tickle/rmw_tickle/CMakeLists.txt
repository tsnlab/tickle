cmake_minimum_required(VERSION 3.8)
project(rmw_tickle C)

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()

find_package(ament_cmake REQUIRED)
find_package(rcutils REQUIRED)
find_package(rmw REQUIRED)
find_package(rmw_implementation_cmake REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_tickle_c REQUIRED)
find_package(rosidl_typesupport_tickle_cpp REQUIRED)
find_package(Threads REQUIRED)

# TickLE itself isn't packaged as an ament/colcon package (it's a plain static-lib Makefile
# project - see the top-level README's "Build") - this package lives inside the tickle monorepo,
# two directories below the repo root, so reach its public headers relative to that instead of
# via find_package(tickle).
set(TICKLE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
set(TICKLE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

add_library(rmw_tickle
  src/rmw_init.c
  src/rmw_node.c
  src/rmw_typesupport.c
  src/rmw_publisher.c
  src/rmw_subscription.c
  src/rmw_serialize.c
  src/rmw_client.c
  src/rmw_service.c
  src/rmw_guard_condition.c
  src/rmw_wait_set.c
  src/rmw_graph.c
  src/rmw_qos.c
  src/rmw_event.c
  src/rmw_features.c
  # TickLE's own core (rmw_node.c's tt_Node_create()/_poll()/_interrupt()/_destroy() calls,
  # Milestone 2, are the first real use of it in this package - rmw_init.c only touched the
  # _tt_CONFIG global struct before this) - compiled straight in, same reason rosidl_typesupport_
  # tickle_c/cmake/rosidl_typesupport_tickle_c_generate_interfaces.cmake does the same for src/
  # encoding.c/log.c: no installed ament/colcon TickLE package exists to link against instead.
  # hal_linux.c only, not hal_freertos.c - rmw_tickle is a host-side (Linux) library, matching
  # every real rmw implementation.
  "${TICKLE_SRC_DIR}/tickle.c"
  "${TICKLE_SRC_DIR}/encoding.c"
  "${TICKLE_SRC_DIR}/log.c"
  "${TICKLE_SRC_DIR}/hal_linux.c"
)

target_include_directories(rmw_tickle PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${TICKLE_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:include>
)

# Modern imported-target style (matching how real rmw_cyclonedds_cpp/rmw_fastrtps_cpp already link
# against these same packages) rather than ament_target_dependencies() - some ROS 2 distros (e.g.
# "lyrical", found while provisioning rmw_tickle/PLAN.md's rmw-perf.yml benchmark rig) have removed
# that macro entirely, not just reorganized which find_package() call provides it. One single
# keyword-form call for everything, including Threads::Threads, sidesteps the plain-vs-keyword
# target_link_libraries() mixing restriction the old two-call split existed to work around.
target_link_libraries(rmw_tickle PUBLIC
  rcutils::rcutils
  rmw::rmw
  rosidl_runtime_c::rosidl_runtime_c
  rosidl_typesupport_tickle_c::rosidl_typesupport_tickle_c
  rosidl_typesupport_tickle_cpp::rosidl_typesupport_tickle_cpp
  Threads::Threads
)

ament_export_targets(export_rmw_tickle HAS_LIBRARY_TARGET)
ament_export_dependencies(
  rcutils rmw rosidl_runtime_c rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp)

# rmw_tickle/PLAN.md's Milestone 8: registers the "rmw_typesupport"/"rmw_typesupport_c"/
# "rmw_typesupport_cpp" ament_index resources that rmw_implementation_cmake's own get_available_
# rmw_implementations()/get_default_rmw_implementation() read - the actual mechanism behind
# `RMW_IMPLEMENTATION=rmw_tickle` selection, not just documentation. Both languages now (see
# rosidl_typesupport_tickle_cpp's own package.xml doc comment for why the "cpp" one had to be
# added: rosidl_typesupport_tickle_c alone is unreachable from any real rclcpp C++ node) - no
# introspection typesupport either way, matching this package's own "Supported subset" (PLAN.md).
register_rmw_implementation(
  "c:rosidl_typesupport_tickle_c"
  "cpp:rosidl_typesupport_tickle_cpp")

install(
  DIRECTORY include/
  DESTINATION include
)

install(
  TARGETS rmw_tickle
  EXPORT export_rmw_tickle
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

# rmw_tickle/PLAN.md's Milestone 10: a scoped test suite - only what "Supported subset" + the QoS
# roadmap actually cover, not full upstream rmw_implementation/test_rmw_implementation conformance
# (no live two-process publish/subscribe/service round trip test infra exists yet for rmw_tickle -
# every prior milestone's own verification depth stopped at "compile+link", and this milestone's
# own new tests stay within that same boundary: a real struct tt_Node is created in-process, but
# nothing here talks to a second node/peer - see PLAN.md's own note on this gap).
if(BUILD_TESTING)
  find_package(ament_cmake_test REQUIRED)

  foreach(_test_name test_qos test_node_lifecycle test_guard_condition_wait test_graph test_events test_liveliness_lost_watchdog test_rmw_api_surface test_multi_node test_names_and_types test_publish_take_reuse test_history_keep_all)
    add_executable(${_test_name} test/${_test_name}.c)
    target_link_libraries(${_test_name} rmw_tickle Threads::Threads)
    # GENERATE_RESULT_FOR_RETURN_CODE_ZERO: these are plain C executables (an assert()-per-check,
    # exit 0/nonzero), not something that emits its own JUnit result file - ament_add_test()'s own
    # run_test.py wrapper otherwise treats "no result file produced" as a failure regardless of
    # the command's own exit code.
    ament_add_test(${_test_name}
      COMMAND "$<TARGET_FILE:${_test_name}>"
      GENERATE_RESULT_FOR_RETURN_CODE_ZERO
    )
  endforeach()
endif()

ament_package()
