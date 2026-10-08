# TickLE performance results

TickLE against FastDDS and CycloneDDS, measured on dedicated hardware. zenoh-pico and rmw_zenoh are shown for
reference. Column order is always **TickLE, FastDDS, CycloneDDS**. Design background is in [DESIGN.md](DESIGN.md),
the ROS 2 layer in [RMW.md](RMW.md), how the numbers are taken in [TESTING.md](TESTING.md), open work in
[ROADMAP.md](ROADMAP.md). Result file paths below are relative to `examples/perf_hil/` unless they start with `~`.

## Summary

- **Cross-host (two Raspberry Pi 5s over 1 GbE):** TickLE is best in every scored row of latency, throughput, CPU,
  memory and bandwidth at P1-P4, with and without injected loss, under identical explicit QoS, against FastDDS both
  as shipped and tuned, with no process pinned to a core. The campaign at `09a0437d` (2026-10-08, every framework
  dropping the same warm-up and cool-down) scored **WIN 151, DRAW/TIE 11, LOSE 0, VOID 0** (`A`, 17 cells, 162
  metric-cells) and **WIN 20, TIE 2, LOSE 0** (`T`). On the 150 metric-cells both runs share (the windowed
  delivered rate `server.recv_mbps` is new, a WIN in all 12 `A` cells and both `T` cells) it is WIN 139, DRAW/TIE
  11, against WIN 136, DRAW/TIE 14 at `3ae721aa` (137 / 13 on the unrounded loss). All four changes are a
  `server.loss_pct` verdict, where TickLE lost nothing in either run and only whether a vendor's range touched zero
  moved (section 2, "Re-measure at `09a0437d`").
- **The largest margin is RELIABLE under 5% loss:** TickLE keeps 86.9% (P1) and 91.6% (P4) of its throughput;
  FastDDS 43.1% / 38.9%, CycloneDDS 4.8% / 1.6% (rows 16-17).
- **Same host (shared memory):** TickLE leads every measured cell, unpinned, with warm-up excluded. At BEST_EFFORT
  KEEP_LAST 1 (the DDS default) TickLE delivered every sample at max rate, CycloneDDS about a fifth and FastDDS
  almost none (S1d-S3d); TickLE's CPU per delivered RELIABLE sample is 1.7 us against 12.0 and 31.1 (S17).
- **rmw layer:** rmw_tickle is first on every block-wait row and every poll-wait row except seven draws with
  CycloneDDS (rows 59, 62-67). It loses no row. Under RELIABLE + KEEP_ALL at 5% loss, with the writers' KEEP_ALL bounds made
  equal, it delivers 41x (Array1k) and 78x (Array4k) CycloneDDS's rate with no sample lost; FastDDS's write times
  out under its default 100 ms bound and the run ends (rows 72-75).
- **rmw layer, same host** (`03585237`, rows R1-R21): rmw_tickle scores WIN 50, DRAW 1, LOSE 1 against both DDS
  rmws; the LOSE is ~3% more peak RSS than CycloneDDS at Array1k RELIABLE. At BEST_EFFORT KEEP_LAST 1 max rate it
  delivers 150k Array1k samples/s against CycloneDDS's 71k and FastDDS's 8k.
- **Not scored:** row 5 (netem dominates), rows 45-47 (all detect correctly), zenoh-pico (reference only).

## 1. Same host (shared memory)

