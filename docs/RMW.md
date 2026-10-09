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

Of the 94 functions `rmw_implementation` dispatches on jazzy, `rmw_tickle` serves 83. The remaining 11 are features
TickLE has no counterpart for. Checked function by function against each
implementation's source on 2026-09-27, and updated as gaps closed.

| Area | functions | rmw_tickle | Fast DDS | CycloneDDS |
|---|---:|---:|---:|---:|
| Init/context, nodes, graph, publishers, subscriptions, services, waiting, QoS | 62 | 62 | 62 | 62 |
| Serialization | 7 | 7 | 6 | 6 |
| Events and on-new-data callbacks | 8 | 8 | 8 | 8 |
| Loaned messages | 6 | 6 (plain fixed-size types) | 6 (shared memory only) | 6 (shared memory only) |
| Pre-allocation | 4 | – | – | – |
| Content filter, network flow endpoints, dynamic messages | 7 | – | 7 | – |
| **Fully supported** | **94** | **83** | **83** (+6 loans with shared memory) | **76** (+6 loans with shared memory) |

`rmw_get_serialized_message_size` is the one entry point `rmw_tickle` serves and neither vendor does.

## What works, and what does not

| Feature | Status | Note |
|---|---|---|
| Publish / subscribe, `rmw_take`, `rmw_take_with_info`, `rmw_take_sequence` | ✅ | `publisher_gid` in message info equals the writer's own `rmw_get_gid_for_publisher` and its graph gid (`test_gid_two_process`) |
| Services and clients | ✅ | **One outstanding request at a time** per client and per service. A second request is refused. `request_id.writer_guid` is the client's gid on both sides; zero on the server for a request that beats the client's announce, or when one context has two clients of the service |
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
| Loaned messages (`rmw_borrow_loaned_message` + 5) | ✅ | For types whose wire bytes are their message in memory (Array1k, `geometry_msgs/Pose`, `std_msgs/Float64`: 67 generated types of the shipped set); `can_loan_messages` is false for the rest. Where copies remain: [Loaned messages](#loaned-messages) |
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
| GID | context id + per-instance `entity_id` | Unique per endpoint, for local and remote endpoints alike. Known core gap: two writers of one topic in one remote context share an `endpoint_id`, and discovery keys on `(context_id, endpoint_id)`, so the graph lists one of them |
| Typesupport | `tools/typesupport` CDR-4 codec | `rosidl_typesupport_tickle_c`/`_cpp`, registered as rosidl extensions and package-qualified so same-named types never collide |

### Threading

- Core is thread-safe, and `rmw_tickle` holds no lock of its own around it. Entry points call core directly and take
  `tt_Context_lock` only where rmw state has to agree with core's. Lock order is always the node lock, then
  `wait_mutex`.
- **Executor-driven receive** is on by default (`RMW_TICKLE_EXECUTOR_POLL=0` turns it off): a blocking `rmw_wait()`
  polls the socket itself, so a message is received and handed back on the executor's own thread. The poll thread
  takes over only when no executor has waited for more than 10 ms.
- A separate watchdog thread checks that the poll thread is still returning, and raises LIVELINESS_LOST if it stops.

### Loaned messages

**Which types.** A type lends when its wire bytes are its ROS message in memory: every field at the same offset in
the CDR-4 wire form (8-byte scalars 4-aligned) as in the C struct (8-aligned), recursively, with no string, sequence or
bool. The generator emits that as a compile-time expression of the C compiler's own `offsetof()`
(`inplace_bytes` in the callbacks struct; the C++ shim keeps it only for a trivially copyable, standard-layout object of
the same size). 67 of the types generated for the shipped interface set qualify (service and action parts
included), among them Array1k, `builtin_interfaces/Time`, `geometry_msgs/Pose`, `Twist`
and `Accel`; `std_msgs/Header` (a string) and anything with a sequence do not. **Interface packages must be rebuilt**:
the callbacks struct grew two fields, and `rmw_tickle` refuses one of the old size by name.

**Subscription.** `rmw_take_loaned_message` hands out the queued message itself, so a take copies nothing and
allocates nothing (an ordinary `rmw_take` copies it into the caller's message, and rclcpp allocates that message):
- a sample in a **receive buffer** (the socket path: another host, or before the ring attaches) is kept where it
  arrived (`tt_Sample_retain`) and lent as those bytes. rmw_tickle attaches 8 receive buffers (`tt_Context_set_rx_pool`,
  64 KiB each) when the context's first loaning subscription is created;
- a sample from the **shared-memory ring** is decoded into a pooled shell, as for any subscription, and the shell is
  lent: one copy, in the delivery. Keeping it in its ring slot instead (`RMW_TICKLE_LOAN_RING_SLOTS=1`) saves that copy
  too, but a held slot stops its ring one lap later (512 records) for **every** topic into the context, for as long as
  the application holds the loan (core DESIGN.md 10; `test_loaned_messages` held-pinned shows it: the ring refuses the
  writer and its KEEP_ALL publish times out). So it is off by default, and a held loan blocks nothing (`held`: every
  one of 1,600 flood samples delivered on another topic while three loans were held, 0 refusals);
- at most 8 queued samples per subscription stay in a buffer (core holds 16 per context); the rest are decoded;
- a byte-swapped sample, a fragmented, reorder-released or locally delivered one, or one not aligned for its type is
  decoded, never lent as is. Every receive buffer is 8-aligned (core's `rx_buffer` since 2026-10-09), so a message
  alone in its datagram lands aligned for 64-bit members: on the PC socket path 296 of 300 Array1k samples are read in
  place, where 8 of 300 were while `rx_buffer` sat 4 bytes off 8. A sample sharing its datagram may still land off
  its alignment and is decoded.

**Publisher.** `rmw_borrow_loaned_message` lends a buffer the publisher keeps (at most 64 out at once), so rclcpp
builds the message there instead of allocating one. A new buffer is initialised as a new message is; a kept one is
lent as its last loan left it (clearing it would write as many bytes as the copy a loan saves), so set every field,
defaults included. `rmw_publish_loaned_message` then encodes it into the ring slot (encode-in-slot) or `tx_buffer`
exactly as `rmw_publish` does: **one copy remains on the publish**.

**Slot loans (`RMW_TICKLE_LOAN_PUBLISH_SLOTS=1`, off by default).** The borrow lends the subscriber's ring slot itself
(core's `tt_Publisher_claim`, DESIGN.md 10), when the publisher sends to one same-host subscriber whose ring is
attached, has no other loan out, and the slot holds the message aligned; otherwise a kept buffer, as above. The
publish then writes the psn ahead of the message and sends the slot: **no copy on the publish** (BEST_EFFORT; RELIABLE
still copies into its retention cache). With ring-slot takes (`RMW_TICKLE_LOAN_RING_SLOTS=1`) the subscriber reads it
in that same slot: `test_loaned_messages` claimed, 286 of 300 built in a slot and 296 of 300 read in place, 0 copies
between them. The slot is lent as the ring left it (another record's bytes): set every field. Off by default because
**a claimed slot stops its ring** for every writer into that subscriber until it is published or returned, and
because **while a slot loan is out, every other publish of that publisher fails** (`RMW_RET_ERROR`, core holds one
claim per publisher and the claim's sequence number is taken at its publish): publish or return the loan first.

`RMW_TICKLE_LOANS=0` turns loans off (`can_loan_messages` false everywhere). rcl's `ROS_DISABLE_LOANED_MESSAGES=1`
does the same from above.

**Measured** (PC only, rmw level, Array1k at max rate, 8 reps, RESULTS.md "Loaned messages (PC)"): no difference the
reading rule can see, in either direction, between loans and plain publish/take - the PC's own scatter (other jobs
running) is wider than any effect. Medians: BEST_EFFORT with ring slots lent +8% delivered and -10% subscriber CPU per
message, RELIABLE -5% and +6%; the default loan arm (decoded shells) within a few percent of plain take, its
publisher's median CPU +10% (RELIABLE) and +33% (BEST_EFFORT). A rig A/B is what would decide it.

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
  The subscription asks for what it declined on its retry timer, so a writer that has stopped publishing still gets
  asked (since 2026-10-09; before, the samples declined after the writer's last Heartbeat were never asked for).
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
| `TICKLE_BROADCAST_ADDR` | broadcast address, and with it the interface used. Unset means the limited broadcast on the default route; since 2026-10-06 two processes on one host still find each other and use shared memory then (before, they only did with this set) | limited broadcast |
| `TICKLE_NODE_ID` | fixed context id | claimed automatically |
| `RMW_TICKLE_EXECUTOR_POLL` | `0` turns executor-driven receive off | on |
| `RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY` | add a Heartbeat to every Nth RELIABLE sample; `0` turns it off | tracking window / 16 (64) |
| `RMW_TICKLE_HEARTBEAT_PERIOD_NS` | also send a periodic Heartbeat | off |
| `RMW_TICKLE_MAX_BLOCKING_MS` | longest a KEEP_ALL publish may block | 100 |
| `RMW_TICKLE_CACHE_BYTES` | KEEP_LAST publisher cache budget | 1 MiB |
| `RMW_TICKLE_KEEP_ALL_BYTES` / `RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES` | KEEP_ALL publisher budget, and the space reserved per sample | 512 KiB / 1472 B |
| `RMW_TICKLE_REORDER_SLOTS` | out-of-order RELIABLE samples a subscription may hold | the tracking window |
| `RMW_TICKLE_LOANS` | `0` turns loaned messages off | on |
| `RMW_TICKLE_LOAN_RING_SLOTS` | `1` lends samples in their shared-memory ring slot; a held one then stops the ring a lap later | off |

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
- **Same host** (one rig Pi, each rmw on its shipped transport, build `06dd78d8`, 2026-10-09, 3 reps): rmw_tickle
  wins all 52 scored rows against both DDS rmws, e.g. RTT mean 40.0 us (Bench, RELIABLE, block) against 151.2 and
  112.7, 140,875 delivered Array1k samples/s at BEST_EFFORT KEEP_LAST 1 against 17,483 and 70,331, and peak RSS
  13.4-16.0 MB against CycloneDDS's 15.2-18.3 MB (since `718220d8`; at `a7e02807` six peak-RSS rows were DRAW or
  LOSE). Rows R1-R21 in RESULTS.md.
- A cross-host rmw throughput comparison exists only for RELIABLE + KEEP_ALL (rows 72-75).
- The generated converters now cost what a `memcpy` costs: a 64 KB `Image` through `rmw_serialize` takes 3.6 us,
  against 2.6 us for Fast DDS and 21.2 us for CycloneDDS (PC, 2026-09-27).

The full tables are in [RESULTS.md](RESULTS.md).

## Open items

- Loaned messages: core changes that would make more of them zero-copy - `tt_Context.rx_buffer` aligned to 8 (an
  8-aligned type landing there is copied today), a ring that skips a held slot (so ring slots could be lent safely),
  and a claimed-slot API for the publisher.
- Messages above 64 KB (large-message stage 2: wider fragment index, 32-bit record length).
- More than one outstanding request per client and per service.
- Round-trip checks for the introspection values: QoS profiles (`test_reported_qos`), and names, type names, counts,
  `*_info_by_topic` rows, GIDs and the serialization format across two processes (`test_introspection_two_process`,
  `test_gid_two_process`; no mismatch found) are done. Open: event counts across processes, and two writers of one
  topic in one remote context, which the graph lists as one (core discovery keys on `endpoint_id`).
- `*_info_by_service`: report two endpoints per entry, a real type hash, and remote QoS.
- Shared-memory tests: the kill test (S5), the fair same-host comparison against both vendors (S6), a CI arm with the
  module on (S8), and both mixed-stream windows (S9).
- rmw behaviour tests still CI-only: the upstream conformance suite (`test_rmw_implementation`), the interface
  workspace controls and the direct-codec identity harness. The ctest suite and `check_ros2_interfaces.sh`'s
  pub/sub, rclcpp and action cases run locally in a private netns (`make test-rmw-behaviour`, and the
  `rmw suite (as CI)` gate). The acceptance suite (`rmw_gap_acceptance.sh`) is run by hand.
- A cross-vendor rmw throughput benchmark that can run across two hosts.
- SROS2 security: parked until the user starts it.
- Line and branch coverage tracking (none today; needed for a higher quality level).
