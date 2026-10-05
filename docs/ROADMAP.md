# TickLE roadmap

One page for what is being worked on, what comes after it, what waits on the user, and what just landed.
The details live in the source documents named in parentheses. Last consolidated 2026-10-05.

## Now

The user's active list (2026-10-05), in order.

0. **A fair testbed** (the user, 2026-10-05; ahead of everything below once rmw KEEP_ALL is closed): the published
   comparison must not rest on a setup tuned for TickLE. Found: the same-host cells (S1-S16, `s6_transport_cells.sh`,
   `s6_witness_check.sh`) pin each process to ONE core, which squeezes the multi-threaded DDS processes and suits
   single-threaded TickLE; the cross-host cells pin every framework to cores 1-3 (symmetric, but not how software is
   run). Pinning began because Pi core 0 takes the interrupts. Re-measure the headline tables with no pinning, the
   OS scheduling every framework, and publish the pinned figures only as a labelled note. Then audit the rest of the
   rig the same way: socket buffer sizes, CPU governor, IRQ affinity, vendor profiles (FastDDS XML, CycloneDDS
   URI, iceoryx), build flags. P1-P4 stay as they are: each measures something real (the user, 2026-10-05).
   Audited 2026-10-05: socket buffers (all three capped at the rig's 212,992 B; if `rmem_max` is ever raised,
   FastDDS's XML must raise its own too), governor (`ondemand` for all; a `performance` arm needs the user's sudo),
   IRQs (all on CPU0 for all), vendor profiles (neutral; CycloneDDS's shared memory is opt-in and enabled for it)
   are even. Fixed: the latency clients now pace alike (TickLE's sent on a fixed period and idled one round trip
   less) and print median and p99 beside the mean; the campaign rotates the frameworks' order; rmw_tickle is
   built at the vendors' `-O2` instead of `-O3` (`rmw_crosshost_rtt.sh`; `rmw_keepall_rig.sh` next); versions
   are in RESULTS section 3. Open: (a) every framework's KEEP_ALL bound in the campaign is derived from TickLE's
   512 KiB budget (`campaign_sweep.sh` common_args) - identical, but chosen from TickLE's side; an arm with the
   vendors' own defaults (CycloneDDS unlimited, FastDDS 5000) should sit beside it; (b) `s6_transport_cells.sh`
   runs each framework's repetitions back to back, not interleaved.
1. **Find the p4 same-host latency gap**: p4's round trip is ~21 us longer than p3's, but only after the processes
   idle (rig: +6 us at 0.5 ms ping spacing, +20 us at 5 ms; CycloneDDS 0-2 us). Ruled out so far: an extra wake
   (p4 rings the same doorbells per round trip as p3, `p4_wake_count.sh`), and the doorbell sitting between the two
   fragments (`93504234` moved it after the batch; the gap stayed +21 us at 5 ms, falsified by its own rule). Next:
   per-stage timestamps on the rig. (`p4_interval_rig.sh`; RESULTS S10)
2. **Fill COMPARISON 1a's empty cells**: CPU and RSS for all three frameworks on every same-host cell (CycloneDDS
   including iox-roudi), and the vendors' DELIVERED rates on the same-host BEST_EFFORT rows. The rows compare send
   rates today, and FastDDS's KEEP_LAST 1 reader took 154 of 3.1M samples. (COMPARISON 1a notes)
3. **Publisher user time** (0.637 us/sample at p3): broken down with perf on the rig (RESULTS, same-host notes):
   clock 25%, memcpy 28%, segment indices 12% (now on their own cache lines, `e0873623`). Next: let
   `tt_Publisher_publish()` called from a scheduled callback reuse the time the poll loop just read (one of the four
   clock reads), and copy once by encoding into the slot.
4. **Reduce the reader wake cost** (~3.1 us of system time per wake) without tuned spin values: each wake is paid
   per sample at low rates. (COMPARISON 2.2c; SHM_PLAN 7 q1)
