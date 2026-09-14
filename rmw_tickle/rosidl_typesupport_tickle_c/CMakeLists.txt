cmake_minimum_required(VERSION 3.12)
project(rosidl_typesupport_tickle_c C)

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_core REQUIRED)
find_package(rosidl_cmake REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_interface REQUIRED)

# TickLE itself isn't packaged as an ament/colcon package (it's a plain static-lib Makefile
# project - see the top-level README's "Build") - this package lives inside the tickle monorepo,
# two directories below the repo root, same as rmw_tickle/rmw_tickle/CMakeLists.txt, so reach its
# public headers the same way it does. Captured here (this package's own configure time, always
# inside the monorepo checkout) as an ABSOLUTE path and baked into rosidl_typesupport_tickle_c-
# extras.cmake.in below, because cmake/rosidl_typesupport_tickle_c_generate_interfaces.cmake runs
# inside an arbitrary *other* package's build (e.g. a real interface package like test_msgs),
# which could be anywhere in a colcon workspace - a relative path wouldn't resolve there.
get_filename_component(TICKLE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(rosidl_typesupport_tickle_c src/identifier.c)
target_include_directories(rosidl_typesupport_tickle_c PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${TICKLE_ROOT}/include>
  $<INSTALL_INTERFACE:include/rosidl_typesupport_tickle_c>
)

ament_export_include_directories("include/rosidl_typesupport_tickle_c")
ament_export_libraries(rosidl_typesupport_tickle_c)
ament_export_targets(export_rosidl_typesupport_tickle_c HAS_LIBRARY_TARGET)

# Registers this package as an available "rosidl_typesupport_c" implementation - get_used_
# typesupports() (rosidl_typesupport_c/cmake/get_used_typesupports.cmake) queries exactly this
# ament_index resource type to decide which typesupport identifiers to bake into each message's
# own dispatch table. Without this, rosidl_typesupport_tickle_c_generate_interfaces.cmake below
# could still generate correct code, but rmw_tickle would have no standard way to ever reach it
# from a real rmw_create_publisher() call - see ros2_adapter.py's own module docstring.
ament_index_register_resource("rosidl_typesupport_c")

install(
  DIRECTORY include/
  DESTINATION include/rosidl_typesupport_tickle_c
)

install(
  DIRECTORY cmake
  DESTINATION share/${PROJECT_NAME}
)

install(
  TARGETS rosidl_typesupport_tickle_c
  EXPORT export_rosidl_typesupport_tickle_c
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include/rosidl_typesupport_tickle_c
)

if(BUILD_TESTING)
  find_package(ament_lint_auto QUIET)
  if(ament_lint_auto_FOUND)
    ament_lint_auto_find_test_dependencies()
  endif()
endif()

ament_package(
  CONFIG_EXTRAS "rosidl_typesupport_tickle_c-extras.cmake.in"
)
