# TickLE performance results

TickLE against FastDDS and CycloneDDS, measured on dedicated hardware. zenoh-pico and rmw_zenoh are shown for
reference. Column order is always **TickLE, FastDDS, CycloneDDS**. Design background is in [DESIGN.md](DESIGN.md),
the ROS 2 layer in [RMW.md](RMW.md), how the numbers are taken in [TESTING.md](TESTING.md), open work in
[ROADMAP.md](ROADMAP.md). Result file paths below are relative to `examples/perf_hil/` unless they start with `~`.

## Summary

- **Cross-host (two Raspberry Pi 5s over 1 GbE):** TickLE is best in every scored row of latency, throughput, CPU,
  memory and bandwidth at P1-P4, with and without injected loss, under identical explicit QoS, against FastDDS both
  as shipped and tuned. The aligned campaign scored **WIN 97, DRAW/TIE 11, LOSE 0, VOID 0** (`A`) and **WIN 27, TIE 3,
  LOSE 0** (`T`).
- **The largest margin is RELIABLE under 5% loss:** TickLE keeps 92.7% (P1) and 90.5% (P4) of its throughput;
  FastDDS 16.1% / 33.7%, CycloneDDS 3.3% / 1.4% (rows 16-17).
- **Same host (shared memory):** TickLE leads every measured cell, but S1-S3 compare *send* rates (see notes).
- **rmw layer:** rmw_tickle is first on every block-wait row and every poll-wait row except seven draws with
  CycloneDDS (rows 59, 62-67). It loses no row.
- **Not scored:** row 5 (netem dominates), rows 45-47 (all detect correctly), zenoh-pico (reference only).

## 1. Same host (shared memory)

