# TickLE performance results

TickLE against FastDDS and CycloneDDS, measured on dedicated hardware. zenoh-pico and rmw_zenoh are shown for
reference. Column order is always **TickLE, FastDDS, CycloneDDS**. Design background is in [DESIGN.md](DESIGN.md),
the ROS 2 layer in [RMW.md](RMW.md), how the numbers are taken in [TESTING.md](TESTING.md), open work in
[ROADMAP.md](ROADMAP.md). Result file paths below are relative to `examples/perf_hil/` unless they start with `~`.

## Summary

- **Cross-host (two Raspberry Pi 5s over 1 GbE):** TickLE is best in every scored row of latency, throughput, CPU,
  memory and bandwidth at P1-P4, with and without injected loss, under identical explicit QoS, against FastDDS both
  as shipped and tuned, with no process pinned to a core. The campaign scored **WIN 136, DRAW/TIE 14, LOSE 0,
  VOID 0** (`A`, 17 cells) and **WIN 18, TIE 2, LOSE 0** (`T`), at `3ae721aa`. Scored on the unrounded loss
  (recomputed from each line's `lost`/`recv`), `A` is **WIN 137, DRAW/TIE 13**: cell 9's (KEEP_LAST 64) server
  `loss_pct` was a DRAW only because FastDDS's 66 and 58 lost samples printed as `loss_pct=0.0`. The RESULT lines
  have printed every scored field with enough digits since 2026-10-06 (`%.6f`; `cpu_s_per_MB` `%.9f`).
- **The largest margin is RELIABLE under 5% loss:** TickLE keeps 93.2% (P1) and 91.6% (P4) of its throughput;
  FastDDS 47.6% / 38.9%, CycloneDDS 5.5% / 1.6% (rows 16-17).
- **Same host (shared memory):** TickLE leads every measured cell, unpinned, with warm-up excluded. At BEST_EFFORT
  KEEP_LAST 1 (the DDS default) TickLE delivered every sample at max rate, CycloneDDS about a fifth and FastDDS
  almost none (S1d-S3d); TickLE's CPU per delivered RELIABLE sample is 1.7 us against 12.0 and 31.1 (S17).
- **rmw layer:** rmw_tickle is first on every block-wait row and every poll-wait row except seven draws with
  CycloneDDS (rows 59, 62-67). It loses no row. Under RELIABLE + KEEP_ALL at 5% loss it delivers 39x (Array1k) and
  78x (Array4k) CycloneDDS's rate with no sample lost; FastDDS's write times out under its default 100 ms bound
  and the run ends (rows 72-75).
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

## 2. Cross-host (two Pis over the rig's link)

**✅ = best, ❌ = worst** in the row. Lower is better for latency, CPU, memory and bandwidth; higher for throughput.

| # | Metric | Condition | TickLE | FastDDS | CycloneDDS | zenoh-pico | rmw_zenoh | meas. |
|---|---|---|---|---|---|---|---|---|
| | **[Latency](#latency)** (ms) | | | | |  | – | |
| 1 | RTT mean | P1 76 B | ✅ **0.209** | ❌ 0.285 | 0.260 | 0.221 † | – | A |
| 2 | RTT max (tail) | P1 76 B | ✅ **0.298** | 0.604 | ❌ 10.761 | 0.336 † | – | A |
| 3 | RTT mean | P2 1292 B | ✅ **0.231** | ❌ 0.316 | 0.273 | 0.246 † | – | A |
| 4 | RTT max (tail) | P2 1292 B | ✅ **0.356** | 0.622 | ❌ 0.830 | 0.372 † | – | A |
| 5 | RTT mean | +10 ms netem | ⚪ 10.1 | ⚪ 10.3 | ⚪ 10.3 | 10.07 | – | A |
| 6 | RTT max (tail) | +10 ms netem | ✅ **12.1** | ❌ 12.3 | 12.2 | 12.15 | – | A |
| 6a | RTT mean | P3 1424 B | ✅ **0.235** | ❌ 0.344 | 0.288 | 0.248 † | – | A |
| 6b | RTT mean | P4 2800 B | ✅ **0.249** | ❌ 0.348 | 0.293 | 0.402 † | – | A |
| | **[Throughput](#throughput)** (Mbps) | | | | |  | – | |
| 7 | RELIABLE | P1 76 B | ✅ **101** | ❌ 29.6 | 50.9 | 236.8 † | – | A |
| 8 | RELIABLE | P2 1292 B | ✅ **938** | ❌ 538 | 830 | ✗ † | – | A |
| 9 | RELIABLE | P3 1424 B | ✅ **943** | ❌ 381 | 714 | ✗ † | – | T |
| 10 | RELIABLE | P4 2800 B | ✅ **944** | ❌ 555 | 871 | ✗ † | – | T |
| 11 | BEST_EFFORT | P1, max rate | ✅ **115** | ❌ 45.7 | 68.4 | 53.4 | – | A |
| 11a | BEST_EFFORT | P2 1292 B, max rate | ✅ **938** | ❌ 715 | 913 | 867 | – | A |
| 11b | BEST_EFFORT | P3 1424 B, max rate | ✅ **943** ‡ | ❌ 604 | 814 | ✅ **941** ‡ | – | A |
| 11c | BEST_EFFORT | P4 2800 B, max rate | ✅ **944** ‡ | ❌ 915 | 938 | ✅ **945** ‡ | – | A |
| 12 | RELIABLE KEEP_LAST 64 | P1 | ✅ **107** | ❌ 41.7 | 83.6 | – | – | A |
| 13 | RELIABLE | P1, **5% loss** | ✅ **94.1** | 14.1 | ❌ 2.78 | ✗ † | – | A |
| 14 | RELIABLE | P4, **5% loss** | ✅ **865** | 216 | ❌ 14.3 | ✗ † | – | T |
| 15 | RELIABLE | P1, 5% reorder | ✅ **100** | ❌ 32.6 | 52.7 | ✗ † | – | A |
| 16 | retention under loss | P1: 5% loss / unshaped | ✅ **93.2%** | 47.6% | ❌ 5.5% | ✗ † | – | A |
| 17 | retention under loss | P4: 5% loss / unshaped | ✅ **91.6%** | 38.9% | ❌ 1.6% | ✗ † | – | T |
| | **[CPU](#cpu)** (cpu_s / Msample) | | | | |  | – | |
| 18 | throughput, client | P1 76 B | ✅ **6.1** | ❌ 24.7 | 7.8 | 2.57 † | – | A |
| 19 | throughput, server | P1 76 B | ✅ **2.8** | ❌ 27.1 | 13.2 | 1.42 † | – | A |
| 20 | throughput, client | P2 1292 B | ✅ **6.6** | ❌ 22.6 | 8.8 | ✗ † | – | A |
| 21 | throughput, server | P2 1292 B | ✅ **3.9** | ❌ 22.0 | 14.6 | ✗ † | – | A |
| 22 | throughput, client | P4 2800 B | ✅ **12.7** | ❌ 43.6 | 14.3 | ✗ † | – | T |
| 23 | throughput, server | P4 2800 B | ✅ **8.4** | ❌ 35.4 | 19.8 | ✗ † | – | T |
| 24 | latency | P1 76 B | ✅ **159** | ❌ 377 | 226 | 121.1 | – | A |
| 25 | latency | P2 1292 B | ✅ **156** | ❌ 376 | 224 | 144.6 | – | A |
| 26 | throughput, client | P1, 5% loss | ✅ **6.6** | ❌ 27.9 | 13.9 | ✗ † | – | A |
| 27 | throughput, client | P4, 5% loss | ✅ **15.2** | ❌ 66.9 | 40.2 | ✗ † | – | T |
| | **[Memory](#memory)** (peak RSS, KB) | | | | |  | – | |
| 28 | throughput, client | P1 76 B | ✅ **2,048** | ❌ 15,911 | 5,359 | 2,127 † | – | A |
| 29 | throughput, client | P2 1292 B | ✅ **2,336** | ❌ 15,063 | 6,407 | ✗ † | – | A |
| 30 | throughput, client | P4 2800 B | ✅ **2,385** | ❌ 13,991 | 5,600 | ✗ † | – | T |
| 31 | latency, client | P1 76 B | ✅ **1,807** | ❌ 14,377 | 4,848 | 2,135 | – | A |
| 32 | throughput, server | P1 76 B | ✅ **1,949** | ❌ 14,708 | 6,281 | 2,127 † | – | A |
| 33 | throughput, server | P4 2800 B | ✅ **2,971** | ❌ 14,237 | 4,939 | ✗ † | – | T |
| 34 | throughput, server | P4, 5% loss | ✅ **2,968** | ❌ 14,351 | 6,011 | ✗ † | – | T |
| | **[Bandwidth](#bandwidth)** (wire B / sample) | | | | |  | – | |
| 35 | wire bytes | P1 76 B | ✅ **138** | ❌ 311 | 184 | 93.6 † | – | A |
| 36 | wire bytes | P2 1292 B | ✅ **1,355** | ❌ 1,503 | 1,397 | ✗ † | – | A |
| 37 | wire bytes | P3 1424 B | ✅ **1,487** | ❌ 1,866 | 1,585 | ✗ † | – | T |
| 38 | wire bytes | P4 2800 B | ✅ **2,920** | ❌ 3,462 | 2,950 | ✗ † | – | T |
| 39 | wire bytes | P1, 5% loss | ✅ **143** | ❌ 290 | 193 | ✗ † | – | A |
| 40 | wire bytes | P4, 5% loss | ✅ **3,057** | ❌ 4,223 | 3,213 | ✗ † | – | T |
| 41 | framing overhead | single datagram | ✅ **62.0** | ❌ 235.0 | 108.0 | 63.0 § | – | A |
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
| 73 | delivered msg/s, KEEP_ALL, max rate | Array1k, 5% loss | ✅ **70,126** | ✗ refused | ❌ 1,784 | – | – | K |
| 74 | delivered msg/s, KEEP_ALL, max rate | Array4k, 0% loss | ✅ ‡ **27,434** | ✅ ‡ 27,917 | ✅ ‡ 27,204 | – | – | K |
| 75 | delivered msg/s, KEEP_ALL, max rate | Array4k, 5% loss | ✅ **26,486** | ✗ refused | ❌ 339 | – | – | K |

### Legend

- **⚪** deliberately not scored: a fair comparison is not possible in that row (5, 45-47), or, in rows 52-67, the
  first two are within each other's spread (a draw).
- **zenoh-pico is reference and never scored**: no ✅/❌ toward totals, no WIN/DRAW/LOSE. Its cells come from its own
  session (`Z`, `results/zenoh_cells_31d58011_2026-09-29.txt`), so its margins are looser than a within-row one.
- **`†`** measured over TCP, the only configuration where zenoh-pico's reliability is real (its RELIABLE means
  monotonic sequence numbers, not retransmission). Untagged zenoh-pico cells are its UDP-multicast best-effort arm.
- **`✗`** in rows 73 and 75: the vendor's publisher ended every run (Fast DDS's KEEP_ALL write timed out; see the rmw
  layer notes).
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
| `A` | aligned 17-cell native campaign (`campaign_sweep.sh`), 3 reps, medians, FastDDS as shipped, no pinning, frameworks' order rotated; `3ae721aa`, 2026-10-06 (`experiments/fair_crosshost_remeasure.sh`; `~/rig_results_safe/fair_crosshost_3ae721aa_20261006-080418.A_free.txt`). Until 2026-10-06 these rows were the pinned 12-cell campaign on `9dbffd40` (`results/cmp_A_9dbffd40_2026-09-29.txt`) and, for 6a-6b and 11a-11c, `G` |
| `T` | P3/P4 cells with FastDDS `maxMessageSize` 1472 (fair-evaluation setting), same session and method as `A` (`...080418.T_free.txt`; cell 6 `fair_crosshost_3ae721aa_T6.T_free.txt`) |
| `G` | p1-p4 gap cells added 2026-09-30 (`results/cmp_p1p4_gap_99033118_2026-09-30.txt`, WIN 38 / DRAW 4 / LOSE 0) |
| `S` | QoS-mechanics sweep 2026-09-24/25 (`experiments/comparison_resweep.sh` at `659013e9`) |
| `L` | liveliness, `liveliness_l2.sh`, 2026-09-27, `0f220ba0`, 20 reps (`results/liveliness_l2e_2026-09-27.txt`) |
| `W` | four-way rmw block wait, `934f90de`, 3 reps (`results/rmw_4way_block_934f90de_2026-09-30.txt`) |
| `J` | four-way rmw poll wait, random phase, RTT at callback, `934f90de`, 7 reps (`results/rmw_4way_poll*_cb_r7_*_2026-10-01.txt`) |
| `K` | rmw RELIABLE + KEEP_ALL, apex `perf_test` at max rate across the link, 20 s, 3 reps, medians, `ae724688`, rmw_tickle built as the vendor debs are (CMake type None, `-g -O2`), 2026-10-06 (`experiments/rmw_keepall_rig.sh`; `~/rig_results_safe/rmw_keepall_rig_O2_20261006-123017.txt`). First measured at `a8c1cd83` with rmw_tickle at `-O3`: 87,977 / 65,051 / 27,421 / 26,483 |

### Latency

Rows 1, 3 and 6b were re-measured on 2026-10-07 with the equal warm-up and cool-down every framework now drops
(`campaign_sweep.sh` CELLS 10 11 17, `f40a2adf`, unpinned;
`~/rig_results_safe/crosshost_latency_warm_f40a2adf_20261007-035608.txt`; p50 / p99 ms there: P1 0.207 / 0.239, 0.282 / 0.324, 0.255 / 0.296; P2 0.228 / 0.261, 0.312 / 0.363, 0.270 / 0.308;
P4 0.248 / 0.271, 0.346 / 0.378, 0.289 / 0.330). The same run could not refresh rows 5-6 (10 ms netem: TickLE's
4,096-round-trip warm-up outran the cell's timeout) or 6a (TickLE's server outlived its run and the leftover guard
voided it) - harness defects, being fixed. Rows 5, 6 and 6a were then re-measured with both fixed (`campaign_sweep.sh`
CELLS 12 16, `9b5491e6`, unpinned; `~/rig_results_safe/crosshost_latency_warm2_9b5491e6_20261007-101404.txt`; 3
reps, every one `window=ok`). Cell 16 (P3) ended both edges on the count and measured 100 round trips per rep; cell 12
(10 ms netem) ended both on the 20 s bound (~1,760-1,780 round trips) and measured 91 per rep, every framework alike,
so its p99 is the maximum. p50 / p99 ms, TickLE, FastDDS, CycloneDDS: +10 ms 10.06 / 12.13, 10.48 / 12.27, 10.37 /
12.24; P3 0.233 / 0.251, 0.342 / 0.386, 0.284 / 0.314. Client CPU (cpu_s / Msample): 38.5, 146, 57.4 and 36.9,
113, 61.0. Row 5 stays unscored (the ranges overlap as well); row 6's tail is a WIN outside the reps' ranges (TickLE
12.11-12.21, CycloneDDS 12.22-12.25, FastDDS 12.23-12.29); CycloneDDS's former 41.2 ms tail did not recur.
Rows 1-6b. `reliable_latency`, one ping in flight, 100 round trips per rep, median of 3; tails (rows 2, 4, 6) are
the median of the reps' maxima. Every client now paces alike (the next ping one interval after the reply) and
prints the median and p99 too: at P1 they are TickLE 0.207 / 0.243, FastDDS 0.286 / 0.475, CycloneDDS 0.255 / 0.303
ms, so row 2's CycloneDDS 10.8 ms is one round trip in a hundred, which pinning hid (0.72 ms pinned). All absolute RTTs were
taken under the `ondemand` CPU governor, which adds about 10% (19-21 us); ordering and margins are unaffected because
all frameworks ran under it in the same sessions. Row 5's mean is a draw by construction (10 ms netem swamps a 0.2 ms
RTT); row 6's tail still separates. zenoh-pico's column here is its TCP arm.

### Throughput

Rows 7-17. `send_mbps`, client, median of 3, RELIABLE + KEEP_ALL with `sent == recv` checked on every side.
11b and 11c are at the link ceiling (‡). **Row 11's FastDDS figure is a send rate it did not fully deliver:** FastDDS
lost 15.5% of samples in that cell on a 2026-10-03 run and 22.2% in the QoS sweep. FastDDS as shipped
(`maxMessageSize` 65,500) at the `T` cells, for reference: P3 533 Mbps and 1,677 B/sample; P4 786 Mbps, 3,101
B/sample and 14,864 KB; P4 under 5% loss 9.9 Mbps, and it **did not deliver everything** within the drain cap (3,252
of 3,439), because kernel IP reassembly fails under loss.

**Unpinned against pinned** (same session, `3ae721aa`; the pinned figures put each process on cores 1-3, away from the
Pi's interrupt core). No verdict changes (pinned: WIN 135, DRAW/TIE 15, LOSE 0). Unpinning costs every framework at
P1, most of all the two DDS: RELIABLE P1 (row 7) pinned is TickLE 115, FastDDS 41.1, CycloneDDS 93.5 Mbps (unpinned
0.88x, 0.72x, 0.54x); BEST_EFFORT P1 (row 11) 120, 48.5, 71.2; KEEP_LAST (row 12) 118, 42.3, 89.8. P2-P4 are at or
near the link and move by under 2%, except FastDDS RELIABLE P2-P4 (641, 433, 621 pinned). Retention under loss
(rows 16-17) pinned: 88.7% / 92.2%, 32.1% / 34.0%, 3.2% / 1.6%. CPU per sample at P1 (rows 18-19) pinned: 5.31 / 2.50,
18.9 / 19.3, 6.86 / 10.9. Raw files: `~/rig_results_safe/fair_crosshost_3ae721aa_*` (`A_pinned`, `T_pinned`).

### RELIABLE under loss

Rows 13-17. `tc netem` on the client's egress, all three under the same RELIABLE + KEEP_ALL contract, every writer
waiting for acknowledgements at teardown, so net loss is 0 for all three and the difference is cost. Retention is
5%-loss throughput over unshaped. zenoh-pico is `✗` here: its best-effort arm keeps 98.2%/99.6% only because dropping
is free, so that figure must not stand beside a recovered one.

### CPU

Rows 18-27. `cpu_s_per_Msample`, median of 3. Absolute CPU seconds are never a verdict: every cell runs for a fixed
duration, so a framework that sends more burns more. TickLE figures from before 2026-09-26 were an `-O0` build;
every row here asserts `core_build=release` on all sides.

### Memory

Rows 28-34. Peak RSS. Two qualifiers travel with these rows: (1) it is *resident* memory on demand-paged Linux; on a
target without demand paging TickLE's declared static storage is the real figure. (2) Every framework is given the
same KEEP_ALL bound in samples (`campaign_sweep.sh` common_args, derived from TickLE's 512 KiB budget: 405 / 368 /
187 samples at P2 / P3 / P4). FastDDS's ~14 MB is its baseline, not held history: its latency cell, which holds one
sample, peaks at 14,377 KB (row 31). (Until 2026-09-29 FastDDS held up to 4,000 samples, which inflated this margin.)

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

**Rows 72-75 (rmw KEEP_ALL, `K`):** apex `perf_test` publisher alone on one Pi and subscriber alone on the other,
RELIABLE + KEEP_ALL, `-r 0`, `tc netem` loss on the publisher's egress, each arm identified by `/proc/PID/maps`.
rmw_tickle lost no sample in any of its 12 runs (`gap_evicted` 0, no give-up). Every rmw blocks a full KEEP_ALL
publisher for 100 ms by default (`max_blocking_time`; ROS 2 QoS has no field for it).

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
  65,051 and 26,483 at `a8c1cd83` (rows 73, 75 now read 70,126 and 26,486). So with a normal configuration Fast
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
