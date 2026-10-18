# TickLE roadmap

One page for what is open, what comes next, what waits on the user, and what landed. The details live in the
documents named in parentheses; the pre-2026-10-05 ones are at `git show 3c0c505b:<path>` (DESIGN.md section 16).
Last consolidated 2026-10-18, against main `829a7d82`. The previous version, with every measurement narrative of the
items now under "Done", is `git show 829a7d82:docs/ROADMAP.md`.

## Now

What is left of the user's list of 2026-10-05 (items 0a-8). Every other item of that list is done; see "Done" below,
which keeps the numbers that commits and source comments cite.

0. **A fair testbed** (the user, 2026-10-05): the headline tables run unpinned since `a695e2d6`, pinned figures only
   as a labelled note; the rig audit (socket buffers, governor, IRQs, vendor profiles, build flags) found them even,
   and the latency clients, framework order and rmw_tickle's `-O2` were fixed (TESTING.md). KEEP_ALL is bounded alike
   for every framework that can be bounded (the user, 2026-10-06; rmw rows `29dff630`; CycloneDDS's rmw row stays
   default, labelled, as rmw_cyclonedds sets no resource limits). **Open:** (b) `s6_transport_cells.sh` still runs each
   framework's repetitions back to back (one loop per framework), not interleaved.
5a. **Testbed-independent constants** (the user, 2026-10-05): inventory done, the fitted values that stay are in
   RMW.md "Options you may tune"; retry granularity G is measured (`33b2cdfc`), discovery and dead-reader timing
   follow what they protect (`075a3476`), RPC retry bounds and the server cache are link-relative (`38d3e9ce`), the RX
   idle recheck is by time (`12036197`, `TT_RX_IDLE_RECHECK_NS`). **Open:**
   - Kept off main after their rig A/Bs (2026-10-06, `ab/hotpath-split`): the segment attach backoff by time (c10 RTT
     +2.7%, and its 64-bit clock does not link on RV32 FreeRTOS), the revalidate by time (needs that clock), and the
     timed receive check (c6 CPU and wire bytes worse twice). Each needs a redesign before another A/B.
   - A restarted client can get its predecessor's cached answer: crashed (no farewell), restarted under the same
     context id within the answer's lifetime, first request ahead of its new announce. Closing it needs a per-launch
     tag in the CallRequest, a wire change: the one spare byte is now `client_tag` (per Client, not per launch).

## Next

### QoS reshaped like rmw (agreed with the user 2026-10-08; supersedes the two 2026-10-05 items)

**This is the next piece of work.** The concrete plan (types, the profile, the API, what changes on the wire, how
rmw_tickle moves over) goes to the user for agreement before any code. It includes the writer's LIFESPAN applied at
the reader, as DDS does (the user, 2026-10-16). The shape below is what was decided on 2026-10-08.

Replaces "RESOURCE_LIMITS shaped like DDS" and "A core QoS API shaped like rmw's" (2026-10-05). Discussion material:
`~/claude_reports/QOS_DDS_SCOPE_2026-10-08.ko.md`. Decided:

- **rmw's model, not DDS's entity scopes.** QoS is per endpoint (publisher, subscriber, client, server), as
  `rmw_qos_profile_t`; context-wide settings are init options, as `rmw_init_options_t`. No Topic or group-level QoS.
- **rmw's defaults** (`rmw_qos_profile_default`: RELIABLE, KEEP_LAST 10, VOLATILE, ...), as `tt_QOS_PROFILE_DEFAULT`
  and the other rmw profile constants.
- **`tt_Topic` becomes `tt_TypeSupport`**: the type name and its encode/decode/size functions only (its three unread
  QoS fields go). The topic name is its own create argument, as in rmw: `create_publisher(ctx, pub, &type_support,
  topic_name, &qos, ...)`; `endpoint_id` stays hash(type name, topic name). Services likewise; the retry settings move
  off `tt_Service` (a type) onto the client and server.
