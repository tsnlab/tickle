# rmw_tickle

`rmw_tickle` is a ROS 2 middleware (`rmw`) implementation backed by TickLE. Select it with
`RMW_IMPLEMENTATION=rmw_tickle`; `rclcpp` and `rclpy` applications then run over TickLE's own wire format instead of
DDS. The package lives in `rmw_tickle/rmw_tickle/`, with its typesupport in `rmw_tickle/rosidl_typesupport_tickle_c`
and `rmw_tickle/rosidl_typesupport_tickle_cpp`.

- Linux only. CI builds against ROS 2 jazzy. Version `0.0.1`, Quality Level 4
  ([QUALITY_DECLARATION.md](../rmw_tickle/rmw_tickle/QUALITY_DECLARATION.md)).
- Core stays embedded-minded (no malloc, small footprint). `rmw_tickle` is not bound by that: it uses threads and
  the caller's `rcutils` allocator wherever they help.
- Anything it cannot honour is refused explicitly (`RMW_RET_UNSUPPORTED` with an error message), never silently
  downgraded.

Core design is in [DESIGN.md](DESIGN.md), measured results in [RESULTS.md](RESULTS.md), the test suites in
[TESTING.md](TESTING.md), and planned work in [ROADMAP.md](ROADMAP.md).

## API coverage

Of the 94 functions `rmw_implementation` dispatches on jazzy, `rmw_tickle` serves 77. The remaining 17 are 6 loaned-message
functions (a gap) and 11 features TickLE has no counterpart for. Checked function by function against each
implementation's source on 2026-09-27, and updated as gaps closed.

| Area | functions | rmw_tickle | Fast DDS | CycloneDDS |
|---|---:|---:|---:|---:|
| Init/context, nodes, graph, publishers, subscriptions, services, waiting, QoS | 62 | 62 | 62 | 62 |
| Serialization | 7 | 7 | 6 | 6 |
| Events and on-new-data callbacks | 8 | 8 | 8 | 8 |
| Loaned messages | 6 | **0 (gap)** | 6 (shared memory only) | 6 (shared memory only) |
| Pre-allocation | 4 | – | – | – |
| Content filter, network flow endpoints, dynamic messages | 7 | – | 7 | – |
| **Fully supported** | **94** | **77** | **83** (+6 loans with shared memory) | **76** (+6 loans with shared memory) |

`rmw_get_serialized_message_size` is the one entry point `rmw_tickle` serves and neither vendor does.

## What works, and what does not

| Feature | Status | Note |
|---|---|---|
| Publish / subscribe, `rmw_take`, `rmw_take_with_info`, `rmw_take_sequence` | ✅ | `publisher_gid` in message info identifies the writer |
| Services and clients | ✅ | **One outstanding request at a time** per client and per service. A second request is refused |
| Actions (`rclcpp_action`) | ✅ | Built from the implicit messages and services. CI runs Fibonacci across two processes |
| Serialized messages (`rmw_publish/take_serialized_message*`, `rmw_serialize`) | ✅ | Served by the direct codec. `ros2 topic echo --raw` works. Format string is `"tickle"` |
| `ros2 bag record` / `ros2 bag play` | ✅ | The acceptance test replayed 77 samples, the same as CycloneDDS |
| All 11 event types | ✅ | Deadline, liveliness, QoS-incompatible, incompatible-type, matched, message-lost |
| On-new-data callbacks (EventsExecutor) | ✅ | One call per item. Whatever is already queued is reported when the callback is set |
| Graph: node, topic and service names and types, counts, `*_info_by_topic` | ✅ | Remote nodes included (wire v11). Reported QoS profiles are ones `rmw_tickle` accepts back |
| `*_info_by_service` (lyrical) | ✅ | Endpoint count 1 where vendors show 2; type hash is INVALID; remote QoS unknown |
| Several nodes in one process (composition, containers) | ✅ | Delivered in-process, with the same QoS rules as remote delivery. Nothing is sent twice |
| Several processes on one host | ✅ | Each claims its own context id through a registry in `/dev/shm` |
| `ROS_DOMAIN_ID` | ✅ | Port 8282 + domain id, maximum 232 |
| `ROS_AUTOMATIC_DISCOVERY_RANGE`, `ROS_STATIC_PEERS` | ✅ | Matches rmw_cyclonedds. `NOT_SET` refuses to create a node |
| Messages up to 64 KB | ✅ | Samples go out in 1472-byte DATA_FRAG fragments. Service requests and responses are not fragmented; the OS fragments them at the IP layer |
| Messages above 64 KB | ❌ | Planned (large-message stage 2) |
| Loaned messages (`rmw_borrow_loaned_message` + 5) | ❌ | `can_loan_messages` is always false. Planned on top of shared-memory receive-buffer lending |
| SROS2 security | ❌ | `ROS_SECURITY_ENFORCEMENT=Enforce` makes `rmw_init` fail. In permissive mode it warns once that security is not applied. Parked until the user starts it |
| Pre-allocation, content filters, network flow endpoints, dynamic messages | ➖ | Not supported, by design. `RMW_RET_UNSUPPORTED`, as CycloneDDS does |
| `wstring` | ➖ | Not supported, by decision. Only `example_interfaces/msg/WString` is affected; use UTF-8 `string` |
| Multi-dimensional arrays, 128-bit types, `string<=N[]`, defaults on nested fields | ➖ | Refused when the code is generated, not at run time |
| Unbounded sequences | ⚠️ | Each gets a fixed capacity (e.g. `LaserScan` 4096 beams, `Image` 64,000 B). Override with a `.capacities` file. A longer sequence fails to publish |
| Introspection typesupport | ➖ | Only the C and C++ TickLE typesupports are registered |

