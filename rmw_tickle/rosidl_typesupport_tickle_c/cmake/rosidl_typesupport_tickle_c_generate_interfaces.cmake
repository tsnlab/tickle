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
# Tracked explicitly (not re-derived via EXISTS on ${_output_path}/msg|srv later) because at
# CMake *configure* time neither directory exists yet at all - the add_custom_command() below
# only creates it once the real *build* actually runs the generator - an EXISTS check this early
# would always be false even on a package that only ever generates one of the two.
set(_tickle_has_msg FALSE)
set(_tickle_has_srv FALSE)

# Cross-package nested message resolution (rmw_tickle/PLAN.md's own Milestone for this): for
# ros2_cli.py's own -I search convention (DIR/<pkg>/msg/<Name>.msg - resolve.py's
# _find_on_search_path()), DIR is a *share root* containing <pkg> as its own subdirectory, not
# <pkg>'s own share directory itself. Every rosidl-generated package's own CMake config lands at
# <share-root>/<pkg>/cmake/<pkg>Config.cmake (ament_cmake's own universal convention -
# rosidl_find_package_idl.cmake's own "${pkg}_DIR}/../<relative-idl-path>" candidate relies on
# this same fact) - so ${<pkg>_DIR}/../.. recovers that shared root once per dependency package,
# already find_package()-d by the time this extension runs (a prerequisite for referencing
# <pkg>/msg/Something in the first place). Deliberately package-by-package, not one shared
# workspace-wide root: two dependencies can come from two different install prefixes.
set(_tickle_dep_include_dirs "")
foreach(_dep_pkg_name ${rosidl_generate_interfaces_DEPENDENCY_PACKAGE_NAMES})
  if(DEFINED ${_dep_pkg_name}_DIR)
    get_filename_component(_tickle_dep_share_root "${${_dep_pkg_name}_DIR}/../.." ABSOLUTE)
    list(APPEND _tickle_dep_include_dirs "${_tickle_dep_share_root}")
  endif()
endforeach()
if(_tickle_dep_include_dirs)
  list(REMOVE_DUPLICATES _tickle_dep_include_dirs)
endif()
set(_tickle_dep_include_args "")
foreach(_tickle_dep_include_dir ${_tickle_dep_include_dirs})
  list(APPEND _tickle_dep_include_args "-I" "${_tickle_dep_include_dir}")
endforeach()

