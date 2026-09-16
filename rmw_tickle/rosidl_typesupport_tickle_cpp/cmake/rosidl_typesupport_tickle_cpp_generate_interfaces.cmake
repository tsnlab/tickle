# Executed by ament_execute_extensions("rosidl_generate_idl_interfaces") - registered via
# ament_register_extension() in ../rosidl_typesupport_tickle_cpp-extras.cmake.in, run once for
# every ROS 2 interface package that calls rosidl_generate_interfaces() and has (directly or
# transitively) find_package()-d rosidl_typesupport_tickle_cpp first. Runs in that CALLING
# package's own CMake scope, same as rosidl_typesupport_tickle_c's own extension.
#
# What this generates, and why it needs no per-field code at all (unlike rosidl_typesupport_
# tickle_c's own extension, which shells out to tools/typesupport's Python generator): every
# output here is a *pure delegation shim* to a symbol rosidl_typesupport_tickle_c's own extension
# already generated for the exact same message - see resource/msg__type_support.cpp.in's own doc
# comment. Its only job is to exist as a "rosidl_typesupport_cpp"-registered candidate (see this
# package's own CMakeLists.txt) so rclcpp's top-level C++ dispatch can find it at all - the actual
# encode/decode work stays exactly where Milestone 1 put it.
#
# .msg and .srv (a .srv's own Request/Response are each an ordinary message in rosidl_generator_c's
# own eyes - resource/srv__type_support.cpp.in's own doc comment covers the one extra shim a
# service itself needs on top of its Request/Response's own message-level ones, generated here the
# same way). This closes the gap rmw_tickle/PLAN.md's Milestone 11 row originally deferred: rmw_
# tickle's own services (rmw_client.c/rmw_service.c) go through rmw_tickle_get_service_callbacks()
# -> rmw_tickle_get_message_callbacks() on the request/response types, which needs this same C++-
# reachability fix to work from a real rclcpp::Client/Service, not just a topic.

if(NOT TARGET ${rosidl_generate_interfaces_TARGET}__rosidl_generator_c)
  message(FATAL_ERROR
    "The 'rosidl_generator_c' extension must be executed before the "
    "'rosidl_typesupport_tickle_cpp' extension.")
endif()
if(NOT TARGET ${rosidl_generate_interfaces_TARGET}__rosidl_typesupport_tickle_c)
  message(FATAL_ERROR
    "The 'rosidl_typesupport_tickle_c' extension must be executed before the "
    "'rosidl_typesupport_tickle_cpp' extension.")
endif()

find_package(rosidl_cmake REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_interface REQUIRED)

set(_generator_output_path "${CMAKE_CURRENT_BINARY_DIR}/rosidl_generator_c")
set(_output_path "${CMAKE_CURRENT_BINARY_DIR}/rosidl_typesupport_tickle_cpp/${PROJECT_NAME}")
set(_msg_template_file
  "${rosidl_typesupport_tickle_cpp_DIR}/../resource/msg__type_support.cpp.in")
set(_srv_template_file
  "${rosidl_typesupport_tickle_cpp_DIR}/../resource/srv__type_support.cpp.in")
set(_generated_sources "")

