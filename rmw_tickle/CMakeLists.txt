cmake_minimum_required(VERSION 3.8)
project(rmw_tickle)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find dependencies
find_package(ament_cmake REQUIRED)
find_package(rmw REQUIRED)
find_package(rcutils REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_interface REQUIRED)
find_package(rosidl_typesupport_c REQUIRED)
find_package(rosidl_typesupport_cpp REQUIRED)

# Include directories
include_directories(include ../include)

# Create rmw_tickle_c library
add_library(rmw_tickle SHARED
  src/rmw_tickle_c/rmw_init.c
  src/rmw_tickle_c/rmw_get_implementation_identifier.c
  src/rmw_tickle_c/rmw_get_serialization_format.c
  src/rmw_tickle_c/identifiers.c
)

# Link libraries
ament_target_dependencies(rmw_tickle
  rmw
  rcutils
  rosidl_runtime_c
  rosidl_typesupport_interface
  rosidl_typesupport_c
  rosidl_typesupport_cpp
)

# Link with pthread only (tickle library removed for now)
target_link_libraries(rmw_tickle
  pthread
)

# Install
install(TARGETS rmw_tickle
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY include/
  DESTINATION include
)

# Export
ament_export_include_directories(include)
ament_export_libraries(rmw_tickle)
ament_export_dependencies(
  rmw
  rcutils
  rosidl_runtime_c
  rosidl_typesupport_interface
  rosidl_typesupport_c
  rosidl_typesupport_cpp
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
