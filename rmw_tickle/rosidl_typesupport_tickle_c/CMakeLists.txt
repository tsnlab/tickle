cmake_minimum_required(VERSION 3.12)
project(rosidl_typesupport_tickle_c C)

# Optimised unless the caller chooses otherwise (2026-09-26). colcon passes no build type by default, and
# CMake's empty one adds no optimisation flags at all, so every rmw_tickle comparison until now ran this
# package unoptimised against rmw_cyclonedds_cpp and rmw_fastrtps_cpp release packages - the same fairness
# gap the TickLE harness had (examples/perf_hil/tickle/build.sh, TICKLE_CORE_BUILD). An explicit
# -DCMAKE_BUILD_TYPE still wins; this only replaces "nothing".
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default Release)" FORCE)
endif()

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_core REQUIRED)
find_package(ament_cmake_python REQUIRED)
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

# tt_MAX_BUFFER_LENGTH for everything built on this package: rmw_tickle, and every interface
# package whose typesupport it generates - one value, set here, exported through the extras file,
# so no two of them can be built for different datagrams. It defaults to 65507, the largest IPv4 UDP
# payload, left to the OS to fragment: the user's decision of 2026-09-24 ("rmw의 기본 최대 패킷
# 크기: UDP 한계에 맞춰서"). Core's own default stays 1472. A user may lower it; everything built
# on this package follows. Note that an existing build tree keeps whatever value its CMake cache
# already holds - pass -DTICKLE_MAX_BUFFER_LENGTH=... (or build afresh) to change it there.
# Changing it means rebuilding every interface package too - rmw_tickle refuses a type generated
# for a different value (rosidl_typesupport_tickle_c_message_callbacks_t.tickle_max_buffer_length).
set(TICKLE_MAX_BUFFER_LENGTH "65507" CACHE STRING
    "tt_MAX_BUFFER_LENGTH for rmw_tickle and every interface package generated with rosidl_typesupport_tickle_c")
if(NOT TICKLE_MAX_BUFFER_LENGTH MATCHES "^[0-9]+$" OR TICKLE_MAX_BUFFER_LENGTH LESS 64 OR
   TICKLE_MAX_BUFFER_LENGTH GREATER 65507)
  message(FATAL_ERROR "TICKLE_MAX_BUFFER_LENGTH='${TICKLE_MAX_BUFFER_LENGTH}': must be a number from 64 to 65507 "
                      "(65507 is the largest IPv4 UDP payload)")
endif()
message(STATUS "rosidl_typesupport_tickle_c: tt_MAX_BUFFER_LENGTH = ${TICKLE_MAX_BUFFER_LENGTH}")

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

# TickLE's shipped capacity defaults, one profile per tt_MAX_BUFFER_LENGTH (capacities/profile_<N>.tsv,
# from Plan's inventory of jazzy) - read at each interface package's configure time by
# rosidl_typesupport_tickle_c.capacity_profile; see its docstring for how one is chosen and how a
# user's own overrides go on top.
install(
  DIRECTORY capacities
  DESTINATION share/${PROJECT_NAME}
)

# The ROS 2 half of the generator (rosidl_typesupport_tickle_c/, the Python package): what
# cmake/rosidl_typesupport_tickle_c_generate_interfaces.cmake runs for every interface of every
# package that builds this typesupport. It lived in tools/typesupport until 2026-09-24, when the
# user decided every ROS-related part belongs in rmw_tickle; it builds on tools/typesupport's
# ROS-agnostic tickle_typesupport package, which still has to be pip-installed (see the extras
# file). Installed the way rosidl's own generators install theirs, so a dependent package's build
# environment already has it on PYTHONPATH.
ament_python_install_package(${PROJECT_NAME})

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
