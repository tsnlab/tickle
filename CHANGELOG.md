# Changelog

All notable changes to TickLE are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) for the library
(`TICKLE_VERSION_*` in `include/tickle/tickle.h`). The on-the-wire protocol has its own
number, `tt_VERSION`, which moves independently.

## [Unreleased]

### Added

- `tt_Node_interrupt()`: wakes a blocking `tt_Node_poll()` call from another thread, returning
  `tt_RET_INTERRUPTED` promptly instead of waiting out the rest of its timeout - the one
  exception to a `tt_Node`'s otherwise-single-threaded rule (DESIGN.md's "Concurrency"). Built for
  `rmw_tickle` (`rmw_tickle/PLAN.md`'s Milestone 0), where a dedicated thread drives
  `tt_Node_poll()` in a loop and every other call (e.g. `rmw_publish()`) needs a way to get that
  thread's attention sooner than its current timeout would otherwise allow, without adding any
  locking inside TickLE itself.
- Liveliness timeout: a remote node that stops sending its periodic UPDATE announce entirely
  (not just an unchanged one - see `tt_Node`'s new `update_last_seen[]`, tracked separately from
  the existing content-change `update_last_modified[]`/`update_seen[]`) is now presumed gone
  after `tt_LIVELINESS_MISS_THRESHOLD` (config.h, default 3) consecutive announce intervals pass
  with nothing heard - same cleanup as an explicit farewell UPDATE (peer-table entries dropped,
  first-contact handling re-armed for a later announce from the same node id). `rmw_tickle/PLAN.md`'s
  Milestone 0(b) - previously a node that crashed or was network-partitioned without sending
  `tt_Node_destroy()`'s own farewell UPDATE lingered in every peer table forever.
- Opt-in discovery API for graph introspection (`rmw_tickle/PLAN.md`'s Milestone 0(c)):
  `tt_Node_set_discovery()` attaches a caller-owned `struct tt_Discovery` (capacity
  `tt_MAX_DISCOVERED_ENTITIES`, config.h, default 16) that records every remote Publisher/
  Subscriber/Client/Server any attached node hears announced - not just ones matching a local
  endpoint the way `struct tt_Peer`'s unicast-address tracking already is - queryable via
  `tt_Discovery_count()`/`tt_Discovery_find()`, with a `tt_DISCOVERY_CALLBACK` fired on
  appearance, refresh, or departure (an explicit farewell UPDATE, or Milestone 0(b)'s liveliness
  timeout). Deliberately not part of `struct tt_Node` itself (adds only 3 pointers there,
  ~24 bytes) - the actual entity table, easily several KB with real name/type strings, is owned
  by whoever opts in, so a `tt_Node` with nothing attached (including a possible future
  micro-ROS-style thin FreeRTOS client, whose *agent* rather than the constrained device itself
  would want this) doesn't pay for it.
- `tools/typesupport/tickle_typesupport/ros2_adapter.py` (`rmw_tickle/PLAN.md`'s Milestone 1(a)):
  generates a converter (`<ros_name>__to_tickle`/`__from_tickle`) between a real ROS 2 interface
  package's own `rosidl_generator_c` struct and this tool's own, already-generated, already-tested
  struct/codec for that same message - field-by-field copy plus bounds checks, reusing the existing
  `<Msg>_encode`/`_decode`/`_encode_size`/`_free` codec completely unchanged rather than
  regenerating CDR-4 logic a second time against ROS 2's struct shape. Verified fully offline (no
  ROS 2 install exists in this tool's own dev/test environment): `tests/fixtures_ros2_adapter/`'s
  hand-written stand-ins for `rosidl_generator_c`/`rosidl_runtime_c` let `tests/test_ros2_adapter.py`
  compile and round-trip the generated converter against the real TickLE codec, covering scalars,
  fixed arrays, bounded/unbounded variable arrays (including over-capacity rejection), and
  bounded/unbounded strings (including over-capacity rejection) - clang-tidy-clean and
  warning-free under the project's own `.clang-tidy`/`-Wall -Wextra`.
- `rosidl_typesupport_tickle_c` (`rmw_tickle/PLAN.md`'s Milestone 1(b)+(c), built together): a
  real, working ROS 2 typesupport package. Registers with `rosidl_generate_interfaces()`'s
  `rosidl_generate_idl_interfaces` extension point and as an `ament_index` `"rosidl_typesupport_c"`
  resource, so any ROS 2 interface package picks it up automatically just by
  `find_package(rosidl_typesupport_tickle_c)` before generating its own interfaces - no explicit
  second macro call needed. For each `.msg`, `python3 -m tickle_typesupport.ros2_cli` (new) writes
  TickLE's own codec, `ros2_adapter`'s converter, and a `rosidl_message_type_support_t` wrapper
  (`ros2_adapter.render_type_support()`, new) reachable through the exact standard
  `get_message_typesupport_handle()` dispatch chain a real `rmw_create_publisher()` call uses -
  proven, not just plausible: `rosidl_typesupport_tickle_c_tests` (new, a minimal real interface
  package) calls that dispatch chain directly in CI and gets a working handle back. `.msg` only for
  this first cut; `.srv`, nested-field converters, and packaging `tickle_typesupport` itself as an
  installable `ament_cmake_python` package are tracked as follow-on work in `rmw_tickle/PLAN.md`.
- `rmw_create_node()`/`rmw_destroy_node()`/`rmw_node_get_graph_guard_condition()`
  (`rmw_tickle/PLAN.md`'s Milestone 2, `rmw_tickle/src/rmw_node.c`): the first `rmw_tickle` entry
  points to actually drive a real TickLE node's lifecycle. A background thread loops
  `tt_Node_poll()`, holding a per-node mutex only around each individual call - every other entry
  point that will touch the node (`rmw_publish()` et al., Milestone 3+) is meant to
  `tt_Node_interrupt()` then lock that same mutex before doing so, the exact pattern
  `rmw_destroy_node()` itself uses to stop the poll thread cleanly. Only one node per process for
  now (`_tt_CONFIG`, `include/tickle/config.h`, is itself process-wide) - a second
  `rmw_create_node()` call fails loudly rather than silently colliding with the first; multiple
  ROS 2 nodes per process stays tracked as deferred work. Also filled in
  `rmw_get_serialization_format()`, missing from PR #20's original scaffold despite its
  identifier/macro already existing right next to it.
- `rmw_create_publisher()`/`rmw_publish()`, `rmw_create_subscription()`/`rmw_take(_with_info)()`,
  `rmw_serialize()`/`rmw_deserialize()` (`rmw_tickle/PLAN.md`'s Milestone 3, `rmw_tickle/src/
  rmw_publisher.c`/`rmw_subscription.c`/`rmw_serialize.c`/`rmw_typesupport.c`): `rmw_tickle` can
  now actually move messages. A subscriber's callback runs on the node's background poll thread
  and converts tickle->ros immediately (TickLE's own `decode()` aliases its receive buffer for
  string/array fields, valid only until the callback returns), pushing the result into a
  fixed-capacity per-subscriber queue `rmw_take()` pops from - QoS-driven real queue depth is
  still on the roadmap (below), this is a fixed placeholder. `rosidl_typesupport_tickle_c/
  message_type_support.h` gained `ros_type_name`/`ros_struct_size` fields so `rmw_tickle` can size
  and name things generically without a per-message struct definition of its own.
- `rosidl_typesupport_tickle_c` now generates `.srv` typesupport too (needed for
  `rmw_tickle/PLAN.md`'s Milestone 4 - `rmw_create_client`/`rmw_create_service`): a `.srv`'s
  Request and Response are each an ordinary ROS 2 message in their own right, so `ros2_cli.py`
  generates the same converter+message-typesupport pair for both (sharing one TickLE codec
  `<Name>.h/.c` pair, per `tools/typesupport`'s own existing `render_service()`), plus a new
  `ros2_adapter.render_service_type_support()` tying them into one `rosidl_service_type_support_t`
  via the standard `ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME` convention - new
  `rosidl_typesupport_tickle_c/service_type_support.h`.
- `rmw_create_client()`/`rmw_destroy_client()`/`rmw_send_request()`/`rmw_take_response()`
  (`rmw_tickle/src/rmw_client.c`) and `rmw_create_service()`/`rmw_destroy_service()`/
  `rmw_take_request()`/`rmw_send_response()` (`rmw_tickle/src/rmw_service.c`)
  (`rmw_tickle/PLAN.md`'s Milestone 4): `rmw_tickle` can now do services. The client side needs no
  special bridging - TickLE's own `tt_CLIENT_CALLBACK` is already the async "call completed"
  notification `rmw_take_response()`'s polling expects, the same pattern topic subscriptions
  already use. The server side does: TickLE's `tt_SERVER_CALLBACK` must synchronously fill a
  response and return, with no "send it later" API, while ROS 2's rmw contract splits that into
  two independent calls a real rclcpp handler makes back-to-back - `server_callback()` bridges the
  two with a bounded `pthread_cond_timedwait()` (`RMW_TICKLE_SERVICE_RESPONSE_TIMEOUT_NS`, 5s),
  blocking the whole node's poll thread for that call's duration (near-zero cost for the
  synchronous single-threaded-executor pattern this targets first; documented, not solved, for a
  slow/deferred handler). Both sides mirror TickLE's own one-outstanding-call-at-a-time limit
  directly rather than queueing several in-flight requests per client.
- `rmw_create_wait_set()`/`rmw_destroy_wait_set()`/`rmw_wait()` (`rmw_tickle/src/rmw_wait_set.c`,
  new) and `rmw_create_guard_condition()`/`rmw_destroy_guard_condition()`/
  `rmw_trigger_guard_condition()` (`rmw_tickle/src/rmw_guard_condition.c`, new)
  (`rmw_tickle/PLAN.md`'s Milestone 5): one condition variable per context, shared by every
  waitable entity (subscriber queues, client responses, service requests, guard conditions) -
  `rmw_wait()` holds it continuously across its own check-every-entity pass and into the actual
  wait call, while every producer takes it only after releasing its own entity-local lock just to
  broadcast, closing the classic lost-wakeup race between the two. `rmw_node_get_graph_guard_
  condition()`'s own guard condition is now a real, safely-waitable one (previously `.data ==
  NULL`, which would have crashed the moment any wait set checked it) - actually triggering it on
  a graph change is still Milestone 6.
- `rmw_get_node_names()`/`rmw_get_node_names_with_enclaves()`/`rmw_count_publishers()`/
  `rmw_count_subscribers()`/`rmw_service_server_is_available()` (`rmw_tickle/src/rmw_graph.c`,
  new), plus the graph-changed guard condition now actually firing on a real graph change
  (`rmw_node.c`'s `discovery_callback()`, wired into TickLE's own opt-in discovery API - Milestone
  0(c)) (`rmw_tickle/PLAN.md`'s Milestone 6). `rmw_count_publishers()`/`_subscribers()`/
  `rmw_service_server_is_available()` scan both TickLE's own remote-only discovery table and this
  node's own local endpoint table, since the former never records this process's own
  locally-created publishers/subscribers/servers. Documented gap: TickLE's wire protocol has no
  node-name concept at all, so `rmw_get_node_names()` can only ever report the local node itself,
  never other nodes actually visible on the segment.

## [1.0.0] - 2026-09-14

First tagged release.

### Added

- Library version macros (`TICKLE_VERSION_MAJOR/MINOR/PATCH/STRING`, `TICKLE_VERSION`,
  `TICKLE_VERSION_MAKE`) and a `tt_version()` accessor.
- `make install` / `make uninstall` (static lib + headers + `tickle.pc`); `PREFIX` / `DESTDIR`
  overridable.
- `tt_RET_INVALID_ARGUMENT` and `tt_CALL_TIMEOUT`.
- Full cross-endian support: a receiver now byte-swaps every framing field (submessage length,
  endpoint ids, sequence numbers, timestamps, string-length prefixes) when the peer's
  `tt_Header` magic says it serialized in the opposite byte order. `tt_hash_id()` is now
  endianness-independent (byte-at-a-time FNV-1a).
- `tests/test_cross_endian.c`; `tests/fuzz_process_packet.c` + `make fuzz`; `make sanitize`
  (ASan + UBSan unit-test run).
- `tt_hash_id()` collision test and reverse-endian string-decode test.
- `tools/typesupport`: generates a message/service's `<Name>.c`/`<Name>.h` codec directly from
  its `.msg`/`.srv` (ROS 2's own interface grammar) instead of hand-writing it - see
  `tools/typesupport/README.md` and DESIGN.md's "Interface serialization (TickLE CDR-4)" for the
  wire format it implements. Every one of TickLE's own example interfaces
  (`examples/{uint64,set_bool,ping_pong,perf}/*.msg`/`*.srv`) is generated this way now; `make
  regen` re-runs it in place (see CONTRIBUTING.md's "Generated codecs"). A bounded string
  (`string<=N`, or a plain `string` with an explicit `# @capacity <N>` annotation) gets a real
  `char[N+1]` buffer and a capacity check on encode/decode, the same as a variable array's
  capacity - previously it silently generated the same alias-only `char*` a plain, unbounded
  `string` does (see DESIGN.md's "Capacity" rule).
- Discovery-learned unicast: a server's `CallResponse` unicasts straight back to its
  `CallRequest`'s own sender instead of broadcasting an answer the rest of the segment never
  asked for, and a `tt_Publisher`/`tt_Client` now does the same to each Subscriber/Server it
  learns of via the periodic UPDATE announce, once it knows `tt_UNICAST_PEER_THRESHOLD` (default
  2) or fewer of them - falls back to today's broadcast once there are more, or none yet known.
  New `struct tt_Peer` and `tt_UNICAST_PEER_THRESHOLD` / `tt_MAX_PEER_COUNT` (`config.h`); see
  DESIGN.md's "Discovery-learned peers: unicast to a few, broadcast to the rest".

### Changed

- `tt_NODE_UPDATE_INTERVAL` is 1s (was a 10s "temporary, for debugging" value).
- `tt_CallRequestHeader` is 8 bytes, not 7 (a `reserved` pad byte) - keeps a CALLREQUEST's CDR
  payload 4-byte aligned, matching CALLRESPONSE/DATA. Wire-incompatible with pre-1.0 builds; not
  a concern before the first tagged release.
- A fully unanswered RPC reports `return_code == tt_CALL_TIMEOUT` to the client callback instead
  of `0` (which a server can legitimately return). A `tt_SERVER_CALLBACK` must not return that
  value.
- Every public entry point validates its arguments (NULL pointers, missing required
  `tt_Service`/`tt_Topic` fields, message sizes outside `(0, tt_MAX_BUFFER_LENGTH]`) and returns
  `tt_RET_INVALID_ARGUMENT` instead of dereferencing them.
- The public headers compile clean under `-std=c11 -pedantic-errors` (`tt_Request` / `tt_Response`
  / `tt_Data` are no longer empty structs). C11 is required (anonymous union in `tt_Header`).

### Fixed

- `process_callresponse()` ignores a response that doesn't match the outstanding call
  (`client->cache == NULL`, or a `seq_no` mismatch) - previously a duplicate or late response
  invoked the client callback twice and corrupted the latency estimate.
- `tt_Node_destroy()` now actually transmits its final "leaving" UPDATE; the receive side
  (`process_update()`) drops a source's peer-table entries when its announce no longer lists
  them, so a departed node no longer lingers forever.
- An `ACKNACK` or unknown submessage type is skipped, not treated as fatal to the whole
  datagram - forward-compatible with newer protocol revisions.
- `tt_hash_id()` no longer does unaligned `uint32_t` reads of the name strings (a fault on
  strict-alignment targets).
- The library no longer allocates: `process_update()`'s per-source `malloc()` is replaced by
  two fixed arrays on `tt_Node`. Nothing in `src/` calls `malloc`/`free` now.

### Known limitations

- Delivery is best-effort. `tt_Topic`'s `history_depth` / `deadline_duration` /
  `lifespan_duration` and the `tt_RELIABLE_*` knobs are reserved for a future reliable-QoS
  release and are ignored.
- One `tt_Node` is single-threaded; drive it from one thread.