5. **rmw_tickle KEEP_ALL on the rig**: the first run lost ~5% of samples under 5% loss in every build. Six causes
   found and fixed on 2026-10-05: a busy socket starved the data socket (`b6de8a4d`); an endpoint was announced
   before its QoS was final (`155eecb7`); a publisher could not learn its reader except from an announce lost in
   its own looped-back flood (`25c2e1ce`, matched from the ACKNACK; `71289352` and `5028366d`, the reader
   acknowledges a KEEP_ALL writer once); the reader's queue overflowed when the reorder buffer released a burst
   (`7b760c5d`); KEEP_ALL evicted the unacknowledged rest of a fragmented sample (`7fb6fabf`). At `7b760c5d`, 0%
   loss evicts nothing (6/6). Then three more: the poll thread starved of the state lock (`40db021d`), a reader
   that had not heard the writer announce did not acknowledge it (`a4408d0a`), and an unmatched KEEP_ALL writer
   evicted by bytes what it broadcast before its first match (`a8c1cd83`). **Closed 2026-10-05:** at `a8c1cd83`
   all 12 runs lose nothing, at 0% and 5% loss (RESULTS rows 72-75). Also queued as its own task: a reader process that hangs after
   its writer leaves. (`rmw_keepall_rig.sh`, `rmw_keepall_evict_repro.sh`, `rxo_mismatch_repro.sh`)
5a. **Testbed-independent constants** (added by the user 2026-10-05, not urgent; ordered after the reader-wake item
   and before rmw same-host and FreeRTOS, because FreeRTOS is the first other platform and re-measurements should
   rest on settled constants): every tunable in TickLE (intervals, thresholds, ring and window sizes, retry and
   spin counts, timer periods) was measured on two Raspberry Pi 5s and may be fitted to them. For each, record
   whether it is (a) derived by an algorithm from what the running system measures (preferred - e.g. the dynamic
   retry interval from srtt/rttvar), (b) a protocol or memory bound that does not depend on the platform, or (c) a
   value fitted to the rig. Replace (c) with (a) where possible; where a fitted value stays, README.md gives the
   formula that produced it and how to recompute it on another platform. Start from the old constants audit
   (`git show 3c0c505b:examples/perf_hil/CONSTANTS_AUDIT.md`) and include config.h, hal_linux.h and rmw_tickle's
   RMW_TICKLE_* defaults.
   Inventory done 2026-10-05; the fitted values that stay (c) are in README.md "Tuning for your platform". Still
   to replace by an algorithm (a), most fitted and hottest first: `tt_RELIABLE_RETRY_GRANULARITY` 100 us (measure
   timer lateness); `TT_RX_IDLE_RECHECK` 64 and `tt_SEGMENT_ATTACH_RETRY_SENDS` / `_REVALIDATE_SENDS` (send counts
   whose period scales with the rate); `tt_CALL_RETRY_INTERVAL` / `_MAX`, `tt_SERVER_CACHE_TIMEOUT` and
   `RMW_TICKLE_CLIENT_RETRY_INTERVAL_NS` (srtt-relative bounds, as `08e568af` did for the reliable retry);
   `RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY` (from the tracking window); `tt_DISCOVERY_REQUEST_RETRY` (RTT +
   `tt_CONTEXT_TX_INTERVAL`) and `tt_SEGMENT_DEAD_READER_NS` (from `tt_LIVELINESS_SILENCE_NS`).
   Done: `tt_SCHEDULER_IO_INTERLEAVE` (8 entries) is now `tt_RX_CHECK_RATIO`, a timed receive check - the budget is
   the ratio times the measured cost of an empty peek, capped at `tt_RECEIVE_TIMEOUT` (`test_rx_check_budget`); a rig
   A/B against the count is still owed before its figures are re-quoted.
6. **Re-measure rmw same-host performance**: the rmw same-host rows predate the segment, FIFO and wake fixes.
   (COMPARISON 2.7; RMW_PERF_PLAN)
7. **FreeRTOS: implement tt_rx_maybe_ready() with an lwIP netconn receive callback**: today only the Linux HAL has
   the receive hint. lwIP's socket layer hard-wires its own netconn callback (`DEFAULT_SOCKET_EVENTCB` in
   `sockets.c` is not configurable), so the callback needs the HAL on the netconn API: `netconn_new_with_callback()`
   per socket, a callback that counts arrivals and gives one FreeRTOS semaphore, `tt_receive()` waiting on that
   semaphore (which also replaces the loopback wake socket), reads with `NETCONN_DONTBLOCK`. About 250 lines of
   `src/hal_freertos.c`; `make test-freertos` is the check. (include/tickle/hal.h; SHM_PLAN 7 q3)
8. **CycloneDDS arm of the mixed shared-memory + network test**: the mixed-delivery test has no Cyclone arm yet.
   It needs iceoryx plus network config. (COMPARISON 1a, mixed delivery)

## Next