- **QoS is given at creation, as in rmw; the field-by-field API goes.**
- **One flat `tt_QosProfile`**: the rmw fields first, same names and order (history, depth, reliability, durability,
  deadline, lifespan, liveliness, liveliness_lease_duration), then TickLE's extensions as plain fields under a one-line
  comment per rmw field they extend - no nested `_ext` structs. 0 in an extension field means "TickLE's computed
  default", so a profile filled from rmw alone works. Ambiguous names get a short prefix (e.g. `call_retry_*`).
  - reliability extensions: `max_blocking_ns` (today `RMW_TICKLE_MAX_BLOCKING_MS`), heartbeat period / piggyback,
    ack solicitation period and watermark, the reader's tracking window, `call_retry_interval_ns` / `call_retry_count`.
  - history extensions: `max_bytes` (the KEEP_ALL / KEEP_LAST byte budget; today `RMW_TICKLE_KEEP_ALL_BYTES` and the
    cache arena), `reorder_slots`. How KEEP_ALL's sample-count bound (the old RESOURCE_LIMITS item) sits beside
    `depth` and `max_bytes` is settled when the work starts.
- **Not QoS goes in options structs**, as rmw's publisher / subscription options: `batch`, `accept_callback`
  (closest: `content_filter_options`), local delivery (`ignore_local_publications`), the writable callback.
- **Context settings in `tt_InitOptions`**, as `rmw_init_options_t`: domain id, discovery options (range, static
  peers), later security; TickLE extensions: broadcast address, announce interval / lease, shared memory, io_uring -
  today compile-time constants and `_tt_CONFIG`.
- **Memory stays the caller's**: the reliable cache, reorder storage and service caches are passed in; a helper
  computes the bytes a profile needs (no malloc in core, FreeRTOS included).
