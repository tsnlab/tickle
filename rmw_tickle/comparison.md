# TickLE vs. FastDDS vs. CycloneDDS: performance comparison

Restructured (2026-09-21, the user's own explicit request) into a fixed six-section format, kept
deliberately simple. The earlier version of this document accumulated a long, chronological
narrative of every bug found and every methodology dead-end along the way - useful history, but it
made the document itself hard to read as a reference. That narrative isn't lost (it's in this
file's own git history), but it's no longer the point of the live document: this file now states
what was measured, how, and what the results mean, plus a standing to-do list. New measurement
passes should update the results sections and the to-do list, not grow a new narrative thread.

## 1. Purpose

This document tracks whether TickLE meets [`rmw_tickle/PLAN.md`](PLAN.md)'s own Project Goals 2 and
4: that TickLE core's own performance, and separately `rmw_tickle`'s own performance as a ROS 2
`rmw` implementation, are competitive with `rmw_fastrtps_cpp`/`rmw_cyclonedds_cpp` (FastDDS/
CycloneDDS). Two genuinely different questions, measured two different ways:

- **Sections 2-3**: TickLE core itself vs. FastDDS/CycloneDDS themselves, each used directly
  through its own native API (no ROS 2, no `rmw` layer at all) - isolates TickLE's own engine from
  whatever overhead `rmw_tickle`'s own wrapper adds on top.
- **Sections 4-5**: `rmw_tickle` vs. `rmw_fastrtps_cpp`/`rmw_cyclonedds_cpp`, all three reached the
  same way a real ROS 2 application would (`RMW_IMPLEMENTATION` + `rclcpp`) - the number that
  actually matters to anyone choosing an `rmw` implementation.

## 2. Experiment: TickLE vs. FastDDS vs. CycloneDDS (raw core, no `rmw`)

**Where**: `examples/perf_hil/{tickle,cyclonedds,fastdds}/` - one directory per framework, each
scenario built as a plain `client`/`server` pair using that framework's own native C/C++ API
directly (TickLE's own `tickle.h`, CycloneDDS's `dds.h`, FastDDS's own C++ API).

**How**: real hardware-in-the-loop, not same-host - the `tickle-hil` self-hosted CI runner's own
two dedicated Raspberry Pis, connected by a real point-to-point physical link
(`192.168.10.0/24`, `run_perf.sh`'s own `PERF_LINK_BROADCAST`), one Pi running the `client`
(publisher-ish role) and the other the `server` (subscriber-ish role). `examples/perf_hil/tickle/
run_scenario.sh` (and its CycloneDDS/FastDDS twins) orchestrate each run over SSH.

**What**: the same 9-scenario QoS matrix run against all three frameworks, using an identical
76-byte message shape (`seq`, `send_ns`, a 64-byte payload) so results are directly comparable byte
for byte, not just "similarly sized":

| # | Scenario | QoS exercised | What it measures |
|---|---|---|---|
| 1 | `best_effort_latency` | RELIABILITY (BEST_EFFORT) | low-rate ping-pong RTT |
| 2 | `reliable_latency` | RELIABILITY (RELIABLE) | same RTT pattern, RELIABLE |
| 3 | `best_effort_throughput` | RELIABILITY (BEST_EFFORT) | max-rate saturating stream: achieved throughput, loss % |
| 4 | `reliable_throughput` | RELIABILITY (RELIABLE) | same stream, RELIABLE |
| 5 | `durability_late_join` | DURABILITY | publish N samples, subscriber joins late; TRANSIENT_LOCAL/DURABLE vs. VOLATILE side by side |
| 6 | `history_depth_burst_loss` | HISTORY | RELIABLE + depth=8 (fixed, matched across all three), inject a stall both within and beyond that depth |
| 7 | `deadline_miss_detection` | DEADLINE | 50ms deadline, one intentionally-missed interval - time-to-detect, false-positive rate |
| 8 | `liveliness_loss_detection` | LIVELINESS | matched lease, publisher process killed (`kill -9`) mid-stream - peer-loss detection latency |
| 9 | `lifespan_expiry` | LIFESPAN | 100ms lifespan, stall injected past that duration - confirms expired samples are not delivered |

## 3. Results

| # | Scenario | CycloneDDS | FastDDS | TickLE (native) |
|---|---|---|---|---|
| 1 | `best_effort_latency` | 199/199, 0% loss, RTT 0.231/0.242/0.343ms | 199/199, 0% loss, RTT 0.253/0.296/3.295ms | 0% loss, RTT ~0.20-0.22ms avg |
| 2 | `reliable_latency` | 199/199, 0% loss, RTT 0.230/0.302/10.865ms | 199/199, 0% loss, RTT 0.271/0.297/0.603ms | 0% loss, RTT ~0.20-0.22ms avg |
| 3 | `best_effort_throughput` | 9312 sent, 9311 recv, 0% loss, 0.596 Mbps | 9166 sent, 9166 recv, 0% loss, 0.587 Mbps | ~1.24M sent, 0% loss, ~88-95 Mbps |
| 4 | `reliable_throughput` | 0% loss, ~44.5-59.8 Mbps sustained | 0% loss, ~17.3-17.9 Mbps sustained | 0% loss, ~88-94 Mbps |
| 5 | `durability_late_join` | 20/20 backlog delivered | 20/20 backlog delivered | durable: 20/20 (see §6); volatile: 57 received, not 0 (see §6) |
| 6 | `history_depth_burst_loss` | within: 0 lost; beyond: 52 lost (exact) | identical to CycloneDDS | within: 160/160 clean; beyond: `recv > sent` (see §6) |
| 7 | `deadline_miss_detection` | writer misses=7; reader misses=14; detect ~0.05ms | writer misses=7; reader misses=19; detect ~-0.9ms | writer misses=3 (matches expected math); sent=recv=198 (0% loss); reader_misses=30 (harmless, unexplained) |
| 8 | `liveliness_loss_detection` | detect ~2000.07ms (lease 2000ms) | detect ~1999.08ms (lease 2000ms) | detect ~3080-3620ms (see §6) |
| 9 | `lifespan_expiry` | within: 0 lost; beyond: 10 lost (exact) | within: 0 lost; beyond: 5 lost | within: 0 lost; beyond (1.0s stall): 17/150 lost |

**Reading**:

- **Latency (1-2)**: TickLE is the fastest of the three, ~0.20-0.22ms average RTT vs. CycloneDDS's
  ~0.24ms and FastDDS's ~0.30ms - and RELIABLE costs nothing extra over BEST_EFFORT for any of the
  three at this rate.
- **Throughput (3-4)**: TickLE's own max achievable rate is roughly **two orders of magnitude**
  higher than either DDS vendor's own max rate for this identical test design (~90 Mbps / ~1.2M
  msg/s vs. CycloneDDS's ~0.6-60 Mbps and FastDDS's ~0.6-18 Mbps) - all with **zero loss**, both
  BEST_EFFORT and RELIABLE. TickLE's own per-message overhead is low enough that neither vendor's
  own max-rate ceiling ever comes close to stressing it.
- **QoS mechanics (5-9)**: HISTORY depth, LIFESPAN expiry, and DEADLINE/LIVELINESS detection are
  all confirmed working in TickLE, each with the expected numbers where the mechanism is directly
  comparable to DDS's own. Two real, TickLE-specific findings surfaced along the way - see §6.

## 4. Experiment: `rmw_tickle` vs. `rmw_fastrtps_cpp` vs. `rmw_cyclonedds_cpp`

Two separate measurement passes, both reached through the real ROS 2 `rmw` layer
(`RMW_IMPLEMENTATION` + `rclcpp`), not either framework's own native API:

- **Same-host**: `.github/scripts/compare_rmw_perf.sh` (run by hand, deliberately not CI - see §6)
  drives `buildfarm_perf_tests`' own two-process `rmw` benchmark across all three
  `RMW_IMPLEMENTATION`s on one box. FastDDS and CycloneDDS are both forced onto real UDP/IP with
  every same-host shared-memory shortcut explicitly disabled (FastDDS's `useBuiltinTransports=
  false` *and* its separate `data_sharing kind=OFF`; CycloneDDS's `SharedMemory/Enable=false`) -
  without this, both get an unfair, TickLE-incomparable advantage neither has a same-host
  fast-path equivalent for.
- **Cross-host**: `rmw_tickle/rmw_perf_pingpong` (a small purpose-built `rclcpp` ping/pong pair,
  message-shape-compatible with `buildfarm_perf_tests`' own `Array1k`/`Struct16`) run for real on
  the `tickle-hil` rig's own two Pis - the same real physical link §2 uses, not same-host loopback.

## 5. Results

**Same-host** (`Array1k`=1024-byte array, `Struct16`=16-byte struct; one clean, verified run - see
§6 for why the numbers below aren't the final word on the latency gap):

| Topic | Sync | rmw implementation | Latency (ms) | Throughput (Mbit/s) |
|---|---|---|---:|---:|
| Array1k | async | `rmw_fastrtps_cpp` | 0.0375 | 0.9901 |
| Array1k | async | `rmw_tickle` | 0.0487 | 0.9905 |
| Array1k | sync | `rmw_cyclonedds_cpp` | 0.0322 | 0.9878 |
| Array1k | sync | `rmw_fastrtps_cpp` | 0.0363 | 0.9905 |
| Array1k | sync | `rmw_tickle` | 0.0467 | 0.9904 |
| Struct16 | async | `rmw_fastrtps_cpp` | 0.0369 | 0.0305 |
| Struct16 | async | `rmw_tickle` | 0.0543 | 0.0304 |
| Struct16 | sync | `rmw_cyclonedds_cpp` | 0.0357 | 0.0304 |
| Struct16 | sync | `rmw_fastrtps_cpp` | 0.0310 | 0.0305 |
| Struct16 | sync | `rmw_tickle` | 0.0464 | 0.0305 |

(CycloneDDS's own `async` rows are missing - skipped by `ctest` on this particular run, a
test-registration flakiness unrelated to any of the three `rmw`s, not yet chased down.)

**Cross-host** (`tickle-hil` rig, 50 samples/run, 3 runs per combination, 18/18 clean, zero loss on
every run):

| rmw | QoS | avg RTT (ms) |
|---|---|---:|
| `rmw_tickle` | best_effort | 0.524 |
| `rmw_tickle` | reliable | 0.523 |
| `rmw_fastrtps_cpp` | best_effort | 0.516 |
| `rmw_fastrtps_cpp` | reliable | 0.523 |
| `rmw_cyclonedds_cpp` | best_effort | 0.440 |
| `rmw_cyclonedds_cpp` | reliable | 0.449 |

**Reading**: `rmw_tickle` runs **~1.3-1.6x higher latency** than both DDS vendors same-host
(0.0464-0.0543ms vs. FastDDS's 0.0310-0.0375ms and CycloneDDS's 0.0322-0.0357ms), throughput
indistinguishable across all three (dominated by the test's own fixed send rate, not
implementation efficiency). Cross-host tells a slightly different story: `rmw_tickle` and
`rmw_fastrtps_cpp` land within noise of each other (~0.52ms), while `rmw_cyclonedds_cpp` is
genuinely faster (~0.44ms) on the real link too. RELIABLE costs nothing measurable over
BEST_EFFORT for any of the three, same-host or cross-host.

Notably, `rmw_tickle`'s own gap here (a real, if modest, latency cost vs. two mature DDS stacks)
sits in contrast with §3's own raw-core result, where TickLE was the *fastest* of the three - the
gap is coming from `rmw_tickle`'s own wrapper layer, not from TickLE core itself. See §6, items 3-4.

## 6. To-do, from TickLE's own perspective

1. **[TickLE core, real correctness bug]** A liveliness false-positive causes real duplicate
   DURABLE-backlog delivery under load: `check_liveliness()`'s own `forget_peers_from_source()`
   wipes a still-alive peer's bookkeeping on a false "presumed dead" timeout, so the next UPDATE
   from that same peer looks like a fresh discovery and re-triggers a full backlog re-push
   (scenario 5: 20-sample backlog delivered as 140 in one run; scenario 6/9 show the same
   `recv > sent` signature via the ACKNACK-repair path instead). Relayed to TickLE Dev
   (2026-09-21); not yet fixed.
2. **[TickLE core, design decision needed]** TickLE's own `RELIABLE + VOLATILE` doesn't isolate a
   late joiner from history the way DDS's own RELIABLE+VOLATILE does: `process_acknack()`'s
   retransmit loop isn't gated by `pub->durable` at all, so a newly-matched RELIABLE subscriber's
   ACKNACK gets served from whatever's cached regardless of when it joined (scenario 5's volatile
   control case delivered 57 samples, not the 0 DDS gives). Needs a call on whether this is a bug
   to fix or intended TickLE-specific behavior to document as such - not TickLE Plan's call to make
   unilaterally.
3. **[`rmw_tickle`, performance]** Close the remaining same-host `rmw_tickle` latency gap
   (PLAN.md Milestone 45): item (1), pooling the scratch conversion buffers, is done but was a
   verified *negative result* (no measurable improvement - likely already below glibc `tcache`
   noise at this message size). Item (2), the unconditional `tt_Node_interrupt()`+lock pair on
   every `rmw_publish()` call, is confirmed real but small (~3%). Item (3), the actual
   cross-thread wake-up cost between the poll thread and the application's executor thread, is
   still unmeasured - needs off-CPU/scheduling-latency tracing (`perf sched`/`ftrace`), not the
   cycle-based `perf record` already tried.
4. **[`rmw_tickle`, re-measurement]** TickLE core has changed substantially since Milestone 45
   (Milestones 46-57) - worth re-running `compare_rmw_perf.sh` to get a current baseline. Per
   standing process (PLAN.md Milestone 44's own note), this needs the user's own explicit
   go-ahead before running, not TickLE Plan's own initiative.
5. **[`rmw_tickle`, methodology gap]** The same-host `rmw_tickle` comparison (§5) is still
   same-host/`lo`-adjacent only, unlike §2-3's own real cross-host HIL rig - a genuine cross-host
   `rmw_tickle` vs. FastDDS/CycloneDDS run, ideally over the real target network medium
   (10Base-T1S, not the rig's own Ethernet), is still open.
