# Adaptive rate control, trimmed full run v8 on library 6b05d93c (results, 2026-11-22)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (tree `220f8e4f` = `origin/ab/adaptive-next` `6b05d93c`). Arm A = `tickle-ad` (`rate_control = ADAPTIVE`, no prior, WIFI profile defaults), control `tickle-lg` in the small cells, scored against the Stage A/B Fast DDS 2.14.6 and CycloneDDS 0.10.5 runs of the same link and binaries (reused: 1 MiB 10 runs, 128 KiB 8, small cells 4). Arm A: 240 runs, 10 per cell, directly comparable to v7. Reduced arm C (the wrong prior: `tickle-ad-c30`, a 30 Mbit/s cap on S1 and S2, 1 MiB and 128 KiB, 10 runs, **not scored**) and arm B on M2 only (`tickle-ad-c30` = the right prior, 10 runs). 300 runs in all, **0 lost, 0 aborts, 0 snapshot changes**, seven clean session ends; Pis clean (no `wtc_*` namespace or tap, `eth0` and `wlan0` addresses unchanged). Raw data `~/rig_results_safe/wtc_adaptive_20261122_v8`, tables `results/tables_S1v8_*.md`. No library change in this commit.

## 1. Arm A scores (WIN / TIE / LOSE / VOID over metric x profile x {CycloneDDS, Fast DDS})
| cell | v5 | v6 | v7 | **v8 (A)** | fixed 95 % |
|---|---|---|---|---|---|
| 1 MiB (24) | 12 / 7 / 0 / 5 | 8 / 6 / 0 / 10 | 5 / 10 / 0 / 9 | **9 / 9 / 0 / 6** | 15 / 6 / 0 / 3 |
| 128 KiB (24) | 5 / 10 / 5 / 4 | 5 / 12 / 1 / 6 | 5 / 14 / 1 / 4 | **6 / 11 / 1 / 6** | 10 / 12 / 0 / 2 |
| be_small + rel_small (64) | 41 / 13 / 0 / 10 | 40 / 13 / 0 / 11 | 40 / 13 / 0 / 11 | **39 / 12 / 0 / 13** | 40 / 13 / 0 / 11 |
| total (112) | 58 / 30 / 5 / 19 | 53 / 31 / 1 / 27 | 50 / 37 / 1 / 24 | **54 / 32 / 1 / 25** | 65 / 31 / 0 / 16 |
**One LOSE, as in v6 and v7: 128 KiB M1 p99 against Fast DDS (101 ms [97.7..111] against 91.85 ms [91.6..92.1], 1.10x).** None at 1 MiB or in the small cells. Delivery: 15/15 in 78 of 80 large-sample runs (two 1 MiB S2 runs delivered 14).

## 2. p50 / p99 (ms, median [min..max] of 10) against the DDS stacks
| cell | tickle-ad | CycloneDDS | Fast DDS | verdicts |
|---|---|---|---|---|
| 128 KiB M1 | 51.9 [42..61] / 101 [98..111] | 55.2 / 126 | 91.2 / 91.9 | p50 TIE / WIN; p99 WIN v Cyclone, **LOSE v Fast DDS** |
| 128 KiB M2 | 98.6 [79..205] V / 261 [152..596] V | 99.7 / 257 | 106 / 119 | VOID / VOID |
| 128 KiB S1 | 281 [281..284] / 321 [289..610] | 307 / 439 | 3562 / 3848 | p50 WIN / WIN; p99 TIE / WIN |
| 128 KiB S2 | 400 [317..1083] / 738 [531..3209] V | 439 / 944 | 2327 / 2749 | p50 TIE / WIN; p99 VOID |
| 1 MiB M1 | 342 [324..391] / 535 [488..1498] | 818 / 919 | 1082 / 1101 | p50 WIN / WIN; p99 TIE / TIE |
| 1 MiB M2 | 528 [424..773] / 1147 [805..1950] V | 866 / 1313 | 1220 / 1272 | p50 WIN / WIN; p99 VOID |
| 1 MiB S1 | 2263 [2258..2273] / 2447 [2273..4977] | 2514 / 2662 | 31069 / 32906 (8-9 of 15) | p50 WIN / WIN; p99 TIE / WIN |
| 1 MiB S2 | 2797 [2354..3308] V / 5944 [4144..12007] V | 3803 V / 4658 | 29326 / 31829 (8-9 of 15) | VOID; delivery WIN v Fast DDS |

