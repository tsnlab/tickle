cmake_minimum_required(VERSION 3.8)
project(rmw_tickle)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find dependencies
find_package(ament_cmake REQUIRED)
find_package(ament_cmake_ros REQUIRED)
find_package(rmw REQUIRED)
find_package(rcutils REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_introspection_c REQUIRED)

# if(NOT DEFINED ENV{TICKLE_PATH} OR "$ENV{TICKLE_PATH}" STREQUAL "")
#   message(FATAL_ERROR "Environment variable TICKLE_PATH is not set or is empty.")
# endif()

# Include directories
include_directories(include)

# Create rmw_tickle_c library
add_library(rmw_tickle SHARED
  src/rmw_tickle_c/rmw_init.c
  src/rmw_tickle_c/rmw_get_implementation_identifier.c
  src/rmw_tickle_c/rmw_get_serialization_format.c
  src/rmw_tickle_c/identifiers.c
  src/rmw_tickle_c/rmw_features.c
  src/rmw_tickle_c/rmw_node.c
  src/rmw_tickle_c/rmw_event.c
  src/rmw_tickle_c/rmw_qos.c
  src/rmw_tickle_c/rmw_count.c
  src/rmw_tickle_c/rmw_gid.c
  src/rmw_tickle_c/rmw_service.c
  src/rmw_tickle_c/rmw_logging.c
  src/rmw_tickle_c/rmw_topic_info.c
  src/rmw_tickle_c/rmw_qos_check.c
  src/rmw_tickle_c/rmw_network_flow.c
  src/rmw_tickle_c/rmw_publisher.c
  src/rmw_tickle_c/rmw_subscription.c
  src/rmw_tickle_c/rmw_client.c
  src/rmw_tickle_c/rmw_serialize.c
  src/rmw_tickle_c/rmw_guard_condition.c
  src/rmw_tickle_c/rmw_wait_set.c
  src/rmw_tickle_c/rmw_event_handling.c
  src/rmw_tickle_c/rmw_node_names.c
  src/rmw_tickle_c/rmw_topic_names_and_types.c
  src/rmw_tickle_c/rmw_publisher_names_and_types.c
  src/rmw_tickle_c/rmw_service_names_and_types_by_node.c
  src/rmw_tickle_c/rmw_event.c
  src/rmw_tickle_c/rmw_dynamic_message.c
  src/rmw_tickle_c/rmw_serialization_support.c
  src/rmw_tickle_c/rmw_callback.c
  src/rmw_tickle_c/__TEMP__messages.c
  src/tickle/encoding.c
  src/tickle/hal_zephyr.c
  src/tickle/log.c
  src/tickle/tickle.c
)

# Link libraries
ament_target_dependencies(rmw_tickle
  rmw
  rcutils
  rosidl_runtime_c
  rosidl_typesupport_introspection_c
)

# Set include directories for the target
# target_include_directories(rmw_tickle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tickle/include)

# Link with pthread and tickle library
target_link_libraries(rmw_tickle
  pthread
  # $ENV{TICKLE_PATH}/libtickle.a
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
)

register_rmw_implementation("c:rosidl_typesupport_introspection_c")

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