Open work that is not parked, deduplicated across all sources. Rough priority order within each group.

### RESOURCE_LIMITS shaped like DDS (the user, 2026-10-05)

KEEP_ALL is bounded today by two process-wide byte budgets (`RMW_TICKLE_KEEP_ALL_BYTES`,
`RMW_TICKLE_READER_KEEP_ALL_BYTES`), a per-publisher override, and fixed sample-count ceilings the user does not see
(the tracking window, the index ring, the reader's 4,096). DDS bounds it per writer and per reader in samples
(`max_samples`, `max_samples_per_instance`), checked against HISTORY at creation. Follow DDS's shape: a sample-count
limit settable per publisher and per subscription, one visible bound rather than several, and the same consistency
check. How (QoS extension, payload, both) is decided when the work starts. Comparison: docs/RMW.md, QoS,
RESOURCE_LIMITS.

### A core QoS API shaped like rmw's (the user, 2026-10-05)

Core's QoS is fields set one by one on the endpoint (`reliable`, `durable`, `keep_all`, `deadline_duration_ns`,
`lifespan_duration_ns`, `liveliness_lease_duration_ns`, `liveliness_manual`), and history is storage the caller
sizes and attaches (`reliable_cache` with its index and arena; `capacity`, `depth` and `sample_depth`; the
subscriber's `reorder_storage`). rmw and DDS take one QoS profile (history + depth, reliability, durability,
deadline, lifespan, liveliness + lease) applied at creation. Give core a QoS profile struct shaped like
`rmw_qos_profile_t`, applied at endpoint creation, with a helper that computes the storage a profile needs so the
caller still owns the memory (no malloc in core). Keep the field API working meanwhile; `max_blocking_time` and the
RESOURCE_LIMITS item above belong in the same profile.

### rmw_tickle at REP-2004 Quality Level 2 (the user, 2026-10-05)

The goal: from Quality Level 4 (`rmw_tickle/rmw_tickle/QUALITY_DECLARATION.md`) to Level 2. What Level 2 asks and
what is missing, in order:

1. **Every change through a pull request**, with DCO sign-off checked and CI required before merge (REP-2004 2.i,
   2.ii, 2.iv). Today changes are pushed to `main` after the local gates, unsigned. Needs the user's decision,
   since it changes how every session works. (Peer review is optional at Level 2 and required at Level 1.)
2. **Coverage tracking** in CI for core and rmw_tickle; Level 2 tracks it, Level 1 enforces a bound (4.iii).
   Core: done - `make coverage` (gcovr, unit tests, HALs excluded), run by `test-all.yml` with the table in the job
   summary and the reports as an artifact; 90.6% of lines and 72.6% of branches on 2026-10-05. rmw_tickle: open -
   CI's colcon build in `check-all.yml` is not built with `--coverage`; an untested recipe is in the declaration's
   4.iii.
3. **A vulnerability disclosure policy** (`SECURITY.md`, REP-2006 response schedule) (7.i).
4. **Dependencies at Level 2 or better** (5): quality declarations for `rosidl_typesupport_tickle_c` and
   `rosidl_typesupport_tickle_cpp`, and a written quality justification for TickLE core, rmw_tickle's one non-ROS
   dependency.
5. **A stable version** (`>= 1.0.0`) with declared API and ABI stability policies (1.ii, 1.iv, 1.v).
6. **All REP-2000 Tier 1 platforms** (6): Linux only today. Check jazzy's Tier 1 list; a platform not supported
   (Windows, if listed) needs a HAL port or a documented exception.

### Wired work (the user's order of 2026-09-29: finish wired, then Security, then wireless)

