# Adaptive rate control, Stage S1 re-run on library 7c6e4fe3 (results, 2026-11-21)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (tree `85d5dc26` = `origin/ab/adaptive-next` `7c6e4fe3`). Same plan and arms as v5/v6: `tickle-ad` = `rate_control = ADAPTIVE` (WIFI profile, defaults), control `tickle-lg` in the small cells; Fast DDS 2.14.6 and CycloneDDS 0.10.5 are the Stage A/B runs of the same link and binaries, reused (1 MiB 10 runs, 128 KiB 8, small cells 4). 240 runs, 10 per cell, **0 lost, 0 aborts, 0 snapshot changes**; Pis clean (no `wtc_*` namespace or tap; `eth0` and `wlan0` addresses unchanged). Raw data `~/rig_results_safe/wtc_adaptive_20261121_v7/D_all`, tables `results/tables_S1v7_*.md`. No library change in this commit.

## 1. Scores (WIN / TIE / LOSE / VOID over metric x profile x {CycloneDDS, Fast DDS})
| cell | failed 323cc282 | v5 | v6 | **v7 (7c6e4fe3)** | fixed 95 % (Stage C) |
|---|---|---|---|---|---|
| 1 MiB (24) | 0 / 8 / 6 / 10 | 12 / 7 / 0 / 5 | 8 / 6 / 0 / 10 | **5 / 10 / 0 / 9** | 15 / 6 / 0 / 3 |
| 128 KiB (24) | 1 / 8 / 1 / 14 | 5 / 10 / 5 / 4 | 5 / 12 / 1 / 6 | **5 / 14 / 1 / 4** | 10 / 12 / 0 / 2 |
| be_small + rel_small (64) | 38 / 13 / 0 / 13 | 41 / 13 / 0 / 10 | 40 / 13 / 0 / 11 | **40 / 13 / 0 / 11** | 40 / 13 / 0 / 11 |
| total (112) | 39 / 29 / 7 / 37 | 58 / 30 / 5 / 19 | 53 / 31 / 1 / 27 | **50 / 37 / 1 / 24** | 65 / 31 / 0 / 16 |
**One LOSE, the same as v6: 128 KiB M1 p99 against Fast DDS (98.4 ms [98.0..159] against 91.85 ms [91.6..92.1], 1.07x).** No LOSE at 1 MiB or in the small cells. Delivery: 15/15 in 79 of 80 large-sample runs (one 1 MiB S2 run delivered 9 of 15). Fewer WINs than v6 (50 against 53) because three 1 MiB cells moved from WIN to TIE or VOID (M1 p50 and p99 A/A VOID, M2 p50 TIE against Cyclone); the p50 values themselves are as good (M1 320 ms).
Sample size: 10 ADAPTIVE runs per cell against 8-10 DDS runs (4 in the small cells; those verdicts rest on 4); p99 of 15 samples is the maximum.

## 2. p50 / p99 (ms, median [min..max] of 10) against the DDS stacks
| cell | tickle-ad | CycloneDDS | Fast DDS | verdicts |
|---|---|---|---|---|
| 128 KiB M1 | 43.9 [42..60] / 98.4 [98..159] | 55.2 / 126 | 91.2 / 91.9 | p50 TIE / WIN; p99 TIE v Cyclone, **LOSE v Fast DDS** |
| 128 KiB M2 | 104 [77..138] V / 171 [132..439] | 99.7 / 257 | 106 / 119 | p50 VOID; p99 TIE |
| 128 KiB S1 | 281 [281..284] / 544 [289..1007] | 307 / 439 | 3562 / 3848 | p50 WIN v both; p99 TIE v Cyclone, WIN v Fast DDS |
| 128 KiB S2 | 349 [324..595] / 765 [475..1280] V | 439 / 944 | 2327 / 2749 | p50 TIE / WIN; p99 VOID |
| 1 MiB M1 | 320 [304..371] V / 492 [432..650] V | 818 / 919 | 1082 / 1101 | VOID (A/A; the medians are 2.5-3.4x faster than both stacks) |
| 1 MiB M2 | 510 [459..1140] / 1034 [630..1980] | 866 / 1313 | 1220 / 1272 | p50 TIE / WIN; p99 TIE |
| 1 MiB S1 | 2265 [2260..2279] / 2592 [2284..6260] V | 2514 / 2662 | 31069 / 32906 (8-9 of 15) | p50 WIN v both; p99 VOID; delivery WIN v Fast DDS |
| 1 MiB S2 | 2780 [2419..9049] / 6043 [3922..17168] V | 3803 V / 4658 | 29326 / 31829 (8-9 of 15) | p50 VOID v Cyclone, WIN v Fast DDS; p99 VOID; delivery 60 % in one run |

