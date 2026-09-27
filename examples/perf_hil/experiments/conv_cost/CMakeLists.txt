cmake_minimum_required(VERSION 3.8)
project(conv_cost)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rosidl_typesupport_cpp REQUIRED)
find_package(rosidl_typesupport_tickle_c REQUIRED)
find_package(rosidl_typesupport_tickle_cpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)

add_executable(conv_cost conv_cost.cpp)
target_link_libraries(conv_cost
  rclcpp::rclcpp
  rosidl_typesupport_cpp::rosidl_typesupport_cpp
  rosidl_typesupport_tickle_c::rosidl_typesupport_tickle_c
  rosidl_typesupport_tickle_cpp::rosidl_typesupport_tickle_cpp
  ${sensor_msgs_TARGETS}
  ${std_msgs_TARGETS}
  ${CMAKE_DL_LIBS}
)

install(TARGETS conv_cost DESTINATION lib/${PROJECT_NAME})

ament_package()
