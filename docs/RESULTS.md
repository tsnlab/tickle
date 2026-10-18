# TickLE performance results

**TL;DR.** TickLE against FastDDS and CycloneDDS on two Raspberry Pi 5s; zenoh is reference only and never scored.
- Core, cross-host: WIN 139, DRAW 11, LOSE 0 of 150. Core, same host: WIN 18 of 18.
- ROS 2 (rmw), cross-host: WIN 22, DRAW 6, LOSE 0 of 28. Same host: WIN 52 of 52.
- Large messages (L4): WIN 55, DRAW 23, LOSE 2 of 80. Both LOSEs are CPU per MB against FastDDS at 4 MB, 15 Hz.
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

**rmw, cross-host** ([rmw layer](#rmw-layer)): WIN 22, DRAW 6, LOSE 0 of 28.

| headline | rmw_tickle | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| RTT mean from creation, block wait, Bench 64 B, ms | **0.302** | 0.389 | 0.336 | 48c |
| delivered msg/s, RELIABLE KEEP_ALL, Array1k, 5% loss | **74,129** | does not run | 1,808 | 73 |

**rmw, same host** ([rmw layer, same host](#rmw-layer-same-host)): WIN 52, DRAW 0, LOSE 0 of 52.

| headline | rmw_tickle | FastDDS | CycloneDDS | row |
|---|---:|---:|---:|---|
| RTT mean from creation, block wait, Bench 64 B, us | **94.9** | 184.1 | 166.3 | R1c |

**rmw, large messages** ([L4](#rmw-large-messages-l4-cross-host)): WIN 55, DRAW 23, LOSE 2 of 80 (one letter per vendor).

| headline | rmw_tickle | FastDDS / FastDDS 1472 | CycloneDDS | cell |
|---|---:|---:|---:|---|
| delivered, Array1m RELIABLE 30 Hz, 5% loss | **100%** | 0% / 100% | 2.4-4.0% | I1L |
| latency median from creation, Array4m 30 Hz (saturating), ms | **55.8** | 2,026 / 7,599 | 1,900 | I4s |

### Every LOSE

- **L4 I4 best effort (Array4m 15 Hz), subscriber CPU per MB against FastDDS as shipped**, 2.19 against 2.10 ms/MB:
  the kernel's copy into a cold 4 MB buffer (`i4be_cpu_gap.sh`).
- **The same cell, publisher CPU per MB**, 3.72 against 3.62: interrupt billing. eth0's interrupt is on CPU 0 and the
  Pi kernel has no `IRQ_TIME_ACCOUNTING`; with every process pinned off CPU 0 the gap is 0.004 ms per sample.

### Every DRAW

- **Row 5c, +10 ms netem:** 10 ms of netem swamps a 0.2 ms RTT (counted as one DRAW in `A`).
- **Loss TIEs** (10 in `A`, 2 in `T`): TickLE lost nothing, and each vendor lost nothing or had a range touching zero.
- **rmw rows 63c-67c against CycloneDDS:** at 100-200 us poll sleeps the half poll cycle dominates every rmw.
- **rmw row 74, Array4k KEEP_ALL at 0% loss:** all three sit at the link ceiling.
- **L4, 23 letters:** delivered % where everyone delivered 100% (13); against FastDDS as shipped, latency at I1 and I1
  best effort (2) and CPU per MB at I1, I1 best effort, I4 RELIABLE and I4s (8).

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
| S1 | BEST_EFFORT KEEP_LAST 1, max rate, P2 1292 B | **20,867** | 0.31 | 934 | SF |
| S2 | BEST_EFFORT KEEP_LAST 1, max rate, P3 1424 B | **21,982** | 0.32 | 1,029 | SF |
| S3 | BEST_EFFORT KEEP_LAST 1, max rate, P4 2800 B | **25,066** | 0.70 | 1,854 | SF |
| S4 | RELIABLE, P2 | **13,764** | 1,012 | ◊ (2,165) | SF |
| S5 | RELIABLE, P3 | **14,771** | 980 | ◊ (2,414) | SF |
| S6 | RELIABLE, P4 | **14,917** | 1,924 | ◊ (4,542) | SF |
| | **RTT mean from creation, ms** (one ping in flight, 200/s unless noted) | | | | |
| S7c | P2 | **0.036** | 0.160 | 0.125 | SC |
| S8c | P2, 20/s | **0.034** | 0.172 | 0.142 | SC |
| S9c | P3 | **0.037** | 0.164 | 0.130 | SC |
| S10c | P4 | **0.086** | 0.162 | 0.130 | SC0 |
| | **CPU, us** | | | | |
| S11 | publisher per delivered sample, BEST_EFFORT P4 max rate | **0.898** | 35,234 | 11.85 ‡ | SP |
| S12 | client per round trip, P2 20/s | **22.4** | 83.1 | 51.6 | SC |
| S17 | client + server per delivered sample, RELIABLE P3 | **1.53** | 31.65 | ◊ (12.04) | SF |
| S18 | client + server per round trip, P3 200/s | **35.5** | 167.0 | 95.6 ‡ | SC |
| | **Memory, MB** | | | | |
| S13 | publisher peak RSS, BEST_EFFORT P4 | **3.2** | 15.9 | – | SE |
| S19 | client + server peak RSS, RELIABLE P3 | **8.5** | 46.9 | ◊ (239.2 ‡) | SF |
| | **Mixed: one publisher, a subscriber on its host and one across the link, k samples/s** | | | | |
| S15 | BEST_EFFORT, local / remote | **90.7 / 90.7**, loss 0 | 53.1 / 54.6, loss 4-22% | 49.7 / 70.6, loss 20-33% / 0.1% | MX |
| S16 | RELIABLE, local / remote | **90.7 / 90.7** | 30.8 / 30.8 | 71.7 / 71.7 | MX |
| S14 | wire bytes per sample, every cell (not scored) | 0 | 0 | 0 | |

### Same-host notes

- **Corrected 2026-10-18:** S7c-S9c, S12 and S18 replace the `596e9c9c` figures (S7c-S9c 0.082 ms): the polling
  thread's 1 ns timer slack (`119d14bd`) cut TickLE's ping delay from ~60 us (as the DDS clients') to 15 us.
- **S10c is NOT READ at `2579e822`** (0.041 / 0.162 / ◊ ms): TickLE's R4 control missed by 0.1 us (`|d - g|` 1.1
  against 1 us; all reps printed the same p50s). The row keeps its `596e9c9c` reading.
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
| R1c | block wait, Bench 64 B, BEST_EFFORT | **94.9** | 184.1 | 166.3 | RSc |
| R2c | block wait, Bench 64 B, RELIABLE | **95.8** | 205.9 | 168.9 | RSc |
| R3c | block wait, Array1k, BEST_EFFORT | **96.9** | 189.0 | 178.7 | RSc |
| R4c | block wait, Array1k, RELIABLE | **99.2** | 214.3 | 174.1 | RSc |
| R5c | poll wait 100 us, Bench, BEST_EFFORT | **207.4** | 247.5 | 239.6 | RSc |
| R6c | poll wait 100 us, Bench, RELIABLE | **208.2** | 262.9 | 245.0 | RSc |
| R7c | poll wait 100 us, Array1k, BEST_EFFORT | **209.4** | 251.3 | 240.5 | RSc |
| R8c | poll wait 100 us, Array1k, RELIABLE | **208.8** | 262.7 | 244.6 | RSc |
| | **CPU, ping + pong, us per round trip** | | | | |
| R9 | block, Bench RELIABLE | **63.5** | 247.9 | 151.0 | RSc |
| R10 | block, Array1k RELIABLE | **67.6** | 259.6 | 157.2 | RSc |
| | **Peak RSS, larger process, kB** | | | | |
| R11 | RTT, Bench BEST_EFFORT block | **14,592** | 27,124 | 16,100 | RSc |
| R12 | RTT, Bench RELIABLE block | **14,676** | 27,148 | 16,172 | RSc |
| R13 | throughput, Array1k RELIABLE | **16,000** | 35,796 | 17,152 | RS |
| | **Delivered msg/s, max rate** | | | | |
| R14 | Array1k BEST_EFFORT KEEP_LAST 1 | **140,885** | 17,547 | 70,420 | RS |
| R15 | Array4k BEST_EFFORT KEEP_LAST 1 | **83,048** | 4,171 | 60,818 | RS |
| R16 | Array1k RELIABLE KEEP_ALL | **187,965** | 2,322 | 64,522 | RS |
| R17 | Array4k RELIABLE KEEP_ALL | **155,635** | 17,507 | 49,904 | RS |
| | **CPU, pub + sub, us per delivered sample** | | | | |
| R18 | Array1k BEST_EFFORT | **14.02** | 152.93 | 33.80 | RS |
| R19 | Array4k BEST_EFFORT | **23.83** | 636.13 | 36.79 | RS |
| R20 | Array1k RELIABLE | **6.35** | 639.34 | 35.12 | RS |
| R21 | Array4k RELIABLE | **8.64** | 83.84 | 38.72 | RS |

- **The 31 cells not shown** (RTT p50 and p99, CPU and RSS of the other RTT cells, three RSS cells) are WIN too.
- **Re-run on `d0650110`** (`RSd`) agrees: every RTT-cell metric a WIN, all 8 controls PASS.
- **Corrected 2026-10-18:** R9-R12 replaced the `06dd78d8` figures; CycloneDDS rose alike (the session).

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
| 7 | RELIABLE, P1 | **107** | 28.1 | 52.2 | WIN | A |
| 8 | RELIABLE, P2 | **937** | 591 | 864 | WIN | A |
| 9 | RELIABLE, P3 | **943** | 401 | 716 | WIN | T |
| 10 | RELIABLE, P4 | **943** | 519 | 891 | WIN | T |
| 11 | BEST_EFFORT, P1, max rate | **115** | 28.8 | 62.1 | WIN | A |
| 11a | BEST_EFFORT, P2 | **938** | 575 | 906 | WIN | A |
| 11b | BEST_EFFORT, P3 | **943** ‡ | 560 | 654 | WIN | A |
| 11c | BEST_EFFORT, P4 | **944** ‡ | 902 | 937 | WIN | A |
| 12 | RELIABLE KEEP_LAST 64, P1 | **117** | 38.0 | 72.6 | WIN | A |
| 13 | RELIABLE, P1, 5% loss | **93.0** | 12.1 | 2.49 | WIN | A |
| 14 | RELIABLE, P4, 5% loss | **865** | 216 | 14.3 | WIN | T |
| 15 | RELIABLE, P1, 5% reorder | **95.2** | 28.9 | 60.7 | WIN | A |
| 16 | kept at 5% loss / unshaped, P1 | **86.9%** | 43.1% | 4.8% | WIN | A |
| 17 | kept at 5% loss / unshaped, P4 | **91.6%** | 38.9% | 1.6% | WIN | T |
| | **CPU, cpu_s per Msample** | | | | | |
| 18 | throughput client, P1 | **5.7** | 24.3 | 7.8 | WIN | A |
| 19 | throughput server, P1 | **2.7** | 27.0 | 12.9 | WIN | A |
| 20 | throughput client, P2 | **6.7** | 20.8 | 9.3 | WIN | A |
| 21 | throughput server, P2 | **3.8** | 21.0 | 14.6 | WIN | A |
| 22 | throughput client, P4 | **12.6** | 46.7 | 14.3 | WIN | T |
| 23 | throughput server, P4 | **7.6** | 35.7 | 20.1 | WIN | T |
| 24 | latency, P1 | **34.3** | 102 | 52.9 | WIN | A |
| 25 | latency, P2 | **35.0** | 103 | 54.9 | WIN | A |
| 26 | throughput client, P1, 5% loss | **6.6** | 28.1 | 13.2 | WIN | A |
| 27 | throughput client, P4, 5% loss | **15.2** | 66.9 | 40.2 | WIN | T |
| | **Peak RSS, KB** | | | | | |
| 28 | throughput client, P1 | **2,087** | 15,947 | 5,407 | WIN | A |
| 29 | throughput client, P2 | **2,371** | 15,100 | 6,373 | WIN | A |
| 30 | throughput client, P4 | **2,425** | 14,013 | 5,540 | WIN | T |
| 31 | latency client, P1 | **1,813** | 14,379 | 4,916 | WIN | A |
| 32 | throughput server, P1 | **2,057** | 14,756 | 6,991 | WIN | A |
| 33 | throughput server, P4 | **1,987** | 14,264 | 4,948 | WIN | T |
| 34 | throughput server, P4, 5% loss | **2,968** | 14,351 | 6,011 | WIN | T |
| | **Wire bytes per sample** | | | | | |
| 35 | P1 | **138** | 312 | 183 | WIN | A |
| 36 | P2 | **1,355** | 1,503 | 1,397 | WIN | A |
| 37 | P3 | **1,487** | 1,866 | 1,585 | WIN | T |
| 38 | P4 | **2,920** | 3,461 | 2,950 | WIN | T |
| 39 | P1, 5% loss | **143** | 292 | 193 | WIN | A |
| 40 | P4, 5% loss | **3,057** | 4,223 | 3,213 | WIN | T |
| 41 | framing, one datagram (row 35 - 76 B) | **62.0** | 236.0 | 107.0 | WIN | A |
| | **QoS mechanics** | | | | | |
| 42 | DURABILITY late join, durable / volatile | 20/20, 0/20 | 20/20 | 20/20 | ⚪ | S |
| 43 | HISTORY within depth 8, burst | 160/160 | 160/160 | 160/160 | ⚪ | S |
| 44 | HISTORY beyond depth 8, burst | **147.7/160** | 109/160 | 109/160 | WIN | S |
| 45 | DEADLINE 50 ms detection | works | works | works | ⚪ | S |
| 46 | LIVELINESS lease 2.0 s, detection | 2000.1 ms | 1999.1 ms | 2000.1 ms | ⚪ | L |
| 47 | LIFESPAN 100 ms expiry | works | works | works | ⚪ | S |

Rows 48c-75 are in [rmw layer](#rmw-layer). Send-stamp means (rows 1, 3, 5, 6a, 6b, 48-67) are diagnostics, and
zenoh-pico and rmw_zenoh are reference; both are in the older text.

- **Corrected 2026-10-17:** rows 11-12 were send rates; now goodput (FastDDS row 11 44.7 -> 28.8 Mbps). TickLE lost 0.
- **Corrected 2026-10-17:** 1c-6bc replace a `4fca013b` run where TickLE lost 1c, 3c and 6bc to CycloneDDS (its ping
  left 111 us late; the `timerfd` deadline wait of `596e9c9c` made it 11 us).

### Latency from creation time (2026-10-17)

R4 control: where nothing waits at the sender, creation and stamp differ only by the sender's timer wake `g`. It
passed in every cell. `C`: 100 round trips per rep; TickLE's `g` 11-12 us, the DDS clients' 58-61 us. rmw ping-pong
control (`Ctl`, not scored): RTT median 0.32 / 0.42 / 0.35 ms. Not yet from creation time: tails 2, 4, 6, Array1m L4.

### rmw layer

`rclcpp` with one ping/pong pair; `RMW_IMPLEMENTATION` alone picks the rmw (checked in `/proc/PID/maps`). Block wait
is `spin()`; poll wait loops on `spin_some()` with a sleep, at a random phase, scored by interquartile overlap.

RTT mean from creation, ms, rmw_tickle / FastDDS / CycloneDDS; WIN unless marked DRAW (against CycloneDDS):

| wait | Bench 64 B BEST_EFFORT | Bench RELIABLE | Array1k BEST_EFFORT | Array1k RELIABLE | src |
|---|---|---|---|---|---|
| block | 48c **0.302** / 0.389 / 0.336 | 49c **0.305** / 0.396 / 0.334 | 50c **0.322** / 0.406 / 0.354 | 51c **0.326** / 0.423 / 0.342 | Wc |
| busy poll | 52c **0.301** / 0.383 / 0.331 | 53c **0.304** / 0.394 / 0.331 | 54c **0.320** / 0.404 / 0.350 | 55c **0.323** / 0.414 / 0.342 | Jc |
| poll 50 us | 56c **0.352** / 0.416 / 0.371 | 57c **0.354** / 0.417 / 0.365 | 58c **0.372** / 0.437 / 0.390 | 59c **0.377** / 0.446 / 0.380 | Jc |
| poll 100 us | 60c **0.377** / 0.433 / 0.393 | 61c **0.383** / 0.445 / 0.391 | 62c **0.399** / 0.454 / 0.412 | 63c 0.402 / 0.467 / 0.400 DRAW | Jc |
| poll 200 us | 64c 0.426 / 0.468 / 0.426 DRAW | 65c 0.431 / 0.486 / 0.428 DRAW | 66c 0.450 / 0.499 / 0.457 DRAW | 67c 0.454 / 0.509 / 0.443 DRAW | Jc |

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
- **VOID(loss):** 14 of 496 reps (rmw_tickle 10, CycloneDDS 4). rmw_tickle's 10 were its reply to seq 1, sent before
  its publisher matched (`rmw_loss_attribution.py`); `ping_node` now warms up first. In the re-run with that warm-up
  (`d0650110`, `Wd`/`Jd`) `rmw_loss_attribution.py` reports "no lossy rep" on `J` and `W`.
- **The `d0650110` re-run does not agree everywhere** (`J` 3 reps, not 7; 20 cells CONTROL PASS): it reads 63c a
  LOSE against CycloneDDS (0.403 against 0.390 ms, on the stamp too) and 59c, 61c DRAWs. The rows stay `18f16b85`.
- **Corrected 2026-10-18:** 48c-67c replace the send-stamp rows 48-67 (`934f90de`); 59 and 62 went DRAW -> WIN with
  the session, not the definition.

### rmw large messages (L4, cross-host)

apex `perf_test` across the link, 5 reps of 60 s. Letters: FastDDS as shipped / FastDDS 1472 / CycloneDDS (– not scored).

| cell | QoS, rate | delivered % | latency med / p99 ms | CPU sub / pub ms/MB | RSS kB | delivered | latency | CPU sub | CPU pub | RSS | src |
|---|---|---:|---:|---:|---:|:-:|:-:|:-:|:-:|:-:|---|
| I1 Array1m | RELIABLE KL10, 30 Hz | 100 | 9.41 / 9.48 ¹ | 2.29 / 3.94 | 18,684 | D–D–D | D–W–W | D–W–W | D–W–W | W–W–W | L16 |
| I1L Array1m, 5% loss | RELIABLE KL10, 30 Hz | 100 | 10.04 / 15.75 ¹ | 4.17 / 3.65 | 18,792 | –D– | –W– | –W– | –W– | –W– | L16 |
| I1 Array1m | BEST_EFFORT KL5, 30 Hz | 100 | 9.24 / 9.31 ¹ | 2.20 / 3.89 | 17,724 | D–D–D | D–W–W | D–W–W | D–W–W | W–W–W | L15 |
| I4s Array4m | BEST_EFFORT KL5, 30 Hz | 95.5 of offered | **55.83** / UNBOUND | 1.40 / 3.60 | 30,952 | W–W–W | W–W–W | D–W–W | D–W–W | W–W–W | L15, LC |
| I4 Array4m | RELIABLE KL10, 15 Hz | 100 | **36.68** / 36.77 | 2.21 / 3.69 | 30,656 | D–D–D | W–W–W | D–W–W | D–W–W | W–W–W | L15, LC |
| I4 Array4m | BEST_EFFORT KL5, 15 Hz | 100 | **36.60** / 36.66 | 2.19 / 3.72 | 30,580 | D–D–D | W–W–W | L–W–W | L–W–W | W–W–W | L15, LC |

¹ Send stamp; not yet measured from creation time.

- **I4s note.** I4s offers more than the link carries. A DDS `publish()` of 4 MB blocks, so its publisher falls
  behind every tick; the send stamp hides that wait (I4s LOSE on the stamp, 54.91 against 37.55 ms) and creation time
  counts it. rmw_tickle does not block and drops at the sender (~84 a run). Delivered % is per offered tick; the p99
  is UNBOUND for every arm and not scored.
- **Corrected 2026-10-16:** I1L was a LOSE on incomplete delivery (f5fcda06, 2 samples in 1 run); first-n3 fixed it.
- **Corrected 2026-10-17:** publisher CPU per MB delivered; I4s CPU pub W–W–W -> D–W–W. Same host: not published.

### Hybrid transport (large samples), rig

AUTO against TickLE's own forced MTU mode, not counted (`HY`, Array1m RELIABLE): AUTO delivered 100% at 0.01-1% loss;
wire bytes WIN at 0.01% and 0.1%, LOSE at 1% (+0.46%); p99 LOSE at 0.01% and 0.1% (9.54 against 9.42 ms), DRAW at 1%.

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
| W | rmw block wait | `934f90de` | 2026-09-30 | `results/rmw_4way_block_934f90de_2026-09-30.txt` |
| Wc, Jc | `rmw_crosshost_rtt.sh`, `rmw_crosshost_creation.py` | `18f16b85` | 2026-10-18 | `creation_rtt_rmw{W,J}_18f16b85_20261018-001954.*` |
| Wd, Jd | the same, ping warm-up | `d0650110` | 2026-10-18 | `creation_rtt_rmw{W,J}_d0650110_20261018-062244.*` |
| K, Ke | `rmw_keepall_rig.sh` (Ke: equal bounds) | `ae724688`, `29dff630` | 2026-10-06, -08 | `rmw_keepall_rig_O2_20261006-123017.txt`, `rmw_keepall_equal_main_29dff630_20261008-134901.*` |
| Ctl | perf_test ping-pong | `4fca013b` | 2026-10-17 | `creation_latency_ctl_20261017-094944.*` |
| SF | `fair_samehost_remeasure.sh`, `fair_samehost_summary.py` | `e521bbba` | 2026-10-09 | `fair_samehost_e521bbba_20261009-033254.*` |
| SP | the same, `s11_publisher_cpu.py` | `228da18d` | 2026-10-17 | `fair_samehost_228da18d_20261017-144159.*` |
| SC | the same, read at `a89aefe2` (R4 of `f2127ec3`) | `2579e822` | 2026-10-18 | `creation_rtt_samehost_2579e822_20261018-092008.*` |
| SC0 | the same | `596e9c9c` | 2026-10-17 | `creation_rtt_samehost_596e9c9c_20261017-225621.*` |
| SE | `s6_transport_cells.sh` | `e17b4e6f` | 2026-10-04 | `p4_shm_headtohead_e17b4e6f.txt` |
| MX | `mixed_delivery.sh`; CycloneDDS `mixed_delivery_campaign.sh` (`e4e090cd`) | `7f127d56` | 2026-10-05, -17 | `mixed_4e49ed43_20261017-152135.*` |
| RS, RSc, RSd | `rmw_samehost.sh`, `rmw_samehost_summary.py` | `06dd78d8`, `18f16b85`, `d0650110` | 2026-10-09, -18, -18 | `rmw_samehost_06dd78d8_20261009-110918.*`, `creation_rtt_rmwsh_{18f16b85_20261018-001954,d0650110_20261018-062244}.*` |
| LN | `rmw_loan_bench.sh` (PC) | `ab/lending` + `f837ae82` | 2026-10-08 | `~/rmw_loans_ws/bench3/` |
| L15, L16 | `rmw_keepall_rig.sh`, `largemsg_rig_summary.py` | f5fcda06, dde87857 | 2026-10-15, -16 | `l4main2_*_20261015-094605.txt`, `l4main3_i1rel_20261016-065517.*` |
| LC | the same, `CREATION_TRACE=1` | `4fca013b` | 2026-10-17 | `creation_latency_{i4s,i4be,i4rel}_20261017-094944.*` |
| HY | hybrid arms | d8980f9b | 2026-10-12 | `hybridLand_s2_loss_20261012-021743.*` |
| NS | `nosend_benefit_rig.sh` | b265e86f vs branch | 2026-10-16 | `nosend_benefit/20261016-020518/reading.txt` |

**Versions:** CycloneDDS 11.0.1 (iceoryx 2.0.5) and Fast DDS 2.14.6 native; jazzy's rmws (CycloneDDS 0.10.5, Fast DDS
2.14.6). Vendors are distribution packages (`-O2`); TickLE `-O2 -DNDEBUG`. Open work: [ROADMAP.md](ROADMAP.md).
