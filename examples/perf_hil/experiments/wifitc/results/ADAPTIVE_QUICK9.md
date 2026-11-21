# Adaptive rate control, quick test 9 on `ab/adaptive-next` 6b05d93c: default, right prior, wrong prior (2026-11-22)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (tree `220f8e4f` = `origin/ab/adaptive-next` `6b05d93c` merged). `tickle-ad`, 1 MiB and 128 KiB x M1 M2 S1 S2, **2 runs per cell and arm** (40 runs, 0 lost, clean session ends, Pis clean). Smoke-level evidence (2 runs per cell). Raw data `~/rig_results_safe/wtc_adaptive_quick9`. No library change in this commit.
Arms (the harness name sets `TT_PACING_BPS`, read by the bench into `tt_LinkOptions.pacing_rate`, which with `rate_control = ADAPTIVE` is the cap and, below 1 Gbit/s, the user prior of this build):
- **A default, no prior:** `tickle-ad` (`pacing_rate` 1 Gbit/s, START 8 Mbit/s).
- **B right prior:** `tickle-ad-c30` on M1/M2 (30 Mbit/s), `tickle-ad-c4` on S1/S2 (4 Mbit/s; carried rates 29.73 and 3.97).
- **C wrong prior:** `tickle-ad-c30` on S1/S2 (30 Mbit/s on a 4 Mbit/s link).

## 1. Table, per cell and arm (two runs: delivered of 15 / p50 / p99 ms / stall restarts / swing after START / tx / retx)
| cell | A default | B right prior | C wrong prior (S only) | oracle (Stage C p95) p50 / p99, tx, retx |
|---|---|---|---|---|
| 128 KiB M1 | 15,15 / 54 / 165, 46 / 109 / 0,0 / 6.4x, 1.9x / 1,597, 1,653 / 9, 11 | 15,15 / 44 / **68**, 52 / 109 / 0,0 / 2.7x, 3.3x / 1,662, 1,675 / 14, 19 | - | 44 / 55; 1,585; 16 |
| 128 KiB M2 | 15,15 / 90 / 470, 116 / 244 / 0,0 / 1.6x, 2.2x / 1,688, 1,699 / 27, 33 | 15,15 / 134 / 170, 189 / 537 / 0,0 / 4.7x, 15.4x / 1,693, 1,725 / 26, 35 | - | 79 / 135; 1,658; 44 |
| 128 KiB S1 | 15,15 / 282 / 541, 281 / 293 / 0,0 / 3.3x, 1.9x / 1,965, 1,936 / 48, 25 | 15,15 / 290 / 361, 290 / 361 / 0,0 / 1.3x, 1.3x / 1,356, 1,932 / 24, 15 | 15,15 / 281 / 346, 281 / 353 / 0,0 / 3.0x, 2.4x / 1,958, 1,986 / 71, 85 | 292 / 304; 1,560; 16 |
| 128 KiB S2 | 15,15 / 339 / 699, 328 / 474 / 0,0 / 4.5x, 2.1x / 1,890, 1,824 / 140, 143 | 15,15 / 320 / 412, 349 / 556 / 0,0 / 1.4x, 2.3x / 1,795, 1,815 / 73, 70 | 15,15 / 324 / 730, 374 / 720 / 0,0 / 3.9x, 3.7x / 1,893, 1,851 / 157, 143 | 325 / 570; 1,693; 80 |
| 1 MiB M1 | 15,15 / 339 / 543, 377 / 520 / 0,0 / 13.2x, 8.7x / 12,786, 12,337 / 1,270, 804 | 15,15 / 315 / 458, 317 / 478 / 0,0 / 2.4x, 2.6x / 11,617, 11,648 / 114, 130 | - | 323 / 334; 11,156; 122 |
| 1 MiB M2 | 15,15 / 506 / 1,392, 671 / 1,679 / 0,0 / 15.1x, 29.2x / 12,294, 11,937 / 976, 573 | 15,15 / 588 / 1,882, 376 / 700 / 0,0 / 15.2x, 3.8x / 11,784, 11,501 / 402, 241 | - | 367 / 488; 11,396; 298 |
| 1 MiB S1 | 15,15 / 2,266 / 3,448, 2,279 / 6,345 / 0,0 / 5.0x, 20.3x / 14,123, 15,498 / 318, 1,726 | 15,15 / 2,378 / 2,482, 2,374 / 2,464 / 0,0 / 1.4x, 1.4x / 12,592, 11,853 / 149, 136 | **6, 12 of 15** / 22,367 / 33,970, 2,414 / 20,770 / 0,0 / 29.9x, 32.2x / 12,117, 12,464 / 3,055, 1,061 | 2,374 / 2,402; 10,838; 122 |
| 1 MiB S2 | 15,15 / 2,545 / 3,629, 2,818 / 3,817 / 0,0 / 11.6x, 12.4x / 13,675, 13,889 / 1,614, 1,669 | 15,15 / 2,426 / 3,086, 2,409 / 3,022 / 0,0 / 2.7x, 2.0x / 12,378, 12,579 / 577, 583 | **2, 13 of 15** / 43,482 / 43,482, 3,469 / 12,861 / 0,0 / 177x, 37.5x / 3,687, 13,561 / 814, 2,731 | 2,497 / 2,660; 11,682; 580 |

