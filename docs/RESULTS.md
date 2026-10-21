# TickLE performance results

**TL;DR.** TickLE against FastDDS and CycloneDDS on two Raspberry Pi 5s; zenoh is reference only and never scored.
- Core, cross-host: WIN 139, DRAW 11, LOSE 0 of 150. Core, same host: WIN 18 of 18.
- ROS 2 (rmw), cross-host: WIN 21, DRAW 6, LOSE 1 of 28 (row 63c). Same host: WIN 52 of 52.
- Large messages (L4): WIN 58, DRAW 21, LOSE 1 of 80: subscriber CPU per MB against FastDDS at 4 MB, 15 Hz.
- Latency is measured from creation time, throughput is goodput, and nothing is pinned.

Older text: `git show a89aefe2:docs/RESULTS.md` (full provenance prose, superseded readings, pinned figures, history).

## At a glance

**Core, cross-host** ([section 2](#2-cross-host-two-pis-over-the-rigs-link)): campaign `A` WIN 139, DRAW/TIE 11, LOSE 0
of 150; campaign `T` (FastDDS `maxMessageSize` 1472) 18 / 2 / 0 of 20; QoS row 44 WIN.

| headline | TickLE | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| RTT p50 from creation, P1, ms | **0.211** | 0.346 | 0.312 | 1c |
| RELIABLE goodput, P1, Mbps | **107** | 28.1 | 52.2 | 7 |
| RELIABLE throughput kept at 5% loss, P1 / P4 | **86.9% / 91.6%** | 43.1% / 38.9% | 4.8% / 1.6% | 16-17 |

**Core, same host** ([section 1](#1-same-host-shared-memory)): WIN 18, DRAW 0, LOSE 0 of 18.

| headline | TickLE | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| BEST_EFFORT goodput, P2, Mbps | **20,867** | 0.31 | 934 | S1 |
| RTT mean from creation, P3, 200/s, ms | **0.037** | 0.164 | 0.130 | S9c |

**rmw, cross-host** ([rmw layer](#rmw-layer)): WIN 21, DRAW 6, LOSE 1 of 28.

| headline | rmw_tickle | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| RTT mean from creation, block wait, Bench 64 B, ms | **0.250** | 0.379 | 0.336 | 48c |
| delivered msg/s, RELIABLE KEEP_ALL, Array1k, 5% loss | **74,129** | does not run | 1,808 | 73 |

**rmw, same host** ([rmw layer, same host](#rmw-layer-same-host)): WIN 52, DRAW 0, LOSE 0 of 52.

| headline | rmw_tickle | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| RTT mean from creation, block wait, Bench 64 B, us | **40.2** | 190.1 | 171.7 | R1c |

**rmw, large messages** ([L4](#rmw-large-messages-l4-cross-host)): WIN 58, DRAW 21, LOSE 1 of 80 (one letter per vendor).

| headline | rmw_tickle | FastDDS / FastDDS 1472 | CycloneDDS | cell |
|---|---:|---:|---:|---|
| delivered, Array1m RELIABLE 30 Hz, 5% loss | **100%** | 0% / 100% | 2.4-4.0% | I1L |
| latency median from creation, Array4m 30 Hz (saturating), ms | **55.8** | 2,026 / 7,599 | 1,900 | I4s |

### Every LOSE

- **L4 I4 best effort (Array4m 15 Hz), subscriber CPU per MB against FastDDS as shipped**, 2.20 against 2.11 ms/MB
  (+4.5%, `HP`): the 4 MB in-place buffer is larger than the Pi 5's 2 MB L3. Five candidates changed nothing: THP,
  `nt`, `rev`, `ntrev` and no in-place read (`i4be_cpu_gap.sh`).
- **rmw row 63c (Array1k RELIABLE, 100 us poll) against CycloneDDS**, 0.4006 against 0.3957 ms (interquartile rule;
  DRAW on range and on the stamp). CycloneDDS's `spin_some` sleeps ~42 us in a futex each call (a zero timeout
  stretched by the kernel's default 50 us timer slack) and takes replies that arrive meanwhile; rmw_tickle's makes no
  syscall and returns in 8.5 us. At busy poll rmw_tickle is 13.6 us faster, and it executes fewer instructions and
  syscalls per round trip on both sides. Kept as measured (user, 2026-10-18).

### Every DRAW

- **Row 5c, +10 ms netem:** 10 ms of netem swamps a 0.2 ms RTT (counted as one DRAW in `A`).
- **Loss TIEs** (10 in `A`, 2 in `T`): TickLE lost nothing, and each vendor lost nothing or had a range touching zero.
- **rmw rows 59c, 61c, 65c-67c against CycloneDDS:** at 50-200 us poll sleeps the poll cycle dominates (see 63c).
- **rmw row 74, Array4k KEEP_ALL at 0% loss:** all three sit at the link ceiling.
- **L4, 21 letters:** delivered % where everyone delivered 100% (13); against FastDDS as shipped, CPU per MB at I1,
  I1 best effort, I4 RELIABLE and I4s (8).

**Behind inside a lead (not scored):** rmw_tickle's kernel wake is ~6 us slower than the vendors' blocking `recvmsg`.
zenoh-pico (reference) has the better figure in rows 5-7, 18-19, 24 and 35. Hybrid AUTO's p99 is above forced MTU at
0.01% and 0.1% loss ([Hybrid](#hybrid-transport-large-samples-rig)).

## How to read these numbers

- **Throughput is goodput**: complete, unique samples the receiving application got in the measured window.
- **Latency is from creation time**: from when the sample or ping was due. The send-stamp figure is a diagnostic.
- **CPU, memory and wire bytes are per delivered sample** (or per MB, or per answered round trip). Nothing is pinned.
- **WIN** when every TickLE rep beats every rep of every scored vendor, **LOSE** when a vendor's reps all beat TickLE's.
- **DELIVERY FAILED ◊**: a vendor that delivered less than it sent in a RELIABLE cell; shown in parentheses, not scored.
- **src** is a measurement code from [Sources](#3-sources). Compare figures within one session. Rules: [TESTING.md](TESTING.md).

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

### Same-host notes

- **Corrected 2026-10-18:** S7c-S9c, S12 and S18 replace the `596e9c9c` figures (S7c-S9c 0.082 ms): the polling
  thread's 1 ns timer slack (`119d14bd`) cut TickLE's ping delay from ~60 us (as the DDS clients') to 15 us.
- **Re-measured 2026-10-19 (`SB`)** after the QoS work, every framework in rotated order per rep (`b83e2513`); the
  old rows ran one framework after another. Every scored row still a WIN. Against TickLE's own previous figures:
  S1-S3 -2.7%, -3.2%, -1.2% (CycloneDDS -2.0%, -0.4%, +1.2% in the same session), S4-S6 +1.3% to +2.2%, S11 +1.0%,
  S10c 0.086 -> 0.041 ms (now read; the timer-slack change of 2026-10-18). BE P4 (S3, S11, S13) ran 5 reps.
- **S9c** was NOT READ in `SB` (R4 control missed by 0.2 us: `|d - g|` 0.5 against 0.3 us); re-run alone the same night
  (`SB9`), control PASS. CycloneDDS missed 1 of 10,140 round trips in 1 rep (◊).
- **Did the QoS work lower S1-S3?** No sign of it. A/B on one Pi, 93ec2b3b (before `e4fc0683`) against `d6a007dd`,
  with a placement arm, 8 reps an arm (`QB`): goodput B-A +0.06% (P2), +0.15% (P3), -0.83% [-1.84, +0.18] (P4); P4's
  1.2% was not excluded, so it ran again at 16 reps an arm (`QB2`): -0.21%, +0.24%, -0.19% [-0.84, +0.46], every
  asked drop excluded, CPU per sample not raised. SB's lower S1-S2 came from the session.
- **◊ CycloneDDS** lost 0.02-0.07% of RELIABLE throughput in every rep, and 1 of 10,139 round trips at P4 in 1 rep.
- **‡ CycloneDDS includes iox-roudi** (212 MB RSS, 1.6 us CPU per round trip).
- **FastDDS delivers ~0% in S1-S3**, a rate-dependent overrun at BEST_EFFORT KEEP_LAST 1.
- **Corrected 2026-10-17:** S1-S3 were send rates until goodput (FastDDS S1 6,370 -> 0.31 Mbps).

### Same host from creation time (2026-10-17)

`SC`: 1,984-1,986 round trips per TickLE rep at 200/s, 1,200 at 20/s, all with their due time; R4 PASS in S7c-S9c.
Reps' means: TickLE 0.0334-0.0369 ms, FastDDS 0.1574-0.1834, CycloneDDS 0.1234-0.1457. Ping delay `g`: TickLE 14.6
us at 200/s and 10.6 at 20/s; DDS 59.7-61.5. S10c (`SC0`): 0.0856-0.0860 against 0.1597-0.1639 and 0.1285-0.1313.

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
- **Re-measured 2026-10-21 (`RS2`, `69f394cc`, slot loans off):** every row now comes from one run (was `RSc` for
  R1c-R12, `RS` for R13-R21); 0 VOID, all 8 controls PASS, ~11,000 round trips a rep in block cells, ~6,400 in poll.
  Against our own figures: block RTT -10..-12% and its CPU -15..-16% (the hold-over `58bd6b1a`, whose A/B gave
  -12%); poll RTT within 2%; delivered msg/s +57..+82% and CPU per sample -25..-44% (against `06dd78d8`, so the
  hold-over plus later changes); R13 RSS +536 kB. CycloneDDS moved -7..+6%, FastDDS RTT -2..+4%; FastDDS BEST_EFFORT
  msg/s spans 3,383-17,457 across reps (R14 median 5,040, was 17,547). No verdict flipped.
- **Earlier:** 2026-10-18 `29826912` gave R1c 45.1 us, after the 1 ns timer slack (`119d14bd`) halved it from 94.9.

### Loaned messages (PC)

PC, rmw level (`LN`): copy, loan and in-ring loan arms read **NO DIFFERENCE** in every comparison.

## 2. Cross-host (two Pis over the rig's link)

⚪ not scored; ✗ refused or transport died (never a figure); ‡ at the ~940 Mbps link ceiling.

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

Rows 48c-75 are in [rmw layer](#rmw-layer). Send-stamp means (rows 1, 3, 5, 6a, 6b, 48-67) are diagnostics, and
zenoh-pico and rmw_zenoh are reference; both are in the older text.

- **Re-measured 2026-10-20 (`D`):** rows 7-10, 12-23, 26-30, 32-41 at `d58873cf` (after the QoS work), 12 reps, every
  framework's order rotated, nothing pinned; 0 VOID, every scored row still a WIN. eth0's interrupt runs on CPU 0, and
  a sender there reads low (TickLE P1 93.8 against 114.8 Mbps elsewhere, 2 of 12 reps); the medians include both.
  FastDDS DELIVERY FAILED in c6 (rep 12 missed 34% of samples), so rows 14, 17, 27, 34 and 40 score CycloneDDS only.
  TickLE against its own previous figures: row 14 865 -> 895 Mbps, row 34 2,968 -> 2,562 kB (QoS storage), row 12
  117 -> 114. c6's retransmitted / sent is 0.167-0.175 (12 reps; 0.184-0.215 at `3ae721aa`), gap_abandoned and
  gap_evicted 0: the 46x of COMPARISON 2.8 assumed IP fragmentation, which P4 no longer uses.
- **Corrected 2026-10-20:** row 47's TickLE "works" came from a harness that could not fail. Its server slept `-p`
  before creating a VOLATILE Subscriber, so "lost" was what was published before it joined: 48 at `-T 0.1` and 48 at
  `-T 1000` (`lifespan_pc_check.sh`, PC, two netns). The scenario-9 and LIFESPAN "harness residuals" (pauses differing
  per arm, the bimodal count at pause 1.5) were that join time. The server now matches, stalls consumption for `-p`
  and drops at take what has outlived `-T`, as the DDS twins: 45 / 95 at pause 1.0 / 2.0 (all expired; CycloneDDS 45
  / 95 on the rig), 0 at `-T 1000` and within the lifespan. On the rig (`LS`), 5 reps: 45 / 70 / 95
  lost at pause 1.0 / 1.5 / 2.0, every one expired, equal to CycloneDDS in every rep; 0 at `-T 1000` and at pause 0.05.
- **Re-scored 2026-10-19 (QoS plan D3):** rows 43-44 counted delivered samples (160/160 all; 147.7 vs 109 of 160).
  KEEP_LAST promises the newest, so they now read the age of a sample when the application takes it, by round trip on
  the sender's clock only: the reader echoes each take over a plain UDP socket outside the middleware, the same for all
  three; cross-host one-way timing is not used. `92059567`, `rig_queue_keep_last_freshness_rt.sh`, cross-host, 3 reps
  per cell, rep ranges; reading pre-registered in `freshness_rt_summary.py`. Control: that socket's own RTT, 195-199 us
  p50 for all three (spread 2-4 us); a 1 ms netem arm moved age and control by 1002-1007 us for every framework. 0
  echoes lost in 45 runs. Age includes the echo's return (~half the control). p99 needs 300 takes a run and is not
  read at 160. Burst cells, same run: 8 per 50 ms slot, p50 TickLE 221-242, FastDDS 369-387, CycloneDDS 308-323 us
  (WIN both); 16 per slot, 234-317 vs 462-521 (WIN) and 318-349 (VOID: gap below the control spread); newest of each
  burst taken 1.000 by all three.
- **Corrected 2026-10-20, row 44's TickLE count:** "147-148 of 160" (`X`) was when the TickLE server's Subscriber
  joined, not the depth (depth 8 and 64 both took 139 of 160 on the PC). The server now matches, stalls consumption
  into a KEEP_LAST 8 cache and takes, as the DDS twins (the user, 2026-10-20; PC: depth 8 takes 108, depth 64 160).
  Re-measured on the rig (`X2`, `6ddcb066`, 45 runs, 0 VOID, fault arm PASS for all three): every framework takes 108
  of 160 in row 44 (the count is a TIE); age p50 WIN in rows 43-44, burst 8 and burst 16 (burst 16: TickLE 243-277,
  FastDDS 436-485, CycloneDDS 382-409 us; TickLE now evicts at take, 1,098-1,403 of 1,600). p99 TIE against
  CycloneDDS, WIN against FastDDS in the burst cells.
- **Corrected 2026-10-17:** rows 11-12 were send rates; now goodput (FastDDS row 11 44.7 -> 28.8 Mbps). TickLE lost 0.
- **Corrected 2026-10-17:** 1c-6bc replace a `4fca013b` run where TickLE lost 1c, 3c and 6bc to CycloneDDS (its ping
  left 111 us late; the `timerfd` deadline wait of `596e9c9c` made it 11 us).

### Latency from creation time (2026-10-17)

R4 control: where nothing waits at the sender, creation and stamp differ only by the sender's timer wake `g`. It
passed in every cell. `C`: 100 round trips per rep; TickLE's `g` 11-12 us, the DDS clients' 58-61 us. rmw ping-pong
control (`Ctl`, not scored): RTT median 0.32 / 0.42 / 0.35 ms. Not yet from creation time: tails 2, 4, 6. L4 is a round trip (below).

### rmw layer

`rclcpp` with one ping/pong pair; `RMW_IMPLEMENTATION` alone picks the rmw (checked in `/proc/PID/maps`). Block wait
is `spin()`; poll wait loops on `spin_some()` with a sleep, at a random phase, scored by interquartile overlap.

RTT mean from creation, ms, rmw_tickle / FastDDS / CycloneDDS; WIN unless marked (DRAW or LOSE against CycloneDDS;
every cell is a WIN against FastDDS):

| wait | Bench 64 B BEST_EFFORT | Bench RELIABLE | Array1k BEST_EFFORT | Array1k RELIABLE | src |
|---|---|---|---|---|---|
| block | 48c **0.250** / 0.379 / 0.336 | 49c **0.251** / 0.395 / 0.327 | 50c **0.270** / 0.402 / 0.351 | 51c **0.276** / 0.419 / 0.335 | Wc |
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
- **Row 63c LOSE:** the cause is in [Every LOSE](#every-lose).
- **VOID(loss):** none of 494 reps; 20 cells CONTROL PASS. The `18f16b85` run lost 14 reps, mostly rmw_tickle's reply to
  seq 1 sent before its publisher matched; `ping_node` warms up first since `d0650110`.
- **Re-read 2026-10-18:** 48c-67c come from `29826912` (7 poll reps, as `18f16b85`). The 1 ns polling-thread timer
  slack (`119d14bd`) cut block wait (48c 0.302 -> 0.250 ms). 59c, 61c WIN -> DRAW, 63c DRAW -> LOSE, 64c DRAW -> WIN;
  the 3-rep `d0650110` run (`Wd`/`Jd`) had read 59c, 61c and 63c the same.
- **Corrected 2026-10-18:** 48c-67c replace the send-stamp rows 48-67 (`934f90de`); 59 and 62 went DRAW -> WIN with
  the session, not the definition.

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

- **RTT, 2026-10-19 (`LR`):** latency is now a round trip on the sender's clock: 1 or 4 MB out, an 8-byte reply
  back through the same rmw (RELIABLE KL10); cross-host one-way timing is not used. Before: one-way from perf_test
  (send stamp; I1 and I1 best effort DRAW against FastDDS as shipped, now WIN). 5 reps x 900 samples, n = 900 in every
  rep (I4s 860, 95.6% of offered), 0 VOID in 160 runs. Control: a small ping on the same rmw and link, rep spread
  0.014 ms p50, 0.119 ms p99; TickLE 0.30, FastDDS 0.39, CycloneDDS 0.33 ms. Every scored letter WIN. I1L: FastDDS as
  shipped answered nothing and CycloneDDS 2.7-4.1% of samples (DELIVERY FAILED, not scored); FastDDS 1472 9.66
  against 59.5 ms. I4s p50: FastDDS 1,007, CycloneDDS 1,204 ms (their publishers fall behind; TickLE drops at the sender).
- **I4s note.** I4s offers more than the link carries. A DDS `publish()` of 4 MB blocks, so its publisher falls
  behind every tick; the send stamp hides that wait (I4s LOSE on the stamp, 54.91 against 37.55 ms) and creation time
  counts it. rmw_tickle does not block and drops at the sender (~84 a run). Delivered % is per offered tick; the p99
  is UNBOUND for every arm and not scored.
- **Corrected 2026-10-16:** I1L was a LOSE on incomplete delivery (f5fcda06, 2 samples in 1 run); first-n3 fixed it.
- **Corrected 2026-10-17:** publisher CPU per MB delivered; I4s CPU pub W–W–W -> D–W–W. Same host: not published.
- **2026-10-18, I4 BE CPU pub L -> W against FastDDS** (`HP`): publisher huge pages (`e00c1ddd`) cut it 3.712 ->
  3.553 ms/MB (FastDDS 3.650; floor 0.022). Part of the old gap was interrupt billing (eth0's interrupt on CPU 0, no
  `IRQ_TIME_ACCOUNTING`; pinned off CPU 0 it was 0.004 ms per sample); huge pages removed the rest. No harm in I1 or
  I4s (I4s publisher -0.133 ms/MB, letters not rescored).

### Large samples step B, same host (rig, `SB2`)

Step B (one copy into the publisher's large region, a descriptor to each reader) against step A (fragments through
the ring), one build, `RMW_TICKLE_LARGE_REGION=0` for A; round trip on the sender's clock, CPU in-process over the
schedule, 5 reps paired. Control: the 8-byte Ack cells, where step B's code does not run: all six held (2 x SE
1.3-2.6%). Every B2 rule PASS:

| cell | CPU per sample, A -> B | RTT p50, A -> B |
|---|---:|---:|
| 1 MiB RELIABLE | 2,406 -> **752** us | 1.390 -> **0.615** ms |
| 1 MiB BEST_EFFORT | | 1.415 -> **0.569** ms |
| 4 MiB RELIABLE | 9,702 -> **2,779** us | 5.815 -> **2.558** ms |
| 4 MiB BEST_EFFORT | | 5.751 -> **2.395** ms |

Against FastDDS and CycloneDDS (shipped same-host defaults): RTT, CPU and RSS WIN in the RELIABLE cells; both vendors
DELIVERY FAILED in the BEST_EFFORT cells (97.4-99.4% delivered). The earlier VOID was perf_test's AnalyzeRunner
spinning (`bcf8624d`); this harness has no spinning thread.

### Hybrid transport (large samples), rig

AUTO against TickLE's own forced MTU mode, not counted (`HY`, Array1m RELIABLE): AUTO delivered 100% at 0.01-1% loss;
wire bytes WIN at 0.01% and 0.1%, LOSE at 1% (+0.46%); p99 LOSE at 0.01% and 0.1% (9.54 against 9.42 ms), DRAW at 1%.

### Measurement debt closed 2026-10-20 (`D2`)

- **DATA_FRAG at 4 fragments, 5% loss** (TickLE only; 800 B datagrams, no vendor arm at that size): 620 Mbps, 8 reps
  (604-631), against 527-536 at `3d1ffeeb`; 4.06 datagrams per sample, so only lost datagrams are resent. Controls:
  2 fragments lossless 943 and at 5% loss 886 Mbps (c6 of `D`: 888-898).
- **Discovery join (M3)**, PC netns, N=8, 20 joins an arm: median 2.2 ms at E=4 and 2.0 ms at E=32, against
  `3d3ed747`'s 2.0 and 1.8 in the same session (published 2.0 and 1.8). PASS (bound: parent + 5 ms).

### No matched reader sends nothing (rig)

With no matched reader nothing goes on the wire (`NS`): writer CPU per sample -69% (p1 1 kHz), -94% (1 MiB 30 Hz).
Cost: a new reader's first sample arrives ~2 ms later at p1.

## 3. Sources

Raw files in `~/rig_results_safe/`, or `examples/perf_hil/results/` for `results/`; harnesses in
`examples/perf_hil/experiments/`. All frameworks of a row come from one session.

| code | harness | build | date | raw file |
|---|---|---|---|---|
| A | `fair_crosshost_remeasure.sh`, `goodput_rescore.py` | `09a0437d` | 2026-10-08 | `fair_crosshost_09a0437d_20261008-112506.A_free.txt` |
| T | the same, FastDDS 1472 (rows 14, 17, 27, 34, 40: `3ae721aa`) | `09a0437d` | 2026-10-08 | `...112506.T_free.txt`, `fair_crosshost_3ae721aa_T6.T_free.txt` |
| C | `campaign_sweep.sh`, `creation_latency_summary.py --rtt` | `596e9c9c` | 2026-10-17 | `creation_latency_core_20261017-204550.txt` |
| S | `comparison_resweep.sh` | `659013e9` | 2026-09-24 | |
| L | `liveliness_l2.sh` | `0f220ba0` | 2026-09-27 | `results/liveliness_l2e_2026-09-27.txt` |
| X | `rig_queue_keep_last_freshness_rt.sh`, `freshness_rt_summary.py` (round trip) | `92059567` | 2026-10-19 | `keep_last_freshness_rt_92059567_20261019-132519.{txt,summary.txt}` |
| X2 | the same, reshaped TickLE server (`HISTORY_DEPTH 8` cache, stall, take) | `6ddcb066` | 2026-10-20 | `keep_last_freshness_rt_6ddcb066_20261020-082304.{txt,summary.txt}` |
| W | rmw block wait | `934f90de` | 2026-09-30 | `results/rmw_4way_block_934f90de_2026-09-30.txt` |
| Wc, Jc | `rmw_crosshost_rtt.sh`, `rmw_crosshost_creation.py` | `29826912` | 2026-10-18 | `creation_rtt_rmw{W,J}_29826912_20261018-111338.*` |
| Wd, Jd | the same, ping warm-up | `d0650110` | 2026-10-18 | `creation_rtt_rmw{W,J}_d0650110_20261018-062244.*` |
| K, Ke | `rmw_keepall_rig.sh` (Ke: equal bounds) | `ae724688`, `29dff630` | 2026-10-06, -08 | `rmw_keepall_rig_O2_20261006-123017.txt`, `rmw_keepall_equal_main_29dff630_20261008-134901.*` |
| Ctl | perf_test ping-pong | `4fca013b` | 2026-10-17 | `creation_latency_ctl_20261017-094944.*` |
| SF | `fair_samehost_remeasure.sh`, `fair_samehost_summary.py` | `e521bbba` | 2026-10-09 | `fair_samehost_e521bbba_20261009-033254.*` |
| SP | the same, `s11_publisher_cpu.py` | `228da18d` | 2026-10-17 | `fair_samehost_228da18d_20261017-144159.*` |
| SC | the same, read at `a89aefe2` (R4 of `f2127ec3`) | `2579e822` | 2026-10-18 | `creation_rtt_samehost_2579e822_20261018-092008.*` |
| SC0 | the same | `596e9c9c` | 2026-10-17 | `creation_rtt_samehost_596e9c9c_20261017-225621.*` |
| SE | `s6_transport_cells.sh` | `e17b4e6f` | 2026-10-04 | `p4_shm_headtohead_e17b4e6f.txt` |
| SB | `samehost_bundle_rig.sh` (SF, SP, SC readers; rotated order) | `60f49f6a` | 2026-10-19 | `samehost_bundle_60f49f6a_20261019-190222.*` |
| SB9 | the same, S9c cell alone | `0117126a` | 2026-10-19 | `samehost_bundle_0117126a_20261019-222752.*` |
| QB | `qos_be_ab.sh`, `qos_be_ab.py` (TickLE only, A P B B P A) | `93ec2b3b`, `d6a007dd` | 2026-10-19 | `ab_samehost_qosbe_20261019-210053.*` |
| QB2 | the same, REPS=8 (16 an arm) | `93ec2b3b`, `d6a007dd` | 2026-10-19 | `ab_samehost_qosbe_20261019-223805.*` |
| D | `debt_crosshost_remeasure.sh` (campaign_sweep.sh, 12 reps, rotated, unpinned; A: c1 c2 c5 c7 c9, T: c3 c4 c6) | `d58873cf` | 2026-10-20 | `debt_crosshost_d58873cf_20261020-011033.{A,T}.txt`, `.reading.txt` |
| D2 | `debt2_remeasure.sh`, `debt2_read.py` (frag_count_scaling.sh, discovery_join.sh) | `df6fbc2c` | 2026-10-20 | `debt2_df6fbc2c_20261020-030104.*` |
| SB2 | `stepb_rtt_rig.sh`, `stepb_rtt_read.py` | `28fd075e` | 2026-10-20 | `stepb_rtt_28fd075e_20261020-181744.{txt,verdicts.txt}` |
| LS | `rig_lifespan_reshaped.sh` (row 47, reshaped TickLE harness) | `3ae5f34f` | 2026-10-20 | `lifespan_reshaped_3ae5f34f_20261020-035439.txt` |
| MX | `mixed_delivery.sh`; CycloneDDS `mixed_delivery_campaign.sh` (`e4e090cd`) | `7f127d56` | 2026-10-05, -17 | `mixed_4e49ed43_20261017-152135.*` |
| RS, RSc | `rmw_samehost.sh`, `rmw_samehost_summary.py` | `06dd78d8`, `29826912` | 2026-10-09, -18 | `rmw_samehost_06dd78d8_20261009-110918.*`, `creation_rtt_rmwsh_29826912_20261018-111338.*` |
| RS2 | the same, rotated order | `69f394cc` | 2026-10-21 | `rmw_samehost_69f394cc_20261021-134220.*` |
| LN | `rmw_loan_bench.sh` (PC) | `ab/lending` + `f837ae82` | 2026-10-08 | `~/rmw_loans_ws/bench3/` |
| L15, L16 | `rmw_keepall_rig.sh`, `largemsg_rig_summary.py` | f5fcda06, dde87857 | 2026-10-15, -16 | `l4main2_*_20261015-094605.txt`, `l4main3_i1rel_20261016-065517.*` |
| LR | `l4_rtt_rig.sh`, `l4_rtt_read.py` (RTT column) | `3583f0d1` | 2026-10-19 | `l4_rtt_3583f0d1_20261019-161849.{txt,verdicts.txt}` |
| LC | the same, `CREATION_TRACE=1` | `4fca013b` | 2026-10-17 | `creation_latency_{i4s,i4be,i4rel}_20261017-094944.*` |
| HP | `pub_hugepage_ab.sh`: pre `26c70597`, head, aa, FastDDS; 5 reps | `e00c1ddd` | 2026-10-18 | `pub_hugepage_rig_20261018-150541.read.txt` |
| HY | hybrid arms | d8980f9b | 2026-10-12 | `hybridLand_s2_loss_20261012-021743.*` |
| NS | `nosend_benefit_rig.sh` | b265e86f vs branch | 2026-10-16 | `nosend_benefit/20261016-020518/reading.txt` |

**Versions:** CycloneDDS 11.0.1 (iceoryx 2.0.5) and Fast DDS 2.14.6 native; jazzy's rmws (CycloneDDS 0.10.5, Fast DDS
2.14.6). Vendors are distribution packages (`-O2`); TickLE `-O2 -DNDEBUG`. Open work: [ROADMAP.md](ROADMAP.md).
