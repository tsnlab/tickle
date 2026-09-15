cmake_minimum_required(VERSION 3.12)
project(rosidl_typesupport_tickle_cpp C)

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_core REQUIRED)
find_package(rosidl_cmake REQUIRED)

# Just the identifier string (see include/rosidl_typesupport_tickle_cpp/identifier.h's own doc
# comment) - plain C is enough, same as rosidl_typesupport_tickle_c's own identifier.c. The actual
# per-message dispatch shims (cmake/rosidl_typesupport_tickle_cpp_generate_interfaces.cmake) are
# C++ libraries built inside *other* packages, not here - this package itself needs no C++ at all.
add_library(rosidl_typesupport_tickle_cpp src/identifier.c)
target_include_directories(rosidl_typesupport_tickle_cpp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

ament_export_include_directories("include")
ament_export_libraries(rosidl_typesupport_tickle_cpp)
ament_export_targets(export_rosidl_typesupport_tickle_cpp HAS_LIBRARY_TARGET)

# Registers this package as an available "rosidl_typesupport_cpp" implementation -
# rosidl_typesupport_cpp's own get_used_typesupports(typesupports "rosidl_typesupport_cpp")
# (queried by its idl__type_support.cpp.em template) reads exactly this ament_index resource type
# to decide which identifiers to bake into each message's *top-level* C++ dispatch table -
# rmw_tickle/PLAN.md's own doc comment on this package explains why this, not rosidl_typesupport_c
# a second time, is the piece that was actually missing.
ament_index_register_resource("rosidl_typesupport_cpp")

install(
  DIRECTORY include/
  DESTINATION include
)

install(
  DIRECTORY cmake resource
  DESTINATION share/${PROJECT_NAME}
)

install(
  TARGETS rosidl_typesupport_tickle_cpp
  EXPORT export_rosidl_typesupport_tickle_cpp
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
  CONFIG_EXTRAS "rosidl_typesupport_tickle_cpp-extras.cmake.in"
)