## 2. Against the questions asked
| question | v7 | A default | B right prior | C wrong prior |
|---|---|---|---|---|
| stall restarts (1 MiB S1 / S2) | 7 in 4 runs / 19 in 7 runs | **0 / 0** | 0 / 0 | 0 / 0 |
| 1 MiB S2 swing; delivery | 84x max; one run 9/15 | 11.6x, 12.4x; 15/15 both | **2.7x, 2.0x**; 15/15 both | 177x, 37.5x; **2/15 and 13/15** |
| 128 KiB M1 p99 (Fast DDS 91.9 ms) | 98.4 | 165, 109 (worse) | **68**, 109 (r1 beats Fast DDS) | - |
| 128 KiB M2 p50 (oracle 79) | 104 | 90, 116 | 134, 189 (worse) | - |
| 1 MiB M2 p50 / p99 (oracle 367 / 488) | 510 / 1,034 | 506, 671 / 1,392, 1,679 (not better) | 588, 376 / 1,882, 700 (bimodal; r2 at the oracle) | - |
| 1 MiB S1 tx datagram ratio (oracle 10,838) | 1.32x | 1.30x, 1.43x | **1.16x, 1.09x** (retx 149, 136 against the oracle's 122) | 1.12x, 1.15x (but 6 and 12 of 15 delivered) |
| C against A on S links | - | 1 MiB S1 and S2 15/15, p99 3-6 s | - | **much worse at 1 MiB** (delivery 2-13 of 15, p99 12-43 s, swing 30-177x); 128 KiB no worse than A |

## 3. Read
- **The stall false-restart problem is fixed in every arm** (0 restarts in 40 of 40 runs; v7 had 26 across 11 runs). Delivery is 15/15 in all A and B runs.
- **B (right prior) is essentially the fixed-rate oracle on the S links at 1 MiB**: p50 2,374-2,426 ms (oracle 2,374 / 2,497), p99 2.5 and 3.0-3.1 s (oracle 2.4 / 2.7), datagrams 1.09-1.16x, retransmissions at or near the oracle's (136-149 against 122; 577-583 against 580), swing 1.4-2.7x. On M1 it matches the oracle too (p50 315-317 against 323; p99 458-478 against 334; retx 114-130 against 122). Its weakness is M2: bimodal (r1 588 / 1,882 ms, r2 376 / 700) and worse at 128 KiB M2 (134 and 189 ms against the oracle's 79 and A's 90 / 116): a start at half the cap (15 Mbit/s) overshoots on a 40 ms loop with bursty loss.
- **A (no prior) did not improve the cells the earlier builds had left weak:** 1 MiB M2 p50 / p99 (506-671 / 1,392-1,679 ms against 367 / 488), 1 MiB S1 datagrams 1.30-1.43x, 128 KiB M1 p99 109-165 ms (v7 98), swings of 13-29x on M1 / M2 and 20x on one S1 run. Its gain over v7 is the S2 case (84x -> 12x, no short run) and zero restarts.
- **C (a cap far above the link) is unsafe on slow links at 1 MiB:** the controller starts at 15 Mbit/s into a 4 Mbit/s link and does not recover within the run (delivery 6, 12, 2, 13 of 15; p99 12-43 s; swing up to 177x). At 128 KiB it is no worse than A. The documentation's existing warning (the nominal PHY rate is an upper bound, not a target) applies: a prior above the carried rate costs more than no prior.
- One caution on B: it needs the user to know the link's carried rate to within what the controller tolerates, which is the same calibration the fixed rate needs; the measured gain over the fixed 95 % arm is not established by this test (same figures, 2 runs each).

## 4. Recommendation
- **Run the full S1 (10 runs per cell) with arms A and B, and a reduced arm C**: A (default) because it is the only arm that claims "no calibration"; B (right prior = the calibrated carried rate) because it is the build's best case and the one to compare with the fixed 95 % arm (65 / 31 / 0 / 16); **C only on 1 MiB S1 and S2 (and 128 KiB for completeness), 10 runs, as a safety bound to document, not as a scored arm.** Cost: A 240 runs plus B 240 runs (about 4 h each) plus C 40 runs (about 1 h), about 9 h of Pi time.
- Expectation to test, not a result: B will be at or near the fixed rate on M1, S1, S2 and weak on M2; A will keep the 1 MiB M2 and 128 KiB M1 p99 gaps. If the Pi time must be cut, B on S1 and S2 plus A everywhere is the minimum that settles "is ADAPTIVE worth shipping".
- Product reading so far: the fixed 95 % rate stays the documented recommendation for 2.2.0; ADAPTIVE with a correct prior is as good on the slow links, but with a wrong prior it is worse than not using it, and without a prior it has not reached the oracle on M2.
- Not changed: no library, wire, API, default, merge or tag in this stage.