## How ROS 2 maps onto TickLE

| ROS 2 / rmw | TickLE | rmw_tickle glue |
|---|---|---|
| `rmw_context_t` | one `struct tt_Context` per context (socket, discovery, scheduler) | `rmw_tickle_context_impl_t`: poll thread, watchdog thread, graph guard condition, a single condvar shared by all waits, node registry |
| `rmw_node_t` | a `struct tt_Node` on the shared context, carrying the node's name and namespace | `rmw_tickle_node_t`: a thin wrapper. Nodes share the context's transport, as DDS participants are shared |
| `rmw_publisher_t` | `tt_Publisher` + `tt_Topic` | `rmw_tickle_publisher_t`: retained-sample cache sized by depth and a byte budget |
| `rmw_subscription_t` | `tt_Subscriber` + `tt_Topic` | `rmw_tickle_subscriber_t`: a bounded queue sized from depth, reorder slots for RELIABLE |
| `rmw_client_t` | `tt_Client` + `tt_Service` | `tt_CLIENT_CALLBACK` already is the async completion that `rmw_take_response` needs |
| `rmw_service_t` | `tt_Server` + `tt_Service` | The callback defers (`tt_CALL_DEFERRED`), and `rmw_send_response` answers with `tt_Server_send_response()` |
| Topic match rule (same name and type) | `tt_hash_id(type_name, topic_name)` | The ROS type name is `tt_Topic.name`, and the ROS topic name is the endpoint name |
| Graph | `struct tt_Discovery` (remote) + the context's endpoint table (local) | `rmw_graph.c` scans both. Any discovery change triggers the graph guard condition |
| Events | core timers (`tt_Context_schedule`) and discovery data | `rmw_event.c`. LIVELINESS_LOST comes from a separate watchdog thread, so a hung poll thread is still detected |
| Guard condition, wait set | none (rmw only) | An atomic flag plus the context's condvar. `rmw_wait` holds `wait_mutex` across check and wait, so no wakeup is lost |
| GID | context id + per-instance `entity_id` | Unique per endpoint, for local and remote endpoints alike |
| Typesupport | `tools/typesupport` CDR-4 codec | `rosidl_typesupport_tickle_c`/`_cpp`, registered as rosidl extensions and package-qualified so same-named types never collide |

### Threading

- Core is thread-safe, and `rmw_tickle` holds no lock of its own around it. Entry points call core directly and take
  `tt_Context_lock` only where rmw state has to agree with core's. Lock order is always the node lock, then
  `wait_mutex`.
- **Executor-driven receive** is on by default (`RMW_TICKLE_EXECUTOR_POLL=0` turns it off): a blocking `rmw_wait()`
  polls the socket itself, so a message is received and handed back on the executor's own thread. The poll thread
  takes over only when no executor has waited for more than 10 ms.
- A separate watchdog thread checks that the poll thread is still returning, and raises LIVELINESS_LOST if it stops.

## QoS

All six policies are supported for topics. `rmw_tickle_validate_qos_profile()` (`src/rmw_qos.c`) enforces the rules
below.

| Policy | Accepted | Note |
|---|---|---|
| RELIABILITY | BEST_EFFORT, RELIABLE, BEST_AVAILABLE (topics) | RELIABLE uses core ACKNACK, retransmission and Heartbeats. Services retry in core |
| DURABILITY | VOLATILE; TRANSIENT_LOCAL and BEST_AVAILABLE for topics | Late joiners get the backlog from the publisher's retained-sample cache |
| HISTORY / DEPTH | KEEP_LAST and KEEP_ALL, for both publishers and subscriptions | KEEP_ALL is bounded by a byte budget: a publisher blocks (`RMW_TICKLE_MAX_BLOCKING_MS`); a full subscription refuses new samples, so a RELIABLE writer resends them |
| DEADLINE | any finite value | Checked locally by timers |
| LIVELINESS | AUTOMATIC, MANUAL_BY_TOPIC, BEST_AVAILABLE (topics) | The lease counts from the last sign of life. Leases under 6 ms are refused |
| LIFESPAN | any finite value | Expires samples in both the publisher cache and the subscriber queue |

