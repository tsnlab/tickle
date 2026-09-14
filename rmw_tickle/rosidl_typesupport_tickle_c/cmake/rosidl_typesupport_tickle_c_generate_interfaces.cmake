# Executed by ament_execute_extensions("rosidl_generate_idl_interfaces") - registered via
# ament_register_extension() in ../rosidl_typesupport_tickle_c-extras.cmake.in, run once for
# every ROS 2 interface package that calls rosidl_generate_interfaces() and has (directly or
# transitively) find_package()-d rosidl_typesupport_tickle_c first. Runs in that CALLING
# package's own CMake scope (PROJECT_NAME, CMAKE_CURRENT_SOURCE_DIR/_BINARY_DIR below are all
# THAT package's, e.g. a real interface package like test_msgs - not this one).
#
# rmw_tickle/PLAN.md's Milestone 1(b)/(c) first cut: messages only (a .srv interface is skipped
# with a warning below - .srv support follows once this is proven end-to-end in CI), and no
# resolution of a message dependency's own nested type from another package (rosidl_generate_
# interfaces_DEPENDENCY_PACKAGE_NAMES, below, is read only to satisfy that guard - not walked the
# way rosidl_generator_c_generate_interfaces.cmake's own version is).

if(NOT TARGET ${rosidl_generate_interfaces_TARGET}__rosidl_generator_c)
  message(FATAL_ERROR
    "The 'rosidl_generator_c' extension must be executed before the "
    "'rosidl_typesupport_tickle_c' extension.")
endif()

find_package(rosidl_cmake REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_interface REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_generator_output_path "${CMAKE_CURRENT_BINARY_DIR}/rosidl_generator_c")
set(_output_path "${CMAKE_CURRENT_BINARY_DIR}/rosidl_typesupport_tickle_c/${PROJECT_NAME}")
set(_generated_sources "")

foreach(_abs_idl_file ${rosidl_generate_interfaces_ABS_IDL_FILES})
  # rosidl_generate_interfaces_ABS_IDL_FILES holds *adapted* .idl paths (rosidl_adapt_interfaces()
  # runs before any extension, including this one, so every generator only ever has to understand
  # one input format) - NOT the original .msg this package's own author actually wrote. tickle_
  # typesupport has no OMG IDL parser (only tickle_typesupport._rosidl_parser, the .msg/.srv line
  # format), so reconstruct the original source path instead of parsing this one: rosidl_adapter
  # mirrors the original relative layout (msg/<Name>.msg -> .../msg/<Name>.idl), so the parent
  # folder name and stem below are the same regardless of which one this path actually is.
  get_filename_component(_parent_folder "${_abs_idl_file}" DIRECTORY)
  get_filename_component(_parent_folder "${_parent_folder}" NAME)
  get_filename_component(_idl_name "${_abs_idl_file}" NAME_WE)
  set(_msg_file "${CMAKE_CURRENT_SOURCE_DIR}/${_parent_folder}/${_idl_name}.msg")

  if(NOT "${_parent_folder}" STREQUAL "msg" OR NOT EXISTS "${_msg_file}")
    message(WARNING
      "rosidl_typesupport_tickle_c: skipping '${_abs_idl_file}' - only .msg is supported so far "
      "(rmw_tickle/PLAN.md's Milestone 1(b)/(c))")
    continue()
  endif()

  set(_msg_output_dir "${_output_path}/${_parent_folder}")
  set(_ros_name "${PROJECT_NAME}__${_parent_folder}__${_idl_name}")
  set(_out_h "${_msg_output_dir}/${_idl_name}.h")
  set(_out_c "${_msg_output_dir}/${_idl_name}.c")
  set(_adapter_h "${_msg_output_dir}/${_ros_name}__rosidl_typesupport_tickle_c.h")
  set(_adapter_c "${_msg_output_dir}/${_ros_name}__rosidl_typesupport_tickle_c.c")
  set(_ts_c "${_msg_output_dir}/${_ros_name}__type_support.c")

  add_custom_command(
    OUTPUT "${_out_h}" "${_out_c}" "${_adapter_h}" "${_adapter_c}" "${_ts_c}"
    COMMAND Python3::Interpreter
    ARGS -m "${rosidl_typesupport_tickle_c_PYTHON_MODULE}"
      --package "${PROJECT_NAME}"
      --subfolder "${_parent_folder}"
      --name "${_idl_name}"
      --input "${_msg_file}"
      --outdir "${_msg_output_dir}"
    DEPENDS "${_msg_file}"
    COMMENT "Generating TickLE type support for ${_idl_name}"
    VERBATIM
  )
  list(APPEND _generated_sources "${_out_c}" "${_adapter_c}" "${_ts_c}")
endforeach()

if(NOT _generated_sources)
  return()
endif()

set(_target_suffix "__rosidl_typesupport_tickle_c")
add_library(${rosidl_generate_interfaces_TARGET}${_target_suffix} ${_generated_sources}
  # TickLE's own generated codec (in ${_generated_sources} above) calls into its shared CDR-4
  # runtime helpers - compiled straight into this per-interface-package library, same as tools/
  # typesupport/tests/test_ros2_adapter.py's own offline round-trip test already does, since
  # TickLE has no installed ament/colcon package of its own to link against instead (see this
  # package's top-level CMakeLists.txt).
  "${rosidl_typesupport_tickle_c_TICKLE_ROOT}/src/encoding.c"
  "${rosidl_typesupport_tickle_c_TICKLE_ROOT}/src/log.c"
)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
  set_target_properties(${rosidl_generate_interfaces_TARGET}${_target_suffix} PROPERTIES
    C_STANDARD 11)
endif()

target_include_directories(${rosidl_generate_interfaces_TARGET}${_target_suffix} PRIVATE
  "${_output_path}/msg"
  "${_generator_output_path}"
  "${rosidl_typesupport_tickle_c_TICKLE_ROOT}/include"
)

target_link_libraries(${rosidl_generate_interfaces_TARGET}${_target_suffix} PUBLIC
  ${rosidl_generate_interfaces_TARGET}__rosidl_generator_c)
target_link_libraries(${rosidl_generate_interfaces_TARGET}${_target_suffix} PRIVATE
  rosidl_typesupport_tickle_c::rosidl_typesupport_tickle_c
  rosidl_runtime_c::rosidl_runtime_c
  rosidl_typesupport_interface::rosidl_typesupport_interface)

add_dependencies(
  ${rosidl_generate_interfaces_TARGET}
  ${rosidl_generate_interfaces_TARGET}${_target_suffix}
)

if(NOT rosidl_generate_interfaces_SKIP_INSTALL)
  install(
    TARGETS ${rosidl_generate_interfaces_TARGET}${_target_suffix}
    EXPORT export_${rosidl_generate_interfaces_TARGET}${_target_suffix}
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
  )

  ament_export_targets(export_${rosidl_generate_interfaces_TARGET}${_target_suffix})
  rosidl_export_typesupport_targets(${_target_suffix}
    ${rosidl_generate_interfaces_TARGET}${_target_suffix})

  ament_export_dependencies(
    "rosidl_typesupport_tickle_c"
    "rosidl_runtime_c"
    "rosidl_typesupport_interface")
endif()