Publisher and subscriber on **one** rig Pi, each framework on its own shared-memory path: TickLE's segment,
**FastDDS as shipped** (data-sharing, on by default) and **CycloneDDS with iceoryx**. Each arm's transport is checked
by a witness that cannot be configured into agreeing (loopback packet count, and TickLE's own `tx_shm` share).
Harness `experiments/s6_transport_cells.sh`, 3 repetitions per framework unless noted, throughput from drop-free
repetitions only. **No process is pinned to a core** (TESTING.md section 5); S1-S10 (but S8) and S1d-S3d,
S17-S19 are from `experiments/fair_samehost_remeasure.sh` on `f128f686` (2026-10-08;
`~/rig_results_safe/fair_samehost_f128f686_20261008-081131.*`, read by `fair_samehost_summary.py`): every
framework drops the same warm-up and cool-down (4,096 round trips, 2 s), BEST_EFFORT runs KEEP_LAST 1 on all three,
explicitly (the DDS default), and the pinned figures are a note below. **✅ = best, ❌ = worst** in the row.

| # | Metric | Condition | TickLE | FastDDS | CycloneDDS | build | details |
|---|---|---|---|---|---|---|---|
| | **Throughput, send Mbps** (higher is better) | | | | | | |
| S1 | BEST_EFFORT, max rate | P2 1292 B | ✅ **18,484** | 6,340 | ❌ 4,445 | `f128f686` | [notes](#same-host-notes) |
| S2 | BEST_EFFORT, max rate | P3 1424 B | ✅ **19,716** ¶ | 6,994 | ❌ 4,897 | `f128f686` | [notes](#same-host-notes) |
| S3 | BEST_EFFORT, max rate | P4 2800 B | ✅ **24,696** | 12,853 | ❌ 9,449 | `f128f686` | [notes](#same-host-notes) |
| S4 | RELIABLE | P2 1292 B | ✅ **12,613** | ❌ 1,004 | 2,223 | `f128f686` | [notes](#same-host-notes) |
| S5 | RELIABLE | P3 1424 B | ✅ **13,539** | ❌ 969 | 2,439 | `f128f686` | [notes](#same-host-notes) |
| S6 | RELIABLE | P4 2800 B | ✅ **14,900** | ❌ 2,041 | 4,651 | `f128f686` | [notes](#same-host-notes) |
| | **Delivered, receive Mbps** (higher is better; the subscriber's rate over the measured window) | | | | | | |
| S1d | BEST_EFFORT, max rate, KEEP_LAST 1 | P2 1292 B | ✅ **18,484** (100%) | ❌ 0 (0%) | 942 (21%) | `f128f686` | [notes](#same-host-notes) |
| S2d | BEST_EFFORT, max rate, KEEP_LAST 1 | P3 1424 B | ✅ **19,716** ¶ (100%) | ❌ 0 (0%) | 1,032 (21%) | `f128f686` | [notes](#same-host-notes) |
| S3d | BEST_EFFORT, max rate, KEEP_LAST 1 | P4 2800 B | ✅ **24,696** (100%) | ❌ 1 (0%) | 1,842 (19%) | `f128f686` | [notes](#same-host-notes) |
| | **Latency, RTT mean ms** (lower is better; one ping in flight, 200/s unless noted) | | | | | | |
| S7 | RTT | P2 1292 B | ✅ **0.023** | ❌ 0.100 | 0.067 | `f128f686` | [notes](#same-host-notes) |
| S8 | RTT, 20/s (60 s) | P2 1292 B | ✅ **0.032** | ❌ 0.197 | 0.071 | `e17b4e6f` | [notes](#same-host-notes) |
| S9 | RTT | P3 1424 B | ✅ **0.023** | ❌ 0.101 | 0.066 | `f128f686` | [notes](#same-host-notes) |
| S10 | RTT | P4 2800 B | ✅ **0.027** | ❌ 0.113 | 0.066 | `f128f686` | [notes](#same-host-notes) |
| | **CPU** (lower is better) | | | | | | |
| S11 | publisher, us per sample | BEST_EFFORT P4, max rate | ✅ **1.071** | ❌ 1.694 | – | `e17b4e6f` | [notes](#same-host-notes) |
| S12 | client, us per round trip | RTT P2, 20/s | ✅ **66.0** | ❌ 136.7 | 102.1 | `e17b4e6f` | [notes](#same-host-notes) |
| | **Memory** (lower is better) | | | | | | |
| S13 | publisher peak RSS, MB | BEST_EFFORT P4 | ✅ **3.2** | ❌ 15.9 | – | `e17b4e6f` | [notes](#same-host-notes) |
| S17 | client + server us per delivered sample | RELIABLE P3 | ✅ **1.67** | ❌ 31.08 | 11.99 | `f128f686` | [notes](#same-host-notes) |
| S18 | client + server us per round trip | RTT P3, 200/s | ✅ **42.3** | ❌ 171.3 | 97.7 ‡ | `f128f686` | [notes](#same-host-notes) |
| S19 | client + server peak RSS, MB | RELIABLE P3 | ✅ **8.3** | 46.7 | ❌ 236.4 ‡ | `f128f686` | [notes](#same-host-notes) |
| | **Mixed: one publisher, one subscriber on its host and one across the link** (delivered k samples/s, p2) | | | | | | |
| S15 | BEST_EFFORT, each subscriber | local / remote | ✅ **90.7 / 90.7**, loss 0 | 53.1 / 54.6, loss 4-22% | – | `7f127d56` | [notes](#same-host-notes) |
| S16 | RELIABLE, each subscriber | local / remote | ✅ **90.7 / 90.7** | 30.8 / 30.8 | – | `7f127d56` | [notes](#same-host-notes) |
| | **Bandwidth** | | | | | | |
| S14 | wire bytes per sample | every cell above | 0 | 0 | 0 | | nothing leaves the host |

### Same-host notes

- **FastDDS delivers ~0% in S1d-S3d.** At BEST_EFFORT KEEP_LAST 1 (the DDS default; data-sharing on, as shipped)
  its publisher sent ~5.5M samples in 9 s and its subscriber took 105-284 of them, in every rep and at every size.
  Re-run 2026-10-07 (`experiments/fastdds_be_delivery.sh` at `5ac23fbc`, p3, 3 reps;
  `~/rig_results_safe/fastdds_be_delivery_5ac23fbc_20261007-103007.*`): reproduced, 250 of 5.50M delivered
  (0.0046%); with the publisher slowed to 2.1% of that rate (148 Mbps) it delivered 100%, so the loss is a
  rate-dependent overrun. Whether the writer's two-payload history pool is the cause was not decided: the depth-64
  arm and the arm counting Fast DDS's own discards were VOID (2 of 3 usable reps). TickLE, the control, delivered 100%.
  Reproduced again at `f128f686` (2026-10-08): `delivered_ratio=0.000` at p2, p3 and p4, 3 reps each (280-463
  samples taken per rep).
- **2026-10-08 re-measure** (`f128f686`, same harness, rules and order as `9f919c8b`; 3 reps per cell, every cell
  n=3 except **¶ S2/S2d TickLE n=2**: one rep dropped 1 sample at a full ring and is excluded, as the rule says).
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
- **S1-S3 compare send rates, not delivery.** FastDDS's bench subscriber uses the default reader QoS (KEEP_LAST 1);
  on one host its writer overwrites what the reader has not taken, and in `mixed_delivery.sh`'s FastDDS LOCAL_ONLY
  arm (p2 BEST_EFFORT, 3 reps) the subscriber took **154 of 3.1 million** samples sent. TickLE's figures are
  drop-free reps, so for it sent equals delivered. Delivered vendor rates are on the roadmap. A TickLE BEST_EFFORT rep
  that drops at a full ring reads *higher* (dropping is cheaper than delivering), which is why such reps are excluded.
- **S6 (RELIABLE p4)** was VOID on `8d1c3712`: a lost-wakeup defect (the writer did not ring a reader that had gone
  back to sleep) made the application give up 4-7 samples per rep. Fixed in `fe45f276` (the writer now rings each
  sleep of the reader once) and re-measured there with every rep `write_fail=0`, `lost=0`. That pinned run gave
  CycloneDDS 5,240 (an earlier one 5,776). S4-S5 were first measured after ACK solicitation stopped being clocked by
  a 1 ms timer (`8d1c3712`); before that, TickLE lost S4 and S6 to CycloneDDS.
- **S11 and S13 compare against FastDDS only.** CycloneDDS's memory excludes iceoryx's `iox-roudi` daemon, which
  reserved 216 MB of shared memory before any application connected. TickLE's segment is in-process, no daemon.
- **S10 is the narrowest RTT lead** (2.5x CycloneDDS at `f128f686`; 1.29x when this was investigated): a p4 sample is two datagrams and two slots on TickLE's path.
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
**Build `03585237`** (2026-10-08; `experiments/rmw_samehost.sh`, rmw_tickle built as the jazzy debs are, `-g -O2`;
`~/rig_results_safe/rmw_samehost_03585237_20261008-101135.{txt,runs}`). It carries the stall fix (`cba66e30`),
reader-wake round 4 (`8c1e6431`), encode-in-slot (`cd09e895`), the ring-turn fix (`65b37569`) and skip-to-newest
(`17c825f3`, ordered once per pass in `03585237`). The last column is rmw_tickle in the same harness at `f128f686`
(the same morning, before skip-to-newest; `~/rig_results_safe/rmw_samehost_f128f686_20261008-065726.txt`).
**Scored: WIN 50, DRAW 1, LOSE 1** of 52 (at `f128f686`: WIN 45, DRAW 1, LOSE 6).

| # | Metric | Cell | rmw_tickle | FastDDS | CycloneDDS | rmw_zenoh (ref) | verdict | rmw_tickle at `f128f686` |
|---|---|---|---|---|---|---|---|---|
| | **RTT mean, us** (lower is better) | | | | | | | |
| R1 | block wait (`spin()`) | Bench 64 B, BEST_EFFORT | **35.8** | 132.1 | 110.7 | 236.7 | WIN | 35.9 |
| R2 | block wait | Bench 64 B, RELIABLE | **37.4** | 150.5 | 111.7 | 232.0 | WIN | 37.6 |
| R3 | block wait | Array1k, BEST_EFFORT | **38.2** | 130.7 | 115.4 | 243.9 | WIN | 37.1 |
| R4 | block wait | Array1k, RELIABLE | **38.6** | 153.4 | 114.7 | 242.6 | WIN | 39.3 |
| R5 | poll wait, 100 us sleep | Bench 64 B, BEST_EFFORT | **149.1** | 192.3 | 183.6 | 287.5 | WIN | 150.0 |
| R6 | poll wait, 100 us sleep | Bench 64 B, RELIABLE | **153.3** | 200.9 | 186.8 | 289.4 | WIN | 151.7 |
| R7 | poll wait, 100 us sleep | Array1k, BEST_EFFORT | **150.0** | 193.9 | 184.5 | 294.1 | WIN | 151.0 |
| R8 | poll wait, 100 us sleep | Array1k, RELIABLE | **150.8** | 203.1 | 188.0 | 296.4 | WIN | 151.5 |
| | **CPU, ping + pong, us per round trip** (lower is better) | | | | | | | |
| R9 | block wait | Bench 64 B, RELIABLE | **64.4** | 256.2 | 150.4 | 339.8 | WIN | 64.4 |
| R10 | block wait | Array1k, RELIABLE | **66.2** | 261.9 | 154.4 | 354.5 | WIN | 67.5 |
| | **Peak RSS, kB, larger of the two processes** (lower is better) | | | | | | | |
| R11 | RTT | Bench 64 B, BEST_EFFORT, block | **15,140** | 26,252 | 15,248 | 22,496 | WIN | 15,136 |
| R12 | RTT | Bench 64 B, RELIABLE, block | 15,260 | 26,260 | 15,240 | 22,500 | DRAW | 15,260 (LOSE) |
| R13 | throughput | Array1k, RELIABLE | 17,664 | 35,814 † | **17,152** | 123,624 | LOSE | 17,536 (LOSE) |
| | **Delivered msg/s** (higher is better) | | | | | | | |
| R14 | max rate, KEEP_LAST 1 | Array1k, BEST_EFFORT | **150,010** | 8,283 | 70,703 | 62,935 | WIN | 42,033 (LOSE) |
| R15 | max rate, KEEP_LAST 1 | Array4k, BEST_EFFORT | **81,716** | 3,835 | 60,939 | 50,419 | WIN | 15,824 (LOSE) |
| R16 | max rate, KEEP_ALL | Array1k, RELIABLE | **176,050** | 2,488 † | 65,110 | 107,879 | WIN | 181,017 |
| R17 | max rate, KEEP_ALL | Array4k, RELIABLE | **146,182** | 16,846 | 48,222 | 93,512 | WIN | 148,682 |
| | **CPU, pub + sub, us per delivered sample** (lower is better) | | | | | | | |
| R18 | max rate | Array1k, BEST_EFFORT | **13.17** | 323.24 | 33.40 | 45.03 | WIN | 46.98 (LOSE) |
| R19 | max rate | Array4k, BEST_EFFORT | **24.29** | 690.34 | 36.66 | 53.17 | WIN | 125.51 (LOSE) |
| R20 | max rate | Array1k, RELIABLE | **6.72** | 594.44 † | 34.93 | 21.15 | WIN | 6.54 |
| R21 | max rate | Array4k, RELIABLE | **9.07** | 85.74 | 39.09 | 25.75 | WIN | 9.02 |

Rows not shown are WINs of the same shape: RTT p50 and p99 in every cell, CPU per round trip in the six other cells
(61.8-79.7 us against FastDDS 172-241 and CycloneDDS 146-160), and the nine other peak-RSS cells.

- **† FastDDS refused in tput Array1k RELIABLE r2:** its publisher ended on "failed to publish" (KEEP_ALL under the
  100 ms `max_blocking_time`). Per the user's rule it is recorded as "does not run" in that rep - REFUSED, excluded,
  listed - and not analysed further; R13, R16 and R20 are the median of its other two reps (n=2).
- **R18 was scored DRAW** until 2026-10-08 by the harness's former 2 x SE rule, although rmw_tickle's reps (13.14 /
  13.73 / 13.17 us) are below every rep of CycloneDDS (32.81-34.45) and FastDDS (171 / 1,006 / 323): FastDDS's
  scatter made the bound wider than the gap. Scored by the reps' ranges, as TESTING.md section 4 says, it is a WIN;
  no other verdict in this run or at `f128f686` changes.
- **R12 and R13 are right as scored.** R12: rmw_tickle 15,252-15,260 kB against CycloneDDS 15,232-15,264, the
  ranges overlap, a DRAW (20 kB apart at the median). R13: rmw_tickle's subscriber 17,536-17,792 kB
  against CycloneDDS's 16,896-17,152, no overlap, a LOSE of ~512 kB (3%).
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
arms rotated; reading rules in its header, enforced by `rmw_loan_bench_summary.py`). Build: `ab/lending` `fe15102e`
merged with `f837ae82` plus the loaned-messages change (`-O3` Release; the library's md5 is in the raw files), 2026-10-08 22:07;
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

| # | Metric | Condition | TickLE | FastDDS | CycloneDDS | zenoh-pico | rmw_zenoh | meas. |
|---|---|---|---|---|---|---|---|---|
| | **[Latency](#latency)** (ms) | | | | |  | – | |
| 1 | RTT mean | P1 76 B | ✅ **0.206** | ❌ 0.290 | 0.257 | 0.221 † | – | A |
| 2 | RTT max (tail) | P1 76 B | ✅ **0.252** | ❌ 0.342 | 0.296 | 0.336 † | – | A |
| 3 | RTT mean | P2 1292 B | ✅ **0.231** | ❌ 0.315 | 0.275 | 0.246 † | – | A |
| 4 | RTT max (tail) | P2 1292 B | ✅ **0.266** | ❌ 0.348 | 0.317 | 0.372 † | – | A |
| 5 | RTT mean | +10 ms netem | ⚪ 10.11 | ⚪ 10.34 | ⚪ 10.19 | 10.07 | – | A |
| 6 | RTT max (tail) | +10 ms netem | ✅ **12.20** | ❌ 12.26 | 12.24 | 12.15 | – | A |
| 6a | RTT mean | P3 1424 B | ✅ **0.234** | ❌ 0.337 | 0.291 | 0.248 † | – | A |
| 6b | RTT mean | P4 2800 B | ✅ **0.249** | ❌ 0.358 | 0.293 | 0.402 † | – | A |
| | **[Throughput](#throughput)** (Mbps) | | | | |  | – | |
| 7 | RELIABLE | P1 76 B | ✅ **107** | ❌ 28.1 | 52.2 | 236.8 † | – | A |
| 8 | RELIABLE | P2 1292 B | ✅ **937** | ❌ 591 | 864 | ✗ † | – | A |
| 9 | RELIABLE | P3 1424 B | ✅ **943** | ❌ 401 | 716 | ✗ † | – | T |
| 10 | RELIABLE | P4 2800 B | ✅ **943** | ❌ 519 | 891 | ✗ † | – | T |
| 11 | BEST_EFFORT | P1, max rate | ✅ **115** | ❌ 44.7 | 66.6 | 53.4 | – | A |
| 11a | BEST_EFFORT | P2 1292 B, max rate | ✅ **938** | ❌ 740 | 911 | 867 | – | A |
| 11b | BEST_EFFORT | P3 1424 B, max rate | ✅ **943** ‡ | ❌ 574 | 828 | ✅ **941** ‡ | – | A |
| 11c | BEST_EFFORT | P4 2800 B, max rate | ✅ **944** ‡ | ❌ 915 | 938 | ✅ **945** ‡ | – | A |
| 12 | RELIABLE KEEP_LAST 64 | P1 | ✅ **117** | ❌ 38.0 | 75.1 | – | – | A |
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
| 72 | delivered msg/s, KEEP_ALL, max rate | Array1k, 0% loss | ✅ **90,849** | ❌ 30,919 | 84,445 | – | – | K |
| 73 | delivered msg/s, KEEP_ALL, max rate | Array1k, 5% loss | ✅ **74,128** | ✗ refused | ❌ 1,807 | – | – | Ke |
| 74 | delivered msg/s, KEEP_ALL, max rate | Array4k, 0% loss | ✅ ‡ **27,434** | ✅ ‡ 27,917 | ✅ ‡ 27,204 | – | – | K |
| 75 | delivered msg/s, KEEP_ALL, max rate | Array4k, 5% loss | ✅ **26,897** | ✗ refused | ❌ 346 | – | – | Ke |

### Legend

- **⚪** deliberately not scored: a fair comparison is not possible in that row (5, 45-47), or, in rows 52-67, the
  first two are within each other's spread (a draw).
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
| `T` | P3/P4 cells with FastDDS `maxMessageSize` 1472 (fair-evaluation setting), same session and method as `A` (`...112506.T_free.txt`). Cell 6 (rows 14, 17, 27, 34, 40) was not in the `09a0437d` run and is still `3ae721aa` without the warm-up (`fair_crosshost_3ae721aa_T6.T_free.txt`; row 17's unshaped base is that session's cell 4, 944 / 555 / 871 Mbps) |
| `G` | p1-p4 gap cells added 2026-09-30 (`results/cmp_p1p4_gap_99033118_2026-09-30.txt`, WIN 38 / DRAW 4 / LOSE 0) |
| `S` | QoS-mechanics sweep 2026-09-24/25 (`experiments/comparison_resweep.sh` at `659013e9`) |
| `L` | liveliness, `liveliness_l2.sh`, 2026-09-27, `0f220ba0`, 20 reps (`results/liveliness_l2e_2026-09-27.txt`) |
| `W` | four-way rmw block wait, `934f90de`, 3 reps (`results/rmw_4way_block_934f90de_2026-09-30.txt`) |
| `J` | four-way rmw poll wait, random phase, RTT at callback, `934f90de`, 7 reps (`results/rmw_4way_poll*_cb_r7_*_2026-10-01.txt`) |
| `Ke` | rmw RELIABLE + KEEP_ALL at equal KEEP_ALL bounds, as `K` but 5 reps, frameworks interleaved, 5% loss only, main `29dff630`, 2026-10-08 (`experiments/rmw_keepall_rig.sh` `EQUAL_BOUND=auto`; `~/rig_results_safe/rmw_keepall_equal_main_29dff630_20261008-134901.{txt,runs,launch.log}`). Until 2026-10-08 rows 73 and 75 were `K`: 70,126 / 1,784 and 26,486 / 339, each rmw at its own default bound |
| `K` | rmw RELIABLE + KEEP_ALL, apex `perf_test` at max rate across the link, 20 s, 3 reps, medians, `ae724688`, rmw_tickle built as the vendor debs are (CMake type None, `-g -O2`), 2026-10-06 (`experiments/rmw_keepall_rig.sh`; `~/rig_results_safe/rmw_keepall_rig_O2_20261006-123017.txt`). First measured at `a8c1cd83` with rmw_tickle at `-O3`: 87,977 / 65,051 / 27,421 / 26,483 |

### Re-measure at `09a0437d`

Every `A` and `T` row except `T` cell 6 comes from `experiments/fair_crosshost_remeasure.sh` at `09a0437d`
(2026-10-08; `~/rig_results_safe/fair_crosshost_09a0437d_20261008-112506.{A_free,A_pinned,T_free,T_pinned}.txt`,
driver log `...112506.driver.log`), the driver of the `3ae721aa` run, now with the equal warm-up and cool-down
(TESTING.md section 5 item 9): throughput is scored over the window (`send_mbps` between 2 s edges; the whole-run rate
is printed beside it and not scored), latency over 100 measured round trips between 4,096-round-trip edges. Samples
before values: `A_free` has 153 RESULT lines, 151 `ok` and `window=ok`, every edge ended on its count except cell 12's
(10 ms netem; both on the 20 s bound, 91 measured round trips per rep, every framework alike). The two VOIDs are FastDDS
as shipped at P4 under 5% loss (cell 6, reps 2-3): its write timed out 86 times at the 100 ms blocking bound and the
run was shorter than its two edges, so cell 6's FastDDS figures rest on one rep and the summary marks them provisional.
`A_pinned` has the same shape (its VOIDs are the same cell's reps 1-2); `T_free` and `T_pinned` are 18 of 18 `ok`.

Totals over the 150 metric-cells both runs score: WIN 136, DRAW/TIE 14 at `3ae721aa` -> WIN 139, DRAW/TIE 11 (the
12 + 2 new `server.recv_mbps` cells, the delivered rate in the window, are all WINs, giving 151 / 11 and 20 / 2). Four
verdicts moved, all `server.loss_pct`, where TickLE lost nothing in either run: cell 1 (P1) TIE -> DRAW (CycloneDDS
and FastDDS lost a few samples in one rep each), cell 5 (P1, 5% loss) TIE -> WIN (FastDDS lost 0.002-0.016%), cell 9
(KEEP_LAST 64) DRAW -> WIN (both vendors lost in every rep; at `3ae721aa` FastDDS's loss printed as 0.0, which the
unrounded count already made a WIN), cell 6 (P4, 5% loss) DRAW -> WIN, provisional (one FastDDS rep). Pinned: WIN
135, DRAW/TIE 15 -> WIN 149, DRAW/TIE 13 of 162.

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

### Latency

Rows 1-6b are from the `09a0437d` campaign: 100 measured round trips per rep (91 in cell 12), every rep
`window=ok`, `reliable_latency` with one ping in flight, the mean of 3 reps (`campaign_summary.py`; this text said
median until 2026-10-08, but the figures were always its mean); tails (rows 2, 4, 6) are the median of the reps'
maxima, each rep's maximum over its measured round trips only. p50 / p99 ms, TickLE, FastDDS, CycloneDDS: P1 0.204 /
0.227, 0.288 / 0.341, 0.253 / 0.288; P2 0.228 / 0.264, 0.311 / 0.345, 0.272 / 0.312; +10 ms 10.00 / 12.20, 10.38 /
12.26, 10.03 / 12.24; P3 0.231 / 0.253, 0.334 / 0.373, 0.285 / 0.325; P4 0.247 / 0.271, 0.356 / 0.396, 0.289 / 0.334.
Every tail is a WIN outside the reps' ranges (maxima, TickLE / CycloneDDS / FastDDS: P1 TickLE 0.241-0.263, CycloneDDS 0.288-0.308, FastDDS
0.327-0.355; P2 0.258-0.308, 0.312-0.332, 0.345-0.367; +10 ms 12.18-12.20, 12.23-12.26, 12.24-12.28). Row 5 stays
unscored (10 ms netem swamps a 0.2 ms RTT, and TickLE's 10.071-10.182 overlaps CycloneDDS's 10.175-10.196).

History. At `3ae721aa` (no warm-up) rows 1-6b were 0.211 / 0.297 / 0.329, tails 0.298 / 0.604 / 10.761, 0.237 /
0.322 / 0.281, tails 0.356 / 0.622 / 0.830, and so on; CycloneDDS's 10.8 ms P1 tail was one first-lap round trip in a
hundred and has not recurred in any warm run. Rows 1, 3 and 6b were then re-measured with the warm-up (`f40a2adf`,
2026-10-07, `~/rig_results_safe/crosshost_latency_warm_f40a2adf_20261007-035608.txt`: 0.209 / 0.285 / 0.260, 0.231 /
0.316 / 0.273, 0.249 / 0.348 / 0.293) and rows 5, 6, 6a with the 20 s edge bound (`9b5491e6`,
`~/rig_results_safe/crosshost_latency_warm2_9b5491e6_20261007-101404.txt`: 10.1 / 10.3 / 10.3, tail 12.1 / 12.3 /
12.2, 0.235 / 0.344 / 0.288); the `09a0437d` run supersedes both under the same rules and agrees with them within the
control (above). Rows 24-25 (CPU per round trip) were 159 / 377 / 226 and 156 / 376 / 224 at `3ae721aa`: the process's
whole CPU over its 100 round trips, start-up included; it is now over all 8,292 round trips of a run, edges included,
which is why every framework fell by 73-78%. All absolute RTTs were taken under the `ondemand` CPU governor, which adds
about 10% (19-21 us); ordering and margins are unaffected because all frameworks ran under it in the same sessions.
zenoh-pico's column here is its TCP arm.

### Throughput

Rows 7-17. `send_mbps`, client, over the window between the 2 s edges, the mean of 3 (the text said median until
2026-10-08; the figures were always `campaign_summary.py`'s mean), RELIABLE + KEEP_ALL with `sent == recv` checked on
every side. The delivered rate in the same window (`server.recv_mbps`) is scored beside it and agrees within 1% for
TickLE in every cell.
11b and 11c are at the link ceiling (‡). **Row 11's FastDDS figure is a send rate it did not fully deliver:** FastDDS
lost 15.5% of samples in that cell on a 2026-10-03 run and 22.2% in the QoS sweep. FastDDS as shipped
(`maxMessageSize` 65,500) at the `T` cells, for reference (`A` cells 3, 4, 6 at `09a0437d`): P3 465 Mbps and 1,677
B/sample; P4 734 Mbps, 3,102 B/sample and 14,932 KB; P4 under 5% loss its write timed out 85-86 times per run at the
100 ms blocking bound, two of three runs ended shorter than their edges (VOID), and the one left sent 3.3 Mbps,
because kernel IP reassembly fails under loss. (Earlier figures, pinned: 533 Mbps; 786 Mbps, 3,101 B/sample, 14,864
KB; 9.9 Mbps with 3,252 of 3,439 delivered.)

**Unpinned against pinned** (same session, `09a0437d`; the pinned figures put each process on cores 1-3, away from the
Pi's interrupt core). Pinned scores WIN 149, DRAW/TIE 13, LOSE 0 of 162; the
verdicts that differ are `server.loss_pct` in cells 1, 5 and 9 (whether a vendor's range touched zero) and row 5's
draw, where pinned TickLE is no longer ahead of CycloneDDS (10.4 against 10.2 ms, ranges overlapping). Unpinning costs every
framework at P1, most of all the two DDS: RELIABLE P1 (row 7) pinned is TickLE 114, FastDDS 37.7, CycloneDDS 89.4 Mbps
(unpinned 0.94x, 0.75x, 0.58x); BEST_EFFORT P1 (row 11) 120, 45.3, 67.9; KEEP_LAST (row 12) 118, 40.0, 87.2. P2-P4
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

Rows 18-27. `cpu_s_per_Msample`, mean of 3 (the text said median until 2026-10-08). Absolute CPU seconds are never a verdict: every cell runs for a fixed
duration, so a framework that sends more burns more. TickLE figures from before 2026-09-26 were an `-O0` build;
every row here asserts `core_build=release` on all sides.

### Memory

Rows 28-34. Peak RSS. Two qualifiers travel with these rows: (1) it is *resident* memory on demand-paged Linux; on a
target without demand paging TickLE's declared static storage is the real figure. (2) Every framework is given the
same KEEP_ALL bound in samples (`campaign_sweep.sh` common_args, derived from TickLE's 512 KiB budget: 405 / 368 /
187 samples at P2 / P3 / P4). FastDDS's ~14 MB is its baseline, not held history: its latency cell, which holds one
sample, peaks at 14,379 KB (row 31). (Until 2026-09-29 FastDDS held up to 4,000 samples, which inflated this margin.)

### Bandwidth

Rows 35-41. Wire bytes per sample from `/proc/net/dev`, so retransmissions and control traffic count. Row 41's per
single-datagram framing includes the 42 B of Ethernet+IP+UDP.

### QoS mechanics

Rows 42-47. Count-based scenarios: late-joiner durability, a burst within and beyond a fixed HISTORY depth of 8,
50 ms DEADLINE, LIVELINESS lease (median detection time from the last received sample; all three match the lease
within 1 ms, hence no winner), 100 ms LIFESPAN. Row 44 is the one scored row: TickLE delivers 147.7 of 160 beyond
depth, both DDS 109 (deterministic, 3/3).

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
  recorded. Medians of 5 reps (range): rmw_tickle 74,128/s (71,324-76,949) and 26,897/s (26,850-27,075), CycloneDDS
  1,807/s (1,727-2,022) and 346/s (322-368). CPU per sample, publisher / subscriber, median: rmw_tickle 13.9 / 11.5 us
  (Array1k) and 31.7 / 24.1 us (Array4k), CycloneDDS 26.6 / 45.6 and 70.3 / 182.4 us. rmw_tickle: no loss and no
  publisher error in any of its 10 runs. Fast DDS's publisher ended on "cannot publish data" in all 10 runs; the
  harness counts 2 Array1k runs as refused and voids the other 8 because the publisher was gone before 2 s, when its
  treatment is read from `/proc` (the launcher's environment shows the profile was set). It does not run these cells.
  The 0% loss rows 72 and 74 were not re-measured at equal bounds beyond the run's 6 s, one-rep dry run.
- **History: the first equal-bound runs (2026-10-07) failed rmw_tickle.** At main `f40a2adf`
  (`~/rig_results_safe/rmw_keepall_equal_20261007-044904.txt`, 3 reps) rmw_tickle's publisher ended in 2 of its 6
  lossy runs (one per topic) with no sample lost; its usable medians were 71,279 and 26,371/s. Cause: a reader ignored
  a heartbeat's request for an answer while its own retry for an open gap was armed. Fixed by `2901121e` (answer a heartbeat that asks, also
  with a retry armed), `e3236048` and `3355e94d` (the answer leaves out repairs still in transit, limited by the
  measured repair transit); the `Ke` run at `29dff630` includes them.
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
  65,051 and 26,483 at `a8c1cd83` (rows 73, 75 now read 74,128 and 26,897 at equal bounds). So with a normal configuration Fast
  DDS does not run this cell: under 5% loss at max rate its publisher ends, and the one setting that keeps it alive
  delivers 760 / 109 msg/s. That is recorded as the result; the cause inside Fast DDS is not pursued.
- **Corrected 2026-10-06: FastDDS at 0% loss is not an incomplete delivery.** This note said it "delivered 22-28%
  fewer samples than it sent" and excluded it (✗). Those 70-80k samples were written before its reader matched
  (~0.7 s): `perf_test` counts every id below the first one received as lost, and a VOLATILE writer rightly does
  not deliver what it wrote before the match. Its `--expected_num_subs` is compiled out in this build, so publishing
  starts before matching for every arm. The rows count delivered samples per second after the start, which this does
  not touch, so FastDDS is scored in rows 72 and 74. CycloneDDS's "lost ~106k" in two Array1k 5% runs is the same
  artefact. At Array1k FastDDS's rate is steady at ~16k/s with ~320 ms latency (its full 5,000-sample history
  draining at that rate); Array4k is at the link's ceiling for all three (‡).
- rmw_tickle is built at the vendors' level (`-g -O2`, no `NDEBUG`) and lost no sample in any of its 12 runs. At 0%
  loss one of FastDDS's three Array1k runs also ended on a write timeout; row 72 is the median of the other two.

### Where TickLE does not come first

- **Poll-wait rows 59 and 62-67 are draws with CycloneDDS**, not wins. In row 67 CycloneDDS's median is lower (0.380
  against 0.391 ms), within the spread. At 100 and 200 us half a poll cycle of waiting, the same for every rmw,
  dominates, so the margin is small.
- **One segment behind, inside a lead:** rmw_tickle's kernel wake (ppoll, then recvfrom) is ~6 us slower than the
  vendors' blocking recvmsg, although it is fastest in every user-space segment of the round trip.
- **Row 5** is a draw by construction.
- **zenoh-pico (reference, unscored) has the better figure in rows 5, 6, 7, 18, 19, 24 and 35**, and ties at the link
  ceiling in 11b-11c. Its P1 TCP lead (row 7) comes from coalescing about eleven samples into each segment (0.094
  packets per sample against TickLE's 1.005), a different mechanism from per-sample datagrams.
- **Same host:** TickLE's segment gains less over its own kernel path than FastDDS's does (9.7x against 11.6x).

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
Full rules, fairness audit and zenoh-pico configuration: [TESTING.md](TESTING.md).

**Versions** (rig, Ubuntu 24.04, aarch64): the native rows use CycloneDDS 11.0.1 (the `rolling` build, linked by
`cyclonedds/build.sh`) with iceoryx 2.0.5 for section 1, and Fast DDS 2.14.6 (ROS jazzy); the rmw rows use jazzy's
`rmw_cyclonedds_cpp` (CycloneDDS 0.10.5) and `rmw_fastrtps_cpp` (Fast DDS 2.14.6). Vendor libraries are the
distribution's packages (CMake build type None, Debian's `-O2`); TickLE's core is built `-O2 -DNDEBUG`. Rows
measured before 2026-10-05 built rmw_tickle as `Release` (`-O3`); later rmw rows build it with the vendors' `-O2`.

## 4. Open work

The to-do list (delivered vendor rates for S1-S3, missing same-host CPU/memory cells, loaned messages, and more) is
in [ROADMAP.md](ROADMAP.md).