- **RESOURCE_LIMITS (for reference; not an rmw QoS policy).** ROS 2's QoS profile has no RESOURCE_LIMITS, but
  KEEP_ALL cannot be kept without one: in DDS, KEEP_ALL means "refuse rather than overwrite" and RESOURCE_LIMITS
  says when to refuse. rmw_tickle bounds it differently from DDS:

  | | DDS `ResourceLimitsQosPolicy` | rmw_tickle |
  |---|---|---|
  | unit | samples: `max_samples`, `max_instances`, `max_samples_per_instance` | bytes, plus fixed sample-count ceilings below |
  | where it is set | each writer's, reader's and topic's QoS | process-wide environment (`RMW_TICKLE_KEEP_ALL_BYTES`, `RMW_TICKLE_READER_KEEP_ALL_BYTES`, 512 KiB each); a publisher can override its own through `rmw_specific_publisher_payload` (`publisher_payload.h`); a subscription cannot |
  | instances | per key | none: rmw_tickle has no keyed topics |
  | writer bound | samples in the writer's history | unacknowledged bytes, and at most the readers' tracking window (1,024 samples) and the index ring (2,048 datagrams); TRANSIENT_LOCAL is bounded by its replay depth (8,192), not bytes |
  | reader bound | `max_samples` | the byte budget divided by one decoded ROS message plus its queue entry, at most 4,096 |
  | consistency check | `max_samples >= max_samples_per_instance >= HISTORY.depth`, refused at creation | none needed: KEEP_ALL has no depth, and a budget below one sample is raised to one |
  | memory | preallocated up to a limit (Fast DDS `allocated_samples`) or as needed | as needed, doubling toward the budget |
  | defaults | CycloneDDS: unlimited (the writer is held by its non-standard byte watermark `WhcHigh`, 500 kB); Fast DDS: 5,000 samples | 512 KiB per publisher and per subscription |

  When the bound is reached both behave alike: a KEEP_ALL publisher blocks up to `RMW_TICKLE_MAX_BLOCKING_MS` (DDS:
  `max_blocking_time`) and then fails the publish with a timeout; a RELIABLE subscription declines new samples, so
  the writer holds and resends them (DDS: the sample is rejected and resent); a BEST_EFFORT one drops and counts them.
- RxO matching covers RELIABILITY, DURABILITY, DEADLINE and LIVELINESS. An incompatible pair does not match, and both
  sides get an event.
- `BEST_AVAILABLE` is resolved once, when the endpoint is created, against the endpoints already discovered.
- Services and clients accept VOLATILE only.

## Build and install

`rmw_tickle` compiles TickLE core (`src/tickle.c`, `encoding.c`, `log.c`, `hal_linux.c`) straight into
`librmw_tickle`, so there is nothing separate to install. It defaults to a Release build.

```sh
source /opt/ros/$ROS_DISTRO/setup.bash
pip install ./tools/typesupport          # the code generator, once
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
  --cmake-args -DBUILD_SHARED_LIBS=ON     # shared, so rosidl can dlopen the typesupport
source install/setup.bash
```

**Standard interface packages need a one-time build.** The ones installed with ROS 2 have no TickLE typesupport, and
without it even a plain `rclcpp::Node` cannot start, because it creates `/rosout` and the parameter services.

```sh
rmw_tickle/scripts/build_ros2_interfaces.sh -w ~/tickle_ifaces_ws -a   # all 23 jazzy interface packages
source ~/tickle_ifaces_ws/install/setup.bash
```

- Build your own interface packages with the `rmw_tickle` install sourced.
- A package configured before the interface workspace existed has the stock packages in its CMake cache. Build it
  afresh once.
- `-DTICKLE_MAX_BUFFER_LENGTH=<N>` lowers the 65507-byte maximum. The interface packages must then be rebuilt, because
  a type generated for a different maximum is refused.

Compile-time sizing `rmw_tickle` sets for core: up to 256 nodes per context, 2048 endpoints and 2048 discovered
entities, and 4 MiB socket receive buffers. The kernel caps the buffer request at `net.core.rmem_max`, and the log
says so when it does.

### Runtime environment

