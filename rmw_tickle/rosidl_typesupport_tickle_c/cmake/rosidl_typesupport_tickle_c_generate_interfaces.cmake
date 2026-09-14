# Executed by ament_execute_extensions("rosidl_generate_idl_interfaces") - registered via
# ament_register_extension() in ../rosidl_typesupport_tickle_c-extras.cmake.in, run once for
# every ROS 2 interface package that calls rosidl_generate_interfaces() and has (directly or
# transitively) find_package()-d rosidl_typesupport_tickle_c first. Runs in that CALLING
# package's own CMake scope (PROJECT_NAME, CMAKE_CURRENT_SOURCE_DIR/_BINARY_DIR below are all
# THAT package's, e.g. a real interface package like test_msgs - not this one).
#
# .msg and .srv only (an .action interface, or any other subfolder, is skipped with a warning
# below) - no resolution of a message dependency's own nested type from another package (rosidl_
# generate_interfaces_DEPENDENCY_PACKAGE_NAMES, below, is read only to satisfy the generator-
# ordering guard just below - not walked the way rosidl_generator_c_generate_interfaces.cmake's
# own version is).

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

  set(_src_file "")
  if("${_parent_folder}" STREQUAL "msg")
    set(_src_file "${CMAKE_CURRENT_SOURCE_DIR}/msg/${_idl_name}.msg")
  elseif("${_parent_folder}" STREQUAL "srv")
    set(_src_file "${CMAKE_CURRENT_SOURCE_DIR}/srv/${_idl_name}.srv")
  endif()

  if("${_src_file}" STREQUAL "" OR NOT EXISTS "${_src_file}")
    message(WARNING
      "rosidl_typesupport_tickle_c: skipping '${_abs_idl_file}' - only .msg/.srv are supported "
      "(rmw_tickle/PLAN.md's Milestone 1)")
    continue()
  endif()

  set(_msg_output_dir "${_output_path}/${_parent_folder}")
  set(_ros_name "${PROJECT_NAME}__${_parent_folder}__${_idl_name}")
  set(_out_h "${_msg_output_dir}/${_idl_name}.h")
  set(_out_c "${_msg_output_dir}/${_idl_name}.c")

  set(_outputs "${_out_h}" "${_out_c}")
  set(_sources "${_out_c}")

  if("${_parent_folder}" STREQUAL "msg")
    list(APPEND _outputs
      "${_msg_output_dir}/${_ros_name}__rosidl_typesupport_tickle_c.h"
      "${_msg_output_dir}/${_ros_name}__rosidl_typesupport_tickle_c.c"
      "${_msg_output_dir}/${_ros_name}__type_support.c")
    list(APPEND _sources
      "${_msg_output_dir}/${_ros_name}__rosidl_typesupport_tickle_c.c"
      "${_msg_output_dir}/${_ros_name}__type_support.c")
  else() # srv - one adapter+type_support pair each for _Request and _Response, plus one more
         # type_support.c tying them together into the rosidl_service_type_support_t itself -
         # see ros2_cli.py's own module docstring for the full per-.srv output list.
    foreach(_part "Request" "Response")
      list(APPEND _outputs
        "${_msg_output_dir}/${_ros_name}_${_part}__rosidl_typesupport_tickle_c.h"
        "${_msg_output_dir}/${_ros_name}_${_part}__rosidl_typesupport_tickle_c.c"
        "${_msg_output_dir}/${_ros_name}_${_part}__type_support.c")
      list(APPEND _sources
        "${_msg_output_dir}/${_ros_name}_${_part}__rosidl_typesupport_tickle_c.c"
        "${_msg_output_dir}/${_ros_name}_${_part}__type_support.c")
    endforeach()
    list(APPEND _outputs "${_msg_output_dir}/${_ros_name}__type_support.c")
    list(APPEND _sources "${_msg_output_dir}/${_ros_name}__type_support.c")
  endif()

  add_custom_command(
    OUTPUT ${_outputs}
    COMMAND Python3::Interpreter
    ARGS -m "${rosidl_typesupport_tickle_c_PYTHON_MODULE}"
      --package "${PROJECT_NAME}"
      --subfolder "${_parent_folder}"
      --name "${_idl_name}"
      --input "${_src_file}"
      --outdir "${_msg_output_dir}"
    DEPENDS "${_src_file}"
    COMMENT "Generating TickLE type support for ${_idl_name}"
    VERBATIM
  )
  list(APPEND _generated_sources ${_sources})
endforeach()

# NOT return() here: ament_execute_extensions()/rosidl_generate_interfaces() are both macros, and
# include() inside a macro runs in the *caller's* scope rather than a scope of its own (unlike a
# function) - a return() here doesn't just exit this file, it unwinds the caller's own scope too,
# silently skipping every extension registered after this one (rosidl_typesupport_c, rosidl_
# typesupport_fastrtps_c/_cpp, rosidl_typesupport_introspection_cpp, ...) without any error of its
# own - their own add_library() calls simply never ran, surfacing later as their targets having no
# sources at all ("CMake Error: Cannot determine link language"). An if() the same size as
# everything below it, indented one level deeper, avoids the whole hazard.
if(_generated_sources)
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
    "${_output_path}/srv"
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
endif()
