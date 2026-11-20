# Adaptive rate control, Stage S1 re-run on library 5de6f40b (results, 2026-11-21)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (tree `ba7e7fee` = `origin/ab/adaptive-pacing` `5de6f40b`). Same plan and arms as the v5 and failed runs (`results/ADAPTIVE_S1_v5.md`, `results/ADAPTIVE_S1.md`): `tickle-ad` = `rate_control = ADAPTIVE` (WIFI profile, defaults), control `tickle-lg` in the small cells; Fast DDS 2.14.6 and CycloneDDS 0.10.5 are the Stage A/B runs of the same link and binaries, reused (1 MiB 10 runs, 128 KiB 8, small cells 4). 240 runs, 10 per cell, **0 lost, 0 aborts, 0 snapshot changes**; Pis clean afterwards (no `wtc_*` namespace or tap; `eth0` and `wlan0` addresses unchanged). Raw data `~/rig_results_safe/wtc_adaptive_20261121_v6/D_all`, tables `results/tables_S1v6_*.md`. No library change in this commit.

## 1. Verdict
| cell (WIN / TIE / LOSE / VOID over metric x profile x {CycloneDDS, Fast DDS}) | failed run 323cc282 | v5 (68d4174a) | **v6 (5de6f40b)** |
|---|---|---|---|
| rel_large 1 MiB (24) | 0 / 8 / 6 / 10 | 12 / 7 / 0 / 5 | **8 / 6 / 0 / 10** |
| rel_large 128 KiB (24) | 1 / 8 / 1 / 14 | 5 / 10 / 5 / 4 | **5 / 12 / 1 / 6** |
| be_small + rel_small (64) | 38 / 13 / 0 / 13 | 41 / 13 / 0 / 10 | **40 / 13 / 0 / 11** |
| total (112) | 39 / 29 / 7 / 37 | 58 / 30 / 5 / 19 | **53 / 31 / 1 / 27** |
Fixed 95 % rate (Stage C confirmation) for reference: 65 / 31 / 0 / 16.
**One LOSE in the whole matrix: 128 KiB M1 p99 against Fast DDS (98.1 ms [97.9..106] against 91.85 ms [91.6..92.1]; 1.07x; every Fast DDS run is below every ADAPTIVE run, so the strict rule scores it a LOSE).** The 128 KiB LOSEs of v5 (M1 p99 v both, M2 p50 v both, S1 p99 v Cyclone) are gone: M1 p99 is now a WIN against CycloneDDS (98 against 126 ms), M2 p50 86 ms [57..171] against 99.7 / 106 ms is VOID on A/A (WIN-sized but noisy), S1 p99 323 ms against 439 ms is VOID. 1 MiB: delivery 15/15 in 38 of 40 runs: S2 had two short runs (14 and 11 of 15), M1, M2 and S1 delivered 15/15 in all 30 runs. The VOIDs went up at 1 MiB (10 against 5) because the ADAPTIVE runs on M2 and S2 are noisy (below).
Small cells: 0 LOSE; be_small p50 WIN over both stacks and rel_small p50 WIN over CycloneDDS in all four profiles, as in every earlier stage. The small-cell DDS verdicts rest on 4 DDS runs per profile.

## 2. 128 KiB: p50/p99 (ms, median [min..max] of 10) against the DDS stacks
| cell | tickle-ad | CycloneDDS | Fast DDS | verdicts |
|---|---|---|---|---|
| M1 | 41.9 [41.9..43.7] / 98.1 [97.9..106] | 55.2 / 126 | 91.2 / 91.9 | p50 TIE / WIN; **p99 WIN v Cyclone, LOSE v Fast DDS** |
| M2 | 86.0 [56.8..171] V / 161 [147..315] | 99.7 / 257 | 106 / 119 | p50 VOID; p99 TIE |
| S1 | 279 [278..280] / 323 [283..473] V | 307 / 439 | 3562 / 3848 | p50 WIN v both; p99 VOID |
| S2 | 331 [300..485] / 626 [350..1457] V | 439 / 944 | 2327 / 2749 | p50 TIE / WIN; p99 VOID |
The start-up ramp of v5 (74.6 / 494 ms at M1, 225 / 658 at M2) is fixed by the START-at-cap/16 rule: M1 p50 42 ms (the oracle's 44), p99 98 ms (the oracle's 55 ms, x1.8).

