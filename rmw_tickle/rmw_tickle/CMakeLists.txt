cmake_minimum_required(VERSION 3.8)
project(rmw_tickle C)

# Optimised unless the caller chooses otherwise (2026-09-26). colcon passes no build type by default, and
# CMake's empty one adds no optimisation flags at all, so every rmw_tickle comparison until now ran this
# package unoptimised against rmw_cyclonedds_cpp and rmw_fastrtps_cpp release packages - the same fairness
# gap the TickLE harness had (examples/perf_hil/tickle/build.sh, TICKLE_CORE_BUILD). An explicit
# -DCMAKE_BUILD_TYPE still wins; this only replaces "nothing".
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default Release)" FORCE)
endif()

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()

find_package(ament_cmake REQUIRED)
find_package(rcutils REQUIRED)
find_package(rmw REQUIRED)
find_package(rmw_implementation_cmake REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_tickle_c REQUIRED)
find_package(rosidl_typesupport_tickle_cpp REQUIRED)
find_package(Threads REQUIRED)

# TickLE itself isn't packaged as an ament/colcon package (it's a plain static-lib Makefile
# project - see the top-level README's "Build") - this package lives inside the tickle monorepo,
# two directories below the repo root, so reach its public headers relative to that instead of
# via find_package(tickle).
set(TICKLE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
set(TICKLE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

add_library(rmw_tickle
  src/rmw_init.c
  src/rmw_node.c
  src/rmw_typesupport.c
  src/rmw_publisher.c
  src/rmw_subscription.c
  src/rmw_serialize.c
  src/rmw_client.c
  src/rmw_service.c
  src/rmw_guard_condition.c
  src/rmw_wait_set.c
  src/rmw_graph.c
  src/rmw_qos.c
  src/rmw_event.c
  src/rmw_features.c
  src/rmw_unsupported.c
  src/rmw_large.c
  # TickLE's own core (rmw_node.c's tt_Context_create()/_poll()/_interrupt()/_destroy() calls,
  # Milestone 2, are the first real use of it in this package - rmw_init.c only touched the
  # _tt_CONFIG global struct before this) - compiled straight in, same reason rosidl_typesupport_
  # tickle_c/cmake/rosidl_typesupport_tickle_c_generate_interfaces.cmake does the same for src/
  # encoding.c/log.c: no installed ament/colcon TickLE package exists to link against instead.
  # hal_linux.c only, not hal_freertos.c - rmw_tickle is a host-side (Linux) library, matching
  # every real rmw implementation.
  "${TICKLE_SRC_DIR}/tickle.c"
  "${TICKLE_SRC_DIR}/encoding.c"
  "${TICKLE_SRC_DIR}/log.c"
  "${TICKLE_SRC_DIR}/hal_linux.c"
)

# tt_MAX_BUFFER_LENGTH: the one value every interface package's typesupport was generated for,
# exported by rosidl_typesupport_tickle_c (its TICKLE_MAX_BUFFER_LENGTH). PUBLIC, because anything
# compiled against rmw_tickle's headers sees struct tt_Context and the rest of tickle.h, whose layout
# depends on it - two translation units in one process disagreeing on it would disagree on those
# structs' sizes.
if(NOT rosidl_typesupport_tickle_c_MAX_BUFFER_LENGTH)
  message(FATAL_ERROR "rosidl_typesupport_tickle_c exported no MAX_BUFFER_LENGTH - rebuild it")
endif()
target_compile_definitions(rmw_tickle PUBLIC "tt_MAX_BUFFER_LENGTH=${rosidl_typesupport_tickle_c_MAX_BUFFER_LENGTH}")
# Core's inline server/client storage (config.h), sized for any message by default: 192 x
# tt_MAX_BUFFER_LENGTH per tt_Server. rmw_tickle attaches storage sized for each service instead
# (tt_Server_set_storage()/tt_Client_set_storage(), rmw_service.c/rmw_client.c), so the inline
# arrays are made as small as allowed - 8 bytes, the alignment unit.
target_compile_definitions(rmw_tickle PUBLIC tt_SERVER_CACHE_ENTRY_LENGTH=8 tt_CLIENT_CACHE_LENGTH=8)
# Large-message stage 2 (docs/DESIGN.md section 8): messages above 64 KB, up to RMW_TICKLE_MAX_SAMPLE_BYTES. Off in
# core unless asked for, so a build that never carries one keeps the smaller window and no large-sample state.
target_compile_definitions(rmw_tickle PUBLIC tt_LARGE_SAMPLES=1)
# Nodes per context (CONTEXT_NODE_PLAN.md stage 2): every rmw node is a core node on the one shared context, and a
# composed ROS 2 bringup (Nav2's, one container) holds 15-20 of them - core's default, 16, would refuse the rest.
# 256 is the most a node's uint8_t index allows; the table costs 8 bytes per entry.
target_compile_definitions(rmw_tickle PUBLIC tt_MAX_NODES=256)
# Scheduler entries per context (tt_MAX_SCHEDULER_LENGTH, config.h; 2026-10-16). Every rmw node shares the one context,
# and a service holds an entry per request it keeps deferred and per answer it keeps for retries - up to 3 x
# tt_CLIENT_MAX_OUTSTANDING per calling Client, 2 x tt_MAX_SERVER_CACHE_COUNT (128) for one saturated service - beside
# every client's call retries, every reliable writer's and reader's timers and the DEADLINE timers. Core's 128 is sized
# for a microcontroller. 512 costs 12 KB per context (24 bytes an entry) and no time per operation: insert is
# O(log entries in use), unschedule scans the entries in use. A full scheduler is counted (sched_full, traffic line).
target_compile_definitions(rmw_tickle PUBLIC tt_MAX_SCHEDULER_LENGTH=512)
# (g8, RMW_GAPS_PLAN.md) Several rmw_tickle processes on one host each claim a context id of their own (config.h).
target_compile_definitions(rmw_tickle PUBLIC tt_CONTEXT_ID_CLAIM=1)
# (g9, RMW_GAPS_PLAN.md) Nodes of one process share a context: samples between them are delivered in-process.
target_compile_definitions(rmw_tickle PUBLIC tt_LOCAL_DELIVERY=1)
# (g6, RMW_GAPS_PLAN.md) ROS_AUTOMATIC_DISCOVERY_RANGE and ROS_STATIC_PEERS: the discovery range and peer links.
target_compile_definitions(rmw_tickle PUBLIC tt_DISCOVERY_OPTIONS=1)
# Endpoints per context, and the remote entities rmw's graph API can see (CONTEXT_NODE_PLAN.md 4a). A composed Nav2
# bringup holds ~20 nodes of ~30 endpoints each (9 per rclcpp node by default, more for lifecycle nodes and action
# servers, plus the application's), ~600 - core's 256 would refuse the rest. 2048 is over 3x that; the endpoint index
# stays at twice the count. core's 16 discovered entities meant `ros2 topic list` against such a process saw 16 of
# its endpoints: 2048 too, ~1.1 MB of tt_DiscoveredEntity per context. And an announce goes in 1472-byte fragments of
# ~15 ROS-sized endpoints, 32 of them by default (~480 endpoints): 255, the wire's most, announces ~3800.
target_compile_definitions(rmw_tickle PUBLIC tt_MAX_ENDPOINT_COUNT=2048 tt_ENDPOINT_INDEX_SIZE=4096
                                             tt_MAX_DISCOVERED_ENTITIES=2048 tt_UPDATE_MAX_PARTS=255)
# The socket receive buffer requested (SO_RCVBUF), the 4 MiB the user approved on 2026-09-24: at
# 65507-byte datagrams core's 1 MiB default holds sixteen. The kernel clamps it to
# net.core.rmem_max, and hal_linux.c logs a warning naming that sysctl when what is granted holds
# fewer than 16 of this build's datagrams.
target_compile_definitions(rmw_tickle PUBLIC tt_SOCKET_BUFFER_SIZE=4194304)

# RMW_TICKLE_MAX_RELIABLE_READERS (2026-10-16): how many readers one RELIABLE publisher tracks acknowledgements for -
# core's tt_MAX_ACK_ENTRIES, 16 by default. Past it a reader is not matched (logged "Ack table full"). Fixed arrays in
# every publisher (tt_Publisher.peer_acks[] and, with the hybrid transport, peer_watch[]): 64 bytes per entry here, so
# 64 readers add 3 KiB to each publisher. PUBLIC, because it changes struct tt_Publisher's layout and everything
# compiled against these headers has to agree on it. docs/RMW.md explains why it is a build option and not an
# environment variable.
set(RMW_TICKLE_MAX_RELIABLE_READERS "" CACHE STRING
    "RELIABLE readers per publisher (core's tt_MAX_ACK_ENTRIES); empty keeps core's 16")
if(NOT RMW_TICKLE_MAX_RELIABLE_READERS STREQUAL "")
  if(NOT RMW_TICKLE_MAX_RELIABLE_READERS MATCHES "^[1-9][0-9]*$" OR RMW_TICKLE_MAX_RELIABLE_READERS GREATER 1024)
    message(FATAL_ERROR "RMW_TICKLE_MAX_RELIABLE_READERS must be a number from 1 to 1024, not "
                        "'${RMW_TICKLE_MAX_RELIABLE_READERS}'")
  endif()
  target_compile_definitions(rmw_tickle PUBLIC "tt_MAX_ACK_ENTRIES=${RMW_TICKLE_MAX_RELIABLE_READERS}")
endif()

# RMW_TICKLE_TRACE (2026-09-26, rmw_tickle/RMW_PERF_PLAN.md H2): latency stamps along the receive-to-reply
# path, core's and this package's, dumped by RMW_TICKLE_TRACE_FILE at shutdown (rmw_init.c). Measurement
# only; off, every stamp compiles to nothing. RMW_TICKLE_RX_BATCH passes tt_RX_BATCH through for the
# recvmmsg arm (hal_linux.h; rmw's default is 1 - see there for why).
option(RMW_TICKLE_TRACE "Build with core and rmw latency trace stamps" OFF)
if(RMW_TICKLE_TRACE)
  target_compile_definitions(rmw_tickle PRIVATE tt_TRACE)
endif()
# Requests one client may have in flight (tt_CLIENT_MAX_OUTSTANDING, config.h; the user's decision of 2026-10-16:
# compile time only, 4 by default). Empty keeps core's default, so the rmw build uses the same constant as core's own.
set(RMW_TICKLE_CLIENT_MAX_OUTSTANDING "" CACHE STRING "tt_CLIENT_MAX_OUTSTANDING for this build; empty keeps config.h's default (4)")
if(NOT RMW_TICKLE_CLIENT_MAX_OUTSTANDING STREQUAL "")
  target_compile_definitions(rmw_tickle PUBLIC "tt_CLIENT_MAX_OUTSTANDING=${RMW_TICKLE_CLIENT_MAX_OUTSTANDING}")
endif()
set(RMW_TICKLE_RX_BATCH "" CACHE STRING "tt_RX_BATCH for this build; empty keeps hal_linux.h's default")
if(NOT RMW_TICKLE_RX_BATCH STREQUAL "")
  target_compile_definitions(rmw_tickle PUBLIC "tt_RX_BATCH=${RMW_TICKLE_RX_BATCH}")
endif()

target_include_directories(rmw_tickle PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${TICKLE_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:include>
)

# Modern imported-target style (matching how real rmw_cyclonedds_cpp/rmw_fastrtps_cpp already link
# against these same packages) rather than ament_target_dependencies() - some ROS 2 distros (e.g.
# "lyrical", found while provisioning rmw_tickle/PLAN.md's rmw-perf.yml benchmark rig) have removed
# that macro entirely, not just reorganized which find_package() call provides it. One single
# keyword-form call for everything, including Threads::Threads, sidesteps the plain-vs-keyword
# target_link_libraries() mixing restriction the old two-call split existed to work around.
target_link_libraries(rmw_tickle PUBLIC
  rcutils::rcutils
  rmw::rmw
  rosidl_runtime_c::rosidl_runtime_c
  rosidl_typesupport_tickle_c::rosidl_typesupport_tickle_c
  rosidl_typesupport_tickle_cpp::rosidl_typesupport_tickle_cpp
  Threads::Threads
)

ament_export_targets(export_rmw_tickle HAS_LIBRARY_TARGET)
ament_export_dependencies(
  rcutils rmw rosidl_runtime_c rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp)

# rmw_tickle/PLAN.md's Milestone 8: registers the "rmw_typesupport"/"rmw_typesupport_c"/
# "rmw_typesupport_cpp" ament_index resources that rmw_implementation_cmake's own get_available_
# rmw_implementations()/get_default_rmw_implementation() read - the actual mechanism behind
# `RMW_IMPLEMENTATION=rmw_tickle` selection, not just documentation. Both languages now (see
# rosidl_typesupport_tickle_cpp's own package.xml doc comment for why the "cpp" one had to be
# added: rosidl_typesupport_tickle_c alone is unreachable from any real rclcpp C++ node) - no
# introspection typesupport either way, matching this package's own "Supported subset" (PLAN.md).
register_rmw_implementation(
  "c:rosidl_typesupport_tickle_c"
  "cpp:rosidl_typesupport_tickle_cpp")

install(
  DIRECTORY include/
  DESTINATION include
)

install(
  TARGETS rmw_tickle
  EXPORT export_rmw_tickle
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

# rmw_tickle/PLAN.md's Milestone 10: a scoped test suite - only what "Supported subset" + the QoS
# roadmap actually cover, not full upstream rmw_implementation/test_rmw_implementation conformance
# (no live two-process publish/subscribe/service round trip test infra exists yet for rmw_tickle -
# every prior milestone's own verification depth stopped at "compile+link", and this milestone's
# own new tests stay within that same boundary: a real struct tt_Context is created in-process, but
# nothing here talks to a second node/peer - see PLAN.md's own note on this gap).
if(BUILD_TESTING)
  find_package(ament_cmake_test REQUIRED)

  foreach(_test_name test_qos test_node_lifecycle test_guard_condition_wait test_graph test_events test_liveliness_lost_watchdog test_rmw_api_surface test_multi_node test_names_and_types test_publish_take_reuse test_history_keep_all test_keep_all_blocking test_best_available test_heartbeat_piggyback_default test_type_checks test_storage_budget test_service_roundtrip test_service_multi_call test_context_fini_leaked_node test_unsupported_entry_points test_executor_poll test_domain_id test_psn_codec test_event_teardown test_security_options test_event_callbacks test_g3_events test_take_sequence test_context_id_registry test_inprocess test_discovery_options test_shell_pool test_serialized test_keep_all_reader test_reported_qos test_gid_two_process test_introspection_two_process test_loaned_messages test_large_loans test_publish_hugepage test_encode_range test_context_resident test_lifespan_clock test_lifespan_writer test_qos_profile test_qos_incompatible_events test_qos_compatible test_teardown_wait)
    add_executable(${_test_name} test/${_test_name}.c)
    target_link_libraries(${_test_name} rmw_tickle Threads::Threads)
    # Every check in these tests is an assert(), so NDEBUG - which Release, now the default build type
    # above, defines - would compile every one of them away and leave tests that pass whatever the
    # library does. Measured, not assumed: a Release build of these binaries referenced no
    # __assert_fail at all, and "passed" 38 of 38.
    target_compile_options(${_test_name} PRIVATE -UNDEBUG)
    # GENERATE_RESULT_FOR_RETURN_CODE_ZERO: these are plain C executables (an assert()-per-check,
    # exit 0/nonzero), not something that emits its own JUnit result file - ament_add_test()'s own
    # run_test.py wrapper otherwise treats "no result file produced" as a failure regardless of
    # the command's own exit code.
    ament_add_test(${_test_name}
      COMMAND "$<TARGET_FILE:${_test_name}>"
      GENERATE_RESULT_FOR_RETURN_CODE_ZERO
    )
  endforeach()

  # DEADLINE gaps of 0.5P to 5.5P at phases on both sides of P/2: one miss per missed period, on both sides (QoS plan D4).
  add_executable(test_deadline_gap test/test_deadline_gap.c)
  target_link_libraries(test_deadline_gap rmw_tickle Threads::Threads)
  target_compile_options(test_deadline_gap PRIVATE -UNDEBUG)
  ament_add_test(test_deadline_gap
    COMMAND "$<TARGET_FILE:test_deadline_gap>"
    GENERATE_RESULT_FOR_RETURN_CODE_ZERO
  )

  # Two same-host processes over the segment, one SIGKILLed mid-stream, against a control that is never killed
  # (test/test_shm_kill.c). Eight arms of ~15-20 s each; it broadcasts, so it runs in run_rmw_suite.sh's netns.
  add_executable(test_shm_kill test/test_shm_kill.c)
  target_link_libraries(test_shm_kill rmw_tickle Threads::Threads)
  target_compile_options(test_shm_kill PRIVATE -UNDEBUG)
  ament_add_test(test_shm_kill
    COMMAND "$<TARGET_FILE:test_shm_kill>"
    GENERATE_RESULT_FOR_RETURN_CODE_ZERO
    TIMEOUT 300
  )

  # RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS needs a saturated link, which it makes with a tbf-shaped veth pair - possible only
  # in a named private netns with `sudo -n ip` (run_rmw_suite.sh's). Anywhere else it exits 77: skipped, not passed.
  add_executable(test_large_publish_block test/test_large_publish_block.c)
  target_link_libraries(test_large_publish_block rmw_tickle Threads::Threads)
  target_compile_options(test_large_publish_block PRIVATE -UNDEBUG)
  ament_add_test(test_large_publish_block
    COMMAND "$<TARGET_FILE:test_large_publish_block>"
    GENERATE_RESULT_FOR_RETURN_CODE_ZERO
    SKIP_RETURN_CODE 77
  )
endif()

ament_package()