## 3. Oracle gap and datagram ratio (A5-A8), against Stage C `tickle-p95` (10 runs per cell)
| cell | p50 ad v p95 | p99 ad v p95 | tx ad / p95 | retx ad / p95 | delivered (min) | stall restarts | swing med / max | rtprop (path) | A5 p50 <=10 % | A8 tx <=1.25x |
|---|---|---|---|---|---|---|---|---|---|---|
| 128 KiB M1 | 52 v 44 (x1.19) | 101 v 55 (x1.8) | 1562 / 1585 (x0.99) | 18 / 16 | 15 | 0 | 3x / 4x | 11.0 ms (10) | no | yes |
| 128 KiB M2 | 99 v 79 (x1.25) | 261 v 135 (x1.9) | 1714 / 1658 (x1.03) | 43 / 44 | 15 | 0 | 2x / 11x | 40.8 (40) | no | yes |
| 128 KiB S1 | 281 v 292 (x0.96) | 321 v 304 (x1.1) | 1943 / 1560 (x1.25) | 41 / 16 | 15 | 0 | 2x / 8x | 11.6 (10) | yes | borderline |
| 128 KiB S2 | 400 v 325 (x1.23) | 738 v 570 (x1.3) | 1878 / 1693 (x1.11) | 106 / 80 | 15 | 7 (1 run) | 5x / 21x | 41.7 (40) | no | yes |
| 1 MiB M1 | 342 v 323 (x1.06) | 535 v 334 (x1.6) | 12 350 / 11 156 (x1.11) | 820 / 122 | 15 | 7 (1 run) | 14x / 30x | 10.7 (10) | yes | yes |
| 1 MiB M2 | 528 v 367 (x1.44) | 1 147 v 488 (x2.3) | 11 956 / 11 396 (x1.05) | 650 / 298 | 15 | 0 | 12x / 25x | 40.9 (40) | no | yes |
| 1 MiB S1 | 2 263 v 2 374 (x0.95) | 2 447 v 2 402 (x1.0) | 14 178 / 10 838 (x1.31) | 372 / 122 | 15 | 0 | 5x / 13x | 11.8 (10) | yes | **no** |
| 1 MiB S2 | 2 797 v 2 497 (x1.12) | 5 944 v 2 660 (x2.2) | 14 044 / 11 682 (x1.20) | 2 118 / 580 | **14** (2 runs) | 0 | 17x / **63x** | 41.7 (40) | borderline | yes |
**A5 (p50 within 10 %): 3 of 8 cells (128 KiB S1; 1 MiB M1, S1), 1 MiB S2 borderline (+12 %), 128 KiB M1 (+19 %), M2 (+25 %), S2 (+23 %) and 1 MiB M2 (+44 %) fail (v7: 5 of 8). p99 within 25 %: 2 of 8 (128 KiB S1 x1.1, 1 MiB S1 x1.0; v7 none).** A6: one LOSE. A7: passes (small cells 39 / 12 / 0 / 13). A8 (datagrams within 1.25x): 7 of 8, fails 1 MiB S1 (x1.31); retransmissions 2-7x the oracle's at 1 MiB M1 / M2 / S2.

## 4. v8 against v7
- Score 54 / 32 / 1 / 25 against 50 / 37 / 1 / 24; the four extra WINs are 1 MiB (M1 and M2 p50 now WIN against both stacks, S1 p99 TIE / WIN).
- Stall restarts at 1 MiB: 7 in 1 run (M1) and 0 on M2 / S1 / S2 against 26 in 11 runs (v7: S1 7, S2 19); 128 KiB S2 has 7 in one run. The false-restart problem is fixed (the stall rule now needs silence).
- 1 MiB S2: delivery 15/15 in 8 of 10 runs (v7 9 of 10) and 14/15 in two, swing median 17x / max 63x (v7 20x / 84x), p99 5.9 s (v7 6.0 s), retransmissions 2 118 (v7 2 390): little change; it remains the weak cell.
- 128 KiB: p50 now x1.19-1.25 of the oracle on M1, M2, S2 (v7 x1.00, 1.31, 1.07): the new START (a half of a cap that defaults to 8 Mbit/s without prior) is no better than v7's at 128 KiB M1 and S2, slightly better on M2.
- No new LOSE; A5 reaches 3 of 8 (v7 5 of 8) because 128 KiB M1 and S2 p50 moved from within 10 % to +19-23 %; p99 within 25 % improved (2 of 8 against 0).

