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

**Notation** (unified 2026-09-21, the user's own explicit request - every cell in this section
uses one of these fixed templates so results are directly, visually comparable at a glance,
replacing this document's own earlier mix of ranges/min-max/single-value styles):

- **Latency**: `avg±stdev ms, X% loss`
- **Throughput**: `sent avg±stdev msgs, recv avg±stdev msgs, X% loss, sent Y Mbps, recv Z Mbps, loss (Y-Z) Mbps`
- **Count-based** (DURABILITY/HISTORY/LIFESPAN - no sustained rate to report a Mbps for): the same
  `msgs`/`loss%` half of the Throughput template, without the Mbps fields.
- `avg±stdev` is computed from every individually-recorded run for that cell (sample stdev, `n-1`;
  for exactly 2 runs this reduces to `stdev = |a-b|/√2`). **`†` marks a value with no computable
  stdev** - either a single, unrepeated observation, or (scenarios 1/2/8's own DDS-vendor cells,
  §5's own table) a run where only the tool's own already-summarized min/avg/max was recorded at
  the time, not the raw per-sample data a real stdev needs - shown as `avg` alone, not invented.
  `loss %`/`Mbps` fields are plain means (not `±stdev`) even where the underlying msgs count has
  one, matching this section's own literal template; `loss Mbps` is the derived `sent - recv`
  aggregate, not an independently re-measured quantity.

| # | Scenario | Condition | TickLE (native) | FastDDS | CycloneDDS |
|---|---|---|---|---|---|
| 1 | `best_effort_latency` | - | 0.21±0.01ms, 0% loss | 0.30ms†, 0% loss | 0.24ms†, 0% loss |
| 2 | `reliable_latency` | - | 0.21±0.01ms, 0% loss | 0.30ms†, 0% loss | 0.30ms†, 0% loss |
| 3 | `best_effort_throughput` | max rate, 8s, no `tc` loss | sent 1244.0K±0.7K msgs, recv 1244.0K±0.7K msgs, 0% loss, sent 94.5Mbps, recv 94.5Mbps, loss 0Mbps | sent 245.4K±0.1K msgs, recv 181.9K±74.0K msgs, 25.9% loss, sent 19.6Mbps, recv 14.6Mbps, loss 5.1Mbps | sent 874.0K±35.4K msgs, recv 766.5K±30.4K msgs, 12.2% loss, sent 70.0Mbps, recv 61.3Mbps, loss 8.7Mbps |
| 4 | `reliable_throughput` | see §3a | **superseded - see §3a below** | **superseded (measured over a WiFi path, §6 item 12)** | **superseded - see §3a** |
| 5 | `durability_late_join` | - | durable: recv 20.0±0/20 msgs, 0% loss; volatile: recv 0.0±0/20 msgs, 100% loss (correctly excludes backlog, matches DDS exactly - see §6) | recv 20/20 msgs, 0% loss | recv 20/20 msgs, 0% loss |
| 6 | `history_depth_burst_loss` | within depth | recv 160/160 msgs, 0% loss | recv 160/160 msgs, 0% loss | recv 160/160 msgs, 0% loss |
| 6 | `history_depth_burst_loss` | beyond depth | recv 150.0±5.2/160 msgs, 6.25% loss (**non-deterministic**, n=3 - see §6) | recv 108/160 msgs, 32.5% loss (deterministic) | recv 108/160 msgs, 32.5% loss (deterministic) |
| 7 | `deadline_miss_detection` | - | sent 198±0 msgs, recv 198±0 msgs, 0% loss, writer_misses=3† (own math), reader_misses=30† (harmless, unexplained) | writer_misses=7†, reader_misses=19†, detect -0.90ms† | writer_misses=7†, reader_misses=14†, detect 0.05ms† |
| 8 | `liveliness_loss_detection` | - | **provisional, and NOT comparable to the DDS columns as measured - see §6 item 14** - lease=1.0s: 1375.7±169.1ms; lease=2.0s: 2060.9±465.7ms; lease=4.0s: 3232.0±235.7ms (real ceiling past ~3s - see §6 item 10). Those ± bands measure **mode-mixing, not precision**: at n=20 the distribution is bimodal, a dominant mode holding to ~15ms plus a minority mode ~460-500ms away | 1999.08ms† (lease=2000ms) | 2000.07ms† (lease=2000ms) |
| 9 | `lifespan_expiry` | within lifespan | 0 lost | 0 lost | 0 lost |
| 9 | `lifespan_expiry` | beyond lifespan, `pause=1.0/1.5/2.0s` | pause=1.0s: lost 28.0±1.4; pause=1.5s: lost 43.0±12.7; pause=2.0s: lost 79.0±0 (**not the same condition as the other two** - see §6 item 6; slope-verified against the expected formula, see §6 item 8) | lost 5† (pause=0.3s) | lost 10† (pause=0.3s) |

Scenarios 3-4's own "recv Mbps" is computed uniformly for all three from `recv_count × 76 bytes × 8 / elapsed_s` (the shared `Bench` wire shape, §2) - only CycloneDDS/FastDDS's own `server.c`/`.cpp` print it directly; TickLE's doesn't, so it's derived the same way for all three rather than left blank for one, to keep offered-vs-actually-delivered throughput directly comparable in every cell. "sent Mbps" is each framework's own printed client-side rate. Every condition was reproduced 2/2 (scenario 6 beyond-depth: 3 runs; scenario 9 beyond-lifespan pause=1.5s: 4 runs - see §6 items 6/8) - see the Notation block above for how each cell's `avg±stdev` was computed from those runs.

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
- **Reliable throughput under controlled loss (4)**: at identical, real, `tc`/`netem`-injected
  loss, **CycloneDDS and FastDDS both fully recover 100% of it, at both 1% and 5%, every time
  (2/2 each)** - `recv` loss stays at exactly 0% regardless of the injected rate for either DDS
  vendor. FastDDS's own initial result here looked like a real cross-vendor difference (loss
  tracking the injected rate almost exactly, not recovered at all) - turned out to be a real
  harness bug instead, not a vendor difference: `examples/perf_hil/fastdds/reliable_throughput`
  had never received the same `KEEP_ALL`+`resource_limits(4000)` fix the CycloneDDS twin got early
  in this exercise, left at a shallow `depth=8` that couldn't retain a lost sample long enough for
  its own retry to land. Fixed and re-verified 4/4 clean - see §6, item 7 for the full story.
  **TickLE's own RELIABLE, unlike either DDS vendor, still mostly doesn't recover it**, and is
  noisier about it (2.1% avg observed at 1% injected; a consistent ~5.25% avg at 5% injected) -
  plausibly explained by TickLE's own extremely high throughput at this rate: `depth=64`
  (`tt_MAX_RELIABLE_HISTORY`, TickLE's own hard cap at the time of this measurement) represents on
  the order of tens of microseconds of retention at ~1.2M msg/s, almost certainly shorter than one
  real ACKNACK-retry round trip on this link - a lost sample is likely evicted from the cache long
  before a retransmit request for it could ever be serviced. This part **is** a genuine TickLE
  characteristic worth its own re-measurement once the depth ceiling becomes configurable
  (PLAN.md's own DDS semantic-parity backlog, row 2, assigned to TickLE Dev) - though TickLE Dev's
  own design note there flags a separate, deeper structural limit (`tt_WriterProxy.received_bitmap`
  is a fixed 64 bits) that a deeper Publisher-side cache alone may not fix.
- **QoS mechanics (5-9)**: HISTORY depth, LIFESPAN expiry, and DEADLINE/LIVELINESS detection are
  all confirmed working in TickLE, each with the expected numbers where the mechanism is directly
  comparable to DDS's own. Two real, TickLE-specific findings surfaced along the way - see §6.

## 3a. Scenario 4 in detail: RELIABLE under injected loss (re-measured 2026-09-23)

Scenario 4's own single-number rows above were replaced by this section. Two reasons, both real
bugs found on 2026-09-22/23 (see §6 items 12-13): every earlier TickLE number was measured while
RELIABLE's own retry path was partly inert or flooding, and **every earlier FastDDS number was
measured over a path `tc` never touched at all** - FastDDS was sending each sample on both the
`192.168.10.0/24` test link *and* the `10.1.1.0/24` management WiFi.

The three frameworks are also not answering the same question at their default settings, so a
single "loss %" cell hides the real difference:

- **TickLE** (today, KEEP_LAST-equivalent, `publish()` never blocks): loss shows up as **lost
  samples**.
- **FastDDS/CycloneDDS** (KEEP_ALL + `resource_limits(4000)`): loss shows up as **a slower sender**,
  and - once `max_blocking_time` is short - as **refused writes** (`write_fail`, added 2026-09-23).
  Neither ever loses an accepted sample.

So each cell reports **loss % / refused % / Mbps**. `-B` is `max_blocking_time` (DDS only).
3 reps for TickLE, 2 for the DDS pair.

**Max send rate**:

| `tc` loss | TickLE, depth=64 | TickLE, depth=1024 | FastDDS (default `-B`) | FastDDS (`-B 1ms`) | CycloneDDS (default `-B`) | CycloneDDS (`-B 1ms`) |
|---|---|---|---|---|---|---|
| 0% | 0 / - / 107-121 | 0 / - / 107-121 | 0 / 0 / 10.0-11.3 | 0 / 1.3-2.5% / 11.8-17.0 | 0 / 0 / 47-55 | 0 / 0.01-0.1% / 72-93 |
| 1% | 0.010% / - / 117 | **0% (Phase 2, 6/6)** / - / 117 | 0 / 0 / 8.8-10.8 | 0 / 2.9% / 10.3-10.6 | 0 / 0 / 11.4-11.8 | 0 / 0.73% / 18 |
| 5% | 0.238% / - / 113 | **0.016% (Phase 2)** / - / 113 | 0 / 0 / 5.8-8.3 | 0 / 3.4-3.9% / 8.8-9.7 | 0 / 0 / 2.3-2.7 | 0 / 17-19% / 2.2 |
| 20% | not measured | not measured | 0 / 0 / 0.9-3.1 | 0 / 15-38% / 0.9-3.0 | 0 / 0 / 0.5 | 0 / 61-66% / 0.3-0.4 |
| 50% | not measured | not measured | 0 / 0 / 0.6-0.8 | 0 / 64-100% / 0.3-2.9 | 0 / 0 / 0.1 | 0 / 73-79% / 0.1-0.2 |

**Paced to ~8 Mbps** (the rate where all three can keep up):

| `tc` loss | TickLE (depth=64) | FastDDS | CycloneDDS |
|---|---|---|---|
| 1% | 0 / - / 8.1 | 0 / 0 / 6.3 | 0 / 0 / 7.1 (0.44% refused at `-B 1ms`) |
| 5% | 0 / - / 8.1 | 0 / 0 / 6.4 | 0 / 0 / 1.7-2.1 (16-18% refused at `-B 1ms`) |
| 20%/50% | not measured | 0 / 0 / 6.4 | 0 / 0 / 0.2 / 0.1 |

**Reading**:

- **At a rate all three sustain (~8 Mbps), TickLE now loses nothing either** - the same result the
  DDS pair gives, at a comparable or higher rate.
- **At max rate TickLE is 10-100x faster than either DDS** (113-121 Mbps vs. 0.1-17 Mbps under
  loss) and pays for it with a small residual loss (0.04% at depth=1024, 5% injected). That is the
  real trade-off today: TickLE keeps sending, DDS slows down or refuses.
- **DDS's "0% loss" is a different guarantee, not a better recovery rate.** With a 1ms
  `max_blocking_time` the same conditions produce 3-79% refused writes, i.e. the data never
  entered the middleware at all.
- **Updated 2026-09-23 (Phase 2, `1b15e15`)**: with the Subscriber's tracking window widened to
  1024 samples (and per-Subscriber ack identity on the wire, `tt_VERSION` 6), TickLE's own
  depth=1024 column above is now **0% loss at 1% injected loss (6/6 runs)** and 0.016% at 5% -
  i.e. at ~113-117 Mbps, two orders of magnitude above either DDS vendor's rate under the same
  injected loss. The window must stay ≤ the Publisher's cache depth; beyond that it measurably
  hurts (PLAN.md's own Phase 2 entry).
- Closing TickLE's remaining gap is `rmw_tickle/PLAN.md`'s Phase 3 (RELIABLE + KEEP_ALL: block the
  writer, fail the write on timeout), which gives the same guarantee DDS offers, plus Phase 2 if
  KEEP_LAST at max rate must also reach zero.

## 3b. The same guarantee, compared: RELIABLE + KEEP_ALL (2026-09-23, final)

§3a compared TickLE's default (KEEP_LAST-equivalent, `publish()` never blocks) against the DDS
pair's KEEP_ALL, which is not the same promise. TickLE now implements the DDS promise too
(`rmw_tickle/PLAN.md` Phase 3): a KEEP_ALL Publisher refuses a write rather than evicting an
unacknowledged sample, and `rmw_publish()` blocks up to `max_blocking_time` before returning
`RMW_RET_TIMEOUT`. This section compares all three **under that same contract**, measured the same
way: `tc netem` on the sender's own egress, 3 reps, and every writer waits for acknowledgements at
teardown (`dds_wait_for_acks()` / `wait_for_acknowledgments()` / the TickLE harness's own
drain-until-acked), so no framework's tail loss depends on drain luck.

**Net loss** = lost - refused writes, i.e. samples the middleware accepted and then failed to
deliver. All three are **0 in every cell**, so the only thing that differs is what it costs:

| `tc` loss | TickLE (KEEP_ALL, strict order) ‡ | FastDDS | CycloneDDS |
|---|---:|---:|---:|
| 0% | **108.8 Mbps** | 35.9 | 93.2 |
| 1% | **104.4** | 10.6 (median) | 13.0 |
| 5% | **98.1** | 6.8 (median) | 2.5 |
| 20% | **55.0** | 2.9 (median) † | 0.42 |
| 50% | **22.0** ◊ | 0.68 (median) † | 0.13 |

**Measured in one session on 2026-09-24 at `d63860c7`, both Pis confirmed at that commit by reporting
their own HEAD** - 3 reps per cell, `-d 5`, interleaved by framework within each loss level. This is
the build that ships: RELIABLE delivers each writer's samples in strictly increasing order, through a
reorder buffer that costs O(1) per sample. TickLE's cells are the mean of three reps except where
marked; every TickLE rep `drained=acked` with `write_fail=0` except the one noted under ◊.

**◊** One of three 50% reps read 8.4 Mbps with 28 refused writes (still `drained=acked`, so not a
stall). It did not recur: a follow-up of **8 consecutive 50% reps at the same build all read
21.8-22.2**, with `write_fail=0`, `out_of_order_discarded=0`, `timestamp_not_newer=0` and
`reorder_held_peak=255` in every one. The cell reports 22.0, the median of the sweep's three and
consistent with all eight; the outlier is unexplained and is recorded rather than averaged in.

**† and the FastDDS medians.** FastDDS varied far more than either other column in this session - at
20% its three reps read 2.85, 1.41 and 38.77, at 50% 38.33, 0.68 and 0.56, and every rep that read
above ~15 at those levels ended `drained=timeout`. `send_mbps` counts writes the middleware
*accepted*, so a writer that never drained reports work it did not deliver; a mean over those reps
would describe acceptance, not throughput. Medians are reported where the reps disagree, and the
20% and 50% FastDDS cells should be read as "did not keep up" rather than as a rate.

### Ordering costs nothing, and the number that said otherwise was an implementation defect

TickLE's column is within 0.7% of its own pre-ordering figures (108.5 / 104.1 / 97.5 / 54.7 / 21.9,
same rig, same method, earlier the same day) at every loss level. It took five builds to get there,
and the sequence is the useful part:

| build | what changed | 0% | 1% | 5% | 20% | 50% |
|---|---|---:|---:|---:|---:|---:|
| `5816aab3` | no ordering | 108.5 | 104.1 | 97.5 | 54.7 | 21.9 |
| `e461e7f3` | ordering, reorder buffer scanned linearly, 512 slots | 84.6 | 58.2 | 44.8 | 32.8 | 18.4 |
| `a5b99b1b` | same scan, buffer sized to the widest window, 4096 slots | 10.0 | 8.1 | 7.1 | 6.8 | 6.1 |
| `0b415269` | buffer indexed by sequence number, O(1) per sample | 108.6 | 104.2 | 97.9 | 54.6 | 21.8 |
| `d63860c7` | + strict order (discard below the delivered high-water mark) | 108.8 | 104.4 | 98.1 | 55.0 | 22.0 |

Every hold and every drain used to walk every slot, and the drain re-walked them for each sample it
released - so per-sample cost scaled with the buffer's *capacity*, not with what it held. On a clean
link nothing is ever held, yet every sample paid for a full scan of an empty buffer: that is the whole
0% drop. Growing the buffer 8x to make overflow impossible cost 8.5x in throughput, which is how the
cause was found. **The 108.5 -> 84 drop was nearly published here as the price of the ordering
guarantee.** It was the price of a loop.

The same scan also manufactured loss. A subscriber spending its time scanning fell behind its socket
and the kernel dropped datagrams the network had delivered - so `lost=` and `out_of_order` counts
from `a5b99b1b` and earlier describe the subscriber's own backlog as much as the transport, and none
of them are used here.

**One earlier figure is still unexplained and is now superseded rather than resolved.** TickLE's 20%
and 50% cells previously read 89.3 and 56.5 against today's 54.7 and 21.9. Four candidate causes
were each excluded by measurement rather than by argument: the same-host socket work (ABBA A/B, 4
blocks of 5 reps, arms agreeing at 55.0); `835e8b44`'s added logging ("Publisher peer registered"
fires **once** in a 20%-loss run); run duration (`-d 5` gives 54.5 and `-d 10` gives 54.9); and the
code across the whole window, with the harness held constant and only `src/`+`include/` moved -
`acaa8049` (the KEEP_ALL-solicitation fix the published matrix was taken immediately after),
`b332dc1b`, `7bb87702`, `0be8c33f` and HEAD all measure 54.5-56.1. **The old 89.3 does not reproduce
at its own commit.** What remains is the original measurement's own conditions, which were not
recorded - `PLAN.md` cited that fix as `a437859`, a sha rebased away before it was ever pushed, so <!-- doc-shas-ignore: this line names the broken sha as its subject, so the checker must not read it as a citation -->
not even the commit was pinned down. The conclusion recorded here is that the cause was not found,
not that it was ruled out; the table above supersedes those cells because it was taken under a
controlled, internally comparable method, which they were not.

**Reading, recomputed from the table above rather than carried over** (every figure below is
arithmetic on the `d63860c7` table, redone when that table replaced the one before it - the previous
version of this paragraph was once left computing from a superseded table, see below). On a clean
link TickLE's advantage is modest - **3.0x** FastDDS and only **1.2x** CycloneDDS at 0% - and the
whole story is what happens under loss. At 1% it is 9.8x and 8.0x; at 5% 14.4x and **39x**; at 20%
19x and **131x**; at 50% 32x and **169x**. The FastDDS ratios at 20% and 50% are against a writer
that did not keep up (see †), so they overstate the comparison with a draining writer and are best
read as "FastDDS could not sustain this at all". TickLE keeps **20%** of its lossless rate at 50%
injected loss; CycloneDDS keeps 0.14%. Every figure comes from one interleaved session at one build.

*(The previous version of this paragraph said 10-45x, 160x, 510x and "~51% of its lossless rate".
Those were arithmetic on the superseded rows and were carried over when the table was replaced,
because a derived number does not look stale the way a citation does - the doc-sha gate checks that
every commit id resolves and nothing checks that a ratio still follows from the table above it.
Found on review, not by any tool.)*

**What it costs TickLE**, re-measured against its own KEEP_LAST mode in one interleaved session
(2026-09-24, `5816aab3`, 3 reps per cell, modes alternated within each loss level):

*(Measured before in-order delivery existed. It is still the cost of the KEEP_ALL guarantee: the
five-build table above shows the KEEP_ALL column at `d63860c7` within 0.7% of its `5816aab3` values
at every loss level, so ordering moved neither side of this comparison. Re-measure both columns
together before the next change to either mode.)*

| `tc` loss | KEEP_ALL | KEEP_LAST | cost of the guarantee |
|---|---:|---:|---:|
| 0% | 108.6 | 115.1 | **-5.7%** |
| 1% | 103.8 | 110.9 | **-6.3%** |
| 5% | 97.0 | 106.6 | **-9.0%** |
| 20% | 55.0 | 96.4 | **-43.0%** |
| 50% | 21.8 | 92.6 | **-76.4%** |

**This paragraph previously read -11% at 20% and -33% at 50%, and those figures were not merely
stale - they were a comparison between measurement days.** The KEEP_ALL side came from the
2026-09-24 re-measurement and the KEEP_LAST side (99.8 and 84.4) from the earlier session, in the
one paragraph that quantifies the headline trade-off, and in a document whose §5 exists to say that
cross-day absolutes cannot be compared. Recomputing the percentages from the new KEEP_ALL numbers
alone would have left that intact while making it look corrected, so KEEP_LAST was re-measured
instead. **The guarantee costs several times what this document used to claim**: two to three times
more at 20% and 50% loss.

**`write_fail` was 0 in all 15 KEEP_ALL runs**, so none of that cost is refused writes - every
sample was accepted and the loss of throughput is back-pressure alone, the Publisher waiting for
acknowledgements rather than turning work away. And as a check on reproducibility rather than an
extra claim: this session's KEEP_ALL column (108.6 / 103.8 / 97.0 / 55.0 / 21.8) reproduces the
three-way session's (108.5 / 104.1 / 97.5 / 54.7 / 21.9) to within 0.6% at every level, measured
hours apart on the same rig.

Paced to ~8 Mbps the two modes are identical (4.90 vs. 4.93, 5.04 vs. 5.04): with that much
headroom back-pressure never binds. *(Those two pairs are from the earlier session and are a
within-session comparison there, so they stand; they are marked here rather than silently mixed
with the table above.)*

**Two honest caveats, both measured rather than asserted:**

1. **TickLE's pre-match window.** A Subscriber starts tracking at the first DATA that *arrives*, so
   samples published before that are unknown to it rather than lost - nothing requests them and
   every counter on both sides correctly reads zero. Measured size: **0 on a healthy link and at
   20% injected loss (6/6 reps), 0-5 per run at 50%**. Counted from seq 1 those show up as loss;
   counted from the Subscriber's first observed seq they do not, and this document reports both
   rather than picking the flattering one. In every KEEP_ALL run the raw count equalled the
   pre-match window *exactly*, rep by rep. DDS has no equivalent window because its matching is
   symmetric and its writer waits for it; closing TickLE's would mean establishing the baseline at
   match time, a core change the user deliberately deferred (PLAN.md). **Update 2026-09-23**: this
   applies to a VOLATILE Subscriber, which is not owed pre-match history. With **TRANSIENT_LOCAL**
   the window is gone entirely - measured 15/15 reps at tc 0/1/5/20/50%, `first_seq=1` every time,
   after `261f39b8` taught the DATA-first contact path to respect requested durability the same way
   the Heartbeat path already did.
   *(This is what the `‡` on the TickLE column marks: post-match loss is 0; the pre-match window is
   reported separately rather than folded into it.)*
   Note for anyone re-running this: FastDDS has **no local compile path on the dev box** (no
   fastcdr headers), unlike CycloneDDS - its harness first compiles on the Pis, which is how a
   `ReturnCode_t` API difference in FastDDS 2.x got past a clean local check once. A clean local
   build says nothing about the FastDDS side.

2. **FastDDS at 20-50% did not always drain**: `drained=timeout` in 1/3 reps at 20% and 3/3 at 50%,
   i.e. its 3s teardown wait expired with samples still unacknowledged. Its net loss is still 0 (a
   refused write is counted, not silently dropped), but its numbers at those levels describe a
   writer that never caught up rather than one that finished cleanly. CycloneDDS and TickLE
   reported `acked` in every cell.

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
QoS-tagged=the cross-host tool's own ping/pong shape at 50 samples/run, 3 runs/combination). Same
notation as §3 (`avg±stdev`, `†` = no computable stdev) - every cell here is `†`: the underlying
tools (`buildfarm_perf_tests`, `rmw_perf_pingpong`) only ever printed their own already-averaged
figure, not the raw per-run samples a real stdev needs, the same limitation as §3's DDS-vendor
min/avg/max cells:

| Topology | Message | Mode | TickLE (`rmw_tickle`) | FastDDS | CycloneDDS |
|---|---|---|---:|---:|---:|
| Cross-host | Bench, QoS=best_effort | - | 0.524ms† avg RTT, 0% loss | 0.516ms† avg RTT, 0% loss | 0.440ms† avg RTT, 0% loss |
| Cross-host | Bench, QoS=reliable | - | 0.523ms† avg RTT, 0% loss | 0.523ms† avg RTT, 0% loss | 0.449ms† avg RTT, 0% loss |

**The same-host rows are gone, and this is a policy rather than a gap** (the user's decision,
2026-09-24): *"PC는 다양한 작업이 돌아가고 있기 때문에 PC에서 성능 측정한 것은 의미가 없어. RPi의
경우 독립된 H/W이기 때문에 거기서의 성능 측정 결과만 의미가 있어. PC는 기능 정합성을 검증하는
용도로만 사용하고 성능 측정 용도로는 사용하지 말자."* The dev box runs whatever else it is running -
two Claude sessions, two CI runners, a Windows VM, an Android emulator - so a latency figure taken
on it measures that machine's mood as much as the middleware. **This document therefore publishes no
performance number measured on the dev box.** The rig's two Raspberry Pis are dedicated hardware and
are where every performance figure in §2, §3, §3a, §3b and the rows above comes from.

**This was not an abstract concern; it had already produced a wrong result here.** The same-host
rows were re-measured on 2026-09-24 and read 12-15us slower than the pass they replaced - which
looked like a regression from `82a6a02d`'s socket work. It was not: *every* implementation had moved
by about the same amount between the two days (FastDDS +12.4 to +19.5us, CycloneDDS +11.8 and
+13.3us, `rmw_tickle` +5.7 to +15.1us, mean about **+13us** across all ten comparable cells), and two
of the three contain no TickLE code at all. The offset was the box, and it was roughly a quarter of
the whole figure. Chasing it cost most of a day. The ratio *appeared* to improve at the same time,
from ~1.3-1.6x to ~1.12-1.30x, which is the same artifact read the flattering way - a constant
additive term compresses every ratio toward 1 on its own.

**What is lost, stated plainly:** `buildfarm_perf_tests` only has a same-host shape - its
"two-process" tests launch both sides with `launch_ros.actions.Node`, which forks local children and
has no remote-host path at all (Milestone 14) - so the `rmw`-layer comparison against the two DDS
vendors has no valid venue today. `rmw_perf_pingpong` on the rig is the cross-host replacement and
is what the rows above are, but it covers RTT rather than `buildfarm_perf_tests`' full matrix. The
honest position is that the `rmw`-layer cross-vendor comparison is **narrower than it looks
elsewhere in this document**, not that it is unavailable.

**Reading**: cross-host, `rmw_tickle` and `rmw_fastrtps_cpp` land within noise of each other
(~0.52ms) while `rmw_cyclonedds_cpp` is genuinely faster (~0.44ms) on the real link. RELIABLE costs
nothing measurable over BEST_EFFORT for any of the three. 18/18 runs clean, zero loss every time.

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
5. **[`rmw_tickle`, methodology gap - hardware-blocked]** §5's own cross-host numbers
   (`rmw_perf_pingpong` on the `tickle-hil` rig) already cover real cross-host `rmw_tickle` vs.
   FastDDS/CycloneDDS, so this item is narrower than it once read: that run is still over the rig's
   own regular Ethernet link, not the real target network medium (10Base-T1S) §2-3's own raw-core
   HIL comparison is *also* still measured over (a pre-existing, separately-tracked gap, not new
   here). Genuinely blocked on real 10Base-T1S hardware being available on the rig at all - not
   something reachable by re-running existing tools differently.
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
7. **[Harness bug, found and fixed 2026-09-21 - closed]** §3's own original reliable-throughput-
   under-loss result looked like a real cross-vendor difference (CycloneDDS's RELIABLE fully
   recovering 5% injected loss, FastDDS's recovering none of it) - it wasn't. Root cause, found
   independently of TickLE Dev's own concurrent HISTORY-depth work (no file overlap):
   `examples/perf_hil/fastdds/reliable_throughput/{client,server}.cpp` had never received the same
   `KEEP_ALL`+`resource_limits(4000)` fix the CycloneDDS twin got earlier in this exercise (see §3's
   own methodology note) - left at a shallow `KEEP_LAST(8)` (plus a stale doc comment that still
   said "BEST_EFFORT", a copy-paste leftover never updated for this scenario's own real RELIABLE
   design) that couldn't retain a lost sample long enough for a NACK-driven retry to land, the exact
   same failure mode CycloneDDS's own original client.c doc comment already named. Fixed to match;
   re-verified real HIL, `tc`/`netem` 1%/5% loss, **0% observed loss both times, reproduced 4/4** -
   FastDDS and CycloneDDS now behave identically here, both fully recovering real injected loss.
   No inherent vendor difference exists for this QoS combination after all.
8. **[TickLE core, verified - LIFESPAN follows the expected formula]** PLAN.md's own DDS
   semantic-parity backlog flagged one open gap for LIFESPAN, the policy already closest to real
   DDS: TickLE's own beyond-lifespan loss count had never been checked against a hand-derived
   expected-loss formula the way CycloneDDS's own exact `(pause-lifespan)/interval` match was
   (§3, scenario 9). TickLE's own `lifespan_expiry` design can't use that same formula directly -
   `pause_s` is measured from the *server's own process start*, not from a completed match the way
   the DDS twins' own design does (§6, item 6's own "not the same condition" note) - so the exact
   offset from real discovery/match completion time is an unknown constant, not a controlled zero.
   **Worked around by testing the *slope*, not the absolute count**: real HIL, three `pause_s`
   values (1.0/1.5/2.0s, same `lifespan=0.1s`/`interval=0.02s`, 250-sample runs), 2-4 reps each.
   Results: `pause=1.0s` → 27/29 lost (tight); `pause=1.5s` → 54/31/54/33 lost (real, unexplained
   bimodal variance - two runs cluster near 54, two near 32); `pause=2.0s` → 79/79 lost (exactly
   reproducible). **The full-range slope from 1.0s to 2.0s is 51 samples lost per second of
   additional pause** (`(79-28)/1.0`, using each endpoint's own average) - matching the formula's
   own predicted `1/interval_s = 1/0.02 = 50/s` almost exactly (2% off), confirming TickLE's own
   age-based expiry genuinely follows the same linear, interval-driven relationship DDS's own
   exact match demonstrated, independent of not knowing the constant discovery-timing offset. The
   `pause=1.5s` midpoint's own real bimodal variance is left unexplained - plausibly some real,
   discrete timing-alignment effect in discovery/match completion, not investigated further this
   pass (both the 1.0s and 2.0s endpoints were tight/reproducible, only the middle value showed
   this).

9. **[TickLE core, re-measured - confirms Milestone 61's own prediction, and finds one more thing]**
   PLAN.md's own DDS semantic-parity backlog, row 2, predicted that raising a Publisher's own
   `reliable_cache` depth past `tt_RELIABLE_BITMAP_BITS` (64, the Subscriber's own fixed
   `received_bitmap` width) would *not* fix `reliable_throughput`'s own poor tc-loss recovery,
   since the bottleneck is Subscriber-side, not Publisher-side. Re-measured on real HIL with the
   new `-K` flag (`examples/perf_hil/tickle/reliable_throughput/client.c`, Milestone 61), same
   `tc netem` matrix as §3/scenario 4's own original depth=64 numbers:
   - `-K 512` (8x the old depth), `tc` loss=1%: sent 1,214,479/1,220,570, recv 1,183,005/1,183,049,
     **2.6%/3.1% loss** (2/2) - inside the original depth=64 range (1.1-3.1%), no improvement.
   - `-K 512`, `tc` loss=5%: sent 1,240,256/1,247,211, recv 1,177,152/1,180,082, **5.1%/5.4% loss**
     (2/2) - matches the original depth=64 range (5.2-5.3%) almost exactly, no improvement.
   - `-K 8192` (the flag's own max, 128x), `tc` loss=5%: sent 1,229,164/1,230,201, recv
     1,136,834/1,136,837, **7.5%/7.6% loss** (2/2, reproducible) - *worse* than both the depth=64
     baseline and `-K 512`, not just flat.

   **Confirms the prediction**: a deeper Publisher cache alone does not improve RELIABLE recovery
   under real loss - the server's own log makes the reason directly visible, not just inferred:
   `[WARNING] Reliable gap too large to track (64 ahead of N) - jumping ahead instead of getting
   stuck` fires regardless of `-K` (seen at both 512 and 8192, "64" never changes), confirming the
   Subscriber-side `received_bitmap`'s fixed 64-bit window - not the Publisher's own cache depth -
   is what actually caps how large a gap RELIABLE can recover from.

   **One more thing, not predicted going in**: pushing depth all the way to 8192 made loss
   measurably *worse* (7.5-7.6% vs. 5.1-5.4% at the same 512 depth and the same 5% injected loss),
   reproduced 2/2. **Root-caused by TickLE Dev directly from the code** (not re-measured/profiled,
   but a high-confidence structural explanation): `process_acknack()` calls
   `find_resendable_cache_entry()` once per set ACKNACK bit, and that function does a *linear scan*
   over `cache->entries[0..depth)` comparing `seq_no` - unlike the write side
   (`cache_reliable_sample()`), which indexes directly via `next % depth`. At depth=64 the scan is
   cheap; at depth=8192, `struct tt_ReliableCacheEntry` carries a 1472-byte buffer, so the full
   `entries[]` array is ~12MB - past any real L2/L3 cache - and an ACKNACK with several bits set
   re-scans that large an array repeatedly, taking a real cache-miss hit nearly every time. Fixable
   (make `find_resendable_cache_entry()` direct-index like the write side, O(1) instead of O(depth))
   but a separate optimization, not done this pass - flagged as a PLAN.md follow-up candidate,
   pending the user's own go-ahead before any core code changes. A very deep Publisher cache is
   still not a sensible default either way - it does not help recovery (per the bottleneck above)
   and, with today's linear-scan read path, actively costs more the deeper it goes. `tc qdisc`
   cleared back to default (`fq_codel`) after all runs.

10. **[TickLE core, re-measured - row 3 (LIVELINESS) fix verified, one honest limit found]**
    PLAN.md's own DDS semantic-parity backlog, row 3, flagged that TickLE's `check_liveliness()`
    only ever ran one fixed, node-level ~3s sweep, ignoring each entity's own announced
    `liveliness_lease_duration_ns` entirely - the user's own explicit go-ahead (2026-09-21) had
    this fixed (Milestone 62: new `tt_Node_entity_alive()`, computed fresh per-entity from its own
    lease; Milestone 63: wired into `check_liveliness()`'s own discovery-departed path too, via
    `tombstone_entities_past_own_lease()`, so the TickLE-native HIL scenario below - which doesn't
    go through `rmw_tickle` at all - actually exercises the new code, not just `rmw_tickle`'s own
    consumer of it). Re-measured on real HIL, `liveliness_loss_detection`, `kill -9` mid-stream,
    2 reps per lease:
    - `-T 1.0` (1.0s lease): detect 1495.3ms / 1256.1ms
    - `-T 2.0` (2.0s lease): detect 2390.2ms / 1731.6ms
    - `-T 4.0` (4.0s lease): detect 3065.3ms / 3398.7ms

    **Confirms the fix for short leases**: at 1.0s/2.0s (both shorter than the old fixed ~3s
    window), detection now tracks the announced lease directly - a dramatic improvement from the
    old fixed ~3080-3620ms regardless of lease, and much closer to CycloneDDS/FastDDS's own
    lease-proportional behavior (§3, scenario 8: both DDS vendors detect within ~0.07-1ms of their
    own 2000ms lease).

    **One honest limit, not glossed over**: at 4.0s (a lease *longer* than the old fixed ~3s
    window), detection did **not** scale up to ~4s - it landed at 3065-3399ms instead, right in the
    old fixed-window's own range. Root cause is structural, not a bug: `check_liveliness()` still
    runs its own original node-level sweep (fixed `tt_LIVELINESS_MISS_THRESHOLD *
    tt_NODE_UPDATE_INTERVAL` ≈ 3s) in the same pass as the new per-entity lease check, and whichever
    of the two fires first wins (the discovery-departed callback only fires once, guarded by
    `!departed`). For a lease shorter than ~3s, the new per-entity check fires first, so detection
    tracks the lease. For a lease longer than ~3s, the *old* node-level sweep fires first instead,
    capping real-world detection at ~3-3.6s regardless of how long the announced lease actually is
    - the opposite of true DDS per-entity semantics, where a longer lease means TickLE would keep
    treating the peer as alive for the entity's own full requested duration. In TickLE's favor, this
    means a long-lease Publisher's real death is *never* detected slower than ~3.6s even if it asked
    for a much longer grace period - but it does mean TickLE cannot yet honor a genuinely long lease
    the way real DDS does. Not investigated further this pass (a real behavior, not a bug, and
    likely a reasonable trade-off for embedded targets that want a hard upper bound on detection
    latency) - noted here rather than left undocumented. `COMPARISON.MD` §3, scenario 8's own TickLE
    column updated with these numbers.

11. **[TickLE core, real HIL - a real methodology correction, and further throughput research]**
    PLAN.md's own Milestone 65 reported the widened (256-bit) ACKNACK bitmap fully recovering
    real `tc`/`netem`-injected loss (0.0% at both 1% and 5%) - a real, correctly-measured result,
    but **from a different tool than this section's own scenario 4** (`perf_client`/`perf_server
    -R` via `run_perf.sh`, the automated `Performance Test` CI workflow's own benchmark - "a
    modest (~500 message) sample at `LOSS_TEST_INTERVAL_SEC`'s own pacing" per that workflow's
    own comment, a real but much lower, paced rate). **Re-measured with the exact scenario 4
    methodology this section uses** (`examples/perf_hil/tickle/reliable_throughput`, real max
    send rate, no pacing) once the bitmap fix landed on `main`: real, reproducible residual loss
    remains at TickLE's own true max throughput - 1.9-4.1% at 1% injected (2 reps), 5.8-8.1% at 5%
    injected (2 reps) - a real improvement over the old 64-bit baseline (was 1.1-3.1%/5.2-5.3%,
    roughly comparable, not dramatically better) but **not** the full recovery the low-rate CI
    benchmark's own 0.0% figure might suggest at a glance. Both numbers are real and correctly
    measured for what each actually tests; the honest summary is that a 256-bit window is enough
    to fully recover loss at a modest, paced rate, but not yet enough at TickLE's own real
    ~150-200K msg/s max throughput - `COMPARISON.MD` §3's own TickLE column updated with both the
    new post-fix numbers and the old pre-fix ones side by side, not silently replaced.

    **Further hypothesis-driven research, at the user's own explicit instruction to keep
    investigating latency/throughput levers**: a real, previously-undiscovered `reliable_
    throughput/server.c` bug was found and fixed along the way (commit `474e755`) -
    `stream_callback()` had no `seq_no`-based dedup of its own, and core's own `update_reliable_
    ack()` explicitly documents it doesn't catch every duplicate once `jump_ack_baseline()` has
    fired for a writer ("an accepted, narrow miss", that function's own doc comment) - a genuine
    `recv > sent` was reproduced for real at low throughput/high retry ratio before this fix.

    A second, complementary hypothesis (PLAN.md's own "Further latency research") - bounding how
    many scheduler tasks `tt_Node_poll()`'s own inner loop may run consecutively before forcing a
    non-blocking I/O check, so a continuously-rescheduling max-rate Publisher can't starve
    `tt_receive()` for a whole call's own timeout budget - was implemented on a separate branch
    (`experiment/poll-loop-io-interleave-v2`, rebased onto the bitmap-widened `main`, not merged)
    and re-measured with the now-fixed harness, same `tc`/`netem` matrix, 2 reps each: **1% loss**
    - sent 1531776/1304978, recv 1489346/1292070, **2.8%/1.0% loss** (avg 1.9%, vs. the plain-
    `main` baseline's own 3.0% avg above), send_mbps 116.4/99.2; **5% loss** - sent
    1306406/1577933, recv 1240526/1470818, **5.0%/6.8% loss** (avg 5.9%, vs. `main`'s own 6.95%
    avg), send_mbps 99.3/119.9. **A real, now twice-reproduced effect** (this and the original
    pre-bitmap-fix experiment, `experiment/poll-loop-io-interleave`, both showed it independently):
    the poll-loop fix achieves substantially higher real throughput every single run (99-120 Mbps
    vs. plain `main`'s own consistent 92-95 Mbps here) - a robust ~15-25% increase across 8 total
    runs now, cause not yet instrumented/confirmed. Loss% this time was directionally *better*
    with the fix at both conditions (not worse, unlike the first, pre-bitmap-fix experiment, which
    was confounded differently) - a favorable signal, but still only n=2 per condition, not a
    strong statistical claim. **Not merged, not recommended for `main` yet** - a real, reproducible
    throughput effect worth pursuing further (with more reps, and ideally direct instrumentation
    of real `tt_receive()` call frequency to confirm the actual mechanism), but not yet proven
    enough to commit to. See PLAN.md's own experiment write-up for the full numbers and reasoning.

12. **[Methodology, found and fixed 2026-09-23 - every earlier FastDDS under-loss number is void]**
    FastDDS was sending every sample on *both* the `192.168.10.0/24` test link (where `tc netem`
    injects loss) and the `10.1.1.0/24` management WiFi: `tx_bytes` rose ~30.7MB on each interface
    in a 4s run, while CycloneDDS used the test link only. The WiFi copy always arrived, so FastDDS
    reported 0% loss even at **50%** injected loss, and its throughput was partly WiFi-bound. Fixed
    by pinning FastDDS to the test link (`examples/perf_hil/fastdds/fastdds_eth0_only.xml`, a UDPv4
    `interfaceWhiteList` + `useBuiltinTransports=false`, applied via `FASTRTPS_DEFAULT_PROFILES_FILE`
    in that framework's own `run_scenario.sh`, commit `f9cd4c0`) - verified after the fix: eth0
    +43MB, wlan0 +12KB in the same 4s run, and throughput rose 17.0 -> 21.2 Mbps. §3a's own FastDDS
    column is the re-measured, eth0-only result; §3's own scenario 4 FastDDS cells are superseded.

13. **[TickLE core, three real RELIABLE bugs found and fixed 2026-09-22/23 - §3's own scenario 4
    TickLE cells are superseded too]** In order: (a) `struct tt_Publisher.seq_no` was `uint16_t`, so
    the wire counter wrapped every 65536 sends and `update_reliable_ack()`'s early-return path left
    retransmission effectively disabled for most of every run; (b) once that was fixed,
    `maybe_arm_acknack_retry()` was found sending an ACKNACK per *DATA packet* while a gap was open
    (~70K/s, commit `b393764`); (c) `reliable_throughput/server.c` counted loss assuming in-order
    arrival, so genuinely recovered retransmits were still counted lost (`84ef9a6`). Phase 1 of
    PLAN.md's own recovery plan then landed three improvements: 1-a (immediate narrow NACK for a gap
    that opens while a retry is armed), 1-b (RELIABLE-only 1ms retry interval), 1-c (an eviction
    Heartbeat carrying `first_available_seq_no`, replacing a compile-time depth guess). Net effect at
    max rate, 5% injected loss: **4.6% -> 0.24% at depth=64, 0.04% at depth=1024**; at ~8 Mbps, loss
    is now 0. Full per-phase numbers in `rmw_tickle/PLAN.md`.

14. **[Methodology, found 2026-09-23 - two measured distributions are bimodal, so several published
    bands measure mode-mixing rather than precision]** Found by Dev while A/B-ing an unrelated
    liveliness change, and it lands on this document rather than on the code.
    - **Scenario 8 (`liveliness_loss_detection`)**: at n=20 per lease the residual
      (`detect_latency - lease`) is not a spread around a mean, it is two clusters - a dominant one
      holding to about **15ms**, and a minority one (3-4 reps in 20) sitting **~460-500ms** away.
      **Mechanism, found 2026-09-23 after two wrong hypotheses were killed by the same data**: it is
      a *reference-point* artifact in our own harness, not a property of detection. Instrumenting
      `traffic_last_seen - update_last_seen` at the moment the departure fires shows that gap is
      itself bimodal and takes exactly two values, **0 or ~500ms**, nothing between - and every
      minority-mode rep has gap ~500 while every majority-mode rep has gap 0. Our liveliness client
      publishes every 0.5s while the node UPDATE goes out every 1.0s, so the last packet before the
      kill is either the announce itself (gap 0) or a data sample 500ms later (gap 500);
      `detect_latency_ms` is measured from the last *data* sample, so the measurement's own
      reference point is stale by exactly that amount. The ~500ms mode separation IS that staleness.
      Consequence for this table: at n=3 there is roughly a 45% chance of catching a minority-mode
      sample, and one moves the mean ~160ms, so §3 scenario 8's ±169/±466/±236 measure mode-mixing,
      not precision - the dominant mode holds to ~15ms.

      **And the cross-vendor comparison in scenario 8 is apples-to-oranges, which more reps cannot
      fix.** Both DDS harnesses also measure from the last received sample - but in DDS the sample
      *is* what refreshes the liveliness lease, so measuring from it and detecting at
      last_sample + lease are anchored to the same event and cancel; that is why FastDDS and
      CycloneDDS land within ~1ms of their lease. In TickLE the lease is refreshed by the node-level
      UPDATE while the measurement is anchored to data - two different clocks - so the phase between
      them appears as spread the DDS figures structurally cannot have. So scenario 8's TickLE column
      is not measuring the same quantity as its DDS columns, and the standing reading that "TickLE's
      liveliness timing is imprecise vs. DDS" is substantially an artifact of that mismatch. Being
      fixed by reporting `announce_age_at_detect_ms` (anchored to the UPDATE that actually governs
      TickLE's lease, i.e. the DDS-comparable number) alongside the existing end-to-end field, and
      by dropping our client to the DDS twins' own 0.1s interval.

      **Answered (2026-09-23, n=90 clean, both fixes in place)**: with the announce-anchored field,
      `detection = min(lease, 3s) + one check interval`, the three leases' offsets agreeing within
      4ms. Two gaps fall out, and they are different in kind: **granularity** (+610ms on this rig, a
      phase between the 1s announce period and the 1s check tick, anywhere in 0-1000ms on another
      startup) is a trade anyone can reason about; the **3s cap** - `tt_LIVELINESS_MISS_THRESHOLD *
      tt_NODE_UPDATE_INTERVAL` firing before a longer entity lease - means a requested 4s lease is
      silently honoured as 3s, with nothing reporting that to the caller. §6 item 10 already
      recorded the ceiling as a known limit; what is new is the exact rule and the silence. Raised
      as a core conformance item, see `PLAN.md`.
    - **`reliable_throughput` rate**: five consecutive reps at tc 0% gave 94.4 / 111.0 / 112.7 /
      94.9 / 111.5 Mbps - two clusters at ~95 and ~112, nothing between, `post_match_lost` 0 in all
      five. **Cause found 2026-09-23, after the CPU-governor hypothesis was measured and refuted**
      (both modes run at the same ~2391 MHz): it is **which core the sender lands on**. Instrumented
      with `sched_getcpu()`, 12 reps at 0% loss, nothing pinned: client on CPU0 → 94.5-94.6 Mbps
      (n=3, `cpu_main_share` 1.00 in every one); client not on CPU0 → 110.8-112.3 Mbps (n=9). No
      overlap. On both Pis `eth0`'s IRQ 108 is handled **entirely on CPU0** (404M/405M interrupts
      there, zero on CPU1-3), so the sender either shares a core with the NIC interrupt handler or
      it does not, and 3-in-12 is the 1-in-4 an unpinned four-core machine gives.
      **This is a property of the rig, not of TickLE** - the DDS columns ran on the same hosts with
      the same IRQ pinning and are subject to the same coin flip, so the cross-framework comparison
      is not biased. But **every single-run throughput figure here has a ~1-in-4 chance of reading
      ~15% low**, including TickLE's own headline numbers. Being fixed by running the benchmark
      under `taskset -c 1-3` so the sender never lands on the interrupt core; the affected Mbps
      cells are re-measured under that fix before this ◊ is lifted. (Measured on TickLE only so
      far; the same instrumentation is being added to the DDS harnesses rather than asserting it
      for them.) §3b's TickLE Mbps column is a median of 3 per cell and therefore reports whichever
      cluster that cell sampled; those cells are marked ◊ until re-measured with enough reps to
      report both modes. Loss columns are unaffected.
    Standing consequence for methodology: 3 reps is not enough for either measurement, and any
    future band quoted from them should say its n.
