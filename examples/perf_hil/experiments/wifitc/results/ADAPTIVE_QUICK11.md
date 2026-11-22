# Adaptive rate control, quick test 11 on `ab/adaptive-next` ae5c192a (2026-11-22)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (tree `eaa0b2b6` = `origin/ab/adaptive-next` `ae5c192a` merged). 41 runs, 0 lost, 0 aborts, clean session ends, Pis clean (no `wtc_*` namespace or tap; the real `eth0` and `wlan0` not touched). Raw data `~/rig_results_safe/wtc_adaptive_quick11`, per-run table `results/quick11_runs.txt`. No library change in this commit.
Arms: **B** right prior (`tickle-ad-c4` on S, `tickle-ad-c30` on M), **C** wrong prior (`tickle-ad-c30` on S1/S2), **A** default (`tickle-ad`). Cells as asked: B 1 MiB S2 x10, C 1 MiB S1 and S2 x5, B 1 MiB S1 x5, A and B on M1/M2 at 128 KiB and 1 MiB x2.

## 1. Table
| item | quick10 / v8 | quick11 | verdict |
|---|---|---|---|
| B 1 MiB S2 (10 runs): collapse? | quick10 r2: 2 of 15, rate to 100 kbit/s, 2 stall restarts | **15/15 in all 10 runs; rate never below 3,800 kbit/s (swing 1.0x); 0 stall restarts; queue trips 1-6 per run; p50 2,474-2,534 ms, p99 2,568-3,218 ms (median 2,764)** | **did not recur**; oracle 2,497 / 2,660 ms |
| B 1 MiB S2 retransmissions / tx | quick10 698 and 105 | 548-696 (oracle 580); tx 12.5-12.7 k (x1.08 of 11,682) | equal to the oracle |
| B 1 MiB S1 (5 runs) vs fixed 95 % | quick10 2,388 / 2,421 ms | 15/15; p50 2,384-2,396, p99 2,408-2,441 ms; swing 1.0x; 0 queue trips; retx 109-186 | **equal** (oracle 2,374 / 2,402, retx 122) |
| C 1 MiB S1 (5 runs) | quick10: 15 and 11 of 15, p99 5.2 and 31 s | **15, 14, 4, 14, 14 of 15**; p99 13.2 / 19.9 / 27.3 / 22.1 / 15.6 s; swing 18-32x; 21-31 drop trips | **not fixed** (one run 4 of 15) |
| C 1 MiB S2 (5 runs) | quick10: 8 and 10 of 15, p99 14 and 35 s, swing 285x | **11, 8, 6, 10, 1 of 15**; p99 12-25 s; swing 148-266x; final rate 0.1-0.2 Mbit/s in four runs; retransmissions up to 17,015 | **not fixed** (one run 1 of 15) |
| B on M1 (2 runs per size) | quick10 / v8 as oracle | 1 MiB 321 / 325, 321 / 328 ms (oracle 323 / 334); 128 KiB 43 / 53, 49 / 69 ms (oracle 44 / 55); no queue trips, swing 1.0x | **equal to the oracle** |
| B on M2 | quick10 128 KiB 93, 67 / 119, 142; 1 MiB 355, 402 / 1,007, 543 | 128 KiB 77, 58 / 439, 101 ms (oracle 79 / 135); 1 MiB **367, 355 / 386, 635** ms (oracle 367 / 488) | **at the oracle** (one 128 KiB p99 of 439 ms, one 1 MiB p99 of 635 ms) |
| A on M1 / M2 (regression) | v8: 128 KiB M1 101 ms p99, M2 99 / 261; 1 MiB M2 528 / 1,147 | 128 KiB M1 47, 42 / 104, 98; M2 99, 91 / 728, 153; 1 MiB M1 337, 306 / 685, 436; M2 520, 547 / 1,733, 1,917; swing 8-25x; 0 stall restarts | **unchanged** (no prior = arm A, as stated); 1 MiB M2 tails x3.5-3.9 of the oracle's |

## 2. Read
- **The collapse is fixed**: 10 of 10 B runs at 1 MiB S2 and 5 of 5 on S1 deliver 15/15 at the rate the prior gives (3,800 kbit/s), with the p50 within 0.5 % and the p99 within 4 % of the fixed 95 % arm. Arm B on all four profiles tested now equals the fixed rate (p50 within 1-2 %; the p99 gaps are single-run outliers on M2).
- **Arm C is not fixed**: a 30 Mbit/s prior on a 4 Mbit/s link still ends 1-11 of 15 at 1 MiB S2 (five runs: 11, 8, 6, 10, 1) and 4-15 of 15 at S1, with 12-27 s tails and 148-266x swings. It is no better than quick10 and no better than the shipped unpaced default (Stage A/B: 1 MiB S2 delivery 40-87 %, p50 14 s). A wrong prior well above the link remains a hazard the documentation must state (the nominal rate is an upper bound, not a target).
- **Arm A unchanged**: it still has the 1 MiB M2 / S2 tails and oscillation of v8.

## 3. Recommendation
- **Stop the full S1.** The new evidence settles what it could settle: arm B equals the fixed 95 % arm on every cell tested (1 MiB S1 / S2 x5-10, M1 / M2 x2), arm C is unsafe, arm A is as before. A full 240-run B run would repeat the Stage C numbers; it would show a tie with the fixed rate, not an advantage.
- **Ship fixed pacing (95 % of the carried rate) as the 2.2.0 Wi-Fi option, as before; leave ADAPTIVE out of 2.2.0** (or, if the user wants it in, opt-in experimental with the wrong-prior hazard documented). On this fixed-rate emulation a prior-anchored controller can at best equal the fixed rate; its possible advantage (tracking a link whose rate changes) was never tested: the rate-step stage (30 -> 4 -> 30 Mbit/s, acceptance A4) and the multi-reader stage (A11) are not done, and real Wi-Fi was never available.
- **If more evidence is wanted, run the rate-step stage only** (about 2 h): arms B (right prior, `tickle-ad-c30` then the link steps down to 4 Mbit/s: a prior that becomes wrong), A (no prior), fixed 95 % (`tickle-p95`, 30 Mbit/s) and the DDS stacks on the stepped link; that is the one test in which an adaptive controller can show a benefit the fixed rate cannot, and the one in which a stale prior (arm C's situation) is realistic.
- Not changed: no library, wire, API, default, merge or tag in this stage.
