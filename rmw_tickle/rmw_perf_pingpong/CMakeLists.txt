cmake_minimum_required(VERSION 3.8)
project(rmw_perf_pingpong)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(rclcpp REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Bench.msg"
)
rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")

add_executable(ping_node src/ping_node.cpp)
target_link_libraries(ping_node "${cpp_typesupport_target}" rclcpp::rclcpp)

add_executable(pong_node src/pong_node.cpp)
target_link_libraries(pong_node "${cpp_typesupport_target}" rclcpp::rclcpp)

install(TARGETS ping_node pong_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