## 3. Oracle gap and datagram ratio (A5, A7, A8), against Stage C `tickle-p95` (10 runs per cell)
| cell | p50 ad v p95 | p99 ad v p95 | tx datagrams ad / p95 | retx ad / p95 | delivered (min of 10) | A5 p50 <=10 % | A8 tx <=1.25x |
|---|---|---|---|---|---|---|---|
| 128 KiB M1 | 44 v 44 (x1.00) | 98 v 55 (x1.8) | 1631 / 1585 (x1.03) | 18 / 16 | 15 | yes | yes |
| 128 KiB M2 | 104 v 79 (x1.31) | 171 v 135 (x1.3) | 1678 / 1658 (x1.01) | 36 / 44 | 15 | no | yes |
| 128 KiB S1 | 281 v 292 (x0.96) | 544 v 304 (x1.8) | 1946 / 1560 (x1.25) | 40 / 16 | 15 | yes | borderline (1.25) |
| 128 KiB S2 | 349 v 325 (x1.07) | 765 v 570 (x1.3) | 1864 / 1693 (x1.10) | 102 / 80 | 15 | yes | yes |
| 1 MiB M1 | 320 v 323 (x0.99) | 492 v 334 (x1.5) | 12 086 / 11 156 (x1.08) | 610 / 122 | 15 | yes | yes |
| 1 MiB M2 | 510 v 367 (x1.39) | 1 034 v 488 (x2.1) | 11 716 / 11 396 (x1.03) | 416 / 298 | 15 | no | yes |
| 1 MiB S1 | 2 265 v 2 374 (x0.95) | 2 592 v 2 402 (x1.1) | 14 272 / 10 838 (x1.32) | 455 / 122 | 15 | yes | **no** |
| 1 MiB S2 | 2 780 v 2 497 (x1.11) | 6 043 v 2 660 (x2.3) | 14 564 / 11 682 (x1.25) | 2 390 / 580 | **9** | borderline (+11 %) | borderline (1.247) |
A5 (p50 within 10 %): 5 of 8 cells clearly (128 KiB M1, S1, S2; 1 MiB M1, S1), 1 MiB S2 borderline (+11 %), 128 KiB M2 (+31 %) and 1 MiB M2 (+39 %) fail. p99 within 25 %: none of the eight (x1.1 to x2.3; four within x1.5). A6: one LOSE (128 KiB M1 p99 v Fast DDS), none at 1 MiB or in the small cells. A7: passes (small cells 40 / 13 / 0 / 11, identical to the fixed rate). A8 (datagrams within 1.25x): 7 of 8; fails at 1 MiB S1 (x1.32); retransmissions are 2-5x the oracle's at 1 MiB (610 / 416 / 455 / 2 390 against 122 / 298 / 122 / 580).