| Variable | Effect | Default |
|---|---|---|
| `TICKLE_BROADCAST_ADDR` | broadcast address, and with it the interface used. **Set it**: unset means limited broadcast on the default route | limited broadcast |
| `TICKLE_NODE_ID` | fixed context id | claimed automatically |
| `RMW_TICKLE_EXECUTOR_POLL` | `0` turns executor-driven receive off | on |
| `RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY` | add a Heartbeat to every Nth RELIABLE sample; `0` turns it off | tracking window / 16 (64) |
| `RMW_TICKLE_HEARTBEAT_PERIOD_NS` | also send a periodic Heartbeat | off |
| `RMW_TICKLE_MAX_BLOCKING_MS` | longest a KEEP_ALL publish may block | 100 |
| `RMW_TICKLE_CACHE_BYTES` | KEEP_LAST publisher cache budget | 1 MiB |
| `RMW_TICKLE_KEEP_ALL_BYTES` / `RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES` | KEEP_ALL publisher budget, and the space reserved per sample | 512 KiB / 1472 B |
| `RMW_TICKLE_REORDER_SLOTS` | out-of-order RELIABLE samples a subscription may hold | the tracking window |

The two publisher storage budgets can also be set per publisher, with `rmw_tickle_publisher_payload_t`
(`rmw_tickle_c/publisher_payload.h`) passed through `rmw_publisher_options_t.rmw_specific_publisher_payload`.

## Performance

Cross-host round trip through `rclcpp`, with ping on one rig Raspberry Pi and pong on the other over eth0. All four rmw
implementations run in one session, 3 repetitions, median shown; the same application binary is used for every rmw.
Build `934f90de`; raw files `examples/perf_hil/results/rmw_4way_block_934f90de_2026-09-30.txt` and
`rmw_4way_poll*_cb_r7_934f90de_2026-10-01.txt` (7 repetitions). **Bold** marks a lead beyond 2xSE.

| Row | Metric (Bench = 64 B, Array1k = 1 KB) | rmw_tickle | Fast DDS | CycloneDDS | rmw_zenoh |
|---|---|---:|---:|---:|---:|
| 48 | RTT ms, block wait, Bench, BEST_EFFORT | **0.246** | 0.320 | 0.269 | 0.381 |
| 49 | RTT ms, block wait, Bench, RELIABLE | **0.247** | 0.335 | 0.269 | 0.386 |
| 50 | RTT ms, block wait, Array1k, BEST_EFFORT | **0.263** | 0.338 | 0.294 | 0.611 |
| 51 | RTT ms, block wait, Array1k, RELIABLE | **0.269** | 0.354 | 0.284 | 0.609 |
| 52-67 | RTT, busy poll and 50/100/200 us poll sleeps, random phase (16 rows) | 9 rows won, 7 tied | none won | 7 tied | none won |
| 68 | ping peak RSS, KB | **12,544** | 23,748 | 14,592 | 70,072 |
| 70 | pong CPU over the whole run, ms (BEST_EFFORT) | **37.4** | 57.7 | 43.5 | 91.8 |
| 71 | pong CPU over the whole run, ms (RELIABLE) | **37.2** | 60.4 | 48.5 | 94.1 |

- Every tie in rows 52-67 is with CycloneDDS, at the larger poll sleeps, where half a poll cycle of waiting dominates.
- Only the rig's numbers count. The dev PC is used to check correctness, never to measure performance.
- `buildfarm_perf_tests` can only run on one host, so no cross-vendor rmw throughput comparison is published.
- The generated converters now cost what a `memcpy` costs: a 64 KB `Image` through `rmw_serialize` takes 3.6 us,
  against 2.6 us for Fast DDS and 21.2 us for CycloneDDS (PC, 2026-09-27).

The full tables are in [RESULTS.md](RESULTS.md).

## Open items

- Loaned messages (6 entry points), built on shared-memory receive-buffer lending. This is the last gap in the API
  coverage table.
- Messages above 64 KB (large-message stage 2: wider fragment index, 32-bit record length).
- More than one outstanding request per client and per service.
- Round-trip checks for the other introspection values (names, type names, GIDs, event counts, serialization format):
  only QoS profiles are verified to be accepted back.
- `*_info_by_service`: report two endpoints per entry, a real type hash, and remote QoS.
- Shared-memory tests: the kill test (S5), the fair same-host comparison against both vendors (S6), a CI arm with the
  module on (S8), and both mixed-stream windows (S9).
- rmw behaviour tests (events, graph, QoS, the acceptance suite) run only in CI. No local gate covers them.
- A cross-vendor rmw throughput benchmark that can run across two hosts.
- SROS2 security: parked until the user starts it.
- Line and branch coverage tracking (none today; needed for a higher quality level).
