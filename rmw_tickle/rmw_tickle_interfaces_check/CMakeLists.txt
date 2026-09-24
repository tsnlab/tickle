cmake_minimum_required(VERSION 3.12)
project(rmw_tickle_interfaces_check C CXX)

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rmw REQUIRED)
find_package(rmw_implementation REQUIRED)
find_package(rcutils REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(std_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)

# Talks to the rmw API directly rather than through rclcpp: an rclcpp node also starts services
# (type description, parameters) whose interface packages would have to be built for TickLE too,
# which is not what this checks. Links rmw_implementation, which loads whichever rmw RMW_IMPLEMENTATION
# names at run time - check_ros2_interfaces.sh sets rmw_tickle and proves which library was loaded.
add_executable(interfaces_check src/interfaces_check.c)
target_link_libraries(interfaces_check
  rmw::rmw rmw_implementation::rmw_implementation rcutils::rcutils rosidl_runtime_c::rosidl_runtime_c
  ${std_msgs_TARGETS} ${builtin_interfaces_TARGETS})

# The other half: what a user actually runs. A default rclcpp::Node, which starts every service and
# publisher rclcpp starts on its own - see the file's own comment.
add_executable(default_node src/default_node.cpp)
target_link_libraries(default_node rclcpp::rclcpp ${std_msgs_TARGETS} ${sensor_msgs_TARGETS})

install(TARGETS interfaces_check default_node DESTINATION lib/${PROJECT_NAME})

# A ROS 2 action through rmw_tickle (check_ros2_interfaces.sh -A). Optional: example_interfaces is
# not part of every ROS 2 install, and a workspace built without it must still build the rest.
find_package(rclcpp_action QUIET)
find_package(example_interfaces QUIET)
if(rclcpp_action_FOUND AND example_interfaces_FOUND)
  add_executable(action_check src/action_check.cpp)
  target_link_libraries(action_check rclcpp::rclcpp rclcpp_action::rclcpp_action ${example_interfaces_TARGETS})
  install(TARGETS action_check DESTINATION lib/${PROJECT_NAME})
endif()

ament_package()
