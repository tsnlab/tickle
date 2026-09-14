cmake_minimum_required(VERSION 3.8)
project(rmw_tickle C)

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()

find_package(ament_cmake REQUIRED)
find_package(rcutils REQUIRED)
find_package(rmw REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
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

ament_target_dependencies(rmw_tickle
  rcutils
  rmw
  rosidl_runtime_c
)

target_link_libraries(rmw_tickle PUBLIC Threads::Threads)

ament_export_targets(export_rmw_tickle HAS_LIBRARY_TARGET)
ament_export_dependencies(rcutils rmw rosidl_runtime_c)

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

ament_package()
