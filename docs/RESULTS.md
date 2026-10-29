# TickLE performance results

**TL;DR.** TickLE against FastDDS and CycloneDDS on two Raspberry Pi 5s; zenoh is reference only and never scored.
- Core, cross-host: WIN 139, DRAW 11, LOSE 0 of 150. Core, same host: WIN 18 of 18.
- ROS 2 (rmw), cross-host: WIN 21, DRAW 6, LOSE 1 of 28 (row 63c). Same host: WIN 52 of 52.
- Large messages (L4): WIN 58, DRAW 21, LOSE 1 of 80: subscriber CPU per MB against FastDDS at 4 MB, 15 Hz.
- Latency is measured from creation time, throughput is goodput, and nothing is pinned.

Full text before this shortening (per-run histories, traces, every rule's reading): `git show e6b23b63:docs/RESULTS.md`.
Older still (provenance prose, pinned figures, zenoh columns): `git show a89aefe2:docs/RESULTS.md`.

## At a glance

| headline | TickLE | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| **Core, cross-host** ([section 2](#2-cross-host-two-pis-over-the-rigs-link)) | | | | |
| RTT p50 from creation, P1, ms | **0.211** | 0.346 | 0.312 | 1c |
| RELIABLE goodput, P1, Mbps | **111** | 24.0 | 50.7 | 7 |
| RELIABLE throughput kept at 5% loss, P1 / P4 | **84.9% / 94.9%** | 44.6% / ◊ | 4.9% / 1.5% | 16-17 |
| **Core, same host** ([section 1](#1-same-host-shared-memory)) | | | | |
| BEST_EFFORT goodput, P2, Mbps | **20,303** | 0.25 | 915 | S1 |
| RTT mean from creation, P3, 200/s, ms | **0.037** | 0.158 | ◊ (0.132) | S9c |
| **rmw, cross-host** ([rmw layer](#rmw-layer)) | | | | |
| RTT mean from creation, block wait, Bench 64 B, ms | **0.245** | 0.383 | 0.335 | 48c |
| delivered msg/s, RELIABLE KEEP_ALL, Array1k, 5% loss | **74,129** | does not run | 1,808 | 73 |
| **rmw, same host** ([rmw layer, same host](#rmw-layer-same-host)) | | | | |
| RTT mean from creation, block wait, Bench 64 B, us | **40.2** | 190.1 | 171.7 | R1c |
| **rmw, large messages** ([L4](#rmw-large-messages-l4-cross-host)); FastDDS as shipped / 1472 | | | | |
| delivered, Array1m RELIABLE 30 Hz, 5% loss | **100%** | 0% / 100% | 2.4-4.0% | I1L |
| latency median from creation, Array4m 30 Hz (saturating), ms | **55.8** | 2,026 / 7,599 | 1,900 | I4s |

Campaign `T` (FastDDS `maxMessageSize` 1472): WIN 18, DRAW 2, LOSE 0 of 20. QoS row 44 WIN.

**Corrected 2026-10-28:** this table showed rows 7, 16-17, S1 and S9c from older sessions (rows 7 and 16-17 from `A`
at `09a0437d`, S1 and S9c from `SF` / `SC`) while the section tables below had been re-measured (`D`, `SB`, `SB9`).
It now repeats the section tables.

### Every LOSE

- **L4 I4 best effort (Array4m 15 Hz), subscriber CPU per MB against FastDDS as shipped**, 2.20 against 2.11 ms/MB
  (+4.5%, `HP`): the 4 MB in-place buffer is larger than the Pi 5's 2 MB L3. THP, `nt`, `rev`, `ntrev` and no in-place
  read changed nothing (`i4be_cpu_gap.sh`).
- **rmw row 63c (Array1k RELIABLE, 100 us poll) against CycloneDDS**, 0.4006 against 0.3957 ms (interquartile rule;
  DRAW on range and on the stamp). CycloneDDS's `spin_some` sleeps ~42 us in a futex each call (zero timeout stretched
  by the 50 us default timer slack) and takes replies that arrive meanwhile; rmw_tickle's makes no syscall and returns
  in 8.5 us. At busy poll rmw_tickle is 13.6 us faster. Kept as measured (user, 2026-10-18).

### Every DRAW

- **Row 5c, +10 ms netem:** netem swamps a 0.2 ms RTT (one DRAW in `A`).
- **Loss TIEs** (10 in `A`, 2 in `T`): TickLE lost nothing; each vendor lost nothing or had a range touching zero.
- **rmw 59c, 61c, 65c-67c against CycloneDDS:** at 50-200 us poll sleeps the poll cycle dominates (see 63c).
- **rmw row 74, Array4k KEEP_ALL at 0% loss:** all three at the link ceiling.
- **L4, 21 letters:** delivered % where everyone delivered 100% (13); against FastDDS as shipped, CPU per MB at I1,
  I1 best effort, I4 RELIABLE and I4s (8).

**Behind inside a lead (not scored):**
- On the pong Pi rmw_tickle's receive returns **+2.0 / +1.1 us** after CycloneDDS / FastDDS (`KH2B`, 2026-10-24, P
  `e9efea98`, `TT_HAL_LIBC_CALLS` build, tap -> receive return p50 15.0 against 13.0 / 13.9 us). The rest is the
  io_uring DEFER_TASKRUN path (blocking recvmsg 8.43 against the ring 9.30 us); no user-space change found. The whole
  tap -> callback is shorter: 24.9-26.4 against 38.8-39.7 / 49.4-53.6 us (`KH2`, `132548dd`). Corrected 2026-10-21
  (was "kernel wake ~6 us slower") and 2026-10-23 (`KH`'s ~4-5 us -> `KH2`'s +2.4 / +1.3).
- Hybrid AUTO's p99 is above forced MTU at 0.01% and 0.1% loss ([Hybrid](#hybrid-transport-large-samples-rig)).
- zenoh-pico 1.10.2 (reference) beats TickLE in no throughput or CPU cell; its RELIABLE latency p99 is just below TickLE's at P1 and P3, and its TCP session dies at max rate and under loss ([zenoh](#zenoh-pico-and-rmw_zenoh-reference-not-scored)).

## How to read these numbers

- **Throughput is goodput**: complete, unique samples the receiving application got in the measured window.
- **Latency is from creation time**: from when the sample or ping was due. The send-stamp figure is a diagnostic.
- **CPU, memory and wire bytes are per delivered sample** (or per MB, or per answered round trip). Nothing is pinned.
- **WIN** when every TickLE rep beats every rep of every scored vendor, **LOSE** when a vendor's reps all beat TickLE's.
- **◊ DELIVERY FAILED**: a vendor delivered less than it sent in a RELIABLE cell; in parentheses, not scored.
- ⚪ not scored; ✗ refused or transport died (never a figure); ‡ at the ~940 Mbps link ceiling (or iox-roudi
  included, section 1).
- **src** is a code from [Sources](#3-sources). Compare figures within one session. Rules: [TESTING.md](TESTING.md).

## 1. Same host (shared memory)

One rig Pi, each framework on its own shared memory (FastDDS data-sharing as shipped, CycloneDDS with iceoryx), 3
reps. Every scored row is a WIN.

| # | metric | TickLE | FastDDS | CycloneDDS | src |
|---|---|---:|---:|---:|---|
| | **Goodput, Mbps** | | | | |
| S1 | BEST_EFFORT KEEP_LAST 1, max rate, P2 1292 B | **20,303** | 0.25 | 915 | SB |
| S2 | BEST_EFFORT KEEP_LAST 1, max rate, P3 1424 B | **21,284** | 0.48 | 1,025 | SB |
| S3 | BEST_EFFORT KEEP_LAST 1, max rate, P4 2800 B | **24,773** | 1.20 | 1,876 | SB |
| S4 | RELIABLE, P2 | **13,949** | 977 | ◊ (2,248) | SB |
| S5 | RELIABLE, P3 | **14,957** | 1,086 | ◊ (2,385) | SB |
| S6 | RELIABLE, P4 | **15,247** | 1,879 | ◊ (4,552) | SB |
| | **RTT mean from creation, ms** (one ping in flight, 200/s unless noted) | | | | |
| S7c | P2 | **0.037** | 0.161 | 0.131 | SB |
| S8c | P2, 20/s | **0.034** | 0.179 | 0.141 | SB |
| S9c | P3 | **0.037** | 0.158 | ◊ (0.132) | SB9 |
| S10c | P4 | **0.041** | 0.156 | 0.126 | SB |
| | **CPU, us** | | | | |
| S11 | publisher per delivered sample, BEST_EFFORT P4 max rate | **0.907** | 17,311 | 12.04 ‡ | SB |
| S12 | client per round trip, P2 20/s | **22.8** | 85.1 | 52.1 | SB |
| S17 | client + server per delivered sample, RELIABLE P3 | **1.51** | 30.38 | ◊ (11.99) | SB |
| S18 | client + server per round trip, P3 200/s | **36.2** | 166.0 | 94.1 ‡ | SB |
| | **Memory, MB** | | | | |
| S13 | publisher peak RSS, BEST_EFFORT P4 | **3.3** | 15.4 | – | SB |
| S19 | client + server peak RSS, RELIABLE P3 | **8.5** | 46.9 | ◊ (239.2 ‡) | SB |
| | **Mixed: one publisher, a subscriber on its host and one across the link, k samples/s** | | | | |
| S15 | BEST_EFFORT, local / remote | **90.7 / 90.7**, loss 0 | 53.1 / 54.6, loss 4-22% | 49.7 / 70.6, loss 20-33% / 0.1% | MX |
| S16 | RELIABLE, local / remote | **90.7 / 90.7** | 30.8 / 30.8 | 71.7 / 71.7 | MX |
| S14 | wire bytes per sample, every cell (not scored) | 0 | 0 | 0 | |

- **`SB`, 2026-10-19, `60f49f6a`**, every framework in rotated order per rep; BE P4 (S3, S11, S13) 5 reps. Against
  TickLE's previous figures S1-S3 -1.2..-3.2% (CycloneDDS -2.0..+1.2% in the same session), S4-S6 +1.3..+2.2%.
  The QoS work did not cause the drop: A/B `QB2` (16 reps an arm) read P2-P4 goodput -0.21%, +0.24%, -0.19%.
- **S9c** re-run alone (`SB9`): `SB`'s R4 control missed by 0.2 us.
- **◊ CycloneDDS** lost 0.02-0.07% of RELIABLE throughput in every rep; 1 of 10,140 round trips in one S9c rep.
  **‡** CycloneDDS includes iox-roudi (212 MB RSS, 1.6 us CPU per round trip).
- **FastDDS delivers ~0% in S1-S3**, a rate-dependent overrun at BEST_EFFORT KEEP_LAST 1.
- **Corrected 2026-10-18:** S7c-S9c, S12, S18 replace `596e9c9c`'s (S7c-S9c 0.082 ms): the polling thread's 1 ns
  timer slack (`119d14bd`) cut TickLE's ping delay from ~60 us to 15 us. S10c 0.086 -> 0.041 ms in `SB` for the same reason.
- **Corrected 2026-10-17:** S1-S3 were send rates until goodput (FastDDS S1 6,370 -> 0.31 Mbps).

### rmw layer, same host

`rclcpp` on one Pi, each rmw on its shipped same-host transport; RTT by `rmw_perf_pingpong` at 1 ms, throughput by
apex `perf_test`; 3 reps. Every row is a WIN.

| # | metric, cell | rmw_tickle | FastDDS | CycloneDDS | src |
|---|---|---:|---:|---:|---|
| | **RTT mean from creation, us** | | | | |
| R1c | block wait, Bench 64 B, BEST_EFFORT | **40.2** | 190.1 | 171.7 | RS2 |
| R2c | block wait, Bench 64 B, RELIABLE | **41.4** | 211.3 | 166.7 | RS2 |
| R3c | block wait, Array1k, BEST_EFFORT | **41.8** | 193.7 | 169.7 | RS2 |
| R4c | block wait, Array1k, RELIABLE | **43.1** | 214.9 | 171.6 | RS2 |
| R5c | poll wait 100 us, Bench, BEST_EFFORT | **206.7** | 245.5 | 241.1 | RS2 |
| R6c | poll wait 100 us, Bench, RELIABLE | **209.4** | 256.4 | 243.0 | RS2 |
| R7c | poll wait 100 us, Array1k, BEST_EFFORT | **212.8** | 253.1 | 241.7 | RS2 |
| R8c | poll wait 100 us, Array1k, RELIABLE | **210.9** | 263.3 | 244.1 | RS2 |
| | **CPU, ping + pong, us per round trip** | | | | |
| R9 | block, Bench RELIABLE | **54.1** | 255.1 | 147.4 | RS2 |
| R10 | block, Array1k RELIABLE | **56.3** | 258.7 | 154.4 | RS2 |
| | **Peak RSS, larger process, kB** | | | | |
| R11 | RTT, Bench BEST_EFFORT block | **14,700** | 27,168 | 16,136 | RS2 |
| R12 | RTT, Bench RELIABLE block | **14,736** | 27,184 | 16,140 | RS2 |
| R13 | throughput, Array1k RELIABLE | **16,536** | 35,972 | 17,152 | RS2 |
| | **Delivered msg/s, max rate** | | | | |
| R14 | Array1k BEST_EFFORT KEEP_LAST 1 | **252,441** | 5,040 | 70,165 | RS2 |
| R15 | Array4k BEST_EFFORT KEEP_LAST 1 | **133,876** | 5,691 | 64,258 | RS2 |
| R16 | Array1k RELIABLE KEEP_ALL | **342,075** | 2,581 | 60,976 | RS2 |
| R17 | Array4k RELIABLE KEEP_ALL | **243,688** | 16,967 | 46,379 | RS2 |
| | **CPU, pub + sub, us per delivered sample** | | | | |
| R18 | Array1k BEST_EFFORT | **7.84** | 527.79 | 33.35 | RS2 |
| R19 | Array4k BEST_EFFORT | **14.62** | 464.87 | 36.20 | RS2 |
| R20 | Array1k RELIABLE | **3.98** | 568.39 | 35.82 | RS2 |
| R21 | Array4k RELIABLE | **6.52** | 85.42 | 40.07 | RS2 |

- **The 31 cells not shown** (RTT p50 and p99, CPU and RSS of the other RTT cells, three RSS cells) are WIN too.
- **`RS2`, 2026-10-21, `69f394cc`, slot loans off**, one run for every row; 0 VOID, all 8 controls PASS. Against
  rmw_tickle's own previous figures: block RTT -10..-12% (the executor hold-over `58bd6b1a`), msg/s +57..+82%. FastDDS
  BEST_EFFORT msg/s spans 3,383-17,457 across reps (R14 median 5,040). R13 +536 kB: no single commit (`ru_maxrss` at
  exit, +-200..400 kB).

### Loaned messages (PC)

PC, rmw level (`LN`): copy, loan and in-ring loan arms read **NO DIFFERENCE** in every comparison.

## 2. Cross-host (two Pis over the rig's link)

| # | metric | TickLE | FastDDS | CycloneDDS | verdict | src |
|---|---|---:|---:|---:|:-:|---|
| | **[Latency from creation time](#latency-from-creation-time-2026-10-17), RTT p50 ms** | | | | | |
| 1c | P1 76 B | **0.211** | 0.346 | 0.312 | WIN | C |
| 3c | P2 1292 B | **0.237** | 0.378 | 0.328 | WIN | C |
| 5c | P1, +10 ms netem | 10.445 | 10.239 | 10.257 | ⚪ | C |
| 6ac | P3 1424 B | **0.241** | 0.391 | 0.337 | WIN | C |
| 6bc | P4 2800 B | **0.259** | 0.403 | 0.347 | WIN | C |
| | **RTT max (tail), send stamp, ms** | | | | | |
| 2 | P1 | **0.252** | 0.342 | 0.296 | WIN | A |
| 4 | P2 | **0.266** | 0.348 | 0.317 | WIN | A |
| 6 | +10 ms netem | **12.20** | 12.26 | 12.24 | WIN | A |
| | **Goodput, Mbps** | | | | | |
| 7 | RELIABLE, P1 | **111** | 24.0 | 50.7 | WIN | D |
| 8 | RELIABLE, P2 | **937** | 579 | 864 | WIN | D |
| 9 | RELIABLE, P3 | **943** | 407 | 721 | WIN | D |
| 10 | RELIABLE, P4 | **943** | 540 | 891 | WIN | D |
| 11 | BEST_EFFORT, P1, max rate | **115** | 28.8 | 62.1 | WIN | A |
| 11a | BEST_EFFORT, P2 | **938** | 575 | 906 | WIN | A |
| 11b | BEST_EFFORT, P3 | **943** ‡ | 560 | 654 | WIN | A |
| 11c | BEST_EFFORT, P4 | **944** ‡ | 902 | 937 | WIN | A |
| 12 | RELIABLE KEEP_LAST 64, P1 | **114** | 36.4 | 76.3 | WIN | D |
| 13 | RELIABLE, P1, 5% loss | **94.2** | 10.7 | 2.47 | WIN | D |
| 14 | RELIABLE, P4, 5% loss | **895** | ◊ (212) | 13.5 | WIN | D |
| 15 | RELIABLE, P1, 5% reorder | **100** | 29.3 | 56.5 | WIN | D |
| 16 | kept at 5% loss / unshaped, P1 | **84.9%** | 44.6% | 4.9% | WIN | D |
| 17 | kept at 5% loss / unshaped, P4 | **94.9%** | ◊ | 1.5% | WIN | D |
| | **CPU, cpu_s per Msample** | | | | | |
| 18 | throughput client, P1 | **5.6** | 26.1 | 7.7 | WIN | D |
| 19 | throughput server, P1 | **2.4** | 30.5 | 13.1 | WIN | D |
| 20 | throughput client, P2 | **6.6** | 21.0 | 8.8 | WIN | D |
| 21 | throughput server, P2 | **3.5** | 21.2 | 14.6 | WIN | D |
| 22 | throughput client, P4 | **12.7** | 45.0 | 14.3 | WIN | D |
| 23 | throughput server, P4 | **7.1** | 35.6 | 20.0 | WIN | D |
| 24 | latency, P1 | **34.3** | 102 | 52.9 | WIN | A |
| 25 | latency, P2 | **35.0** | 103 | 54.9 | WIN | A |
| 26 | throughput client, P1, 5% loss | **6.6** | 28.7 | 13.5 | WIN | D |
| 27 | throughput client, P4, 5% loss | **14.4** | ◊ | 42.4 | WIN | D |
| | **Peak RSS, KB** | | | | | |
| 28 | throughput client, P1 | **2,116** | 15,952 | 5,386 | WIN | D |
| 29 | throughput client, P2 | **2,406** | 15,120 | 6,398 | WIN | D |
| 30 | throughput client, P4 | **2,460** | 14,029 | 5,586 | WIN | D |
| 31 | latency client, P1 | **1,813** | 14,379 | 4,916 | WIN | A |
| 32 | throughput server, P1 | **2,117** | 15,249 | 6,789 | WIN | D |
| 33 | throughput server, P4 | **2,008** | 14,259 | 4,955 | WIN | D |
| 34 | throughput server, P4, 5% loss | **2,562** | ◊ | 6,849 | WIN | D |
| | **Wire bytes per sample** | | | | | |
| 35 | P1 | **138** | 326 | 183 | WIN | D |
| 36 | P2 | **1,355** | 1,503 | 1,397 | WIN | D |
| 37 | P3 | **1,487** | 1,866 | 1,585 | WIN | D |
| 38 | P4 | **2,920** | 3,461 | 2,950 | WIN | D |
| 39 | P1, 5% loss | **143** | 292 | 194 | WIN | D |
| 40 | P4, 5% loss | **3,019** | ◊ | 3,218 | WIN | D |
| 41 | framing, one datagram (row 35 - 76 B) | **62.0** | 250.0 | 107.0 | WIN | D |
| | **QoS mechanics** | | | | | |
| 42 | DURABILITY late join, durable / volatile | 20/20, 0/20 | 20/20 | 20/20 | ⚪ | S |
| 43 | HISTORY within depth 8: age at take, round trip p50 (us) | **200-201** | 304-317 | 281-282 | WIN | X2 |
| 44 | HISTORY beyond depth 8: age at take, round trip p50 (us); taken of 160 | **199-200; 108** | 302-312; 108 | 278-283; 108 | WIN | X2 |
| 45 | DEADLINE 50 ms detection | works | works | works | ⚪ | S |
| 46 | LIVELINESS lease 2.0 s, detection | 2000.1 ms | 1999.1 ms | 2000.1 ms | ⚪ | L |
| 47 | LIFESPAN 100 ms expiry | works | works | works | ⚪ | S, LS |

Rows 48c-75 are in [rmw layer](#rmw-layer). Send-stamp means (rows 1, 3, 5, 6a, 6b, 48-67) are diagnostics, in the
older text.

- **`D`, 2026-10-20, `d58873cf`** (after the QoS work): rows 7-10, 12-23, 26-30, 32-41; 12 reps, rotated order,
  unpinned, 0 VOID. A sender on CPU 0 (eth0's interrupt) reads low (TickLE P1 93.8 against 114.8 Mbps, 2 of 12 reps);
  the medians include both. FastDDS DELIVERY FAILED in c6 (one rep missed 34%), so rows 14, 17, 27, 34, 40 score
  CycloneDDS only. c6's retransmitted / sent is 0.167-0.175.
- **Corrected 2026-10-20, row 47:** TickLE's "works" came from a harness that could not fail (its server joined late,
  so "lost" counted samples sent before the join). Reshaped to stall and drop at take as the DDS twins; on the rig
  (`LS`) 45 / 70 / 95 lost at pause 1.0 / 1.5 / 2.0, every one expired, equal to CycloneDDS in every rep.
- **Re-scored 2026-10-19, rows 43-44:** they counted delivered samples; KEEP_LAST promises the newest, so they now read
  a sample's age at take, by round trip on the sender's clock (an echo over a plain UDP socket; control: that socket's
  own RTT, 195-199 us for all three).
- **Corrected 2026-10-20, row 44's count:** "147-148 of 160" (`X`) was when TickLE's Subscriber joined, not the depth.
  Re-measured with the reshaped server (`X2`, `6ddcb066`): all three take 108 of 160 (a TIE); age p50 WIN.
- **Corrected 2026-10-17:** rows 11-12 were send rates (FastDDS row 11 44.7 -> 28.8 Mbps); rows 1c-6bc replace a
  `4fca013b` run where TickLE lost 1c, 3c and 6bc (its ping left 111 us late; `596e9c9c`'s `timerfd` wait made it 11 us).

### Latency from creation time (2026-10-17)

R4 control: where nothing waits at the sender, creation and stamp differ only by the sender's timer wake `g`; it passed
in every cell. `C`: 100 round trips per rep; TickLE's `g` 11-12 us, the DDS clients' 58-61 us. Tails 2, 4, 6 are not
yet from creation time.

### rmw layer

`rclcpp` with one ping/pong pair; `RMW_IMPLEMENTATION` alone picks the rmw (checked in `/proc/PID/maps`). Block wait is
`spin()`; poll wait loops on `spin_some()` with a sleep at a random phase, scored by interquartile overlap.

RTT mean from creation, ms, rmw_tickle / FastDDS / CycloneDDS; WIN unless marked (DRAW or LOSE against CycloneDDS;
every cell is a WIN against FastDDS):

| wait | Bench 64 B BEST_EFFORT | Bench RELIABLE | Array1k BEST_EFFORT | Array1k RELIABLE | src |
|---|---|---|---|---|---|
| block | 48c **0.245** / 0.383 / 0.335 | 49c **0.246** / 0.398 / 0.325 | 50c **0.266** / 0.401 / 0.350 | 51c **0.268** / 0.411 / 0.339 | WH |
| busy poll | 52c **0.299** / 0.377 / 0.331 | 53c **0.301** / 0.389 / 0.325 | 54c **0.320** / 0.398 / 0.350 | 55c **0.320** / 0.413 / 0.334 | Jc |
| poll 50 us | 56c **0.348** / 0.413 / 0.370 | 57c **0.353** / 0.421 / 0.368 | 58c **0.373** / 0.427 / 0.388 | 59c 0.376 / 0.438 / 0.377 DRAW | Jc |
| poll 100 us | 60c **0.370** / 0.431 / 0.389 | 61c 0.377 / 0.444 / 0.378 DRAW | 62c **0.394** / 0.452 / 0.410 | 63c 0.401 / 0.465 / 0.396 LOSE | Jc |
| poll 200 us | 64c **0.424** / 0.470 / 0.437 | 65c 0.423 / 0.486 / 0.428 DRAW | 66c 0.450 / 0.492 / 0.446 DRAW | 67c 0.442 / 0.509 / 0.433 DRAW | Jc |

| # | metric, cell | rmw_tickle | FastDDS | CycloneDDS | verdict | src |
|---|---|---:|---:|---:|:-:|---|
| 68 | peak RSS ping, KB, Bench block BEST_EFFORT | **12,544** | 23,748 | 14,592 | WIN | W |
| 69 | peak RSS pong, KB, same | **13,216** | 23,632 | 14,532 | WIN | W |
| 70 | pong CPU per run, ms, same | **37.4** | 57.7 | 43.5 | WIN | W |
| 71 | pong CPU per run, ms, Bench block RELIABLE | **37.2** | 60.4 | 48.5 | WIN | W |
| 72 | delivered msg/s, KEEP_ALL max rate, Array1k, 0% loss | **90,850** | 30,919 | 84,446 | WIN | K |
| 73 | the same, Array1k, 5% loss | **74,129** | ✗ refused | 1,808 | WIN | Ke |
| 74 | the same, Array4k, 0% loss | 27,434 ‡ | 27,917 ‡ | 27,204 ‡ | DRAW | K |
| 75 | the same, Array4k, 5% loss | **26,897** | ✗ refused | 347 | WIN | Ke |

- **Rows 73 and 75, "refused":** FastDDS's KEEP_ALL `write()` timed out at its 100 ms bound in every run.
- **48c-51c re-measured 2026-10-21** (`WH`, `430ae86d`, 3 reps x 100 round trips, CONTROL PASS): 4-9 us below `Wc`;
  two sessions, not an A/B, so not attributed. 52c-67c from `29826912` (`Jc`, 7 poll reps); the 1 ns timer slack
  (`119d14bd`) had cut block wait 0.302 -> 0.250 ms and moved 59c, 61c WIN -> DRAW, 63c DRAW -> LOSE.
- **VOID(loss):** none of 494 reps; `ping_node` warms up before seq 1 since `d0650110`.
- **Corrected 2026-10-18:** 48c-67c replace the send-stamp rows 48-67 (`934f90de`). rmw ping-pong control (`Ctl`, not
  scored): RTT median 0.32 / 0.42 / 0.35 ms.

### rmw large messages (L4, cross-host)

apex `perf_test` across the link, 5 reps of 60 s. Letters: FastDDS as shipped / FastDDS 1472 / CycloneDDS (– not scored).

| cell | QoS, rate | delivered % | RTT med / p99 ms | CPU sub / pub ms/MB | RSS kB | delivered | latency | CPU sub | CPU pub | RSS | src |
|---|---|---:|---:|---:|---:|:-:|:-:|:-:|:-:|:-:|---|
| I1 Array1m | RELIABLE KL10, 30 Hz | 100 | **9.50** / 9.65 | 2.29 / 3.94 | 18,684 | D–D–D | W–W–W | D–W–W | D–W–W | W–W–W | L16, LR |
| I1L Array1m, 5% loss | RELIABLE KL10, 30 Hz | 100 | **9.66** / 12.23 | 4.17 / 3.65 | 18,792 | –D– | –W– | –W– | –W– | –W– | L16, LR |
| I1 Array1m | BEST_EFFORT KL5, 30 Hz | 100 | **9.48** / 9.65 | 2.20 / 3.89 | 17,724 | D–D–D | W–W–W | D–W–W | D–W–W | W–W–W | L15, LR |
| I4s Array4m | BEST_EFFORT KL5, 30 Hz | 95.5 of offered | **55.30** / UNBOUND | 1.40 / 3.60 | 30,952 | W–W–W | W–W–W | D–W–W | D–W–W | W–W–W | L15, LC, LR |
| I4 Array4m | RELIABLE KL10, 15 Hz | 100 | **36.79** / 36.91 | 2.21 / 3.69 | 30,656 | D–D–D | W–W–W | D–W–W | D–W–W | W–W–W | L15, LC, LR |
| I4 Array4m | BEST_EFFORT KL5, 15 Hz | 100 | **36.78** / 36.94 | 2.20 / 3.55 ² | 30,580 | D–D–D | W–W–W | L–W–W | W–W–W | W–W–W | L15, LC, HP, LR |

² `HP`, perf task-clock at `e00c1ddd` (FastDDS 2.11 / 3.65).

- **RTT (`LR`, 2026-10-19):** a round trip on the sender's clock, 1 or 4 MB out and an 8-byte reply back through the
  same rmw; 5 reps x 900 samples, 0 VOID in 160 runs. I1L: FastDDS as shipped answered nothing, CycloneDDS 2.7-4.1%
  (DELIVERY FAILED); FastDDS 1472 9.66 against 59.5 ms.
- **I4s note.** I4s offers more than the link carries. A DDS `publish()` of 4 MB blocks, so its publisher falls behind
  every tick; the send stamp hides that wait and creation time counts it. rmw_tickle does not block and drops at the
  sender (~84 a run). Delivered % is per offered tick; p99 is UNBOUND for every arm and not scored.
- **Corrected 2026-10-16:** I1L was a LOSE on incomplete delivery (`f5fcda06`, 2 samples in 1 run); first-n3 fixed it.
- **Corrected 2026-10-17:** publisher CPU is per MB delivered; I4s CPU pub W–W–W -> D–W–W.
- **2026-10-18, I4 BE CPU pub L -> W against FastDDS** (`HP`): publisher huge pages (`e00c1ddd`) 3.712 -> 3.553 ms/MB
  (FastDDS 3.650).

### Large samples step B, same host (rig, `SB2`)

Step B (one copy into the publisher's large region, a descriptor to each reader) against step A (fragments through the
ring), one build, round trip on the sender's clock, 5 reps paired; control (8-byte Ack cells) held. Every B2 rule PASS:

| cell | CPU per sample, A -> B | RTT p50, A -> B |
|---|---:|---:|
| 1 MiB RELIABLE | 2,406 -> **752** us | 1.390 -> **0.615** ms |
| 1 MiB BEST_EFFORT | | 1.415 -> **0.569** ms |
| 4 MiB RELIABLE | 9,702 -> **2,779** us | 5.815 -> **2.558** ms |
| 4 MiB BEST_EFFORT | | 5.751 -> **2.395** ms |

Against FastDDS and CycloneDDS: RTT, CPU and RSS WIN in the RELIABLE cells; both DELIVERY FAILED in the BEST_EFFORT
cells (97.4-99.4% delivered).

### Hybrid transport (large samples), rig

AUTO against TickLE's own forced MTU mode, not counted (`HY`, Array1m RELIABLE): AUTO delivered 100% at 0.01-1% loss;
wire bytes WIN at 0.01% and 0.1%, LOSE at 1% (+0.46%); p99 LOSE at 0.01% and 0.1% (9.54 against 9.42 ms), DRAW at 1%.

### Measurement debt closed 2026-10-20 (`D2`)

- **DATA_FRAG at 4 fragments, 5% loss** (TickLE only): 620 Mbps, 8 reps (604-631), against 527-536 at `3d1ffeeb`;
  4.06 datagrams per sample.
- **Discovery join (M3)**, PC netns, N=8: median 2.2 ms at E=4, 2.0 ms at E=32. PASS (bound: parent + 5 ms).

### No matched reader sends nothing (rig, `NS`)

With no matched reader nothing goes on the wire: writer CPU per sample -69% (p1 1 kHz), -94% (1 MiB 30 Hz). Cost: a
new reader's first sample arrives ~2 ms later at p1.

### zenoh-pico and rmw_zenoh (reference, not scored)

**zenoh-pico 1.10.2** (release tag, commit 6a881c21, pinned in `examples/perf_hil/zenohpico/build.sh` since `fdfc065e`) on the rig,
2026-10-28, in the same campaign and the same rotated order as the scored columns (`ZF`). Never scored: no WIN/DRAW/LOSE,
no count. Transport: BEST_EFFORT over **UDP** (zenoh-pico's multicast session), RELIABLE over **TCP** (†), the only
configuration where its reliability is real; its UDP RELIABLE has no retransmission (a `@TODO` in 1.10.2's
`_z_unicast_handle_frame`). Median of 3 reps; a cell with fewer than 3 usable reps is `VOID`. Goodput is complete
samples a second at the subscriber; BEST_EFFORT throughput runs at max rate, RELIABLE at 10 kHz offered (`-i 0.0001`) unless marked max. Drift control (c1
repeated last) moved every framework less than its rep spread. Array1m is not run: zenoh-pico's default
`FRAG_MAX_SIZE` is 4096. rmw_zenoh is not in this round.

Throughput, samples/s (delivered %):

| cell | TickLE | CycloneDDS | Fast DDS | zenoh-pico |
|---|---:|---:|---:|---:|
| BEST_EFFORT P1 N0 | 198,696 (100.0) | 100,095 (89.2) | 62,452 (83.0) | 87,370 (100.0) |
| BEST_EFFORT P2 N0 | 90,720 (100.0) | 87,421 (99.2) | 53,761 (82.9) | 83,396 (100.0) |
| BEST_EFFORT P3 N0 | 82,791 (100.0) | 57,280 (79.1) | 42,895 (96.6) | 71,756 (100.0) |
| BEST_EFFORT P4 N0 | 42,131 (100.0) | 41,799 (99.8) | 40,119 (98.2) | 36,942 (100.0) |
| RELIABLE P1 N0 | 9,389 (100.0) | 6,001 (100.0) | 5,522 (100.0) | 6,001 (100.0) † |
| RELIABLE P2 N0 | 9,400 (100.0) | 6,000 (100.0) | 5,500 (100.0) | 6,000 (100.0) † |
| RELIABLE P3 N0 | 9,400 (100.0) | 5,999 (100.0) | 5,440 (100.0) | 6,000 (100.0) † |
| RELIABLE P4 N0 | 9,403 (100.0) | 5,997 (100.0) | 5,252 (100.0) | 5,809 (100.0) † |
| RELIABLE P1 N0 max | 191,319 (100.0) | 81,094 (100.0) | 24,608 (100.0) | died 3/3 † |
| RELIABLE P4 N0 max | 42,118 (100.0) | 39,770 (100.0) | 33,029 (100.0) | died 3/3 † |
| BEST_EFFORT P1 0.1% | 163,115 (99.9) | 102,140 (95.6) | 53,633 (76.1) | 84,117 (99.9) |
| BEST_EFFORT P4 0.1% | 42,087 (99.8) | 41,528 (99.0) | 40,344 (98.7) | 39,378 (99.7) |
| RELIABLE P1 0.1% | 9,388 (100.0) | 6,002 (100.0) | 5,511 (100.0) | 6,000 (100.0) † |
| RELIABLE P4 0.1% | 9,403 (100.0) | 5,904 (100.0) | 5,228 (100.0) | 5,672 (100.0) † |
| BEST_EFFORT P1 1% | 192,318 (99.0) | 101,831 (97.3) | 51,648 (71.2) | 84,609 (99.0) |
| BEST_EFFORT P4 1% | 41,711 (98.0) | 41,331 (97.7) | 39,804 (96.5) | 39,107 (97.0) |
| RELIABLE P1 1% | 9,398 (100.0) | 5,575 (100.0) | 5,497 (100.0) | 6,028 (100.0) † |
| RELIABLE P4 1% | 9,401 (100.0) | 3,848 (100.0) | VOID | died 2/3 † |
| BEST_EFFORT P1 5% | 186,900 (95.0) | 102,317 (93.9) | 47,629 (73.7) | 81,284 (95.0) |
| BEST_EFFORT P4 5% | 40,021 (90.2) | VOID | VOID | 32,211 (86.1) |
| RELIABLE P1 5% | 9,385 (100.0) | 2,247 (100.0) | 5,499 (100.0) | died 2/3 † |
| RELIABLE P4 5% | 9,393 (100.0) | 560 (100.0) | VOID | died 3/3 † |

Latency, round trip from creation, p50 / p99 ms (all delivered 100% unless noted; the BEST_EFFORT rows are a follow-up run,
2026-10-29, `16614bda`, all 24 reps ok and every one reporting `reliability=best_effort`):

| cell | TickLE | CycloneDDS | Fast DDS | zenoh-pico |
|---|---:|---:|---:|---:|
| RELIABLE P1 N0 | 0.200 / 0.241 | 0.253 / 0.279 | 0.282 / 0.316 | 0.218 / 0.235 † |
| RELIABLE P2 N0 | 0.225 / 0.266 | 0.268 / 0.302 | 0.307 / 0.359 | 0.246 / 0.269 † |
| RELIABLE P3 N0 | 0.229 / 0.274 | 0.282 / 0.321 | 0.333 / 0.377 | 0.249 / 0.266 † |
| RELIABLE P4 N0 | 0.244 / 0.281 | 0.292 / 0.339 | 0.348 / 0.392 | 0.410 / 0.426 † |
| BEST_EFFORT P1 N0 | 0.200 / 0.234 | 0.239 / 0.277 | 0.271 / 0.307 | 0.218 / 0.244 |
| BEST_EFFORT P4 N0 | 0.242 / 0.285 | 0.287 / 0.318 | 0.315 / 0.385 | 0.266 / 0.299 |
| RELIABLE P1 0.1% | 0.201 / 0.240 | 0.251 / 0.290 | VOID | 0.218 / 0.235 † |
| RELIABLE P4 0.1% | 0.251 / 0.290 | 0.293 / 0.363 | VOID | 0.415 / 0.435 † |
| RELIABLE P1 1% | 0.201 / 250.4 | 0.250 / 0.284 | VOID | 0.220 / 203.9 † |
| RELIABLE P4 1% | 0.243 / 250.4 | 0.291 / 100.9 | VOID | 0.411 / 4.2 † |
| RELIABLE P1 5% | 0.203 / 250.4 | 0.253 / 100.6 | VOID | 0.220 / 208.9 † |
| RELIABLE P4 5% | 0.249 / 250.5 | 0.293 / 200.8 | 0.347 / 258.3 (50%) | 0.410 / 208.8 † |

CPU, client + server per delivered sample (throughput) or round trip (latency), us, N0:

| cell | TickLE | CycloneDDS | Fast DDS | zenoh-pico |
|---|---:|---:|---:|---:|
| T BEST_EFFORT P1 | 7.31 | 30.20 | 38.06 | 20.02 |
| T BEST_EFFORT P2 | 9.38 | 30.07 | 41.09 | 20.93 |
| T BEST_EFFORT P3 | 9.67 | 46.46 | 46.71 | 23.04 |
| T BEST_EFFORT P4 | 18.22 | 40.88 | 44.22 | 45.75 |
| T RELIABLE P1 | 23.72 | 43.35 | 74.86 | 37.00 |
| T RELIABLE P2 | 24.41 | 45.66 | 76.30 | 34.69 |
| T RELIABLE P3 | 24.51 | 51.31 | 81.15 | 34.19 |
| T RELIABLE P4 | 34.47 | 52.32 | 88.68 | 54.60 |
| L RELIABLE P1 | 29.1 | 52.2 | 103.1 | 88.9 |
| L RELIABLE P2 | 31.1 | 54.4 | 104.7 | 97.6 |
| L RELIABLE P3 | 31.2 | 60.8 | 111.3 | 97.2 |
| L RELIABLE P4 | 41.6 | 62.0 | 118.8 | 154.4 |
| T RELIABLE P1 max | 7.58 | 21.47 | 61.46 | - |
| T RELIABLE P4 max | 19.70 | 34.24 | 57.45 | - |
| L BEST_EFFORT P1 | 29.3 | 49.0 | 64.4 | 90.6 |
| L BEST_EFFORT P4 | 40.6 | 58.0 | 73.6 | 152.9 |

- **zenoh-pico's TCP session drops.** RELIABLE at max rate died in every rep at P1 (3/3, after about 2.1 million
  samples each) and P4 (3/3), and under loss at P4 1% (2/3), P1 5% (2/3) and P4 5% (3/3): the subscriber stops
  receiving while the publisher keeps reporting successful puts. At 10 kHz with no loss it survived every rep. The same
  shape reproduces off the rig on a PC veth (`zenoh_tcp_disconnect_repro.sh`: 15/15 survived at 1-10 kHz with no loss;
  died 5/9 at max rate, 4/4 at max rate with 1% loss, 2/2 at 10 kHz with 5% loss), with the subscriber logging
  `Dropping peer due to processing error`. The cause is not investigated; it is expected from zenoh-pico's internal
  structure.
- Where it completes, zenoh-pico delivers every BEST_EFFORT sample at N0 (the DDS pair lose 0.2-21%), at 44-92% of
  TickLE's goodput. Its RELIABLE latency p50 at P1-P3 is within 0.025 ms of TickLE's, with a p99 just below TickLE's at
  P1 and P3; at P4 it is 0.41 ms against TickLE's 0.24. Its CPU per sample or round trip is 1.4-3.8 times TickLE's.
- Under 1-5% loss the RELIABLE latency p99 is a repair wait: TickLE 250 ms in every cell, zenoh-pico about 200 ms (4.2
  ms at P4 1%), CycloneDDS 0.28-201 ms.
- The September figures (1.10.1, pinned, from zenoh-pico's own sessions) are at `git show 88e73b56:docs/RESULTS.md`.
- **rmw_zenoh** (`rmw_zenoh_cpp` 0.2.9 over TCP through its router, jazzy; reference, older): RTT mean from creation, block
  wait, 48c-51c 0.449 / 0.445 / 0.686 / 0.682 ms (`Wc`, `29826912`); rows 68-71 70,072 / 70,484 KB and 91.8 / 94.1 ms
  (`W`, `934f90de`). It accepts DEADLINE and LIVELINESS but refuses their events (`UnsupportedEventTypeError`, rows
  45-46).

## 3. Sources

Raw files in `~/rig_results_safe/`, or `examples/perf_hil/results/` for `results/`; harnesses in
`examples/perf_hil/experiments/`. All frameworks of a row come from one session.

| code | harness | build | date | raw file |
|---|---|---|---|---|
| A | `fair_crosshost_remeasure.sh`, `goodput_rescore.py` | `09a0437d` | 2026-10-08 | `fair_crosshost_09a0437d_20261008-112506.A_free.txt` |
| T | the same, FastDDS 1472 (rows 14, 17, 27, 34, 40: `3ae721aa`) | `09a0437d` | 2026-10-08 | `...112506.T_free.txt`, `fair_crosshost_3ae721aa_T6.T_free.txt` |
| C | `campaign_sweep.sh`, `creation_latency_summary.py --rtt` | `596e9c9c` | 2026-10-17 | `creation_latency_core_20261017-204550.txt` |
| D | `debt_crosshost_remeasure.sh` (12 reps, rotated, unpinned) | `d58873cf` | 2026-10-20 | `debt_crosshost_d58873cf_20261020-011033.{A,T}.txt` |
| D2 | `debt2_remeasure.sh`, `debt2_read.py` | `df6fbc2c` | 2026-10-20 | `debt2_df6fbc2c_20261020-030104.*` |
| S | `comparison_resweep.sh` | `659013e9` | 2026-09-24 | |
| L | `liveliness_l2.sh` | `0f220ba0` | 2026-09-27 | `results/liveliness_l2e_2026-09-27.txt` |
| LS | `rig_lifespan_reshaped.sh` | `3ae5f34f` | 2026-10-20 | `lifespan_reshaped_3ae5f34f_20261020-035439.txt` |
| X, X2 | `rig_queue_keep_last_freshness_rt.sh`, `freshness_rt_summary.py` (X2: reshaped server) | `92059567`, `6ddcb066` | 2026-10-19, -20 | `keep_last_freshness_rt_{92059567_20261019-132519,6ddcb066_20261020-082304}.*` |
| W | rmw block wait | `934f90de` | 2026-09-30 | `results/rmw_4way_block_934f90de_2026-09-30.txt` |
| Wc, Jc | `rmw_crosshost_rtt.sh`, `rmw_crosshost_creation.py` | `29826912` | 2026-10-18 | `creation_rtt_rmw{W,J}_29826912_20261018-111338.*` |
| WH, KH | `rmw_holdover_rig.sh` (KH: `rmw_pcap_split.py`, capture + `sysstamp`) | `430ae86d` | 2026-10-21 | `holdover_rmw{W,K}_430ae86d_20261021-185529.*` |
| KH2 | `rig_queue_s1_kh.sh` part KH, `s1_kh_rig_read.py --kh` | `132548dd` | 2026-10-23 | `s1kh/132548dd_20261022-230208/kh_reading.txt` |
| KH2B | `kh2_rig_ab.sh`, `kh2_pong_calls.py` | `e9efea98` (P), `b72eab0c` (B) | 2026-10-24 | `kh2ab_20261024-055604.*` |
| K, Ke | `rmw_keepall_rig.sh` (Ke: equal bounds) | `ae724688`, `29dff630` | 2026-10-06, -08 | `rmw_keepall_rig_O2_20261006-123017.txt`, `rmw_keepall_equal_main_29dff630_20261008-134901.*` |
| Ctl | perf_test ping-pong | `4fca013b` | 2026-10-17 | `creation_latency_ctl_20261017-094944.*` |
| SB, SB9 | `samehost_bundle_rig.sh` (SB9: S9c alone) | `60f49f6a`, `0117126a` | 2026-10-19 | `samehost_bundle_{60f49f6a_20261019-190222,0117126a_20261019-222752}.*` |
| SF, SP, SC, SC0 | `fair_samehost_remeasure.sh` and readers (older same-host figures) | `e521bbba`, `228da18d`, `2579e822`, `596e9c9c` | 2026-10-09 .. -18 | `fair_samehost_*`, `creation_rtt_samehost_*` |
| QB, QB2 | `qos_be_ab.sh`, `qos_be_ab.py` (TickLE only) | `93ec2b3b`, `d6a007dd` | 2026-10-19 | `ab_samehost_qosbe_20261019-{210053,223805}.*` |
| SE | `s6_transport_cells.sh` | `e17b4e6f` | 2026-10-04 | `p4_shm_headtohead_e17b4e6f.txt` |
| MX | `mixed_delivery.sh`; CycloneDDS `mixed_delivery_campaign.sh` (`e4e090cd`) | `7f127d56` | 2026-10-05, -17 | `mixed_4e49ed43_20261017-152135.*` |
| RS, RSc, RS2 | `rmw_samehost.sh`, `rmw_samehost_summary.py` (RS2: rotated) | `06dd78d8`, `29826912`, `69f394cc` | 2026-10-09 .. -21 | `rmw_samehost_69f394cc_20261021-134220.*` |
| LN | `rmw_loan_bench.sh` (PC) | `ab/lending` + `f837ae82` | 2026-10-08 | `~/rmw_loans_ws/bench3/` |
| L15, L16 | `rmw_keepall_rig.sh`, `largemsg_rig_summary.py` | `f5fcda06`, `dde87857` | 2026-10-15, -16 | `l4main2_*_20261015-094605.txt`, `l4main3_i1rel_20261016-065517.*` |
| LR | `l4_rtt_rig.sh`, `l4_rtt_read.py` | `3583f0d1` | 2026-10-19 | `l4_rtt_3583f0d1_20261019-161849.*` |
| LC | the same, `CREATION_TRACE=1` | `4fca013b` | 2026-10-17 | `creation_latency_{i4s,i4be,i4rel}_20261017-094944.*` |
| HP | `pub_hugepage_ab.sh` | `e00c1ddd` | 2026-10-18 | `pub_hugepage_rig_20261018-150541.read.txt` |
| SB2 | `stepb_rtt_rig.sh`, `stepb_rtt_read.py` | `28fd075e` | 2026-10-20 | `stepb_rtt_28fd075e_20261020-181744.*` |
| HY | hybrid arms | d8980f9b (ab/hybrid-land) | 2026-10-12 | `hybridLand_s2_loss_20261012-021743.*` |
| NS | `nosend_benefit_rig.sh` | `b265e86f` vs branch | 2026-10-16 | `nosend_benefit/20261016-020518/reading.txt` |
| ZF | `zenoh_fair_queue.sh`, `zenoh_fair_be_latency_queue.sh`, `zenoh_fair_read.py` (reference column; zenoh-pico 1.10.2) | `1998d6fc` / `16614bda` | 2026-10-28 | `~/zenoh_fair_results/zenoh_fair_20261028-203558.txt.zenoh_fair`, `zenoh_fair_be_latency_20261028-235332.*` |

**Versions:** CycloneDDS 11.0.1 (iceoryx 2.0.5) and Fast DDS 2.14.6 native; jazzy's rmws (CycloneDDS 0.10.5, Fast DDS
2.14.6). Vendors are distribution packages (`-O2`); TickLE `-O2 -DNDEBUG`. Open work: [ROADMAP.md](ROADMAP.md).

## 4. Investigations (PC netns and rig A/Bs)

TickLE against itself: no vendor, not scored. PC netns runs use the WB harness (`wifi_burst_loss_netns.sh`, three
namespaces on a bridge, netem with Gilbert-Elliott loss `geL bB` = L% average, mean burst B; `tv` = a time-varying
schedule; `bc` = broadcast loss). A rig check is core gate v2 (88 rows) plus `rmw_keepall_rig.sh` Array1m 0/0.1/1% and
Array4m 0%, arms pre / head / aa (aa = head's own library again). Every reading rule was committed before its run;
each code's full reading is in the pre-shortening text (top of this file).

| code | finding | build | date | data |
|---|---|---|---|---|
| WB | Wi-Fi-like loss baseline, ETHERNET profile. RELIABLE 100 B delivered 98.3-100%, one-way p99 6-94 ms; R2, R5 (false peer deaths only in pairs with no data), R6, R7 FAIL. **Corrected 2026-10-23 (`WBS`):** the 1 MiB repair storm was not netem's queue but a reliable HEARTBEAT riding a batched summary broadcast; fixed in core (`heartbeat_unicast_count()`), pinned control parent 7 of 20 stormed, fix 0 of 20 | `9c503bc1`, `25dd0fb0` | 2026-10-22, -23 | `wifi_baseline/`, `large_storm/` |
| WW | WIFI profile's discovery: false peer deaths 81 -> **0**, joins PASS on both, wire bytes 169 -> 171 a sample | `ca540c2c` | 2026-10-23 | `wifi_live/` |
| WR | WIFI repair timing (Karn's rule after a first timed recovery): resends per lost datagram 2.2-2.9 -> 0.93-1.07; `tv` copies after delivery 12.7% -> 2.5%; false peer deaths 77 -> 0. R4 FAIL on both at ge10b4 (reverse bytes 0.25%: a timer value, no wire lift). Instructions per sample equal | ab/wifi-repair | 2026-10-23 | `wifi_repair/final2_04be_*` |
| R4K | Not landed. R4's excess at ge10b4 is the reader's retry interval (37 ms on a 6.9 ms RTT). Karn's rule for large writers too (K) cut p50 100.8-104.1 -> 74.2-74.7 ms, only at the limit; K's ge5b2 reps at ~2x resends recur | `b434f649`, `1a7b5376` | 2026-10-25 | `~/r4_results/` |
| K5 | Not landed. K's 2x reps: a seed probe faster than any repair (its unit named before), so the interval locks low. Floor fixes stop it (clamp 0/80 against 5/80, p 0.03) but lose K's ge5b2 tail (p99 247 against 177-193 ms). Real fix: the repair echoes its request's serial, a wire change, parked | `06d23f21` | 2026-10-26 | `~/k5_results/` |
| LRF | HEARTBEAT fix's follow-ups. (b) not shown. (c) `request_ack` sent batched broadcast-only content to its peers: real, rare; fixed, landed 2026-10-24 (rig gate PASS 0/88). (a) large-sample repair storm on ETHERNET at 1%: one rep of 3 resent 251,028 units; fixed by `LRO` | ab/large-repair-followups(-c) | 2026-10-23, -24 | `large_repair/` |
| LRO | Large-sample resend order: a resent unit goes again only once shown lost or a repair timeout passed. Sim queued path 259,042 -> 9,541 resent; netns 0.1% storms 3 of 11 -> 0 of 12. Cost: ge1b1 median rep p99 11.0 -> 17.2 ms (see `LRT`) | P `896d4a29` | 2026-10-24 | `large_repair/ab_order_*` |
| LRT | RACK's time rule for the order's tail: ge1b1 p99 19.6 -> 14.1 ms (0.66 of the cost back). Rig, 12 reps (`LRTR`): tail against main +0.4-0.8 ms at 1%; landed on the user's call | `53ed4e1f` (base), `d6231dd4` (tail), main `36700555` | 2026-10-24 | `large_repair/ab_tail_ed46*`, `hmr_1m_20261024-192010.txt` |
| TVS | Wi-Fi `tv` storms (2 of 80 reps, up to 845,268 resent) came from the order's timeout timing units an ACKNACK left out (srtt ~40 ms -> under 128 us). Fix: never time them. Collapsed-timeout reps 25/60 -> 0/30 (p 4.5e-6); rig PASS 0/88, p99 equal. Landed | core `8d0d2eed`; rig A `b434f649`, B `5b7c09e7` | 2026-10-26 | `~/storm_results/` |
| RLC | The reader's retry estimate collapses on main ~1 rep in 60 (Wi-Fi 1 MiB) and ~1 in 12 (ETHERNET amp): an ambiguous seed of 3-90 us locks the interval for 2-9 s. No loss; 2-3x resends and duplicates | main `81daa880` | 2026-10-26 | `~/rlc_results/` |
| RLF | Least-transit floor on the reader's path samples: COLLAPSE 22/840 -> **0/840** (p 2e-7), resends -1..-15%, duplicates -9..-46%. One WORSE row: `tv` median rep p50 68.5 -> 74.3 ms (ge5b4 p99 256 -> 274, within band). Rig PASS. Landed 2026-10-27 on the user's call | main `a5b52eee`; rig `fc5fb79b` / `dc4fa180` | 2026-10-26, -27 | `~/rlf_results/` |
| EWS | ETHERNET amp writer storm, 30 of 2,400 traced reps, load-driven: one ACKNACK folds ~250 Karn samples into srtt, the timeout is a bare 2 x srtt, and a whole lost sample (722 units, a 9.5 ms batch) outlasts it; then every ACKNACK re-releases every unit. Forced: timeout pinned 1.5 ms stormed 10/20, with a per-unit back-off 0/20 (p 0.0002) | census `2b482592`, force `231f10bd` | 2026-10-26 | `~/ews_results/` |
| EWF | The repair timeout runs from the later of the resend and the reader's last progress (G): forced loop 10/20 -> 0/20, no row WORSE. Partial: a stall with no progress still storms. Rig PASS; its Array1m 1% p99 +0.54 ms on head-built arms was noise: a 12-rep recheck (B5, 2026-10-28) gave median p99 pre 10.23, G 10.12, A/A 9.99 ms, no cost by the registered rule (G - pre <= 0.2 ms). Landed 2026-10-27 | `3bd2acec`; rig `b61e2302` / `45129b71` | 2026-10-27, -28 | `~/ewf_results/`, `~/rig_results_safe/gcost_1m_20261028-195231.txt` |
| EWB | The timeout doubles per resend with no progress between (at most x64). Stall cell storms 14/20 -> 2/20 (none through the timeout), delivery 92.6 -> 99.8%, p99 195 -> 137 ms; natural amp 7/1,440 -> 0/720 (UNDECIDED). One WORSE row (R4 ge10b4 delivered 98.25%, one rep of 8); 32-rep follow-up UNDECIDED. Corrected 2026-10-29: that rep (129/150) had a 14,527-datagram send deficit from the emulator's ARP outage (C8), not EWB's back-off. PC 1% p99 add-on VOID. Rig PASS; landed on the user's call | `102ec895`; rig pre `3e353df1` | 2026-10-27 | `~/ews_stall_results/` |
| HMA | Hybrid IP-run mode at 1% was a latched p (re-announced only on a factor of 2, and the n = 1 / 2 boundary 0.886% is inside 1%'s band). Fix: re-announce when n* moves. Rig (`HMAR`): IP-run reps 5/12 -> 0/24 (p 0.002); median p99 +0.22 ms | pre `7694bc87`, fix `3f0cf05e` | 2026-10-25 | `haa_1m_20261025-002405.txt` |
| HFN | Runs of 45 at 1%: a stale forget announced "none seen", the runs filled the reader's IP reassembly memory, and the writer's rule (e) went to MTU for 60 s (4 of 16 reps) | `0f33a562` | 2026-10-25 | `hyb_forget/mech1/` |
| HSF | No estimate after a stale forget (options (a) + (e)): chain 14/79 -> 0/79. Registered FAIL on CPU at 1% (sub +7.6-8.9%, pub +15.7-18.1%), mostly main's chain itself; rig CPU equal, PASS. Landed 2026-10-27 on the overall judgement | M `fc5fb79b`, F `4244d84e` | 2026-10-27 | `~/hsf_results/` |
| SRS | Reassembly slots per Subscriber. Run 3 (`911c7c3c`) INCONCLUSIVE, nothing WORSE; beside 8 no-buffer RELIABLE streams at 5% loss, RELIABLE delivery 80.4 -> 100%, p99 182 -> 36 ms, CPU -10%. Runs 1-2 FAILED on a sample ahead left in a spent own slot; fixed by the oldest unpinned pool slot. Rig PASS (8/88 listed, +0.08-0.45% user instructions a sample). Landed 2026-10-28 `d57eb469`. BEST_EFFORT re-measured with a fresh netns per run (`srs_be_ab`, 8 reps, M = landed SRS reverted): UNDECIDED in all 6 cells, point estimates +1.2..+5.7 points in 5 of 6 inside A/A floors of 7-13 (deciding ~5 points needs 3-7x the reps); RELIABLE n12_l5 delivery 33.6 -> 100%, p99 -89%, CPU -40% | M `149a281f`; B6 M ab/b6-no-srs, F `e6b23b63` | 2026-10-27, -28 | `~/sub_reasm_results/`, `/home/semih/b46_results/srs_be/` |
| HRW | The hybrid reader's writer table: 8 entries recycled every sample with 9+ fragmenting writers. 16 entries, least-recent replaced: run 2 INCONCLUSIVE, nothing WORSE; BEST_EFFORT delivery 8 streams 5% 9.1 -> 12.5%, 12 streams 1% 38.4 -> 55.3%. Rig PASS (2/88 listed, +0.23%). Landed | M `2ae7b83d`, F `96a2b507` (after the rebase) | 2026-10-28 | `~/hybrid_rx_results/` |
| B4F | A no-buffer RELIABLE reader went silent: a datagram placed or dropped past its watermark without being recorded raised no gap, and the writer's steady publishing held its idle HEARTBEAT back, so the stream stalled until the next announce (startup: seq 1 refused before the writer was tracked; mid-run: runs lost to IP reassembly memory). Probe on main: 6 stalls in 600 streams. Fix: such a datagram counts as announced and arms the retry, as a declined DATA does. PC probe n8_l5 stalls 3/20 -> 0/20, streams with loss 8/160 -> 0/160. Rig PASS (2/88 listed: server user insn c4 +0.05%, c15 +0.36%); Array1m 1% p99 pre 9.79, head 10.02 ms; Array4m 36.67 / 36.68 ms; delivery 100% everywhere. Landed | `391f8360`; rig `088f88c3` | 2026-10-28, -29 | `/home/semih/b46_results/`, `~/rig_results_safe/b4_{1m,4m}_20261029-020332.txt` |
| RLC2 | The reader's retry estimate after RLF, census as `RLC` on main `088f88c3`: COLLAPSE 0 and SEVERE 0 in all five cells (60 / 150 reps each; `RLC` had 5, 1, 5, 2, 2); the estimate fell under SHORT in 0 of 480 traced reps (`RLC`: 111 of 473). The PC was quieter than in `RLC` (load median 0.45). Closed | `088f88c3` | 2026-10-29 | `~/rlc_results/rlc2*` |
| C8 | A reader the writer first heard after its 8th publish lost the samples the writer still held: the match HEARTBEAT counted down per publish whether or not the reader had acknowledged. Fix: the last match HEARTBEAT stays until the reader's ack passes the match point. Wi-Fi outage A/B: recovered within KEEP_LAST 16/16 vs 0/16. Rig PASS 0/88 (2 listed: c17 p4 user insn client +0.84%, server +0.79%); Array1m delivery 100%, p99 0/0.1/1% head 9.38/9.44/9.92 vs pre 9.37/9.46/10.15 ms; Array4m 36.62 vs 36.69 ms. Landed on the user's call | core `f8200f15`; rig A `27789ad4`, B branch `ab/c8-fix` | 2026-10-29 | `/home/semih/c8_results/`, `~/rig_results_safe/c8_{1m,4m}_20261029-083434.txt` |
| C12 | The ping side received 100 of 101 echoes by epoll + `recvfrom()`, not the ring: the ring owner's idle waits reset the streak that moves the ring. Fix: the streak counts datagrams read from the data socket. KH2 rig A/B (`experiments/c12/c12ring_read.py`, rules before the run), ring receives 1 -> 99 a rep: TickLE ping tap -> receive p50 15.45 -> 12.37 us (-3.08; every B rep below every P), controls Cyclone +0.31, Fast DDS +0.26 us; PASS. First run 15.63 -> 12.35 us, NOT COMPARABLE (Fast DDS moved -0.57). Pong +0.72 / -1.13 / +0.28 us. Core gate v2 PASS (13/88 listed: user insn +0.12..+0.33%). Landed on the user's call | B `b9439f3d`, P `4a7a4beb` | 2026-10-29 | `~/rig_results_safe/kh2ab_*_20261029-{151908,174403}*` |
| C7SL | Slow-reader repair bursts (C7): a pending large repair's slot was freed only by an ACKNACK the resend order read; one naming nothing moved the watermark unread, so passed repairs stayed live (unit test 15 stale slots -> 0). Fix: each reader keeps the last watermark the order read. Rig PASS 0/88; Array1m delivery 100%, p99 0/0.1/1% head 9.39/9.48/9.87 vs pre 9.39/9.45/10.03 ms; Array4m 36.71 vs 36.70 ms (branch CI overlapped the first core blocks). The repair window itself (C7 candidate A) was WORSE in netns twice and is not landed. Landed on the user's call | core `b2bd8a16`; rig A `73e7a38c`, B branch `ab/c7-slot-leak` | 2026-10-30 | `~/rig_results_safe/c7sl_{1m,4m}_20261030-013907.txt`, `queue_core_ab_c7sl.log` |
| RXIP | Not landed. Datagrams 2..N of a `recvmmsg()` batch are copied from the HAL's rows; handing core the row cut server user cycles 8.4% at BE p2 max rate, nothing paced. Not worth a rig A/B; kept on `experiment/rx-rows-in-place` | core `1a2ef53d` | 2026-10-25 | `rx_rows_inplace_pc.sh` |
| S1R | Shared-memory module on but unused, cross-host: no client cell WORSE (-0.2..+1.7%); server +0.98% at p4 only (a candidate); publisher +330-530 user instructions a sample | `132548dd` | 2026-10-22 | `s1kh/132548dd_20261022-230208/` |
