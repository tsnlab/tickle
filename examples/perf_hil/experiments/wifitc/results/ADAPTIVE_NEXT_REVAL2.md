# Re-validation 2 of ADAPTIVE commit 448b685f (`ab/adaptive-next`, on top of 1054f2ff), 2026-11-25

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi** (netns + GRE tap; the Pis' real `eth0`/`wlan0` untouched; addresses and qdisc unchanged afterwards; Pis clean). Library `448b685f` merged into `ab/wifi-adaptive-s1` as tree `5a0fb18e` (no push to the release branch or main); binaries rebuilt on both Pis (16:58). The change (DESIGN section 25): `fast_probe` removed; a round that delivers 196,608 bytes or more at once is read over its active time and is due a probe after 8 s; the carry of a running probe over quiet rounds stays. 53 runs, 0 lost, 0 aborts. Raw data `~/rig_results_safe/wtc_adaptive_reval2_20261125`; per-phase tables `results/reval2_ratestep_tables.txt`; static runs `results/reval2_static_runs.txt`. No library change in this commit.
**What I ran / reused:** new `tickle-ad` only: rate step 1 MiB x **12** runs and 128 KiB x 5 (30 -> 4 at 45 s -> 30 Mbit/s at 90 s, M1, 135 s); static arms A (no prior) and B (right prior), 3 runs per cell on M1/M2 at 128 KiB and 1 MiB and S1/S2 at 1 MiB. **Reused:** old ADAPTIVE (`ae5c192a`), fixed 95 %, CycloneDDS and Fast DDS from `wtc_ratestep_exact_20261124` (5 runs each); `1054f2ff` from `wtc_adaptive_reval_20261125`; fixed 95 % static from Stage C, DDS static from Stage A/B.

## 1. Rate step, 1 MiB: delivered / p50 / p90 / max (ms) per phase, 12 runs for the new build, 5 for the others
| arm | before the step (30 Mbit/s) | at 4 Mbit/s | after the step up | recovery down / up (s, median) | failed runs |
|---|---|---|---|---|---|
| ae5c192a | 55/55 / 329 / 502 / 800 | 59/60 / 2,266 / 9,454 / 17,999 | 55/55 / 338 / 433 / 868 | 17 / 4 | 0 of 5 |
| 1054f2ff | 55/55 / 332 / 482 / 806 | 51/60 / 2,267 / 12,432 / 42,794 | 45/55 / 349 / 513 / 43,475 | 13 / 4 (1 never) | **1 of 5** |
| **448b685f** | 132/132 / 338 / 461 / 727 | 139/144 / **2,269 / 7,727 / 16,033** | 132/132 / **327 / 440 / 1,263** | 17 / 4 | **0 of 12** |
| fixed 95 % of 30 | 55/55 / 324 / 333 / 533 | 60/60 / 6,062 / 10,477 / 15,718 | 55/55 / 324 / 332 / 354 | 1 / 4 | 0 of 5 |
| CycloneDDS | 60/60 / 818 / 855 / 1,018 | 55/55 / 2,541 / 3,462 / 4,637 | 50/50 / 818 / 918 / 1,018 | 3 / 2 | - |
| Fast DDS | 60/60 / 1,086 / 1,107 / 5,595 | 55/55 / 23,727 / 34,436 / 38,322 | 50/50 / 1,948 / 3,954 / 3,979 | 31 / 16 (3 of 5 never) | - |
Failed run = three or more samples undelivered or a latency over 20 s. 448b685f per run: no failure in 12; slow-phase maxima 2.3-16.0 s; undelivered samples 1, 2, 2 in three runs (5 of 144 in the slow phase); recovery after the step down per run 5, 17, 17, 21, 21, 17, 13, 1, 21, 25, 5, 5 s, after the step up 4 s in 10 runs and 12 and 16 s in two. ae5c192a per run down 21, 17, 25, 9, 5 s; 1054f2ff 1, 33, 13, 13, 21 s (run 2 failed).

## 2. Rate step, 128 KiB (5 runs each build)
| arm | before | at 4 Mbit/s | after the step up | recovery down / up (s, median; per run) | failed |
|---|---|---|---|---|---|
| ae5c192a | 55/55 / 46 / 109 / 233 | 60/60 / 281 / 290 / 307 | 55/55 / **102** / 203 / 281 | 1 / **36** (36, 36, 4, 36, 44) | 0 |
| 1054f2ff | 55/55 / 48 / 98 / 127 | 60/60 / 281 / 303 / 404 | 55/55 / **49 / 76 / 172** | 1 / **8** (4, 8, 8, 4, 8) | 0 |
| **448b685f** | 55/55 / 44 / 98 / 114 | 60/60 / 281 / 293 / 333 | 55/55 / **47 / 161 / 202** | 1 / **24** (36, 28, 24, 4, 8) | 0 |
| fixed 95 % | 55/55 / 44 / 47 / 54 | 60/60 / 279 / 282 / 299 | 55/55 / 43 / 51 / 55 | 1 / 4 | 0 |
| CycloneDDS | 60/60 / 56 / 73 / 127 | 55/55 / 301 / 339 / 407 | 50/50 / 55 / 69 / 218 | 3 / 2 | - |
| Fast DDS | 60/60 / 89 / 92 / 93 | 55/55 / 618 / 621 / 621 | 50/50 / 90 / 93 / 93 | 3 / 2 | - |
**The 128 KiB step-up gain holds in p50 (47 ms; 1054f2ff 49; ae5c192a 102) but not in recovery:** the median is 24 s against 8 s for 1054f2ff (three of five runs took 24, 28, 36 s; two took 4 and 8 s) and the p90 is 161 against 76 ms; it is still better than ae5c192a (36 s median, 36-44 s in four of five runs), and p50 is level with fixed 95 % (43) and CycloneDDS (55), ahead of Fast DDS (90).
Per-phase verdicts of 448b685f (strict rule on per-run phase p50): 1 MiB against CycloneDDS WIN / TIE / WIN (before / at 4 Mbit/s / after), against Fast DDS WIN / WIN / WIN, against fixed 95 % TIE / WIN / TIE; 128 KiB against CycloneDDS TIE / TIE / TIE, against Fast DDS WIN / WIN / TIE, against fixed 95 % TIE / TIE / TIE (the old `ae5c192a` LOSEs against the fixed rate are gone). The A/A noise rule does not apply (one copy per arm).

## 3. Static cells (3 runs; p50 / p99 ms medians [max]; delivery 15/15 in all runs; stall restarts 0 in all 53 runs of this chain and in all earlier static runs of 1054f2ff)
| cell | ae5c192a (quick11, 2 runs; S: 6b05d93c v8) | 1054f2ff | **448b685f** | fixed 95 % | CycloneDDS | Fast DDS |
|---|---|---|---|---|---|---|
| 1 MiB M1, arm A | 321 / 561; queue trips 4, drop 36 / 28, retx 1,411 / 855 | 357 / 570 [698]; queue trips 12 | **348 / 559 [840]**; queue trips **6**; drop 15-39; retx 329-1,126 | 323 / 334 | 818 / 919 | 1,082 / 1,101 |
| 1 MiB M2, arm A | 534 / 1,825 | 479 / 1,046 | **506 / 803 [1,081]**; qtrips 2; drop 12-16 | 367 / 488 | 866 / 1,313 | 1,220 / 1,272 |
| 1 MiB S1, arm A | (v8) 2,263 / 2,447 | 2,267 / **4,024 [6,359]**; qtrips 14; drop 69, 28, 26; retx 2,189, 1,116, 987 | **2,258 / 2,663 [2,671]**; qtrips 10; drop **7, 22, 12**; retx **242, 799, 432** | 2,374 / 2,402 | 2,514 / 2,662 | 31,069 / 32,906 (8-9 of 15) |
| 1 MiB S2, arm A | (v8) 2,797 / 5,944 | 3,172 / **6,734 [8,807]**; qtrips 5; drop 31, 55, 60; retx 1,663-2,638 | **2,641 / 4,490 [5,051]**; qtrips 10; drop 35, 67, 46; retx 2,160, 2,893, 2,030 | 2,497 / 2,660 | 3,803 / 4,658 | 29,326 / 31,829 (8-9 of 15) |
| 128 KiB M1, arm A | 44 / 101 | 48 / 98 | 46 / 109 [109] | 44 / 55 | 55 / 126 | 91 / 92 |
| 128 KiB M2, arm A | 95 / 440 | 93 / 221 [1,196] | 92 / 172 [380] | 79 / 135 | 100 / 257 | 106 / 119 |
| **arm B (right prior), same cells** | - | M1 322 / 333; M2 356 / 416; S1 2,388 / 2,429; S2 2,516 / 2,776; 128 KiB M1 44 / 54, M2 81 / 129 | **M1 321 / 332; M2 367 / 542; S1 2,391 / 2,428; S2 2,503 / 2,732; 128 KiB M1 44 / 53, M2 65 / 114**; swing 1.0x, 0 drop trips | 323 / 334; 367 / 488; 2,374 / 2,402; 2,497 / 2,660; 44 / 55; 79 / 135 | as above | as above |
**Arm B still equals fixed 95 %:** p50 within 1-2 % in all six cells (128 KiB M2 faster: 65 against 79 ms), p99 within the spread (1 MiB M2 542 against 488 ms), no drop trips, swing 1.0x.

## 4. Plainly
- **Improved against 1054f2ff:** the 1 MiB rate-step failure is gone (0 of 12 failed runs against 1 of 5; the slow-phase max is 16 s against 43 s, p90 7.7 s against 12.4 s; the after-step-up max 1.3 s against 43 s); in the static cells S1 p99 (4.0 -> 2.7 s) and drop trips and retransmissions fell by 2-9x (drop trips 7, 22, 12 against 69, 28, 26; retx 242-799 against 987-2,189), S2 p50 and p99 are 17 % and 33 % lower, M1 queue-guard trips halved (12 -> 6) and M2 p99 is lower (803 against 1,046 ms). Against the old build ae5c192a: 128 KiB step-up p50 102 -> 47 ms, recovery 36 -> 24 s.
- **Did not improve:** 1 MiB recovery after the step down (17 s median, range 1-25 s; unchanged from ae5c192a and 1 s-33 s in 1054f2ff); the 1 MiB slow-phase tail is still 7.7 s p90 / 16 s max against CycloneDDS's 3.5 / 4.6 s; S2 retransmissions (2,000-2,900) and p99 (4.5 s) stay well above the fixed rate (580 and 2.7 s); arm A latency in static cells is not equal to the fixed rate (1 MiB M2 506 against 367 ms).
- **Regressed:** **128 KiB recovery after the step up is 24 s median against 8 s for 1054f2ff** (three of five runs at 24-36 s; p90 161 against 76 ms); p50 holds at 47 ms. 1 MiB slow-phase delivery: 5 of 144 samples undelivered in 3 of 12 runs (1054f2ff 9 of 60, ae5c192a 1 of 60), that is a few late samples not a failed run. Nothing else regressed; stall restarts remain 0.
- **Sample sizes:** 12 runs at 1 MiB (a failure rate of 0 of 12 has a 95 % upper bound near 22 %), 5 at 128 KiB (recovery per run is quantised at 4 s), 3 per static cell; one emulated link, one loss pattern, one step pair.
- Not changed: no library, wire, API, default, merge or tag in this stage.