## 4. Controller state (RATETRACE and RESULT counters of the publisher, 10 runs per cell)
| cell | stall_restarts (total; runs with >0) | min rate after START (median / lowest, kbit/s) | swing after START median / max | rtprop median (path 10 / 40 ms) |
|---|---|---|---|---|
| 128 KiB M1 | 2 (1 run) | 16 908 / 5 969 | 3x / 6x | 10.7 ms |
| 128 KiB M2 | 0 | 8 000 / 3 818 | 2x / 3x | 41.0 |
| 128 KiB S1 | 0 | 1 533 / 711 | 5x / 11x | 11.7 |
| 128 KiB S2 | 0 | 1 629 / 762 | 4x / 8x | 41.7 |
| 1 MiB M1 | 0 | 7 071 / 2 859 | 11x / 21x | 11.1 |
| 1 MiB M2 | 0 | 6 404 / 2 319 | 9x / 21x | 40.8 |
| 1 MiB S1 | 7 (4 runs) | 1 932 / 927 | 6x / 30x | 11.8 |
| 1 MiB S2 | **19 (7 runs)** | 772 / 460 | 20x / **84x** | 41.8 |
Round-trip estimate: now within 10-17 % of the path everywhere (v6: 79 and 128 ms at 128 KiB S1/S2; v5 and earlier: microseconds). Swing at 128 KiB 2-5x (target 5-10x met); at 1 MiB 6-20x with maxima of 21-84x. Compared with v6: stall restarts at 1 MiB S1 7 (v6 6), S2 19 (v6 22); 128 KiB M1 now has 2 (v6 0).

## 5. Remaining weak cells and the likely cause
1. **1 MiB S2 (4 Mbit/s, 40 ms round trip, bursty loss): p99 6.0 s (x2.3 of the oracle's), delivery 9-15 of 15 (one run 60 %), 19 stall restarts in 7 of 10 runs, swing up to 84x, retransmissions 4x the oracle's.** Cause (inference from the counters): a sample takes 2-3 s there and the Gilbert-Elliott bursts (2 % entry, 70 % loss in the bad state) look to the controller like a delivery stall; it restarts START (lowest rates 460-772 kbit/s), re-probes upward and overshoots. The fixed rate does not react to loss and avoids this entirely (p99 2.66 s).
2. **1 MiB S1 (4 Mbit/s, 10 ms, 1 % loss): 4 of 10 runs restart (7 in all), tx x1.32, swing up to 30x.** The same mechanism on a lighter loss pattern: a rare 2-3 s delivery gap with random loss is read as a stall.
3. **1 MiB M2 (30 Mbit/s, 40 ms, bursty): p50 x1.39 and p99 x2.1 of the oracle's** (swing 9-21x): each probe above the link rate on a 40 ms loop costs a queue overflow (retransmissions 416 against 298 are modest); the cost is in the tail and the probing rhythm, not in volume.
4. **128 KiB M2 p50 x1.31 (104 against 79 ms) and M1 p99 98 ms against Fast DDS's 92 ms (1.07x LOSE):** the START rate (8 Mbit/s, 1/16 of the cap clamped) against a 30 Mbit/s link; at 128 KiB the first samples set the p99 (the maximum of 15) and, at M2's 40 ms loop, the p50. The fixed rate sends at 95 % of the link from datagram one (p99 55 ms).
No cell fails on delivery except one 1 MiB S2 run; no cell is a LOSE except the M1 128 KiB p99 by 1.07x.

## 6. Recommendation
- **Ship fixed pacing as the documented Wi-Fi option in 2.2.0; do not make ADAPTIVE a headline or a default.** The measured guideline (`pacing_rate` = 95 % of the carried rate) has 0 LOSE in 112 verdicts (65 / 31 / 0 / 16); ADAPTIVE after eight iterations has 1 LOSE, 24 VOID and a visible oscillation in the cells with bursty loss at 1 MiB, and it has never been run on a real Wi-Fi link (the emulation has a fixed rate, no MAC retries, one flow). Its value is that it needs no calibration, which the emulation cannot show it needs.
- If the user wants ADAPTIVE in 2.2.0 at all: **opt-in and clearly marked experimental**, off by default (the OFF path is wire-identical to the parent per the implementer; A1/A2/A3 were checked there, not here), with this report's limits in the documentation (1 MiB on slow bursty links, start-up on 128 KiB).
- **Fix more before a default:** (a) separate "loss burst" from "stall" (a 2-3 s sample with bursty loss on a 4 Mbit/s link restarts START 19 times in 7 runs); (b) damp the probe overshoot (swing 20-84x on S2, 9-21x on M2); (c) start nearer the link rate when the caller's `link_rate` is known (128 KiB p99). A real-Wi-Fi test (not available here) and the rate-step and multi-reader stages (A4, A11) are not done.
- Not changed: no library, wire, API, default, merge or tag in this stage.
