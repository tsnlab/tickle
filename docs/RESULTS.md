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
| RTT mean from creation, block wait, Bench 64 B, ms | **0.245** | 0.383 | 0.335 | 48c |
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

**Behind inside a lead (not scored):** on the pong Pi, rmw_tickle's receive returns ~1.3-2.4 us after the vendors'
(`KH2`, `132548dd`, after the wait ring; Bench BEST_EFFORT, 3 reps x 101 round trips an rmw, wire control PASS: tap ->
receive return p50 15.1 (reps 14.9-16.0) against CycloneDDS 12.7 (12.7-13.0) and FastDDS 13.8 (12.9-13.9) us, SLOWER
against both by the range rule; RELIABLE NOT READ, wire control failed). Read against `KH` (`430ae86d`, the same harness:
18.0 against 12.8 and 13.8): the vendors moved -0.1 and +0.0 us, rmw_tickle -2.9, so the gap NARROWED from +5.2/+4.3 to
+2.4/+1.3 us (`s1_kh_rig_read.py --kh`), as the wait ring's own A/B predicted (-2.59 us). The wait ring waits and reads
in one `io_uring_enter()`, so there is no separate wait return any more. Measured on the `TT_HAL_LIBC_CALLS` build (the
shipped build's raw syscalls are invisible to `sysstamp`, which adds 6-8 us to its round trip). The whole tap ->
callback is still shorter: p50 24.9-26.4 (default build 22.3-23.4) against 38.8-39.7 / 49.4-53.6 us. Corrected
2026-10-21: this line read "kernel wake ~6 us slower" (September, 18.2 against 12.6 us), not split further; corrected
2026-10-23 from `KH`'s ~4-5 us to `KH2`'s. **2026-10-24, `KH2B`:** KH2's first ~20 round trips a run were read by epoll +
recvfrom, not the ring (the executor needed 64 waits to take it over); with the first move at 2 waits (`ab/kh2-receive-return`,
P e9efea98, B 11a838a5 before its rebase, the same call, `kh2ab_20261024-055604`) epoll round trips went 15-19 a rep -> 0
and rmw_tickle@libc's p50 15.9 -> 15.0 us against CycloneDDS 13.0 and FastDDS 13.9 (vendors moved +-0.1): **behind by
+2.0 / +1.1 us**, NOT CLOSED. The rest is the ring's DEFER_TASKRUN path itself (rig `recv_wake_xhost`: blocking recvmsg
8.43 against the ring 9.30 us, kernel stamp -> return); no user-space change found.
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
- **R13's +536 kB** is no single commit: the DDS arms' idle RSS rose +150..260 kB on the rig too; rmw_tickle's code grew
  +124 kB (large samples, hybrid, QoS) with an equal storage saving; the rest is reorder slots touched by the hold-over's
  higher rate (PC: ~+420 kB in runs that hold). R13 reads `ru_maxrss` at exit, not a true peak (+-200..400 kB).
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
- **Row 63c LOSE:** the cause is in [Every LOSE](#every-lose).
- **VOID(loss):** none of 494 reps; 20 cells CONTROL PASS. The `18f16b85` run lost 14 reps, mostly rmw_tickle's reply to
  seq 1 sent before its publisher matched; `ping_node` warms up first since `d0650110`.
- **Re-measured 2026-10-21:** 48c-51c come from `430ae86d` (`WH`, 3 reps x 100 round trips, rotated order, CONTROL
  PASS in all 4 cells, WIN against both unchanged). rmw_tickle against its own `Wc` (0.250 / 0.251 / 0.270 / 0.276):
  4-9 us lower, rep ranges disjoint in 48c and 51c, overlapping in 49c and 50c; the vendors moved -8 to +4 us. Of
  the 63 production commits between the builds, the one aimed at this path is the executor hold-over (`58bd6b1a`: no
  `timerfd_settime()` between the datagram and its callback). Two sessions, not an A/B, so not attributed: the PC had
  not resolved it and the harness expected "within noise". (`Wd`, before the timer slack, read 0.300-0.322.)
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

### Wi-Fi-like burst loss, PC netns (`WB`, network profiles phase 3 baseline)

The current build (ETHERNET, which WIFI maps to) between three private namespaces on a bridge, every egress netem
`delay 2ms slot 1ms 4ms packets 42` plus Gilbert-Elliott loss (L% average, mean burst B packets) both ways; `bc`:
unicast 1%/B2, broadcast 30%/B3; `tv`: clean / 5%B4 / 10%B8 +20 ms, every 5 s. 100 B at 100 Hz (2,900 scored a rep),
1 MiB RELIABLE at 5 Hz (150 a rep). Base round trip 6.90 ms, one-way 3.43 ms. Latency only from reps the PC was not
loaded past nproc/2 (other sessions' builds loaded most of them: 2 of 6 reps per decision cell).

| RELIABLE 100 B | ctl | 1%B2 | 1%B8 | 5%B2 | 5%B4 | 5%B8 | 10%B4 | bc | tv |
|---|---|---|---|---|---|---|---|---|---|
| delivered % | 100 | 100 | 99.86 | 100 | 99.55 | 98.28 | 99.02 | 100 | 98.36 |
| one-way p99 ms | 5.98 | 9.43 | 6.04 | 38.6 | 60.3 | 87.5 | 81.1 | 18.0 | 93.6 |
| resends per lost datagram | - | 4.1 | 2.7 | 3.0 | 3.4 | 2.2 | 2.3 | 3.8 | 2.2 |

BEST_EFFORT delivered 1 - L within the realized loss (the treatment check). Against the pre-registered rule
(`wifi_burst_read.py`): R1 PASS; R2 FAIL at 5%B2 (38.6 > 34.1 ms) and 5%B4 (60.3 > 44.5 ms), with core's
detect-to-recover under 8 ms for ~90% of gaps - the excess is the burst before a gap is seen, which R2's round count
leaves out; R5 FAIL: 0 false peer deaths between the two nodes exchanging data in any arm, but 112 in pairs that exchange
none (summaries only; `bc` 62 across its 15 runs); R6 FAIL once (a 4.0 s join at 10%B4; 1.0-1.1 s at 5%, one lost
announce); R7 FAIL: copies after delivery 22% of delivered under `tv`. R3/R4 (1 MiB) VOID twice: with UDP GSO a netem
skb carried up to 9.2 frames on average, so slot and loss act per skb (p50 5 ms against the control's 47 ms); with
`TT_UDP_OFFLOAD=0` every frame is an skb, and the control itself overflowed the 10,000-packet queue (5,161 drops,
4,995-127,131 units resent a rep, p99 up to 503 ms) - 45-unit IP runs, a lost frame resending the run. Delivered 100% in
every 1 MiB cell; p50 47-99 ms, p99 58-651 ms.

Corrected 2026-10-23 (`WBS`): the storm was neither the queue nor the IP runs. One variant per hypothesis, the control
offload off, 3 reps: netem limit 100,000 stormed 2 of 3 with 0 drops (the drops were the storm's own overflow); MTU
units (`-F mtu`) 3 of 3; no `slot` 0 of 3 (0-151 resent, for DATA gaps); broadcast in the unicast band 0 of 3 (0-112
resent, no timer ACKNACK); the base 3 of 3 at load 47-51 and 0 of 3 at load 2. The cause, in core: a reliable HEARTBEAT
was unicast only from an empty `tx_buffer`, so one sent while a discovery summary was batched there rode it broadcast -
through the empty broadcast band, while the unicast band still held the sample's ~45 ms drain - and announced a sample
still arriving. The reader asked for all of it, and asked again every 1 ms (`retry_initial`: nothing timed yet on a
lossless path) while it drained, each request resent in full. Traced: one broadcast HEARTBEAT, one revealed gap, 7,404
resends; runs with none resent nothing. The fix sends the batched content first and the HEARTBEAT to the peers
(`heartbeat_unicast_count()`). The control with every node pinned to one CPU (their own contention, no load generator),
parent and fix in alternation, 20 reps each: the parent stormed in 7 (5,025-78,840 resent), the fix in 0. Rerun per
frame with the fix (3 reps, ctl 0 resent, load 1-7): R3 PASS at 5%B4 and 10%B4 (100% delivered); R4 PASS at 5%B4 (p50
54.1 <= 74.2 ms) and FAIL at 10%B4 (111.2 ms), reverse bytes 0.25% of forward: by the rule a timer or pacing value, no
wire lift. 1.15 and 1.21 resends per lost unit.

Levers for the Wi-Fi profile, by evidence: (1) the large-sample retry against the link's serialization (`link_rate`, the
retry floor) and pacing - the lossless control storms (corrected above: the storm was a HEARTBEAT on the broadcast path,
fixed in core; R4 at 10%B4 is what is left of this lever, and the 1 ms first retry is what made one false gap 10 samples
of resends); (2) liveliness of pairs with no data - unicast summaries to known peers or a longer silence; (3) the
reader's ACKNACK retry under jitter - timer ACKNACKs outnumber immediate ones 4.7x in the median rep (101x under `tv`),
2-4 resends per loss; (4) the hybrid estimator under bursts - at 1%B8 it kept 2-unit runs, 5.2 resends per lost frame
against 2.0 at 1%B2 in MTU units. Batching was not exercised.

### Wi-Fi discovery against ETHERNET, PC netns (`WW`, network profiles phase 3)

The WIFI profile's discovery (DESIGN "Wi-Fi discovery": unicast summary copies, miss threshold 6, announce repeats),
build `ca540c2c` (af4ca9ab before its rebase; core unchanged by the rebase), against ETHERNET from the same build, both
run at once: the WB harness amended before the run (the large bench takes the profile, each node reports its treatment),
all nine arms, the three cells, 3 reps, `TT_UDP_OFFLOAD=0`. Load per rep: median 1.7 on both, 12 of 81 runs per profile
above nproc/2 (at most 18.8). Treatment as named: WIFI nodes sent 612-666 summary copies and 27 announce repeats a
cell, ETHERNET none.

| | ETHERNET | WIFI |
|---|---|---|
| R5 false peer deaths (27 cells x 3 reps) | 81, all in pairs with no data: `bc` 51, ge10b4 15, ge5b* 13, `tv` 2 | **0** |
| R6 ping join, max (54 joins) | 1.01 s in at least 5 (ge5b2, ge10b4), PASS | 1.05 s once (ge10b4), the rest <= 24 ms, PASS |
| wire bytes per delivered sample, ctl be_small | 169 | 171 |

R5: FAIL -> **PASS**; R6 PASS on both (the baseline's 4.0 s join did not recur on ETHERNET). The controls delivered 100%
on both; ETHERNET's ctl rel_large overflowed netem's queue (1,814 tc drops, as the baseline's), which voids its R1-R4,
not R5/R6. WIFI's other rules, as coded: R1 PASS, R2 FAIL (ge5b2 40.5 > 34.2 ms, ge5b4 61.5 > 44.6 ms, 8,698 and
8,681 latencies), R3 PASS, R4 FAIL at ge10b4 (p50 84.6 > 74.7 ms, reverse bytes 0.24%: a local value), R7 FAIL (1,498
of 8,558). The discovery values do not touch repair; R2's pre-registered reading is the FEC lift, a decision for the user.

### Wi-Fi repair timing, PC netns (`WR`, network profiles phase 3)

**R2 amended before these runs** (the WB harness header and `wifi_burst_read.py`): the limit gains a detection term
D = (B - 1) / R - netem's burst counts packets, so after a lost datagram it lasts B - 1 more at the egress's own rate
R (tc counters) before a later datagram reveals the gap; K, T and the 1.5 factor unchanged. Written after WB and WW
failed R2, so their re-reading is a control of the formula, not a test: WB ge5b2 / ge5b4 38.6 / 60.3 ms, WW WIFI
40.5 / 61.5 ms, FAIL as registered (34.1 / 44.5, 34.2 / 44.6), PASS amended (41.4 / 67.4, 42.0 / 68.4); WW ETHERNET
stays VOID (its ctl rel_large overflowed; the reader printed "R1/R2 pass" there - fixed). The reader prints both.

**The cause.** Traced (ge5b2, 6.9 ms round trip): a reader timed no recovery for the first ~13 s, because every one
took longer than `tt_RELIABLE_RETRY` + 1 waits of `retry_initial` and a recovery the back-off reached is not timed, so
it asked every 1 ms for good - on both profiles, 2-4 resends per lost datagram and the R7 copies. Each candidate rule,
WIFI, against the parent P, under `CLOCK_PPS=1000` (a steady clock flow on every egress, so a burst lasts B ms
whatever a node sends: the parent's extra packets otherwise end its own bursts sooner), `wifi_repair_ab.sh`, 3-4 reps:

| rule | p99 ge5b2 / ge5b4 (median of per-rep) | resends a rep | tv copies after delivery | tv delivered |
|---|---|---|---|---|
| P: as on main (1 ms, never timed) | 22.5 / 22.4 ms | ~650 | 18.5% | 99.64-100% |
| S: first recovery timed past the back-off, then the default rule | 45.9 / 38.2 | ~160 | 0.5% | 99.40-99.54% |
| **K: S, then Karn's rule (only a first-try recovery timed)** | **24.1 / 24.5** | **~160** | **6.8%** | 99.97-99.98% |
| K + interval never below the repair transit's mean | 23.3 / 24.0 | ~160 | 1.3% | 99.44% |

S times retried recoveries, so its estimate grew with the loss (intervals 14-113 ms traced). Also tried, no clock: the
default rule with half the variance weight (R7 3.7%, a `tv` 1 MiB rep resending 445,849); the repair transit as the
interval before the first timed recovery (it puts a busy writer's send into the interval: test_large_sample's
busy-writer sample 1 whole 4.92 ms after its send against 1.95). Landed as WIFI's `repair_timing` (DESIGN "Wi-Fi repair
timing"): K, and a writer of large samples keeps the default rule past the seed - under K its estimate stayed at the
seed (timer ACKNACKs up to 12,480 a rep against ETHERNET's 19-126, one ge5b2 rep resending 208,479 units).

**ETHERNET against WIFI, one build** (the WB harness, all nine arms, three cells, 3 reps, `TT_UDP_OFFLOAD=0`, load per
rep median 0.2, at most 1.4; ETHERNET is main's repair, WIFI the profile's; WIFI's discovery as in `WW`):

| | ETHERNET | WIFI |
|---|---|---|
| rel_small ge5b2: delivered, one-way p99 (limit) | 99.94%, 51.1 ms (40.5) | 100%, 40.8 ms (43.1) |
| rel_small ge5b4 | 99.63%, 60.2 ms (66.8) | 99.49%, 68.6 ms (71.5) |
| resends per lost datagram, ge5b2 / ge5b4 / ge10b4 / tv | 2.2 / 2.9 / 2.7 / 2.3 | 0.97 / 0.97 / 0.93 / 1.07 |
| tv copies after delivery (R7) | 1,088 of 8,589 (12.7%) | 217 of 8,563 (2.5%) |
| rel_large ge10b4 p50 (limit 74.0-74.4 ms), reverse bytes | 110.9 ms, 0.25% | 102.9 ms, 0.25% |
| false peer deaths (R5) | 77 | 0 |

R1 ETHERNET PASS, WIFI FAIL at ge5b4 by 0.01 point (99.49%; 99.72% in the run before, whose small cells ran the same
code); R2 amended ETHERNET FAIL at ge5b2 (its falsifier fired: 500 of 562 gaps recovered within 8 ms of detection),
WIFI PASS both; as registered both FAIL. R3 PASS both. R4 FAIL at ge10b4 on both, reverse bytes 0.25%. R5 ETHERNET
FAIL, WIFI PASS. R6 FAIL once each (ETHERNET a 3.0 s join at ge10b4, WIFI 5.0 s at ge5b4 be_small; PASS on both in
the run before). R7 FAIL on both, WIFI 5 times fewer copies. The same with the clock flow (small cells, 3 reps): R1/R2
PASS on both (WIFI p99 23.4 / 23.7 ms, ETHERNET 22.1 / 22.3), resends 3.4-3.7x fewer on WIFI, R7 FAIL on both
(WIFI 8.1%, ETHERNET 40%).
1 MiB rows otherwise within each other (no WIFI rep storming in 27).

**Reading, by the pre-registered rules.** FEC: WIFI's R1/R2 fail only at ge5b4, and only R1 by 0.01 point, which the
rule reads as XOR FEC losing its parity in the burst - interleaving or RS, a separate decision, not this lift;
ETHERNET's ge5b2 fail reads "lift FEC" but is ETHERNET's repair, not the tuned profile, and its p99 moved 40.5 -> 51.1
ms between two runs of one build. Range NACK: R4 fails with reverse bytes 0.25% < 2%: a timer or pacing value (the
drain: repairs queue behind a 1 MiB sample's ~45 ms in netem), no wire lift. R7: no rule tried keeps both 1% copies
and full delivery under `tv`: a doubly lost sample at a 47 ms round trip recovers inside KEEP_LAST 10's 100 ms only if
the second request goes before the first repair could have come - a speculative copy. That is redundancy's job (FEC),
not a timer's; recorded for the wireless-stage decision, not read as a lift (R7 is pre-registered local-only).

Cost (`initoptions_insn_pc.sh`, A = the parent, P = A placement-shifted, B = the change, 3 reps, 0 void): client user
instructions per sample B vs A (P vs A) p1 -0.01% (-0.01%), p3 +0.00% (-0.01%), t1 -0.00% (-0.00%), t3 +0.01%
(+0.00%), 1% loss l1 +0.00% (+0.01%), l3 +0.02% (+0.02%); every row EQUAL, server rows within their spread.

### R4 at ge10b4: where a 1 MiB sample's repair time goes, PC netns (`R4K`, 2026-10-25, not landed)

**TL;DR.** R4's excess is the reader's retry timer, not netem's queue or the link. Main's interval was 37 ms on a
6.9 ms round trip, because a writer of large samples kept the default timing rule, which times recoveries spanning a
lost repair's wait. Karn's rule for that writer too (K, branch `r4-karn-large`) cuts the ge10b4 p50 from 100.8-104.1 ms
to 74.2-74.7 ms. That is the limit (74.3-74.6), not clearly under it: one run FAIL by 0.4 ms, one PASS by 0.5 ms.
Nothing else got WORSE. R7 is unchanged and FAIL on every tree. Held, not landed: at 10 reps K stormed on the `tv`
arm (main did too: `TVS`, fixed since); re-judged on the fix, K reaches the limit again (74.2 ms) with no storm, but
its ge5b2 reps at ~2x the resends repeat (below).

Reproduced on main b434f649 (WIFI, `TT_UDP_OFFLOAD=0`, ctl and ge10b4, 3 reps, 450 samples a cell, load at most 0.3):
p50 138.4 ms against 74.2, reverse bytes 0.26%. Trace (`LARGE_TRACE`: `r4_trace.patch` on core, `r4_trace_read.py`;
300 samples at ge10b4) splits each sample's time along its last-arriving unit. For the samples near the p50 (108 ms):

| term | ms | what |
|---|---|---|
| drain | 42.2 | the sample's 723 frames through netem's slots; the control has it too (ctl p50 46.6) |
| to first resend | 9.9 | detection, the request's transit, the writer's answer |
| re-ask wait | 49.2 | from a resend that was lost to the next request for that unit; the reader's timer (187 of 326) or its answer to the writer's HEARTBEAT chase (134) |
| writer hold | 3.9 | the resend order holding a request whose resend is not shown lost |
| last transit | 3.1 | the last resend to its arrival |

The writer hands the whole sample to the socket in 1.4-1.8 ms, so all the queueing is netem's. A repair queued behind
fresh units took 17.8 ms (p50; p90 32.9) against 3.2 ms with nothing ahead. On the critical path that queue is only the
first-resend term. Its real cost is indirect: it enters the estimates the timers run on. The reader's retry interval
was 37.4 ms at the p50 (p90 78.4) because the default rule, past the seed, timed recoveries that spanned a retry. It
was kept for large writers on 2026-10-23 (`WR`), before the writer's resend order (a664dae9) existed. That order now
makes a frequent request cost an ACKNACK, not a resend.

K against main, pre-registered (`r4_repair_ab.sh`, committed before the prototype). Trees M = main plus the harness,
A = M in a second checkout (the A/A control), K = M plus the change. All ran at once. Arms ctl ge5b2 ge5b4
ge10b4 bc tv, three cells, 3 reps. The small cells build without `tt_LARGE_SAMPLES`, so the change is compiled out
there: a second control.

| | M | A | K |
|---|---|---|---|
| ge10b4 p50, run 1 (per rep) | 100.8 (100.8 100.9 88.4) | 113.3 (79.8 113.3 137.4) | 74.7 (74.7 73.3 79.4), limit 74.3: FAIL |
| ge10b4 p50, run 2 (ctl+ge10b4 only) | 104.1 (95.2 125.2 104.1) | 99.3 (120.3 83.2 99.3) | 74.2 (71.9 74.2 78.3), limit 74.6: PASS |
| ge10b4 writer resends a rep | 11,989-12,861 | 12,131-12,866 | 10,864-12,670 |
| ge5b4 / tv / bc p50 (run 1) | 53.7 / 57.6 / 48.3 | 55.7 / 69.1 / 48.0 | 55.0 / 56.7 / 48.7 |
| R1 R2 R3 R5 R6 (run 1) | R2 FAIL at ge5b2 (44.0 > 43.2) | R2 FAIL at ge5b2 | all PASS |
| R7 tv copies after delivery | 227 of 8,479 | 265 of 8,495 | 275 of 8,538 |

P2 (no WORSE) passed on both runs. be_small's 0.1-point delivery rule fired for A against M as often as for K against
M, so it is read as noise. K's fall against M was 25.9% and 28.8%, against an A/A spread of 12.4% and 4.6%. P5, the
treatment: on a traced K run the interval was 15.1 ms at the p50 against 37.4. The falsifier did not fire: the p50 fell
26.1 and 29.9 ms, and at least 24.5 ms was required. Two runs needed watching. One K ge5b2 rep resent 13,847 units
against ~5,900 (2.3x, under the 10x storm rule). One run-2 K ge10b4 rep matched at sample 17, so it scored 133 samples.
That is discovery under loss, with none missing after the match.

What is left at K's p50 (75 ms, trace): a lost repair is asked for again 20.8-33.1 ms after the drain ends. The repair
transit estimate, which decides how long a request counts as "on its way", also carries the drain's queue. K2 (branch
`r4-k2`, pre-registered before its prototype) stopped a request counting once the send had passed it by the path's interval. It
was withdrawn before its A/B. On 5 traced reps, 2 collapsed the reader's estimate to an interval of 12-25 us (one
at load 1.5, one at 13.4): 21-42k
timer ACKNACKs and 35-76k resends (Karn's ambiguity; leaving out recoveries an older request had named did not stop
it). In the other reps the wait moved to the writer's resend-order hold, whose timeout and RACK time are also measured
behind the drain. The next lever is the same queue in the writer's repair estimate. No wire change is involved.

**K at 10 reps** (rel_large only, the six arms; M = main, A = M again, K = M + the change; rules registered in
`~/r4_results/*.prereg.txt` before each run):

| run | base | ge10b4 p50 M / A / K ms | writer resends: reps over 2 x M's median | storms (> 10 x) | verdict |
|---|---|---|---|---|---|
| r4_storm | `b434f649` | 112.1 / 96.5 / 73.1 | A 3, K 3 | A 1 (tv, 525,319), K 1 (tv, 1,829,941) | FAIL as registered; the storm was main's (`TVS`) |
| k_rejudge | `1a7b5376` (the fix) | 108.9 / 97.7 / 74.2 | A 1 (bc), K 2 (ge5b2: 12,325, 12,750 against 5,925) | none | storms PASS, every arm NOT WORSE; R4 VOID as registered (no rel_small ctl, so no base_rtt), at the limit (~74.3) read with r4_ab's base_rtt |

K's ge5b2 reps at ~2x the resends recur: 1 of 3, 1 of 10 and 2 of 10, against 0 of 23 for M and A on that arm. Not
landed: R4 is reached only at the limit, and the ge5b2 cost has no mechanism yet (found since: below).

**K's 2x reps: a seed faster than any repair** (`K5`, 2026-10-26, PC netns, not landed). **TL;DR.** A reader's probe
starts at the first request naming its watermark, but a 1 MiB sample's missing unit has nearly always been named
before, while it lay above the watermark, so its repair may already be on its way. Such a probe ends sooner than any
round trip. Under K these short probes are all Karn's rule keeps. When the seed is one of them (~0.2 ms), the interval
locks below every first-try recovery and nothing is timed again for the rep. Two cheap fixes were tried, on K, by the
least repair transit (no wire change, 4 B a writer proxy). Both stop the 2x reps. Neither keeps K's ge5b2 tail
(median per-rep p99 K 177-193 ms, main 254, fixes 247-248), so nothing lands: K's tail gain rests on the same short
samples that sometimes lock it.

| ge5b2 + ge5b4 rel_large, median a rep | main (M, A), 172 reps | K normal, 77 | K 2x, 9 |
|---|---:|---:|---:|
| writer resends | 5,986 | 6,080 | 12,053 |
| reader timer ACKNACKs | 148 | 179 | 7,162 |
| units named / ACKNACKs received | 6,490 / 3,655 | 6,680 / 2,909 | 120,877 / 10,130 |
| resends released: shown lost / timeout / RACK | 443 / 40 / 30 | 476 / 41 / 36 | 1,304 / 1,441 / 4,016 |
| requests held (first resend not shown lost) | 126 | 144 | 11,331 |
| reader duplicates | 302 | 371 | 6,246 |
| missing units opened (the loss) | 5,400 | 5,449 | 5,242 |
| tc drops, writer / reader egress | 5,788 / 174 | 5,841 / 175 | 5,900 / 556 |
| one-way p50, ms | 64.4 | 55.5 | 50.3 |

- **Not the loss, the link or a storm**: the same units go missing and netem drops the same (its loss model, no
  overflow). The extra ~6,000 resends are duplicates. The reader asks every ~0.6 ms, the writer's order holds most of
  those requests (11,331 held), and RACK's time rule and the timeout release the rest while the first repair is still
  queued behind the drain. A 2x rep is faster (p50 50 against 55 ms) and costs the duplicates.
- **The reader's interval** (`r4_trace.patch` + `k5_add_r_event.py`, `k5_trace_read.py`; 35 traced K reps, 20
  of main): normal reps run at 10-20 ms. The one traced 2x rep (`trace3_KT` ge5b2-3, 12,269 resent) was seeded at
  0.04 s by a probe of 210 us whose unit had been named 37.6 ms before. srtt 210 us gave an interval of 631 us for all
  30 s. 0 of the rep's 155 later probes were first-try, so Karn's rule timed nothing after the seed.
- **Nearly every timed probe was ambiguous** (`k5_ambig.py`, 55 traced reps): 4,135 of the 4,139 recoveries timed
  for the path were of a unit named before its probe began; 112 ended under 3 ms. The least repair transit (replayed
  from the oldest of the last 32 requests, `k5_transit_min.py`) is 4.1-4.7 ms in 53 reps. In two main reps the replay
  read 0.01 and 1.98 ms (an original the replay took for a repair, or a request already out of the ring). Seeds under
  1 ms: K 10 of 35 reps, M 8 of 20.
  Main's default rule also times retried (long) recoveries, which pull srtt back up (one M rep seeded at 35 us, timer
  ACKNACKs 1,019, recovered). Karn's rule keeps only the short ones, so a seed of ~0.2 ms is a lock.
- **Not netem's seed**: it differs per rep, and the 2x reps' loss counts are the median's.

**The fixes** (`k5_floor_ab.sh`, rules committed before each run; M = main `06d23f21`, A = M again, K = M + K,
F = the fix; all four at once; 40 reps a tree on ge5b2 + ge5b4, 6 on ctl + ge10b4 with the rel_small ctl, 5 on bc and
tv):

- **Clamp** (`experiment/k5-floor`, `path_recovery_sample()`): a sub-floor recovery is folded as the floor.
- **Discard** (`experiment/k5-discard`, `path_sample_too_short()`): it is not timed at all, like a retried one.

| | K | clamp | discard |
|---|---|---|---|
| 2x reps (resends > 1.5 x M's median), F against K | - | 0/40 vs 3/40, p 0.12; 0/80 vs 5/80 with the registered extension, p 0.03 | 0/40 vs 2/40, p 0.25 |
| COLLAPSE reps (timer ACKNACKs > 5 x M's) | - | 0/40 vs 4/40; 0/80 vs 7/80, p 0.007 | 3/40 vs 2/40: its slow start (below), no lock |
| M, A | 0 HIGH in 120 reps each; COLLAPSE M 1, A 3 of 120 (ge5b4, up to 3,364) | | |
| ge10b4 p50 (R4 limit) | 72.5 / 74.1 ms | 72.97 (74.31) PASS | 78.15 (74.54) FAIL |
| ge5b2 p50 / per-rep p99, M 74 / 254 | 65.8 / 177-193 | 68.4 / 247 | 65.6 / 248 |
| bc per-rep p99 (M, A) | 77 / 89 | 102 (99, 98) | 84 (138, 85) |
| verdict as registered | | CONTROL PASS, MECHANISM and OUTCOME UNDECIDED at 40 (SHOWN and FIXED at 80, a second look), R4 PASS, NO WORSE; TAIL LOSES at ge5b2 and bc (rule written after this run) | MECHANISM, OUTCOME UNDECIDED; R4 FAIL; TAIL LOSES at ge5b2: FALSIFIED as registered |

- **Why the clamp costs the tail** (`k5_sample_lat.py`, `k5_floor_traj.py`, 10 traced reps): its slow samples are a
  rep's first ~20. The seed probe ends at ~0.04 s, inside the first sample's drain, when the least transit is still
  20-36 ms (about 5 ms by the next probe). The seed is raised to that, and the interval stays long for ~2 s.
- **Why the discard does not get it back** (10 traced reps): seeds are 5-10 ms at 0-2.5 s and the slow samples are
  spread through the rep, as on main. Its unseeded start asks every 1 ms (timer ACKNACKs median 298 a rep against
  K's 185), and two ge10b4 reps were slow (89.5, 103.7 ms). The ~250 ms samples are the per-rep p99's second mode
  (M 17 of 20 reps there, K 8-9, the fixes 11-14). What in K's short samples avoids them is not found.
- **Cost of K's 2x reps as they are**: in 7 of 120 K reps this night (13 of 166 with `R4K`'s), ~6,000 more resends
  (~9 MB a 30 s rep), ~6,000 duplicates, 3x the ACKNACKs; those reps are faster (p50 50 ms). No storm, no loss.
- **A real fix needs the repair to say which request it answers** (a request serial echoed in the resend): then a
  probe is timed from that request, whatever its watermark. A wire change: parked with the other wire changes for the
  wireless stage.
- Controls: rel_small is built without `tt_LARGE_SAMPLES`, so the change is compiled out there. Its ge10b4 delivered
  rule fired for A once and for the discard once (98.60% against 98.89%); read as noise.

Data: `~/k5_results/` (`trace1-4_*`, `k5ab*`, `k5abx*`, `k5d*`, `*.verdict.txt`, `existing_counters.txt`).

### The reliable HEARTBEAT fix's follow-ups, PC netns (`LRF`)

The three items the fix left open (CHANGELOG), measured on its successor before anything changed:
`large_repair_followups_netns.sh` (the WB harness, PROFILE=ethernet, offload off; reading registered in it first), its
RSTATS route counters (`flush_tx()`: broadcast with a sample, broadcast with a reliable HEARTBEAT/ACKNACK, addressed
while a batched announce or summary was in it), and test_large_sample's sim. Two of three parent runs were loaded by
other sessions (load 9-38: every amp, slow and keepall rep VOID); the third ran at load 2.5-6. The idle-node control
read 0 on every counter.

| item | measured on the parent (usable reps) | reading |
|---|---|---|
| (b) small DATA broadcast with a batched summary | writer `broadcast_with_data` 0 in every cell (rel_small, rel_large, KEEP_ALL rel_large; ctl, ge5b4; 3 reps each) | NOT SHOWN: an unbatched publish flushes batched content first (`flush_pending_before_unicast()`); holds only for a batching Publisher, by design. Nothing changed; `test_publish_sends_batched_discovery_first()` holds it |
| (c) `request_ack` sends batched broadcast-only content to its peers | `addressed_with_discovery` 0 in every KEEP_LAST cell; 2 in one of 3 KEEP_ALL ge5b4 reps (the cell whose writer solicits) | REAL by the rule, rare: a summary waits <= 1 ms for its tick each interval. Fixed (below), landed 2026-10-24 |
| (a) large samples: 1 ms first retry, no per-unit hold-down | rig-like path (1 Gbit/s netem, one queue, Bernoulli loss), 1 MiB at 30 Hz: 0.1% 1.05 resends per lost unit, duplicates 5.2%; **1% 3.83 and 79%** - one rep of three stormed (251,028 resent, 83% duplicates, 75 of 900 samples lost, p99 421 ms; the other two 1.01-1.02); ctl 0 | **REAL on ETHERNET.** Not fixed here (below) |

(c), the fix and its A/B. On `ab/large-repair-followups-c`: `tt_Publisher_request_ack()` sends what is batched first,
where its flush tick would (`flush_batched()`); `test_publisher_request_ack_sends_batched_discovery_first()` fails on
the parent, two mutants die, and `tests/wire_identity_repair_routes.sh` shows the wire unchanged without batched content
at a solicitation and, with it, the same submessages at the same times with only the batched summary and the batching
Publisher's sample moved from the solicited reader to broadcast. Its netns A/B (`ab_c_routes`, 5 reps a tree in
alternation, 4-5 usable): the parent counted 0 in every KEEP_ALL rep this time, so UNDECIDED by the rule; the fix's tree
counted 1, in a ge5b4 rel_small rep, from the echo's writer - an addressed send the fix does not touch (a retransmission
or an initial HEARTBEAT to one reader, which also flush batched content to that reader). The class costs at most one
summary for the nodes that are not peers, once in minutes of lossy traffic; left for the lead to take or drop. Readers'
ACKNACKs ride a batched summary broadcast 1-2 times a 30 s rep (`broadcast_with_reliable_control` on the reader), by
design (the receiver field confines them). Rig (2026-10-24, A 896d4a29, B 1b54c2f1 before its rebase): core gate v2
`coreAB_1b54c2f1_20261023-223424` PASS, 0/88 WORSE; `lrf_1m_20261024-000628` (Array1m 30 Hz reliable, loss 0/0.1/1%)
and `lrf_4m_20261024-000628` (Array4m 15 Hz, loss 0), pre/head/aa 5 reps each: every rep delivered 100% (lost 0), median
latency and CPU ms/MB equal between pre and head (1 MiB 1%: 9.39-9.47 ms; 4 MiB: 36.47-36.62 ms), and the aa arm alike;
aa is B's own library under a second name (`lib_for()` in rmw_keepall_rig.sh; the run's identity line counts 2 distinct
libraries), so it is a same-binary control of B, not of A. Corrected 2026-10-24: 33801201 called aa the older ack build
8d1c3712, read from the `libs: requested ... ack=` line, which lists what the harness could build, not what an arm ran.
Landed.

(a), what the storm is. In the reps that stormed the reader dropped nothing in its socket and the ACKNACK count was the
calm reps' (777 immediate, 219 timer): each ACKNACK named hundreds of units whose repairs were still queued behind the
path's drain, and the writer resent every named unit each time. The sim reproduces it without a PC: a path that takes
12 us a datagram and queues 4,000, 1% loss, 30 x 1 MiB at 30 Hz - 34,813-475,436 resent for 352-574 lost, 19,989-445,111
dropped at the queue, in each of 4 seeds (the committed case: 427 lost, 243,128 dropped, 259,042 resent). A per-unit writer hold-down (explored, not landed; seeds 7 and 11, 475,436 and
50,045 resent without it) cut it to 26,823 / 24,767 at 5 ms and 9,144 / 7,153 at 20 ms. A fixed hold is a testbed constant (ROADMAP 5a); the landing design is a time-ordered
rule - resend a unit again only once the reader shows it received something sent after that unit's last resend (RACK) -
for a separate A/B with the rig. A slow reader (20 us a datagram, 400 in its socket) loses to the same repair bursts:
564,402 resent for 10 samples, 1.01 per socket drop; the registered per-drop criterion cannot decide there (a mutant
resending every unit twice read 1.09), so that sim case is a measurement and the per-unit guard is the path-loss test. The
netns slow phase (the reader spinning 25 ms of each 33 ms) dropped nothing in its socket in two reps (VOID, the treatment
did not apply) and in the third collapsed the path's queue: 4,644,237 resent, 212 of 900 delivered (VOID by tc drops).

### The large-sample resend order, sim and PC netns (`LRO`)

LRF item (a)'s fix (DESIGN.md section 4, "Large-sample resend order"): a large unit already resent to a reader goes
again only once that reader has shown the resend lost, or a repair timeout has passed. Branch ab/large-resend-order;
P = 896d4a29, its parent. Sim (`test_large_sample`, the parent's tickle.c built against the same test):

| case | parent | the rule |
|---|---|---|
| queued path (12 us a datagram, 4,000 queued, 1% loss), 30 x 1 MiB, resent at seeds 7 / 13 / 23 | 259,042 / 29,331 / 27,024 | 9,541 / 8,235 / 6,049 (the guard: below the samples' 21,720 datagrams) |
| slow reader (20 us a datagram, 400 in its socket), 10 x 1 MiB, resent | 564,402 | 36,999 |
| a tail lost once on a 40 ms unicast path, 4 x 1 MiB, resent | 41 | 4 (the 0 ms control: 4 and 4) |
| busy writer, first 4 MB sample whole after its send ended | 1.95 ms | 2.95 ms (later samples equal) |
| quiet path at 5% loss, the reader's recovery srtt over 2 ms | 1.80 ms | 1.87 ms |

Wire (`tests/wire_identity_resend_order.sh`, both contexts' datagrams with times): the parent against itself the same
in all three scenarios; this tree the same as the parent with nothing lost (5 x 1 MiB + a small sample) and with each
tail lost once on a 0 ms path; different, as designed, with the tail's repair on its way for 40 ms (3,706 writer
datagrams against 3,661); a mutant starting each large send at its second unit different. The default build's object
code is the parent's; FreeRTOS RV32 sizes are equal (`tt_Context` 29,632 B, `tt_Publisher` 960 B, `tt_PeerAck` 24 B:
large samples compile out). rmw builds: `tt_Context` 969,328 -> 1,165,960 B, `tt_Publisher` 2,680 -> 2,944 B.

Netns A/B (`large_repair_followups_netns.sh` PHASE=order, its reading registered first; PROFILE=ethernet, offload off,
1 Gbit/s netem, one queue, 1 MiB RELIABLE KEEP_LAST 10 at 30 Hz, 6 reps a tree in alternation, 30 s each; load-voided
reps left out):

| run (F) | arm | P: storm reps, delivered | F: storm reps, delivered | median rep p99 P / F |
|---|---|---|---|---|
| 1 (the rule) | ge0.1b1 | 2 of 5 (3,980,759 resent in one), 3,685 / 4,390 | 0 of 6, 5,400 / 5,400 | 10.3 / 10.4 ms |
| 1 | ge1b1 | 0 of 6, 5,400 | 0 of 6, 5,400 | 10.7 / 21.4 ms |
| 2 (+ the words below the reader's highest) | ge0.1b1 | 1 of 6 (3,665,344 resent), 4,604 / 5,161 | 0 of 6, 5,400 / 5,400 | 10.6 / 10.6 ms |
| 2 | ge1b1 | 0 of 6, 5,400 | 0 of 6, 5,400 | 11.0 / 17.2 ms |

The control arm (nothing lost) resent 0 on both trees each run, its p99 within 0.00-0.04 ms between them. By the
registered rule ge1b1 is UNDECIDED both times (the parent did not storm there) and a COST: F's 1% p99 is 6-11 ms
higher. The storms came at 0.1% this time, where P stormed in 3 of 11 reps and lost 1,262 of the 9,551 it published, F in
none of 12 and lost none (outside the registered arm: a reading, not the verdict). The cost's likely cause (read from
the design and the sim, not instrumented in the run): a lost repair that is the highest a burst resent has nothing
sent after it to show it lost, and waits the repair timeout (2 x srtt) where the parent resent it at the reader's
next request; run 2's change cut that from 21.4 to 17.2 ms. A third run
(`ab_order_v7`, not landed) timed the srtt only from ACKNACKs that bracket the arrival: 16.3 against 11.0 ms, no
better, and a busy writer's seventh sample 8.35 ms late in the sim, so it was dropped.

WIFI (`wifi_repair_ab.sh` TAG=lro_685a, its reading registered in it; PROFILE=wifi with its own repair timing,
run 2's tree against P, rel_small - small samples, which the change cannot touch - and rel_large, 9 arms, 4 reps, the
two trees at once): NOT WORSE in all 18 cells; no STORM rep on either tree; rel_small medians within the reps' spread
(ge5b2 43.9 / 43.7 ms, ge5b4 70.0 / 65.3). rel_large, pooled: copies after delivery P / F 5,959 / 225 at ge1b2, 3,590 /
1,544 at ge5b4, 20,010 / 1,579 at ge10b4, 17,059 / 1,925 at tv; resends per lost datagram 2.57 / 1.28, 1.14 / 1.08,
1.38 / 1.02, 1.86 / 1.29; delivery 100% on both; one-way p99 mixed (ge1b2 217.7 / 92.9 ms, ge5b4 257.3 / 385.7, tv
739.5 / 661.0; 4 reps at 5 Hz, ~1% of 600 samples each). The harness's own R1-R7: F fails R2-as-registered at
ge5b2/ge5b4, R4 at ge10b4 and R7 at tv, as P does; P alone fails R1 at ge5b4 (99.41%), R2 at ge5b2 and R6.

### The resend order's tail, PC netns (`LRT`)

The cost LRO left (ge1b1 median rep p99 11.0 -> 17.2 ms), instrumented and then fixed with RFC 8985's time rule
(DESIGN.md section 4, "The tail: RACK's time rule"); branch ab/large-resend-order-tail, base P = d1c900fe.

Cause (RSTATS `lro`, `tt_RELIABLE_STATS` only; the base's tree plus counters, ge1b1, 2 reps x 900 samples,
`large_repair/lrt_diag1/`): 12 and 14 held units a rep released by the repair timeout, 8 and 12 of them with nothing at
all sent after their resend; each asked for again soon after its resend and released 4-16 ms after that request
(the timeout itself, 2 x srtt = 4-16 ms - the reader's request-to-recover is mostly 4-8 ms, repairs queuing behind a
sample's 8 ms burst). 12-14 late samples of 900 are the rep's p99 (16.0, 19.7 ms). The hypothesis held.

Sim (`test_large_sample`): the two new checks fail on the base's tickle.c and pass here; 16 resend-order mutants
(5 new) die. Queued path, resent at seeds 7 / 13 / 23: 13,276 / 13,757 / 9,489 (base 9,541 / 8,235 / 6,049; the guard,
21,720, holds); slow reader 38,893 (base 36,999); a tail lost once on a 40 ms path 4 and 4 (as the base); busy
writer's first 4 MB sample 2.95 ms, quiet path srtt 1.87 ms (as the base). Wire (`wire_identity_resend_order.sh`):
against the base identical in all three scenarios, the 40 ms one included; against the order's parent as the base was.

Netns A/B (`large_repair_followups_netns.sh` PHASE=tail, its reading registered first; PROFILE=ethernet, offload off,
1 Gbit/s netem, 1 MiB RELIABLE KEEP_LAST 10 at 30 Hz, 30 s reps, O = 896d4a29 the order's parent, P, F = ed46fcc6,
5 reps a tree in alternation; reps starting above load 4 left out, `large_repair/ab_tail_ed46/`, re-read in
`ab_tail_ed46_reread/` after the delivery comparison was corrected to a share - F lost no sample):

| arm | O: reps, storms, median rep p99 | P | F | F's held units released by order / RACK / timeout |
|---|---|---|---|---|
| ctl | 4, -, 9.6 ms | 4, -, 9.8 | 3, -, 9.7 | 0 / 0 / 0 |
| ge0.1b1 | 4, **2** (resent / missing 3.68), 10.6 | 4, 0, 10.6 | 3, 0, 11.0 | 233 / 501 / 332 |
| ge1b1 | 5, 0, 11.3 | 3, 0, 19.6 | 3, 0, 14.1 (11.5 14.1 15.2) | 160 / 50 / 3 |

Verdicts as registered (N = 0.18 ms): ge1b1 SHOWN, 19.6 -> 14.1 ms, 0.66 of P's cost over O taken back; ge0.1b1 COST,
10.6 -> 11.0 ms (3 reps against 4); no storm on P or F, delivery 100% on all. Copies: reader duplicates a rep about
equal to P's (ge0.1b1 354 / 347). What is left at ge1b1: the RACK releases still wait 1-4 ms past the reader's request
(10-19 a rep) - RACK.rtt is a drain-length round trip too; timeout releases fell from 12-14 a rep to 0-2.

WIFI (`wifi_repair_ab.sh` TAG=lrt_1157, PROFILE=wifi, F = `9f6f5375` (1157d024 before its rebase) against P, 9 arms, 3 reps, the two trees at
once, no rep loaded): rel_large NOT WORSE in all 9 cells, no STORM rep, delivery 100%, writer resends within P's rep
range or below (ge5b2 5,644-6,008 against 6,099-6,128). rel_small - built without large samples, so the change is not
in its binary - read WORSE on delivery at ge1b8 (99.52 / 99.93%) and ge5b8 (98.24 / 98.82%): the control's own
spread at 3 reps, the bar the large cells are read against.

Rig, the base (d1c900fe) against e9efea98 (`lro_1m_20261024-043242`; `aa` is head's own library again, an A/A of
B - an earlier version of this paragraph called it the older build 8d1c3712, read from the `libs: requested` line):
Array1m 1%'s p99 and CPU split by rep on the hybrid transport's mode, not on the tree. Reps whose writer stayed on
IP-fragment runs of 2 (GSO sends 0-660) read p99 13.7-14.0 ms and sub/pub CPU ~5.5 / 4.6 ms/MB; MTU-mode reps (GSO
sends ~39,580) 9.8-10.7 ms and ~4.6 / 3.5 on all three arms. The tail, core gate v2 `coreAB_f002166c_20261024-094912`
PASS 0/88; `lrt_4m_20261024-111914` (Array4m 0%) equal on all arms, lost 0.

Rig, 12 reps an arm (`LRTR`, `hmr_1m_20261024-192010`, Array1m 30 Hz reliable KEEP_LAST 10, 1%; main 36700555, base d1c900fe,
tail f002166c, aa = the tail again; every rep delivered 100%). The IP-run mode (publisher GSO sends < 10,000) is not the
tree's: main 9/12, base 5/12, tail 5/12, aa 4/12 (Fisher, main against each, p 0.10-0.21; tail against aa p 1). Within
a mode CPU is equal on all four (IP ~5.4 / 4.4 ms/MB, MTU ~4.6 / 3.45). Median p99, IP / MTU mode: main 10.01 / 9.64
ms, base 13.87 / 10.29, tail 10.75 / 10.09, aa 10.85 / 10.00 - the tail's cost against main is 0.4-0.8 ms (5-8%) at
1%, its gain the 0.1% storm the PC shows and the rig does not. Landed on the user's call.

### The tv repair storm, PC netns and rig (`TVS`, landed 2026-10-26)

**TL;DR.** Main's Wi-Fi `tv` arm (1 MiB RELIABLE at 5 Hz) stormed in 2 of 80 untraced reps (525,319 and 845,268 units
resent against a median ~10,000; the K candidate in 3 of 40, up to 11,623,361). The cause was in core. The resend order's repair timeout (2 x srtt) took Karn samples
from units an ACKNACK left out. A reader also leaves out repairs it reckons still on their way, so such an ACKNACK can
arrive microseconds after the resend. A run of those samples took srtt from ~40 ms to under 128 us; the timeout then
released every held unit at every request, and the resends overflowed netem's 10,000-packet queue. The fix (branch
`storm-fix`, core `8d0d2eed`): a left-out unit is still taken as arrived but never timed into that srtt; RACK still takes
it. Pre-registered A/B, 30 reps a tree: collapsed-timeout reps main 25 of 60, fix 0 of 30 (p < 0.0001); storms main 1,
K 2, fix 0, fix + K 0; no Wi-Fi arm WORSE; ETHERNET's 1% tail NO COST against the A/A's spread (+0.7 ms, not ruled
out).

**Rig** (2026-10-26, A = main `b434f649`, B = `5b7c09e7` on `ab/storm-fix`; reading registered in
`~/rig_jobs/storm_fix_queue.sh` before the run): core gate v2 PASS, 0/88 WORSE (one reported row, c17 server user
instructions +0.18%, t 1.0). Array1m 30 Hz KEEP_LAST 10, 5 reps an arm (pre = A, head = B, aa = B again): delivered
100% in every arm; median p99 head - pre 0.00 / +0.02 / 0.00 ms at 0 / 0.1 / 1% (|aa - head| 0.04 / 0.04 / 0.09);
Array4m 15 Hz 0%: 100%, p99 36.62 against 36.64 ms (aa 36.55). The PC's +0.7 ms at 1% does not show on the rig.

**The loop** (main, `wifi_repair_ab.sh` PROFILE=wifi `TT_UDP_OFFLOAD=0` LARGE_RSTATS=1; writer RSTATS, reader RSTATS,
the writer egress's tc counters):

| rep | resent | of them released by the timeout | timeout when released | units named / ACKNACKs | writer egress tc drops | reader socket drops | delivered |
|---|---:|---:|---|---:|---:|---:|---:|
| calm (r4_storm A tv-6) | 8,489 | 90 | 8-16 ms | 11,970 / 1,917 | 6,471 | 0 | 150 |
| storm (r4_storm A tv-5) | 525,319 | 430,633 | 430,291 under 256 us | 545,448 / 2,326 | 407,389 | 0 | 145 |
| storm (ab1 M tv-8) | 845,268 | 737,588 | 734,397 under 512 us | 874,886 / 2,439 | 721,586 | 0 | 148 |
| storm (r4_storm K tv-10) | 1,829,941 | 1,620,036 | 1,617,999 under 256 us | 1,875,799 / 2,350 | 1,680,378 | 0 | 143 |

- **The same units, again and again**: M tv-8 opened 36,250 missing units and resent 845,268 (23 a unit); each ACKNACK
  named ~360. The reader's socket dropped nothing. netem's queue did: the calm rep's ~6,400 drops are the loss model,
  the storm's 400,000-1,700,000 are overflow.
- **Why the timeout fell** (`storm_trace.patch`, `storm_trace_scan.py`; 105 traced main reps and 60 of K): Karn
  samples from the watermark were never under 1 ms; samples from units left out were, in 152 of 165 reps (up to 244 a
  rep), and the timeout stood under 1 ms at some point in 50. 48 of those recovered without a storm, once watermark
  samples brought srtt back (2,073-7,034 timeout releases first in three of them); two stormed. The fix traced (30
  reps): 2,087-2,414 watermark samples a rep, least srtt 7.5 ms, least timeout 15 ms.
- **What sustains it**: each overflowed repair is named again, released by the collapsed timeout and resent into the
  full queue; each new resend is a first resend whose next skipping ACKNACK gives another microsecond sample.
- **One storm traced end to end** (`trace2` T2 tv-22, main, 8,811,630 resent): the rep's first sample gave 55
  left-out samples (median 111 us) and no watermark sample, so srtt began at 148 us - a first sample sets it outright
  - and the timeout at 297 us. A second later the writer resent ~540,000 units a second into a link that carries
  ~17,000; netem dropped 8,467,138. For the rest of the writer's trace (capped at 8.7 s) no Karn sample was taken -
  every unit had gone more than once and the watermark could not move - so srtt stayed at 148 us: nothing in the loop
  can correct it. The writer's CPU went from 0.38 s to 16.2 s a rep, working through ACKNACKs naming up to ~4,000 units
  each; the reader, receiving nothing but duplicates (~16,700 a second), sent no ACKNACK for 9 s of it.
- **What ends it**: not the timeout. The stuck samples leave the writer's KEEP_LAST history (938,195 requests answered
  as evicted; the reader skipped 24 times; 81 of 150 delivered), the reader stops naming them, and a clean `tv` phase
  drains the queue. Until then the rep is lost: p99 3.9 s (2.0-5.6 s in the other storms).
- **Not the cause**: the reader's socket (0 drops), the HEARTBEAT chase (the reader's answers to it, 600 a rep, are the
  same in calm and storm reps), netem's tv schedule (the fix's tree ran the same schedules with no overflow: tc drops at
  most 6,922). The reader's own retry estimate collapses too in some reps (timer ACKNACKs 1,300-25,558 against ~80,
  the K2 note in `R4K`); there the writer holds the requests (up to 789,943 held) and resends little - a separate loop,
  contained, not this one.