foreach(_abs_idl_file ${rosidl_generate_interfaces_ABS_IDL_FILES})
  # rosidl_generate_interfaces_ABS_IDL_FILES holds *adapted* .idl paths (rosidl_adapt_interfaces()
  # runs before any extension, including this one, so every generator only ever has to understand
  # one input format) - NOT the original .msg this package's own author actually wrote. tickle_
  # typesupport has no OMG IDL parser (only tickle_typesupport._rosidl_parser, the .msg/.srv line
  # format), so this extension needs the original source file back.
  get_filename_component(_parent_folder "${_abs_idl_file}" DIRECTORY)
  get_filename_component(_parent_folder "${_parent_folder}" NAME)
  get_filename_component(_idl_name "${_abs_idl_file}" NAME_WE)

  # rcl_interfaces' own .srv files (SetParameters etc.) all nest Parameter/ParameterValue, which
  # tickle_typesupport.resolve.Ros2Resolver's own "sibling .msg in the same package" lookup
  # (_find_independent_source()) cannot actually find from a .srv file's own call site: it looks
  # relative to os.path.dirname(input_path) - this .srv's own srv/ directory - not msg/, so the
  # sibling lookup misses and falls through to a different (unpatched, apt-installed) copy of the
  # same message, one that doesn't carry rcl_interfaces_rmw_tickle.patch's own `# @capacity`
  # annotations rcl_interfaces/msg/ParameterValue.msg needs (see that patch's own comment) -
  # surfacing as "auto-derived capacity only supports a single trailing variable array" instead of
  # generating correctly. A real bug in tickle_typesupport's own cross-.srv/.msg sibling
  # resolution, not specific to rcl_interfaces - affects any package mixing .srv and .msg with a
  # same-package nested reference between them - tracked as a real follow-up (rmw_tickle/PLAN.md),
  # not fixed here. Scoped narrowly to rcl_interfaces' own .srv files in the meantime: rmw_tickle
  # has no RPC/service typesupport story for rcl_interfaces either way yet (parameter get/set
  # services stay unavailable over rmw_tickle, same as the already-documented TestClient/
  # TestService.check_qos conformance gaps), so skipping just these costs nothing not already
  # missing - the actual goal (every real rclcpp::Node's own unconditional /parameter_events
  # subscription, NodeTimeSource, working over rmw_tickle) only ever needed the *messages*.
  if("${PROJECT_NAME}" STREQUAL "rcl_interfaces" AND "${_parent_folder}" STREQUAL "srv")
    continue()
  endif()

  # Found the hard way via a real CI failure: the original source is NOT reliably at
  # CMAKE_CURRENT_SOURCE_DIR/<subfolder>/<name>.<ext> - that only holds for a package's *own*
  # local message files. A package can (and test_msgs really does, for every message except its
  # own Builtins.msg) pass in .msg/.srv files that physically live in a completely different
  # package's own share directory (test_interface_files, in test_msgs' case) - rosidl_generate_
  # interfaces() is a macro (not a function), so its own internal `_non_idl_tuples` variable
  # (list of "<abs_base_path>:<relative_path>" tuples for every non-.idl file passed in, BEFORE
  # rosidl_adapter ran - see rosidl_generate_interfaces.cmake's own comment on this tuple format)
  # is still set in this exact scope when this extension runs. rosidl_adapter preserves both the
  # relative path and the ordering, so matching this .idl file's own relative path (same parent
  # folder + stem, original extension) against that list recovers the real original location
  # regardless of which package it actually lives in.
  set(_src_relpath "${_parent_folder}/${_idl_name}.${_parent_folder}")
  set(_src_file "")
  if("${_parent_folder}" STREQUAL "msg" OR "${_parent_folder}" STREQUAL "srv")
    foreach(_non_idl_tuple ${_non_idl_tuples})
      string(REGEX REPLACE "^.*:" "" _non_idl_relpath "${_non_idl_tuple}")
      if("${_non_idl_relpath}" STREQUAL "${_src_relpath}")
        string(REGEX REPLACE ":([^:]*)$" "/\\1" _src_file "${_non_idl_tuple}")
        break()
      endif()
    endforeach()
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
    set(_tickle_has_msg TRUE)
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
    set(_tickle_has_srv TRUE)
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
      ${_tickle_dep_include_args}
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

  # rosidl_export_typesupport_targets() below (already present before this same milestone) reads
  # this exact alias name back out (@PROJECT_NAME@::${_target} in rosidl_cmake's own generated
  # rosidl_cmake_export_typesupport_targets-extras.cmake.in) when a *downstream* package does
  # find_package(${PROJECT_NAME}) - without it, that generated file's own `if(NOT TARGET
  # "${_target}")` guard silently fails (a CMake WARNING, not a fatal error) and never populates
  # ${PROJECT_NAME}_TARGETS${_target_suffix} at all, so cross-package linking below could never
  # have worked even before this milestone added anything that needed it - the same alias pattern
  # rosidl_generator_c_generate_interfaces.cmake already establishes for its own target.
  add_library(${PROJECT_NAME}::${rosidl_generate_interfaces_TARGET}${_target_suffix} ALIAS
    ${rosidl_generate_interfaces_TARGET}${_target_suffix})

  if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_target_properties(${rosidl_generate_interfaces_TARGET}${_target_suffix} PROPERTIES
      C_STANDARD 11)
  endif()

  # msg/srv PUBLIC (not PRIVATE): every generated file's own #include is a bare filename (e.g.
  # "Vector3.h" - see ros2_adapter.py's own nested_ros_includes()/nested_adapter_includes(), never
  # a ROS 2-style "pkg/msg/name.h" nested path), so a *different* package's own generated adapter
  # referencing this package's own nested type needs this exact directory on its own include path
  # too - BUILD_INTERFACE for another target still in the same CMake build tree (shouldn't
  # normally happen for two separate ament packages, but costs nothing to allow), INSTALL_INTERFACE
  # for the real cross-package case (a colcon build consuming this package once already installed
  # - see the install(DIRECTORY ...)/ament_export_include_directories() calls below, which is what
  # makes the installed copy this points at actually exist). _generator_output_path/TICKLE_ROOT
  # stay PRIVATE: every package independently computes its own TICKLE_ROOT (a global variable set
  # by rosidl_typesupport_tickle_c-extras.cmake.in whenever any package find_package()s it, not
  # something that needs propagating from a dependency) and never needs a *dependency's* own
  # rosidl_generator_c output directory (it only calls that dependency's own already-compiled
  # to_tickle/from_tickle functions, declared in the headers installed below, not its raw
  # rosidl_generator_c struct definitions directly).
  # Listed only for whichever of msg/srv this package actually generated (_tickle_has_msg/_srv,
  # the same flags install(DIRECTORY ...)/ament_export_include_directories() below use) - an
  # $<INSTALL_INTERFACE:...> path that was never actually install()ed hits the exact same hard
  # "Imported target ... includes non-existent path" CMake Generate-step error a downstream
  # package's own find_package() would raise as ament_export_include_directories() itself
  # (found in real CI by rosidl_typesupport_tickle_c_tests_dep, a real package with only .msg,
  # no .srv, at all) - this is CMake's own modern target-based export mechanism, a *different*
  # (stricter) code path from ament_export_include_directories()'s own legacy one, so both need
  # the identical guard independently, not just one or the other.
  set(_tickle_public_include_dirs "")
  if(_tickle_has_msg)
    list(APPEND _tickle_public_include_dirs
      "$<BUILD_INTERFACE:${_output_path}/msg>"
      "$<INSTALL_INTERFACE:include/${PROJECT_NAME}/rosidl_typesupport_tickle_c/msg>")
  endif()
  if(_tickle_has_srv)
    list(APPEND _tickle_public_include_dirs
      "$<BUILD_INTERFACE:${_output_path}/srv>"
      "$<INSTALL_INTERFACE:include/${PROJECT_NAME}/rosidl_typesupport_tickle_c/srv>")
  endif()
  if(_tickle_public_include_dirs)
    target_include_directories(${rosidl_generate_interfaces_TARGET}${_target_suffix} PUBLIC
      ${_tickle_public_include_dirs})
  endif()
  target_include_directories(${rosidl_generate_interfaces_TARGET}${_target_suffix} PRIVATE
    "${_generator_output_path}"
    "${rosidl_typesupport_tickle_c_TICKLE_ROOT}/include"
  )

  target_link_libraries(${rosidl_generate_interfaces_TARGET}${_target_suffix} PUBLIC
    ${rosidl_generate_interfaces_TARGET}__rosidl_generator_c)
  target_link_libraries(${rosidl_generate_interfaces_TARGET}${_target_suffix} PRIVATE
    rosidl_typesupport_tickle_c::rosidl_typesupport_tickle_c
    rosidl_runtime_c::rosidl_runtime_c
    rosidl_typesupport_interface::rosidl_typesupport_interface)

  # Link against every dependency package's own rosidl_typesupport_tickle_c library too (PUBLIC,
  # matching rosidl_generator_c_generate_interfaces.cmake's/rosidl_typesupport_c_generate_
  # interfaces.cmake's own identical pattern) - a generated adapter file referencing a *cross-
  # package* nested type calls that type's own already-compiled __to_tickle/__from_tickle/
  # _encode/_decode functions directly (ros2_adapter.py's own direct-call design, not a dispatch
  # table - unlike rosidl_typesupport_c's own fully dispatch-based one, this needs a real link
  # dependency here, not just at the final executable). ${<pkg>_TARGETS${_target_suffix}} is only
  # ever populated for a package that actually has this target (see the ALIAS comment above); a
  # dependency that never used rosidl_typesupport_tickle_c at all leaves it unset, silently
  # contributing nothing - correct, not an error, since nothing here could reference a nested type
  # from a package that was never itself given TickLE typesupport.
  foreach(_dep_pkg_name ${rosidl_generate_interfaces_DEPENDENCY_PACKAGE_NAMES})
    if(${_dep_pkg_name}_TARGETS${_target_suffix})
      target_link_libraries(${rosidl_generate_interfaces_TARGET}${_target_suffix} PUBLIC
        ${${_dep_pkg_name}_TARGETS${_target_suffix}})
    endif()
  endforeach()

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

    # The generated headers themselves (mirrors rosidl_generator_c_generate_interfaces.cmake's
    # own install(DIRECTORY ...) for its own struct headers) - a downstream package's own
    # generated adapter #include-ing this package's own nested type's header needs these to
    # actually exist post-install, not just the compiled library above. .c files are deliberately
    # excluded (PATTERN "*.h" only) - already baked into the library just installed, never
    # #included directly by anything.
    if(_tickle_has_msg)
      install(DIRECTORY "${_output_path}/msg/"
        DESTINATION "include/${PROJECT_NAME}/rosidl_typesupport_tickle_c/msg"
        FILES_MATCHING PATTERN "*.h")
    endif()
    if(_tickle_has_srv)
      install(DIRECTORY "${_output_path}/srv/"
        DESTINATION "include/${PROJECT_NAME}/rosidl_typesupport_tickle_c/srv"
        FILES_MATCHING PATTERN "*.h")
    endif()
    # Found the hard way, in real CI, by rosidl_typesupport_tickle_c_tests_dep (a real package
    # with only .msg, no .srv, at all): exporting a path that was never actually install()ed above
    # doesn't just print ament_cmake_export_include_directories-extras.cmake's own soft "doesn't
    # exist" WARNING - a downstream package's own find_package() hits a hard CMake Generate-step
    # ERROR the *imported target* mechanism itself raises ("includes non-existent path"), failing
    # the whole configure. Each directory's own export must match its own install() above exactly.
    set(_tickle_export_include_dirs "")
    if(_tickle_has_msg)
      list(APPEND _tickle_export_include_dirs "include/${PROJECT_NAME}/rosidl_typesupport_tickle_c/msg")
    endif()
    if(_tickle_has_srv)
      list(APPEND _tickle_export_include_dirs "include/${PROJECT_NAME}/rosidl_typesupport_tickle_c/srv")
    endif()
    if(_tickle_export_include_dirs)
      ament_export_include_directories(${_tickle_export_include_dirs})
    endif()

    ament_export_targets(export_${rosidl_generate_interfaces_TARGET}${_target_suffix})
    rosidl_export_typesupport_targets(${_target_suffix}
      ${rosidl_generate_interfaces_TARGET}${_target_suffix})

    ament_export_dependencies(
      "rosidl_typesupport_tickle_c"
      "rosidl_runtime_c"
      "rosidl_typesupport_interface")
  endif()
endif()
