cmake_minimum_required(VERSION 3.8)
project(tickle_rmw_test)

# Find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)

# Add executable
add_executable(tickle_test_node test_tickle_rmw.cpp)

# Link libraries
ament_target_dependencies(tickle_test_node
  rclcpp
  std_msgs
)

# Install
install(TARGETS tickle_test_node
  DESTINATION lib/${PROJECT_NAME}
)

# Export
ament_export_dependencies(
  rclcpp
  std_msgs
)

ament_package()
