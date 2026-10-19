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
  "msg/Array1k.msg"
  "msg/Struct16.msg"
  "msg/Array1m.msg"
  "msg/Array4m.msg"
  "msg/Ack.msg"
)
rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")

add_executable(ping_node src/ping_node.cpp)
target_link_libraries(ping_node "${cpp_typesupport_target}" rclcpp::rclcpp)

add_executable(pong_node src/pong_node.cpp)
target_link_libraries(pong_node "${cpp_typesupport_target}" rclcpp::rclcpp)

# The L4 round trip (examples/perf_hil/experiments/l4_rtt_rig.sh): a large message at a fixed rate one way, a small
# reply the other, the round trip on the sender's clock.
add_executable(large_rtt_tx src/large_rtt_tx.cpp)
target_link_libraries(large_rtt_tx "${cpp_typesupport_target}" rclcpp::rclcpp)

add_executable(large_rtt_rx src/large_rtt_rx.cpp)
target_link_libraries(large_rtt_rx "${cpp_typesupport_target}" rclcpp::rclcpp)

install(TARGETS ping_node pong_node large_rtt_tx large_rtt_rx
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
