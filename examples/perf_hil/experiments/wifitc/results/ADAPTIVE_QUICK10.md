# Adaptive rate control, quick test 10 on `ab/adaptive-next` 32e2a6c2: prior-anchored mode (2026-11-22)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (tree `0a0b2c3f` = `origin/ab/adaptive-next` `32e2a6c2` merged). 32 runs, 2 per cell and arm, 0 lost, clean session ends, Pis clean. **Smoke-level evidence (2 runs per cell).** Raw data `~/rig_results_safe/wtc_adaptive_quick10`, per-run table `results/quick10_runs.txt`. No library change in this commit.
Arms: **B** right prior (`tickle-ad-c30` on M1/M2, `tickle-ad-c4` on S1/S2), **C** wrong prior (`tickle-ad-c30` on S1/S2), **A** default (`tickle-ad`, no prior) on M2 and on 1 MiB S1/S2 as the regression check.

## 1. Table (two runs: delivered of 15 / p50 / p99 ms / stall restarts / swing / tx / retx)
| cell | arm B right prior | arm C wrong prior | arm A | oracle (Stage C p95) p50 / p99 |
|---|---|---|---|---|
| 128 KiB M1 | 15,15 / 44 / **48**, 43 / 54 / 0,0 / 1.0x / 1,647, 1,651 / 15, 17 | - | - | 44 / 55 |
| 128 KiB M2 | 15,15 / **93 / 119, 67 / 142** / 0,0 / 2.0x, 1.2x / 1,671, 1,571 / 30, 17 | - | 93 / 155, 155 / 259 | 79 / 135 |
| 128 KiB S1 | 15,15 / 292 / 303, 298 / 305 / 0,0 / 1.0x / 1,703, 1,937 / 15, 26 | 15,15 / 280 / 391, 287 / 364 / 0,0 / 1.2-1.6x / 2,345, 2,339 / 626, 619 | - | 292 / 304 |
| 128 KiB S2 | 15,15 / 357 / 510, 324 / 599 / 0,0 / 1.6x / 1,856, 1,770 / 117, 79 | 15,15 / 409 / 1,113, 323 / 511 / 0,0 / 33x, 3.8x / 2,174, 2,192 / 417, 494 | - | 325 / 570 |
| 1 MiB M1 | 15,15 / 320 / 323, 321 / 349 / 0,0 / 1.0x / 11,586, 10,547 / 100, 132 | - | - | 323 / 334 |
| 1 MiB M2 | 15,15 / **355 / 1,007, 402 / 543** / 0,0 / 1.6x / 11,578, 10,893 / 320, 373 | - | 754 / 1,892, 482 / 1,423 / 0,0 / 27x, 14x / 12,697, 11,731 / 1,303, 410 | 367 / 488 |
| 1 MiB S1 | 15,15 / 2,388 / 2,421, 2,392 / 2,418 / 0,0 / 1.0x / 13,930, 13,963 / 110, 130 | **15 and 11 of 15** / 2,322 / **31,028**, 2,263 / 5,204 / 0,0 / 46x, 19x / 11,550, 16,556 / 1,130, 2,925 | 15,15 / 2,263 / 2,776, 2,265 / 5,791 / 0,0 / 5.0x, 8.1x / 14,087, 14,603 / 262, 781 | 2,374 / 2,402 |
| 1 MiB S2 | 15 and **2 of 15** / 2,506 / 2,925, **50,085 / 50,085** / 0 and **2** / 1.2x, **38x** / 12,718, **2,276** / 698, 105 | **8 and 10 of 15** / 9,968 / 35,460, 7,724 / 14,090 / 0,0 / **285x** / 9,733, 28,210 / 2,411, **18,857** | 15,15 / 2,970 / 4,828, 3,062 / 5,092 / 0,0 / 13x, 20x / 14,487, 14,234 / 2,324, 2,138 | 2,497 / 2,660 |

