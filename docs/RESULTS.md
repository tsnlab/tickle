# TickLE performance results

TickLE against FastDDS and CycloneDDS, measured on two dedicated Raspberry Pi 5s. zenoh-pico and rmw_zenoh are shown
for reference and never scored. Columns are always in the order **TickLE, FastDDS, CycloneDDS**. Design background is
in [DESIGN.md](DESIGN.md), the ROS 2 layer in [RMW.md](RMW.md), the measurement rules in [TESTING.md](TESTING.md), and
open work in [ROADMAP.md](ROADMAP.md). Result file paths are relative to `examples/perf_hil/` unless they start with
`~`.

## At a glance

One table per area. A count is a number of scored comparisons: a table row, or one metric of one campaign cell. A row
or cell is a WIN only when TickLE beats every scored vendor, and a LOSE when any vendor beats it. The large-message
(L4) table instead scores each vendor separately, one letter each. Latency is measured **from the data's creation time** (its
scheduled publish time) since 2026-10-17. The older send-stamp figures stay as diagnostics. Rows not yet re-measured
from creation time say so.

**Cross-host, core** (each framework's native API; [section 2](#2-cross-host-two-pis-over-the-rigs-link))

| scored set | WIN | DRAW/TIE | LOSE | of |
|---|---:|---:|---:|---:|
| campaign `A` (17 cells, `09a0437d`), latency cells from creation time (`596e9c9c`, 2026-10-17) | 139 | 11 | 0 | 150 |
| campaign `T` (FastDDS `maxMessageSize` 1472) | 18 | 2 | 0 | 20 |
| QoS mechanics (row 44, the one scored row) | 1 | 0 | 0 | 1 |
| *campaign `A` until 2026-10-17 (latency from the send stamp)* | *139* | *11* | *0* | *150* |

| headline | TickLE | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| RELIABLE goodput, P1 76 B, Mbps | **107** | 28.1 | 52.2 | 7 |
| RELIABLE at 5% loss, share of unshaped throughput kept, P1 / P4 | **86.9% / 91.6%** | 43.1% / 38.9% | 4.8% / 1.6% | 16-17 |
| client CPU, cpu_s per Msample, P1 throughput | **5.7** | 24.3 | 7.8 | 18 |
| client peak RSS, KB, P1 throughput | **2,087** | 15,947 | 5,407 | 28 |
| wire bytes per sample, P1 | **138** | 312 | 183 | 35 |
| RTT p50 from creation time, P1, ms | **0.211** | 0.346 | 0.312 | 1c |
| RTT mean from the send stamp, P1, ms (diagnostic) | **0.206** | 0.290 | 0.257 | 1 |

**Same host, core** (shared memory on one Pi; [section 1](#1-same-host-shared-memory))

| scored set | WIN | DRAW | LOSE | of |
|---|---:|---:|---:|---:|
| rows S1-S19 (S14 not scored; latency from the send stamp) | 18 | 0 | 0 | 18 |

| headline | TickLE | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| BEST_EFFORT KEEP_LAST 1 goodput, max rate, P2, Mbps | **20,867** | 0.31 | 934 | S1 |
| RELIABLE goodput, P2, Mbps | **13,764** | 1,012 | delivery failed | S4 |
| RTT mean from the send stamp, P2, ms | **0.023** | 0.107 | 0.074 | S7 |
| client + server CPU per delivered sample, RELIABLE P3, us | **1.53** | 31.65 | delivery failed | S17 |

**rmw, cross-host** (`rclcpp` over each rmw; [rmw layer](#rmw-layer))

| scored set | WIN | DRAW | LOSE | of |
|---|---:|---:|---:|---:|
| rows 48-75 (latency from the send stamp) | 20 | 8 | 0 | 28 |

| headline | rmw_tickle | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| RTT mean from the send stamp, block wait, Bench 64 B BEST_EFFORT, ms | **0.246** | 0.320 | 0.269 | 48 |
| delivered msg/s, RELIABLE KEEP_ALL, Array1k, 5% loss | **74,129** | does not run | 1,808 | 73 |
| RTT median from creation time, Array1k RELIABLE 100 Hz, ms (the control, not scored) | **0.32** | 0.42 | 0.35 | [control](#latency-from-creation-time-2026-10-17) |

**rmw, same host** ([rmw layer, same host](#rmw-layer-same-host))

| scored set | WIN | DRAW | LOSE | of |
|---|---:|---:|---:|---:|
| rows R1-R21 and the 31 cells not shown (latency from the send stamp) | 52 | 0 | 0 | 52 |

| headline | rmw_tickle | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| RTT mean, block wait, Bench 64 B BEST_EFFORT, us | **35.8** | 121.4 | 111.6 | R1 |
| delivered msg/s, Array1k BEST_EFFORT KEEP_LAST 1, max rate | **140,885** | 17,547 | 70,420 | R14 |
| peak RSS, larger process, RTT Bench RELIABLE, kB | **13,572** | 26,264 | 15,248 | R12 |

**rmw, large messages** (Array1m and Array4m cross-host; [rmw large messages](#rmw-large-messages-l4-cross-host))

| scored set | WIN | DRAW | LOSE | of |
|---|---:|---:|---:|---:|
| L4 table, latency from creation time where measured (I4 cells, 2026-10-17) | 55 | 23 | 2 | 80 |
| *L4 table until 2026-10-17 (latency from the send stamp)* | *52* | *23* | *5* | *80* |

| headline | rmw_tickle | FastDDS / FastDDS 1472 | CycloneDDS | cell |
|---|---:|---:|---:|---|
| delivered, Array1m RELIABLE 30 Hz at 5% loss | **100%** | 0% / 100% | 2.4-4.0% | I1L |
| latency median from creation time, Array4m 30 Hz (link-saturating), ms | **55.8** | 2,026 / 7,599 | 1,900 | I4s |
| latency median from creation time, Array4m RELIABLE 15 Hz, ms | **36.68** | 37.59 / 42.35 | 38.73 | I4 |

### Every LOSE

- **rmw large messages, I4 best effort (Array4m, 15 Hz), CPU per MB against Fast DDS as shipped.** Subscriber 2.19
  against 2.10 ms/MB (+4%), publisher 3.72 against 3.62 (+3%). TickLE wins both against Fast DDS at 1472 and against
  CycloneDDS. Attributed 2026-10-17 (`experiments/i4be_cpu_gap.sh`, `i4be_cpu_gap2.sh`, perf stat per thread over 600
  samples; `~/rig_results_safe/i4be_cpu_20261017-145938.read.txt`, `i4be_cpu_nodirect_20261017-161251.read.txt`):
  the subscriber's gap is kernel time in the copy into the 4 MB sample buffer, which arrives cold (kernel cycles +23%
  on 19% fewer instructions, IPC 1.02 against 1.55; user cycles equal). Reading in place (`TT_RX_DIRECT`) is still
  the cheaper path: turned off, the kernel cost matches Fast DDS but the copy moves to user space and the total is
  0.29 ms per sample worse. The publisher's gap is interrupt billing, not work: these Pi kernels have no
  `IRQ_TIME_ACCOUNTING` and eth0's interrupt is on CPU 0, so network softirq time is charged to whatever thread runs
  there; with every process pinned to CPUs 1-3 the gap falls from 0.196 to 0.004 ms per sample (kernel instructions
  equal). The headline stays unpinned, so both rows stay LOSE as measured.

### Every DRAW

- **+10 ms netem** (rows 5 and 5c): a draw by construction, since 10 ms of netem swamps a 0.2 ms RTT. The rows are
  not scored; campaign `A` counts the cell's RTT as one DRAW.
- **Loss TIEs in the campaigns** (10 in `A`, 2 in `T`): TickLE lost nothing, and every vendor either lost nothing or
  had a range that touched zero.
- **rmw poll wait, rows 59 and 62-67, against CycloneDDS.** At 100 and 200 us sleeps, half a poll cycle of waiting
  dominates the round trip, and it is the same for every rmw. In row 67 CycloneDDS's median is lower (0.380 against
  0.391 ms) but inside the spread.
- **rmw KEEP_ALL Array4k at 0% loss** (row 74): all three are at the link ceiling.
- **rmw large messages, 23 of 80:**
  - delivered % in every complete cell (13), where everyone delivered 100%;
  - latency against Fast DDS as shipped at I1 and I1 best effort (2, send stamp; one TickLE rep lies inside Fast DDS's
    range);
  - subscriber and publisher CPU per MB against Fast DDS as shipped at I1, I1 best effort, I4 RELIABLE and I4s (8).
    At I4s TickLE's non-blocking `publish()` spends CPU on the ~84 samples per run that it drops at the sender.
- **I4s latency p99 from creation time** (not one of the 80): UNBOUND for every arm. Each arm leaves some offered ticks
  undelivered in a saturating cell.

**Not scored as rows:** rows 5 and 5c (netem dominates), rows 45-47 (every framework detects correctly), S14 (nothing leaves the
host), and zenoh-pico and rmw_zenoh (reference only). Places where
TickLE is behind inside a lead:

- rmw_tickle's kernel wake (ppoll, then recvfrom) is ~6 us slower than the vendors' blocking recvmsg, although it is
  fastest in every user-space segment of the round trip.
- On one host, TickLE's segment gains less over its own kernel path than FastDDS's does (9.7x against 11.6x).
- zenoh-pico has the better figure in rows 5, 6, 7, 18, 19, 24 and 35, and ties at the link ceiling in rows 11b-11c.
  Its P1 TCP lead (row 7) comes from coalescing about eleven samples into each segment (0.094 packets per sample
  against TickLE's 1.005), which is a different mechanism from per-sample datagrams.
- Hybrid transport under loss, TickLE against its own forced modes (ab/hybrid-land d8980f9b, before the receive drain;
  [Hybrid transport](#hybrid-transport-large-samples-rig)): AUTO's p99 is above forced MTU's, beyond the A/A floor, at
  0.01% loss (+0.12 ms) and 0.1% loss (+0.025 ms). Its wire bytes per unit are 0.46% higher at 1%. The pre-registered
  criterion against forced MTU failed at all three rates.

## How to read these numbers

The full rules are in [TESTING.md](TESTING.md) sections 4 and 5. In short:

- **Throughput is goodput.** It counts the samples the receiving application got complete and unique, inside the
  measured window. A partly received large sample counts as zero, and so do duplicates and retransmissions. A
  sender's rate is a diagnostic, never a figure, because a reader may have kept up with only a tenth of it.
- **Latency is measured from creation time.** The clock starts at the time the sample was due to be published (in a
  fixed-rate cell, start + k / rate) and stops when it arrives. A publisher that falls behind, for example one whose
  `publish()` blocks on a full link, pays for that wait. Latency measured from the stamp the sender writes just before
  sending (the *send stamp*) does not include that wait. It stays beside the creation figure as a diagnostic. In a
  ping-pong where nothing waits, the two differ only by how late the sender's timer wakes.
- **CPU, memory and wire bytes are per delivered sample** (or per MB delivered, or per round trip answered). Absolute
  CPU seconds are never compared: a framework that sends more in a fixed time burns more.
- **Nothing is pinned.** The OS schedules every process, as it does for real users. Figures with processes pinned to
  cores are labelled notes beside the unpinned headline, never instead of it.
- **Every figure has its provenance:** the build (a commit), the rate, the arm, and the raw file. The vendors run their
  distribution packages with their shipped defaults. A tuned vendor arm, such as FastDDS with `maxMessageSize` 1472, is
  shown beside the shipped one, never instead of it.
- **WIN, DRAW, LOSE** are decided by the repetitions' ranges. TickLE wins when every one of its repetitions beats every
  repetition of every scored vendor. It loses when a vendor's repetitions all beat all of TickLE's. Anything else is a
  draw. ✅ marks the best figure in a row and ❌ the worst.
- **Incomplete delivery.** A vendor that delivers less than it sent in a RELIABLE cell is listed as DELIVERY FAILED,
  with its counts, and is not scored. If TickLE does that, it loses the cell. VOID is reported as VOID, never as zero.
- **A figure belongs to its session.** Compare figures within one session. Day-to-day offsets of about 13 us move
  every framework.

## 1. Same host (shared memory)

Publisher and subscriber on **one** rig Pi, each framework on its own shared-memory path: TickLE's segment,
**FastDDS as shipped** (data-sharing, on by default) and **CycloneDDS with iceoryx**. Each arm's transport is checked
by a witness that cannot be configured into agreeing (loopback packet count, and TickLE's own `tx_shm` share).
Harness `experiments/s6_transport_cells.sh`, 3 repetitions per framework unless noted. Throughput is **goodput**
since 2026-10-17 (TESTING.md section 4): the subscriber's complete samples in the measured window, every repetition
counted, CPU per delivered sample; a RELIABLE framework that delivered fewer samples than it sent is listed as
DELIVERY FAILED and not scored (`fair_samehost_summary.py` applies both rules; until 2026-10-17 it applied no
delivery check, and S1-S6 were send rates from drop-free repetitions). **No process is pinned to a core** (TESTING.md section 5); S1-S10 (but S8) and
S17-S19 are from `experiments/fair_samehost_remeasure.sh` on `e521bbba` (2026-10-09;
`~/rig_results_safe/fair_samehost_e521bbba_20261009-033254.*`, read by `fair_samehost_summary.py`): every
framework drops the same warm-up and cool-down (4,096 round trips, 2 s), BEST_EFFORT runs KEEP_LAST 1 on all three,
explicitly (the DDS default), and the pinned figures are a note below. **✅ = best, ❌ = worst** in the row.

The RTT rows (S7-S10, S12, S18) are from the send stamp; they are not yet measured from creation time. Corrections
and earlier sessions of these rows are in [Corrections and history](#same-host-corrections-and-earlier-sessions).

| # | Metric | Condition | TickLE | FastDDS | CycloneDDS | build | details |
|---|---|---|---|---|---|---|---|
| | **Throughput, goodput Mbps** (higher is better; the subscriber's complete samples over the measured window) | | | | | | |
| S1 | BEST_EFFORT, max rate, KEEP_LAST 1 | P2 1292 B | ✅ **20,867** | ❌ 0.31 | 934 | `e521bbba` | [notes](#same-host-notes) |
| S2 | BEST_EFFORT, max rate, KEEP_LAST 1 | P3 1424 B | ✅ **21,982** | ❌ 0.32 | 1,029 | `e521bbba` | [notes](#same-host-notes) |
| S3 | BEST_EFFORT, max rate, KEEP_LAST 1 | P4 2800 B | ✅ **25,066** | ❌ 0.70 | 1,854 | `e521bbba` | [notes](#same-host-notes) |
| S4 | RELIABLE | P2 1292 B | ✅ **13,764** | ❌ 1,012 | DELIVERY FAILED ◊ (2,165) | `e521bbba` | [notes](#same-host-notes) |
| S5 | RELIABLE | P3 1424 B | ✅ **14,771** | ❌ 980 | DELIVERY FAILED ◊ (2,414) | `e521bbba` | [notes](#same-host-notes) |
| S6 | RELIABLE | P4 2800 B | ✅ **14,917** | ❌ 1,924 | DELIVERY FAILED ◊ (4,542) | `e521bbba` | [notes](#same-host-notes) |
| | **Latency, RTT mean ms** (lower is better; one ping in flight, 200/s unless noted) | | | | | | |
| S7 | RTT | P2 1292 B | ✅ **0.023** | ❌ 0.107 | 0.074 | `e521bbba` | [notes](#same-host-notes) |
| S8 | RTT, 20/s (60 s) | P2 1292 B | ✅ **0.032** | ❌ 0.197 | DELIVERY FAILED ◊ (0.071) | `e17b4e6f` | [notes](#same-host-notes) |
| S9 | RTT | P3 1424 B | ✅ **0.023** | ❌ 0.110 | DELIVERY FAILED ◊ (0.067) | `e521bbba` | [notes](#same-host-notes) |
| S10 | RTT | P4 2800 B | ✅ **0.027** | ❌ 0.100 | DELIVERY FAILED ◊ (0.069) | `e521bbba` | [notes](#same-host-notes) |
| | **CPU** (lower is better) | | | | | | |
| S11 | publisher, us per delivered sample | BEST_EFFORT P4, max rate, KEEP_LAST 1 | ✅ **0.898** | ❌ 35,234 | 11.85 ¶ | `228da18d` | [notes](#same-host-notes) |
| S12 | client, us per round trip | RTT P2, 20/s | ✅ **66.0** | ❌ 136.7 | DELIVERY FAILED ◊ (102.2) | `e17b4e6f` | [notes](#same-host-notes) |
| | **Memory** (lower is better) | | | | | | |
| S13 | publisher peak RSS, MB | BEST_EFFORT P4 | ✅ **3.2** | ❌ 15.9 | – | `e17b4e6f` | [notes](#same-host-notes) |
| S17 | client + server us per delivered sample | RELIABLE P3 | ✅ **1.53** | ❌ 31.65 | DELIVERY FAILED ◊ (12.04) | `e521bbba` | [notes](#same-host-notes) |
| S18 | client + server us per round trip | RTT P3, 200/s | ✅ **41.9** | ❌ 167.9 | DELIVERY FAILED ◊ (96.6 ‡) | `e521bbba` | [notes](#same-host-notes) |
| S19 | client + server peak RSS, MB | RELIABLE P3 | ✅ **8.5** | ❌ 46.9 | DELIVERY FAILED ◊ (239.2 ‡) | `e521bbba` | [notes](#same-host-notes) |
| | **Mixed: one publisher, one subscriber on its host and one across the link** (delivered k samples/s, p2) | | | | | | |
| S15 | BEST_EFFORT, each subscriber | local / remote | ✅ **90.7 / 90.7**, loss 0 | 53.1 / 54.6, loss 4-22% | 49.7 / 70.6, loss 20-33% / 0.07-0.13% † | `7f127d56`, CycloneDDS `e4e090cd` | [notes](#same-host-notes) |
| S16 | RELIABLE, each subscriber | local / remote | ✅ **90.7 / 90.7** | ❌ 30.8 / 30.8 | 71.7 / 71.7 † | `7f127d56`, CycloneDDS `e4e090cd` | [notes](#same-host-notes) |
| | **Bandwidth** | | | | | | |
| S14 | wire bytes per sample | every cell above | 0 | 0 | 0 | | nothing leaves the host |

### Same-host notes

- **† S15-S16 CycloneDDS (2026-10-17):** measured at `e4e090cd` (as 4e49ed43 before its rebase onto main; the same
  harness) by `experiments/mixed_delivery_campaign.sh`, all three frameworks under one hold of the rig lock;
  `~/rig_results_safe/mixed_4e49ed43_20261017-152135.*`, 4 reps (TickLE and FastDDS 3). CycloneDDS's local subscriber
  used shared memory (iox-roudi) in every MIXED rep: witness 0.0000-0.0004 IP octets per local payload byte, against
  1.069-1.075 in the MIXED_NET control with shared memory off. RELIABLE delivered every sample to both subscribers
  (e.g. 670,482 of 670,482). Its RELIABLE LOCAL_ONLY control was DELIVERY FAILED (33% delivered, e.g. 743,091 of
  2,257,128, in all 4 reps). The same-day TickLE and FastDDS arms reproduce S15/S16 (TickLE 90.6 / 90.4, FastDDS
  56.6 / 56.2 and 28.5), so the rig did not move. S15 has no single worst: CycloneDDS is lower locally, FastDDS
  remotely.

- **◊ CycloneDDS DELIVERY FAILED in every RELIABLE cell it ran here**, so it is listed with its figure in
  parentheses and not scored. Throughput (S4-S6, and S17, S19 from the same P3 runs), whole-run delivered of sent:
  P2 1,846,706 of 1,847,846, 1,891,539 of 1,892,787, 1,912,083 of 1,912,622; P3 1,899,458 of 1,900,191, 1,914,870
  of 1,915,270, 1,909,889 of 1,910,394; P4 1,836,739 of 1,837,157, 1,875,547 of 1,876,358, 1,828,471 of 1,829,659
  (3 of 3 reps each, 0.02-0.07% missing, as sequence gaps mid-stream). RTT (S9, S18 from P3 and S10 from P4): one
  round trip not answered in 1 of 3 reps (10,133 of 10,134; 10,131 of 10,132). S8 and S12 (`e17b4e6f`,
  `~/rig_results_safe/s6_latency_p2_20hz_e17b4e6f.txt`): 1,186 of 1,187 in 2 of 3 reps. TickLE and FastDDS
  delivered everything they sent in every one of these cells. Pinned (the note below): CycloneDDS RELIABLE
  throughput delivered 33-44% of what it sent (e.g. 769,327 of 2,246,638 at P2), so the pinned CycloneDDS S4-S6
  figures (2,673 / 2,876 / 5,209) were send rates of an incomplete delivery; its pinned goodput was 912 / 1,035 /
  1,997 Mbps. Until 2026-10-17 this reader applied no delivery check; the cross-host reader always has. No verdict for
  TickLE changes; S19's worst (❌) moves from CycloneDDS to FastDDS. S12 is now per answered round trip: CycloneDDS
  102.1 -> 102.2 us.
- **S11 re-measured on goodput (2026-10-17)** at `228da18d` with `experiments/fair_samehost_remeasure.sh`
  (`CELLS=best_effort_throughput:p4 REPS=5`, free arm, 5 of 5 reps per framework), read by
  `experiments/s11_publisher_cpu.py` against the `e521bbba` vendor control (publisher CPU per sample sent 0.98x
  FastDDS, 0.99x CycloneDDS). Raw files `~/rig_results_safe/fair_samehost_228da18d_20261017-144159.*`. Per delivered
  sample: TickLE 0.898 us (0.896-0.900; delivered 99.99%-100% of 10.0M per rep), FastDDS 35,234 us (its subscriber
  took 207-377 of ~5.25M samples per rep, the overrun of S1-S3), CycloneDDS 11.85 us (19.9-20.8% delivered). ¶
  CycloneDDS's figure includes iox-roudi's CPU (0.012 us); its publisher alone is 11.83. Per sample sent (the old
  metric): 0.898 / 1.715 / 2.383. Pinned arm: 0.897 / 58,305 / 23.13. The paragraph below is the reading before this
  re-measure.
- **S11 could not be re-scored from existing data (until 2026-10-17):** its file (`~/rig_results_safe/p4_shm_headtohead_e17b4e6f.txt`) has
  no FastDDS receive count, so CPU per delivered sample cannot be formed; the figures stay as measured (per sample
  sent) and are no longer scored. For scale, `e521bbba`'s P4 BEST_EFFORT client + server CPU per delivered sample
  is 1.75 / 87,167 / 31.9 us (another build and another metric, so not a substitute).
- **‡ CycloneDDS includes iox-roudi**: its RSS adds 212 MB (206 MB of it shared memory) and its CPU 1.6 us per round
  trip (measured at `9f919c8b`).
- **Pinned** (the server on core 1, the client on core 2, back to back with the unpinned run): no row's leader changes
  in any session. At `e521bbba` TickLE's unpinned/pinned ratio is 1.00-1.02x, and the multi-threaded DDS processes
  gain more from pinning. Each session's pinned medians are in
  [Corrections and history](#same-host-corrections-and-earlier-sessions).
- **FastDDS delivers ~0% in S1-S3 (S1d-S3d until 2026-10-17).** At BEST_EFFORT KEEP_LAST 1 (the DDS default; data-sharing on, as shipped)
  its publisher sent ~5.5M samples in 9 s and its subscriber took 105-284 of them, in every rep and at every size.
  Re-run 2026-10-07 (`experiments/fastdds_be_delivery.sh` at `5ac23fbc`, p3, 3 reps;
  `~/rig_results_safe/fastdds_be_delivery_5ac23fbc_20261007-103007.*`): reproduced, 250 of 5.50M delivered
  (0.0046%); with the publisher slowed to 2.1% of that rate (148 Mbps) it delivered 100%, so the loss is a
  rate-dependent overrun. Whether the writer's two-payload history pool is the cause was not decided: the depth-64
  arm and the arm counting Fast DDS's own discards were VOID (2 of 3 usable reps). TickLE, the control, delivered 100%.
  Reproduced again at `f128f686` (2026-10-08): `delivered_ratio=0.000` at p2, p3 and p4, 3 reps each (280-463
  samples taken per rep). And at `e521bbba` (2026-10-09): `delivered_ratio=0.000` at p2, p3 and p4, 3 reps each.
- **S11 and S13 compare against FastDDS only.** CycloneDDS's memory excludes iceoryx's `iox-roudi` daemon, which
  reserved 216 MB of shared memory before any application connected. TickLE's segment is in-process, no daemon.
- **S10 is the narrowest RTT lead** (2.5x CycloneDDS at `f128f686` and `e521bbba`; 1.29x when this was investigated): a p4 sample is two datagrams and two slots on TickLE's path.
  The extra time appears only after idling: p4 - p3 is +6 us at 0.5 ms ping spacing and +20 us at 5 ms, against
  CycloneDDS's 0-2 us (`experiments/p4_interval_rig.sh`, `6234e915`). Ruled out: an extra wake (p4 rings the same
  doorbells per round trip, `p4_wake_count.sh`) and the doorbell's place between the fragments (`93504234` moved it
  after the batch; the gap at 5 ms stayed +21 us, which its pre-registered rule reads as refuted). Still open.
- **Where a same-host publisher's user time goes** (p3 BEST_EFFORT at max rate, 1.24M samples/s, 0.637 us of user
  time per sample, `25c2e1ce`, `perf record -e cycles:u` on rpi-1, 3 reps each within 2% of the unprofiled rate;
  `experiments/perf_publisher_profile.sh`): the clock 25% (4.13 `clock_gettime` per sample: 2 by the bench
  application, as in the FastDDS and CycloneDDS benches, 1 by `tt_Publisher_publish()`'s timestamp, 1 by the poll
  loop's scheduler; counted by `perf_publisher_clock.sh`), `memcpy` 28%, the segment header's shared indices 12%,
  the rest 35%. The header's three indices shared one cache line; `e0873623` gave each its own: +1.77% rate and
  -2.24% user time per sample, against +0.23% / +0.20% on the same commits built without the segment
  (`segment_layout_ab.sh`, 4 rounds of A B B A).
  FastDDS's S7/S8 ranges are wide (0.098..0.278, 0.103..0.383 ms) from one rep with a ~112 ms maximum.
- **Where the lead comes from:** on p4 BEST_EFFORT, TickLE's shared memory is 9.7x its own kernel path and FastDDS's
  is 11.6x its own; TickLE's kernel path is 1.87x faster than FastDDS's (`e17b4e6f`). Raw files:
  `~/rig_results_safe/s6_*`, `p4_shm_headtohead_e17b4e6f.txt`, `s6_latency_p{2,3,4}_*`.
- **Mixed delivery (S15, S16)**, `experiments/mixed_delivery.sh`, 2026-10-05: publisher and one subscriber on
  10.1.1.214, a second on 10.1.1.213, against LOCAL_ONLY and REMOTE_ONLY controls, 3 reps each. With a subscriber
  across the link both frameworks deliver locally at the link's pace (TickLE's local subscriber: 90.7k/s mixed
  against 1,269k/s alone). TickLE gives both subscribers the full link rate with no loss; FastDDS loses 4-22% at
  BEST_EFFORT and delivers a third of that rate at RELIABLE. No RELIABLE subscriber lost a sample after matching.

### rmw layer, same host

`rmw_tickle`, `rmw_fastrtps_cpp` and `rmw_cyclonedds_cpp` (jazzy), with `rmw_zenoh_cpp` 0.2.9 and its router as
reference, on **one** rig Pi through `rclcpp`, each on its shipped same-host transport: rmw_tickle's segment, Fast DDS's
SHM (both witnessed as shared memory), CycloneDDS's UDP over loopback (no iceoryx by default) and zenoh's TCP through
the router. RTT: the `rmw_perf_pingpong` pair, 20 s at 1 ms, 4,096 round trips excluded at each end. Throughput: apex
`perf_test` at max rate, 20 s, 2 s excluded at each end, delivered samples counted at the subscriber; BEST_EFFORT is
KEEP_LAST 1, RELIABLE is KEEP_ALL. Poll wait loops on `spin_some()` with 100 us sleeps, random phase. 3 reps per arm,
medians; verdicts by the reps' ranges (TESTING.md section 4's win rule), from `rmw_samehost_summary.py`.
**Build `06dd78d8`** (2026-10-09; `experiments/rmw_samehost.sh`, rmw_tickle built as the jazzy debs are, `-g -O2`;
`~/rig_results_safe/rmw_samehost_06dd78d8_20261009-110918.{txt,runs}`; preflight 9 usable runs, 0 VOID, no rep refused;
every cell n=3 for every arm). Since `a7e02807` it carries the zero-allocated context (`718220d8`, below), the
RELIABLE reader's repeated ask for a declined tail (`783bb5d5`) and the Calls changes (`f4bea420`, `06dd78d8`). The
last column is rmw_tickle in the same harness at `a7e02807` (2026-10-09,
`~/rig_results_safe/rmw_samehost_a7e02807_20261009-054624.txt`), with its verdict where it was not a WIN.
**Scored: WIN 52** of 52 (at `a7e02807`: WIN 46, DRAW 3, LOSE 3, every non-win a peak-RSS row; at `03585237`: WIN 50,
DRAW 1, LOSE 1; at `f128f686`: WIN 45, DRAW 1, LOSE 6).

| # | Metric | Cell | rmw_tickle | FastDDS | CycloneDDS | rmw_zenoh (ref) | verdict | rmw_tickle at `a7e02807` |
|---|---|---|---|---|---|---|---|---|
| | **RTT mean, us** (lower is better) | | | | | | | |
| R1 | block wait (`spin()`) | Bench 64 B, BEST_EFFORT | **35.8** | 121.4 | 111.6 | 234.2 | WIN | 36.0 |
| R2 | block wait | Bench 64 B, RELIABLE | **40.0** | 151.2 | 112.7 | 236.3 | WIN | 36.9 |
| R3 | block wait | Array1k, BEST_EFFORT | **38.3** | 129.8 | 111.5 | 242.5 | WIN | 37.8 |
| R4 | block wait | Array1k, RELIABLE | **39.1** | 151.8 | 112.9 | 241.6 | WIN | 38.5 |
| R5 | poll wait, 100 us sleep | Bench 64 B, BEST_EFFORT | **150.8** | 192.4 | 185.0 | 289.1 | WIN | 149.1 |
| R6 | poll wait, 100 us sleep | Bench 64 B, RELIABLE | **151.4** | 201.2 | 189.4 | 288.3 | WIN | 149.3 |
| R7 | poll wait, 100 us sleep | Array1k, BEST_EFFORT | **151.2** | 194.7 | 187.4 | 292.7 | WIN | 150.2 |
| R8 | poll wait, 100 us sleep | Array1k, RELIABLE | **150.9** | 204.2 | 191.1 | 291.7 | WIN | 150.9 |
| | **CPU, ping + pong, us per round trip** (lower is better) | | | | | | | |
| R9 | block wait | Bench 64 B, RELIABLE | **68.1** | 257.2 | 151.7 | 345.5 | WIN | 63.3 |
| R10 | block wait | Array1k, RELIABLE | **66.5** | 258.9 | 152.6 | 353.1 | WIN | 65.6 |
| | **Peak RSS, kB, larger of the two processes** (lower is better) | | | | | | | |
| R11 | RTT | Bench 64 B, BEST_EFFORT, block | **13,480** | 26,260 | 15,248 | 22,496 | WIN | 15,236 (DRAW) |
| R12 | RTT | Bench 64 B, RELIABLE, block | **13,572** | 26,264 | 15,248 | 22,464 | WIN | 15,336 (LOSE) |
| R13 | throughput | Array1k, RELIABLE | **16,000** | 35,796 | 17,152 | 102,588 § | WIN | 17,664 (LOSE) |
| | **Delivered msg/s** (higher is better) | | | | | | | |
| R14 | max rate, KEEP_LAST 1 | Array1k, BEST_EFFORT | **140,885** | 17,547 | 70,420 | 61,560 | WIN | 132,752 |
| R15 | max rate, KEEP_LAST 1 | Array4k, BEST_EFFORT | **83,048** | 4,171 | 60,818 | 45,879 | WIN | 79,187 |
| R16 | max rate, KEEP_ALL | Array1k, RELIABLE | **187,965** | 2,322 | 64,522 | 110,992 | WIN | 166,128 |
| R17 | max rate, KEEP_ALL | Array4k, RELIABLE | **155,635** | 17,507 | 49,904 | 88,879 | WIN | 147,550 |
| | **CPU, pub + sub, us per delivered sample** (lower is better) | | | | | | | |
| R18 | max rate | Array1k, BEST_EFFORT | **14.02** | 152.93 | 33.80 | 46.09 | WIN | 14.92 |
| R19 | max rate | Array4k, BEST_EFFORT | **23.83** | 636.13 | 36.79 | 58.17 | WIN | 24.98 |
| R20 | max rate | Array1k, RELIABLE | **6.35** | 639.34 | 35.12 | 21.17 | WIN | 7.09 |
| R21 | max rate | Array4k, RELIABLE | **8.64** | 83.84 | 38.72 | 26.40 | WIN | 9.04 |

Rows not shown: RTT p50 and p99 in every cell (WIN), CPU per round trip in the six other cells (WIN, 61.8-78.1 us
against FastDDS 170-245 and CycloneDDS 147-160), and the nine other peak-RSS cells (WIN, rmw_tickle 13,448-15,616 kB
against CycloneDDS 15,240-18,344).

- **The six peak-RSS non-wins of `a7e02807` are WINs** (R11-R13, and Array1k BEST_EFFORT block, Array1k RELIABLE
  block, Bench RELIABLE poll): across all twelve cells rmw_tickle peaks at 13.4-16.0 MB against CycloneDDS's
  15.2-18.3 MB. R11 15,236 -> 13,480 kB (-1,756), R12 15,336 -> 13,572 (-1,764), R13 17,664 -> 16,000 (-1,664); the
  control, CycloneDDS in the same cells, moved +8, -8 and +128 kB. The cause is `718220d8`: rmw_init() took the 2.2 MB
  context with allocate() and memset() it, which made every page resident for the life of the process.
  zero_allocate() takes pages the kernel has already zeroed and writes none, so pages never touched (the 2048-entity
  discovery table, most fragment slots) never become resident: ~1.75 MB less per process. `test_context_resident`
  checks it with mincore() and fails with the memset() put back; `50dbf111` turns transparent huge pages off for that
  test. **A host property:** where transparent huge pages are set to `always`, the kernel can back the context with
  huge pages, which become resident whole, so the saving there is smaller. This run does not record the rig's mode.
- **R2 and R9 rose** 36.9 -> 40.0 us and 63.3 -> 68.1 us per round trip (+8% each) at the median, while CycloneDDS in
  the same cell moved +0.2 us and +0.7 us. R2's reps (39.1 / 40.0 / 41.0) overlap `a7e02807`'s (36.8 / 39.5 / 36.9);
  R9's do not (67.8-70.2 against 63.0-67.2). R4, the Array1k RELIABLE block cell, moved +0.6 us. A
  pre-registered rig A/B did not confirm it (2026-10-09, `ab_rmw_regress.sh` `957f851d`, `a7e02807` vs `06dd78d8`, phases
  A B B A B A A B x 3 reps; `~/rig_results_safe/ab_rmw_regress_20261009-134229.verdict.txt`): RELIABLE RTT +0.78 us
  (95% CI -0.9..+2.4, t 1.14), CPU +1.5 us/rt (-1.4..+4.3), BEST_EFFORT RTT -0.2 us, CycloneDDS control +0.14 us;
  minor faults 0.018 per round trip in every phase, so `718220d8`'s lazy paging is not a cost. The cell has two states
  (~36 and ~39-41 us) at both commits; the published +3.1 us lies outside the A/B's interval.
- **R14 rose 132,752 -> 140,885 (+6.1%)**, with the control, CycloneDDS in the same cell, +6.2% (66,291 ->
  70,420). R16 rose +13.1% (166,128 -> 187,965) against CycloneDDS's +5.5% (61,166 -> 64,522). Both compare rmw_tickle
  at `a7e02807` with `06dd78d8`, two builds; the reader correction below moved neither by more than 0.02%.
- **No FastDDS rep refused**, in this run or at `a7e02807` (at `03585237` its tput Array1k RELIABLE r2 ended on
  "failed to publish"; see † below). **§ rmw_zenoh's** Array1k RELIABLE subscriber peaked at 103 MB (1.46 GB at
  `a7e02807`, 123,624 kB at `03585237`); it is a reference arm and not analysed further.
- **† (at `03585237`) FastDDS refused in tput Array1k RELIABLE r2:** its publisher ended on "failed to publish"
  (KEEP_ALL under the 100 ms `max_blocking_time`). Per the user's rule it was recorded as "does not run" in that
  rep (REFUSED, excluded, listed) and not analysed further; its R13, R16 and R20 were the median of its other two reps
  (n=2).
- **Accepted cost of skip-to-newest** (the user, 2026-10-08). A KEEP_LAST 1 reader that falls behind now takes the
  newest sample rather than draining in order, which gives R14/R15 their x3.6 / x5.2 over `f128f686` (in the rig A/B
  `~/rig_results_safe/ab_drain_20261008-021000.*`, phases A C C A C A A C: x2.3 at Array1k, x4.9 at Array4k). It
  costs two things, measured in that A/B: the delivered sample's age at Array1k BEST_EFFORT KEEP_LAST 1 max rate rose
  from 3.86 to 4.53 us (+17%, t +13.4), and native `reliable_latency` client CPU rose by +0.7% (p2) and +1.1% (p4)
  (t +6.1, +5.0). For scale, CycloneDDS's sample age in the same cell is 33 us (median at `f128f686`; 30.4-32.6 in
  this run, where rmw_tickle's is 4.38-4.51 and FastDDS's ~1,950). The vendor arms, which the change cannot touch, moved by +6% / +1% (CycloneDDS
  delivered, R14/R15) between the two runs.
- **rmw_tickle as shipped** (no `TICKLE_BROADCAST_ADDR`) is not in the rig arms, because every datagram would be a
  limited broadcast on the network the orchestration uses. The dry run measured it once (rtt Bench RELIABLE block,
  3 s, 2,223 round trips): p50 36.2 us, mean 37.4 us, `tx_shm` share 0.996 - on its segment, like the scored arm.
- At BEST_EFFORT max rate rmw_tickle's publisher sends ~1.8M samples/s and its KEEP_LAST 1 reader keeps the newest;
  CycloneDDS's publisher sends about what its reader takes (~70k/s). The delivered rate is the scored figure.

### Loaned messages (PC)

**A PC figure, not a rig one**, and rmw level, without rclcpp: `experiments/rmw_loan_bench.sh` (one publisher and one
subscriber process in a private netns, an Array1k-shaped type, max rate, 6 s with 1 s dropped at each end, 8 reps,
arms rotated; reading rules in its header, enforced by `rmw_loan_bench_summary.py`). Build: `ab/lending`'s head (the same three
patches main carries as `9d924607`) merged with `f837ae82` plus the loaned-messages change (`-O3` Release; the library's md5 is in the raw files), 2026-10-08 22:07;
raw files `~/rmw_loans_ws/bench3/`. The PC was carrying other jobs (load 5-8). Arms: `copy` (RMW_TICKLE_LOANS=0,
`rmw_publish`/`rmw_take`), `copy2` (the same again: the control), `loan` (borrow/publish_loaned, take_loaned/return:
decoded shells), `loan_ring` (`RMW_TICKLE_LOAN_RING_SLOTS=1`: read in the ring slot). Medians [range]:

| QoS | Arm | Delivered/s | Sub CPU us/msg | Pub CPU us/msg | Read in place |
|---|---|---:|---:|---:|---:|
| RELIABLE KEEP_ALL | copy | 1,151,275 [668,819-1,283,118] | 0.746 [0.692-0.809] | 0.816 [0.747-0.894] | - |
| | copy2 | 1,163,035 [603,433-1,324,266] | 0.728 [0.681-0.803] | 0.794 [0.729-0.864] | - |
| | loan | 988,960 [254,482-1,265,849] | 0.870 [0.684-1.212] | 0.900 [0.751-1.119] | 0 |
| | loan_ring | 1,091,068 [467,783-1,463,842] | 0.790 [0.624-1.222] | 0.843 [0.659-1.015] | 0.36 |
| BEST_EFFORT KEEP_LAST 1 | copy | 397,684 [325,241-462,234] | 2.476 [2.159-2.656] | 0.487 [0.328-0.606] | - |
| | copy2 | 379,180 [280,488-457,952] | 2.569 [2.183-2.609] | 0.487 [0.330-0.648] | - |
| | loan | 396,161 [345,223-461,996] | 2.470 [2.107-2.569] | 0.645 [0.394-0.710] | 0 |
| | loan_ring | 429,417 [310,540-503,978] | 2.221 [1.964-2.511] | 0.424 [0.353-0.529] | 1.00 |

- **Every comparison reads NO DIFFERENCE**: each arm's range overlaps `copy`'s. Two earlier 5-rep runs of the same
  harness (`bench1`, before the first two fixes below, and `bench2`, before the third) read the same way, with
  BEST_EFFORT `loan_ring` the one arm whose medians moved the same direction every time (+26%, +28%, +8% delivered;
  -21%, -22%, -10% sub CPU). `bench1` read three BETTER verdicts (`loan_ring` delivered and sub CPU, `loan` sub CPU
  by -2.9%) that neither later run repeated.
- Fixed on the way, from those runs: a borrow resolved its type support through the dispatch chain (and reset an rmw
  error) on every call; a loaned take held `queue_mutex` three times where a take holds it once; a kept publisher
  buffer was cleared (1 KB written) on every borrow.
- RELIABLE `loan_ring` reads only 36% in place: KEEP_ALL lets the queue run deep, and a subscription keeps at most 8
  queued samples where they arrived (core holds 16 per context); the rest are decoded.

## 2. Cross-host (two Pis over the rig's link)

**✅ = best, ❌ = worst** in the row. Lower is better for latency, CPU, memory and bandwidth; higher for throughput.
Latency is scored from creation time (rows 1c-6bc). Rows 1-6b are the send-stamp figures, kept as diagnostics.

| # | Metric | Condition | TickLE | FastDDS | CycloneDDS | zenoh-pico | rmw_zenoh | meas. |
|---|---|---|---|---|---|---|---|---|
| | **[Latency from creation time](#latency-from-creation-time-2026-10-17)** (RTT p50, ms) | | | | |  | – | |
| 1c | RTT p50 from creation | P1 76 B | ✅ **0.211** | ❌ 0.346 | 0.312 | – | – | C |
| 3c | RTT p50 from creation | P2 1292 B | ✅ **0.237** | ❌ 0.378 | 0.328 | – | – | C |
| 5c | RTT p50 from creation | +10 ms netem | ⚪ 10.445 | ⚪ 10.239 | ⚪ 10.257 | – | – | C |
| 6ac | RTT p50 from creation | P3 1424 B | ✅ **0.241** | ❌ 0.391 | 0.337 | – | – | C |
| 6bc | RTT p50 from creation | P4 2800 B | ✅ **0.259** | ❌ 0.403 | 0.347 | – | – | C |
| | **[Latency from the send stamp](#latency)** (ms; diagnostic since 2026-10-17) | | | | |  | – | |
| 1 | RTT mean | P1 76 B | ✅ **0.206** | ❌ 0.290 | 0.257 | 0.221 † | – | A |
| 2 | RTT max (tail) | P1 76 B | ✅ **0.252** | ❌ 0.342 | 0.296 | 0.336 † | – | A |
| 3 | RTT mean | P2 1292 B | ✅ **0.231** | ❌ 0.315 | 0.275 | 0.246 † | – | A |
| 4 | RTT max (tail) | P2 1292 B | ✅ **0.266** | ❌ 0.348 | 0.317 | 0.372 † | – | A |
| 5 | RTT mean | +10 ms netem | ⚪ 10.11 | ⚪ 10.34 | ⚪ 10.19 | 10.07 | – | A |
| 6 | RTT max (tail) | +10 ms netem | ✅ **12.20** | ❌ 12.26 | 12.24 | 12.15 | – | A |
| 6a | RTT mean | P3 1424 B | ✅ **0.234** | ❌ 0.337 | 0.291 | 0.248 † | – | A |
| 6b | RTT mean | P4 2800 B | ✅ **0.249** | ❌ 0.358 | 0.293 | 0.402 † | – | A |
| | **[Throughput](#throughput)** (goodput, Mbps) | | | | |  | – | |
| 7 | RELIABLE | P1 76 B | ✅ **107** | ❌ 28.1 | 52.2 | 236.8 † | – | A |
| 8 | RELIABLE | P2 1292 B | ✅ **937** | ❌ 591 | 864 | ✗ † | – | A |
| 9 | RELIABLE | P3 1424 B | ✅ **943** | ❌ 401 | 716 | ✗ † | – | T |
| 10 | RELIABLE | P4 2800 B | ✅ **943** | ❌ 519 | 891 | ✗ † | – | T |
| 11 | BEST_EFFORT | P1, max rate | ✅ **115** | ❌ 28.8 | 62.1 | 53.4 | – | A |
| 11a | BEST_EFFORT | P2 1292 B, max rate | ✅ **938** | ❌ 575 | 906 | 867 | – | A |
| 11b | BEST_EFFORT | P3 1424 B, max rate | ✅ **943** ‡ | ❌ 560 | 654 | ✅ **941** ‡ | – | A |
| 11c | BEST_EFFORT | P4 2800 B, max rate | ✅ **944** ‡ | ❌ 902 | 937 | ✅ **945** ‡ | – | A |
| 12 | RELIABLE KEEP_LAST 64 | P1 | ✅ **117** | ❌ 38.0 | 72.6 | – | – | A |
| 13 | RELIABLE | P1, **5% loss** | ✅ **93.0** | 12.1 | ❌ 2.49 | ✗ † | – | A |
| 14 | RELIABLE | P4, **5% loss** | ✅ **865** | 216 | ❌ 14.3 | ✗ † | – | T |
| 15 | RELIABLE | P1, 5% reorder | ✅ **95.2** | ❌ 28.9 | 60.7 | ✗ † | – | A |
| 16 | retention under loss | P1: 5% loss / unshaped | ✅ **86.9%** | 43.1% | ❌ 4.8% | ✗ † | – | A |
| 17 | retention under loss | P4: 5% loss / unshaped | ✅ **91.6%** | 38.9% | ❌ 1.6% | ✗ † | – | T |
| | **[CPU](#cpu)** (cpu_s / Msample) | | | | |  | – | |
| 18 | throughput, client | P1 76 B | ✅ **5.7** | ❌ 24.3 | 7.8 | 2.57 † | – | A |
| 19 | throughput, server | P1 76 B | ✅ **2.7** | ❌ 27.0 | 12.9 | 1.42 † | – | A |
| 20 | throughput, client | P2 1292 B | ✅ **6.7** | ❌ 20.8 | 9.3 | ✗ † | – | A |
| 21 | throughput, server | P2 1292 B | ✅ **3.8** | ❌ 21.0 | 14.6 | ✗ † | – | A |
| 22 | throughput, client | P4 2800 B | ✅ **12.6** | ❌ 46.7 | 14.3 | ✗ † | – | T |
| 23 | throughput, server | P4 2800 B | ✅ **7.6** | ❌ 35.7 | 20.1 | ✗ † | – | T |
| 24 | latency | P1 76 B | ✅ **34.3** | ❌ 102 | 52.9 | 121.1 | – | A |
| 25 | latency | P2 1292 B | ✅ **35.0** | ❌ 103 | 54.9 | 144.6 | – | A |
| 26 | throughput, client | P1, 5% loss | ✅ **6.6** | ❌ 28.1 | 13.2 | ✗ † | – | A |
| 27 | throughput, client | P4, 5% loss | ✅ **15.2** | ❌ 66.9 | 40.2 | ✗ † | – | T |
| | **[Memory](#memory)** (peak RSS, KB) | | | | |  | – | |
| 28 | throughput, client | P1 76 B | ✅ **2,087** | ❌ 15,947 | 5,407 | 2,127 † | – | A |
| 29 | throughput, client | P2 1292 B | ✅ **2,371** | ❌ 15,100 | 6,373 | ✗ † | – | A |
| 30 | throughput, client | P4 2800 B | ✅ **2,425** | ❌ 14,013 | 5,540 | ✗ † | – | T |
| 31 | latency, client | P1 76 B | ✅ **1,813** | ❌ 14,379 | 4,916 | 2,135 | – | A |
| 32 | throughput, server | P1 76 B | ✅ **2,057** | ❌ 14,756 | 6,991 | 2,127 † | – | A |
| 33 | throughput, server | P4 2800 B | ✅ **1,987** | ❌ 14,264 | 4,948 | ✗ † | – | T |
| 34 | throughput, server | P4, 5% loss | ✅ **2,968** | ❌ 14,351 | 6,011 | ✗ † | – | T |
| | **[Bandwidth](#bandwidth)** (wire B / sample) | | | | |  | – | |
| 35 | wire bytes | P1 76 B | ✅ **138** | ❌ 312 | 183 | 93.6 † | – | A |
| 36 | wire bytes | P2 1292 B | ✅ **1,355** | ❌ 1,503 | 1,397 | ✗ † | – | A |
| 37 | wire bytes | P3 1424 B | ✅ **1,487** | ❌ 1,866 | 1,585 | ✗ † | – | T |
| 38 | wire bytes | P4 2800 B | ✅ **2,920** | ❌ 3,461 | 2,950 | ✗ † | – | T |
| 39 | wire bytes | P1, 5% loss | ✅ **143** | ❌ 292 | 193 | ✗ † | – | A |
| 40 | wire bytes | P4, 5% loss | ✅ **3,057** | ❌ 4,223 | 3,213 | ✗ † | – | T |
| 41 | framing overhead | single datagram | ✅ **62.0** | ❌ 236.0 | 107.0 | 63.0 § | – | A |
| | **[QoS mechanics](#qos-mechanics)** | | | | |  | – | |
| 42 | DURABILITY late join | durable / volatile | 20/20, 0/20 | 20/20 | 20/20 | – | – | S |
| 43 | HISTORY, within depth | burst | 160/160 | 160/160 | 160/160 | – | – | S |
| 44 | HISTORY, beyond depth | burst | ✅ **147.7/160** | ❌ 109/160 | ❌ 109/160 | – | – | S |
| 45 | DEADLINE detection | 50 ms | ⚪ works | ⚪ works | ⚪ works | – | ✗ no event ¶ | S |
| 46 | LIVELINESS detection | lease 2.0 s | ⚪ 2000.1 ms | ⚪ 1999.1 ms | ⚪ 2000.1 ms | – | ✗ no event ¶ | L |
| 47 | LIFESPAN expiry | 100 ms | ⚪ works | ⚪ works | ⚪ works | – | – | S |
| | **[rmw layer](#rmw-layer)** (ms, cross-host) | | | | |  | – | |
| 48 | RTT mean, block wait, BEST_EFFORT | Bench (64 B) | ✅ **0.246** | 0.320 | 0.269 | – | ❌ 0.381 | W |
| 49 | RTT mean, block wait, RELIABLE | Bench (64 B) | ✅ **0.247** | 0.335 | 0.269 | – | ❌ 0.386 | W |
| 50 | RTT mean, block wait, BEST_EFFORT | Array1k | ✅ **0.263** | 0.338 | 0.294 | – | ❌ 0.611 | W |
| 51 | RTT mean, block wait, RELIABLE | Array1k | ✅ **0.269** | 0.354 | 0.284 | – | ❌ 0.609 | W |
| 52 | RTT mean, busy poll (no sleep), BEST_EFFORT | Bench (64 B) | ✅ **0.241** | 0.315 | 0.268 | – | ❌ 0.382 | J |
| 53 | RTT mean, busy poll (no sleep), RELIABLE | Bench (64 B) | ✅ **0.241** | 0.327 | 0.272 | – | ❌ 0.382 | J |
| 54 | RTT mean, busy poll (no sleep), BEST_EFFORT | Array1k | ✅ **0.264** | 0.338 | 0.290 | – | ❌ 0.613 | J |
| 55 | RTT mean, busy poll (no sleep), RELIABLE | Array1k | ✅ **0.265** | 0.350 | 0.280 | – | ❌ 0.610 | J |
| 56 | RTT mean, poll wait, 50 us sleep, random phase, BEST_EFFORT | Bench (64 B) | ✅ **0.294** | 0.354 | 0.307 | – | ❌ 0.424 | J |
| 57 | RTT mean, poll wait, 50 us sleep, random phase, RELIABLE | Bench (64 B) | ✅ **0.297** | 0.359 | 0.311 | – | ❌ 0.421 | J |
| 58 | RTT mean, poll wait, 50 us sleep, random phase, BEST_EFFORT | Array1k | ✅ **0.315** | 0.369 | 0.329 | – | ❌ 0.650 | J |
| 59 | RTT mean, poll wait, 50 us sleep, random phase, RELIABLE | Array1k | ⚪ 0.314 | 0.382 | ⚪ 0.318 | – | ❌ 0.647 | J |
| 60 | RTT mean, poll wait, 100 us sleep, random phase, BEST_EFFORT | Bench (64 B) | ✅ **0.318** | 0.367 | 0.330 | – | ❌ 0.455 | J |
| 61 | RTT mean, poll wait, 100 us sleep, random phase, RELIABLE | Bench (64 B) | ✅ **0.322** | 0.384 | 0.327 | – | ❌ 0.444 | J |
| 62 | RTT mean, poll wait, 100 us sleep, random phase, BEST_EFFORT | Array1k | ⚪ 0.345 | 0.396 | ⚪ 0.351 | – | ❌ 0.672 | J |
| 63 | RTT mean, poll wait, 100 us sleep, random phase, RELIABLE | Array1k | ⚪ 0.343 | 0.406 | ⚪ 0.343 | – | ❌ 0.671 | J |
| 64 | RTT mean, poll wait, 200 us sleep, random phase, BEST_EFFORT | Bench (64 B) | ⚪ 0.358 | 0.410 | ⚪ 0.365 | – | ❌ 0.499 | J |
| 65 | RTT mean, poll wait, 200 us sleep, random phase, RELIABLE | Bench (64 B) | ⚪ 0.369 | 0.425 | ⚪ 0.373 | – | ❌ 0.504 | J |
| 66 | RTT mean, poll wait, 200 us sleep, random phase, BEST_EFFORT | Array1k | ⚪ 0.387 | 0.443 | ⚪ 0.403 | – | ❌ 0.736 | J |
| 67 | RTT mean, poll wait, 200 us sleep, random phase, RELIABLE | Array1k | ⚪ 0.391 | 0.444 | ⚪ 0.380 | – | ❌ 0.726 | J |
| 68 | peak RSS, ping process (KB) | Bench, block, BEST_EFFORT | ✅ **12,544** | 23,748 | 14,592 | – | ❌ 70,072 | W |
| 69 | peak RSS, pong process (KB) | Bench, block, BEST_EFFORT | ✅ **13,216** | 23,632 | 14,532 | – | ❌ 70,484 | W |
| 70 | pong CPU, whole run (ms) | Bench, block, BEST_EFFORT | ✅ **37.4** | 57.7 | 43.5 | – | ❌ 91.8 | W |
| 71 | pong CPU, whole run (ms) | Bench, block, RELIABLE | ✅ **37.2** | 60.4 | 48.5 | – | ❌ 94.1 | W |
| 72 | delivered msg/s, KEEP_ALL, max rate | Array1k, 0% loss | ✅ **90,850** | ❌ 30,919 | 84,446 | – | – | K |
| 73 | delivered msg/s, KEEP_ALL, max rate | Array1k, 5% loss | ✅ **74,129** | ✗ refused | ❌ 1,808 | – | – | Ke |
| 74 | delivered msg/s, KEEP_ALL, max rate | Array4k, 0% loss | ✅ ‡ **27,434** | ✅ ‡ 27,917 | ✅ ‡ 27,204 | – | – | K |
| 75 | delivered msg/s, KEEP_ALL, max rate | Array4k, 5% loss | ✅ **26,897** | ✗ refused | ❌ 347 | – | – | Ke |

### Legend

- **⚪** deliberately not scored: a fair comparison is not possible in that row (5, 5c, 45-47), or, in rows 52-67 and
  the first two are within each other's spread (a draw).
- **zenoh-pico is reference and never scored**: no ✅/❌ toward totals, no WIN/DRAW/LOSE. Its cells come from its own
  session (`Z`, `results/zenoh_cells_31d58011_2026-09-29.txt`), so its margins are looser than a within-row one.
- **`†`** measured over TCP, the only configuration where zenoh-pico's reliability is real (its RELIABLE means
  monotonic sequence numbers, not retransmission). Untagged zenoh-pico cells are its UDP-multicast best-effort arm.
- **`✗`** in rows 73 and 75: the vendor's publisher ended every run (Fast DDS's KEEP_ALL write timed out; see the rmw
  layer notes). With a normal configuration it does not run these cells.
- **`✗`** elsewhere: measured, and the transport did not survive the cell: zenoh-pico's TCP session dies a few hundred samples
  into a max-rate run above 76 B while its publisher keeps reporting success. Never read it as a figure.
- **`‡`** both at the ~940 Mbps link ceiling and inseparable (inside the ~1% floor between builds); both carry ✅.
- **`§`** derived from a measured row: row 41 is row 35 minus the 76 B payload.
- **`¶`** rmw_zenoh_cpp accepts the DEADLINE and LIVELINESS policies but refuses the deadline-missed and
  liveliness-lost events (`UnsupportedEventTypeError`, `experiments/rmw_qos_support_probe.{py,sh}`, jazzy, both DDS
  rmws as controls). Rows 45-46 measure an event, so they cannot be measured for it. Rows 42-44 and 47 are `–`.
- **`–`** not measured for that implementation.

### Where the rows come from (`meas.`)

Within a row all scored frameworks come from one session; across letters they do not.

| meas. | source |
|---|---|
| `A` | aligned 17-cell native campaign (`campaign_sweep.sh`), 3 reps, FastDDS as shipped, no pinning, frameworks' order rotated, every framework dropping the same warm-up and cool-down (TESTING.md section 5 item 9); `09a0437d`, 2026-10-08 (`experiments/fair_crosshost_remeasure.sh`; `~/rig_results_safe/fair_crosshost_09a0437d_20261008-112506.A_free.txt`, rows from `fair_crosshost_rows.py`). Until 2026-10-08 these rows were the same campaign at `3ae721aa`, 2026-10-06, without the warm-up (`fair_crosshost_3ae721aa_20261006-080418.A_free.txt`), with rows 1, 3, 5, 6, 6a, 6b from the warm-up re-runs at `f40a2adf` and `9b5491e6`; until 2026-10-06 the pinned 12-cell campaign on `9dbffd40` (`results/cmp_A_9dbffd40_2026-09-29.txt`) and, for 6a-6b and 11a-11c, `G` |
| `C` | creation-time latency, the five latency cells of `A` (c10, c11, c12, c16, c17) again with `campaign_sweep.sh`, 3 reps, FastDDS as shipped, no pinning, TickLE with gate v2's layout-stable flags; main `596e9c9c` (with the scheduler wait ending at its deadline), 2026-10-17 (`~/rig_results_safe/creation_latency_core_20261017-204550.txt`, read by `creation_latency_summary.py --rtt` at `596e9c9c`; first run at `4fca013b`, before that fix; [Latency from creation time](#latency-from-creation-time-2026-10-17)) |
| `T` | P3/P4 cells with FastDDS `maxMessageSize` 1472 (fair-evaluation setting), same session and method as `A` (`...112506.T_free.txt`). Cell 6 (rows 14, 17, 27, 34, 40) was not in the `09a0437d` run and is still `3ae721aa` without the warm-up (`fair_crosshost_3ae721aa_T6.T_free.txt`; row 17's unshaped base is that session's cell 4, 944 / 555 / 871 Mbps) |
| `G` | p1-p4 gap cells added 2026-09-30 (`results/cmp_p1p4_gap_99033118_2026-09-30.txt`, WIN 38 / DRAW 4 / LOSE 0) |
| `S` | QoS-mechanics sweep 2026-09-24/25 (`experiments/comparison_resweep.sh` at `659013e9`) |
| `L` | liveliness, `liveliness_l2.sh`, 2026-09-27, `0f220ba0`, 20 reps (`results/liveliness_l2e_2026-09-27.txt`) |
| `W` | four-way rmw block wait, `934f90de`, 3 reps (`results/rmw_4way_block_934f90de_2026-09-30.txt`) |
| `J` | four-way rmw poll wait, random phase, RTT at callback, `934f90de`, 7 reps (`results/rmw_4way_poll*_cb_r7_*_2026-10-01.txt`) |
| `Ke` | rmw RELIABLE + KEEP_ALL at equal KEEP_ALL bounds, as `K` but 5 reps, frameworks interleaved, 5% loss only, main `29dff630`, 2026-10-08 (`experiments/rmw_keepall_rig.sh` `EQUAL_BOUND=auto`; `~/rig_results_safe/rmw_keepall_equal_main_29dff630_20261008-134901.{txt,runs,launch.log}`). Until 2026-10-08 rows 73 and 75 were `K`: 70,127 / 1,784 and 26,486 / 340, each rmw at its own default bound |
| `K` | rmw RELIABLE + KEEP_ALL, apex `perf_test` at max rate across the link, 20 s, 3 reps, medians, `ae724688`, rmw_tickle built as the vendor debs are (CMake type None, `-g -O2`), 2026-10-06 (`experiments/rmw_keepall_rig.sh`; `~/rig_results_safe/rmw_keepall_rig_O2_20261006-123017.txt`). First measured at `a8c1cd83` with rmw_tickle at `-O3`: 87,977 / 65,051 / 27,421 / 26,483 |

### Re-measure at `09a0437d`

Every `A` and `T` row except `T` cell 6 comes from `experiments/fair_crosshost_remeasure.sh` at `09a0437d`
(2026-10-08; `~/rig_results_safe/fair_crosshost_09a0437d_20261008-112506.{A_free,A_pinned,T_free,T_pinned}.txt`,
driver log `...112506.driver.log`), the driver of the `3ae721aa` run, now with the equal warm-up and cool-down
(TESTING.md section 5 item 9): throughput is scored over the window (`send_mbps` between 2 s edges; the whole-run rate
is printed beside it and not scored; since 2026-10-17 goodput, the server's delivered rate in the same window, see
"Throughput"), latency over 100 measured round trips between 4,096-round-trip edges. Samples
before values: `A_free` has 153 RESULT lines, 151 `ok` and `window=ok`, every edge ended on its count except cell 12's
(10 ms netem; both on the 20 s bound, 91 measured round trips per rep, every framework alike). The two VOIDs are FastDDS
as shipped at P4 under 5% loss (cell 6, reps 2-3): its write timed out 86 times at the 100 ms blocking bound and the
run was shorter than its two edges, so cell 6's FastDDS figures rest on one rep and the summary marks them provisional.
`A_pinned` has the same shape (its VOIDs are the same cell's reps 1-2); `T_free` and `T_pinned` are 18 of 18 `ok`.

The totals, and how TickLE moved against the vendors since `3ae721aa`, are in [Corrections and
history](#cross-host-the-09a0437d-re-measure).

### Latency from creation time (2026-10-17)

The user decided on 2026-10-17 that latency is measured from the data's creation time: the time each ping or sample
was due (TESTING.md section 4). The send-stamp figures (rows 1-6b, below) stay as diagnostics. Harness `4fca013b`:
the core `reliable_latency` clients print `crt_*` (RTT from each ping's due time) beside `rtt_*` (RTT from the send
stamp), and perf_test writes a creation trace in the rmw cells. The queue was
`experiments/rig_queue_creation_latency.sh`, run 09:49-11:59 KST. It took the rig lock once for every cell
(`~/rig_results_safe/creation_latency_queue.log`). TickLE was built with the layout-stable flags of gate v2
(`TICKLE_EXTRA_CFLAGS`, TESTING.md section 6.1). FastDDS ran as shipped, and nothing was pinned.

**Rows 1c-6bc are from the second run, on main `596e9c9c` (2026-10-17 20:45-22:55,
`~/rig_results_safe/creation_latency_queue2.log`).** That main carries the fix described below (a wait for a scheduler
entry ends at its deadline, `timerfd`; DESIGN.md section 2), landed after its gate-v2 rig A/B passed
(`~/rig_results_safe/queue_core_ab_swt.log`, 0 CPU or latency rows WORSE; TickLE's gap 112 -> 13 us, creation RTT
-98 us). Same queue, harness, flags and reps as the first run; every rep `window=ok`, 100 measured round trips (91 at
+10 ms netem; CycloneDDS at P3 100 / 100 / 99); the control passed (rmw ping-pong and core R4). Reader
`creation_latency_summary.py --rtt` at `596e9c9c` on `~/rig_results_safe/creation_latency_core_20261017-204550.txt`.

| row | cell | TickLE creation p50 | FastDDS | CycloneDDS | stamp p50, T / F / C | g us, T / F / C | vs FastDDS | vs CycloneDDS |
|---|---|---:|---:|---:|---:|---:|:-:|:-:|
| 1c | c10 P1 76 B | 0.211 [0.211-0.214] | 0.346 [0.344-0.350] | 0.312 [0.311-0.314] | 0.200 / 0.284 / 0.254 | 11.2 / 61.3 / 58.1 | WIN | WIN |
| 3c | c11 P2 1292 B | 0.237 [0.237-0.238] | 0.378 [0.363-0.380] | 0.328 [0.324-0.329] | 0.225 / 0.316 / 0.270 | 11.3 / 61.1 / 58.0 | WIN | WIN |
| 5c | c12 P1, +10 ms netem | 10.445 [9.960-10.465] | 10.239 [10.229-10.470] | 10.257 [10.036-10.463] | 10.432 / 10.179 / 10.195 | 10.7 / 60.7 / 60.5 | DRAW | DRAW |
| 6ac | c16 P3 1424 B | 0.241 [0.238-0.242] | 0.391 [0.388-0.407] | 0.337 [0.337-0.344] | 0.229 / 0.331 / 0.280 | 11.4 / 61.4 / 58.2 | WIN | WIN |
| 6bc | c17 P4 2800 B | 0.259 [0.255-0.262] | 0.403 [0.402-0.413] | 0.347 [0.340-0.349] | 0.248 / 0.342 / 0.288 | 11.7 / 61.2 / 58.4 | WIN | WIN |

- **The reading.** From creation time TickLE now beats both vendors at P1-P4 (by 90-101 us against CycloneDDS) and
  draws at +10 ms netem; its ping leaves 11-12 us after its due time, the DDS clients' 58-61 us. Campaign `A` is
  WIN 139, DRAW/TIE 11, LOSE 0 of 150 again. The I4 cells re-measured in the same queue agree with the first run
  (`creation_latency_{i4s,i4be,i4rel}_20261017-204550.creation.txt`: I4s TickLE 55.55-55.72 ms against 1,842-7,448;
  I4 BE and RELIABLE WIN on median and p99).

**The first run, on `4fca013b` (before the wait fix).** Its table and reading follow; they were the published rows
1c-6bc until the second run.

**Samples first.** Each cell has 3 reps per framework. Each rep has 100 measured round trips between 4,096-round-trip
edges; the exceptions are +10 ms netem (91, both edges ending on the 20 s bound) and CycloneDDS at P3 (100 / 99 / 99).
Every rep is `window=ok` and `crt_no_due=0`. With 100 round trips a p50 is read, and no p99 is scored. The reader is
`experiments/creation_latency_summary.py --rtt` at `228da18d`, on
`~/rig_results_safe/creation_latency_core_20261017-094944.txt`. Figures are medians of the reps, in ms, with the reps'
range in brackets. `g` is the median delay from each ping's due time to its send.

| row | cell | TickLE creation p50 | FastDDS | CycloneDDS | stamp p50, T / F / C | g us, T / F / C | vs FastDDS | vs CycloneDDS |
|---|---|---:|---:|---:|---:|---:|:-:|:-:|
| 1c | c10 P1 76 B | 0.315 [0.312-0.315] | 0.345 [0.342-0.351] | 0.311 [0.309-0.311] | 0.204 / 0.283 / 0.251 | 111.4 / 62.5 / 59.3 | WIN | LOSE |
| 3c | c11 P2 1292 B | 0.338 [0.337-0.338] | 0.366 [0.364-0.370] | 0.326 [0.325-0.331] | 0.227 / 0.306 / 0.267 | 111.3 / 62.3 / 59.4 | WIN | LOSE |
| 5c | c12 P1, +10 ms netem | 10.362 [10.288-10.458] | 10.438 [10.401-10.471] | 10.515 [10.130-10.564] | 10.250 / 10.376 / 10.455 | 110.9 / 62.5 / 60.9 | DRAW | DRAW |
| 6ac | c16 P3 1424 B | 0.337 [0.337-0.339] | 0.392 [0.389-0.399] | 0.341 [0.339-0.347] | 0.227 / 0.329 / 0.282 | 111.1 / 62.4 / 59.4 | WIN | DRAW |
| 6bc | c17 P4 2800 B | 0.355 [0.350-0.356] | 0.417 [0.401-0.432] | 0.341 [0.339-0.349] | 0.245 / 0.358 / 0.283 | 111.1 / 62.4 / 59.3 | WIN | LOSE |

- **The reading.** From creation time, TickLE loses to CycloneDDS at P1, P2 and P4 (by 4, 12 and 14 us at the median)
  and draws at P3. It beats FastDDS in every cell except +10 ms netem, which is a draw. From the send stamp in the
  same run, TickLE wins every cell against both vendors except +10 ms netem.
- **The cause is when TickLE's client sends.** It sends each ping about 111 us after the due time, and the DDS clients
  send after 59-62 us. That ~50 us difference is larger than TickLE's stamp lead over CycloneDDS at P1, P2 and P4
  (38-47 us), and about the size of it at P3 (55 us). TickLE's scheduler waits with a relative epoll/ppoll timeout,
  which the kernel lets end up to 0.1% of the timeout late. A fix on branch `ab/sched-wake-deadline` makes a wait
  longer than 10 ms end at its deadline. A gate-v2 rig A/B compared main `cf815dbb` with the branch, TickLE against
  TickLE, 6 reps per arm (`~/rig_results_safe/core_ab/coreab_swd/coreab_swd_{A,B}.txt`,
  `~/rig_results_safe/queue_core_ab_swd.log`). The fix moved the gap from 111 to 59.4-59.7 us and creation RTT p50 by
  -49 to -54 us at P1-P4; stamp RTT did not move. It is **not landed**: the same A/B read P4 client CPU per round trip
  +1.07% (WORSE), and that cost is under review. The rows above are the measured LOSE as it stands.
- **The control passed** (rule R4: where nothing waits at the sender, creation and stamp differ only by the sender's
  timer wake, `|d - g| <= noise`). This is the core RTT reading, all five cells, every framework. **The rule was changed
  after this run, with the user's agreement** (`228da18d`): the core reading now allows `|d - g| <= noise + 1 us`,
  because its `d` is a difference of two medians that the clients print to 1 us. At the run, c16 and c17 TickLE read
  `|d - g|` = 1.1 us against a noise of 1.0 us, and the reader then printed RTT CONTROL FAIL
  (`...094944.txt.creation.txt`). Re-read at `228da18d` it prints RTT CONTROL PASS, and no figure or verdict moves.
- **The rmw ping-pong control** (perf_test Array1k RELIABLE KEEP_LAST 10 at 100 Hz, one Pi to the other, 3 reps of
  30 s, 2,531-2,534 round trips per rep; `~/rig_results_safe/creation_latency_ctl_20261017-094944.{txt,runs,creation.txt}`)
  is not a scored row. RTT median from creation, rmw_tickle / FastDDS / CycloneDDS: 0.32 / 0.42 / 0.35 ms. From the
  stamp: 0.26 / 0.36 / 0.29 ms. `g` is 59.4 / 62.6 / 60.4 us, and the verdict is CONTROL PASS. There the sender is
  perf_test's own timer, so rmw_tickle sends as promptly as the DDS rmws.
- **Not yet measured from creation time:** the tails (rows 2, 4, 6), the same-host RTT rows (S7-S10, S12, S18), the
  rmw RTT rows (48-67, R1-R8) and the Array1m cells of the L4 table. Their figures are from the send stamp.

### Latency

Rows 1-6b, from the send stamp (diagnostic since 2026-10-17; the creation-time rows are above). They are from the
`09a0437d` campaign: 100 measured round trips per rep (91 in cell 12), every rep
`window=ok`, `reliable_latency` with one ping in flight, the mean of 3 reps (`campaign_summary.py`; this text said
median until 2026-10-08, but the figures were always its mean); tails (rows 2, 4, 6) are the median of the reps'
maxima, each rep's maximum over its measured round trips only. p50 / p99 ms, TickLE, FastDDS, CycloneDDS: P1 0.204 /
0.227, 0.288 / 0.341, 0.253 / 0.288; P2 0.228 / 0.264, 0.311 / 0.345, 0.272 / 0.312; +10 ms 10.00 / 12.20, 10.38 /
12.26, 10.03 / 12.24; P3 0.231 / 0.253, 0.334 / 0.373, 0.285 / 0.325; P4 0.247 / 0.271, 0.356 / 0.396, 0.289 / 0.334.
Every tail is a WIN outside the reps' ranges (maxima, TickLE / CycloneDDS / FastDDS: P1 TickLE 0.241-0.263, CycloneDDS 0.288-0.308, FastDDS
0.327-0.355; P2 0.258-0.308, 0.312-0.332, 0.345-0.367; +10 ms 12.18-12.20, 12.23-12.26, 12.24-12.28). Row 5 stays
unscored (10 ms netem swamps a 0.2 ms RTT, and TickLE's 10.071-10.182 overlaps CycloneDDS's 10.175-10.196).

All absolute RTTs were taken under the `ondemand` CPU governor, which adds about 10% (19-21 us); ordering and margins
are unaffected because all frameworks ran under it in the same sessions. zenoh-pico's column here is its TCP arm.

### Throughput

Rows 7-17. **Goodput** since 2026-10-17 (TESTING.md section 4): the complete samples the server's application took
inside the window between the 2 s edges, the mean of 3 (the text said median until 2026-10-08; the figures were
always `campaign_summary.py`'s mean). Read by `experiments/goodput_rescore.py` on the `A`, `T` and `T6` files above,
which puts the server's windowed `win_recv_mbps` where the client's send rate was (G1-G6 in its header; the window
lengths agree in every rep, 4.995 s), then `fair_crosshost_rows.py` on the rewritten files. RELIABLE + KEEP_ALL has
`sent == recv` checked on every side.
11b and 11c are at the link ceiling (‡). The zenoh-pico column is its own session's figure and was not re-read.

The vendors' BEST_EFFORT and KEEP_LAST rows (11-12) were send rates until 2026-10-17
([Corrections and history](#cross-host-throughput)). FastDDS as shipped (`maxMessageSize` 65,500) at the `T` cells, for reference (`A` cells 3, 4, 6 at `09a0437d`): P3 465 Mbps and 1,677
B/sample; P4 734 Mbps, 3,102 B/sample and 14,932 KB; P4 under 5% loss its write timed out 85-86 times per run at the
100 ms blocking bound, two of three runs ended shorter than their edges (VOID), and the one left sent 3.3 Mbps,
because kernel IP reassembly fails under loss. (Earlier figures, pinned: 533 Mbps; 786 Mbps, 3,101 B/sample, 14,864
KB; 9.9 Mbps with 3,252 of 3,439 delivered.)

**Unpinned against pinned** (same session, `09a0437d`; the pinned figures put each process on cores 1-3, away from the
Pi's interrupt core). Pinned scores WIN 149, DRAW/TIE 13, LOSE 0 of 162 (WIN 137, DRAW/TIE 13 of 150 with throughput
counted once as goodput, `campaign_summary.py` at `e24894d3`; latency here still from the send stamp); the
verdicts that differ are `server.loss_pct` in cells 1, 5 and 9 (whether a vendor's range touched zero) and row 5's
draw, where pinned TickLE is no longer ahead of CycloneDDS (10.4 against 10.2 ms, ranges overlapping). Unpinning costs every
framework at P1, most of all the two DDS: RELIABLE P1 (row 7) pinned is TickLE 114, FastDDS 37.7, CycloneDDS 89.4 Mbps
(unpinned 0.94x, 0.75x, 0.58x); BEST_EFFORT P1 (row 11) 120, 33.0, 67.7 goodput (send rates 120, 45.3, 67.9, as published until
2026-10-17); KEEP_LAST (row 12) 118, 40.0, 87.2. P2-P4
TickLE is at the link either way and CycloneDDS moves by under 1% except at P3 (836 pinned against 716); FastDDS
RELIABLE P2-P4 is 664, 442, 635 pinned. Retention at P1 (row 16) pinned: 88.6%, 30.5%, 3.0%. CPU per sample at P1
(rows 18-19) pinned: 5.34 / 2.56, 19.5 / 19.4, 6.95 / 11.1. At `3ae721aa` (pinned WIN 135, DRAW/TIE 15) row 7 was
115, 41.1, 93.5 pinned, and row 17 pinned 92.2%, 34.0%, 1.6% (cell 6 was not re-run). Raw files:
`~/rig_results_safe/fair_crosshost_09a0437d_20261008-112506.{A,T}_pinned.txt`.

### RELIABLE under loss

Rows 13-17. `tc netem` on the client's egress, all three under the same RELIABLE + KEEP_ALL contract, every writer
waiting for acknowledgements at teardown, so net loss is 0 for all three and the difference is cost. Retention is
5%-loss throughput over unshaped. zenoh-pico is `✗` here: its best-effort arm keeps 98.2%/99.6% only because dropping
is free, so that figure must not stand beside a recovered one.

### CPU

Rows 18-27. `cpu_s_per_Msample`, mean of 3 (the text said median until 2026-10-08), per DELIVERED sample since
2026-10-17 (TESTING.md section 4; `goodput_rescore.py` rule G5). Every row here is a RELIABLE + KEEP_ALL or latency
cell that delivered all it sent, so none moved; per delivered sample the BEST_EFFORT cells' client figures (not table
rows) rose for the vendors, e.g. cell 8 FastDDS 13.6 -> 21.8, CycloneDDS 13.6 -> 14.7, TickLE unchanged at 5.35. Absolute CPU seconds are never a verdict: every cell runs for a fixed
duration, so a framework that sends more burns more. TickLE figures from before 2026-09-26 were an `-O0` build;
every row here asserts `core_build=release` on all sides.

### Memory

Rows 28-34. Peak RSS. Two qualifiers travel with these rows: (1) it is *resident* memory on demand-paged Linux; on a
target without demand paging TickLE's declared static storage is the real figure. (2) Every framework is given the
same KEEP_ALL bound in samples (`campaign_sweep.sh` common_args, derived from TickLE's 512 KiB budget: 405 / 368 /
187 samples at P2 / P3 / P4). FastDDS's ~14 MB is its baseline, not held history: its latency cell, which holds one
sample, peaks at 14,379 KB (row 31). (Until 2026-09-29 FastDDS held up to 4,000 samples, which inflated this margin.)

### Bandwidth

Rows 35-41. Wire bytes per sample from `/proc/net/dev`, so retransmissions and control traffic count; per delivered
sample since 2026-10-17, which moves no table row (each delivered all it sent; cell 8 FastDDS, not a row, 254 -> 404). Row 41's per
single-datagram framing includes the 42 B of Ethernet+IP+UDP.

### QoS mechanics

Rows 42-47. Count-based scenarios: late-joiner durability, a burst within and beyond a fixed HISTORY depth of 8,
50 ms DEADLINE, LIVELINESS lease (median detection time from the last received sample; all three match the lease
within 1 ms, hence no winner), 100 ms LIFESPAN. Row 44 is the one scored row: TickLE delivers 147.7 of 160 beyond
depth, both DDS 109 (deterministic, 3/3).

### No matched reader sends nothing (rig)

Since no-send-unmatched (DESIGN.md section 4) a Publisher with no matched reader puts no data on the wire. What that
buys and costs, on the rig 2026-10-16 with the native core bench of branch exp/nosend-benefit (`nosend_benefit_rig.sh`;
U cells 3 reps of 60 s, F cells 20 reps a loss rate): A = main b265e86f (before no-send) against B = branch
ab/no-send-unmatched; the writer on one Pi, on the other an observer context with no endpoint
(`~/rig_results_safe/nosend_benefit/20261016-020518/reading.txt`; U PASS, F NO REGRESSION, both pre-registered).

| cell, no subscriber | data packets on the wire, A -> B | writer CPU per sample | observer CPU |
|---|---:|---:|---:|
| p1 BEST_EFFORT 1 kHz | 59,961 -> 0 | 27.8 -> 8.5 us (-69%) | 873 -> 491 ms (-44%) |
| p1 RELIABLE 1 kHz | 59,963 -> 0 | 28.0 -> 8.6 us (-69%) | 885 -> 491 ms (-45%) |
| 1 MiB RELIABLE 30 Hz | 1,303,207 -> 0 | 4.12 -> 0.26 ms (-94%) | 5,680 -> 500 ms (-91%) |

- **The cost:** a new reader's time from its Subscriber to its first sample rose by a median +1.95 to +2.05 ms at p1
  (best effort and RELIABLE, 0% and 5% loss; 95% CIs +1.68 to +2.56), the writer now waiting for the match instead of
  broadcasting; the control, context start to first announce, moved -0.11 to +0.08 ms. At 1 MiB no change was resolved
  (CIs span 0). B's p90 of 266 ms at p1 RELIABLE 5% travels with its control's p90 (267 ms): a lost discovery announce.

### rmw layer

Rows 48-71 (72-75 below). `rmw_tickle`, `rmw_fastrtps_cpp`, `rmw_cyclonedds_cpp` and `rmw_zenoh_cpp` driven through `rclcpp` by one
unchanged ping/pong binary pair; `RMW_IMPLEMENTATION` alone selects the rmw, and every row checks both sides'
`/proc/PID/maps`. Block wait (`W`) is the `spin()` pattern; poll wait (`J`) loops on `spin_some()` with 0, 50, 100 or
200 us sleeps and a random pause so the reply lands at a random phase. `J` verdicts use overlap of the repetitions'
interquartile ranges; ⚪ is a draw. rmw_zenoh_cpp needs a Zenoh router, run on the pong host and included in its
figures. Memory is from outside each process; CPU is the pong's whole-run thread time (4 s idle plus 100 round
trips), not a per-message cost. rmw_tickle's RSS includes its shared-memory segment: row 68 rose from 11,264 to
12,544 kB when the segment was added to core.

**Rows 72-75 (rmw KEEP_ALL, `K`, `Ke`):** apex `perf_test` publisher alone on one Pi and subscriber alone on the other,
RELIABLE + KEEP_ALL, `-r 0`, `tc netem` loss on the publisher's egress, each arm identified by `/proc/PID/maps`.
rmw_tickle lost no sample in any of its 12 runs (`gap_evicted` 0, no give-up). Every rmw blocks a full KEEP_ALL
publisher for 100 ms by default (`max_blocking_time`; ROS 2 QoS has no field for it).

- **Rows 73 and 75 at equal KEEP_ALL bounds (`Ke`, 2026-10-08).** Each writer's KEEP_ALL bound is set to the same
  N samples (Array1k N = 492, Array4k N = 125): rmw_tickle `RMW_TICKLE_KEEP_ALL_BYTES` = (N + 1) x its per-sample
  footprint (524,552 B / 1,064 B; 525,924 B / 4,174 B), Fast DDS `max_samples` = N through its XML. CycloneDDS cannot
  be bounded in samples (rmw_cyclonedds never sets resource_limits), so it runs at its default, **not equal**, as
  recorded. Medians of 5 reps (range): rmw_tickle 74,129/s (71,324-76,950) and 26,897/s (26,851-27,076), CycloneDDS
  1,808/s (1,728-2,023) and 347/s (323-368). CPU per sample, publisher / subscriber, median: rmw_tickle 13.9 / 11.5 us
  (Array1k) and 31.7 / 24.1 us (Array4k), CycloneDDS 26.5 / 44.0 and 70.1 / 176.4 us. rmw_tickle: no loss and no
  publisher error in any of its 10 runs. Fast DDS's publisher ended on "cannot publish data" in all 10 runs; the
  harness counts 2 Array1k runs as refused and voids the other 8 because the publisher was gone before 2 s, when its
  treatment is read from `/proc` (the launcher's environment shows the profile was set). It does not run these cells.
- **"Refused" (FastDDS at 5% loss, all runs) means:** Fast DDS's `DataWriter::write()` returned `RETCODE_TIMEOUT`.
  Its KEEP_ALL history (default `max_samples` 5,000) was full, the oldest sample was not yet acknowledged by the
  reader, and 100 ms passed. One lost sample holds the whole 5,000-sample window, and when a repair and the
  heartbeat riding on it are both lost, the next heartbeat comes from Fast DDS's 3 s periodic timer, 30 times the
  100 ms budget. `rmw_fastrtps_cpp` turns the timeout into `RMW_RET_ERROR` ("cannot publish data"), rclcpp into an
  `RCLError` exception, and `perf_test` does not catch it, so the run ends (after 1-6 s). Nothing was delivered
  wrongly; the writer gave up waiting, as KEEP_ALL with a 100 ms bound allows.
- **Fast DDS tuned through its own XML** (`experiments/fastdds_keepall_arms.sh`, `6bfda159`, 2026-10-06, 3 reps per
  arm, CycloneDDS as the control; `~/rig_results_safe/fastdds_keepall_arms_20261006-104033.txt`). The heartbeat
  explanation above was tested and **falsified**: with `max_blocking_time` 5 s (F1) and also with a 50 ms
  `heartbeatPeriod` (F2), every 5%-loss run still ended - after 4-7 s in which the writer made no progress at all, so
  its oldest sample stayed unacknowledged for over 5 s however often it asked. Only a 50,000-sample writer history
  (F3) kept the runs alive, because it never filled in 20 s: it then delivered **760 msg/s at Array1k (9.0 s
  latency) and 109 msg/s at Array4k**, against CycloneDDS's 1,949 and 346 in the same session and rmw_tickle's
  65,051 and 26,483 at `a8c1cd83` (rows 73, 75 now read 74,129 and 26,897 at equal bounds). So with a normal configuration Fast
  DDS does not run this cell: under 5% loss at max rate its publisher ends, and the one setting that keeps it alive
  delivers 760 / 109 msg/s. That is recorded as the result; the cause inside Fast DDS is not pursued.
- rmw_tickle is built at the vendors' level (`-g -O2`, no `NDEBUG`) and lost no sample in any of its 12 runs. At 0%
  loss one of FastDDS's three Array1k runs also ended on a write timeout; row 72 is the median of the other two.

### rmw large messages (L4, cross-host)

Large-message stage 2 step A (DESIGN.md section 8): samples above 65,507 B, sent in TickLE's own 1,472-byte
fragments. apex `perf_test` publisher on one Pi 5, subscriber on the other, 5 reps of 60 s per arm, arms interleaved,
medians. rmw_tickle is the large-message step A build as measured: branch ab/largemsg-a2 at dc056907 (the head sha is in
each file's header); it landed from ab/largemsg-land a5747a85, which adds the claimed-slot and lending interaction
fixes and was not re-measured here. Built `-g -O2` on both Pis as the vendors; each arm identified by
`/proc/PID/maps`. Latency is one-way, corrected by a clock probe
before and after every run; p99 is a lower bound from the per-second maxima. CPU is ms per MB delivered, subscriber and
publisher (since 2026-10-17, TESTING.md section 4; the publisher's was per MB published until then); RSS is the subscriber's peak. Read by `experiments/largemsg_rig_summary.py` (whole sample
counts, since `perf_test`'s rate columns are floor(count / D)) from
`~/rig_results_safe/largemsgA4_{s2_rel1m_20261010-125430,s3a_20261010-140102,s3b_20261010-145247,s3c_20261010-152057}.txt`
and their `.runs/`. Letters: rmw_tickle against Fast DDS as shipped / Fast DDS with `maxMessageSize` 1472 / CycloneDDS
(W win, L lose, D draw by the range rule; – not scored).

Every row was re-measured on 2026-10-15 on main f5fcda06: the hybrid transport (section 8 of DESIGN.md; "Hybrid
transport (large samples), rig" below) with the receive work of rx-gap3/3b/3c, the hot socket calls made without
glibc, and **UDP checksum 0 on IP-fragmented datagrams, the default since that day** (`TT_UDP_CSUM0=0` restores the
checksum; DESIGN.md section 8, "UDP checksum 0"). All four arms in every cell, Fast DDS at 1472 included. Read by the
same `largemsg_rig_summary.py` from `~/rig_results_safe/l4main2_{i1rel,i1be,i4rel,i4be,i4s}_20261015-094605.txt` and
their `.runs/`; letters per vendor by the same range rule. The previous rows (f1f2f14d, 2026-10-12, and I1L at step A
dc056907) are in git history; the same cells on main 0c9f9a3b (2026-10-13, `l4main_*_20261013-125140`) are the
before-figures quoted below. The two I1 RELIABLE rows (I1, I1L) were measured again on 2026-10-16 (06:55-07:50) on main
dde87857: first-n3 (68b637f2), no-send-unmatched (a Publisher with no matched reader sends nothing, DESIGN.md section 4)
with its O(1) matched check, and UDP checksum 0 as above; same four arms, 5 reps, same reader
(`~/rig_results_safe/l4main3_i1rel_20261016-065517.{txt,runs}`).

**Latency from creation time** (the user, 2026-10-17; TESTING.md section 4) was measured in the three Array4m cells on
main `4fca013b` (2026-10-17, harness `rmw_keepall_rig.sh CREATION_TRACE=1`, the same five arms, 5 reps of 60 s, arms
interleaved). The reader was `experiments/creation_latency_summary.py`, gated by its rmw ping-pong control (CONTROL PASS;
[Latency from creation time](#latency-from-creation-time-2026-10-17)). The files are
`~/rig_results_safe/creation_latency_{i4s,i4be,i4rel}_20261017-094944.{txt,runs,creation.txt}`. The `latency` letter is
the creation-median verdict where it was measured, and the send-stamp median verdict (¹) elsewhere. The send-stamp
columns are diagnostic. The other columns are from f5fcda06 and dde87857, as above.

| cell | QoS, rate | delivered % | latency from creation, med / p99 ms | latency from send stamp, med / p99 ms (diag.) | CPU sub / pub ms/MB | RSS kB | delivered | latency | latency, stamp (diag.) | CPU sub | CPU pub | RSS |
|---|---|---:|---:|---:|---:|---:|:-:|:-:|:-:|:-:|:-:|:-:|
| I1 Array1m | RELIABLE KL10, 30 Hz | 100 | not measured | 9.41 / 9.48 | 2.29 / 3.94 | 18,684 | D–D–D | D–W–W ¹ | D–W–W | D–W–W | D–W–W | W–W–W |
| I1L Array1m, 5% loss | RELIABLE KL10, 30 Hz | 100 | not measured | 10.04 / 15.75 | 4.17 / 3.65 | 18,792 | –D– | –W– ¹ | –W– | –W– | –W– | –W– |
| I1 Array1m | BEST_EFFORT KL5, 30 Hz | 100 | not measured | 9.24 / 9.31 | 2.20 / 3.89 | 17,724 | D–D–D | D–W–W ¹ | D–W–W | D–W–W | D–W–W | W–W–W |
| I4s Array4m (saturating) | BEST_EFFORT KL5, 30 Hz | 95.5 of offered | **55.83** / UNBOUND ² | 54.98 / 71.23 | 1.40 / 3.60 | 30,952 | W–W–W | **W–W–W** | L–L–L | D–W–W | D–W–W | W–W–W |
| I4 Array4m | RELIABLE KL10, 15 Hz | 100 | **36.68** / 36.77 | 36.57 / 36.64 | 2.21 / 3.69 | 30,656 | D–D–D | W–W–W | W–W–W | D–W–W | D–W–W | W–W–W |
| I4 Array4m | BEST_EFFORT KL5, 15 Hz | 100 | **36.60** / 36.66 | 36.52 / 36.60 | 2.19 / 3.72 | 30,580 | D–D–D | W–W–W | W–W–W | L–W–W | L–W–W | W–W–W |

¹ Not yet measured from creation time; this is the send-stamp verdict of the f5fcda06 / dde87857 run.
² UNBOUND for every arm, so a DRAW against every vendor. In a saturating cell every arm leaves some offered ticks
undelivered, and the p99 lands on one. It is not one of the 80 letters.

**L4 totals** (80 letters: delivered, latency, CPU sub, CPU pub and RSS per vendor): **WIN 55, DRAW 23, LOSE 2**. Was
WIN 52, DRAW 23, LOSE 5 with latency from the send stamp; the three I4s latency letters moved from LOSE to WIN.
This table's earlier corrections (publisher CPU per MB delivered, 2026-10-17; the 2026-10-15 re-measure) are in
[Corrections and history](#rmw-large-messages).

- **Creation-time latency, the three Array4m cells.** Sample counts come first. In the reader's window on scheduled
  times (55.0-55.1 s), each run had 1,650-1,652 offered ticks at 30 Hz and 826 at 15 Hz, which is enough for a median
  and for a p99 (rule R1). Medians of 5 reps, with the reps' range, TickLE / Fast DDS as shipped / Fast DDS at 1472 /
  CycloneDDS, in ms:
  - **I4s** (saturating), creation median: **55.83** [55.73-55.89] / 2,026 [1,954-2,054] / 7,599 [6,479-8,965] / 1,900
    [1,878-2,654]. WIN against all three. On the send stamp it is 54.91 / 37.55 / 41.61 / 38.93, which is LOSE against
    all three.
  - **I4 RELIABLE**, creation median 36.68 [36.61-36.72] / 37.59 [37.57-37.61] / 42.35 [42.28-46.26] / 38.73
    [38.72-41.42]. p99 36.77 [36.76-37.05] / 37.73 [37.70-37.74] / 45.28 [44.88-52.16] / 38.85 [38.83-42.43]. WIN
    against every vendor on both.
  - **I4 best effort**, creation median 36.60 [35.91-36.62] / 37.57 [37.53-37.61] / 41.50 [41.47-41.52] / 38.68
    [38.66-38.71]. p99 36.66 [36.40-36.69] / 37.72 [37.66-37.75] / 43.15 [42.90-43.43] / 38.84 [38.81-38.90]. WIN
    against every vendor on both.
  - **Why I4s turns over.** I4s offers more than the link carries. A DDS `publish()` of 4 MB blocks until the sample is
    in the socket, so a DDS publisher falls further behind every tick. Its stamp is written only after that wait, so
    the stamp hides it. From the schedule, the DDS arms' realized gap (stamp - schedule) has a median of 1.79-1.97 s
    (CycloneDDS, Fast DDS) and 4.75-4.81 s (Fast DDS at 1472). rmw_tickle's publish does not block; its gap is
    0.06 ms, and it drops at the sender the samples that wait behind the one in flight.
  - **The DDS figures depend on the run's length.** Each DDS arm's creation latency grows across the window: CycloneDDS
    +3.2 s, Fast DDS +3.5 s and Fast DDS at 1472 +9.7 s over 55 s, against rmw_tickle's +0.0 to +0.7 ms. A longer run
    gives larger DDS figures. The arms are compared at equal windows (within 1 s, rule R2), and every arm passed the
    grid check (rule R3).
  - **`tickle@block`** (`RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS`, off by default; see the I4s note below) reads 1,516-1,565 ms
    from creation, against 38.30-38.70 ms on the stamp. It blocks as the DDS publishers do, so it falls behind as they
    do (+2.6 s over the window). The option improves only the send-stamp figure, and the default stays off.
  - **The counts are not the scored delivered %.** This reader counts ticks scheduled inside its window. For I4s,
    delivered of offered per run is: rmw_tickle 1,575-1,576 of 1,651; Fast DDS 1,601-1,603 of 1,650-1,651; Fast DDS at
    1472 836-1,185; CycloneDDS 1,080-1,608. For a blocking publisher, the ticks it never published by the end are the
    ones scheduled in the last ~1.5 s of the window. The scored delivered % of offered stays the L4 run's
    whole-run figure.
- **I1L (main dde87857, 2026-10-16): TickLE delivered every sample in 5 of 5 runs** and wins every other metric
  against the one vendor arm that runs the cell completely, Fast DDS at 1472 (latency 58.30 / 83.48 ms against 10.04 /
  15.75, subscriber CPU 7.48 against 4.17 ms/MB). Fast DDS as shipped delivered nothing in all 5 runs (does not run),
  CycloneDDS 2.4-4.0% (incomplete); neither is scored. **Corrected in place:** on f5fcda06 (2026-10-15) this cell was a
  LOSE on incomplete delivery - 2 samples abandoned in 1 of 5 runs, because the writer, with no loss estimate from the
  reader yet, sent its first 4-5 samples as runs of n = 45 onto the 5% path. first-n3 (68b637f2; DESIGN.md section 8)
  fixed it: rig A/B `~/rig_results_safe/firstn3_20261016-005411` (12 reps at 5%; `n_hist` in the publisher logs), 0
  runs with any sample at n > 1 against 6 of 12 for the control, main + no-send (46554d68).
- **Delivered % under no-send.** `perf_test` counts every sample it published, including the one published before the
  reader matched; TickLE now keeps that sample unsent (`tx_unmatched_kept=1`, `tx_unmatched_dropped=0` in all 10
  publisher logs; 1,802 published, 1,801 reassembled). The reader (`largemsg_rig_summary.py`) takes the id gap before
  the first delivered sample as published before the match, not lost (Fast DDS's arms show 5 such samples a run), so
  TickLE reads 100.00% per published. The incomplete-delivery rule counts what the reader was owed after the match:
  lost 0 in every TickLE run of both cells.
- **I1 against its own 2026-10-15 figures:** latency 9.29 -> 9.41 ms, subscriber CPU 2.28 -> 2.29, publisher CPU 3.88
  -> 3.94 ms/MB, subscriber RSS 17,624 -> 18,684 kB. Latency and publisher CPU against Fast DDS as shipped went from
  WIN to DRAW: one TickLE rep (r2, 9.77 ms) lies inside Fast DDS's 9.51-9.97 ms, and publisher CPU's 3.85-4.08 overlaps
  its 3.99-4.16. Not attributed; the other usable reps read 9.34-9.43 ms.
- **Memory:** the subscriber is the smallest in every cell. The publisher is too in four of six; at 4 MB best effort
  CycloneDDS's publisher is smaller (28,628 and 28,668 kB against 29,312 and 33,408).
- **Where the 1.2 ms at 4 MB goes** (`experiments/largemsg_wire_budget.py` on the I4 rows, 2026-10-11): 0.52 ms is
  wire - one sample is 4,431,228 B of frames for TickLE (2,889 datagrams; per frame 8 B of UDP header and 16 B of
  framing that IP fragments do not carry) against 4,366,590 B for Fast DDS's 64 KB datagrams, 35.45 against 34.93 ms
  at 1 Gb/s; the UDP header is the price of sending in MTU units. The other 0.67-0.70 ms is off the wire: Fast DDS
  and CycloneDDS both spend 2.53-2.60 ms there, TickLE 3.23-3.24. That is one extra 4 MB copy: rmw_tickle decoded the
  sample into a shell in the delivery and copied it again in the take, where a DDS deserialises once. The fix lends a
  large sample where core assembled it (RMW.md, "Loaned messages"). Its rig A/B (`largemsg_lend_ab.py`, I4 5 reps,
  `largemsgLend_s1_20261011-100430.txt` RELIABLE, `largemsgLend_s2_20261011-103836.txt` best effort): median latency
  -0.88/-0.87 ms with an ordinary take, -2.26/-2.23 ms where rclcpp takes the loan (`ROS_DISABLE_LOANED_MESSAGES=0`),
  no WORSE; against Fast DDS as shipped +0.40/+0.36 ms ordinary, -0.98/-1.00 ms loaned. Measured on branch
  ab/largemsg-latency at ba324d3b against its parent 11cb21e8, both with the UDP offload commits of ab/udp-offload-v2;
  landed without UDP offload and not re-measured.
- **I4s, Array4m best effort at 30 Hz, saturates the link** (30 Hz against the link's ~28.6 Hz for 4 MB). Its
  delivered % is scored per offered tick (30 Hz x the publisher's 60 s window = 1,800 samples, the same for every
  framework), not per published sample. **Corrected 2026-10-12** (the user's decision): this row and step A's read
  delivered % per published, and a DDS `publish()` of 4 MB blocks until the sample is in the socket, so the DDS
  publishers published only what the link could carry (Fast DDS 1,698-1,699, CycloneDDS 1,702-1,703) and read ~100% of
  that, while rmw_tickle's `publish()` does not block, published all 1,802 and drops at the sender the samples that
  wait behind the one in flight (KEEP_LAST: 84 a run). Per published, the denominators differ; step A's row read 92.3%
  and LWL for that reason, and the f1f2f14d run read 95.49 against 100 and L–L. Per offered tick: rmw_tickle 95.3-95.4%
  (1,715-1,717 complete), Fast DDS 94.1-94.2%, CycloneDDS 94.4-94.5% - W–W
  (`experiments/largemsg_rig_summary.py`, offered_pct, selftest case 5 on these runs). Step A (dc056907) delivered
  1,655-1,679, below CycloneDDS's 1,699-1,700, and falsified F3; f1f2f14d's 1,715-1,717 holds it. Latency is lost
  (55.66 / 72.02 ms against Fast DDS 37.52 / 37.76): it includes the wait behind the sample in flight, which a DDS
  publisher spends blocked in `publish()` before it stamps the next sample. **Corrected 2026-10-17:** that is the
  send-stamp reading. Measured from creation time, the wait counts for every arm, and I4s latency is a WIN against all
  three (above). The subscriber's socket still drops 57-382
  datagrams a run. Since 2026-10-16 rmw_tickle can block the same way, off by default
  (`RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS`, RMW.md "Large samples: blocking publish"). Rig, 2026-10-16, branch
  ab/large-publish-block on main dde87857, I4s, 5 reps, arms interleaved (`~/rig_results_safe/block_i4s_20261016-102913.txt`
  and `.runs/`; each `tickle@block` run 1,718-1,719 waits, 0 timeouts, 0 samples dropped at the sender): with the
  option at 1000 ms, send-stamp latency median / p99 38.48 / 39.50 ms against 54.92 / 71.23 off, Fast DDS 37.60 / 37.81 and
  CycloneDDS 39.00 / 39.67 - a WIN against CycloneDDS, still a LOSE against Fast DDS by 0.3-0.9 ms (ranges
  38.45-38.55 against 37.56-38.17); delivered 95.44% of offered in both modes; publisher CPU 3.61 ms/MB (off 3.44,
  Fast DDS 3.58); publisher peak RSS 25.5 MB, the smallest of the four. The default stays off, so the row above is
  the non-blocking mode.
- **Same host is not published.** In step A a 4 MB best-effort sample delivers ~0% same-host: its ~2,900 fragments do
  not fit the receiver's 512 x 1,472 B ring. Step B, a writer-owned same-host area, is to fix it.

### Hybrid transport (large samples), rig

Section 8 of DESIGN.md: a large sample goes as IP-fragmented runs of n units (up to 45) while the path is clean, MTU
units on loss; n from the reader's measured loss. Array1m RELIABLE KL10 at 30 Hz, netem loss on the link, 5 reps of
60 s per arm, arms interleaved: AUTO, AUTO again (the A/A floor), forced MTU (n = 1) and forced IP (n = 45, cap off).
Measured as ab/hybrid-land d8980f9b on 4fbb8fe1, before the receive drain; landed rebased, not re-measured
(`~/rig_results_safe/hybridLand_s2_loss_20261012-021743.{txt,hybrid.txt}`).

| loss | AUTO wire B/unit vs MTU | AUTO p99 ms vs MTU | forced IP delivered | AUTO CPU sub / pub ms/MB |
|---|---|---|---|---|
| 0.01% | 1,475.8 vs 1,494.6 (WIN) | 9.54 vs 9.42 (LOSE) | 63.5% | 3.02 / 4.31 |
| 0.1% | 1,484.3 vs 1,494.7 (WIN) | 9.45 vs 9.43 (LOSE) | 71.0% | 4.32 / 4.44 |
| 1% | 1,502.0 vs 1,495.1 (LOSE) | 13.42 vs 10.16 (DRAW) | 100% | 4.93 / 4.58 |

- AUTO delivered 100% at every loss rate; forced IP left RELIABLE samples undelivered at 0.01% and 0.1%, which is why
  runs are only sent while the reader measures a clean path. At 1% AUTO's cap collapsed in 3 of 5 runs (to 2-9 units).
- **UDP offload stays on by default (the user's decision).** Same binary, offload on against `TT_UDP_OFFLOAD=0`
  (`~/rig_results_safe/hybrid_land/offload_decision.txt`): forced MTU at 1% loss, publisher -1.08 and subscriber
  -0.68 ms/MB (BETTER beyond the A/A floor); AUTO at 1% -0.37 / -0.34, within the floor; nothing WORSE at 0% or 0.01%.
  The pre-registered rule asked for a gain in both and printed DEFAULT-OFF. Not measured on f1f2f14d.
- **Receive drain** (f1f2f14d against 4fbb8fe1, I4s): subscriber socket drops 57-382 a run against 649-1,755, complete
  samples 1,715-1,717 against 1,514-1,644, subscriber CPU 1.70 against 4.00 ms/MB. The pre-registered "no socket drop
  in 4 of 5 runs" failed: 0 of 5 (`hybridDrain_i4s_20261012-130948.drain.txt`).

## 3. Methodology

Every figure comes from the rig's two dedicated Raspberry Pi 5s over a point-to-point 1 GbE link (or one of them, for
section 1); nothing measured on the dev PC is published. Native rows use each framework's own API with identical
payload shapes, the same explicit QoS asserted on every row, optimised builds on all sides, 3 repetitions and medians.
No process is pinned to a core (TESTING.md section 5); pinned figures are labelled notes in sections 1 and 2. Rows
from other sessions (`S`, `L`, `W`, `J`, `Z`) were taken pinned and say so by their date. Cross-host cells interleave
the frameworks within each repetition, in an order that rotates; section 1's harness runs each framework's repetitions back to back, one
framework after another within a cell. A cell is VOID when a payload crosses a framework's datagram boundary
unexpectedly, when history policies differ, or when delivery is incomplete (an incomplete vendor is excluded and
listed; an incomplete TickLE loses the cell). FastDDS's only tuned parameter is `maxMessageSize` 1472 in the `T` rows.
Latency is the latency from creation time since 2026-10-17 (the sample's scheduled publish time; for a ping, its due
time), read with a control in which nothing waits at the sender. The send-stamp latency is printed beside it as a
diagnostic. Full rules, fairness audit and zenoh-pico configuration: [TESTING.md](TESTING.md).

**Versions** (rig, Ubuntu 24.04, aarch64): the native rows use CycloneDDS 11.0.1 (the `rolling` build, linked by
`cyclonedds/build.sh`) with iceoryx 2.0.5 for section 1, and Fast DDS 2.14.6 (ROS jazzy); the rmw rows use jazzy's
`rmw_cyclonedds_cpp` (CycloneDDS 0.10.5) and `rmw_fastrtps_cpp` (Fast DDS 2.14.6). Vendor libraries are the
distribution's packages (CMake build type None, Debian's `-O2`); TickLE's core is built `-O2 -DNDEBUG`. Rows
measured before 2026-10-05 built rmw_tickle as `Release` (`-O3`); later rmw rows build it with the vendors' `-O2`.

## 4. Corrections and history

A correction stays with its cause, and a superseded figure stays with the reading drawn from it. These notes were
moved here unchanged from the tables' notes on 2026-10-17.

### Readings superseded on 2026-10-17

On 2026-10-17 the summary became "At a glance", and latency became the latency from creation time. Every figure the
old summary quoted is still in the tables above. These are the readings that changed:

- **Cross-host latency.** The summary read: "TickLE is best in every scored row of latency, throughput, CPU, memory
  and bandwidth at P1-P4". Campaign `A` stood at WIN 151, DRAW/TIE 11, LOSE 0, VOID 0 of 162 metric-cells, and WIN 139,
  DRAW/TIE 11 of 150 counted once. That was against WIN 136, DRAW/TIE 14 at `3ae721aa`, or 137 / 13 on the unrounded
  loss. From the send stamp this still holds. From creation time, TickLE lost rows 1c, 3c and 6bc to CycloneDDS on the
  first run (`4fca013b`) and won them on the second (`596e9c9c`, the scheduler wait fix)
  ([Latency from creation time](#latency-from-creation-time-2026-10-17)).
- **I4s latency.** The summary and the list "Where TickLE does not come first" read that I4s median latency "is
  behind all three because a DDS `publish()` blocks before it stamps the sample" (55.0 against 37.6-41.6 ms). L4 then
  stood at WIN 52, DRAW 23, LOSE 5 of 80 (WIN 53, DRAW 22 per MB published). The cause given was right: it is the wait
  that creation time now counts. From creation time, I4s latency is a WIN against all three.
- The rest of "Where TickLE does not come first" is now in "Every LOSE" and "Every DRAW", unchanged. It also said that
  I1L (5% loss) "lost on incomplete delivery on f5fcda06, is complete and won on main dde87857 (2026-10-16)"; that
  correction is in the I1L note.

### Same host: corrections and earlier sessions

- **Corrected 2026-10-17: goodput (TESTING.md section 4), the same raw files re-read.** `fair_samehost_summary.py`
  on `~/rig_results_safe/fair_samehost_e521bbba_20261009-033254`, `free` arm, medians:
  - **S1-S3 are now the delivered rate** and absorb S1d-S3d. Until 2026-10-17 S1-S3 were the client's send rate (S1 20,867
    / 6,370 / 4,439, S2 21,982 / 7,006 / 4,882, S3 25,137 / 12,832 / 9,333 Mbps, TickLE / FastDDS / CycloneDDS) and
    S1d-S3d the subscriber's (20,867 / 0 / 934, 21,982 / 0 / 1,029, 25,137 / 1 / 1,854, with each framework's
    delivered share of its own published samples: 100% / 0% / 21%, 21%, 20%). The worst framework (❌) moves from
    CycloneDDS to FastDDS, which took 0.31-0.70 Mbps. The share column is gone: it divided by each framework's own
    published samples, the denominator the user rejected for saturating cells on 2026-10-12. TickLE's leader verdict
    does not change.
  - **S3 TickLE 25,137 (n=2) -> 25,066 (n=3):** the drop-free rule is retired, so the rep with `shm_full_dropped=2690`
    (9,995,120 of 9,996,465 samples delivered) counts at its goodput, 24,879 Mbps in the window. All other S cells were n=3 already.
- **S1-S3 compared send rates, not delivery, until 2026-10-17** (corrected above). FastDDS's bench subscriber uses the default reader QoS (KEEP_LAST 1);
  on one host its writer overwrites what the reader has not taken, and in `mixed_delivery.sh`'s FastDDS LOCAL_ONLY
  arm (p2 BEST_EFFORT, 3 reps) the subscriber took **154 of 3.1 million** samples sent. TickLE's figures were
  drop-free reps, so for it sent equals delivered. A TickLE BEST_EFFORT rep that drops at a full ring reads a *higher*
  send rate (dropping is cheaper than delivering), which is why such reps were excluded until goodput made the
  delivered rate the figure (2026-10-17, above).
- **2026-10-09 re-measure** (`e521bbba`, same harness, rules and order as `f128f686`; 3 reps per cell, every cell
  n=3 except **¶ S3/S3d TickLE n=2**: one rep dropped at a full ring and is excluded, as the rule then said; S2/S2d
  are n=3 again; since 2026-10-17 that rep counts, above). Was at `f128f686` -> now: S1 18,484 -> **20,867** (+12.9%), S2 19,716 -> **21,982** (+11.5%), S3
  24,696 -> **25,137** (+1.8%), S4 12,613 -> **13,764** (+9.1%), S5 13,539 -> **14,771** (+9.1%), S6 14,900 ->
  **14,917** (+0.1%) Mbps; S7 0.023 -> **0.023**, S9 0.023 -> **0.023**, S10 0.027 -> **0.027** ms (-0.8%, -1.0%,
  +2.4% unrounded); S17 1.67 -> **1.53** us (-8.5%); S18 42.3 -> **41.9** us (-0.8%); S19 8.3 -> **8.5** MB (+2.4%).
  Between the two builds main gained receive-buffer lending with the in-place segment drain (`0be14584`; its rig A/B,
  `~/rig_results_safe/ab_samehost_lending_20261008-211707.summary.txt`, read +12.1% delivered at BEST_EFFORT p3 and
  +8.6% RELIABLE p3, IMPROVED 12 of 15, none worse), skip-to-newest for KEEP_LAST readers (`17c825f3`, `03585237`),
  the reader-wake generation fix (`e6226bbf`), the retry granularity G (`33b2cdfc`), `forget_peer`'s gap fix
  (`29dff630`) and discovery per entity (`33436ab3`); loaned messages (`7a321d26`) are rmw only and not on this path.
  The S2 and S5 gains match the lending A/B's p3 figures. The vendor arms, which none of these touch, moved within
  -6..+1% on throughput (FastDDS S6 the widest) and -12..+10% on RTT (FastDDS S10 -11.6%, CycloneDDS S7 +10.4%),
  against TickLE's +0..+13% on throughput and -1..+2% on RTT; no row's leader or worst changed. Pinned medians of
  the same session (TickLE / FastDDS / CycloneDDS): S1-S3 20,715 / 6,385 / 4,510, 21,893 / 6,746 / 4,932, 25,103 / 13,008 / 9,478; S4-S6 13,700 / 899
  / 2,673, 14,811 / 977 / 2,876, 14,778 / 1,876 / 5,209; S7, S9, S10 0.023 / 0.098 / 0.062, 0.023 / 0.099 / 0.061,
  0.027 / 0.098 / 0.063 ms. No leader changes pinned; TickLE's unpinned/pinned is 1.00-1.02x, CycloneDDS's
  0.81-0.87x at S4-S6 and 1.10-1.20x RTT, FastDDS's 1.13x at S4 and 1.10-1.11x RTT at S7, S9.
- **2026-10-08 re-measure** (`f128f686`, same harness, rules and order as `9f919c8b`; 3 reps per cell, every cell
  n=3 except **S2/S2d TickLE n=2**: one rep dropped 1 sample at a full ring and is excluded, as the rule says).
  Every TickLE figure moved; was at `9f919c8b` -> now: S1 13,555 -> **18,484**, S2 14,303 -> **19,716**, S3 22,613
  -> **24,696**, S4 10,146 -> **12,613**, S5 10,556 -> **13,539**, S6 12,333 -> **14,900** Mbps; S7 0.031 ->
  **0.023**, S9 0.031 -> **0.023**, S10 0.036 -> **0.027** ms; S17 2.13 -> **1.67** us; S18 78.8 † -> **42.3** us
  (this run's server no longer reads the CPU clock per echo, `8e98ba48`, so the † no longer applies); S19 8.0 ->
  **8.3** MB. Between the two builds main gained the frag in-order fast path (`bc398ed2`), reader-wake round 4
  (`8c1e6431`), encode-in-slot (`cd09e895`), the ring-turn starvation fix (`65b37569`) and the `departed_next` fix
  (`6f4b87d4`). The vendor arms, which none of these touch, moved within -12..+10% (FastDDS S5, the widest;
  CycloneDDS within 7%), against TickLE's +9..+36% and -25..-26% RTT; no row's leader or worst changed. Pinned
  medians of the same session (TickLE / FastDDS / CycloneDDS): S1-S3 18,508 / 6,381 / 4,475, 19,530 / 6,999 / 4,947, 24,813 / 13,023 / 9,511; S4-S6 12,649 / 897 /
  2,597, 13,453 / 977 / 2,880, 14,654 / 2,103 / 5,062; S7, S9, S10 0.023 / 0.097 / 0.061, 0.023 / 0.097 / 0.061,
  0.027 / 0.100 / 0.062 ms. No leader changes pinned; TickLE's unpinned/pinned is 1.00-1.02x, CycloneDDS's
  0.85-0.92x at S4-S6 and 1.07-1.10x RTT, FastDDS's 1.13x RTT at S10.
- **2026-10-06 re-measure** (`9f919c8b`): S10 fell from 0.052 to **0.036 ms** - the old figure was a first-lap one,
  ~16 us of first-touch page faults on the reliable reorder ring before warm-up was excluded (ROADMAP Now 1). The
  BEST_EFFORT rows now also show what was delivered (S1d-S3d): at KEEP_LAST 1, the DDS default, FastDDS's reader took
  almost nothing at max rate and CycloneDDS's about a fifth (its bench now takes everything per wake), while TickLE
  delivered every sample. **‡ CycloneDDS includes iox-roudi**: its RSS adds 212 MB (206 MB of it shared memory) and
  its CPU 1.6 us per round trip. **† TickLE's latency server read the CPU clock on every echo in this run** (the
  vendors' servers do not; fixed in `8e98ba48`), so S18's TickLE figure then (78.8) was overstated. Pinned medians
  of the same session: S1-S3 13,512 / 6,329 / 4,478, 14,197 / 6,932 / 4,893, 22,322 / 12,519 / 9,385; S4-S6 10,062 / 909 / 2,649,
  10,631 / 987 / 2,842, 12,458 / 1,874 / 5,200; S7, S9, S10 0.030 / 0.098 / 0.061, 0.031 / 0.096 / 0.061, 0.036 /
  0.099 / 0.061 ms.
- **Unpinned is the headline; pinned is this note** (2026-10-05, `fair_samehost_remeasure.sh`, `7fb6fabf`, each cell
  run unpinned and with the server on core 1 and the client on core 2, back to back, order alternating). No row's
  leader changes. TickLE moves within 3% either way. Pinning to one core each helped the multi-threaded DDS processes,
  not TickLE: unpinned/pinned is 0.86x / 0.82x / 0.88x for CycloneDDS at S4-S6, 0.83x for FastDDS at S6, and
  1.06-1.15x RTT for CycloneDDS at S7, S9, S10. Pinned medians (TickLE, FastDDS, CycloneDDS): S1 13,542 / 6,489 /
  4,539; S2 14,277 / 7,003 / 4,960; S3 22,584 / 13,460 / 9,501; S4 10,026 / 939 / 2,629; S5 10,811 / 1,045 / 2,934;
  S6 12,701 / 2,049 / 5,235; S7 0.030 / 0.099 / 0.062; S9 0.031 / 0.098 / 0.061; S10 0.052 / 0.099 / 0.063 ms.
  Raw files `~/rig_results_safe/fair_samehost_7fb6fabf_20261005-190124.*`. S8 and S11-S13 are still the pinned
  figures of their builds and have not been re-measured unpinned.
- **S6 (RELIABLE p4)** was VOID on `8d1c3712`: a lost-wakeup defect (the writer did not ring a reader that had gone
  back to sleep) made the application give up 4-7 samples per rep. Fixed in `fe45f276` (the writer now rings each
  sleep of the reader once) and re-measured there with every rep `write_fail=0`, `lost=0`. That pinned run gave
  CycloneDDS 5,240 (an earlier one 5,776). S4-S5 were first measured after ACK solicitation stopped being clocked by
  a 1 ms timer (`8d1c3712`); before that, TickLE lost S4 and S6 to CycloneDDS.

### rmw same host

- **Corrected 2026-10-10: R14-R21 and the `a7e02807` column.** `perf_test`'s received, sent and lost columns are
  floor(count / D) per row, not counts; the reader summed them (and divided by the summed T_loop). Corrected by the
  whole-count reader (`rmw_samehost_summary.py`, `whole_counts()`) on the same raw runs; no verdict changes. rmw_tickle
  moved by under 0.02%, the vendors by up to 0.7% (e.g. R14 CycloneDDS 70,331 -> 70,420, R16 FastDDS 2,313 -> 2,322).
- **R18 was scored DRAW** until 2026-10-08 by the harness's former 2 x SE rule, although rmw_tickle's reps (13.14 /
  13.73 / 13.17 us) are below every rep of CycloneDDS (32.81-34.45) and FastDDS (171 / 1,006 / 323): FastDDS's
  scatter made the bound wider than the gap. Scored by the reps' ranges, as TESTING.md section 4 says, it is a WIN;
  no other verdict in the `03585237` run or at `f128f686` changes.
- **At `03585237`, R12 and R13 were right as scored.** R12: rmw_tickle 15,252-15,260 kB against CycloneDDS
  15,232-15,264, the ranges overlap, a DRAW (20 kB apart at the median). R13: rmw_tickle's subscriber 17,536-17,792
  kB against CycloneDDS's 16,896-17,152, no overlap, a LOSE of ~512 kB (3%).

### Cross-host: the `09a0437d` re-measure

Totals over the 150 metric-cells both runs score: WIN 136, DRAW/TIE 14 at `3ae721aa` -> WIN 139, DRAW/TIE 11 (the
12 + 2 new `server.recv_mbps` cells, the delivered rate in the window, are all WINs, giving 151 / 11 and 20 / 2). Four
verdicts moved, all `server.loss_pct`, where TickLE lost nothing in either run: cell 1 (P1) TIE -> DRAW (CycloneDDS
and FastDDS lost a few samples in one rep each), cell 5 (P1, 5% loss) TIE -> WIN (FastDDS lost 0.002-0.016%), cell 9
(KEEP_LAST 64) DRAW -> WIN (both vendors lost in every rep; at `3ae721aa` FastDDS's loss printed as 0.0, which the
unrounded count already made a WIN), cell 6 (P4, 5% loss) DRAW -> WIN, provisional (one FastDDS rep). Pinned: WIN
135, DRAW/TIE 15 -> WIN 149, DRAW/TIE 13 of 162.

**Correction (2026-10-17): `3ae721aa` A read with every rep's delivery checked.** `campaign_summary.py` compared sent
with received on a cell's last rep only (it overwrote the counts rep by rep); since `e24894d3` it checks every rep.
At `3ae721aa`, cell 6 TickLE rep 1 delivered 194,215 of 194,216 samples (one sample written before the reader matched,
`prematch_window=1 first_seq=2 post_match_lost=0`; this predates the no-send-before-match rule of `46554d68`). Under
the RELIABLE incomplete-delivery rule (TESTING.md section 4) that makes every metric of cell 6 a LOSE, so `3ae721aa` A
reads WIN 127, DRAW/TIE 13, LOSE 10, not the WIN 136, DRAW/TIE 14 quoted above. No exemption for a pre-match sample
was added: it would be a rule change in TickLE's favour made after seeing the data. The `09a0437d` files are unchanged
by the every-rep check (A WIN 139, DRAW/TIE 11, LOSE 0 of 150; T WIN 18, TIE 2 of 20).

**TickLE's delta against the vendors' (the control).** Since `3ae721aa`, main gained on the cross-host path the
Heartbeat answer bounded by the measured repair transit (`3355e94d`; its rig A/B gave cell 6 send +4.5%), the writer-owned
VOLATILE match point (`da3029e0`), the in-order fragment fast path (`bc398ed2`) and the `departed_next` fix
(`6f4b87d4`); the peer-table fix (`29dff630`) came after and is not in this build. The harness change (the window)
applies to every framework alike, so a TickLE move counts only where it leaves the vendors' moves behind:

- **Moved with the vendors, not a TickLE effect:** latency rows 1, 3, 6a, 6b against the warm-up re-runs
  (TickLE -1.4% to +0%, FastDDS -2.0% to +2.9%, CycloneDDS -1.2% to +1.0%); P2-P4 throughput (TickLE at the link,
  +-0.1%); P1 RELIABLE (row 7: TickLE +5.9%, FastDDS -5.1%, CycloneDDS +2.6%, all inside the reps' ranges, TickLE
  93.6-114 against 93.2-115); P1 under loss and reorder (rows 13, 15: TickLE -1.2% / -4.8%, FastDDS -14% / -11%,
  CycloneDDS -10% / +15%).
- **TickLE alone moved, outside its own range:** P4 server CPU per sample (row 23) 8.36 -> 7.63 (-8.7%; reps
  8.34-8.39 -> 7.58-7.67) while FastDDS and CycloneDDS moved +0.8% and +1.5%; P4 server peak RSS (row 33) 2,971 ->
  1,987 KB (-33%) while both vendors moved +0.2%, and in all four P4 cells without loss (`A` and `T`, free and pinned).
  Both are the receive side of in-order fragments, which `bc398ed2` now assembles in `frag_scratch` instead of the
  reorder ring; under 5% loss, where fragments arrive out of order and still use the ring, the server stays at 3,035
  KB (was 2,967). KEEP_LAST 64 (row 12) 107 -> 117 (+9.3%; reps 102-117 -> 116-118) while FastDDS and CycloneDDS
  fell 8.9% and 10.2%. P4 under 5% loss with FastDDS as shipped (`A` cell 6, not a table row) TickLE 860 -> 891 Mbps
  (+3.6%, reps 843-870 -> 889-893) while CycloneDDS fell 22%, the same direction and size as `3355e94d`'s A/B.
- **TickLE moved against itself:** P1 server peak RSS (row 32) 1,949 -> 2,057 KB (+5.5%), within CycloneDDS's +11%
  and FastDDS's +0.3%; P1-P4 client RSS +1.5% to +1.9% (rows 28-30) against the vendors' -1.1% to +0.9%. Retention at
  P1 (row 16) 93.2% -> 86.9%, the quotient of row 7's rise and row 13's fall, both inside the ranges; the vendors'
  retention fell by as much or more, relative to its own (47.6% -> 43.1%, 5.5% -> 4.8%).

### Cross-host latency

History. At `3ae721aa` (no warm-up) rows 1-6b were 0.211 / 0.297 / 0.329, tails 0.298 / 0.604 / 10.761, 0.237 /
0.322 / 0.281, tails 0.356 / 0.622 / 0.830, and so on; CycloneDDS's 10.8 ms P1 tail was one first-lap round trip in a
hundred and has not recurred in any warm run. Rows 1, 3 and 6b were then re-measured with the warm-up (`f40a2adf`,
2026-10-07, `~/rig_results_safe/crosshost_latency_warm_f40a2adf_20261007-035608.txt`: 0.209 / 0.285 / 0.260, 0.231 /
0.316 / 0.273, 0.249 / 0.348 / 0.293) and rows 5, 6, 6a with the 20 s edge bound (`9b5491e6`,
`~/rig_results_safe/crosshost_latency_warm2_9b5491e6_20261007-101404.txt`: 10.1 / 10.3 / 10.3, tail 12.1 / 12.3 /
12.2, 0.235 / 0.344 / 0.288); the `09a0437d` run supersedes both under the same rules and agrees with them within the
control (above). Rows 24-25 (CPU per round trip) were 159 / 377 / 226 and 156 / 376 / 224 at `3ae721aa`: the process's
whole CPU over its 100 round trips, start-up included; it is now over all 8,292 round trips of a run, edges included,
which is why every framework fell by 73-78%.

### Cross-host throughput

**Corrected 2026-10-17: rows 11, 11a-11c and 12 were send rates** (the client's `send_mbps` in the window), and the
vendors did not deliver all they sent: rows 11-11c are BEST_EFFORT and row 12 is KEEP_LAST, where a writer may
replace unacknowledged samples. Was -> now (TickLE / FastDDS / CycloneDDS, Mbps, 3 reps each, `A_free` cells 8, 13,
14, 15, 9): row 11 115 / 44.7 / 66.6 -> 115 / **28.8** [22.0-33.6] / **62.1** [60.9-64.1]; 11a 938 / 740 / 911 ->
938 / **575** [481-641] / **906**; 11b 943 / 574 / 828 -> 943 / **560** [497-598] / **654** [640-666]; 11c 944 /
915 / 938 -> 944 / **902** [895-910] / **937**; row 12 117 / 38.0 / 75.1 -> 117 / 38.0 / **72.6** [66.1-81.2].
FastDDS in row 11 delivered 457,479 of 657,793, 507,930 of 669,528 and 324,111 of 655,022 samples (64% of its send
rate). TickLE delivered every sample it sent in every one of these cells (lost 0), so none of its figures moves, nor
any verdict or ✅/❌. Rows 7-10 and 13-17 do not move: RELIABLE + KEEP_ALL delivered everything it sent. Pinned, row 11
reads 120 / **33.0** / **67.7** (was 120 / 45.3 / 67.9) and 11a FastDDS **632** (was 735).
(Before this correction this paragraph read: "Row 11's FastDDS figure is a send rate it did not fully deliver: FastDDS
lost 15.5% of samples in that cell on a 2026-10-03 run and 22.2% in the QoS sweep.")

### rmw cross-host

- **History: the first equal-bound runs (2026-10-07) failed rmw_tickle.** At main `f40a2adf`
  (`~/rig_results_safe/rmw_keepall_equal_20261007-044904.txt`, 3 reps) rmw_tickle's publisher ended in 2 of its 6
  lossy runs (one per topic) with no sample lost; its usable medians were 71,279 and 26,372/s. Cause: a reader ignored
  a heartbeat's request for an answer while its own retry for an open gap was armed. Fixed by `2901121e` (answer a heartbeat that asks, also
  with a retry armed), `e3236048` and `3355e94d` (the answer leaves out repairs still in transit, limited by the
  measured repair transit); the `Ke` run at `29dff630` includes them.
- **Corrected 2026-10-06: FastDDS at 0% loss is not an incomplete delivery.** This note said it "delivered 22-28%
  fewer samples than it sent" and excluded it (✗). Those 70-80k samples were written before its reader matched
  (~0.7 s): `perf_test` counts every id below the first one received as lost, and a VOLATILE writer rightly does
  not deliver what it wrote before the match. Its `--expected_num_subs` is compiled out in this build, so publishing
  starts before matching for every arm. The rows count delivered samples per second after the start, which this does
  not touch, so FastDDS is scored in rows 72 and 74. CycloneDDS's "lost ~106k" in two Array1k 5% runs is the same
  artefact. At Array1k FastDDS's rate is steady at ~16k/s with ~320 ms latency (its full 5,000-sample history
  draining at that rate); Array4k is at the link's ceiling for all three (‡).
- **Corrected 2026-10-10: rows 72-75 and these notes.** `perf_test`'s received, sent and lost columns are
  floor(count / D) per row, not counts; the reader summed them. Corrected by the whole-count reader
  (`rmw_keepall_rig_summary.py`, `whole_counts()`) on the same raw runs; no verdict changes. Rates moved by under 1/s
  (row 72 rmw_tickle 90,849 -> 90,850, CycloneDDS 84,445 -> 84,446; row 73 74,128 -> 74,129, CycloneDDS 1,807 ->
  1,808; row 75 CycloneDDS 346 -> 347); CycloneDDS's subscriber CPU per sample at 5% loss fell 3-4% (45.6 -> 44.0,
  182.4 -> 176.4 us).

### rmw large messages

- **Corrected 2026-10-17: publisher CPU per MB delivered** (the user's goodput decision, TESTING.md section 4; same
  raw runs, `experiments/largemsg_goodput.py`, which re-uses `largemsg_rig_summary.py` unchanged and divides the
  publisher's CPU by the subscriber's delivered MB; its `pub` mode reproduces the previous letters). Only **I4s** moves
  by more than 0.01 ms/MB: rmw_tickle 3.43 -> **3.60** (reps 3.577-3.615), Fast DDS as shipped 3.57 -> 3.56
  (3.536-3.579), Fast DDS at 1472 6.87 -> 9.27, CycloneDDS 3.90 -> 3.93; letters **W–W–W -> D–W–W**. Samples first,
  delivered of published per run: rmw_tickle 1,718-1,720 of 1,802-1,803, Fast DDS 1,690-1,691 of 1,694-1,695, Fast
  DDS at 1472 999-1,156 of 1,531-1,532, CycloneDDS 1,696-1,699 of 1,699-1,701 (r4 1,140). The cause is rmw_tickle's
  non-blocking `publish()`: it spends CPU on the ~84 samples per run it drops at the sender, where a DDS publisher
  blocks. For CPU this supersedes the offered-tick denominator; delivered % of offered still ranks I4s, and ranks it
  as goodput does. I4 RELIABLE publisher 3.68 -> 3.69; I1, I1L, I1 best effort and I4 best effort unchanged at the
  table's precision (I4 best effort Fast DDS 3.61 -> 3.62, still L). Subscriber CPU and delivered % were already per
  delivered sample. L4 totals: WIN 53, DRAW 22, LOSE 5 -> **WIN 52, DRAW 23, LOSE 5** of 80.
- **Re-measured 2026-10-15 (main f5fcda06 against main 0c9f9a3b, same harness):** subscriber CPU fell 0.19-0.28
  ms/MB in every 0%-loss cell (I1 RELIABLE 2.56 -> 2.28, I1 best effort 2.38 -> 2.20, I4 RELIABLE 2.45 -> 2.21, I4
  best effort 2.40 -> 2.19, I4s 1.61 -> 1.40), most of it UDP checksum 0 (its rig A/B: -0.10 to -0.12 ms/MB). Against
  Fast DDS as shipped it went from LOSE to DRAW in I1 RELIABLE, I1 best effort, I4 RELIABLE and I4s; it still loses at
  I4 best effort (2.19 against 2.10, +4%), as does the publisher there (3.72 against 3.61, +3%). Latency and
  subscriber RSS are unchanged or lower.
- **CPU per MB at 0% loss before the re-measure** (f1f2f14d, 2026-10-12): Fast DDS as shipped spent 13.7-15.6% less
  on the subscriber (18.2% at I4s; means) and 2.0-8.4% less on the publisher at best effort. Against main before the
  hybrid transport (4fbb8fe1) subscriber CPU fell 2.1-2.5 ms/MB (-46 to -57%). Step A (dc056907) was 0.15-0.19 ms and
  1.2 ms behind Fast DDS in median latency; TickLE is now ahead at 1 and 4 MB.

## 5. Open work

The to-do list (missing same-host CPU/memory cells, loaned messages, and more) is
in [ROADMAP.md](ROADMAP.md).