- **Wire, API and struct changes are allowed** (a `tt_VERSION` bump past 13 if the announce must carry more for rmw's QoS
  compatibility check, `rmw_qos_profile_check_compatible`'s rules). The wire rule still applies: no test may get worse.
- Order: before Security; rmw_tickle then passes rmw's profile through nearly as is.

### Wired work (the user's order of 2026-09-29: finish wired, then Security, then wireless)

- **Zero-copy loans, the last core change**: a segment ring that skips held slots, so ring slots could be lent safely.
  The aligned `rx_buffer` and the claimed-slot publish are on main (`324f6ced`); slot loans stay off by default (the
  user, 2026-10-10). (RMW.md "Loaned messages")
- **Large samples, step B on the rig**: the same-host large path is on main since 2026-10-15; its rig session
  (`rig_queue_largemsg_B.sh`) went VOID on its control, perf_test's own spin (`bcf8624d`), so the same-host large
  cells are not published (RESULTS.md, large messages). Needs a control perf_test's spin cannot move.
- **Stage 1 / S1 against the WIRE 10.4 floors**: measured on the PC 2026-10-09/10 (`d131d82e`, two costs fixed in
  `81bf2f8b`); the CPU-time half is the rig's and has not run. Details at `git show 228da18d:docs/DESIGN.md`,
  section 10, "What the module costs where it is not used".
- **FreeRTOS form of the segment** (HAL-provided rather than `shm_open`): the segment only; discovery stays UDP.
- **rmw shared-memory kill test**: two ROS processes, the subscriber killed mid-stream. Core's is done
  (`test_segment_owner`). (RMW.md, open items)

### rmw latency and correctness

- **rmw kernel wake (~6 us) and poll-to-executor handoff (~21 us)**: the one segment where the vendors are faster.
- **C++ subscriber segfault, once in ~370 runs**: cause unknown and never reproduced.
- **Reorder buffer smaller than the window causes a re-request storm**: integrators who size the buffer small are
  exposed. Add a size check or stop re-requesting what cannot be held.

### Measurement debt (carried from the 2026-10-05 consolidation; not re-checked unless a state is given)

- **Re-run the c6 cell with the pre-registered `retransmitted` figure (~46x)**: not yet comparable.
- **The reliable_throughput cells marked ◊** (CPU0 IRQ coin flip): 11 marks remain in RESULTS.md. The 2026-10-05
  plan said `taskset -c 1-3`; headline figures are now unpinned, so the re-measure must be unpinned too.
- **COMPARISON's V rows after large-message stage 1**: A and T were re-measured at `09a0437d` (`e9d96fc6`).
- **Confirm DATA_FRAG's under-loss throughput (527-536 / 830-857 Mbps) in the full cross-vendor campaign.**
- **DATA_FRAG follow-ups**: arm C with a 30 s FastDDS drain cap, a raw-line re-run of the 4-fragment arm, the
  unexplained 0.57 duplicates per loss on the rig.
- **Discovery join time (M3)**: `discovery_join.sh` must be adapted first.
- **rmw_zenoh QoS rows 42-44 and 47**: still "–" in RESULTS.md; the QoS probe can measure them.
- **Harness residuals**: scenario 6's late-join loss varies between runs, scenario 9 pauses for different times per
  arm, the LIFESPAN pause is bimodal.
- **FastDDS liveliness server's unsynchronised globals**: a formal data race; fix it when the file is next touched.

### Throughput and CPU

- **Reuse the poll loop's clock reading** in a publish from a scheduled callback. Since `4de1f0d7` a publish reads the
  clock once (it was 4-5), so this saves at most that one read.
- **Poll-loop I/O interleave branch** (`experiment/poll-loop-io-interleave-v2`): ~15-25% more throughput, never merged.
- **recvmmsg rows processed in place, then scatter-gather for large fields**: copies remain on the receive path.
- **Decide what `rmw-perf.yml` and the two dashboard charts become**: recorded but never decided.

## Later - not to be started yet (the user, 2026-10-11)

- **Time-varying networks**: delay and loss both change over time; today p is treated as fixed and delay and its
  variation are ignored. Add test cases with varying loss and delay (netem schedules), and tune the estimator (e.g. a
  moving average / EWMA, windowed loss and RTT, jitter) so n and the RELIABLE timers track them. (DESIGN.md section 8,
  "Hybrid transport": its loss windows are a plain halving at 2^20 frames, `tt_HYBRID_WINDOW_FRAMES`, until then)

## Parked - only on the user's explicit go

- **SECURITY_PLAN: SROS2-compatible security with MACsec**: five open questions. Nothing is to be designed until
  the user starts it, and it comes before wireless. (SECURITY_PLAN; MODULE_PLAN 0/4; SHM_PLAN 7 q4)
- **WIRELESS_PLAN** (W1 correlated loss + delay, W2 Wi-Fi, W3 internet, relay, congestion control): queued behind
  Security. (WIRELESS_PLAN; PLAN "Priority")
- **zenoh-pico p2-p4 reliable cells stay not measurable**: zenoh-pico does not repair loss. Never quote the
  rate-limited figure. (ZENOH_PICO_PLAN 1; COMPARISON 4.4a)
- **O(1) `find_resendable_cache_entry()`**: a deep cache makes loss worse. Pending the user's go-ahead.
  (COMPARISON to-do 9)
- **KEEP_LAST cache default of 1 MiB**: it retains fewer than depth for max-size samples. The user's call.
  (PLAN, cache budgets)
- **Full segment ring: back-pressure the writer or drop?** This defines the module's promise. Awaits the user's
  decision 6. (SHM_PLAN 6c)
- **Wire changes until the wireless stage** (the user, 2026-10-15): run header merge, FEC and range NACK wait for
  the wireless stage.
- **Rig interrupt coalescing** (macb rx/tx-usecs 49): part of the 168 us wire time. It is a rig setting.
  (RMW_PERF_PLAN 8)
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

### rmw_tickle at REP-2004 Quality Level 2 (the user, 2026-10-05; after the security plan, the user, 2026-10-06)

Deferred until after the parked security plan (SECURITY_PLAN), including the switch to pull requests and the
disclosure policy.

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

## Done (2026-10-05 to 2026-10-18)

The user's list of 2026-10-05, by its numbers:

- **0a. Warm-up and cool-down for every framework** (the user, 2026-10-06): every bench takes the same window
  (`BenchWindow.h`); re-measured with it, same-host rows at `e521bbba`, cross-host campaigns A and T at `09a0437d`.
- **0. A fair testbed**, item (a): the rmw KEEP_ALL rows bounded alike (`29dff630`). Item (b) is open, above.
- **1. The p4 same-host latency gap**: first-touch page faults on the reorder ring. In-order fragments go together in
  `frag_scratch` (`bc398ed2`); rig A/B PASS cross host, first lap p4 - p3 gap +21 -> +3 us (2026-10-08).
- **2. Fill COMPARISON 1a's empty cells**: CPU and peak RSS for all three frameworks on the same-host cells (harness
  `8f57a512`, rows S11-S13 and S17-S19 at `e521bbba`, `8ba9528c`); BEST_EFFORT is KEEP_LAST 1 on all three; the
  throughput rows score delivered samples (goodput) since `75d82c42`.
