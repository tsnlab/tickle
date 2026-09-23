# Changelog

All notable changes to TickLE are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) for the library
(`TICKLE_VERSION_*` in `include/tickle/tickle.h`). The on-the-wire protocol has its own
number, `tt_VERSION`, which moves independently.

## [Unreleased]

### Added

- `rosidl_typesupport_tickle_c_message_callbacks_t` gains `tickle_max_encoded_size`: an upper bound
  on the type's encoded payload, generated per type, or
  `ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED` (0) when the type has none. **Adding a field
  changes the struct's layout**, so generated typesupport and `rmw_tickle` must be rebuilt
  together; a hand-written callbacks struct that omits it reads as "no bound" and behaves exactly
  as before, which is why the marker is 0 rather than a distinctive sentinel.
  `rmw_tickle` uses it to size a `KEEP_ALL` publisher's retained-sample arena, which previously had
  to assume TickLE's whole single-datagram ceiling for every type: a depth-8192 `TRANSIENT_LOCAL`
  publisher of a 76-byte type reserved about 12.06 MB and now reserves about 0.6 MB.
  A bound exists for any type whose variable-length fields all resolve to a capacity. It does not
  for one carrying a plain unbounded `string`, an array of strings, or a nested type containing
  either - such a string is a `char*` aliasing external memory with no capacity, and the generator
  deliberately doesn't auto-derive one, so there is nothing to bound it below
  `tt_MAX_STRING_LENGTH` (65535). `sensor_msgs/msg/Image` and `std_srvs/srv/SetBool` are both in
  that category and keep the ceiling. `RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES` now applies only to
  those types: it fills the gap the generator leaves rather than overriding a computed bound.

