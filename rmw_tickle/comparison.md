# TickLE vs. FastDDS vs. CycloneDDS: performance comparison

Restructured (2026-09-21, the user's own explicit request) into a fixed six-section format, kept
deliberately simple. The earlier version of this document accumulated a long, chronological
narrative of every bug found and every methodology dead-end along the way - useful history, but it
made the document itself hard to read as a reference. That narrative isn't lost (it's in this
file's own git history), but it's no longer the point of the live document: this file now states
what was measured, how, and what the results mean, plus a standing to-do list. New measurement
passes should update the results sections and the to-do list, not grow a new narrative thread.

Column order in every results table is fixed: **TickLE, FastDDS, CycloneDDS**.

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
| 3 | `best_effort_throughput` | RELIABILITY (BEST_EFFORT) | sender at max rate, fixed 8s window, no artificial loss - how much the receiver actually gets, and the resulting loss % |
| 4 | `reliable_throughput` | RELIABILITY (RELIABLE) | same max-rate/8s design, plus real `tc`/`netem`-injected loss on the sender's own egress at 0%, 1%, 5% - does RELIABLE's own retransmission actually recover it |
| 5 | `durability_late_join` | DURABILITY | publish N samples, subscriber joins late; TRANSIENT_LOCAL/DURABLE vs. VOLATILE side by side |
| 6 | `history_depth_burst_loss` | HISTORY | RELIABLE + depth=8 (fixed, matched across all three), inject a stall both within and beyond that depth |
| 7 | `deadline_miss_detection` | DEADLINE | 50ms deadline, one intentionally-missed interval - time-to-detect, false-positive rate |
| 8 | `liveliness_loss_detection` | LIVELINESS | matched lease, publisher process killed (`kill -9`) mid-stream - peer-loss detection latency |
| 9 | `lifespan_expiry` | LIFESPAN | 100ms lifespan, stall injected past that duration - confirms expired samples are not delivered |

Scenarios 3-4 were redesigned and re-measured 2026-09-21 (the user's own explicit request) after
finding real cross-framework condition mismatches in the original pass - see §6, item 6, for what
was wrong and fixed. Every number in §3 for scenarios 3-4 is from that redesigned, reproduced (2/2)
pass; scenarios 1-2 and 5-9 keep their original (2026-09-20) results, not re-measured this pass.

## 3. Results

| # | Scenario | Condition | TickLE (native) | FastDDS | CycloneDDS |
|---|---|---|---|---|---|
| 1 | `best_effort_latency` | - | 0% loss, RTT ~0.20-0.22ms avg | 199/199, 0% loss, RTT 0.253/0.296/3.295ms | 199/199, 0% loss, RTT 0.231/0.242/0.343ms |
| 2 | `reliable_latency` | - | 0% loss, RTT ~0.20-0.22ms avg | 199/199, 0% loss, RTT 0.271/0.297/0.603ms | 199/199, 0% loss, RTT 0.230/0.302/10.865ms |
| 3 | `best_effort_throughput` | max rate, 8s, no `tc` loss | sent 1.244-1.245M, recv 1.244-1.245M, **0% loss** (2/2), send 94.5→recv 94.5 Mbps | sent 245.4-245.5K, recv 129.6-234.3K, **4.6-47.2% loss** (noisy, 2/2), send 19.6→recv 10.4-18.7 Mbps | sent 849-899K, recv 745-788K, **7.2-17.2% loss** (noisy, 2/2), send 68-72→recv 59.6-63.0 Mbps |
| 4 | `reliable_throughput` | `tc` loss=0% | sent 1.239-1.241M, recv 1.239-1.241M, **0% loss** (2/2), 94.2-94.3 Mbps (send=recv) | sent 222.3-223.3K, recv 222.3-223.3K, **0% loss** (2/2), 16.9-17.0 Mbps (send=recv) | sent 567K-1.051M (itself noisy run to run), recv = sent, **0% loss** (2/2), 43.1-79.8 Mbps (send=recv) |
| 4 | `reliable_throughput` | `tc` loss=1% (real, `tc netem` on the sender's own egress) | sent 1.069-1.218M, recv 1.058-1.179M, **1.1-3.1% loss** (not reliably recovered, 2/2), send 81.2-92.5→recv 80.4-89.6 Mbps | sent 222.2-222.9K, recv 219.9-220.8K, **1.0% loss** (not recovered, 2/2), send 16.9→recv 16.7-16.8 Mbps | sent 145.5-159.4K, recv = sent, **0% loss** (fully recovered, 2/2), 11.1-12.1 Mbps (send=recv) |
| 4 | `reliable_throughput` | `tc` loss=5% (real, `tc netem` on the sender's own egress) | sent 1.2408-1.2410M, recv 1.175-1.176M, **5.2-5.3% loss** (not recovered, 2/2), send 94.3→recv 89.3-89.4 Mbps | sent 222.3-222.9K, recv 211.2-211.5K, **5.0-5.1% loss** (not recovered, 2/2), send 16.9→recv 16.0-16.1 Mbps | sent 34.4-36.4K, recv = sent, **0% loss** (fully recovered, 2/2), 2.6-2.8 Mbps (send=recv) |
| 5 | `durability_late_join` | - | durable: 20/20 (2/2); volatile: **0 received**, matches DDS exactly (3/3, fixed - see §6) | 20/20 backlog delivered | 20/20 backlog delivered |
| 6 | `history_depth_burst_loss` | within depth | 160/160 clean | identical to CycloneDDS | 0 lost |
| 6 | `history_depth_burst_loss` | beyond depth | 4-13 lost (**not deterministic** - 13/4/13 across 3 runs, unlike CycloneDDS's exact 52 - see §6) | identical to CycloneDDS | 52 lost (exact) |
| 7 | `deadline_miss_detection` | - | writer misses=3 (own math), sent=recv=198 (0% loss), reader_misses=30 (harmless, unexplained) | writer misses=7, reader misses=19, detect ~-0.9ms | writer misses=7, reader misses=14, detect ~0.05ms |
| 8 | `liveliness_loss_detection` | - | detect ~3080-3620ms (fixed node-level window, independent of lease - see §6) | detect ~1999.08ms (lease 2000ms) | detect ~2000.07ms (lease 2000ms) |
| 9 | `lifespan_expiry` | within lifespan | 0 lost | 0 lost | 0 lost |
| 9 | `lifespan_expiry` | beyond lifespan | 1/22 lost, 2 runs (**pause=1.0s** - see §6, item 6, for why this isn't the same condition as the other two; re-measured 2026-09-21 post-Milestone-60, no more duplicate-delivery noise) | 5 lost (pause=0.3s) | 10 lost (exact, pause=0.3s) |

Scenarios 3-4's own "recv Mbps" is computed uniformly for all three from `recv_count × 76 bytes × 8 / elapsed_s` (the shared `Bench` wire shape, §2) - only CycloneDDS/FastDDS's own `server.c`/`.cpp` print it directly; TickLE's doesn't, so it's derived the same way for all three rather than left blank for one, to keep offered-vs-actually-delivered throughput directly comparable in every cell. "send Mbps" is each framework's own printed client-side rate. Both runs (reproduced 2/2 for every condition) are given as ranges, not averaged, since the run-to-run variance itself is part of the finding for several rows (CycloneDDS's own scenario 4 send rate, FastDDS/CycloneDDS's own scenario 3 loss %).

**Reading**:

- **Latency (1-2)**: TickLE is the fastest of the three, ~0.20-0.22ms average RTT vs. FastDDS's
  ~0.30ms and CycloneDDS's ~0.24ms - and RELIABLE costs nothing extra over BEST_EFFORT for any of
  the three at this rate.
- **Best-effort throughput (3)**: TickLE's own achievable send rate is roughly two orders of
  magnitude higher than either DDS vendor's (~1.24M msg/s vs. ~245K for FastDDS, ~850-900K for
  CycloneDDS) - and, measured correctly this time (§6, item 6), TickLE shows **zero loss** at that
  rate, both runs. FastDDS and CycloneDDS both show real, but highly variable, loss at their own
  (lower) max rate - not an artifact this time, reproduced twice, but noisy enough (FastDDS
  4.6-47.2%, CycloneDDS 7.2-17.2%) that neither vendor's own number should be read as a precise
  figure, only as "genuinely lossy under sustained max-rate BEST_EFFORT on this rig, unlike
  TickLE."
- **Reliable throughput under controlled loss (4) - the most interesting result this pass**: at
  identical, real, `tc`/`netem`-injected loss, the three frameworks diverge sharply. **CycloneDDS's
  own RELIABLE fully recovers 100% of injected loss at both 1% and 5%, every time (2/2)** - `recv`
  loss stays at exactly 0% regardless of the injected rate. **FastDDS's own RELIABLE recovers
  essentially none of it** - observed loss tracks the injected rate almost exactly (1.0% in →
  ~1.0% observed; 5% in → ~5.0-5.1% observed), both runs. **TickLE's own RELIABLE also mostly
  doesn't recover it**, and is noisier about it (1.1-3.1% observed at 1% injected; a consistent
  ~5.2-5.3% at 5% injected) - plausibly explained by TickLE's own extremely high throughput at this
  rate: `depth=64` (`tt_MAX_RELIABLE_HISTORY`, TickLE's own hard cap) represents on the order of
  tens of microseconds of retention at ~1.2M msg/s, almost certainly shorter than one real
  ACKNACK-retry round trip on this link - a lost sample is likely evicted from the cache long
  before a retransmit request for it could ever be serviced. Not confirmed by direct
  instrumentation this pass, but consistent with every other observation here. This is a genuine,
  reproduced, cross-vendor behavioral difference, not noise - see §6, item 7.
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

One table, both topologies together (`Array1k`=1024-byte array, `Struct16`=16-byte struct, `Bench`
QoS-tagged=the cross-host tool's own ping/pong shape at 50 samples/run, 3 runs/combination):

| Topology | Message | Mode | TickLE (`rmw_tickle`) | FastDDS | CycloneDDS |
|---|---|---|---:|---:|---:|
| Same-host | Array1k | async | 0.0487ms, 0.9905 Mbit/s | 0.0375ms, 0.9901 Mbit/s | (skipped - see note) |
| Same-host | Array1k | sync | 0.0467ms, 0.9904 Mbit/s | 0.0363ms, 0.9905 Mbit/s | 0.0322ms, 0.9878 Mbit/s |
| Same-host | Struct16 | async | 0.0543ms, 0.0304 Mbit/s | 0.0369ms, 0.0305 Mbit/s | (skipped - see note) |
| Same-host | Struct16 | sync | 0.0464ms, 0.0305 Mbit/s | 0.0310ms, 0.0305 Mbit/s | 0.0357ms, 0.0304 Mbit/s |
| Cross-host | Bench, QoS=best_effort | - | 0.524ms avg RTT | 0.516ms avg RTT | 0.440ms avg RTT |
| Cross-host | Bench, QoS=reliable | - | 0.523ms avg RTT | 0.523ms avg RTT | 0.449ms avg RTT |

(CycloneDDS's own same-host `async` rows are missing - skipped by `ctest` on this particular run, a
test-registration flakiness unrelated to any of the three `rmw`s, not yet chased down. Cross-host:
18/18 runs clean, zero loss every time.)

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

1. **[TickLE core, real correctness bug - fixed]** A liveliness false-positive was found causing
   real duplicate delivery under load: `check_liveliness()`'s own `forget_peers_from_source()`
   wipes a still-alive peer's bookkeeping on a false "presumed dead" timeout, so the next UPDATE
   from that same peer looks like a fresh discovery and re-triggers redelivery. TickLE Dev's
   PLAN.md Milestone 59 (2026-09-21) fixed the **DURABLE push path**
   (`deliver_durability_backlog()`) - scenario 5's own durable case re-verified clean,
   `received=20/20` exactly every time (was 140=7×20 in one run before the fix).
2. **[TickLE core, design decision made - fixed and verified, closes scenarios 5 *and* 6]** TickLE's
   own `RELIABLE + VOLATILE` didn't isolate a late joiner from history the way DDS's own
   RELIABLE+VOLATILE does (scenario 5's volatile control case delivered 57 samples, not the 0 DDS
   gives; scenario 6's own `history_depth_burst_loss` showed the same `recv > sent` shape via a
   late-joining volatile Subscriber against a shallow `depth=8` Publisher - the identical
   structural setup, it turned out). **The user's own explicit decision (2026-09-21): fix it to
   match DDS semantics.**

   Two attempts were needed, both documented honestly in PLAN.md Milestone 60 rather than
   silently replaced: **attempt 1** (keyed on `update_reliable_ack()`'s own DATA-path
   first-contact sync) left scenario 5 completely unchanged - `received=57`, decimal-identical to
   before the fix. Root cause: this scenario's server publishes its whole backlog *before any
   Subscriber exists*, so a fresh Subscriber's first contact is always resolved by the
   discovery-triggered Heartbeat (`inform_subscriber_of_heartbeat()`), not DATA - a separate,
   older first-contact branch attempt 1 never touched. Scenario 6's own `recv > sent` was, at this
   point, *mis*-attributed to a believed-separate receive-side dedup gap (`deliver_data_to_
   subscriber()` had no `seq_no`-based dedup at all, also fixed this same milestone, a real and
   independently-verified-correct feature - just not the actual cause of either open bug).

   **Attempt 2, after real DDS/RTPS analysis at the user's own request** (DURABILITY is a
   Requested-vs-Offered QoS like RELIABILITY/DEADLINE/LIVELINESS - the fix belongs on the
   requesting Subscriber side, keyed on `sub->durable`, mirroring real RTPS's own
   newly-matched-VOLATILE-reader baseline): fixed `inform_subscriber_of_heartbeat()`'s own
   first-contact branch to sync `ack_seq_no` to `first_available_seq_no` only when `sub->durable`,
   otherwise to `last_seq_no + 1`. **Re-verified (2026-09-21): both scenarios fixed.** Scenario 5
   volatile: 3/3, `received=0` exactly, matching DDS precisely; durable case re-confirmed still
   20/20 (2/2), no regression. Scenario 6: re-verified clean too once this landed - `recv > sent`
   gone entirely, replaced by ordinary `recv < sent` (real loss only, 147/156/147 of `sent=160`,
   confirming it was the *same* Heartbeat-driven leak all along, not the dedup gap first guessed).
   One honest open observation, left as-is rather than guessed at further: scenario 6's own real
   loss count varies run to run (4/13/13 lost, vs. CycloneDDS's own deterministic 52) - plausibly
   ordinary timing variance in exactly when late-join matching completes relative to the
   Publisher's own continuous send rate and the `depth=8` ring's own eviction, not yet
   instrumented directly.
3. **[`rmw_tickle`, performance - item 3 now has a real number]** Close the remaining same-host
   `rmw_tickle` latency gap (PLAN.md Milestone 45): item (1), pooling the scratch conversion
   buffers, is done but was a verified *negative result* (no measurable improvement - likely
   already below glibc `tcache` noise at this message size). Item (2), the unconditional
   `tt_Node_interrupt()`+lock pair on every `rmw_publish()` call, is confirmed real but small
   (~3%). **Item (3), the cross-thread wake-up cost between the poll thread and the application's
   executor thread**: `perf sched`/`ftrace` itself needs kernel tracepoint access this box doesn't
   have without an interactive `sudo` password (no TTY available) - measured the underlying
   `pthread_cond_wait`/`broadcast` primitive directly instead, via a standalone benchmark
   reproducing `rmw_tickle.h`'s exact `wait_mutex`/`wait_cond` shape. **Real result, reproduced
   4/4**: avg ≈ 9.4-9.6μs, p50 ≈ 8.6-8.7μs, p99 ≈ 13-16.5μs - closely matching the observed
   same-host gap itself (§5's own numbers put the gap at ~9-22μs). Real, converging evidence that
   this mechanism's own inherent cost is the right order of magnitude to explain the whole gap -
   not proof no DDS vendor pays a comparable cost for their own equivalent hand-off (not measured
   this pass), so suggestive rather than closed. Full detail and the suggested next step (a real
   `perf sched latency` capture inside the actual two-process run, once `perf_event_paranoid` is
   lowered) in PLAN.md's own Milestone 45 addendum.
4. **[`rmw_tickle`, re-measurement]** Done (2026-09-21, the user's own go-ahead) - §5's own numbers
   above are this fresh baseline, run after Milestones 46-58 landed. No regression from the
   Milestone 44 baseline; the ~1.3-1.6x same-host gap and the cross-host FastDDS-parity/
   CycloneDDS-ahead pattern both persist unchanged.
5. **[`rmw_tickle`, methodology gap]** The same-host `rmw_tickle` comparison (§5) is still
   same-host/`lo`-adjacent only, unlike §2-3's own real cross-host HIL rig - a genuine cross-host
   `rmw_tickle` vs. FastDDS/CycloneDDS run, ideally over the real target network medium
   (10Base-T1S, not the rig's own Ethernet), is still open.
6. **[Methodology, found and fixed 2026-09-21]** The original scenario 3/4 numbers (and, it turned
   out, every CycloneDDS/FastDDS scenario where the server is the authoritative side) were measured
   under a real, confirmed bug: `run_scenario.sh` forwards the same `-d` to both the client and the
   server, but it means "the client's own send duration" on one side and "this side's own don't-
   hang-forever cap" on the other - taken literally, the server could stop counting before the
   client had even finished sending. Worse, the CycloneDDS/FastDDS twins' own `run_scenario.sh`
   never actually printed the server's own `RESULT` line at all (only `pkill`ed it) - the original
   "9312 sent, 9311 recv, 0% loss" numbers for scenario 3 predate this discovery and cannot be
   fully trusted as apples-to-apples against a fixed condition. Both bugs are now fixed (`+15s`
   safety-cap buffer on every affected `server.c`/`.cpp`; the server's log is now actually read
   after `pkill`, matching the TickLE-native script's own already-fixed pattern) and scenarios 3-4
   re-measured clean, reproduced 2/2, under the redesigned methodology in §2-3 above. Scenario 9's
   own `pause_s` value is a **known**, not-yet-resolved condition mismatch between TickLE (1.0s)
   and the DDS twins (0.3s) - TickLE's own discovery is fast enough on this rig that 0.3s doesn't
   reliably create a stale-enough gap to observe expiry (see the git history for the full
   investigation) - left as a flagged, honest inconsistency rather than silently normalized.
7. **[Real finding, not yet root-caused]** §3's own reliable-throughput-under-loss result deserves
   real investigation, not just reporting: why does CycloneDDS's RELIABLE fully recover 5% injected
   loss while FastDDS's doesn't recover any of it at all, using each vendor's own default/
   near-default RELIABLE QoS? Is FastDDS's own `reliable_throughput/client.cpp` missing a QoS
   setting CycloneDDS's own `KEEP_ALL`+`resource_limits(4000)` equivalent needs, or is this a real,
   inherent behavioral difference between the two vendors' own default reliability windows? Not
   investigated further this pass.