## 2. Against the questions asked
| question | v8 / quick9 | quick10 | verdict |
|---|---|---|---|
| B on M2, 128 KiB p50 / p99 (oracle 79 / 135) | 165 / 283 ms | **93 and 67 / 119 and 142** | **fixed** (at the oracle) |
| B on M2, 1 MiB p50 / p99 (oracle 367 / 488) | 523 / 1,114 ms | 355 and 402 / 1,007 and 543 | **fixed at p50**; p99 r2 at the oracle, r1 1.0 s |
| B stall restarts on M2 | 26 in 2 runs | **0** (all M and S1 runs 0) | **fixed** |
| B on M1, S1 vs fixed 95 % | - | M1: 44 / 48-54 and 320-321 / 323-349 ms; S1: 292-298 / 303-305 and 2,388-2,392 / 2,418-2,421 ms; swing 1.0x | **equal to the oracle** (p50 within 1 %, p99 within 1-2 %) |
| B on S2 | quick9: 15/15 both runs, p99 3.0-3.1 s | 128 KiB equal to the oracle; 1 MiB r1 2,506 / 2,925 ms (oracle), **r2 collapsed: 2 of 15 delivered, 2 stall restarts, p50 / p99 50 s, rate down to 100 kbit/s** | **regression: one failure in two 1 MiB S2 runs** |
| C (wrong prior) at 1 MiB on S | v8: S1 11 of 15 (3 of 10 runs short), S2 4 of 15, p99 25-55 s, swing 181x | S1: 15 and 11 of 15, p99 5.2 and 31 s; S2: 8 and 10 of 15, p99 14 and 35 s, swing 285x, 18,857 retransmissions in one run | **not fixed**: still unsafe at 1 MiB; 128 KiB no worse than A (p50 280-409 ms) |
| 128 KiB M1 p99 vs Fast DDS 91.9 ms | v8 A 101 ms | B **48 and 54 ms** (A not rerun at M1) | B beats Fast DDS (and the oracle's 55 ms) |
| A regression vs v7 / v8 | v8 128 KiB M2 99 / 261; 1 MiB M2 528 / 1,147; S1 2,263 / 2,447; S2 2,797 / 5,944 | 128 KiB M2 93 and 155 / 155 and 259; 1 MiB M2 754 and 482 / 1,892 and 1,423; S1 2,263 / 2,776-5,791; S2 2,970-3,062 / 4,828-5,092; 0 stall restarts | unchanged within the spread (as stated: no prior = arm A unchanged); 1 MiB M2 a little worse in this sample |

## 3. The 1 MiB S2 failure of arm B (r2), from its RATETRACE
`rate_bps` ends at 0.29 Mbit/s, `btlbw` 0.23 Mbit/s, 2,276 datagrams sent for 15 samples, 2 stall restarts, 149 trace rounds. After the start at 3,800 kbit/s (95 % of the 4 Mbit/s prior) the rate falls round after round: 3,800 -> 2,432 -> 1,945 -> 1,556 -> 1,245 -> 1,000 -> 674 -> 420 -> 237 -> 198 -> **100 kbit/s**, with `qover` 0 in 58 of 62 listed rounds and `drain` = 1 in most of them, while `held` follows `units_sent` closely (lag 7-20 units). The measured bandwidth tracks the falling paced rate (`bw` is always about the last rate), so each cut re-bases the estimate lower: a downward ratchet that the loss alone (bursty profile, 2 % entry / 70 % loss in the bad state) starts and nothing arrests. The floor that stopped it was **100 kbit/s, not the 25 % of the prior (950 kbit/s) the change describes**; the 2 stall restarts follow. This is the only collapse among the 8 arm-B runs, but it hits the arm that is supposed to be the safe one.

## 4. Read and recommendation
- Arm B with a right prior is now **equal to the fixed 95 % arm on M1, S1 and 128 KiB S2, and fixed on M2** (p50 and the 128 KiB p99 at the oracle; the 1 MiB M2 p99 is 543 ms in one run and 1.0 s in the other); no stall restarts in 15 of 16 runs.
- **One collapse in 8 runs (1 MiB S2 r2) is a release blocker for B as the "good" arm**: the prior-anchored decrease ratchets down on bursty loss with no queue, and the floor is not the documented 25 % of the prior. Arm C is not materially better than v8 (8 and 10 of 15 on S2, 285x swing), so the wrong-prior step-down is not working at 1 MiB either.
- **Do not run the full S1 now.** Ask the implementer for (1) the floor: why `rate_bps` reaches 100 kbit/s when 25 % of the prior is 950 kbit/s; (2) the decrease on `drain` without a queue trip or a real drop in held units (the rounds above had `held` within 2 % of `units_sent`); (3) the wrong-prior step-down at 1 MiB S1 and S2. Then re-run this quick test with **10 runs of 1 MiB S2 in arm B and arm C (and 1 MiB S1)**, the cells that fail, before the full run (the full run, when it comes: arm B on all four profiles plus arm A for the no-prior claim and a reduced arm C, about 9 h as before).
- Product reading unchanged: the fixed 95 % rate remains the documented 2.2.0 option; the prior-anchored ADAPTIVE would, if fixed, match it with a self-adjusting guard but not beat it.
- Not changed: no library, wire, API, default, merge or tag in this stage.