- **3. Publisher user time**: encode into the segment slot (`cd09e895`; rig IMPROVED, p3 publisher CPU per sample
  -15.8%) and one clock read per publish (`4de1f0d7`). The poll-loop clock reuse is under "Throughput and CPU".
- **4. Reader wake cost**: the lost-wakeup fence (`7f9f7c62`), the epoll wait taken only where it measures cheaper
  (`8c1e6431`; rig IMPROVED 7/7, RTT p50 29 -> 23 us), a called-off sleep keeping its generation (`e6226bbf`, on PC
  evidence; no rig A/B of its own, it is in every rig figure since, e.g. as the A arm of the lending A/B).
- **5. rmw_tickle KEEP_ALL on the rig**: nine causes fixed, 12 of 12 runs lose nothing at 0% and 5% loss at
  `a8c1cd83` (RESULTS rows 72-75). The reader that hung after its writer left was perf_test's own spin lock under
  one-CPU pinning, not rmw (`06a12a3e`).
- **6. rmw same-host performance**: re-measured; at `06dd78d8` (after `718220d8`) 52 of 52 rows WIN (R1-R21).
- **7. FreeRTOS receive hint on lwIP netconn**: `b149dce7`, default `tt_HAL_FREERTOS_NETCONN=1`; checked per datagram
  with three HAL mutants killed (`e5eb4a61`).
- **8. CycloneDDS arm of the mixed shared-memory + network test**: `e4e090cd`; Cyclone's local subscriber took shared
  memory in every rep (RESULTS.md, mixed delivery).

Other work closed since:

- **Fast DDS under KEEP_ALL, from its side**: run 2026-10-06 (`6bfda159`, `fastdds_keepall_arms.sh`). Closed by the
  user the same day: with a normal configuration it does not run, and RESULTS says so; its cause is not pursued.
- **Large-message stage 2**: step A, the socket path (2026-10-10), and step B, the same-host region (2026-10-15), on
  main; UDP GSO/GRO on by default (`ab5c3c6a` and after); the ACK bitmap fast path (`fc97a914`).
- **Hybrid transport for multi-datagram samples** (the user, 2026-10-11): steps from `dc24c09c`, `tt_VERSION` 13; rig
  figures in RESULTS "Hybrid transport (large samples), rig".
- **UDP checksum 0 by default** (`f5fcda06`, the user, 2026-10-15); `TT_UDP_CSUM0=0` keeps every checksum.
- **No send without a matched reader** (`4fbfe459`, the user, 2026-10-15): BEST_EFFORT drops, RELIABLE keeps in its
  history. This also closes the discovery peer-registration race (a first DATA broadcast before its peer was known).
- **RELIABLE repair as DDS/RTPS** (`763b8d55`, `939dd57d`), the idle HEARTBEAT (`ec13c9de`, period 250 ms
  `ae12f1fd`), RELIABLE readers per publisher configurable (`8559084e`), several calls outstanding per Client
  (`1014a7ed`), deadlines on CLOCK_MONOTONIC (`3ed02b18`) - the user's decisions of 2026-10-16.
- **A service server keeps one answer per calling Client**, not per context (`06dd78d8`).
- **rmw same-host peak RSS**: the context is zero-allocated, not memset (`718220d8`).
- **Receive-buffer lending** (`0be14584`; rig IMPROVED 12 of 15) and **loaned messages** (`7a321d26`..`e521bbba`;
  slot loans `324f6ced`, off by default). Rig A/Bs in RMW.md "Loaned messages".
- **Introspection round trips across two processes** (`bf1378da`, `91b1988b`), **discovery per entity**
  (`33436ab3`), **the shm-only submessage refused on the socket** (`ac9394d5`), **the rmw behaviour tests in the local
  gates** (`f837ae82`).
- **S9 window A** (`029e9a85`, PC): the socket and segment paths reorder across two clients' streams, never within
  one; the timeouts found on the way were a client timing defect, fixed.
- **Same-host discovery stays UDP** on Linux and FreeRTOS (the user, 2026-10-09).
- **Scheduler waits end at their deadline**: a wait longer than 10 ms on the deadline `timerfd` (`f02f5e7b`,
  `abb01149`), shorter ones with the polling thread's timer slack at 1 ns (`119d14bd`). Latency is scored from
  creation time since 2026-10-17 (`037de4d1`; RESULTS.md).