- `rmw_tickle`: `rmw_publish()` now applies real back-pressure for `HISTORY.KEEP_ALL` publishers
  instead of letting the promise quietly lapse. A `RELIABLE` + `KEEP_ALL` publisher sets core's
  `tt_Publisher.keep_all`, so core refuses a write (`tt_RET_WOULD_BLOCK`) rather than evicting a
  sample no matched subscriber has acknowledged; `rmw_publish()` turns that refusal into a bounded
  wait on the publisher's writable callback and returns `RMW_RET_TIMEOUT` if it expires, matching
  `rmw_publisher_wait_for_all_acked()`'s own existing use of that code. The wait is configurable
  per process through `RMW_TICKLE_MAX_BLOCKING_MS` (default 100; `0` means never block, refuse
  immediately), and the timeout's `rcutils` message names the topic and says what to do about it,
  because `rclcpp` flattens every non-`OK` rmw return into one generic exception type - the text is
  all an application author gets. Waiting never holds the per-node mutex: the poll thread needs it
  to process the very acknowledgements that end the wait. `rmw_tickle/PLAN.md`'s Phase 3 step 3.


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
- QoS rejection logic (`rmw_tickle/src/rmw_qos.c`, new `rmw_tickle_validate_qos_profile()`),
  called from `rmw_create_publisher()`/`_subscription()`/`_client()`/`_service()`
  (`rmw_tickle/PLAN.md`'s Milestone 7): anything the QoS roadmap hasn't implemented yet
  (`RELIABLE`, `TRANSIENT_LOCAL`, non-`AUTOMATIC` liveliness, a finite `deadline`/`lifespan`/
  `liveliness_lease_duration`, `HISTORY_KEEP_ALL`) is now rejected outright
  (`RMW_RET_UNSUPPORTED`) instead of silently ignored. `qos_profile->depth` now really sizes a
  subscription's queue (allocated at `rmw_create_subscription()` time) instead of a fixed
  placeholder capacity.
- Packaging (`rmw_tickle/PLAN.md`'s Milestone 8): `package.xml` gained `<member_of_group>
  rmw_implementation_packages</member_of_group>`, and `CMakeLists.txt` now calls
  `register_rmw_implementation("c:rosidl_typesupport_tickle_c")` - the real mechanism (not just
  documentation) behind `RMW_IMPLEMENTATION=rmw_tickle` selection. New `check-all.yml` step
  reproduces `rmw_implementation`'s own runtime loading (`dlopen("librmw_tickle.so")` +
  `dlsym("rmw_get_implementation_identifier")`) to prove selection actually works, not just that
  the ament_index marker's text looks right.
- `rmw_get_zero_initialized_context()` (`rmw_tickle/src/rmw_init.c`), missing from the #20-era
  scaffold the same way `rmw_get_serialization_format()` was (Milestone 2) - `rmw_init()`'s own
  real contract requires it, but nothing had actually *called* `rmw_init()` until Milestone 10's
  own tests did.
- `rmw_tickle/PLAN.md`'s Milestone 9: a "Concept mapping" table (ROS 2/`rmw` concepts ↔ TickLE
  ones) and a "Threading and locking model" summary added to PLAN.md, consolidating what was
  previously only scattered across individual milestones' own prose and code comments.
- A scoped test suite (`rmw_tickle/test/test_qos.c`, `test_node_lifecycle.c`,
  `test_guard_condition_wait.c`) and a real `colcon test` CI step (`rmw_tickle/PLAN.md`'s
  Milestone 10) - every QoS roadmap rejection `rmw_tickle_validate_qos_profile()` enforces, the
  first real end-to-end `rmw_init()`/`rmw_create_node()`/`rmw_destroy_node()` exercise, and
  Milestone 5's guard condition/wait_set design (including its edge-triggered "consumed once
  observed ready" behavior) proven for real rather than only compiled and linked. A full
  publish/subscribe/service round trip (a second node/peer) and the upstream `rmw_implementation`/
  `test_rmw_implementation` conformance suites remain out of scope - no live multi-process test
  infra exists yet for rmw_tickle, the same boundary every milestone through this one has kept.
- `rosidl_typesupport_tickle_cpp` (`rmw_tickle/PLAN.md`'s Milestone 11): closes a gap found while
  provisioning a benchmark comparing `rmw_tickle` against `rmw_fastrtps_cpp`/`rmw_cyclonedds_cpp`
  via `ros2/buildfarm_perf_tests` - `rclcpp::create_publisher<T>()`/`create_subscription<T>()`
  always start from the C++-level `"rosidl_typesupport_cpp"` dispatch, which `rosidl_typesupport_
  tickle_c` (Milestone 1) was never registered under, making it structurally unreachable from any
  real rclcpp C++ node. The new package registers `"rosidl_typesupport_cpp"` and delegates
  straight to the existing `rosidl_typesupport_tickle_c` symbol per message via a real link-time
  call. `rmw_typesupport.c` now tries the new identifier first, falling back to the plain `"_c"`
  one for lower-level/C-only callers. New `rosidl_typesupport_tickle_c_tests/test/test_dispatch_
  cpp.cpp` exercises the actual rclcpp-style entry point, closing the blind spot `test_dispatch.c`'s
  own C-only macro left open.
- `rmw_publisher_get_actual_qos`/`rmw_subscription_get_actual_qos`, `rmw_get_gid_for_publisher`,
  and `rmw_publisher_event_init`/`rmw_subscription_event_init` (`RMW_RET_UNSUPPORTED`, per `rmw/
  event.h`'s own documented contract) - found missing by the same benchmark: a real `rclcpp::
  Publisher`/`Subscription` construction calls all of these unconditionally, unlike most other
  optional `rmw_*()` extras `rclcpp` merely probes and tolerates the absence of.
- `.github/workflows/rmw-perf.yml` + `.github/scripts/README-rmw-perf.md`: a new self-hosted-
  runner (`tickle-perf` label) workflow comparing `rmw_tickle`/`rmw_fastrtps_cpp`/
  `rmw_cyclonedds_cpp` via `ros2/buildfarm_perf_tests`, `workflow_dispatch`-only for now.

### Changed

- **Breaking wire change, `tt_VERSION` 5 -> 6** (`rmw_tickle/PLAN.md`'s Phase 2 + prerequisite (b),
  one bump covering both). `tt_AckNackHeader` now carries `sender_entity_id`, the *sending*
  Subscriber's own entity id - it previously identified only the target Publisher, and every
  Subscriber matching one Publisher shares `endpoint_id` by construction, so two Subscriptions of
  one topic in one process were indistinguishable and the faster one's acknowledgement spoke for
  both. Its `bitmap` is variable length (`bitmap_words` + that many 64-bit words) instead of a
  fixed 256-bit field: a one-gap ACKNACK is 28 bytes rather than 44, and a wider tracking window
  costs nothing on the wire until gaps genuinely spread that far. `tt_UpdateEntity` carries the
  announcing entity's `entity_id`, and a Subscriber's own tracking window in what used to be
  `reserved[2]` (free - that padding already existed for CDR-4 alignment).

  A version mismatch is now refused in both directions (exact match, with a per-peer rate-limited
  log) where the check used to accept a *newer* peer's packet and parse it with the older layout.
  This only helps from version 6 onward; nothing is deployed, so the older asymmetry is accepted.

  API: `struct tt_PeerAck` is keyed by `(node_id, entity_id)` and sized by its own
  `tt_MAX_ACK_ENTRIES` (16); entries are claimed when a Subscriber matches, so a matched-but-silent
  one already counts as "hasn't acknowledged", and a full table refuses the match rather than
  matching a Subscriber whose acknowledgements could never be counted. Read it through
  `tt_Publisher_is_acked_by_all_peers()` / `tt_Publisher_min_acked_seq_no()`, never by pairing it
  with `peers[]` by index. New `tt_Publisher_unacked_bound()` reports the narrowest tracking window
  across matched Subscribers.

  `struct tt_Subscriber` gains `tracking_bitmaps`/`tracking_words`: a caller-owned RELIABLE
  tracking window, `tt_MAX_PEER_COUNT * tracking_words` words, clamped to
  `tt_RELIABLE_BITMAP_MAX_BITS` (4096). NULL/0 keeps the `tt_RELIABLE_BITMAP_BITS` default, which
  stays 256 because TickLE core is embedded-first; `rmw_tickle` asks for 1024. At TickLE's own
  measured maximum rate a 256-sample window lasts ~1.35 ms - shorter than one retry interval plus a
  round trip, which is what left an occasional burst unrecoverable.

  Size the window **at or below the matched Publisher's retained depth**; it is not a
  bigger-is-better knob. Tracking further back than the Publisher still holds cannot recover
  anything, and recovers strictly *less* than a narrower window: against a depth-1024 Publisher at
  maximum rate, a 1024-sample window lost nothing across 6 runs while a 4096-sample one lost
  191-368 per run, every loss being a sample the Publisher had already evicted. A Publisher now
  logs a warning when a matching Subscriber announces a window deeper than it retains.

- **Breaking API change** - `struct tt_ReliableCache` now stores a retained sample's encoded bytes
  in a caller-provided byte arena instead of a fixed `tt_MAX_BUFFER_LENGTH` buffer per slot
  (`rmw_tickle/PLAN.md`'s B1). `struct tt_ReliableCacheEntry` is replaced by
  `struct tt_ReliableCacheIndex` (metadata only: seq_no, arena offset, length, retry, timestamp),
  and the cache gains `arena`/`arena_size` next to `index`/`capacity`/`depth`. A caller sets the
  five fields itself or calls the new `tt_ReliableCache_init()`, sizing the arena with
  `tt_RELIABLE_CACHE_ARENA_BYTES(depth, max_record)` / `tt_RELIABLE_RECORD_BYTES(payload)`
  (config.h). The old field names are gone rather than deprecated, so an un-migrated caller fails
  to compile instead of silently misbehaving. Behavior is unchanged - same KEEP_LAST-by-count
  retention, same O(1) lookup, same wire format (`tt_VERSION` untouched) - but a 76-byte sample
  now costs ~100 bytes of cache instead of 1488: depth 1024 drops from 1.45 MB to ~124 KB per
  Publisher, and `rmw_tickle`'s own KEEP_ALL depth (8192) from 12.2 MB to ~1 MB. Samples larger
  than the whole arena are published but not retained (a warning, and an ACKNACK for one gets the
  eviction Heartbeat), and `depth` may no longer change once anything is cached.

- `rmw_tickle_validate_qos_profile()` now takes an `rmw_tickle_entity_kind_t` instead of a plain
  `bool is_subscription`, and accepts `RELIABLE` for services/clients specifically (backed by
  `tt_Client_call()`'s own existing bounded retry) - a real `rclcpp::Node` unconditionally creates
  internal services (e.g. the type description service) at `RELIABLE` with no way to opt out, so
  rejecting it there would make `rmw_tickle` unable to host any real rclcpp node at all. Still
  rejected for publishers/subscriptions (topics have no retry mechanism at all).
- `rmw_tickle/CMakeLists.txt`: `ament_target_dependencies()` (fully removed on some newer ROS 2
  distros) replaced with modern imported targets, matching real `rmw_cyclonedds_cpp`'s own style.
- `include/tickle/tickle.h`: `_Alignas` (a C11 keyword, not valid C++) replaced with a portable
  `tt_ALIGNAS()` macro so the header stays includable from C++ translation units.
- `tt_Publisher_publish()` now flushes every call immediately by default (`tt_Publisher.batch`,
  new field, defaults to `false`) instead of always batching until `node_flush()`'s next
  `tt_NODE_TX_INTERVAL` (1ms) tick - see DESIGN.md's "RPC and Publish flush immediately by
  default; batching is opt-in" for the measured ~9x real `rmw_tickle` round-trip latency drop
  this produced (0.44ms → 0.048ms average, two-process). Set `pub->batch = true` on a specific
  Publisher to opt back into the previous always-batch behavior - measured cost of *not* doing
  that for a genuinely high-rate, many-small-messages stream: throughput fell ~4.4x (16-byte
  messages, uncapped rate, `examples/linux/perf`). `tt_receive()` (`hal_linux.c`) switched from
  `poll()` to `ppoll()` for the same reason from the other direction: `poll()`'s own timeout
  is a whole millisecond, silently rounding any shorter wait *up* to 1ms; `ppoll()` takes a real
  nanosecond-resolution `struct timespec`, so a sub-millisecond scheduled tick (`node_flush()`'s
  own, in particular) no longer waits for a full millisecond it never asked for.
- `examples/linux/perf/perf_client.c`: new `-B` flag, `tt_Publisher.batch` exposed on the command
  line (defaults off, matching the new default above) - the reproducible way to see both sides of
  that same tradeoff. Confirmed again on real hardware: an uncapped (`-i 0`), small (`-s 16`)
  flood without `-B` doesn't just send slower once discovery switches to per-message unicast, the
  receiver saw 100% loss; `-B` restored both throughput (~4x) and reliability (0.7% loss) by
  coalescing the same flood into far fewer, larger packets.

### Fixed

- `tt_Publisher_unacked_bound()` no longer clamps a matched subscriber's announced RELIABLE
  tracking window to `tt_RELIABLE_BITMAP_BITS` (256). It is documented as the narrowest window
  across matched subscribers, but was implemented as "start at the protocol default and let peers
  lower it", so a subscriber announcing a *wider* window was silently ignored. `rmw_tickle`
  announces 1024 samples on every subscription, so in the configuration that actually ships, a
  `KEEP_ALL` publisher blocked after 256 unacknowledged samples rather than the 1024 its peers
  could genuinely recover from - a quarter of the intended in-flight depth. Every existing core
  test used windows narrower than the default, which is exactly why none of them caught it; found
  by `rmw_tickle`'s new `test_keep_all_blocking.c` counting the writes it got before the refusal.


- `rmw_tickle`'s own `poll_thread_main()` no longer starves `rmw_publish()` (and every other
  entry point sharing the same per-node mutex) for tens to hundreds of milliseconds at a time
  under sustained load - found by getting `rmw-perf.yml`'s benchmark to actually produce non-zero
  numbers (a passing two-process run had silently exchanged zero messages the whole time).
  `poll_thread_main()`'s own `lock()` -> `tt_Node_poll()` -> `unlock()` loop re-locked the same
  mutex again immediately with no gap; glibc's mutex makes no fairness guarantee, and a thread
  re-locking a mutex it just released can win the race against a different, already-waiting
  thread's futex wake+reschedule far more often than intuition suggests, especially at this call
  pattern's high frequency (~1000/s) and short critical section. A real (measured, not guessed)
  `nanosleep()` between `unlock()` and the next `lock()` fixes it - `sched_yield()` does not
  (Linux's CFS scheduler treats it as close to a no-op).
- `TICKLE_NODE_ID` environment variable (`rmw_init.c`, mirroring the existing
  `TICKLE_BROADCAST_ADDR`): two `rmw_tickle` nodes running on the *same host* (e.g. `buildfarm_
  perf_tests`' own "two-process" test topology) previously derived the identical node id
  (`tt_get_node_id()`'s own auto-detect uses the last octet of the host's address - correct for
  two real separate hosts, not two processes sharing one), causing each to silently discard
  100% of the other's traffic as "self sent" - a real, silent, zero-throughput failure with no
  crash or error message. `examples/linux/*`'s own standalone binaries already had a `-I
  node_id` CLI flag for exactly this; this is the equivalent for anything (like an rmw plugin)
  with no CLI of its own to extend.

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
