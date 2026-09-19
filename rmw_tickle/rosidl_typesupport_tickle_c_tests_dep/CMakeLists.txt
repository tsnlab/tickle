cmake_minimum_required(VERSION 3.8)
# C CXX, not just C - same reasoning as rosidl_typesupport_tickle_c_tests's own CMakeLists.txt:
# several of the *other* typesupport generators rosidl_generate_interfaces() below invokes
# generate .cpp sources regardless of this package having no C++ code of its own.
project(rosidl_typesupport_tickle_c_tests_dep C CXX)

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()

find_package(ament_cmake REQUIRED)
find_package(rosidl_cmake REQUIRED)
find_package(rosidl_generator_c REQUIRED)
find_package(rosidl_typesupport_c REQUIRED)
find_package(rosidl_typesupport_tickle_c REQUIRED)
find_package(rosidl_runtime_c REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Leaf.msg"
)

ament_package()