Publisher and subscriber on **one** rig Pi, each framework on its own shared-memory path: TickLE's segment,
**FastDDS as shipped** (data-sharing, on by default) and **CycloneDDS with iceoryx**. Each arm's transport is checked
by a witness that cannot be configured into agreeing (loopback packet count, and TickLE's own `tx_shm` share).
Harness `experiments/s6_transport_cells.sh`, 3 repetitions per framework unless noted, throughput from drop-free
repetitions only. **✅ = best, ❌ = worst** in the row.

| # | Metric | Condition | TickLE | FastDDS | CycloneDDS | build | details |
|---|---|---|---|---|---|---|---|
| | **Throughput, send Mbps** (higher is better) | | | | | | |
| S1 | BEST_EFFORT, max rate | P2 1292 B | ✅ **12,916** | 6,445 | ❌ 4,531 | `313cda37` | [notes](#same-host-notes) |
| S2 | BEST_EFFORT, max rate | P3 1424 B | ✅ **13,771** | 6,922 | ❌ 4,924 | `313cda37` | [notes](#same-host-notes) |
| S3 | BEST_EFFORT, max rate | P4 2800 B | ✅ **20,448** | 13,445 | ❌ 9,389 | `313cda37` | [notes](#same-host-notes) |
| S4 | RELIABLE | P2 1292 B | ✅ **9,774** | ❌ 953 | 2,621 | `8d1c3712` | [notes](#same-host-notes) |
| S5 | RELIABLE | P3 1424 B | ✅ **10,359** | ❌ 1,028 | 2,778 | `8d1c3712` | [notes](#same-host-notes) |
| S6 | RELIABLE | P4 2800 B | ✅ **12,289** | ❌ 2,004 | 5,240 | `fe45f276` | [notes](#same-host-notes) |
| | **Latency, RTT mean ms** (lower is better; one ping in flight, 200/s unless noted) | | | | | | |
| S7 | RTT | P2 1292 B | ✅ **0.030** | ❌ 0.158 | 0.063 | `e17b4e6f` | [notes](#same-host-notes) |
| S8 | RTT, 20/s (60 s) | P2 1292 B | ✅ **0.032** | ❌ 0.197 | 0.071 | `e17b4e6f` | [notes](#same-host-notes) |
| S9 | RTT | P3 1424 B | ✅ **0.030** | ❌ 0.098 | 0.061 | `5689da96` | [notes](#same-host-notes) |
| S10 | RTT | P4 2800 B | ✅ **0.051** | ❌ 0.099 | 0.064 | `5689da96` | [notes](#same-host-notes) |
| | **CPU** (lower is better) | | | | | | |
| S11 | publisher, us per sample | BEST_EFFORT P4, max rate | ✅ **1.071** | ❌ 1.694 | – | `e17b4e6f` | [notes](#same-host-notes) |
| S12 | client, us per round trip | RTT P2, 20/s | ✅ **66.0** | ❌ 136.7 | 102.1 | `e17b4e6f` | [notes](#same-host-notes) |
| | **Memory** (lower is better) | | | | | | |
| S13 | publisher peak RSS, MB | BEST_EFFORT P4 | ✅ **3.2** | ❌ 15.9 | – | `e17b4e6f` | [notes](#same-host-notes) |
| | **Mixed: one publisher, one subscriber on its host and one across the link** (delivered k samples/s, p2) | | | | | | |
| S15 | BEST_EFFORT, each subscriber | local / remote | ✅ **90.7 / 90.7**, loss 0 | 53.1 / 54.6, loss 4-22% | – | `7f127d56` | [notes](#same-host-notes) |
| S16 | RELIABLE, each subscriber | local / remote | ✅ **90.7 / 90.7** | 30.8 / 30.8 | – | `7f127d56` | [notes](#same-host-notes) |
| | **Bandwidth** | | | | | | |
| S14 | wire bytes per sample | every cell above | 0 | 0 | 0 | | nothing leaves the host |

### Same-host notes

- **S1-S3 compare send rates, not delivery.** FastDDS's bench subscriber uses the default reader QoS (KEEP_LAST 1);
  on one host its writer overwrites what the reader has not taken, and in `mixed_delivery.sh`'s FastDDS LOCAL_ONLY
  arm (p2 BEST_EFFORT, 3 reps) the subscriber took **154 of 3.1 million** samples sent. TickLE's figures are
  drop-free reps, so for it sent equals delivered. Delivered vendor rates are on the roadmap. A TickLE BEST_EFFORT rep
  that drops at a full ring reads *higher* (dropping is cheaper than delivering), which is why such reps are excluded.
- **S6 (RELIABLE p4)** was VOID on `8d1c3712`: a lost-wakeup defect (the writer did not ring a reader that had gone
  back to sleep) made the application give up 4-7 samples per rep. Fixed in `fe45f276` (the writer now rings each
  sleep of the reader once) and re-measured there with every rep `write_fail=0`, `lost=0`. S6's CycloneDDS figure is
  from that same run; an earlier run gave 5,776. S4-S5 were measured after ACK solicitation stopped being clocked by
  a 1 ms timer (`8d1c3712`); before that, TickLE lost S4 and S6 to CycloneDDS.
- **S11 and S13 compare against FastDDS only.** CycloneDDS's memory excludes iceoryx's `iox-roudi` daemon, which
  reserved 216 MB of shared memory before any application connected. TickLE's segment is in-process, no daemon.
- **The rows come from four builds** of one night, each measured after the change that moved it.
- **S10 is the narrowest lead (1.26x CycloneDDS):** a p4 sample is two datagrams and two slots on TickLE's path.
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

## 2. Cross-host (two Pis over the rig's link)

**✅ = best, ❌ = worst** in the row. Lower is better for latency, CPU, memory and bandwidth; higher for throughput.

| # | Metric | Condition | TickLE | FastDDS | CycloneDDS | zenoh-pico | rmw_zenoh | meas. |
|---|---|---|---|---|---|---|---|---|
| | **[Latency](#latency)** (ms) | | | | |  | – | |
| 1 | RTT mean | P1 76 B | ✅ **0.207** | ❌ 0.287 | 0.266 | 0.221 † | – | A |
| 2 | RTT max (tail) | P1 76 B | ✅ **0.270** | 0.604 | ❌ 0.720 | 0.336 † | – | A |
| 3 | RTT mean | P2 1292 B | ✅ **0.233** | 0.316 | ❌ 0.376 | 0.246 † | – | A |
| 4 | RTT max (tail) | P2 1292 B | ✅ **0.349** | 0.634 | ❌ 10.8 | 0.372 † | – | A |
| 5 | RTT mean | +10 ms netem | ⚪ 10.1 | ⚪ 10.4 | ⚪ 10.5 | 10.07 | – | A |
| 6 | RTT max (tail) | +10 ms netem | ✅ **12.2** | 12.3 | ❌ 23.1 | 12.15 | – | A |
| 6a | RTT mean | P3 1424 B | ✅ **0.239** | ❌ 0.347 | 0.308 | 0.248 † | – | G |
| 6b | RTT mean | P4 2800 B | ✅ **0.281** | ❌ 0.364 | 0.339 | 0.402 † | – | G |
| | **[Throughput](#throughput)** (Mbps) | | | | |  | – | |
| 7 | RELIABLE | P1 76 B | ✅ **115** | ❌ 40.8 | 93.0 | 236.8 † | – | A |
| 8 | RELIABLE | P2 1292 B | ✅ **937** | ❌ 647 | 851 | ✗ † | – | A |
| 9 | RELIABLE | P3 1424 B | ✅ **943** | ❌ 439 | 808 | ✗ † | – | T |
| 10 | RELIABLE | P4 2800 B | ✅ **943** | ❌ 635 | 863 | ✗ † | – | T |
| 11 | BEST_EFFORT | P1, max rate | ✅ **119** | ❌ 47.9 | 71.7 | 53.4 | – | A |
| 11a | BEST_EFFORT | P2 1292 B, max rate | ✅ **938** | ❌ 756 | 914 | 867 | – | G |
| 11b | BEST_EFFORT | P3 1424 B, max rate | ✅ **943** ‡ | ❌ 608 | 820 | ✅ **941** ‡ | – | G |
| 11c | BEST_EFFORT | P4 2800 B, max rate | ✅ **944** ‡ | ❌ 915 | 938 | ✅ **945** ‡ | – | G |
| 12 | RELIABLE KEEP_LAST 64 | P1 | ✅ **117** | ❌ 41.9 | 89.3 | – | – | A |
| 13 | RELIABLE | P1, **5% loss** | ✅ **106** | 6.6 | ❌ 3.1 | ✗ † | – | A |
| 14 | RELIABLE | P4, **5% loss** | ✅ **854** | 214 | ❌ 11.8 | ✗ † | – | T |
| 15 | RELIABLE | P1, 5% reorder | ✅ **104** | ❌ 32.2 | 67.4 | ✗ † | – | A |
| 16 | retention under loss | P1: 5% loss / unshaped | ✅ **92.7%** | 16.1% | ❌ 3.3% | ✗ † | – | A |
| 17 | retention under loss | P4: 5% loss / unshaped | ✅ **90.5%** | 33.7% | ❌ 1.4% | ✗ † | – | T |
| | **[CPU](#cpu)** (cpu_s / Msample) | | | | |  | – | |
| 18 | throughput, client | P1 76 B | ✅ **5.3** | ❌ 19.0 | 6.9 | 2.57 † | – | A |
| 19 | throughput, server | P1 76 B | ✅ **2.5** | ❌ 19.2 | 11.0 | 1.42 † | – | A |
| 20 | throughput, client | P2 1292 B | ✅ **6.5** | ❌ 19.3 | 8.5 | ✗ † | – | A |
| 21 | throughput, server | P2 1292 B | ✅ **3.8** | ❌ 20.2 | 14.5 | ✗ † | – | A |
| 22 | throughput, client | P4 2800 B | ✅ **12.2** | ❌ 38.7 | 14.1 | ✗ † | – | T |
| 23 | throughput, server | P4 2800 B | ✅ **8.2** | ❌ 33.5 | 19.6 | ✗ † | – | T |
| 24 | latency | P1 76 B | ✅ **155** | ❌ 356 | 237 | 121.1 | – | A |
| 25 | latency | P2 1292 B | ✅ **135** | ❌ 393 | 239 | 144.6 | – | A |
| 26 | throughput, client | P1, 5% loss | ✅ **5.7** | ❌ 27.6 | 12.9 | ✗ † | – | A |
| 27 | throughput, client | P4, 5% loss | ✅ **14.4** | ❌ 63.5 | 43.0 | ✗ † | – | T |
| | **[Memory](#memory)** (peak RSS, KB) | | | | |  | – | |
| 28 | throughput, client | P1 76 B | ✅ **1,916** | ❌ 15,904 | 5,232 | 2,127 † | – | A |
| 29 | throughput, client | P2 1292 B | ✅ **2,196** | ❌ 15,048 | 6,400 | ✗ † | – | A |
| 30 | throughput, client | P4 2800 B | ✅ **2,268** | ❌ 13,976 | 5,572 | ✗ † | – | T |
| 31 | latency, client | P1 76 B | ✅ **1,660** | ❌ 14,340 | 4,836 | 2,135 | – | A |
| 32 | throughput, server | P1 76 B | ✅ **1,828** | ❌ 14,332 | 4,888 | 2,127 † | – | A |
| 33 | throughput, server | P4 2800 B | ✅ **2,848** | ❌ 13,992 | 4,900 | ✗ † | – | T |
| 34 | throughput, server | P4, 5% loss | ✅ **2,848** | ❌ 14,332 | 6,400 | ✗ † | – | T |
| | **[Bandwidth](#bandwidth)** (wire B / sample) | | | | |  | – | |
| 35 | wire bytes | P1 76 B | ✅ **138** | ❌ 286 | 180 | 93.6 † | – | A |
| 36 | wire bytes | P2 1292 B | ✅ **1,355** | ❌ 1,502 | 1,397 | ✗ † | – | A |
| 37 | wire bytes | P3 1424 B | ✅ **1,487** | ❌ 1,865 | 1,585 | ✗ † | – | T |
| 38 | wire bytes | P4 2800 B | ✅ **2,921** | ❌ 3,460 | 2,950 | ✗ † | – | T |
| 39 | wire bytes | P1, 5% loss | ✅ **143** | ❌ 287 | 194 | ✗ † | – | A |
| 40 | wire bytes | P4, 5% loss | ✅ **3,041** | ❌ 4,384 | 3,230 | ✗ † | – | T |
| 41 | framing overhead | single datagram | ✅ **62.1** | ❌ 210.3 | 104.2 | 63.0 § | – | A |
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

### Legend

- **⚪** deliberately not scored: a fair comparison is not possible in that row (5, 45-47), or, in rows 52-67, the
  first two are within each other's spread (a draw).
- **zenoh-pico is reference and never scored**: no ✅/❌ toward totals, no WIN/DRAW/LOSE. Its cells come from its own
  session (`Z`, `results/zenoh_cells_31d58011_2026-09-29.txt`), so its margins are looser than a within-row one.
- **`†`** measured over TCP, the only configuration where zenoh-pico's reliability is real (its RELIABLE means
  monotonic sequence numbers, not retransmission). Untagged zenoh-pico cells are its UDP-multicast best-effort arm.
- **`✗`** measured, and the transport did not survive the cell: zenoh-pico's TCP session dies a few hundred samples
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
| `A` | aligned 12-cell native campaign, 3 reps, medians, FastDDS as shipped; re-measured on `9dbffd40` (`results/cmp_A_9dbffd40_2026-09-29.txt`) |
| `T` | P3/P4 cells with FastDDS `maxMessageSize` 1472 (fair-evaluation setting) (`results/cmp_T_9dbffd40_2026-09-29.txt`) |
| `G` | p1-p4 gap cells added 2026-09-30 (`results/cmp_p1p4_gap_99033118_2026-09-30.txt`, WIN 38 / DRAW 4 / LOSE 0) |
| `S` | QoS-mechanics sweep 2026-09-24/25 (`experiments/comparison_resweep.sh` at `659013e9`) |
| `L` | liveliness, `liveliness_l2.sh`, 2026-09-27, `0f220ba0`, 20 reps (`results/liveliness_l2e_2026-09-27.txt`) |
| `W` | four-way rmw block wait, `934f90de`, 3 reps (`results/rmw_4way_block_934f90de_2026-09-30.txt`) |
| `J` | four-way rmw poll wait, random phase, RTT at callback, `934f90de`, 7 reps (`results/rmw_4way_poll*_cb_r7_*_2026-10-01.txt`) |

### Latency

Rows 1-6b. `reliable_latency`, one ping in flight, 100 round trips per rep, median of 3. All absolute RTTs were
taken under the `ondemand` CPU governor, which adds about 10% (19-21 us); ordering and margins are unaffected because
all frameworks ran under it in the same sessions. Row 5's mean is a draw by construction (10 ms netem swamps a 0.2 ms
RTT); row 6's tail still separates. zenoh-pico's column here is its TCP arm.

### Throughput

Rows 7-17. `send_mbps`, client, median of 3, RELIABLE + KEEP_ALL with `sent == recv` checked on every side.
11b and 11c are at the link ceiling (‡). **Row 11's FastDDS figure is a send rate it did not fully deliver:** FastDDS
lost 15.5% of samples in that cell on a 2026-10-03 run and 22.2% in the QoS sweep. FastDDS as shipped
(`maxMessageSize` 65,500) at the `T` cells, for reference: P3 533 Mbps and 1,677 B/sample; P4 786 Mbps, 3,101
B/sample and 14,864 KB; P4 under 5% loss 9.9 Mbps, and it **did not deliver everything** within the drain cap (3,252
of 3,439), because kernel IP reassembly fails under loss. The `A` rows predate the shared-memory module, which on its
first build cost 2.0-2.7% at P1 cross-host (`f938461e`) before lazy segment creation; no verdict depends on it.

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
target without demand paging TickLE's declared static storage is the real figure. (2) The KEEP_ALL bound is not
identical: FastDDS may hold 4,000 samples of any size (about 11 MB at p4), so **the margin against FastDDS at p2-p4 is
inflated**; subtracting all of it still leaves FastDDS at about 17 MB against TickLE's 2.2 MB.

### Bandwidth

Rows 35-41. Wire bytes per sample from `/proc/net/dev`, so retransmissions and control traffic count. Row 41's per
single-datagram framing includes the 42 B of Ethernet+IP+UDP.

### QoS mechanics

Rows 42-47. Count-based scenarios: late-joiner durability, a burst within and beyond a fixed HISTORY depth of 8,
50 ms DEADLINE, LIVELINESS lease (median detection time from the last received sample; all three match the lease
within 1 ms, hence no winner), 100 ms LIFESPAN. Row 44 is the one scored row: TickLE delivers 147.7 of 160 beyond
depth, both DDS 109 (deterministic, 3/3).

### rmw layer

Rows 48-71. `rmw_tickle`, `rmw_fastrtps_cpp`, `rmw_cyclonedds_cpp` and `rmw_zenoh_cpp` driven through `rclcpp` by one
unchanged ping/pong binary pair; `RMW_IMPLEMENTATION` alone selects the rmw, and every row checks both sides'
`/proc/PID/maps`. Block wait (`W`) is the `spin()` pattern; poll wait (`J`) loops on `spin_some()` with 0, 50, 100 or
200 us sleeps and a random pause so the reply lands at a random phase. `J` verdicts use overlap of the repetitions'
interquartile ranges; ⚪ is a draw. rmw_zenoh_cpp needs a Zenoh router, run on the pong host and included in its
figures. Memory is from outside each process; CPU is the pong's whole-run thread time (4 s idle plus 100 round
trips), not a per-message cost. rmw_tickle's RSS includes its shared-memory segment: row 68 rose from 11,264 to
12,544 kB when the segment was added to core.

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
payload shapes, the same explicit QoS asserted on every row, release builds on all sides, processes pinned away from
the NIC interrupt core, frameworks interleaved within one session, 3 repetitions and medians. A cell is VOID when a
payload crosses a framework's datagram boundary unexpectedly, when history policies differ, or when delivery is
incomplete (an incomplete vendor is excluded and listed; an incomplete TickLE loses the cell). FastDDS's only tuned
parameter is `maxMessageSize` 1472 in the `T` rows. Full rules, fairness audit and zenoh-pico configuration:
[TESTING.md](TESTING.md).

## 4. Open work

The to-do list (delivered vendor rates for S1-S3, missing same-host CPU/memory cells, loaned messages, and more) is
in [ROADMAP.md](ROADMAP.md).
