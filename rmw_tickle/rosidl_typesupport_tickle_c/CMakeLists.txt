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

# Headers live under include/rosidl_typesupport_tickle_c/ (matching the package name, like
# rosidl_typesupport_introspection_c's own layout) - callers #include "rosidl_typesupport_tickle_c/
# identifier.h" *with* that subdirectory, so the -I root below is plain include/ (BUILD_INTERFACE)
# or plain "include" (INSTALL_INTERFACE, relative to this package's own install prefix) - matching
# rmw_tickle/rmw_tickle/CMakeLists.txt's own established, working convention exactly. NOT
# include/rosidl_typesupport_tickle_c as the -I root itself (that duplicates the subdirectory name
# with install(DIRECTORY include/ DESTINATION include) below, producing .../rosidl_typesupport_
# tickle_c/rosidl_typesupport_tickle_c/*.h - a real bug this comment exists because of).
add_library(rosidl_typesupport_tickle_c src/identifier.c)
target_include_directories(rosidl_typesupport_tickle_c PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  # Not $<BUILD_INTERFACE:...>-only: this package's own include/rosidl_typesupport_tickle_c/
  # message_type_support.h needs <tickle/tickle.h> (tt_DATA_ENCODE et al.), and unlike the include/
  # entries above, an *installed* consumer reached via find_package() only ever sees this target's
  # INSTALL_INTERFACE, never its BUILD_INTERFACE - TickLE has no installed package of its own to
  # find_package() instead (see TICKLE_ROOT's own comment above), so this needs to be unconditional
  # rather than interface-gated, same absolute path either way.
  "${TICKLE_ROOT}/include"
)

ament_export_include_directories("include")
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
  # include/ already contains its own rosidl_typesupport_tickle_c/ subdirectory (include/
  # rosidl_typesupport_tickle_c/identifier.h et al.) - DESTINATION include/rosidl_typesupport_
  # tickle_c here would duplicate it (install/.../include/rosidl_typesupport_tickle_c/rosidl_
  # typesupport_tickle_c/*.h), matching neither ament_export_include_directories() below nor what
  # message_type_support.h's own #include "rosidl_typesupport_tickle_c/identifier.h" expects.
  DIRECTORY include/
  DESTINATION include
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
  INCLUDES DESTINATION include
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