- **Receive-buffer lending** (`tt_Sample_retain`/`release`, SHM stage 2 / S7): most of the same-host win is
  here, and it is the machinery loans need. User-approved 2026-09-28. (PLAN "wired work" #2; SHM_PLAN 5; RMW_GAPS S7)
- **Loaned messages** (`rmw_borrow_loaned_message` and five more): the last ❌ in the API table, 0 of 6 today.
  (PLAN #3; COMPARISON 2.7a)
- **Large-message stage 2** (samples above 64 KB): user's staged-support decision. It reuses lending's machinery
  and is not yet pre-registered. (PLAN #4; LARGE_MESSAGE_PLAN)
- **Introspection round-trip rows** (names, type names, GIDs, event counts, serialization format): only the QoS row
  is done. (PLAN #6; RMW_GAPS g14 generalisation)
- **Housekeeping**: delete the merged ablation branches `d4-state-lock-chunk-on-main` and `d5-inline-skip-gate`.
  Nothing blocks this since WIRE 10.5. (PLAN #8; WIRE_PLAN 10.5)
- **Same-host discovery**: keep UDP for discovery, or move it into the segment? Moving it means no network stack
  on embedded. (SHM_PLAN 7 q2)
- **FreeRTOS form of the segment** (HAL-provided rather than `shm_open`): it shapes the seam. (SHM_PLAN 7 q3)
- **Stage 1 / S1 against the WIRE 10.4 floors** (CPU, RSS and size, p1-p4): the module's "no cost when off" is still
  owed. g15's uncounted zero-copy sends close with it. (SHM_PLAN 6b; RMW_GAPS S1/S3/g15; MODULE_PLAN 3)
- **Refuse the shm-only submessage when it arrives on the socket, with a test**: stops injection from the network.
  (SHM_PLAN 6e(a))
- **S9 window A**: a service datagram too large for one path sends one leg over UDP and one in shm, and may
  reorder. It can be measured now. (RMW_GAPS S9)
- **Received samples carry an all-zero publisher_gid**: tools cannot match a sample to its writer.
  (RMW_GAPS g14 generalisation)
- **Run the rmw behaviour tests locally in the netns**: today they run only in CI. (RMW_GAPS "Known gap in the local
  gates")

### rmw latency and correctness

- **rmw kernel wake (~6 us) and poll-to-executor handoff (~21 us)**: the one segment where the vendors are faster.
  (COMPARISON "Where TickLE does not come first"; COMPARISON to-do 3; RMW_PERF_PLAN 8.4/8.5; WIRE_PLAN 5)
- **C++ subscriber segfault, once in ~370 runs**: cause unknown and never reproduced. (PLAN "Open", rmw tests)
- **Discovery peer-registration race**: a first DATA can still be broadcast before its peer is registered.
  (DISCOVERY_PLAN 9)
- **Reorder buffer smaller than the window causes a re-request storm**: integrators who size the buffer small are
  exposed. Add a size check or stop re-requesting what cannot be held. (COMPARISON to-do 17)

### Measurement debt

- **Re-run the c6 cell with the pre-registered `retransmitted` figure (~46x)**: it is not yet comparable.
  (COMPARISON 2.8; OPTIMIZATION_PLAN 9a)
- **Re-measure the reliable_throughput cells marked ◊ under `taskset -c 1-3`**: the CPU0 IRQ coin flip.
  (COMPARISON to-do 14; PLAN "still outstanding")
- **Re-measure COMPARISON's A/T/V rows after large-message stage 1**: those rows read slightly optimistic on client
  CPU and RSS. (WIRE_PLAN 10)
- **Confirm DATA_FRAG's under-loss throughput (527-536 / 830-857 Mbps) in the full cross-vendor campaign**: the gain
  was not predicted. (DATAFRAG_PLAN 14)
- **DATA_FRAG follow-ups**: arm C with a 30 s FastDDS drain cap, a raw-line re-run of the 4-fragment arm, the
  unexplained 0.57 duplicates per loss on the rig. (DATAFRAG_PLAN 12, 16)
- **More digits in the RESULT lines of all three harnesses**: the p4 cpu_s_per_MB verdict flips on rounding.
  (DATAFRAG_PLAN 11)
- **P3 bench shape has 3.4 B of headroom**: move it to ~1400 B or gate it in `check_bench_shapes.sh`.
  (OPTIMIZATION_PLAN 3a)
- **Discovery join time (M3) is not measured**: `discovery_join.sh` must be adapted first.
  (CONTEXT_NODE_PLAN stage 3 result)
- **rmw_zenoh QoS rows 42-44 and 47 are not measured**: the QoS probe now exists to measure them.
  (COMPARISON 2.6)
- **Harness residuals**: scenario 6's late-join loss varies from run to run (to-do 2), scenario 9 pauses for
  different times per arm (to-do 6), the LIFESPAN pause is bimodal (to-do 8). (COMPARISON to-do 2/6/8)
- **FastDDS liveliness server's unsynchronised globals**: a formal data race. Low priority; fix it when the file is
  next touched. (COMPARISON to-do 16)

### Throughput and CPU

- **Poll-loop I/O interleave branch** (`experiment/poll-loop-io-interleave-v2`): ~15-25% more throughput, never
  merged. (COMPARISON to-do 11)
- **recvmmsg rows processed in place, then scatter-gather for large fields**: copies remain on the receive path.
  (OPTIMIZATION_PLAN 12.1/12.2)
- **Decide what `rmw-perf.yml` and the two dashboard charts become**: recorded but never decided.
  (PLAN "standing methodology")

## Parked - only on the user's explicit go

- **SECURITY_PLAN: SROS2-compatible security with MACsec**: five open questions. Nothing is to be designed until
  the user starts it, and it comes before wireless. (SECURITY_PLAN; MODULE_PLAN 0/4; SHM_PLAN 7 q4)
- **WIRELESS_PLAN** (W1 correlated loss + delay, W2 Wi-Fi, W3 internet, relay, congestion control): queued behind
  Security. (WIRELESS_PLAN; PLAN "Priority")
- **OS IP fragmentation under loss**: deferred by the user. The sources record DATA_FRAG as the answer since
  2026-09-26. (COMPARISON to-do 15; DATAFRAG_PLAN header)
- **zenoh-pico p2-p4 reliable cells stay not measurable**: zenoh-pico does not repair loss. Never quote the
  rate-limited figure. (ZENOH_PICO_PLAN 1; COMPARISON 4.4a)
- **O(1) `find_resendable_cache_entry()`**: a deep cache makes loss worse. Pending the user's go-ahead.
  (COMPARISON to-do 9)
- **KEEP_LAST cache default of 1 MiB**: it retains fewer than depth for max-size samples. The user's call.
  (PLAN, cache budgets)
- **Full segment ring: back-pressure the writer or drop?** This defines the module's promise. Awaits the user's
  decision 6. (SHM_PLAN 6c)
- **Rig interrupt coalescing** (macb rx/tx-usecs 49): part of the 168 us wire time. It is a rig setting.
  (RMW_PERF_PLAN 8)
- **Perf access on the rig or PC**: blocks Now item 3. (COMPARISON 2.2c)
- **Anything on 10BASE-T1S**: cross-host rmw over the real medium, and revisiting duplicate suppression where
  bandwidth binds. Waits on T1S hardware on the rig. (COMPARISON to-do 5; DATAFRAG_PLAN 16.1)

### The parked security plan, in short

Parked by the user on 2026-09-27 ("이건 내가 실행 하자고 할 때까지 실행하지 말자"); nothing is designed or coded. Only g7
went ahead: refuse to start under `ROS_SECURITY_ENFORCEMENT=Enforce`, and say once that security is not applied.

- **Idea:** MACsec (802.1AE) gives every link confidentiality and integrity. TickLE reads the same SROS2 keystore
  and enclave files and adds what MACsec lacks: per-process authentication on the enclave certificate, access control
  from the signed permissions at endpoint creation and matching, and optional signing or encryption for intra-host
  traffic and paths MACsec does not cover. SROS2 file-compatible, not DDS-Security wire-compatible.
- **Open questions:** (1) must authentication be mandatory whenever access control is on (Plan: yes); (2) crypto
  library and budget on FreeRTOS (mbedTLS?); (3) the wire change and its bytes under the wire rule; (4) what
  "enforced" means when MACsec cannot be verified from user space; (5) testing against a CycloneDDS SROS2 control
  and the rig's links with MACsec on.

## Done recently (2026-10-04/05)

- io_uring receive hint: a busy context learns of arrivals without an empty read. (`e19c349f`)
- FIFO segment doorbell: 1.42x the UDP doorbell's rate at p3, and publisher CPU per sample down 24%. (`e17b4e6f`)
- Same-host p2 latency: the segment now saves a third of a round trip. (`674f0dcb`)
- ACK solicitation clocked by the ACKNACK that answers it, plus solicit-by-data. (`d7e02846`, `8d1c3712`)
- Per-sleep doorbell fixes the lost wakeup: same-host RELIABLE p4 is 2.35x CycloneDDS. (`fe45f276`, `313cda37`)
- COMPARISON gets its own same-host table (1a), ours on every latency cell. (`c746572c`, `63f00280`)
- Cross-host check of the last two core changes: 20 better, 121 held, 9 within the layout floor. (`c29630ff`)
- QoS probe: rmw_zenoh cannot report deadline or liveliness events. (`7f127d56`)
- Layout control: p1's +0.2% is not function alignment. Recorded and left. (`7f127d56`)
- Mixed delivery: one publisher, one subscriber on its host and one across the link. (`3c0c505b`)
