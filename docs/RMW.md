# rmw_tickle

**TL;DR**
- A ROS 2 `rmw` on TickLE: `RMW_IMPLEMENTATION=rmw_tickle`. Linux, jazzy, 83 of 94 rmw functions, Quality Level 4.
- Build the three packages, then the standard interface packages once (below).
- It does not talk to DDS. Unsupported features are refused with `RMW_RET_UNSUPPORTED`, never silently downgraded.
- Knobs: [Options you may tune](#options-you-may-tune); DDS differences: [below](#how-tickle-differs-from-dds).

Older text: `git show 0a7926a0:docs/RMW.md`. Core: [DESIGN.md](DESIGN.md); figures: [RESULTS.md](RESULTS.md).

## What is supported

| Feature | Status | Note |
|---|---|---|
| Pub/sub, `rmw_take*`, serialized messages, `ros2 bag` | ✅ | Serialization format `"tickle"`; `rmw_get_serialized_message_size` too, which neither DDS rmw serves |
| **Services** and clients, actions | ✅ | Up to `tt_CLIENT_MAX_OUTSTANDING` (4) requests in flight per client (`-DRMW_TICKLE_CLIENT_MAX_OUTSTANDING=`), answered in any order; when all are busy `rmw_send_request` waits 100 ms, then fails. A service queues 64 untaken requests |
| All 11 events, EventsExecutor callbacks | ✅ | |
| Graph and introspection | ✅ | Remote nodes included. `*_info_by_service`: one endpoint, INVALID type hash, remote QoS unknown |
| Several nodes per process; several processes per host | ✅ | In-process delivery; context ids claimed in `/dev/shm`; shared memory between processes is automatic |
| `ROS_DOMAIN_ID`, `ROS_AUTOMATIC_DISCOVERY_RANGE`, `ROS_STATIC_PEERS` | ✅ | Port 8282 + domain id (max 232). `NOT_SET` refuses to create a node |
| Messages to 64 KB / above 64 KB | ✅ | 1472-byte fragments / up to `RMW_TICKLE_MAX_SAMPLE_BYTES` (8 MiB). Services are not fragmented (the OS does it, max 64 KB) |
| [Loaned messages](#loaned-messages) | ✅ | Plain fixed-size types only (67 shipped types) |
| Unbounded sequences | ⚠️ | Fixed capacity per field (`LaserScan` 4096, `Image` 64,000 B), `.capacities` file to override |
| SROS2 security | ❌ | `Enforce` fails `rmw_init`; permissive warns once. Parked |
| Pre-allocation, content filter, flow endpoints, dynamic messages, `wstring`, introspection typesupport | ➖ | By design; multi-dim arrays, 128-bit types, `string<=N[]` refused at generation |

Threads: `rmw_wait()` receives on the executor's thread; a poll thread takes over after 10 ms without one, and a
watchdog raises LIVELINESS_LOST if it stalls for 3 s.

## Build and use

```sh
source /opt/ros/$ROS_DISTRO/setup.bash
pip install ./tools/typesupport          # the code generator, once
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
  --cmake-args -DBUILD_SHARED_LIBS=ON
source install/setup.bash
rmw_tickle/scripts/build_ros2_interfaces.sh -w ~/tickle_ifaces_ws -a   # all 24 jazzy interface packages, once
source ~/tickle_ifaces_ws/install/setup.bash
export RMW_IMPLEMENTATION=rmw_tickle
```

- The stock interface packages have no TickLE typesupport; without the step above not even `rclcpp::Node` starts.
- Build your own interface packages with `rmw_tickle` sourced. A package configured earlier: build it afresh once.
- Build options: `-DTICKLE_MAX_BUFFER_LENGTH=<N>` (below 65507; rebuild the interfaces), and
  `-DRMW_TICKLE_MAX_RELIABLE_READERS=<N>` (1-1024, default 16; 64 B per entry in every publisher; the next reader is
  not matched, log `Ack table full`).
- Fixed sizing: 256 nodes per context, 2048 endpoints, 2048 discovered entities, 4 MiB socket receive buffers
  (capped by `net.core.rmem_max`, logged).

### Runtime environment

| Variable | Effect | Default |
|---|---|---|
| `TICKLE_BROADCAST_ADDR` | broadcast address, and with it the interface | limited broadcast |
| `TICKLE_NODE_ID` | fixed context id | claimed automatically |
| `TICKLE_ID_REGISTRY` | directory of the context id registry, `off` for none | `/dev/shm` |
| `RMW_TICKLE_EXECUTOR_POLL` | `0`: receive on the poll thread only | on |
| `RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY` | Heartbeat on every Nth RELIABLE sample; `0` off | 64 (window / 16) |
| `RMW_TICKLE_IDLE_HEARTBEAT_NS` | Heartbeat when unacknowledged and idle this long ([DESIGN 6](DESIGN.md#6-reliability)); `0` off | 250 ms |
| `RMW_TICKLE_HEARTBEAT_PERIOD_NS` | also a FINAL Heartbeat at this fixed rate | off |
| `RMW_TICKLE_MAX_BLOCKING_MS` | longest a KEEP_ALL publish blocks | 100 |
| `RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS` | see [below](#large-samples-blocking-publish) | 0 (off) |
| `RMW_TICKLE_CACHE_BYTES` | KEEP_LAST publisher cache | 1 MiB |
| `RMW_TICKLE_KEEP_ALL_BYTES` / `_KEEP_ALL_MAX_SAMPLE_BYTES` | KEEP_ALL publisher budget / space reserved per sample | 512 KiB / 1472 B |
| `RMW_TICKLE_READER_KEEP_ALL_BYTES` | untaken bytes one KEEP_ALL subscription holds; then it declines | 512 KiB |
| `RMW_TICKLE_REORDER_SLOTS` | out-of-order RELIABLE samples held; fewer than the window less one, by this or the budget, hold none (core refuses them: they storm). An unbounded or large type (window 8192) holds none | tracking window (1024) |
| `RMW_TICKLE_REORDER_BYTES` | reorder budget per RELIABLE subscription | 1024 x (24 + 1472) B |
| `RMW_TICKLE_REORDER_SLOT_PAYLOAD` | largest sample a reorder slot holds | 2 KiB |
| `RMW_TICKLE_MAX_SAMPLE_BYTES` | largest message (max ~11.3 MiB) | 8 MiB |
| `RMW_TICKLE_LARGE_REGION` | `0`: no shared region for same-host large messages | on |
| `RMW_TICKLE_PUBLISH_HUGEPAGE` | `0`: a publisher's large-message buffers of 2 MiB or more stay on ordinary pages (default: 2 MiB-aligned, `MADV_HUGEPAGE`; falls back silently without THP) | on |
| `RMW_TICKLE_LOANS` | `0`: no loans (as `ROS_DISABLE_LOANED_MESSAGES=1`) | on |
| `RMW_TICKLE_LOAN_RING_SLOTS` | `1`: lend received samples in their ring slot | off |
| `RMW_TICKLE_LOAN_PUBLISH_SLOTS` | `1`: borrow lends the subscriber's ring slot (up to 4 at once; then a kept buffer) | off |

Test switches, not for use: `RMW_TICKLE_LARGE_FRAGMENTATION`, `RMW_TICKLE_REASSEMBLY_CAP_OFF`, `RMW_TICKLE_RUN_UNITS`
(forced hybrid arms, DESIGN.md section 8), `RMW_TICKLE_TRACE_FILE` (counters at shutdown), the Linux HAL's `TT_*`
(DESIGN.md section 15). Publisher budgets can also be set per publisher: `rmw_tickle_publisher_payload_t`
(`publisher_payload.h`) via `rmw_specific_publisher_payload`.

## QoS

| Policy | Accepted | Note |
|---|---|---|
| RELIABILITY | BEST_EFFORT, RELIABLE, BEST_AVAILABLE | Services: default `services_default` (RELIABLE, retried in core); a BEST_EFFORT client sends each request once and, with every slot busy, ends its oldest call instead of waiting |
| DURABILITY | VOLATILE, TRANSIENT_LOCAL, BEST_AVAILABLE | Services: VOLATILE only |
| HISTORY | KEEP_LAST, KEEP_ALL | KEEP_ALL is bounded in bytes, not samples (no RESOURCE_LIMITS in rmw): a full publisher blocks up to `RMW_TICKLE_MAX_BLOCKING_MS` then times out; a full RELIABLE subscription declines (the writer resends), BEST_EFFORT drops |
| DEADLINE, LIFESPAN | any finite value | Deadline: one miss per period P with no publish (offered) or no sample received (requested), checked at the last activity + P. Lifespan expires samples in the publisher cache and the subscriber queue; a subscription applies the shorter of the writer's and its own (both announced, `tt_VERSION` 14) |
| LIVELINESS | AUTOMATIC, MANUAL_BY_TOPIC, BEST_AVAILABLE | Lease at least 6 ms. Services: AUTOMATIC only (nothing asserts a service's liveliness) |

RxO matching covers reliability, durability, deadline and liveliness; both sides get the event, once per incompatible
endpoint, when its announce arrives (no periodic rescan). `rmw_qos_profile_check_compatible()` returns ERROR for those
rules and WARNING for SYSTEM_DEFAULT, UNKNOWN or BEST_AVAILABLE where it decides the outcome. BEST_AVAILABLE is
resolved at creation against endpoints already discovered. `rmw_get_publishers_info_by_topic()` and
`_subscriptions_info_` report a remote endpoint's every policy from its announce, history, depth and lifespan included
(`tt_VERSION` 14; UNKNOWN before), so `ros2 bag` replays a topic with the depth it recorded.

## Loaned messages

- **Which types:** those whose CDR-4 wire bytes equal the C struct (no string, sequence or bool), e.g. Array1k,
  `geometry_msgs/Pose`. `can_loan_messages` is false for the rest. Interface packages built before loans must be rebuilt.
- **Take:** a sample in a receive buffer (socket path) or a large sample is lent where it lies: no copy. A ring sample
  is decoded once into a shell. At most 8 queued samples per subscription stay in buffers; byte-swapped, fragmented,
  local or misaligned ones are decoded.
- **Publish:** the borrow lends a kept buffer (up to 64); one copy remains. Set every field: a reused buffer is not cleared.
  A large in-place type is copied a send at a time, just ahead of the datagrams (DESIGN.md section 8, "Encoding while sending").
- **Ring slots (both off by default, the user, 2026-10-10):** `RMW_TICKLE_LOAN_RING_SLOTS=1` lends the ring slot; a held
  slot is passed over by the ring's writers (DESIGN.md section 10, "Held slots"), so it stops nothing (until 2026-10-20
  it stopped the ring one lap later). `RMW_TICKLE_LOAN_PUBLISH_SLOTS=1` builds in
  the subscriber's slot (no publish copy for BEST_EFFORT): up to `tt_PUBLISHER_CLAIMS` (4) slot loans out at once, each
  held and passed over by the ring, other publishes going on meanwhile, read in publish order. No slot free (full ring,
  several or remote peers, 4 out): the borrow lends a kept buffer, and a slot loan that cannot go where it is (a psn
  past the short form, KEEP_ALL without room) is published by copy (`slot_loans_copied` on the loans line).
- **Measured:** PC, no visible difference (RESULTS.md "Loaned messages (PC)"). Rig A/B 2026-10-09 (`7a321d26`..`e521bbba`
  against `9d924607`): defaults PASS 12/12; with rclcpp taking loans, RELIABLE Array4k throughput +14.3%. Slot loans
  (`324f6ced`, 2026-10-10): PASS, nothing better or worse.

## Large samples: blocking publish

Off by default: a large publish hands the sample to core and returns; core sends it from the poll and a newer
KEEP_LAST sample replaces a waiting one. `RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS=<ms>` waits after the hand-off until the
sample is in the socket, as a DDS `publish()` does; it then returns `RMW_RET_OK` even on time-out. KEEP_ALL publishers
and types that fit one datagram never wait. Counters: `large_block_waits`, `large_block_timeouts` in the stats line.
Rig figures: RESULTS.md, I4s note.

## Performance

Rig figures against Fast DDS, CycloneDDS and rmw_zenoh: [RESULTS.md](RESULTS.md), "rmw layer" (cross host) and
"rmw layer, same host" (R1-R21).

## Open items

- A segment ring that skips held slots, so ring slots can be lent safely.
- Event counts across processes; `*_info_by_service` with two endpoints, a type hash and remote QoS.
- An rmw-level shared-memory kill test (two ROS processes, subscriber killed mid-stream).
- A two-host cross-vendor rmw throughput benchmark; rmw_tickle coverage in CI; SROS2 (parked).

## Tuning and differences from DDS

### Options you may tune

Defaults were checked on the rig (two Raspberry Pi 5s, 1 GbE); derive yours. Core constants are `-D` overrides
(`include/tickle/config.h`); `(env)` is an environment variable. Every core knob: DESIGN.md section 15.

| option | default | derive it as |
|---|---|---|
| `tt_SEGMENT_BYTES` | 768 KiB | slots = peak datagram rate x longest reader stall, rounded **down** to a power of two; bytes = slots x (16 + `tt_SEGMENT_SLOT_BYTES`) |
| `tt_SOCKET_BUFFER_SIZE` | 1 MiB | peak receive rate x longest drain stall; raise `net.core.rmem_max` / `wmem_max` too |
| `tt_RX_BATCH` | 32 | datagrams one wake drains at peak (p95). rmw_tickle uses 1 |
| `tt_RX_CLOCK_REFRESH` | 16 | accepted receive-stamp staleness / time per datagram |
| `tt_RX_LOCK_CHUNK` | 8 | longest wait for a publishing thread / time per datagram |
| `tt_RELIABLE_RETRY_INITIAL` | 1 ms | ~4 x round trip; only until the first is measured |
| `tt_DISCOVERY_REQUEST_RETRY` | 10 ms | above round trip + `tt_CONTEXT_TX_INTERVAL`; only until timed |
| `RMW_TICKLE_KEEP_ALL_BYTES` (env) | 512 KiB | link rate x longest acknowledgement stall to ride out |
| `RMW_TICKLE_READER_KEEP_ALL_BYTES` (env) | 512 KiB | sample size x samples left untaken before writers are pushed back |
| `RMW_TICKLE_TRACKING_WORDS` | 16 (1,024 samples) | 64 x words >= peak sample rate x repair time |
| `RMW_TICKLE_HEARTBEATS_PER_WINDOW` | 16 | piggyback interval = window / this; raise it if a reader jumps its window under loss |
| `RMW_TICKLE_LOAN_PUBLISH_SLOTS` (env) | off | `1` builds loaned messages of same-host one-subscriber topics in its ring slot; `tt_PUBLISHER_CLAIMS` (4) is how many at once |

Retry granularity G (`tt_RELIABLE_RETRY_GRANULARITY`, `tt_CALL_RETRY_GRANULARITY`) is measured by the context; set it
only to fix G (DESIGN.md section 6).

### How TickLE differs from DDS

DDS ideas, own protocol: **no interoperation with DDS**. "DDS" means Fast DDS and CycloneDDS defaults.

| Behaviour | TickLE | DDS | What to expect |
|---|---|---|---|
| Wire | Own UDP framing, CDR-4 | RTPS, CDR/XCDR | 20 B framing per sample |
| Discovery | Link broadcast, 1 s summaries; UDP even on one host | Multicast SPDP/SEDP | Set `TICKLE_BROADCAST_ADDR` to the directed broadcast |
| Peer departure | 3.5 s silence (to 10 s with long leases) | ~10 s Cyclone, ~20 s Fast DDS | Faster failover |
| Contexts | 8-bit id, max 254 per link | 12-byte GUID prefix | Plan large systems around it |
| Several readers | Unicast to 2, broadcast to more | Unicast per reader or multicast | Unsubscribed hosts discard |
| RELIABLE readers per publisher | 16, `-DRMW_TICKLE_MAX_RELIABLE_READERS=N` | Unlimited | Raise for e.g. `/clock` |
| Heartbeats | 250 ms after the last send while unacknowledged; every 64th sample | Periodic (Cyclone 100 ms, Fast DDS 3 s) | A lost last sample is repaired in 125-250 ms |
| Repair | Immediate NACK; retry from measured RTT | Fixed delays | Better under loss; a few duplicates |
| Large samples | MTU units, own sequence numbers; only lost units resent; 8 MiB max | 64 KB IP fragments + DATA_FRAG | Robust under loss |
| UDP checksum | 0 on large-sample IP fragments (Linux) | On | DESIGN.md, "UDP checksum 0"; `TT_UDP_CSUM0=0` restores it |
| KEEP_ALL bound | 512 KiB per publisher and subscription | Samples (Fast DDS 5,000, Cyclone unlimited) | Publish blocks up to 100 ms at the bound |
| KEEP_LAST history | At most 1 MiB (`RMW_TICKLE_CACHE_BYTES`) | `depth` samples | Fewer than depth for large samples (warned) |
| TRANSIENT_LOCAL | Publisher pushes history | Reader pulls | No TRANSIENT or PERSISTENT |
| LIFESPAN | The shorter of the writer's and the subscription's own | Cyclone the writer's (not verified); Fast DDS 2.14.7 the reader's own (`DataReaderImpl.cpp`) | Covers both; aged by the source timestamp, so cross-host it shifts by the clock offset, as in DDS |
| Source timestamp | 32-bit us, clocks within ±35 min | 64-bit | Sync clocks if you use them |
| Same host | Automatic shared-memory ring, no daemon | SHM / iceoryx (opt-in) | Nothing to configure |
| Services | 4 in flight per client; max 64 KB; dropped after 5 s | Unbounded, no timeout | Keep requests small on lossy links |
| Service QoS | RELIABLE: resent by the client from the measured RTT. BEST_EFFORT: sent once; with every slot busy the oldest call ends | Carried by the request and reply topics' QoS | A BEST_EFFORT call can be lost, as on DDS, and never blocks the next |
| QoS compatibility | Judged when an announce is applied, once per endpoint pair; fails open past `tt_MAX_DISCOVERED_ENTITIES` | Judged at endpoint match | One INCOMPATIBLE event per pair |
| Liveliness lease | At least 6 ms; services AUTOMATIC only | Any | A shorter lease is refused at creation |
| QoS | No ownership, partitions, filters, keys, `wstring`, security | All | `RMW_RET_UNSUPPORTED` |