## 5. Reduced arm C (wrong prior, 30 Mbit/s cap on the 4 Mbit/s links), unscored, against arm A
| cell | C delivered (min of 10) / p50 / p99 (ms) | A delivered / p50 / p99 | C swing max | C stall |
|---|---|---|---|---|
| 128 KiB S1 | 15 / 281 / 341 [311..864] | 15 / 281 / 321 | 30x | 0 |
| 128 KiB S2 | 15 / 341 / 641 [401..4211] | 15 / 400 / 738 | 19x | 0 |
| 1 MiB S1 | **11** (3 runs short) / 2 276 [2266..10569] / **9 443** [3997..24874] | 15 / 2 263 / 2 447 | 159x | 0 |
| 1 MiB S2 | **4** (3 runs short) / 3 037 [2467..51117] / **6 710** [3943..55023] | 14 / 2 797 / 5 944 | 181x | 1 |
**Safety bound: a cap 7.5x above the link costs nothing at 128 KiB but breaks 1 MiB on slow links: 3 of 10 runs short on S1 (delivery down to 11 of 15, p99 to 25 s) and 3 of 10 on S2 (down to 4 of 15, one run at 55 s p99), swings up to 181x.** Arm A without a prior delivers 15/15 (14/15 twice). A wrong prior is worse than no prior at 1 MiB on slow links (confirms quick test 9).

## 6. Arm B (right prior, 30 Mbit/s cap on the 30 Mbit/s link) on M2, against arm A and the oracle
| cell | B delivered / p50 / p99 (ms) | A | oracle (Stage C p95) | B stall / swing med / max |
|---|---|---|---|---|
| 128 KiB M2 | 15 / 165 [118..220] / 283 [168..487] | 15 / 99 / 261 | 79 / 135 | 0 / 7x / 14x |
| 1 MiB M2 | 15 / 523 [417..1605] / 1 114 [639..4137] | 15 / 528 / 1 147 | 367 / 488 | **26 in 2 runs** / 6x / 23x |
**B does not repair M2: at 128 KiB it is worse than A (p50 165 against 99 ms, oracle 79); at 1 MiB it equals A (523 against 528 ms; p99 1.1 s against 1.15 s; the oracle is 367 / 488 ms) and it adds stall restarts in two runs.** The M2 weakness (40 ms round trip with bursty loss) is not about the prior.

## 7. Remaining weak cells and the likely cause (inferences from the counters)
1. **1 MiB S2:** p99 x2.2, swing up to 63x, two runs of 14/15, retransmissions 3.6x the oracle's; slow link, 2-3 s samples, bursty loss that looks like congestion, so the rate goes down to 260-870 kbit/s and probes back up.
2. **1 MiB M2:** p50 x1.44, p99 x2.3, swing 12-25x; the 40 ms loop makes each probe above the link rate cost a round of drops before the controller sees it; a prior does not help (arm B).
3. **128 KiB M1 and M2:** p99 101 ms against Fast DDS 91.9 ms (1.10x, the one LOSE) and p50 x1.19-1.25: the early samples run below the link rate (START at 8 Mbit/s without a prior; a fixed rate is at 95 % from the first datagram).
4. **1 MiB S1:** datagrams x1.31 (retransmissions 3x), the rare slow round triggers repair bursts.

## 8. Recommendation
**Keep fixed pacing (95 % of the carried rate) as the 2.2.0 Wi-Fi option; offer ADAPTIVE, if at all, as an experimental opt-in (off by default, no prior, with the wrong-prior hazard documented).** Arm A is now stable (0 LOSE except one 1.10x p99, 15/15 delivery in 78 of 80 runs, stall restarts fixed) and clearly better than the shipped unpaced default at 1 MiB, but it trails the fixed rate in every large-sample cell's tails (A5 3 of 8, p99 x1.6-2.3 at M1/M2/S2), a prior does not close M2, a wrong prior makes slow links worse than no prior, and it has not seen a real Wi-Fi link, the rate-step or the multi-reader stages. Not changed: no library, wire, API, default, merge or tag in this stage.