foreach(_abs_idl_file ${rosidl_generate_interfaces_ABS_IDL_FILES})
  # Same "recover the original .msg/.srv path from the adapted .idl one, via rosidl_generate_
  # interfaces()'s own still-in-scope _non_idl_tuples" reasoning as rosidl_typesupport_tickle_c's
  # own extension - see its own comment on this for the full rationale (found the hard way via
  # the same real CI failure: the old CMAKE_CURRENT_SOURCE_DIR-only guess this used to make broke
  # identically here, silently skipping every message instead of FATAL_ERROR-ing, since this
  # extension only ever `continue()`s on a miss).
  get_filename_component(_parent_folder "${_abs_idl_file}" DIRECTORY)
  get_filename_component(_parent_folder "${_parent_folder}" NAME)
  get_filename_component(_idl_name "${_abs_idl_file}" NAME_WE)

  if("${_parent_folder}" STREQUAL "msg")
    set(_src_relpath "msg/${_idl_name}.msg")
  elseif("${_parent_folder}" STREQUAL "srv")
    set(_src_relpath "srv/${_idl_name}.srv")
  else()
    continue() # .action - never supported (see rmw_client.c/rmw_service.c)
  endif()
  set(_src_file "")
  foreach(_non_idl_tuple ${_non_idl_tuples})
    string(REGEX REPLACE "^.*:" "" _non_idl_relpath "${_non_idl_tuple}")
    if("${_non_idl_relpath}" STREQUAL "${_src_relpath}")
      string(REGEX REPLACE ":([^:]*)$" "/\\1" _src_file "${_non_idl_tuple}")
      break()
    endif()
  endforeach()
  if("${_src_file}" STREQUAL "" OR NOT EXISTS "${_src_file}")
    continue() # rosidl_typesupport_tickle_c's own extension already warned about this .idl file
  endif()

  # The header stem rosidl_generator_c actually emits - for a .srv this is the *service's* own
  # name (one shared "<name>__functions.h" covering both Request and Response structs, confirmed
  # against a real generated std_srvs build - NOT "<name>_request"/"<name>_response", which don't
  # exist as files), so this must be computed from _idl_name (the base name) before any _Request/
  # _Response suffix is appended below, same fix rmw_tickle/PLAN.md's Milestone 1(c) row already
  # made for rosidl_typesupport_tickle_c's own analogous C-level generator (ros2_header_path()).
  string_camel_case_to_lower_case_underscore("${_idl_name}" HEADER_NAME)

  # @ONLY-substituted into resource/msg__type_support.cpp.in / srv__type_support.cpp.in below -
  # names matter (@PKG_NAME@, @SUBFOLDER@, @IDL_NAME@ there), not just values.
  set(PKG_NAME "${PROJECT_NAME}")
  set(SUBFOLDER "${_parent_folder}")
  set(_msg_output_dir "${_output_path}/${_parent_folder}")

  if("${_parent_folder}" STREQUAL "msg")
    set(IDL_NAME "${_idl_name}")
    set(_out_cpp "${_msg_output_dir}/${HEADER_NAME}__type_support.cpp")
    configure_file("${_msg_template_file}" "${_out_cpp}" @ONLY)
    list(APPEND _generated_sources "${_out_cpp}")
  else() # srv - one message-level shim each for _Request/_Response (rosidl_generator_c emits an
         # ordinary message struct for each - see rosidl_typesupport_tickle_c's own analogous
         # comment), reusing the exact same msg template, plus one service-level shim tying them
         # together (resource/srv__type_support.cpp.in).
    foreach(_part "Request" "Response")
      set(IDL_NAME "${_idl_name}_${_part}")
      set(_out_cpp "${_msg_output_dir}/${HEADER_NAME}_${_part}__type_support.cpp")
      configure_file("${_msg_template_file}" "${_out_cpp}" @ONLY)
      list(APPEND _generated_sources "${_out_cpp}")
    endforeach()

    set(IDL_NAME "${_idl_name}")
    set(_out_cpp "${_msg_output_dir}/${HEADER_NAME}__type_support.cpp")
    configure_file("${_srv_template_file}" "${_out_cpp}" @ONLY)
    list(APPEND _generated_sources "${_out_cpp}")
  endif()
endforeach()

# See rosidl_typesupport_tickle_c_generate_interfaces.cmake's own comment on why this is an if()
# rather than an early return() - same include()-inside-a-macro hazard applies here too.
if(_generated_sources)
  set(_target_suffix "__rosidl_typesupport_tickle_cpp")
  add_library(${rosidl_generate_interfaces_TARGET}${_target_suffix} ${_generated_sources})

  set_target_properties(${rosidl_generate_interfaces_TARGET}${_target_suffix} PROPERTIES
    CXX_STANDARD 17)

  target_include_directories(${rosidl_generate_interfaces_TARGET}${_target_suffix} PRIVATE
    "${_generator_output_path}"
  )

  target_link_libraries(${rosidl_generate_interfaces_TARGET}${_target_suffix} PUBLIC
    ${rosidl_generate_interfaces_TARGET}__rosidl_generator_c)
  # A real link dependency, not just build-order - see resource/msg__type_support.cpp.in's own
  # top comment for why this calls straight into rosidl_typesupport_tickle_c's own generated
  # symbol rather than going through rosidl_typesupport_cpp's generic map-walking dispatch.
  target_link_libraries(${rosidl_generate_interfaces_TARGET}${_target_suffix} PRIVATE
    ${rosidl_generate_interfaces_TARGET}__rosidl_typesupport_tickle_c
    rosidl_typesupport_tickle_cpp::rosidl_typesupport_tickle_cpp
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
      "rosidl_typesupport_tickle_cpp"
      "rosidl_typesupport_cpp"
      "rosidl_runtime_c"
      "rosidl_typesupport_interface")
  endif()
endif()
