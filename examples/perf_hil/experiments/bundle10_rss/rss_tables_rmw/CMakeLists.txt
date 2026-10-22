cmake_minimum_required(VERSION 3.8)
project(rss_tables_rmw)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(rclcpp REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Small.msg"
  "msg/Mid.msg"
  "msg/Large.msg"
  "srv/Echo.srv"
)
rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")

add_executable(rss_node src/rss_node.cpp)
target_link_libraries(rss_node "${cpp_typesupport_target}" rclcpp::rclcpp)

install(TARGETS rss_node DESTINATION lib/${PROJECT_NAME})

ament_package()