## 3. Oracle gap and datagram ratio (against the Stage C `tickle-p95` confirmation, 10 runs per cell)
| cell | p50 ad v p95 | p99 ad v p95 | tx datagrams ad / p95 | retransmissions ad / p95 | delivered (min of 10) |
|---|---|---|---|---|---|
| 128 KiB M1 | 42 v 44 ms (x0.96) | 98 v 55 (x1.8) | 1540 / 1585 (x0.97) | 17 / 16 | 15 |
| 128 KiB M2 | 86 v 79 (x1.09) | 161 v 135 (x1.2) | 1690 / 1658 (x1.02) | 39 / 44 | 15 |
| 128 KiB S1 | 279 v 292 (x0.95) | 323 v 304 (x1.1) | 1818 / 1560 (x1.17) | 149 / 16 | 15 |
| 128 KiB S2 | 331 v 325 (x1.02) | 626 v 570 (x1.1) | 1959 / 1693 (x1.16) | 274 / 80 | 15 |
| 1 MiB M1 | 318 v 323 (x0.98) | 456 v 334 (x1.4) | 12 015 / 11 156 (x1.08) | 694 / 122 | 15 |
| 1 MiB M2 | 532 v 367 (x1.45) | 1 210 v 488 (x2.5) | 13 142 / 11 396 (x1.15) | 1 892 / 298 | 15 |
| 1 MiB S1 | 2 239 v 2 374 (x0.94) | 3 531 v 2 402 (x1.5) | 13 857 / 10 838 (x1.28) | 720 / 122 | 15 |
| 1 MiB S2 | 3 174 v 2 497 (x1.27) | 6 547 v 2 660 (x2.5) | 15 310 / 11 682 (x1.31) | 3 156 / 580 | 11 |
Against the proposed thresholds (X = 10 % p50, 25 % p99, 1.25x datagrams): **A5 (p50 within 10 %): passes in 6 of 8 cells (128 KiB M1, M2, S1, S2; 1 MiB M1, S1), fails 1 MiB M2 (+45 %) and S2 (+27 %). p99 within 25 % passes in 3 of 8 (128 KiB M2 +20 %, S1 +6 %, S2 +10 %), fails 128 KiB M1 (x1.8), 1 MiB M1 (x1.4), M2 (x2.5), S1 (x1.5), S2 (x2.5).** **A8: datagrams within 1.25x in 6 of 8 cells (128 KiB all four, 1 MiB M1, M2); borderline 1 MiB S1 (x1.28) and S2 (x1.31); retransmissions 4-9x the oracle's at 1 MiB** (the oracle sends almost none). A6: one LOSE (128 KiB M1 p99 v Fast DDS), none at 1 MiB or in the small cells. A7: passes (0 LOSE, no regression against Stage A/C counts).

## 4. Controller state: stall restarts, rate swing, round-trip estimate (RATETRACE and RESULT counters of the publisher)
| cell | stall_restarts (total over 10 runs; runs with >0) | min rate after START (median / lowest run) | swing after START median / max | rtprop median (path 10 / 40 ms) |
|---|---|---|---|---|
| 128 KiB M1 | 0 | 8 000 / 8 000 kbit/s | 8x / 8x | 11.7 ms |
| 128 KiB M2 | 0 | 8 000 / 5 158 | 2x / 4x | 41.4 |
| 128 KiB S1 | 1 (1 run) | 3 682 / 3 037 | 2x / 3x | 78.9 |
| 128 KiB S2 | 0 | 6 640 / 1 435 | 2x / 4x | 128.3 |
| 1 MiB M1 | 0 | 14 313 / 6 483 | 5x / 12x | 11.7 |
| 1 MiB M2 | 0 | 9 341 / 6 891 | 8x / 38x | 43.0 |
| 1 MiB S1 | 6 (3 runs) | 2 394 / 1 011 | 6x / 29x | 16.5 |
| 1 MiB S2 | **22 (6 runs)** | 1 366 / 446 | 13x / **103x** | 56.4 |
False stall restarts are essentially gone on the medium links (0 in 40 runs M1/M2 at both sizes) and 128 KiB (1 in 40 runs); the remaining ones are at 1 MiB on S1 (3 of 10 runs) and S2 (6 of 10), where the link is slowest and one sample takes 2-3 s. **M2 no longer falls to 1 000 kbit/s** (lowest minimum after START 5 158 kbit/s). Swing is 2-8x at 128 KiB (the design's target of 5-10x met) and 5-13x at 1 MiB, with 38x (M2) and 103x (S2) worst runs. Round-trip estimate: within 17 % of the path on M1 and M2, 16.5 ms against 10 ms on S1 at 1 MiB; **at 128 KiB on S1 and S2 the trace's median round trip reads 79 ms and 128 ms against 10 and 40 ms** (the estimate lags upward there: a sample of 1 Mbit does not keep the pipe full, so the per-round measurement is dominated by the app-limited gaps; it did not hurt delivery or p50 in those cells).

## 5. What remains
- **1 MiB S2 is the weak cell:** delivery 11-15 of 15 (2 of 10 runs short), p50 +27 % and p99 x2.5 against the oracle, 22 stall restarts in 6 runs, a 103x swing in the worst run, retransmissions 3 156 against 580. 1 MiB M2 has +45 % p50 and x2.5 p99 (a few runs with a swing up to 38x). These two cells are the ones where the controller still oscillates; both are the bursty-loss profiles (M2, S2) at 1 MiB.
- **128 KiB M1 p99 against Fast DDS** is a 1.07x LOSE (98 against 92 ms): ADAPTIVE's start rate (8 Mbit/s) against the 30 Mbit/s link costs the first sample about 6 ms; the oracle's p99 is 55 ms. This one is a cost of starting below the link rate.
- Compared with v5: 128 KiB fixed (p50 and p99 near the oracle), small cells unchanged, 1 MiB p50 on M1 and S1 as good as or better than the fixed-rate oracle (-2 %, -6 %) and the stall restarts nearly gone; the price is more A/A VOIDs on the noisy M2 and S2 1 MiB cells and two short S2 runs.
- Not changed: no library, wire, API, default, merge or tag in this stage.
## 6. Sample size
10 ADAPTIVE runs per cell against 8-10 DDS runs (4 in the small cells); p99 of 15 samples is the maximum; VOIDs are real (bimodal runs on M2 and S2 at 1 MiB), more reps would show them more clearly rather than remove them.
