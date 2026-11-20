# Adaptive rate control, quick test 7 on `ab/adaptive-next` 74df5385 (2026-11-21)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (tree `3c210f76` = `origin/ab/adaptive-next` `74df5385` merged). `tickle-ad` only, 1 run per cell, 2 rounds (copy A), 128 KiB and 1 MiB x M1 M2 S1 S2 (16 runs, 0 lost, clean session end, Pis clean). One run per cell: indicative only. Raw data `~/rig_results_safe/wtc_adaptive_quick7`. No library change in this commit.

## Table (delivered of 15 / p50 / p99 ms / stall restarts / peak rate / final BtlBw / tx / retx; two runs per cell)
| cell | delivered | p50 / p99 (ms) | stall restarts | peak rate (Mbit/s) | final btlbw (Mbit/s) | tx / retx |
|---|---|---|---|---|---|---|
| 128 KiB M1 | 15, 15 | 42 / 98, 62 / 98 | 0, 0 | 59, 61 | 31.1, 25.2 | 1,496 / 12; 1,083 / 18 |
| 128 KiB M2 | 15, 15 | 188 / 303, 183 / 453 | 0, 0 | 16, 16 | 9.8, 7.7 | 1,698 / 20; 1,534 / 27 |
| 128 KiB S1 | 15, 15 | 279 / 398, 281 / 393 | 0, 0 | 8, 8 | 4.0, 4.1 | 1,797 / 132; 1,812 / 129 |
| 128 KiB S2 | 15, 15 | 315 / 433, 352 / 581 | 0, 0 | 8, 8 | 3.6, 4.1 | 1,852 / 179; 1,805 / 91 |
| 1 MiB M1 | 15, 15 | 375 / 672, 320 / 388 | 0, 0 | 61, 77 | 24.9, 31.3 | 11,947 / 983; 11,520 / 863 |
| 1 MiB M2 | 15, 15 | 394 / 639, 646 / 993 | 0, 0 | 61, 53 | 19.2, 9.4 | 12,147 / 945; 11,624 / 340 |
| 1 MiB S1 | 15, 15 | 2,225 / 2,452, 2,198 / 2,294 | 0, 0 | 10, 11 | 4.0, 4.0 | 13,269 / 425; 12,553 / 339 |
| 1 MiB S2 | 15, 15 | 3,270 / 9,511, 2,442 / 5,971 | 1, 0 | 46, 23 | 4.6, 2.5 | 16,062 / 4,403; 15,148 / 3,594 |

## Against the questions asked
| item | quick6 (5de6f40b) / v6 | quick7 (74df5385) | verdict |
|---|---|---|---|
| 1 MiB M2 overshoot | peak 180.8 Mbit/s, 47,623 tx / 36,353 retx, p99 2.4 s (quick6 r2) | peak 61 and 53 Mbit/s, 12,147 and 11,624 tx, 945 and 340 retx, p99 0.64 and 0.99 s | **fixed** (tx within 1.1x of the 11,396 oracle) |
| 1 MiB stall restarts | v6: S1 3 of 10 runs, S2 22 in 6 runs | 1 in 8 runs (S2 r1), 0 elsewhere | **much lower** (one run is not a rate) |
| 128 KiB S1 final BtlBw / rtprop | quick6 r1: BtlBw 0, rtprop 152 ms | BtlBw 4.0 and 4.1 Mbit/s (the 3.97 link), **trace-median rtprop still 153 ms** (path 10 ms) | BtlBw **fixed**; rtprop **unchanged** (the new tail sample does not fix it here) |
| 128 KiB M1 p50 / p99 | v6 42 / 98 ms (Fast DDS p99 91.9) | 42 and 62 / 98 and 98 ms; time from first trace line to 80 % of the link rate 80 ms | **unchanged**: p99 98 ms = 1.07x Fast DDS, the 128 KiB M1 LOSE of v6 stays; M1 r2 p50 62 ms |
| 128 KiB M2 p50 / p99 | v6 86 / 161 ms (DDS 100 / 106, 257 / 119) | **188 and 183 / 303 and 453 ms**; the rate never reaches 80 % of the link (peak 16 Mbit/s, final 9.8 and 5.8-7.7) | **worse**: p50 about 2x v6 and 1.8x the DDS stacks, p99 1.2-1.8x Cyclone (257) and 2.5-3.8x Fast DDS (119) |
| 128 KiB S1/S2 | v6 279 / 323 and 331 / 626 ms | 279-281 / 393-398 and 315-352 / 433-581 ms | S1 p50 same, p99 +20 %; S2 p50 same, p99 inside v6's spread |
| 1 MiB S2 | v6 p99 6.5 s, swing 103x, retx 3,156 | p99 9.5 and 6.0 s, swing 31x and 11x, retx 4,403 and 3,594, peak 46 and 23 Mbit/s on a 4 Mbit/s link | **still the weak cell** |
| 1 MiB M1, S1 | v6 p50 318, 2,239 ms | 320-375, 2,198-2,225 ms | same |

## Read
- The 1 MiB M2 overshoot and the stall restarts are fixed, as the change intended; delivery is 15/15 in all 16 runs.
- **128 KiB M2 regressed**: START ends earlier (on a "loss storm", per the commit) and the rate stays at 5-10 Mbit/s of a 30 Mbit/s link (never 80 %), so p50 is 183-188 ms against v6's 86 ms and the DDS stacks' 100-106 ms. The 128 KiB M1 case is unchanged because its start rate suffices; the 40 ms round trip of M2 is where the shorter START hurts.
- The `rtprop` estimate at 128 KiB S1/S2 is still far above the path (153 ms and 48-268 ms against 10 and 40 ms), and 1 MiB S2 still oscillates (swing 31x, up to 46 Mbit/s on a 4 Mbit/s link) with 3,600-4,400 retransmissions and 6-9.5 s tails.
- Net against v6: better 1 MiB M2 and stall restarts, worse 128 KiB M2; no change to the one v6 LOSE (128 KiB M1 p99 v Fast DDS). I would not start the full S1 run on this build until 128 KiB M2 is back near v6; one run per cell is a smoke-level result.
