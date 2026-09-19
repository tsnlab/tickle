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
# rosidl_typesupport_tickle_c_tests (the package that nests this one's own Leaf.msg) also
# generates C++ typesupport for its own messages - its own generated Branch struct #includes
# Leaf's own C++ struct header directly (ROS 2's own rosidl_generator_cpp convention, unrelated
# to anything TickLE-specific), so this package needs the identical generator set too - found
# the hard way, in real CI, when it didn't.
find_package(rosidl_generator_cpp REQUIRED)
find_package(rosidl_typesupport_cpp REQUIRED)
find_package(rosidl_typesupport_tickle_cpp REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Leaf.msg"
)

ament_package()