**The fix** (DESIGN.md section 4, "Large-sample resend order"): a unit an ACKNACK leaves out counts as arrived for the
order (its next request is answered at once, as before) and its round trip goes to RACK, whose rule releases a unit's
first resend only. It is never folded into the timeout's srtt, and its slot stays marked untimed until the watermark
passes it, so a later copy arriving as the earlier one is not timed either. No wire change; `tt_LargeRepair` stays
24 B. Sim (`test_large_sample`): a 20 ms unicast path at 5% loss, the timeout's least srtt 0.000 ms on the parent
(2,893 resent), never timed below the path here (975 resent); queued path 16,274 / 9,551 / 10,621 resent at seeds
7 / 13 / 23 (parent 13,276 / 13,757 / 9,489; guard 21,720); slow reader 40,537 (38,893); quiet path's recovery srtt
after the path became 8 ms 10.09 ms (parent 6.53; the check's band is 6-12 ms). 19 resend-order mutants die (3 new).
`wire_identity_resend_order.sh`: identical to the parent in all three scenarios.

**A/B** (`storm_tv_ab.sh`, reading registered in it and `storm_tv_read.py` before the first netns run; M = main and
the harness `849fdccf`, A = M in a second checkout, F = `8d0d2eed`, K = M + 11908dea, FK = F + 11908dea; all five at
once):

| tv, 30 reps a tree | M | A | F | K | FK |
|---|---:|---:|---:|---:|---:|
| COLLAPSE reps (a timeout release under 1 ms) | 12 | 13 | **0** | 11 | **0** |
| STORM reps (> 10 x main's median rep) | 1 (845,268) | 0 | 0 | 2 (554,816; 11,623,361) | 0 |
| resends a rep, median / max | 9,966 / 845,268 | 10,141 / 29,840 | 8,971 / 11,671 | 9,233 / 11,623,361 | 8,721 / 17,909 |
| timeout releases a rep, median | 253 | 436 | 91 | 229 | 124 |
| reader duplicates a rep, median | 1,492 | 1,190 | 729 | 1,216 | 884 |
| writer egress tc drops, max | 721,586 | 8,600 | 6,922 | 11,212,355 | 6,889 |
| delivered | 99.96% | 100% | 100% | 98.18% | 100% |
| median rep p50 / p99 ms | 70.1 / 603.0 | 71.8 / 628.2 | 66.5 / 629.1 | 68.9 / 541.1 | 68.0 / 538.9 |

Verdicts as registered: CONTROL PASS (M 12/30 against A 13/30, p 1.0); MECHANISM **SHOWN** (25/60 against 0/30,
one-sided p 4.5e-6); OUTCOME UNDECIDED (reps over 2 x the median, not reader-driven: 2/60 against 0/30, p 0.44 - the
storm itself is too rare for 30 reps, as the header said); NOT WORSE on tv. K still storms on main's timeout (11/30
collapsed, two storms, one of 11.6 M resends and 76 of 150 delivered); on the fix, none. Arms (10 reps, the same
trees but K; four reps a tree started above load 8, other sessions' VMs, the same reps for all three): NOT WORSE at
ge5b2, ge5b4, ge10b4 and bc; median rep p50 F / M / A: ge5b2 74.9 / 76.8 / 74.7 ms, ge5b4 53.3 / 54.1 / 54.0, ge10b4
98.9 / 106.1 / 127.9, bc 48.0 / 48.1 / 48.2; resends within 1% of the higher main tree or below; delivered 100% (A
99.80% at ge10b4).

ETHERNET tail (`large_repair_followups_netns.sh` PHASE=tail: 1 Gbit/s netem, 1 MiB RELIABLE KEEP_LAST 10 at 30 Hz,
trees in alternation, O = main's second checkout as the A/A). Most of the timeout's samples had come from left-out
units, so the fix could cost LRT's tail. Run 1 (5 reps): ge1b1 COST as registered (median rep p99 P 11.9, F 12.8 ms,
ctl band 0.16), but the A/A read 13.1: main's own code fails that rule by 1.2 ms. Run 2 (10 reps, the rule amended
before it: band = max(ctl band, |O - P|), `storm_tail_read.py`): ge1b1 P 12.1 / F 12.8 / O 13.9 ms, **NO COST**
(+0.7 against a 1.8 ms band); ge0.1b1 10.2 / 10.0 / 9.8, NO COST. No storm, 100% delivered, resends per lost unit
1.01 at ge1b1 on all three. F sat 0.7-0.9 ms above P in both runs and below O in both: a cost of that size at 1%
is not ruled out at 10 reps.

### The reader's retry estimate still collapses on main, PC netns (`RLC`, 2026-10-26; fixed by `RLF`, 2026-10-27)

**TL;DR.** Yes, in every Wi-Fi 1 MiB cell, at about 1 rep in 60, and on ETHERNET's 30 Hz cell at about 1 in 12. The
cause is the reader's own seed, not the writer storm. A probe on the watermark is timed from the first request
naming the watermark. The unit was named 10-20 ms before that, while it lay above the watermark, so the repair
answering that older request can arrive microseconds later. A seed of 3-90 us sets the interval to tens of us. An
honest probe (>= 4 ms) then runs past the default rule's 3 retries and is never timed, so nothing corrects the
estimate. It locks for 2-9 s, or for the whole rep when the seed is 3 us. No loss, no storm. The writer's resend
order holds the extra requests. The cost is 2-3x resends and duplicates.

Census (`reader_collapse_census.sh`, reading registered in `reader_collapse_read.py` before the run; main `81daa880`,
core = `8d0d2eed`; WIFI rel_large, 1 MiB RELIABLE at 5 Hz, offload off; T1-T4 = main + `r4_trace.patch` +
`k5_add_r_event.py`, U = main untraced, all at once; E1 = traced main, ETHERNET's amp cell, 30 Hz, 1 Gbit/s; COLLAPSE =
timer ACKNACKs > 5 x the arm's median rep and >= 500):

| cell | reps (this run) | COLLAPSE | timer ACKNACKs median / max | earlier main-core reps on disk | pooled |
|---|---:|---:|---:|---:|---:|
| tv | 150 | 2 | 76 / 31,661 | 0 / 80 | 2 / 230 |
| ge5b2 | 150 | 5 | 167 / 18,395 | 1 / 140 | 6 / 290 |
| ge5b4 | 150 | 2 | 114 / 34,075 | 4 / 140 | 6 / 290 |
| bc | 150 | 1 | 58 / 4,967 | 0 / 30 | 1 / 180 |
| ETHERNET ge0.1b1 (amp) | 60 | 5 | 6 / 1,063 | 1 / 15 | 6 / 75 |

- **Trace control** PASS: traced 7/480 against untraced 3/120 (p 0.43). Every COLLAPSE rep started at load <= 2.6 on 16 cores.
- **Not the storm fix's leftover**: before `8d0d2eed`, tv collapsed 8 of 185. In the 4 traced pre-fix collapses the
  reader's interval fell first, and 2 of them had no writer timeout under 1 ms (`reader_collapse_prefix.py`). The
  earlier 0 of 80 on main was chance.
- **Mechanism H SHOWN as registered** (7 of 7 traced Wi-Fi COLLAPSE reps hold P1-P3). >= 50% of timer ACKNACKs fired
  at an interval under 1 ms. srtt fell under 1 ms through an ambiguous sample, and in every rep that sample was the
  seed itself (3, 3, 26, 36, 42, 65, 90 us). Inside the lock no timed sample reached 4 ms.
  - Example `rlc1_T1` ge5b4-8 (`reader_collapse_seed.py`): unit 447 was first named at 26.5 ms. The watermark reached
    it at 41.310 ms and the repair arrived 3 us later. The interval was 23 us, then 10 us. 34,075 timer ACKNACKs, of
    which 32,646 backed off, and not one more probe was timed in 30 s.
- **What decides it is the seed's size.** Seeds under 100 us: 7 of 16 reps collapsed. Seeds of 100 us or more: 0 of
  464. 111 normal reps also went under 1 ms srtt, for a median 0.4 s: a sample a little longer than srtt, within
  3 retries, ratchets it back up (e.g. 42 us -> 1.6 ms in 9.2 s).
- **ETHERNET**: same shape (seeds 2-24 us, ambiguous), held 7-30 s. It is descriptive only; P3's 200 us PATH was not
  measured for that path.
- **Cost of a COLLAPSE rep**: 100% delivered. p50 50-57 ms against ~55-70. Resends up to 21,875 (median ~6,000-8,600).
  Writer requests held up to 618,883. Duplicates up to 13,047 against a median ~50-800. Reader CPU 1.7x.

**Fix candidates** (none written):
1. **The least-transit floor ported to main** (`experiment/k5-floor`, measured only on K). Take no path
   sample faster than the least repair transit the reader has timed. On main this needs two changes: apply it on every
   link, since ETHERNET collapses the same way, and apply it to the seed. On K it stopped the lock (COLLAPSE 0/80
   against 7/80). Its ge5b2 tail (247 ms) was main's, not worse. No wire change; 4 B a writer proxy. A/B: this census
   as the harness, ~300 reps a tree (0 of 300 against 10 of 600 is p ~0.02).
2. **Backstop**: a probe that goes past the retry limit while lasting longer than the interval's ceiling (64 x srtt)
   shows the estimate is impossible. Drop srtt and re-seed. Reader-only; no wire change. It ends a whole-rep lock but
   does not stop the seed.
3. **The real fix**: the repair echoes the serial of the request it answers, so a probe is timed from that request.
   This is a wire change, parked with the others for the wireless stage (`K5`).

Data: `~/rlc_results/` (`rlc1_*`, `rlc1.read.txt`, `rlc1.census.log`, `old_traces_mech.txt`,
`prefix_writer_vs_reader*.txt`). Candidate 1 is measured in `RLF` below.

### The least-transit floor stops the collapse and costs some latency, PC netns (`RLF`, 2026-10-26, landed 2026-10-27)

**TL;DR.** The floor stops the collapse: 0 of 840 reps against main's 22 of 840 (p 2e-7). It also cuts resends 1-15%
and duplicates 9-46%. The cost is latency, and one registered row fails: tv's median rep p50 is 74.3 ms against main's
68.5 (band 3.4). The Wi-Fi tails are higher too, within their bands: ge5b4's median rep p99 is 274 against 256 ms,
bc's 99 against 92. Main sits below any repair transit because of the short samples, so it asks sooner. LAND by the
rules: no. The user chose to land it anyway (2026-10-26), after a rig check, which passed.

- **The change** (branch `ab/reader-floor`): `path_recovery_sample()` folds a path recovery as no faster than
  `tt_WriterProxy.transit_min_ns`. That field is the least repair transit the proxy has timed, from the oldest
  remembered ACKNACK naming a missing unit to a targeted copy's arrival (`note_repair_arrival()`). The seed needs no
  case of its own. On the same arrival the transit is timed before the probe ends, and the probe's request named the
  unit, so the floor includes that unit's own transit. An unambiguous recovery equals its arrival's transit and
  passes as it is. The rule applies on every link, under both timing rules. It adds 4 B in existing padding and no
  wire change. Limit: the floor is 0, or its unit's transit short, only when 32 later requests that did not name
  the unit pushed out every one that did.
- **Test**: `test_no_recovery_is_folded_faster_than_the_least_repair_transit` (main folds the 3 us seed as 3 us; the
  fix folds 11.003 ms). Three mutants in `mutants_large_sample.py`, all die.
- **A/B** (`reader_floor_ab.sh` + `reader_floor_read.py`, committed `3e8c6fd7` before the run): M = main `a5b52eee`,
  A = main again, F = the fix, all at once. WIFI 1 MiB at 5 Hz, offload off, 160 reps a tree an arm; ETHERNET amp
  ge0.1b1, 30 Hz, 200 reps; R4's ctl / ge10b4, 8 reps. The run stopped by the clock at 16:30. Load 0.2-2.1.

| cell | COLLAPSE M / A / F | median rep p50 ms M / A / F | median rep p99 ms M / A / F | resends M / F | duplicates M / F |
|---|---|---|---|---:|---:|
| ge5b4 | 2 / 2 / 0 | 53.2 / 53.4 / 53.8 | 255.8 / 254.6 / 273.6 | 6,203 / 6,119 | 450 / 410 |
| ge5b2 | 2 / 0 / 0 | 74.3 / 74.7 / 76.1 | 254.3 / 254.7 / 257.4 | 5,923 / 5,871 | 180 / 164 |
| tv | 1 / 1 / 0 | 68.5 / 70.4 / **74.3** | 602.0 / 602.4 / 603.7 | 8,628 / 8,380 | 683 / 524 |
| bc | 4 / 5 / 0 | 48.3 / 48.2 / 48.2 | 92.1 / 89.2 / 99.2 | 1,481 / 1,265 | 62 / 49 |
| ETHERNET ge0.1b1 | 13 / 8 / 0 | 9.53 / 9.53 / 9.53 | 10.57 / 10.69 / 10.63 | 3,463 / 2,933 | 260 / 142 |

- **Rules**: CONTROL A 16/840 against M 22/840 (p 0.41), PASS. COLLAPSE FIXED (F 0/840, one-sided p 2.1e-7; against
  M + A, 1.8e-7). NO WORSE: 1 row WORSE (tv p50 +5.9 against a band of 3.4). The other 54 rows are NOT WORSE: CPU
  within ±1%, delivered 100% on F (ge5b4 99.95%, as M and A). R4 NOT WORSE: p50 M 105.0, A 108.4, F 102.9 ms, and all three
  fail the harness's own 74 ms line, as main did in `K5`. ge10b4 rel_small p99 82.0 against 82.8 ms.
- **The latency cost goes in one direction** (rep quantiles, `rlf1.latency_quantiles.txt`). Mean rep p99: ge5b4 304
  against 271 / 270 ms (q90 402 against 338), bc 111 against 98 / 96, tv 648 against 616 / 631. Mean rep p50: tv 70.9
  against 66.8 / 67.5, ge5b2 77.4 against 74.6 / 75.2. A traced side run (`diag/d1`, 6 reps each of ge5b4 and tv, not
  registered) shows why. F's time-weighted srtt q10 is 13-20 ms at tv against main's 5-10 ms, and 6-10 against 2-8 ms
  at ge5b4. Main's short ambiguous samples keep its estimate under the least repair transit. Its retries then come
  sooner, which buys latency for duplicates. The floor gives that up.

- **Rig** (2026-10-27, pre = main `fc5fb79b`, head = `ab/reader-floor-v2` `dc4fa180`, aa = head again). The landing
  rule, set before the run: core gate v2 PASS; in every Array cell head delivers no less than pre by more than |aa - head|,
  and its median p99 is not above pre's by more than max(|aa - head|, 1 ms). Core gate v2
  (`coreAB_dc4fa180_20261026-223203`): PASS, 0/88 WORSE. `rmw_keepall_rig.sh`, 5 reps of 60 s, delivered 100% on every
  arm and cell. Median p99 aa / head / pre: Array1m 0% 9.39 / 9.38 / 9.38 ms, 0.1% 9.46 / 9.45 / 9.44, 1% 9.94 / 10.06 /
  10.04; Array4m 0% 36.62 / 36.64 / 36.63. LAND.

Data: `~/rlf_results/` (`rlf1_*`, `rlf1.read.txt`, `rlf1.driver.log`, `rlf1.latency_quantiles.txt`, `diag/`); rig
`~/rig_results_safe/` (`queue_core_ab_rlf.log`, `rlf_1m_20261027-000449*`, `rlf_4m_20261027-000449*`).

### The ETHERNET amp writer storm: a frozen timeout after a whole-sample loss, PC netns (`EWS`, 2026-10-26, not fixed)

**TL;DR.** Main still storms on ETHERNET's amp cell (1 MiB RELIABLE at 30 Hz, 1 Gbit/s): 30 of 2,400 traced reps,
and the rate follows the PC's load (0 of 495 reps started at load under 2, 8 of 1,251 at 2-4, 22 of 654 at 4 or
more). It is not TVS's left-out samples. Two things in the writer's resend order combine. srtt is folded once per
unit, so one ACKNACK that passes ~250 once-resent units sets it from ~5 ms to ~1.5 ms within a microsecond. The
timeout is a fixed 2 x srtt. When the reader's receive drain overruns, a whole sample (722 units) is asked for at
once. The loaded link takes ~9.5 ms to carry that resend batch, longer than the timeout. Every ACKNACK then releases
every unit it names again, every unit has gone twice (so Karn takes no sample and the timeout cannot grow), and the
resends build up to a second of netem queue, or overflow its 100,000 packets. A per-unit back-off stops it: with the
timeout pinned at ~1.5 ms and a whole sample dropped every 3 s, 10 of 20 reps stormed without it and 0 of 20 with
it (p 0.0002). No wire change is needed.

- **The rep that raised it** (`RLF`'s A tree, main `a5b52eee`, rep `e5_A-6`, untraced), against two calm neighbours:

| rep | resent | released by the timeout (its value) | ACKNACKs / units named | answered as evicted | writer egress tc drops | reader drain overruns (grows) | reader timer ACKNACKs | published / delivered | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| e5_A-5 | 2,786 | 0 | 4,249 / 2,791 | 0 | 683 | 0 (0) | 2 | 900 / 900 | 10.6 ms |
| **e5_A-6** | **4,572,542** | **4,551,938 (all 1-2 ms)** | 18,046 / 22,663,833 | 18,040,881 | **1,903,943** | 17 (2) | 13,009 | **476 / 47** | 1.74 s |
| e5_A-7 | 4,522 | 219 (2-4 ms) | 4,678 / 23,763 | 0 | 652 | 3 (1) | 473 | 900 / 900 | 10.1 ms |

  The writer's CPU went to 34.7 s in a 30 s rep and it published 476 of 900. The reader's drain overran twice in the
  rep's first 3 s and the writer marked the path as dropping fragments (MTU units only).
- **Census** (`eth_storm_census.sh`, committed `2b482592` before the run): main + `eth_storm_trace.patch` (the R4
  trace hooks plus the writer's Karn samples, timeout / RACK / shown releases and path marks), 8 instances at once,
  30 rounds of 10 reps, 17:03-20:30. Round 1's trace control PASS (80 reps: trace S and T counts equal RSTATS). Calm
  srtt: least 345 us, q01 768 us, median 5.6 ms. STORM 30 of 2,400 (1.25%, 95% upper 1.69%). They come in rounds when
  another session loaded the PC (round 15 at load 7.5: 6 storms; round 27 at 6.9: 10), several instances at the same
  moment. All 30 are reps whose reader's drain overran twice (30 of 223 such reps; none in the other 2,177). Resent
  100,888-5,736,302; delivered 97-880 of 900.
- **Two traced storms read by hand** (`ews1_r10_i4` rep 3, 377,957 resent; `ews1_r10_i1` rep 8, 241,555; load 2-3.5).
  In rep 3 at 5.81 s one ACKNACK passed ~250 once-resent units, each a Karn sample of ~2.1 ms: srtt went from 5.3 to
  ~1.5 ms in 1 us. At 5.84 s the reader asked for all 722 units of a sample its drain had lost. RACK released the
  first resends again 5-9 ms later, then the timeout (3.3 ms) did, at every ACKNACK, ~5 ms apart: the units' resend
  counts climbed 2, 3, 4 ... and no Karn sample was taken from 6.0 to 7.5 s. The reader received only duplicates,
  ~82,000 a second, which is the link. Both storms ended on their own after 1.5-2.5 s: once-resent units that had
  sat ~0.9 s in the queue were finally passed, srtt jumped to 0.86-0.96 s, and the releases stopped (tc drops ~1,000,
  no overflow; delivered 804 and 854 of 900, p99 1.04 and 0.43 s). `e5_A-6` never got such a sample (its timeout
  stayed at 1-2 ms for 4.55 M releases), overflowed the queue and lost the rep, as TVS's storms did. Under heavy load
  srtt need not fall: in `ews1_r15_i8` rep 4 (load 23, 2,740,885 resent) it stood at 3.4 ms, the frozen timeout at
  6.6 ms, and no Karn sample came for 9 s.
- **Not the cause**: srtt collapsing under a real round trip (both storms' srtt was 1.3-1.7 ms at the loss, within
  calm reps' range; TVS's was under 128 us); the reader's socket (0 drops in all three; its drain overran, which is
  the trigger, and a second overrun happened in 223 of the census's 2,400 reps, 30 of them storms); the reader's own
  timer ACKNACKs (300-600 in calm collapse reps; the 10,000+ here come after the onset).
- **Registered reading, as it came out**: VERDICT MIXED; H NOT SUPPORTED in all 30. P2 (at most 5 Karn samples in the 2
  s after onset) held in 20 of 30. P3 (the repairs' one-way time >= 10 x the timeout) failed in every readable rep: the
  ratio was 0.3-4.3, median 2.0, above 1 in 20 of 26. The releases are early by about 2x, not 10x, and that is enough:
  each ACKNACK repeats them. The onset rule (the first 100 ms with 200 timeout releases) also fires at start-up bursts
  in some reps, seconds before the storm. S1: srtt at onset CALM in 28, LOW in 2, COLLAPSED in none. The mechanism
  above is read from the traces by hand.
- **Provocation** (`eth_storm_provoke.sh`, `4f9dcccd`): the timeout pinned at ~1.5 ms stormed in 1 of 50 reps with
  natural triggers, so a short timeout alone is not enough; the trace of that one storm began at a whole-sample loss,
  with the 722-unit batch paced to 9.5 ms by the link and five stale ACKNACKs read after it.
- **Forced trigger** (`eth_storm_force.sh`, `231f10bd`, all four trees at once, 20 reps each, every 90th sample's first
  send dropped from the 60th on, so 10 whole-sample losses a rep): FM = main 0 of 20, FB = main + back-off 0 of 20
  (resends median 9,946 against 10,387), FP = pinned 1.5 ms 10 of 20 (LOOP REACHABLE), FPB = pinned + back-off 0 of 20
  (BACK-OFF BREAKS IT, one-sided p 0.0002; resends 22,648, no storm). The back-off: a unit already resent waits base
  << min(resends - 1, 6) before the timeout releases it.
- **Rate on main**: untraced, 1 in 505 reps on disk before today's census (RLF's run, load 1.75 at that rep); traced,
  30 in 2,400 (0 of 495 under load 2). The trigger is the reader's drain overrunning, which load makes common; the
  loop is in the writer. On the rig the reader does not share a CPU with 8 other benches, so its rate there is
  unmeasured.

**Proposal** (no production code here; no wire change):

1. Back off per unit: a unit resent k times waits timeout << min(k - 1, 6) before the timeout releases it again (RFC
   6298 5.5). This alone broke the forced loop. On main under the forced losses (FB against FM, not a registered
   cost test): resends +4%, median rep p50 9.59 against 9.58 ms, p99 12.14 against 12.17 ms, delivered 900 in all.
2. Fold one Karn sample per ACKNACK, not one per unit it passes (the most recent resend it shows), so one moment's
   repair time cannot rewrite srtt.
3. Take the timeout as srtt + max(G, 4 x rttvar), as the other retry timers do, not a bare 2 x srtt.
4. Later: judge an ACKNACK by when it arrived, not when a busy writer read it, or poll the socket between chunks of a
   resend batch. RACK released a whole batch on stale ACKNACKs here.

A wire change, an ACKNACK echoing the newest send serial its reader received, would give a sample for units sent
more than once and end the Karn freeze outright. It is not needed for this fix and stays parked with the other wire
changes.

Data: `~/ews_results/` (`ews1_r*_i*/`, `ews1.read.txt`, `ews1.driver.log`, `provoke/`, `provoke2/`, `force1/`,
`STATUS.txt`).

### The hybrid mode at 1% was a latched p (rig data, PC netns, `HMA`)

**TL;DR.** The IP-run mode above is not a build's and not a cost the design accepts: the writer kept an old p. The
reader re-announced p only on a factor of 2, and the n = 1 / n = 2 boundary (0.886%) is inside 1%'s band, so the
first p announced inside the band (the first tail's, or a GSO-burst dip's) held n for the stream (DESIGN.md section 8,
"Announcing p"). Fixed by re-announcing when n* moves; on the rig the IP-run mode went from 5 of 12 reps to 0 of 24 (Fisher p = 0.002).

- **Rig, re-read** (LRF, LRO, LRT, LRTR: Array1m 30 Hz RELIABLE 1%, 93 reps, 4 builds): 41 reps in runs of 2, every one
  with its reader at 0.94-1.05% (n* = 1). The switch came at the first tail (sample 2, 10 reps) or an announce tick
  (samples 29-450), never back. Delivery 100% in both modes; IP-run costs ~17% sub / ~27% pub CPU per MB (LRTR).
- **PC netns** (`hybrid_mode_netns.sh`, 1 MiB at 30 Hz, netem 1 Gbit/s, writer-egress loss; parent = main 7694bc87,
  fix = this change; 30 s reps, usable after the load rule):

| run | parent in runs (latched) | fix in runs | samples at n = 2, parent / fix | CPU sub ms/MB mean, parent / fix |
|---|---|---|---|---|
| GSO on, 1% | 5 / 14 (5) | 0 / 15 | 34.5% / 11.4% | 2.11 / 1.73 |
| GSO off (`TT_UDP_OFFLOAD=0`), 1% | 0 / 16 | 0 / 16 | 0.4% / 1.2% | 3.88 / 3.88 |
| step 0.8% for 6 s, then 1.2% (GSO off) | 8 / 12 (8) | 0 / 12 | - | - |

- Reading (registered in the harness first): GSO on SHOWN; GSO off UNDECIDED (the parent latches there only on a first
  tail of exactly 5 events in 723 frames); step SHOWN. LOSS=0 control: runs on both trees.
- **Why GSO matters**: netem drops a whole `UDP_SEGMENT` skb (~33 units), so MTU-mode loss comes in bursts and the
  estimate wanders (0.75-0.90% in the first seconds of a 1% rep) - the rig's later switches. That noise remains: the
  fix still crosses 0.886% with it (11.4% of samples at n = 2, against equal wire cost there by the objective).
- **Rig A/B** (`HMAR`, `haa_1m_20261025-002405`, Array1m 30 Hz RELIABLE KEEP_LAST 10, 1%, 12 reps an arm; pre = main
  7694bc87, head = the fix (3f0cf05e, docs 042da3bf), aa = head's own library). Reading registered in the queue script
  first. Every rep delivered 100%.

| arm | IP-run reps (`pub_gso_sends` < 20,000) | p99 ms, median | sub / pub CPU ms/MB, median | sub CPU ms/MB, mean |
|---|---|---|---|---|
| pre | 5 / 12 | 9.91 (11 usable) | 4.66 / 3.47 | 4.90 |
| head | 0 / 12 | 10.13 | 4.72 / 3.53 | 4.75 |
| aa | 0 / 12 | 10.14 | 4.68 / 3.50 | - |

- Verdict **SHOWN**: pre >= 3, head and aa none, one-sided Fisher 5/12 against 0/24 p = 0.002. Not falsified: no
  head or aa rep ran in runs.
- Cost: the median p99 is 0.22 ms (2%) higher on the fix, with |head - aa| = 0.01. It comes from the reps whose estimate
  still crosses 0.886% (the noise above: `pub_gso_sends` 24,000-37,000 in 6 of head's 12). head's six reps that stayed in
  MTU mode (> 38,000) have a median p99 of 9.91 ms, and pre's seven MTU reps 9.86 ms. The worst rep is better: pre's
  IP-run reps reach 10.23-10.88 ms and 5.2-5.5 sub CPU, head's worst is 10.42 ms and 4.85. Mean sub CPU over all reps is 3% lower.

### The runs of 45 at 1%: a forgotten p, then a full reassembly (PC netns, `HFN`)

**TL;DR.** The GSO-on reps above that sent a few samples at n = 45 forgot their first sample's events (stale rule)
and announced "none seen". Their runs then filled the reader host's IP reassembly memory, and the writer's rule (e)
went to MTU for 60 s. The drain was not where runs died. Nothing changed: each step is stated behaviour, and the fixes
change the estimator (DESIGN.md section 8, "Train losses and the stale rule", options a-e).

- **Run** (`hybrid_forget_netns.sh`, mech1, core `0f33a562`, 1 MiB 30 Hz RELIABLE, 10 s reps, writer-egress netem;
  the reader namespace's `ipfrag_high_thresh` 4 MiB, `ipfrag_time` 30 s; every rep at load < 1). Reading registered
  first. Amended once, for a reading defect only: "none seen" was looked for on the wrong stream. Same files, re-read.

| arm | usable | CHAIN (writer's rule (e)) | reassembly FULL | ReasmFails | receiver drops | p99 ms |
|---|---|---|---|---|---|---|
| auto, 1% | 16 | 4 (each: stale forget of 7-8 events, "none seen", 8-10 samples at 45) | the 4 CHAIN reps only | 676-1,116 in CHAIN, 0 in the rest | 0 | 16.3-19.2 CHAIN, 10.6-13.4 the rest |
| forced n = 45, 1% | 4 | 4 | 4 | ~209,000 | 0 | 42.7-43.2 |
| forced n = 45, 0% (control) | 3 | 0 | 0 (peak 99 KB) | 0 | 0 | 9.6-9.8 |

- Verdict **SHOWN** (re-read). The drain overran in 3 of the 4 CHAIN reps and in 4 of the 12 others; one CHAIN rep
  had none. At n = 2 memory peaks at 0.5-2.6 MB and stays below the threshold, likely because the IP ID wraps every
  ~6 s there (~11,000 runs/s) and recycles stale queues (inferred, not measured).
- Archive (`hybrid_mode_netns.sh` runs of HMA): 12 of 37 GSO-on reps had a stale forget, each with the warning and
  runs of 45; none of 44 GSO-off reps did. The sim reproduces the forget (`test_train_losses_outlast_the_stale_rule`).

### A recvmmsg batch's rows in place, PC netns (`RXIP`)

**TL;DR.** One payload copy is left on the small-sample socket path: datagrams 2..N of a `recvmmsg()` batch are copied
from the HAL's rows into the receive buffer (`rx_take_pending()`); the first, a wait's read and UDP_GRO segments already
land in place, and rmw_tickle reads one datagram a call (`tt_RX_BATCH` 1). A prototype that hands core the row cut
server user cycles 8.4% at BE p2 max rate and nothing at a paced rate, where the kernel dominates. The reading
registered first says **not worth a rig A/B**; nothing landed. The prototype is kept on `experiment/rx-rows-in-place`.

- **Run** (`rx_rows_inplace_pc.sh`, core `1a2ef53d`; UDP path, segment compiled out; perf stat on the server, per
  received sample). Arms A (main), A2 (a second build of A, the control), B (prototype); 6 interleaved runs an arm a
  cell, ~2.5-3.4 M samples a run at max rate, ~546 k paced. A 1.3 KB row copy costs 147 instructions, ~32 cycles hot.

| cell | rows share | ins:u a sample A -> B | drop predicted / measured | cyc:u B-A | A2-A | floor | verdict |
|---|---:|---|---:|---:|---:|---:|---|
| BE p1 max (76 B) | 0.83 | 949 -> 937 | 7 / 11 | +0.4% | +0.1% | 2.6% | SAME (B recv 96% of A) |
| BE p2 max (1292 B) | 0.82 | 1056 -> 937 | 120 / 119 | **-8.4%** | -1.1% | 4.5% | SAVES |
| BE p2 paced (90 k/s, 1.56 a batch) | 0.21 | 1392 -> 1371 | 31 / 21 | -2.1% | -0.5% | 2.6% | SAME |
| RELIABLE p2 max | 0.78 | 1613 -> 1497 | 114 / 115 | -3.5% | -0.3% | 0.5% | SAVES |

- Kernel ~2,500 cycles a sample against ~400 user at max rate; task-clock a sample does not move in any cell. The rule
  needed both p2 BE cells SAVES; the paced one is SAME, so the copy is real (not falsified) and too small to matter.
- Rig projection, estimated, not measured: the rig's P2 server fills 4.05 datagrams a batch (61% through a row), and a
  1.3 KB copy on a Pi is ~50-70 ns, about 1-2% of row 21's 3.5 us a sample.
- A first prototype that stopped inlining `process_datagram_locked()` cost ~15 instructions a sample and made p1 4.8%
  worse; the measured one keeps it inlined.
- Copies that remain elsewhere (for the record): out-of-order arrival into a reorder slot; DATA_FRAG into the
  scratch or pool (two copies when RELIABLE and out of order); MTU units of a large sample into the assembly; the
  io_uring drain's hand-out; rmw's take moving a decoded message.

### Shared-memory module on but unused, cross-host (rig, `S1R`)

Stage 1 / S1's CPU-time half (the PC could not resolve CPU time). One commit three ways, gate v2 A P B B P A, 6 runs an
arm, cells 1-4 (RELIABLE KEEP_ALL p1-p4); A = segment and lending compiled out, B = the default build, whose segment is
never found across two Pis (0 shm samples, attach attempts in every B run and none in A or P). CPU per sample: no
client cell WORSE (-0.2% to +1.7%); server WORSE at p4 only (+0.98% against a 0.79% floor), so a CANDIDATE, not a cost
(WIRE 8.3: 2 of 4 cells needed). Rate equal. Instructions: user +330-530 a sample on the publisher (+11-13% of its user,
~+2% of user + kernel), +70-90 on the subscriber (+2-5%), kernel unchanged - the residual the PC found (+1.7-1.9% total
at p4, `stage1_payg.sh` full5), not yet visible as CPU time.

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
| WH, KH | `rmw_holdover_rig.sh` (WH: `rmw_crosshost_creation.py`; KH: `rmw_pcap_split.py`, capture + `sysstamp`) | `430ae86d` | 2026-10-21 | `holdover_rmw{W,K}_430ae86d_20261021-185529.*` (`.creation.txt`, `.split.txt`) |
| KH2 | `rig_queue_s1_kh.sh` part KH (KH's call), `rmw_pcap_split.py`, `s1_kh_rig_read.py --kh` | `132548dd` | 2026-10-23 | `s1kh_rmwK_132548dd_20261022-230208.*`, `s1kh/132548dd_20261022-230208/kh_reading.txt` |
| KH2B | `kh2_rig_ab.sh` (KH2's call for P and B under one lock), `rmw_pcap_split.py`, `kh2_pong_calls.py`, `s1_kh_rig_read.py --kh` | `e9efea98` (P), `b72eab0c` (B; 11a838a5 before its rebase) | 2026-10-24 | `kh2ab_20261024-055604.*`, `kh2ab_P_e9efea98_20261024-055604.*` |
| S1R | `rig_queue_s1_kh.sh` part S1 (`campaign_ab_chain.sh` GATE=v2, cells 1-4), `s1_kh_rig_read.py --s1` | `132548dd` | 2026-10-22 | `s1kh/132548dd_20261022-230208/` (`s1_reading.txt`, `s1rig_*`) |
| K, Ke | `rmw_keepall_rig.sh` (Ke: equal bounds) | `ae724688`, `29dff630` | 2026-10-06, -08 | `rmw_keepall_rig_O2_20261006-123017.txt`, `rmw_keepall_equal_main_29dff630_20261008-134901.*` |
| Ctl | perf_test ping-pong | `4fca013b` | 2026-10-17 | `creation_latency_ctl_20261017-094944.*` |
| SF | `fair_samehost_remeasure.sh`, `fair_samehost_summary.py` | `e521bbba` | 2026-10-09 | `fair_samehost_e521bbba_20261009-033254.*` |
| SP | the same, `s11_publisher_cpu.py` | `228da18d` | 2026-10-17 | `fair_samehost_228da18d_20261017-144159.*` |
| SC | the same, read at `a89aefe2` (R4 of `f2127ec3`) | `2579e822` | 2026-10-18 | `creation_rtt_samehost_2579e822_20261018-092008.*` |
| WB | `wifi_burst_loss_netns.sh`, `wifi_burst_read.py` (PC netns; -O2, segment off) | `9c503bc1` | 2026-10-22 | `wifi_baseline/base_20261022/`, `wifi_baseline/large_nogso_20261023/` (`final_reading.txt`) |
| WW | the same, amended 2026-10-23 (PROFILE=ethernet and wifi, `TT_UDP_OFFLOAD=0`) | `ca540c2c` (af4ca9ab before its rebase) | 2026-10-23 | `wifi_live/eth_af4ca9ab/`, `wifi_live/wifi_af4ca9ab/` (`summary.txt`) |
| WBS | `large_storm_netns.sh`, `large_storm_ab_netns.sh`, the WB harness (offload off) | `25dd0fb0` core, and with the HEARTBEAT fix | 2026-10-23 | `large_storm/variants_20261023/`, `ab_pinned_20261023/`, `ab_pinned2_20261023/`, `r34_fix_20261023/` |
| WR | `wifi_repair_pair.sh` (the WB harness, R2 amended), `wifi_repair_ab.sh` (`CLOCK_PPS`), `initoptions_insn_pc.sh` | ab/wifi-repair (WIFI repair timing) | 2026-10-23 | `wifi_repair/final2_04be_{ethernet,wifi}/`, `final2_04be_clock_*`, `ab_clock{,2,3}.ab.txt`, `ab_seed.ab.txt`, `insn_pc_04be.txt` |
| LRF | `large_repair_followups_netns.sh` (the WB harness; routes, amp, slow; RSTATS route counters), `test_large_sample` sim cases | ab/large-repair-followups (parent: its RSTATS commit), ab/large-repair-followups-c (the (c) fix) | 2026-10-23 | `large_repair/parent_947e/`, `parent2_947e/` (load-voided), `parent3_947e/`, `ab_c_routes/` (`summary.txt`) |
| LRO | `test_large_sample` (sim, parent's tickle.c against the same test), `wire_identity_resend_order.sh`, `large_repair_followups_netns.sh` PHASE=order, `wifi_repair_ab.sh` | ab/large-resend-order (its two core commits: run 1 the first, run 2 both; P `896d4a29`) | 2026-10-24 | `large_repair/ab_order_c7e9/`, `ab_order_685a/`, `ab_order_v7/` (not landed), `wifi_repair/lro_685a.ab.txt`, `lro_685a_{P,F}/` |
| LRT | RSTATS `lro` (`tt_RELIABLE_STATS`), `test_large_sample` (the base's tickle.c against the same test), `mutants_large_sample.py`, `wire_identity_resend_order.sh`, `large_repair_followups_netns.sh` PHASE=tail, `wifi_repair_ab.sh` | ab/large-resend-order-tail (F `7c8389fd`, WIFI `9f6f5375`; P `53ed4e1f`, O `896d4a29`; ed46fcc6, 1157d024, d1c900fe before their rebase) | 2026-10-24 | `large_repair/lrt_diag1/`, `ab_tail_ed46/`, `ab_tail_ed46_reread/`, `wifi_repair/lrt_1157.ab.txt`, `lrt_1157_{P,F}/` |
| LRTR | `rmw_keepall_rig.sh` (Array1m 1%, 12 reps; aa = the tail's own library), `rig_queue_core_ab.sh` | `36700555` (main), `53ed4e1f` (base; d1c900fe before its rebase), `d6231dd4` (tail; f002166c before its rebase) | 2026-10-24 | `hmr_1m_20261024-192010.txt`, `lrt_1m_20261024-111914.txt`, `lrt_4m_20261024-111914.txt`, `lro_1m_20261024-043242.txt`, `coreAB_f002166c_20261024-094912` |
| HMAR | `rmw_keepall_rig.sh` via `~/rig_jobs/hybrid_announce_ab.sh` (Array1m 1%, 12 reps; aa = head's own library) | `7694bc87` (pre), `3f0cf05e` (fix), `042da3bf` (head) | 2026-10-25 | `haa_1m_20261025-002405.txt` |
| TVS | `storm_tv_ab.sh` (`wifi_repair_ab.sh`, read by `storm_tv_read.py`), `storm_trace.patch` + `storm_trace_scan.py` / `storm_trace_read.py` / `storm_sub_trace.py`, `large_repair_followups_netns.sh` PHASE=tail, `test_large_sample`, `wire_identity_resend_order.sh` | storm-fix: M/A `849fdccf`, F `8d0d2eed`, K/FK + 11908dea | 2026-10-25 | `~/storm_results/`: `ab1_tv.storm.txt`, `ab1_arms.storm.txt`, `ab1_{tv,arms}_*/`, `repro_trace_T{1,2,3}/`, `trace2_*/`, `tail1/`; r4_storm in `~/r4_results/` |
| EWS | `eth_storm_census.sh` + `eth_storm_trace.patch` + `eth_storm_read.py`, `eth_storm_provoke.sh` / `.py`, `eth_storm_force.sh` / `.py` (all over `wifi_burst_loss_netns.sh`) | census `2b482592` (main core + trace), provoke `4f9dcccd`, force `231f10bd` | 2026-10-26 | `~/ews_results/`: `ews1_r*_i*/`, `ews1.read.txt`, `provoke/`, `provoke2/`, `force1/`; the raising rep `~/rlf_results/rlf1_e5_A/runs/ge0.1b1-rel_large-6/` |
| TVS rig | `rig_queue_core_ab.sh` (gate v2), `rmw_keepall_rig.sh` + `largemsg_rig_summary.py` via `~/rig_jobs/storm_fix_queue.sh` | A `b434f649`, B `5b7c09e7` | 2026-10-26 | `~/rig_results_safe/core_ab/coreAB_5b7c09e7_20261025-234811/`, `tvs_1m_20261026-011959.*`, `tvs_4m_20261026-011959.*` |
| HFN | `hybrid_forget_netns.sh` (PC netns; reader reassembly sampled; REREAD after its amendment) | `0f33a562` | 2026-10-25 | `hyb_forget/mech1/` (`results.txt`, `reread.txt`) |
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
