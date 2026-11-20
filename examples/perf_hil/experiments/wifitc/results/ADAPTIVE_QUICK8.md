# Adaptive rate control, quick test 8 on `ab/adaptive-next` 7c6e4fe3 (2026-11-21)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (tree `85d5dc26` = `origin/ab/adaptive-next` `7c6e4fe3` merged). `tickle-ad` only, 1 run per cell, 2 rounds (copy A), 128 KiB and 1 MiB x M1 M2 S1 S2 (16 runs, 0 lost, clean session end, Pis clean). One run per cell: indicative only. Raw data `~/rig_results_safe/wtc_adaptive_quick8`. No library change in this commit.

## Table (delivered of 15 / p50 / p99 ms / stall restarts / peak rate / swing after START / tx / retx; two runs per cell)
| cell | delivered | p50 / p99 (ms) | stall | peak (Mbit/s) | swing | tx / retx |
|---|---|---|---|---|---|---|
| 128 KiB M1 | 15, 15 | 50 / 98, 56 / 148 | 0, 0 | 61, 61 | 3.7x, 4.6x | 1,584 / 20; 1,508 / 20 |
| 128 KiB M2 | 15, 15 | 100 / 179, 132 / 472 | 0, 0 | 19, 16 | 2.4x, 2.9x | 1,678 / 22; 1,316 / 33 |
| 128 KiB S1 | 15, 15 | 281 / 589, 281 / 450 | 0, 0 | 10, 12 | 5.3x, 3.8x | 1,952 / 45; 1,971 / 95 |
| 128 KiB S2 | 15, 15 | 344 / 1,146, 329 / 515 | 0, 0 | 8, 8 | 5.7x, 4.6x | 1,820 / 78; 1,820 / 76 |
| 1 MiB M1 | 15, 15 | 330 / 481, 301 / 365 | 0, 0 | 91, 86 | 8.1x, 5.5x | 12,747 / 1,231; 11,912 / 445 |
| 1 MiB M2 | 15, 15 | 475 / 1,018, 604 / 1,419 | 0, 0 | 66, 58 | 18.8x, 12.1x | 12,413 / 1,128; 11,296 / 292 |
| 1 MiB S1 | 15, 15 | 2,254 / 2,285, 2,267 / 3,317 | 0, 2 | 10, 32 | 3.6x, 23.8x | 14,082 / 266; 15,300 / 1,593 |
| 1 MiB S2 | 15, 15 | 3,014 / 6,782, 2,761 / 3,984 | 0, 0 | 17, 14 | 13.8x, 8.8x | 14,538 / 2,327; 13,612 / 1,515 |

## Against the questions asked
| item | v6 / quick7 | quick8 | verdict |
|---|---|---|---|
| 128 KiB M2 p50 / p99 | v6 86 / 161 ms; quick7 188 and 183 / 303 and 453 | **100 and 132 / 179 and 472 ms**; rate never reaches 80 % of the link (peak 19 and 16 Mbit/s; final 19.4 and 7.3) | **recovered most of the regression**: p50 equals Cyclone's 99.7 / Fast DDS's 106 in r1, +25 % over Fast DDS in r2; p99 r1 179 (Cyclone 257, Fast DDS 119), r2 472 |
| 128 KiB S1/S2 rtprop (trace median) | quick7 S1 153, S2 48-268 ms (path 10 / 40) | **S1 11.5 and 12.1 ms, S2 48.4 and 41.5 ms** | **fixed** (within 20 % of the path) |
| 1 MiB S2 | quick7 peak 46 / 23 Mbit/s, swing 31x / 11x, retx 4,403 / 3,594, p99 9.5 / 6.0 s | **peak 17 / 14, swing 13.8x / 8.8x, retx 2,327 / 1,515, p99 6.8 / 4.0 s** | **better** (peak 3x lower, retx -47 / -58 %, p99 -29 / -34 %); still above the fixed-rate oracle (p99 2.66 s, retx 580) |
| 1 MiB M2 | quick7 peak 61 / 53, retx 945 / 340, p99 0.64 / 0.99 s | peak 66 / 58, retx 1,128 / 292, p99 1.02 / 1.42 s | **same or slightly worse** (swing 18.8x / 12.1x against 3.1x / 6.7x); no overshoot (tx 12,413 / 11,296) |
| stall restarts | quick7: 1 in 8 runs; v6 S1 3 of 10, S2 22 in 6 runs | **2, in one run (1 MiB S1 r2)**; 0 in the other 15 | low (that one run swings 23.8x and has 1,593 retx) |
| 128 KiB M1 p99 | v6 98 ms (Fast DDS 91.9) | 98 and 148 ms; p50 50 and 56 ms (v6 42) | **no better**: the 1.07x LOSE of v6 stays in r1, r2 is worse; p50 +20-33 % over v6 |
| delivery | 16 of 16 | 16 of 16 at 15/15 | held |

## Read and recommendation
- Fixed or clearly improved against quick7: the 128 KiB M2 regression (p50 188 -> 100 / 132 ms), the S1/S2 round-trip estimate (153 ms -> 12 ms), 1 MiB S2 (peak, swing, retransmissions and tails all 30-60 % lower), stall restarts (one run).
- Not at the oracle yet: 1 MiB S2 p99 4-7 s (oracle 2.7 s) and 2,300 / 1,500 retransmissions (oracle 580); 1 MiB M2 swing 12-19x; 128 KiB M2 r2 stays at 7-16 Mbit/s on a 30 Mbit/s link (p99 472 ms); 128 KiB M1 p50 50-56 ms against 42-44 ms for the oracle.
- No metric is clearly worse than v6 except the 128 KiB M1 p50/p99 in this sample (one run each) and the 1 MiB M2 tail (1.0-1.4 s against v6's median p99 1.2 s: unchanged).
- **Recommendation: start the full S1 (240 runs, about 4 h) on 7c6e4fe3.** The two defects that made v6 unsatisfactory (1 MiB S2 oscillation, the estimate at 128 KiB on S1/S2) are corrected, delivery is 16 of 16, and nothing regressed beyond what one run per cell can resolve. The full run is what decides the remaining cells (1 MiB S2, 128 KiB M1 p99 against Fast DDS) with 10 runs each; I would not iterate further on 1-2 run samples. Not changed: no library, merge, tag or default.
