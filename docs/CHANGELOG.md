# Changelog

All notable changes to TickLE are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) for the library
(`TICKLE_VERSION_*` in `include/tickle/tickle.h`). The on-the-wire protocol has its own
number, `tt_VERSION`, which moves independently.

Entries cite planning documents (`PLAN.md`, `RMW_GAPS_PLAN.md`, ...) that were condensed into `docs/` on 2026-10-05;
read them with `git show 3c0c505b:<path>` ([DESIGN.md](DESIGN.md), section 16).

## [Unreleased]

### Changed (breaking)

- **QoS at creation, rmw-shaped** (QoS plan, 2026-10-18; [DESIGN.md](DESIGN.md) section 9). Endpoints are created with
  `tt_Node_create_publisher/_subscriber/_client/_server(node, ep, type, name, qos, options, storage, size)`:
  `struct tt_QosProfile` (one struct per policy, TickLE's extensions inside), `tt_*Options` for what is not QoS, and the
  caller's storage of `tt_*_storage_size()` bytes. `struct tt_Topic` is `struct tt_TypeSupport`. A zeroed profile is
  rmw's default (RELIABLE, KEEP_LAST 10), not BEST_EFFORT, and fails without storage. `tt_Context_create_*()`,
  `tt_Node_create_*_legacy()` and QoS written into endpoint fields are deprecated (`9caef96e`, `6624e9bc`).
- **Wire `tt_VERSION` 14**: each announce record carries the endpoint's lifespan and history depth (+12 B). Nodes on 13
  and 14 refuse each other (`d9d1c55c`).
- **A RELIABLE Subscriber's reorder buffer must cover its window** (the user, 2026-10-20). Fewer `reorder_slots` than
  the window less one, other than `tt_QOS_REORDER_NONE`, are refused: they storm with re-requests under loss. A zero
  `reorder_slots` is the window, not 16; `tt_QOS_REORDER_SLOTS_DEFAULT` is removed. rmw_tickle holds none where its
  budget pays for fewer, which an unbounded or large type's 8192-sample window always is.
- **A missing peer segment is asked again after a time, not 256 sends, and an attached one revalidated after a time,
  not 4096** (ROADMAP 5a; [DESIGN.md](DESIGN.md) section 10, "Counts made times"). `tt_SEGMENT_ATTACH_RETRY_SENDS`,
  `_REVALIDATE_SENDS` and `segment.attach_retry_sends`, `.revalidate_sends` are `tt_SEGMENT_ATTACH_RETRY_NS`,
  `_REVALIDATE_NS` and `segment.attach_retry_ns`, `.revalidate_ns` (one update interval each); the old names stop the
  build. The summary tick ages the answers; a send reads one byte and no clock.
- **A busy poll checks its socket after a time, not after 8 scheduler entries** (ROADMAP 5a; [DESIGN.md](DESIGN.md)
  section 10). `poll.scheduler_io_interleave` is `poll.rx_check_ns` (0 = `poll.receive_timeout`), counted from the
  poll's last look at its socket across calls; `tt_SCHEDULER_IO_INTERLEAVE` stops the build.
- Both landed on the user's overall judgement (2026-10-23) despite a formal same-host WORSE: the times do not depend on
  the testbed. Rig, A c4c7e194 (the design commit), pieces bbe395b6, 66d74ed4, 2fa07cc9, each before its rebase.
  Same-host gate v2 (`ab_samehost_segment_time_limits_20261023-063521`): piece 1 WORSE 3/34, user instructions
  +0.03..+0.06% on reliable_latency p1/p3; piece 2 WORSE 2/34, client user instructions +0.03% on reliable_throughput
  p3 and server kernel instructions +1.95% on reliable_latency p3 (a re-attach every ~2 s instead of every 4096 sends,
  ~4 s at 1,000 sends a second), against 22 rows better - throughput +7.6% to +17.0%, CPU per sample -5.8% to -12.8%.
  Core gate v2 with c6: piece 1 PASS 0/96 (`coreAB_bbe395b6_20261023-091236`), piece 3 PASS with 3/96 listed, client
  user instructions +0.11..+0.31% on c4/c6/c15 (`coreAB_2fa07cc9_20261023-105010`).

### Added

- **FreeRTOS service caches sized to their use** (the user, 2026-10-22; [DESIGN.md](DESIGN.md) section 7, "Sizing a
  server's kept answers"). The FreeRTOS set_bool and ping_pong examples keep their answers and requests in storage
  sized for their messages (`max_response_bytes` / `max_request_bytes`) and build with the inline caches cut to 8 bytes
  and 8 answer slots (one Client's whole call window): server images 398,244 -> 207,444 B of .data+.bss, client images
  218,820 -> 207,140 B. `tt_MAX_SERVER_CACHE_COUNT` is 16 on FreeRTOS (was 64): a default server is 47,976 B (was
  191,544) and takes 12.8 calls/s summed over its clients, or 2 clients at any rate; past that a request is turned away
  and retried, never run twice (`tests/test_service_small_cache.c`).
- **Wait ring** (Linux HAL; [DESIGN.md](DESIGN.md) section 2, "Receive path"): `tt_receive()`'s wait and read in one
  `io_uring_enter()` on a ring set up `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`, owned by the first thread
  that waits and moved to a thread that keeps the poll role; other threads wait with epoll as before. A context option
  (`tt_InitOptions.socket.rx_uring_wait`) and `TT_RX_URING_WAIT`; its default is the build's `tt_RX_URING_WAIT` (1). A
  kernel that refuses it (seccomp, Linux < 6.1) leaves the epoll path, said once.
- **Wi-Fi repair timing** (network-profiles phase 3, local values; [DESIGN.md](DESIGN.md) section 2):
  `tt_LinkOptions.repair_timing` (0 = the profile's: WIFI on, the rest off). A reliable reader's first recovery is timed
  even past the back-off, and after it only one its first request answered. Without it the WB Wi-Fi arms never timed a
  recovery and retried every 1 ms for good. Every other profile resolves it off: recoveries timed as before.
- **Wi-Fi discovery** (network-profiles phase 3, local values; [DESIGN.md](DESIGN.md) section 2): a WIFI link sends each
  discovery summary also by unicast to its known peers (`tt_LinkOptions.summary_unicast`, at most 16 a round, one round
  per update interval), repeats a change's broadcast announce 3 times at 100/200/400 ms gaps
  (`tt_LinkOptions.announce_repeats`), and sets the context's miss threshold to 6 where it is left at the default
  (`tt_LINK_WIFI_MISS_THRESHOLD`). Counters `tt_Context.summary_copies_sent`, `announce_repeats_sent`. Every other
  profile resolves both fields off: byte-identical on the wire.
- **Send pacing** (network-profiles phase 0, item F; [DESIGN.md](DESIGN.md) section 2): `tt_LinkOptions.pacing_rate`
  (bits per second on the wire; 0 = the profile's: T1S 75% of `link_rate`, the rest `tt_PACING_OFF`) and
  `pacing_burst` (bytes; 0 = four frames of the link MTU). A token bucket per link counts every UDP datagram; a large
  sample's units wait for it in the scheduler, a retransmission past two bursts is left to the reader's next request;
  ordinary sends are never held. Counters `tt_Context.pacing_waits`, `pacing_repairs_deferred`. Off - every profile but
  T1S - is wire- and time-identical.
- **Link profiles** (network-profiles phase 0, items B and G; [DESIGN.md](DESIGN.md) section 2): `tt_LinkOptions.profile`
  (`tt_LINK_PROFILE_ETHERNET` 0, `_T1S`, `_TSN`, `_WIFI`) and the per-link values a profile fills where left 0 - `mtu`,
  `link_rate`, `retry_floor_ns`, `batch`, `batch_tick_ns`, `idle_heartbeat_ns`, `piggyback_divisor`,
  `large_fragmentation`, and `unicast_threshold`'s 0. The unicast threshold applies per link; the rest is combined
  across links into the context. A link MTU below 1500 now cuts batches, announce parts and fragments (it only warned
  before). `tt_PublisherOptions.batching` (`tt_SWITCH_*`) and `heartbeat_piggyback_every` 0 take the links' default;
  `tt_HEARTBEAT_PIGGYBACK_NONE` is none. ETHERNET, and a context with no links configured, are byte-identical on the
  wire.
- **Context options** (`struct tt_InitOptions`, ROADMAP "Context options" bundle 1; [DESIGN.md](DESIGN.md) section 2):
  `tt_InitOptions_default()` and `tt_Context_create_with(ctx, opts)` (NULL = defaults). The `net` group holds what
  `_tt_CONFIG` held, per context; HALs read the context's copy; bad values are refused at creation with the field named
  (the `tt_MAX_PEER_COUNT > threshold` static_assert moved there). `tt_Context_create()` reads `_tt_CONFIG` once, as
  before; writing it is deprecated. rmw_tickle builds its options from the same environment variables and keeps them
  per rmw context. Wire bytes of a default context unchanged. HAL: `tt_get_node_id()` takes the context.
- **QoS defaults per context** (`tt_InitOptions.qos`, bundle 5): depth, KEEP_ALL depth, the two byte caps and
  `max_blocking_ns` that a zero profile field takes. `tt_*_storage_size()` keep their signatures and config.h's
  defaults; `tt_Publisher_storage_size_for()` / `tt_Subscriber_storage_size_for()` size for a context, and storage short
  of what a raised default needs is refused with the field named. rmw_tickle reads `RMW_TICKLE_CACHE_BYTES`,
  `RMW_TICKLE_KEEP_ALL_BYTES` and `RMW_TICKLE_MAX_BLOCKING_MS` once, at `rmw_init()`, into these.
- **Service options** (`tt_InitOptions.service`, bundle 4): `tt_CALL_RETRY_*`, `tt_CALL_DEADLINE_PER_SEND`,
  `tt_SERVER_CACHE_GAP_MULTIPLE` and `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` are per-context defaults; their
  static_asserts are creation checks; a non-default client schedule warns (a peer's server assumes the build's).
  `RMW_TICKLE_CLIENT_RETRY_COUNT` is replaced by `rmw_tickle_client_retry_count(ctx)`.
- **Discovery options** (`tt_InitOptions.discovery`, bundle 2): the discovery and liveliness timings (`tt_CONTEXT_CYCLE`,
  `_UPDATE_INTERVAL`, `_TX_INTERVAL`, `_MAX_LEASE_NS`, `tt_LIVELINESS_LEASE_DIVISOR`, `_DEFER_NS`, `_MAX_DEFERRALS`,
  `_MISS_THRESHOLD`, `tt_DISCOVERY_REQUEST_RETRY`, `_ATTEMPTS`, `tt_SCHED_FULL_WARN_INTERVAL`) are per context, the
  constants their defaults; `tt_SEGMENT_DEAD_READER_NS < silence` checked at creation. A non-default update interval or
  miss threshold logged a WARNING (peers judged silence by their own) until the summary carried both (below), and an
  INFO line since. rmw_tickle reads them from its core context.
- **Reliable options** (`tt_InitOptions.reliable`, bundle 3): the retry interval, its starting value, G, ceiling, retry
  count, back-off, stuck-gap warning and the idle HEARTBEAT's period and floor per context (`struct tt_ReliableOptions`;
  each zero is its config.h constant). `tt_reliable_retry_interval_configured()` takes the context. Default wire bytes
  and timing unchanged.
- **Poll and socket settings per context** (`tt_InitOptions.poll` and `.socket`, bundle 6): the receive loop's clock
  refresh, lock chunk, busy slice and I/O interleave; the socket buffer size, the io_uring hint (AUTO or URING), the
  drain's sizes and hold-off, and the six Linux HAL switches that were environment variables only (`TT_UDP_OFFLOAD`,
  `TT_UDP_CSUM0`, `TT_RX_DIRECT`, `TT_RX_LAZY_TIMER`, `TT_RX_LAZY_ADAPT`, `TT_RX_NONBLOCK`). A switch's variable, when
  set, still decides over the option. Defaults unchanged; the receive loop reads its two per-datagram bounds once per
  drain.
- **Hybrid options** (`tt_InitOptions.hybrid_transport`, bundle 7): the `tt_HYBRID_*` estimator and control-law
  constants are per-context defaults (`tt_HYBRID_N_MAX` also the build's bound, `tt_HYBRID_COLLAPSE_Z` compiled, the
  stale exposure derived from `stale_false_forget`), checked at creation; a build without the hybrid transport refuses
  them set.
- **Segment options** (`tt_InitOptions.segment`, bundle 8): `tt_SEGMENT_BYTES`, `_SLOT_BYTES`, `_FENCE_HOLD_NS`,
  `_DEAD_READER_NS`, `_DRAIN_PER_POLL`, `_PROBE_EVERY_MAX`, `_STALL_PASSES`, `_ATTACH_RETRY_SENDS` and `_REVALIDATE_SENDS`
  are per context, the constants their defaults (`tt_SEGMENT_SLOTS` stays the build's most slots). `net.segment_slot_bytes`
  (unreleased) is `segment.slot_bytes`. The dead-reader time follows the context's discovery interval, and the check
  against the silence moved from the discovery group to it. Wire bytes and the default ring unchanged.
- **Capacity** (`tt_InitOptions.capacity`, bundle 9): runtime limits on the fixed tables - RELIABLE readers per
  Publisher, peers and writer proxies per endpoint, remembered ACKNACK requests, outstanding claims, retained large
  samples - each at most its compiled size, which stays the array size and the default. A table is full at its limit,
  with the refusal it always had; `peers` must exceed every link's unicast threshold.
- **Traffic classes and send time** (network profiles items D and E; `tt_InitOptions.socket.tx_class[]` and `.txtime`,
  Linux HAL): an SO_PRIORITY (to a qdisc band, and to a VLAN PCP through the interface's egress-qos-map) and a DSCP per
  class - DATA (a user's sample or call), REPAIR (a resend, a durability backlog, a call's retry), CONTROL (discovery,
  HEARTBEAT, ACKNACK) - read from what each datagram carries; and SO_TXTIME (CLOCK_TAI) with
  `tt_Publisher_publish_at()`, which an ETF qdisc honours. Every option 0 (the default) sets nothing: the same system
  calls and bytes. No wire change; other HALs ignore the options.
- `tt_*_get_actual_qos()`, `tt_QosProfile_resolve()`, `tt_QOS_PROFILE_DEFAULT`, `_SENSOR_DATA`, `_SERVICES_DEFAULT`.
- History depth counts samples; `history.bytes` caps a Publisher's cache, with a warning at creation (D10).
- Compatibility judged when an announce is applied, counted once per pair (`tt_QosIncompatibleStatus`,
  `tt_Context_set_qos_incompatible_callback()`); rmw_tickle's events follow it and its periodic rescan is gone;
  `rmw_qos_profile_check_compatible()` returns WARNING too (D2, `0876adf0`, `3bc51bde`).
- A reader is held to the shorter of its writer's lifespan and its own (D1, `c138def7`); the rmw graph reports a remote
  endpoint's history, depth and lifespan (D8, `cfeef643`).
- A BEST_EFFORT service Client sends each request once; `tt_Client_end_oldest_call()` frees a slot (D6, `d33bcb52`).
- **The discovery summary carries its sender's update interval and miss threshold** (the user's decision, 2026-10-22,
  network-profiles item C; [DESIGN.md](DESIGN.md) sections 3 and 5). `tt_HeartbeatHeader`'s 3 reserved bytes became
  `summary_interval` (2 B, unit + count, 0 = default) and `summary_miss_threshold` (0 = default); a receiver judges
  each peer's silence by what that peer's summary says, and by its own until the first one. 0 extra bytes and no
  `tt_VERSION` bump - the documented exception for a reserved field whose zero keeps the old meaning: older peers send
  zeros and ignore the bytes, and a default context's datagrams are unchanged. A non-default interval or threshold at
  creation is now an INFO line, not a WARNING.

### Changed

- **A scheduled publish reads no clock** ([DESIGN.md](DESIGN.md) section 2): the first publish of a scheduler entry
  takes the reading the poll ran the entry at (`tt_Context.entry_clock_ns`), so its stamp is early by the entry's own
  work before it. A second publish in the entry, and every entry after the first of a non-blocking poll, reads the
  clock as before. PC: -4% to -7% user instructions per sample on the throughput clients
  (`poll_clock_insn_pc.sh`). Rig core gate v2 (A 132548dd, B 1f5acb48 before its rebase,
  `coreAB_5de951a7_20261022-234808`): PASS; client user instructions -1.4% to -4.1% on all 8 throughput cells, server
  +0.06% to +0.19% on 8 rows (listed), no CPU or latency row WORSE.
- **A context no longer writes its large tables at creation** (bundle 10 item 2, the user, 2026-10-22;
  [DESIGN.md](DESIGN.md) section 11): `tx_buffer`, `rx_buffer`, the scheduler heap, `endpoints[]`, the endpoint index,
  the announce-fragment bitmaps and the reassembly slots are written where they are used and read only there, so their
  untouched pages stay out of memory: an rmw_tickle context's resident 369-441 KiB fell to 70-150 KiB. The endpoint
  index puts in use only the smallest power of two of slots at least twice its endpoints, and a rebuild NULLs only
  those (`endpoint_index_mask`, which is also the staleness flag, so a lookup loads one field and tests no bitmap). The wire is
  unchanged. `platform/linux/garbage_ctx_check.c` (test-linux, and under MemorySanitizer) runs contexts from unzeroed
  storage. Rig core gate v2 (A 71ed68f4, B 064de602 before its rebase, `coreAB_064de602_20261023-012715`): PASS; no CPU
  or latency row WORSE, 1/88 rows listed (c15 p4 server user instructions +0.55%), against 12/88 at +0.42% to +0.84%
  for the bitmap form.

### Removed

- **`.github/workflows/rmw-perf.yml`** (the user, 2026-10-23). Dispatch-only since 2026-09-24 and last run that day; its
  `buildfarm_perf_tests` figures were used nowhere (rmw_tickle's figures come from the rig harnesses in
  `examples/perf_hil/experiments/`, published in [RESULTS.md](RESULTS.md)). The one thing only it did moved first: a live
  two-process stream through `rclcpp` is now `rmw_tickle/scripts/check_ros2_interfaces.sh -S` (`stream_check.cpp`), run
  by `check-all.yml` on every push on GitHub-hosted runners and by `make test-rmw-behaviour` (the gate table's `rmw
  suite` row) in a netns. Its shape is the old matrix's - BEST_EFFORT, KEEP_LAST 10, 1000 Hz from default rclcpp nodes,
  a 1 KiB array and a nested `std_msgs/Header` - now across the block and poll receive paths (4 cells), and so are its
  gates: a sample whose publish time is not strictly newer than the last ("Fail on a timestamp-ordering violation"), a
  "Data consistency violated" or `std::terminate` abort, a cell that received nothing, and fewer cells printing a result
  than the matrix holds ("Verify every expected cell actually ran"). CI runs a control per failure (an injected
  timestamp regression, a cell that prints nothing, an injected abort), each of which must fail for its own reason.
  `check-rig-lock` (`check_rig_lock_coverage.sh`), which read only `rmw-perf.yml`, goes with it, from `make test-all`,
  `test-all.yml` and the gate table. The lab PC's `buildfarm_perf_tests` workspace stays for `compare_rmw_perf.sh`
  ([RMW_PERF_SETUP.md](RMW_PERF_SETUP.md)).

### Fixed

- A SIGKILLed context's segment (768 KiB by default) and doorbell stayed in `/dev/shm` for good: release relied on a
  successor at the same name replacing them, and the name carries the ephemeral data port. A context building its own
  segment now removes every one at its address that no live owner holds (`sweep_orphaned_segments()`, Linux HAL).
- A writer kept a dead same-host reader's ring mapped until it exited when it sent too little to fill the ring (a
  10 Hz publisher whose subscriber was killed); it is now unmapped when the peer is presumed dead. Both found by
  `rmw_tickle/rmw_tickle/test/test_shm_kill.c` ([DESIGN.md](DESIGN.md) section 10, "Lifecycle").
- The zero-copy publish (`publish_zerocopy()`, an in-place encoder) compared its peers with the compiled
  `tt_UNICAST_PEER_THRESHOLD`, not the link's resolved `unicast_threshold`, and unicast on a T1S link; it now takes each
  link's share as a flush does.
- rmw_tickle: a process could hang at `rclcpp::shutdown()` after its last node was destroyed (the action check's
  client, 3 of 100 runs on the PC). The context was destroyed while an `rmw_wait()` holding the executor poll role -
  rclcpp's graph listener - still polled it, and a poll that re-entered `epoll_wait()` while the descriptors were being
  closed was never woken. The role is now taken back before the destroy (`retire_poll_role()`), and a wake of a closed
  context is refused without touching a descriptor (it wrote to the closed eventfd's number: "Cannot send wake signal:
  Bad file descriptor", or into whatever the process had opened under it since)
  (`rmw_tickle/rmw_tickle/test/test_teardown_wait.c`).
- A core service call on the auto retry schedule could run its callback twice: the server kept an answer for the
  client's seed schedule (75 ms), while the client's srtt-relative retries come up to its 1 s deadline. Under 5% loss
  each way at 1 kHz (rig cell c21, on the PC) 1-2 calls in 3,000-7,000 ran twice. The server now keeps an answer for
  the auto client's deadline plus one `tt_CALL_DEADLINE_PER_SEND` (1.25 s), still at most `tt_CLIENT_CALL_WINDOW` per
  calling client (`tests/test_service_exactly_once.c`).
- A core service client's retry estimate grew without bound under loss: every answer was an RTT sample, an answer that
  came only after a resend included, whose R is the lost send's wait plus the round trip. At 5% loss each way the
  first wait went from 0.4 ms to 290-600 ms within ~120 calls, and c21 made 130-430 calls/s of 1,000. Only calls never
  resent are sampled now (Karn); a resent call's doublings carry to the next call's first wait until a clean answer
  (RFC 6298 5.5-5.7, `tt_CALL_BACKOFF_MAX`).
- A core service call on the auto path gave up 15 first waits after it was made, ~0.8 ms at a 30 us srtt, so a peer
  preempted for a millisecond timed calls out on a lossless link; once Karn stopped resent calls' answers from widening
  the estimate, the PC preflight of c20 (-r 0) failed 14 of 57 jobs that way (main: 1 of 53). After its last send a call
  now waits the ceiling, `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` x max(srtt, G), for an answer: no extra send, and still
  within the deadline (`tests/test_call_retry_adaptive.c`).
- A server whose `tt_MAX_SERVER_CACHE_COUNT` slots all held live answers evicted the one sent longest ago to keep a new
  one, and a retry for the evicted call ran its callback again. The new request is now turned away before its
  callback runs (the client retries; a slot frees within one answer lifetime), unless its Client already holds
  `tt_CLIENT_CALL_WINDOW` answers, whose oldest is provably finished. Under more callers than slots within a lifetime
  this trades waiting for a second execution.
- An rmw_tickle service request whose answer was lost was served twice. The server's `tt_Service` left the client's
  retry schedule at 0, so core kept the answer 75 ms (the auto schedule) while the client retries after 100 ms; the
  retry found nothing kept and ran the callback again. The server now carries the client's 100 ms x 50, and keeps an
  answer for the client's 5 s horizon - at most `tt_CLIENT_CALL_WINDOW` (8) per calling client
  (`tests/test_service_retry_horizon.c`: 2 runs before, 1 after, including a retry delivered after 49 lost).
- A Server's kept answers no longer fill the context's scheduler. Each held a scheduler entry (its expiry timer), so
  with answers kept for the 5 s horizon, 17 Client-Server pairs calling at 100 Hz into one context reached the 128
  entries: 127 of 204 calls were served, 68 turned away by the full scheduler (`tests/test_kept_answer_lazy_expiry.c` on
  the parent). The expiry is now a time in the answer's slot, checked when the slot is looked up or reused: 0 entries,
  every call served, the same answers kept and served. Server instructions per call on the PC's mock HAL -15% (one
  pair) and -30% (12 pairs) (`examples/perf_hil/experiments/kept_answer_insn.sh`).
- rmw_tickle DEADLINE re-arms at the last activity + P and counts every missed period; a gap between P and 2P was
  missed before (D4, `d2818e79`).
- A hybrid reader no longer counts samples the segment drain passed over as superseded (KEEP_LAST skip-to-newest) as
  samples lost whole. Same-host Array4k BEST_EFFORT read up to 2.2M `missed_samples` in 20 s and announced p = 0.63 for
  shared memory, and paid two `getsockopt(SO_MEMINFO)` per gap on the drain's thread (~13 a delivered sample on the PC).

## [2.0.0] - 2026-10-18

The core and the rmw_tickle packages now share one version. Incompatible with 1.0.0 on the wire (`tt_VERSION` 13).

### Highlights

- **Beats FastDDS and CycloneDDS** on two Raspberry Pi 5s, unpinned, throughput scored as goodput and latency from
  creation time ([RESULTS.md](RESULTS.md)): cross-host core 139 WIN / 11 DRAW / 0 LOSE, same host 18 / 18, rmw same
  host 52 / 52, rmw cross-host 21 / 6 / 1, rmw large messages 56 / 23 / 1. The two LOSE rows are documented with
  their causes: I4 best-effort subscriber CPU (+4.5%, the 4 MB buffer exceeds the Pi's L3) and rmw row 63c
  (CycloneDDS's `spin_some` waits ~42 us each call).
- **Reliable delivery** (RTPS-like HEARTBEAT/ACKNACK, KEEP_LAST and KEEP_ALL, durability, liveliness, deadline,
  lifespan) - 1.0.0 was best-effort only.
- **Large samples** up to 8 MiB: DATA_FRAG, hybrid IP-fragmented runs sized from the measured loss, in-place receive,
  UDP GSO/GRO.
- **Same-host shared memory** transport with lending and claimed-slot publish.
- **ROS 2**: `rmw_tickle` and the `rosidl_typesupport_tickle_c/cpp` packages, all standard interfaces.
- **Services** with several outstanding calls per Client (`tt_CLIENT_MAX_OUTSTANDING`, default 4).
- **Linux**: io_uring receive readiness by default (with fallback), UDP checksum 0 by default, deadline-exact waits
  (timerfd, 1 ns timer slack on the polling thread), publisher huge pages for large samples.
- **FreeRTOS + lwIP** HAL with receive readiness from the netconn callback.
- Every change below, in detail.

### Detailed changes since 1.0.0

### Added

- **A full scheduler is safe and visible** (2026-10-16). With several calls per Client, each request a Server holds
  deferred and each answer it keeps takes a scheduler entry. A request that found the scheduler full ran its callback
  and was then dropped: a deferred one after the application had it (`tt_Server_send_response()` then found nothing),
  a synchronous answer computed and never sent, and the retry ran the callback again - 17 of 20 deferred requests
  and 20 of 20 synchronous ones in `tests/test_scheduler_headroom.c` on 1014a7ed, each a separate ERROR line. Now a
  request is turned away before its callback when there is no entry for it, and the client's retry asks again; every
  refused entry is counted (`sched_full`, with `sched_peak` and `sched_capacity` on the traffic line) and warned
  about at most once per `tt_SCHED_FULL_WARN_INTERVAL` (5 s). rmw_tickle builds `tt_MAX_SCHEDULER_LENGTH` 512
  (12 KB per context); core keeps 128 (2 KB on the rv32 FreeRTOS build). Insert is O(log entries in use) and
  unschedule scans the entries in use, so the larger capacity costs no time (measured: 45 ns at 10 in use, 142 ns at
  120, the same at 128 and 512).

- **Several calls outstanding per Client** (the user's decision of 2026-10-16; docs/DESIGN.md section 7). A Client has
  `tt_CLIENT_MAX_OUTSTANDING` call slots (config.h, default 4, compile time only), each with its own seq_no, retry
  timer and cached request; answers are matched in any order, and the callback names the call it reports
  (`tt_Client.callback_seq_no`; `tt_Client_call_id()` returns the seq_no, `tt_Client_outstanding()` counts the
  calls). Call N + 1 is refused with `tt_RET_ILLEGAL_STATUS`, and so is a call while one outstanding is 2N calls
  old (`tt_CLIENT_CALL_WINDOW`): a Server keeps 2N answers per calling Client and lets the one to its oldest call go,
  so a lost answer is still there when its retry comes. A full cache takes a Client's own answer when it holds a full
  depth. 1 builds the old behaviour, and the services unit tests run in that build too. The wire is unchanged. Memory: N x `tt_CLIENT_CACHE_LENGTH` inline
  per Client (11.8 KB at core's defaults, was 2.9 KB); `tt_Client_set_storage()` now splits the area evenly across
  the slots. rmw_tickle: `rmw_send_request()` keeps up to N requests in flight and, with all in flight (or the
  window closed), waits up to 100 ms for one to end and then returns `RMW_RET_ERROR`; responses are queued (QoS depth, KEEP_LAST) and taken under
  their own sequence ids; a service queues up to 64 untaken requests from any clients, each answerable until it times
  out in core. rclcpp's `async_send_request()` in a row and an action client's `get_result` for several goals failed
  before. `-DRMW_TICKLE_CLIENT_MAX_OUTSTANDING=` changes N for the rmw build.
- **`tt_CALL_IGNORED`**: a server callback may turn a request away unanswered - nothing sent or cached - and the
  client's retry asks again. rmw_tickle's service uses it when its queue is full, where it used to answer with an
  error that ended the client's call.
- **rmw_tickle: an optional blocking publish for large samples, `RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS`** (default 0, off:
  unchanged). With it set, a KEEP_LAST publish of a large sample returns once that sample has been sent, as a DDS
  publish blocks in its send, or after that many ms; it returns OK either way, and the next publish then meets core's
  KEEP_LAST as before. KEEP_ALL publishers (which already wait) and types that cannot be large never wait.
  Core gains `tt_Publisher_large_in_flight()`, which also arms one writable callback for when the send has gone. See
  docs/RMW.md, "Large samples: blocking publish"; the rig arm is `tickle@block` in `rmw_keepall_rig.sh`.
- **Idle HEARTBEAT: a lost last sample of a RELIABLE burst is repaired without waiting for the next sample**
  (docs/DESIGN.md section 6; the user's decision 2026-10-16). While a matched RELIABLE reader has not acknowledged
  everything and neither a sample nor a HEARTBEAT announcing everything has gone for a period (`tt_IDLE_HEARTBEAT_PERIOD`,
  250 ms; `tt_Publisher.idle_heartbeat_ns`; rmw_tickle `RMW_TICKLE_IDLE_HEARTBEAT_NS`, 0 off), the writer sends a
  non-FINAL HEARTBEAT, and again a period later while unanswered. Nothing in a stream with samples at most half a
  period apart, once everything is acknowledged, with no matched reader, or for a BEST_EFFORT reader. One scheduler
  entry per writer, moved in place by at most one publish per half period. On the PC's real HAL
  (`idle_heartbeat_tail.sh`, 10% loss, 80 bursts of 10): every lost tail repaired, 7 at 240 ms and 2 at 492 ms (a second
  round after a lost HEARTBEAT or repair); without it they waited for the next burst (1,001-1,007 ms). Cost on the PC
  (`srv_gap_pc.sh` core cells, x86, not a rig figure): the 10 Hz latency cell +3..+6 user instructions a round trip
  (within 3 SE of the build with it off); the throughput cells' client +39..+43, which a never-due dummy scheduler
  entry reproduces (+45..+54): the bench schedules a timer per sample, and the entry deepens its heap. At 100 ms, the
  first choice, the latency cell paid a HEARTBEAT every round trip (+81 user), which is why the period is 250 ms.
  `RMW_TICKLE_HEARTBEAT_PERIOD_NS` is unchanged (a fixed-rate FINAL HEARTBEAT); each one pushes the idle one back.
- **RELIABLE readers per publisher is a documented build setting**: core's `tt_MAX_ACK_ENTRIES` (16), and rmw_tickle's
  `-DRMW_TICKLE_MAX_RELIABLE_READERS=N` (1-1024). 64 bytes per reader in every publisher with the hybrid transport.
  The behaviour at the limit is kept (the next reader is not matched); its warning now names the setting.
  `tests/test_reliable_readers.c`: 20 RELIABLE Subscribers matched, delivered and acknowledged at 24; the 17th refused at
  16.

- **`tt_Publisher_matched_count()`**: how many remote nodes a Publisher has matched a reader on; 0 means a publish
  sends nothing. The uint64 publisher examples (Linux and FreeRTOS) use it to wait up to 5 s for their subscriber
  before the first publish, so they no longer lose the samples published before the match, and `make test-linux`
  now requires all 20 of its uint64 samples.

- **The retry timers' G is measured** (docs/ROADMAP.md "Now" 5a, docs/DESIGN.md section 6). G in
  `srtt + max(G, 4 x rttvar)` was a fixed 100 us (`tt_RELIABLE_RETRY_GRANULARITY`; twice it for calls). It is now the
  context's own timer lateness: mean + 4 x deviation of how late its waits for a retry timer return, floored at the new HAL call
  `tt_timer_resolution_ns()`, starting from 100 us (`tt_TIMER_LATENESS_INITIAL`). Both constants default to 0
  (measured); a non-zero value is a fixed G.

- **rmw_tickle: the five event types it lacked, and lyrical's service endpoint queries** (rmw_tickle/RMW_GAPS_PLAN.md
  g3).
  - `PUBLICATION_MATCHED` / `SUBSCRIPTION_MATCHED` are raised on every match and unmatch, whether it comes from a
    remote endpoint appearing, lapsing, reviving or leaving, or from a local endpoint being created or destroyed.
  - `PUBLISHER_INCOMPATIBLE_TYPE` / `SUBSCRIPTION_INCOMPATIBLE_TYPE` are raised by endpoints on the topic with
    another type.
  - `MESSAGE_LOST` counts messages from the publication sequence number (psn) per writer, whatever the reliability.
    A late join, a durable replay and a late arrival are not losses.
  - `rmw_get_clients_info_by_service` / `rmw_get_servers_info_by_service` (lyrical) list local and remote clients
    and servers, one row each.
- **rmw_tickle: `ROS_AUTOMATIC_DISCOVERY_RANGE` and `ROS_STATIC_PEERS`** (rmw_tickle/RMW_GAPS_PLAN.md g6,
  `tt_DISCOVERY_OPTIONS` in config.h). Both were ignored until now; they are read as rmw_cyclonedds reads them.
  - LOCALHOST keeps everything on loopback and drops other senders.
  - OFF sends nothing and processes nothing.
  - Static peers become peer links (`_tt_CONFIG.links`, `peer`), sent every announce by unicast.
  - NOT_SET refuses node creation.
  - `rmw_init_options_init` now defaults to LOCALHOST, as the vendors do, and copies and frees discovery options
    properly.
  - The flag is set only in rmw_tickle's build; core's default build is byte-identical.
- **rmw_tickle: a KEEP_LAST publisher keeps `depth` samples from the first publish** (rmw_tickle/RMW_GAPS_PLAN.md g10).
  - Its cache arena starts small and used to grow only after `depth` messages had gone out, so a TRANSIENT_LOCAL
    publisher of a large type gave a late joiner fewer than `depth`: 3 of 4 60 KB samples, where DDS gives 4. The
    RELIABLE retransmit window was short the same way.
  - Core now asks, before caching, whether the sample would evict one inside the depth. If it would, core calls the
    Publisher's new `cache_grow` hook (rmw_tickle's doubles toward the budget).
  - Shortfalls the budget forces are counted (`tt_ReliableCache.depth_shortfalls`), warned about once per
    publisher, and shown on rmw's shutdown line.
- **rmw_tickle: nodes in one process exchange topics** (rmw_tickle/RMW_GAPS_PLAN.md g9, `tt_LOCAL_DELIVERY` in
  config.h). All of a process's nodes share one context, and a context dropped its own DATA as self-sent, so
  composition and every multi-node process got nothing on their in-process topics.
  - A publish now also delivers the sample to the context's own matching Subscribers, after it has gone to the link,
    by the same RxO rule as remote pairs.
  - `tt_Subscriber_deliver_local_backlog()` hands a late durable Subscriber the local durable backlog.
  - The flag is set only in rmw_tickle's build; core's default build is byte-identical.
- **rmw_tickle: several processes on one host each get a context id of their own** (rmw_tickle/RMW_GAPS_PLAN.md g8,
  `tt_CONTEXT_ID_CLAIM` in config.h). Until now every process on one address took the same id (the address's last
  octet) and dropped the others' packets as its own, so a talker and a listener in two terminals exchanged nothing
  unless `TICKLE_NODE_ID` was set.
  - A context now claims its id in a host registry in /dev/shm, keeping the last octet when it is free.
  - A packet is its own only when it comes from its own data socket.
  - Two contexts that still meet on one id are separated on the link: the newer one moves to an id nobody uses.
  - An explicit `TICKLE_NODE_ID` never moves.
  - No wire change. The flag is set only in rmw_tickle's build; core's default build is byte-identical.
- **rmw_tickle: `rmw_take_sequence`** (rmw_tickle/RMW_GAPS_PLAN.md g5). Takes up to `count` messages in order, following
  rmw.h's argument rules. The symbol was not defined before, so `rcl_take_sequence()` failed and
  rmw_implementation logged a failed lookup at every start.
- **rmw_tickle: on-new-data and event callbacks** (rmw_tickle/RMW_GAPS_PLAN.md g2). The four setters rclcpp's and
  rclpy's EventsExecutor use: `rmw_subscription_set_on_new_message_callback`,
  `rmw_service_set_on_new_request_callback`, `rmw_client_set_on_new_response_callback`, `rmw_event_set_callback`.

- **Nodes in discovery, wire `tt_VERSION` 11** (rmw_tickle/CONTEXT_NODE_PLAN.md stage 3, DESIGN.md "Contexts and
  nodes").
  - An announce lists the context's nodes (`tt_KIND_NODE` entries: namespace, name, own index), and every entry
    carries its node's index in 8 bits `kind` and `qos` did not use.
  - Receivers record the nodes and each remote endpoint's node (`tt_DiscoveredEntity.node_index`).
  - rmw_tickle's `ros2 node list`, `ros2 node info` and `ros2 param` now reach nodes in other processes; they saw
    only their own process's before.
  - An empty default node is not announced.
  - 40 B per node per announce list; the summary is unchanged.
  - Not compatible with v10 peers, which drop v11 datagrams, as on every version bump.

- **Nodes within a context** (rmw_tickle/CONTEXT_NODE_PLAN.md stage 2, DESIGN.md "Contexts and nodes").
  - A `struct tt_Node` is a name, a namespace and an index within its context, and owns endpoints:
    `tt_Node_create()`, `tt_Node_destroy()` (refused while the node owns an endpoint), and
    `tt_Node_create_{publisher,subscriber,client,server}()`. Every endpoint records its node's index (`tt_Endpoint.node_index`, in existing padding; `tt_Endpoint_node()`).
  - The `tt_Context_create_*()` shorthands create on the context's default node: index 0, named `tickle_<context
    id>` in `/`, and brought into use by the first shorthand call.
  - A context hosts up to `tt_MAX_NODES`: 16 by default, 256 in rmw_tickle builds, at most 256. Without a default
    node - every rmw_tickle context - index 0 goes to an explicit node too, and the shorthands then refuse.
  - rmw_tickle's nodes are core nodes and create every endpoint on them, so its contexts have no default node.
  - No wire change: the announce bytes are identical to before for the same endpoints.

- **A poll with no response pending takes no lock for it** (OPTIMIZATION_PLAN.md 11, D3):
  `tt_Server_send_response()` counts what it makes READY (`tt_Node.responses_ready`), and `tt_Node_poll()`
  flushes responses only when that count is non-zero - -13 ns per poll on the PC.
- **A received datagram reads no clock** (OPTIMIZATION_PLAN.md 11, D1): a peer's last sign of life
  (`traffic_last_seen`) and a MANUAL writer's assertion are stamped with the poll's reading of the clock, taken
  when a wait returns and every `tt_RX_CLOCK_REFRESH` (16) datagrams of a drain, instead of a read per
  datagram - -24 ns of an 83 ns received sample on the PC.
- **A received sample reads no clock** (WIRE_PLAN.md 8.1a): the tt_VERSION 10 timestamp is rebuilt against the
  time the running `tt_Node_poll()` already read (`tt_Node.rx_clock_ns`), not a clock read per sample - which
  had cost v10's receivers 19-70 ns of user time per sample on the Pi.
- **Short-lease summaries give way to traffic** (LIVELINESS_PLAN.md 10). A summary at the lease/6 cadence
  is skipped when every known peer has had a datagram from the node since the last tick; the
  `tt_NODE_UPDATE_INTERVAL` summary always goes. Under traffic a lease then runs from the data, as with DDS.
  New counter `tt_Node.summaries_skipped`. No wire change.
- **DATA_FRAG: samples larger than one datagram.** A new `tt_MAX_SAMPLE_LENGTH` bounds a sample's
  encoded size while `tt_MAX_BUFFER_LENGTH` keeps meaning one datagram. When it is larger,
  fragmentation is compiled in: a sample no `DATA` can carry is sent as `FRAG_FIRST` (type 8, the
  whole `tt_DataHeader` + `frag_count`) and `FRAG_CONT` (type 9, `entity_id`, `seq_no`,
  `frag_index`, `frag_count`), each alone in a datagram of at most `tt_CONTROL_MAX_LENGTH`. The
  receiver reassembles it in `tt_FRAG_REASSEMBLY_SLOTS` (8) node-level slots. At the default the
  two limits are equal and nothing changes: no reassembly memory, and an oversized sample is
  refused as before. New counters: `tt_Node.frag_reassembled`, `frag_abandoned`, `frag_dropped`,
  `frag_duplicate`.
  **Every datagram takes its own seq_no** - DATA, `FRAG_FIRST` and each `FRAG_CONT` - so a lost
  fragment is asked for and resent on its own through the existing ACKNACK bitmap. Consequences for
  callers: the subscriber callback's `seq_no` is monotonic, not contiguous, once samples fragment;
  `tt_ReliableCache.depth` counts datagrams (KEEP_LAST evicts whole samples); and a RELIABLE subscriber
  of a fragmented topic needs a reorder buffer, since fragments are held there until their sample is
  whole and acknowledged only once held. `tt_ReorderSlot` gained `frag_index`/`frag_count`, and reorder
  slots are now offset per writer.
  See DESIGN.md's "Samples larger than a datagram".
- **`tt_ReliableCache.sample_depth`**: KEEP_LAST's bound in samples, now that `depth` counts
  datagrams. When set, core evicts whole samples before holding more than that many
  (`retained_samples` counts them). 0 leaves `depth` as the only bound, as before.
  `tt_sample_datagrams()` and `tt_sample_cache_bytes()` (tickle.h) size a cache in samples: the seq_no
  and the arena bytes one sample of a given CDR length takes.
- **Latency trace stamps** (`include/tickle/trace.h`, `-Dtt_TRACE`; `rmw_tickle`: `-DRMW_TICKLE_TRACE=ON`):
  CLOCK_MONOTONIC stamps along a responder's receive-to-reply path - ppoll return, datagram to core, rmw
  delivery, signal, executor wake, take, publish, send return - into one process-global ring any thread
  may write. Measurement only; off, every stamp compiles to nothing. `rmw_tickle` writes them, with the
  node's lock counters, to `RMW_TICKLE_TRACE_FILE` at shutdown; `experiments/rmw_trace_segments.py` turns
  a dump into per-segment latencies. `tt_LockStats` gains `poller_contended`/`poller_wait_ns`, the waits
  the polling thread did, so a contended lock says who waited for whom. `rmw_tickle` also takes
  `-DRMW_TICKLE_RX_BATCH=N` for `tt_RX_BATCH`.
- **HAL: `tt_send_batch()`**, several datagrams in one call - `sendmmsg()` on Linux, one send each on
  FreeRTOS. **A HAL port must now provide it.** Core uses it for a sample's fragments and for one
  datagram to several destinations; a single datagram to a single destination still uses
  `tt_send()`/`tt_send_to()`.

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

- **A Publisher with no matched reader sends nothing** (docs/DESIGN.md section 4; the user's decision,
  2026-10-15), as a DDS writer does. With no known peer it used to broadcast every sample to the whole segment.
  - BEST_EFFORT samples published before any reader is matched are dropped and counted (`tx_unmatched_dropped`).
  - RELIABLE and durable ones are kept in the history as before (`tx_unmatched_kept`). A VOLATILE reader that matches
    later gets the samples from its match point on, and a TRANSIENT_LOCAL one gets the history.
  - Every sample size and path is covered, and so are the periodic, piggybacked, match and end-of-sample
    HEARTBEATs.
  - Discovery stays broadcast, and so does a send to more readers than `tt_UNICAST_PEER_THRESHOLD`.
  - A reader's first sample now waits for its announce to reach the writer: one `tt_CONTEXT_TX_INTERVAL` after the
    Subscriber is created, or within one `tt_CONTEXT_UPDATE_INTERVAL` if that announce is lost.
- **RELIABLE repair is as DDS/RTPS has it: no give-up on a count** (2026-10-15, the user's decision; docs/DESIGN.md
  section 6). A writer resends a datagram for as long as its history holds it (the per-sample cap of
  `tt_RELIABLE_RETRY`, after which a KEEP_LAST writer went silent without saying the datagram was gone, is removed)
  and answers a request for one it no longer holds with the eviction HEARTBEAT, as before; a RELIABLE writer with no
  history now answers the same way. Past its third resend a datagram goes at most once per retry interval (nack
  suppression, not under KEEP_ALL). A reader gives a gap up only on that HEARTBEAT (`gap_evicted`) or with the writer
  (departure, lapsed lease), never after `tt_RELIABLE_RETRY` retries. Past those retries its requests back off,
  doubling up to 16 times the interval (`tt_RELIABLE_RETRY_BACKOFF_SHIFT`, new), but only while the writer is quiet:
  a writer whose DATA still arrives is asked at the plain interval. KEEP_ALL gaps keep the plain interval. A
  backed-off recovery is not timed, and the retry count saturates. Before: a reader that did not know its writer's
  policy waited for good on a writer that had stopped (seed 34: 28 of 40 samples), and a KEEP_LAST reader gave up
  units the writer still held. `tests/test_hybrid_send.c` seeds 1-100, shared tests: 17 samples given up before, 0
  after (98 of 100 seeds pass in both). PC A/B (`keep_last_repair_pc_ab.sh`, veth, netem 1% and 5%, 5 reps; p4 10
  more): delivered %, wire bytes per sample, throughput and p99 HELD in every cell (core p1-p4 KEEP_LAST at max rate,
  latency p1/p4, 1 MB KEEP_LAST 10 at 30 Hz; the quiet-writer rule re-measured on p4 alone, the cell it decides); L4
  at 1% delivered +0.24 points (BETTER). The p4 cell's occasional receiver collapses, in every arm, ran deepest in the
  change (97.4% delivered against the parent's worst 98.3%). Two designs were refuted on
  the way: without the nack suppression p4 at 1% resent 412,632 datagrams against 19-31 thousand and delivered half;
  with a back-off on the retry count alone one p4 run in ten collapsed to 35% delivered. Rebased onto no-send
  (46554d68) and measured again against it, p4 alone (10 reps at 5%, 20 at 1%): every metric HELD; at 5% the change
  delivered 99.1-99.9% (parent 95.6-99.9%) with `gap_abandoned` 0 in 7 of 10 runs, at most 11 (window jumps; parent up
  to 411). At 1% receiver collapses hit every arm on a shared PC: runs below 90% delivered, parent 4 of 20 (worst
  10.6%), parent again 1, the change 2 (worst 60.3%).
- **RPC retry bounds are relative to the link** (docs/ROADMAP.md 5a, the constants audit). The auto retry interval
  (`call_retry_interval` 0) is RFC 6298's `srtt + max(tt_CALL_RETRY_GRANULARITY, 4 x rttvar)`, doubled at each
  retry of a call and at most `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` x srtt, in place of 1.5 x an EMA held between a fixed
  5 ms floor and 250 ms ceiling. `tt_CALL_RETRY_INTERVAL` is now only the seed before a first answer;
  `tt_CALL_RETRY_INTERVAL_MAX` is replaced by `tt_CALL_DEADLINE_PER_SEND` (250 ms), which bounds the call rather
  than the interval: an auto-path call still reports failure within `(count + 1) x` it (1 s), as before, while a
  measured srtt above 250 ms is used unclamped.
- **A Server keeps an answered response as long as its client may retry** (same item). `tt_SERVER_CACHE_TIMEOUT`
  (100 ms) is gone: the lifetime is the longer of the client's seed schedule (or the service's explicit retry
  schedule) and `tt_SERVER_CACHE_GAP_MULTIPLE` x the retry gaps the server has measured, re-armed by every retry it
  serves. A response is dropped when its client turns out to be a new incarnation - discovery lists it under
  another entity_id, or its node said farewell - so a restarted client reusing its id (seq_no back at 0) does
  not get its predecessor's answer, while a duplicated first request is still answered from the cache. A full
  cache evicts its oldest response instead of not sending the new one.

- **rmw_tickle: "matched" now means what it means in DDS** (RMW_GAPS_PLAN.md g3). An endpoint of the other kind is
  matched when all of these hold: same topic, same type, compatible QoS, and alive if remote.
  `rmw_publisher_count_matched_subscriptions` / `rmw_subscription_count_matched_publishers` use this predicate, so
  rclcpp's `get_subscription_count()` / `get_publisher_count()` no longer count type-mismatched or QoS-incompatible
  endpoints. Until now they counted by topic name.

- **`tt_Node` is now `tt_Context`** (rmw_tickle/CONTEXT_NODE_PLAN.md stage 1). The object that owns the
  sockets, the scheduler, liveliness and discovery is what rmw calls a context, and the name `tt_Node` is freed
  for the node rmw means (stage 2). No behaviour change: the -O2 `tickle.o`, `hal_linux.o`, `encoding.o` and
  `log.o` disassemble identically once names are normalised. A breaking API change, with no alias, so that code
  written against the old `tt_Node` fails to compile rather than meet stage 2's:
  - `struct tt_Node` -> `struct tt_Context`, and every `tt_Node_*` function -> `tt_Context_*`.
  - The settings `tt_NODE_CYCLE`, `tt_NODE_UPDATE_INTERVAL`, `tt_NODE_TX_INTERVAL`, `tt_NODE_MAX_LEASE_NS`,
    `_tt_NODE_ADDRESS`, `_tt_NODE_PORT`, `_tt_NODE_BROADCAST` and the sentinels `tt_NODE_ID_INVALID`/`_BROADCAST`
    -> `tt_CONTEXT_*`. An old name still set with `-D` stops the build with an `#error` naming the new one
    (`make -C platform/linux config-renames` checks each).
  - Public struct fields `node_id` -> `context_id` (`_tt_CONFIG`, `tt_Peer`, `tt_DiscoveredEntity`,
    `tt_DurableDeliveryRecord`, `tt_PeerAck`, `tt_WriterProxy`, `tt_ReorderSlot`). The wire is unchanged.
  - rmw_tickle's `tickle_node` field -> `tickle_context`.
- **The deferred-response storage is gone** (CONTEXT_NODE_PLAN.md roadmap, the user's item 3). A deferred response is
  encoded from the caller's struct when `tt_Server_send_response()` is called, so nothing was ever kept in it:
  `tt_Server.pending_response_buf` (64 x `tt_MAX_BUFFER_LENGTH` bytes per server by default), `pending_storage`
  and `pending_entry_length` are removed, and `tt_Server_set_storage()` takes only the cache area:
  `tt_Server_set_storage(server, cache_storage, cache_entry_length)`. `tt_SERVER_PENDING_ENTRY_LENGTH` is removed and
  stops the build if still set. rmw_tickle no longer allocates a pending area per service. Storage may now be
  attached while a request is deferred, since the deferred request keeps nothing in it.

- **The generated C++ converters copy primitive arrays and sequences as a block** (2026-09-27): `std::copy` /
  `assign()` over the container instead of an element loop, which the compiler could not vectorise. A 64-KB
  `sensor_msgs/Image` converts in 1.6 us each way instead of 86 / 43 us at -O2 (`rmw_tickle/RMW_PERF_PLAN.md`
  12.2). Interface packages must be rebuilt to get it: the converters are generated code.

- **`tt_Server_send_response()` encodes and sends before it returns** (2026-09-27), on the caller's thread under
  the node's state lock, as `tt_Publisher_publish()` does; the caller may free the response and everything it
  points at as soon as it returns. It used to hand the struct to the poll thread. `flush_pending_responses()` and
  D3's `tt_Node.responses_ready` gate (OPTIMIZATION_PLAN.md 11), which served that handoff, are removed.

- **A node announces a new endpoint at once, and answers a changed announce** (2026-09-26). Creating an
  endpoint used to wait for the next periodic announce, up to `tt_NODE_UPDATE_INTERVAL` (1 s), before any
  remote node heard of it, and a changed announce was answered only on first contact, so a Publisher created
  after its Subscriber's node was known could broadcast every sample for up to a second. Now a burst of
  endpoint creations sends one announce once it has been quiet for `tt_NODE_TX_INTERVAL`, and a changed
  announce that arrived by broadcast is answered with one unicast announce; a unicast announce - a reply - is
  never answered, so two nodes cannot trade them. The periodic announce, its interval and the liveliness
  arithmetic are unchanged. And an immediate publish, or a call request or its retry, with known peers
  flushes anything pending in `tx_buffer` (a batched announce, typically) as its own broadcast first and then
  unicasts, instead of joining that broadcast. On a veth rmw ping-pong: broadcast data samples 3/0/0 per run before, 0/0/0
  after, announces per node +0-7%.
- **Linux HAL reads with `recvmmsg()`** (`tt_RX_BATCH`, hal_linux.h): the drain after a wake-up takes up to
  32 queued datagrams a call, the first straight into the caller's buffer and the rest held in the node
  until `tt_try_receive()` asks - always before the next wait, so batching never holds a datagram back.
  The read that ends a wait stays a single `recvfrom()`: it is on the latency path, and a `recvmmsg()`
  probes for a second datagram before returning the first (+0.3 us on x86,
  `experiments/recv_single_cost.c`). A batch
  that comes back short marks its socket drained, sparing the drain loop the EAGAIN read that used to end
  it. On a backlogged receiver (veth, `experiments/veth_rx_batch.sh`) receive syscalls fell from 1.08 to
  0.06 a sample; on one that keeps up, batches run short and CPU per sample is unchanged within noise, as
  is round-trip latency. Costs 31 x `tt_MAX_BUFFER_LENGTH` of node memory (45 KB at 1472), so a build with
  a larger datagram - `rmw_tickle` - keeps `tt_RX_BATCH` 1, the plain `recvfrom()` path, until measured.
  The public HAL interface is unchanged; FreeRTOS is untouched.
- **Fragmentation follows the control datagram, not `tt_MAX_BUFFER_LENGTH`.** `tt_FRAG_ENABLED` is on
  whenever `tt_MAX_SAMPLE_LENGTH > tt_CONTROL_MAX_LENGTH` and can be overridden (`-Dtt_FRAG_ENABLED=0`),
  and a sample goes as one `DATA` only if it fits `tt_CONTROL_MAX_LENGTH`. A build with a large datagram -
  `rmw_tickle`, 65507 - now sends its large samples as `FRAG_FIRST`/`FRAG_CONT` instead of one datagram
  the OS splits into IP fragments; service requests and responses keep the large datagram.
- **`rmw_tickle`: every message carries an 8-byte publication sequence number ahead of its CDR**
  (`RMW_TICKLE_PSN_BYTES`), the publisher's own count of messages, which the subscription reports as
  `publication_sequence_number`. Core's seq_no counts datagrams once messages fragment, and ROS requires
  the gap between two psns to be the messages sent in between. **rmw_tickle nodes built before and after
  this do not interoperate**, and a core node reading an rmw_tickle topic must skip the 8 bytes.
- **`rmw_tickle` publisher caches are sized for fragmented messages.** KEEP_LAST: the ring is depth x
  the datagrams of the type's largest message (capped by what the arena can hold, and at 65535), with
  `sample_depth` = depth; growth is asked for when fewer than depth messages are retained. A message's
  record is `tt_sample_cache_bytes()` of its CDR plus the psn, and the first arena slice holds at least
  one of the largest. KEEP_ALL's depth (2048 / 8192) now counts datagrams, as the reader's tracking window
  it is paired with does. Reorder slots hold the psn too.
- **Wire protocol `tt_VERSION` 7 -> 8: discovery sends a small periodic summary and pulls the list on
  demand.** The whole endpoint list is no longer broadcast every `tt_NODE_UPDATE_INTERVAL`. A node
  broadcasts a ~28-byte summary instead - a `HEARTBEAT` of the discovery endpoint whose seq_no is its
  announce generation - which refreshes liveliness as the announce did. A receiver that has not applied
  that generation asks with an `ACKNACK` of the discovery endpoint, unicast, and the list comes back
  unicast (more than `tt_UNICAST_PEER_THRESHOLD` requests in one tick: one broadcast). Changes are still
  broadcast at once. New `tt_Node.discovery_reply_tick`/`discovery_reply_count`. Nodes of version 7 and 8
  do not interoperate. See DESIGN.md's "The periodic summary".
  An unanswered request is re-sent every `tt_DISCOVERY_REQUEST_RETRY` (10 ms), `tt_DISCOVERY_REQUEST_ATTEMPTS`
  (4) times in all, from a fixed `tt_Node.discovery_requests[tt_DISCOVERY_PENDING_REQUESTS]` table.
- **Wire protocol `tt_VERSION` 9 -> 10: the first wire bundle** (`rmw_tickle/WIRE_PLAN.md` §6). Framing per
  sample falls from 28 to 20 bytes in core and from 36 to 24 in `rmw_tickle`:
  - `tt_DataHeader.timestamp` is the low 32 bits of the sender's microseconds, rebuilt by the receiver
    within ±35.8 min of its own clock (W2). Callbacks still get nanoseconds. `tt_FRAG_DATA_HEADER_LENGTH`
    is 16, and `tt_RELIABLE_RECORD_BYTES()` counts 16.
  - `rmw_tickle`'s psn header is one 32-bit word below 2^31, two past it (W3).
  - A datagram with exactly one submessage addressed to every node goes with the 4-byte
    `tt_SingleHeader` instead of `tt_Header` + `tt_SubmessageHeader` (W4).
  Nodes of version 9 and 10 do not interoperate.
- **`rmw_tickle` honours `ROS_DOMAIN_ID`**: the well-known port is `_tt_NODE_PORT` + the domain id (0-232),
  so domains on one network no longer discover each other. Domain 0 keeps port 8282. Found when a PC's
  unit tests (domain 0) were heard by the rig's nodes (domain 73) over a shared management LAN.
- **Wire protocol `tt_VERSION` 8 -> 9: liveliness runs from the last sign of life** (LIVELINESS_PLAN.md).
  An AUTOMATIC lease is refreshed by any datagram from the entity's node, a MANUAL_BY_TOPIC one only by
  that Publisher's DATA or its new liveliness HEARTBEAT (`tt_HEARTBEAT_FLAG_LIVELINESS`, sent by the new
  `tt_Publisher_assert_liveliness()`). The verdict is taken by one timer at the earliest expiry
  (`check_liveliness()` no longer sweeps once a second), a lapsed entity revives on its next sign of life,
  a node is kept alive at least as long as the longest lease it announced, and a node's summary goes out
  at a sixth of its shortest lease (`tt_LIVELINESS_LEASE_DIVISOR`) when that is under a second. New `tt_DiscoveredEntity.last_asserted_ns`,
  `tt_Publisher.liveliness_asserted_ns`, `tt_Node.liveliness_check_*`, `liveliness_flags[]`,
  `next_summary_ns`. `rmw_tickle`: RMW_EVENT_LIVELINESS_CHANGED is updated from the core's discovery
  callback instead of a timer of its own; the 3 s lease floor is now 6 ms; AUTOMATIC Publishers announce
  their lease too; INFINITE goes on the wire as no lease; `rmw_publisher_assert_liveliness()` reaches the wire.
- **A node is presumed dead after `tt_LIVELINESS_SILENCE_NS`** (new, config.h): 3.5 update intervals of
  silence instead of 3, so two lost summaries in a row can no longer declare a live node dead when the
  nodes' schedulers drift across each other.
- **Wire protocol `tt_VERSION` 6 -> 7.** The discovery announce is now a `DATA` sample of a built-in
  endpoint (`tt_DISCOVERY_ENDPOINT_ID`, `tt_DISCOVERY_ENTITY_ID`) whose `seq_no` is the announce
  generation (the low 32 bits of `last_modified`), with a `tt_AnnounceHeader` + entities payload. A
  large announce is split into `FRAG_FIRST`/`FRAG_CONT` at entity boundaries, each fragment
  processed on arrival. `UPDATE` (type 1) and `UPDATE_PART` (type 7), `tt_UpdateHeader` and
  `tt_UpdatePartHeader` are retired. Nodes of version 6 and 7 do not interoperate.
  `tt_Node.update_last_modified[]` became `update_generation[]` (u32), and
  `tt_DurableDeliveryRecord.last_modified` became `generation`.
- A user entity is never given `tt_DISCOVERY_ENTITY_ID`; entity-id assignment skips it.
- `make lint` refuses a clang-format/clang-tidy whose major version is not CI's (`LINT_CLANG_MAJOR`,
  19) instead of reporting what the tool on `PATH` said. `LINT_ANY_CLANG=1` runs it anyway, and
  `make check-gates` passes it when it has already marked lint advisory.

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

- **A busy writer's send no longer sets the reader's retry interval** (2026-10-16; DESIGN.md section 6). A writer
  sending a large sample answers a request only behind the rest of its send, and those recoveries fed srtt: the
  interval came to measure the send, not the path. In `tests/test_large_sample.c`'s sim of 8 x 4 MB at 5% loss with a
  writer that reads nothing while it sends, it climbed to 43-61 ms and every sample after the first was whole 7-15 ms
  after its send ended, against 0.95 ms for the first. The reader now keeps the path's estimate without the
  recoveries whose wait saw the writer's original fragments arrive (`probe_busy`), and one over every recovery
  (`sending_srtt_ns`) that sets how often a retry may ask while the sample still arrives; then a retry also leaves out
  repairs younger than the transit time and asks nothing of a writer that has answered nothing within the interval.
  Sim: whole 0.05-1.95 ms after each send, writer datagrams 25,503 against 26,226; a writer answering while it sends
  over a 2 ms path: 6 duplicate copies against 245; a quiet path's srtt follows its round trip, 2 -> 8 ms. The
  reliable retry's ceiling is now 64 x max(srtt, G), as the call retry's is: of srtt alone, a recovery timed at ~0 cut
  every wait to 64 ns. PC (`srtt_busy_writer_pc_ab.sh`, veth, 10 reps, 4 MB RELIABLE KEEP_LAST at 15 Hz): at 5% netem
  p99 latency -25% (BETTER, base 13.7 ms), median HELD, delivered 100% in every arm; the writer's resends +7.7% and the
  reader's datagrams +11.6% (WORSE), wire bytes +0.3% (HELD); at 0% every metric HELD. Core p4 at 1%/5% (5 reps, the
  previous step of the change, whose non-large code is this one's): delivered, wire bytes and p99 HELD. Two earlier
  steps were refuted on the way: retries at the path's interval while the writer sends (+37% writer datagrams at 0%
  netem on a loaded PC), and the transit rule without the sending cadence (the reader's occasional collapses ran
  deeper than the parent's: 1.23 million resends and 234 of 300 delivered against 0.35 million and 300).

- **The hybrid reader's collapse test no longer fires on plain frame loss read off a young baseline, and calls a
  squeeze sooner** (2026-10-17; DESIGN.md section 8, "The collapse test", amended). Its significance now reads `L0` at
  `p_mtu`'s 99.9% bound - the test's own level - instead of the 95% one, which a baseline of a few hundred tails
  undershot one time in twenty: `test_frame_loss_never_moves_the_cap` (1% loss) called a false collapse (cap 45 -> 7)
  in 7 seeds of 1,000, in none of them now; the squeeze arm passes seeds 1-132 and 601-654 (seed 37, a false collapse, among them). The size rule `X >= 2 R L0` (still at 95%) is read on the survival side
  where `2 L0 > 1`, where it could never be met: a squeeze that refused every run on a young baseline waited for more
  tails. Detection after the first refused run (sim, 1 MB at 30 Hz, 4 MB reassembly memory): 4.52 -> 1.17 s at 0.1%,
  6.22 -> 2.08 s at 1%, 0.33 -> 0.38 s at 0.01%; runs refused by then 2,571 -> 863 at 0.1%.

- **p_mtu counts samples lost whole** (2026-10-17). A hybrid reader never saw a sample whose runs and tail were all
  lost, so it counted none of it: `p_mtu` read `p (1 - L(n))` for one-run samples (0.88 p at 1%, 0.56 p at 5%, 16 KB at
  `n = 11`), the collapse test's baseline with it - 3 of the 7 false collapses in seeds 1-1000 of
  `test_frame_loss_never_moves_the_cap`, and both in 50 plain streams at 5% - and the run window missed the same runs. The reader now counts them from the writer's seq_no: a jump of up to 8 whole samples of the
  size on both sides is that many samples lost, laid out as the last one (DESIGN.md section 8, "What the reader
  measures"; `missed_samples=` on the hybrid line). A repair, addressed to its reader, no longer passes for a first
  transmission (a repaired tail read as a tail through). `p_mtu` and the run loss now match the link's own count
  exactly in every measured cell (0.1%, 1%, 5%; one run, two runs, AUTO).

- **A step of the date no longer disturbs timers, liveliness or lifespan** (2026-10-16, `ab/monotonic`; DESIGN.md
  section 2, "Time"). The Linux HAL read `CLOCK_REALTIME` for everything, so an NTP or manual step moved every
  deadline: three hours forward, a leased writer was declared gone and a lost sample's repair was refused as past the
  publisher's lifespan; three hours back, the scheduler stalled - no heartbeat, retry or application timer ran
  (`tests/test_clock_step.c` against the single-clock core). `tt_get_ns()` is now `CLOCK_MONOTONIC` (as are the
  epoll deadline `timerfd`, `tt_Context_lock_timed()` and rmw_tickle's condition-variable waits), and a new HAL call,
  `tt_get_wall_ns()`, gives `CLOCK_REALTIME` to what leaves the process as a point in time: the DATA timestamp, the
  source timestamp handed to a subscriber, the per-launch identity draws. A HAL port adds `tt_get_wall_ns()`
  (FreeRTOS: the same counter, no RTC). rmw_tickle: `received_timestamp` is the date, and a subscription's lifespan
  takes the age on arrival and counts the rest on the monotonic clock, so a source stamp ahead of the reader's date is
  no longer dropped at once as an unsigned wrap. `examples/linux/perf/perf_server.c` times its windows on the receiver's
  own clock and only the one-way latency on the date.

- **A late DURABLE reader gets the whole backlog of a writer that had sent more than its window** (2026-10-15). The
  reader's first contact is the backlog's oldest DATA, which lay a window or more past its default baseline of 1; the
  baseline jumped there but the reorder cursor stayed at 1, so the drain walked past the oldest sample. Its first
  sample was lost, and with samples of more than one datagram its stranded fragments filled the reorder ring until
  the stream stalled: 127 of 200 two-datagram samples and 24 of 200 ten-datagram ones from a writer at seq_no 1000,
  the first of three 1 MB samples from one at 100000. The reader now takes that sample as its baseline with the cursor,
  as a VOLATILE reader does (`tests/test_durable_late_joiner.c`). Still open: a backlog datagram lost before the first
  that arrives (the oldest sample is then lost), and a loss early in a backlog wider than the reader's window (the
  burst jumps past the hole).

- **A server runs its callback once per call when one context has two Clients of the service** (2026-10-09). The
  server kept one cached answer per source context, so a retry from one Client that arrived after the other Client
  was answered ran the callback again. A CallRequest's pad byte now carries `client_tag` (which Client of its
  context is calling, 1..255; 0 from an older sender), and the server keeps one answer per (context, client_tag) and
  looks it up by (context, client_tag, seq_no). Compatible both ways without a `tt_VERSION` change: an older sender
  writes 0 and gets the old behaviour, an older server ignores the byte. A Client also skips a seq_no another Client
  of the service still waits on when the context's counter comes round. In a soak of 400 interleaved calls at 25%
  loss, 70 callbacks ran twice before and none after.

- **Two interface packages with a same-named message no longer share one codec** (RMW_GAPS_PLAN.md g12,
  2026-09-28). **Every interface package must be rebuilt**: the generated names change, so code naming
  `<Name>Data` on the ROS 2 path stops compiling rather than linking to the wrong thing.
  - The generator named a message's TickLE struct and its codec after the message alone, so
    `std_msgs/String` and `example_interfaces/String` both defined `StringData_encode`, and
    `action_msgs/GoalStatus` and `actionlib_msgs/GoalStatus` both defined `GoalStatusData_encode`. Whichever
    library loaded first served both. 144 symbols collided across the shipped inventory.
  - Where the two types had the same layout nothing showed, which is why it went unseen. Where they did not,
    one type's messages were encoded by the other type's codec: `actionlib_msgs/GoalStatus` came out as 25
    bytes of `action_msgs`' fixed-size struct, heap pointers included - wrong data, and addresses on the wire.
  - Generated ROS 2 names are now package-qualified (`std_msgs__msg__StringData_encode`). TickLE's own
    generator is unchanged: its users write those names by hand.
  - `rmw_tickle/scripts/check_interface_symbols.sh` refuses a workspace where two interface packages define
    the same symbol, and Check all runs it over the whole inventory on every push. A one-package-at-a-time
    check could not have found this, because a collision needs two packages loaded at once.

- **A datagram from a peer is dropped and counted, never a fatal poll return** (RMW_GAPS_PLAN.md g11,
  2026-09-28).
  - An unknown wire version, a bad magic, a truncated header or a submessage that does not walk made
    `tt_Context_poll()` return `tt_RET_PROTOCOL_ERROR`, and every poll loop TickLE ships ends on anything but
    OK or TIMEOUT. One UDP datagram from anyone who could reach the port therefore ended such a node, before
    any discovery - and one v10 process ended a v11 server mid-measurement on the rig. rmw_tickle was never
    affected: its poll thread ignores the return.
  - `rx_malformed_drops` counts them, `version_mismatch_drops` the version mismatches among them, and both are
    on rmw_tickle's shutdown line. An error return is now only for this node's own failures.

- **Discovery lookups no longer scan the table** (CONTEXT_NODE_PLAN.md 4b, 2026-09-27).
  - Every received DATA's RxO check found its publisher with a linear scan, as did a MANUAL_BY_TOPIC publisher's
    liveliness assertion, which scanned all of it. So did every announced entity, up to three times (the lookup, an
    empty slot, the source's liveliness flags).
  - At the 2048 entries rmw_tickle has held since 4a, that was up to 1.2 us per received sample in a large graph,
    and 8.5 ms per 2000-entity announce, repeated on each changed announce.
  - Now, in builds whose table is larger than 64 (`tt_DISCOVERY_INDEXED`, rmw_tickle's), `tt_Discovery` has an
    index over (context id, endpoint id) and a free-slot cursor, and the liveliness flags are refreshed once per
    announce fragment: 8 ns per sample wherever the entry sits, and 0.24 ms per 2000 entities.
  - A smaller table keeps the scan, which at 16 costs no more than a hash; core's default build is byte-identical to
    before.
  - A caller that writes an entry's key directly calls the new `tt_Discovery_reindex()`.

- **A discovery table too small for the graph let RxO-incompatible DATA through, and rmw_tickle's held 16**
  (CONTEXT_NODE_PLAN.md 4a, 2026-09-27).
  - `tt_Discovery` does more than introspection. A subscriber's RxO check reads it, and fails open for a publisher it
    has no room for: that publisher's DATA is delivered.
  - A reliable subscriber's KEEP_ALL classification, per-entity liveliness leases and rmw_tickle's BEST_AVAILABLE
    resolution read it too.
  - rmw_tickle never raised core's default of 16, so every ROS 2 graph of more than 16 remote entities was affected
    (every rclcpp node has 9 by default). `test_discovery_capacity` shows it: at 16, the 20th of 20 best-effort
    publishers is dropped and its DATA reaches a reliable subscriber. rmw_tickle builds now hold 2048.
  - A full table now counts what it drops (`tt_Discovery.entities_dropped`, printed in rmw_tickle's shutdown line) and
    warns once, naming the setting. It used to log one line per dropped entity.
  - Fail-open on a missing entry is unchanged; whether it should fail closed is a separate decision.
- **An rmw_tickle context of more than ~416 endpoints announced nothing** (4a). An announce was limited to 32
  1472-byte fragments of about 13 ROS-length endpoints each.
  - `tt_UPDATE_MAX_PARTS` is now a setting, at most 255 (the wire's `uint8_t` count); rmw_tickle builds set 255.
  - `tt_MAX_ENDPOINT_COUNT` is no longer capped at 256. It never indexed anything 8-bit: the per-peer tables it sized
    are indexed by a remote context's id, and are now sized by a fixed `tt_MAX_CONTEXT_IDS`.
  - rmw_tickle builds hold 2048 endpoints; a composed Nav2 bringup holds about 600.

- **A client on the auto retry interval could time out every call, forever** (CONTEXT_NODE_PLAN.md "Client retry
  fix", 2026-09-27). With `call_retry_interval` 0 - what core's examples and generated services pass - the interval
  was 1.5 x an EMA of accepted answers' latency, with no floor, and a timeout taught it nothing: one fast answer
  (85 us, measured) shrank every later call's budget to about 0.5 ms, and once an answer took longer every call timed
  out and the estimate never moved again. Native users on the default interval were affected; rmw_tickle was not
  (it sets its own interval). Now, on the auto path only:
  - the interval never goes below `tt_CALL_RETRY_INTERVAL` (5 ms);
  - a timeout doubles the estimate (from the floor at least), up to a new `tt_CALL_RETRY_INTERVAL_MAX` (250 ms), so
    a call ends within `(tt_CALL_RETRY_COUNT + 1) x tt_CALL_RETRY_INTERVAL_MAX` (1 s) even against a dead server;
  - the first answer accepted after a backoff replaces the estimate, so a server that is fast again gets its short
    budget back at once.
  An explicit `call_retry_interval` is used as given, as before. `test_call_retry_adaptive` checks each rule with a
  simulated server; the old code fails 7 of its checks.
- **Submessage padding carried stale bytes onto the wire - a confidentiality fix** (2026-09-27). `end_encode()` pads
  every submessage to 4 bytes and counts the padding in its length, so it is sent; it was never written, and held
  whatever an earlier datagram had left in `tx_buffer` at that offset - up to 3 bytes per submessage, possibly of a
  datagram addressed to a different peer, sent to a receiver it was not meant for. It is now zeroed.
  - It also made a payload end in arbitrary bytes. Generated TickLE codecs (core's and rmw_tickle's, both from
    `tools/typesupport`) read strings through their length prefix and never read the tail, so they, and rmw
    services and topics, were not affected (`test_encode_padding` decodes a generated message with stale and with
    zeroed padding). A hand-written codec that expects its payload to end where its data does - as
    `test_thread_safety`'s did, a string with its terminator last - failed on every retry of a call whose padding
    held a non-zero byte: the cached response is resent unchanged, so the client never completed the call. That
    was the 1-2% of `test_thread_safety` runs that failed or hung.

- **A deferred service response no longer carries freed memory** (2026-09-27): `tt_Server_send_response()` copied
  the response struct shallowly and the poll thread encoded it later. rmw_tickle's response structs alias the ROS
  response, which rclcpp frees as soon as `rmw_send_response()` returns, so every string longer than
  `std::string`'s inline 15 bytes arrived as garbage - `ros2 service call .../list_parameters` on any default node.
  The response is now encoded and sent before the call returns.
- **An rmw_tickle service no longer dies after ~121 calls in 5 s** (2026-09-27): answering a deferred request left
  its 5 s slot timeout armed, the scheduler filled, the next request could not be deferred, and rclcpp terminated
  the server ("Cannot schedule pending_response_timeout"). The answered slot's timeout is disarmed.

- A Publisher or Client created after a matching remote endpoint had been announced never learned it,
  and a Publisher without peers broadcasts every sample: `process_announce()` skips the periodic resend of
  an announce it has already acted on, and a remote node whose endpoints do not change resends the same
  one indefinitely. Creating a Publisher or Client now marks every known node's last announce as not acted
  on, so its next resend (within `tt_NODE_UPDATE_INTERVAL`) is matched against the new endpoint. Seen on
  the rig: 4 of 7 `rmw_tickle` ping runs broadcast all 100 pings (2026-09-26).
- `rmw_tickle`'s `publication_sequence_number` was core's 16-bit callback `seq_no`, so it wrapped to 0
  every 65,536 messages, breaking ROS's contract that it increases (DATAFRAG_PLAN.md 13.5). It is now
  the publisher's own 64-bit count, starting at 1 and advancing only for a message that was sent.
- The logger formatted its timestamp with `localtime()`, whose one static `struct tm` is shared by
  every thread, so two nodes logging at once on two threads could print each other's time. Now
  `localtime_r()`. Found by `test_thread_safety` under ThreadSanitizer.
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
