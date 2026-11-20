# Adaptive rate control, Stage S1 re-run on library 68d4174a (results, 2026-11-21)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (tree `46d33347` = `origin/ab/adaptive-pacing` `68d4174a`). Same plan and arms as the failed run of `323cc282` (`results/ADAPTIVE_S1.md`): `tickle-ad` = `rate_control = ADAPTIVE` (WIFI profile, defaults), control `tickle-lg` in the small cells; Fast DDS 2.14.6 and CycloneDDS 0.10.5 are the Stage A/B runs of the same link, binaries and command lines (reused; 1 MiB 10 runs, 128 KiB 8, small cells 4). 240 runs, 10 per cell, **0 lost, 0 aborts, 0 snapshot changes**; Pis clean afterwards (no `wtc_*` namespace or tap; `eth0` qdisc and addresses, `wlan0` addresses unchanged; `sch_netem` was already loaded). Raw data `~/rig_results_safe/wtc_adaptive_20261120_v5/D_all`, tables `results/tables_S1v5_*.md`. Quick tests 2-5 that led here: `results/ADAPTIVE_QUICK_TESTS.md`. No library change in this commit.

## 1. Verdict: ADAPTIVE now delivers everything and wins or ties 1 MiB, but loses 5 of 24 verdicts at 128 KiB
| cell (WIN / TIE / LOSE / VOID over metric x profile x {CycloneDDS, Fast DDS}) | failed run (323cc282, b9587eaa) | **this run (68d4174a)** |
|---|---|---|
| rel_large 1 MiB (24) | 0 / 8 / 6 / 10 | **12 / 7 / 0 / 5** |
| rel_large 128 KiB (24) | 1 / 8 / 1 / 14 | **5 / 10 / 5 / 4** |
| be_small + rel_small (64) | 38 / 13 / 0 / 13 | **41 / 13 / 0 / 10** |
| total (112) | 39 / 29 / 7 / 37 | **58 / 30 / 5 / 19** |
For comparison, the fixed 95 % rate (Stage C confirmation): 65 / 31 / 0 / 16. **No cell is lost at 1 MiB or in the small cells; the 5 LOSEs are all 128 KiB** (below). Delivery: 15/15 in 79 of 80 large-sample runs (one 1 MiB S2 run delivered 14).
A/A and sample size: 10 ADAPTIVE runs per cell against 8-10 DDS runs (large cells) or 4 (small cells; those verdicts rest on 4 DDS runs); p99 of 15 samples is the maximum.

## 2. Where it loses, and why
128 KiB, p50 and p99 (ms, median [min..max] of 10 runs):
| cell | tickle-ad p50 / p99 | CycloneDDS | Fast DDS | verdicts |
|---|---|---|---|---|
| M1 | 74.6 [52..164] V / 494 [485..557] | 55.2 / 126 | 91.2 / 91.9 | p50 VOID, **p99 LOSE v both** |
| M2 | 225 [143..473] / 658 V | 99.7 / 257 | 106 / 119 | **p50 LOSE v both**, p99 VOID |
| S1 | 280 [280..286] / 579 | 307 / 439 | 3562 / 3848 | p50 WIN v both, **p99 LOSE v CycloneDDS** (WIN v Fast DDS) |
| S2 | 488 [326..553] / 698 | 439 / 944 | 2327 / 2749 | p50 TIE v Cyclone, p99 TIE; WIN v Fast DDS |
Cause, from the RATETRACE counters (inference, not a pcap proof): the controller starts every run at its floor (1 Mbit/s) and ramps by about +1 Mbit/s per round trip. A 128 KiB sample is 1 Mbit: at 1 Mbit/s it takes about 1 s, so the first samples go out slowly: on M1 the estimate needs about 4.0 s to reach 15 Mbit/s (all three runs inspected: first rates 1000, 1000, 1999, 2994, 5180, 8125 kbit/s), on M2 (40 ms round trip, one round per 40 ms) 8-28 s, and in 2 of 3 inspected M2 runs the rate falls back to 1000 kbit/s in round 6 and needs 28 s to recover. With 15 samples at 0.25 Hz (60 s) the early samples set the median on M2 (225 against 99.7 ms) and the maximum (p99) everywhere. The fixed 95 % arm has the target rate from the first datagram: p50 44 / 79 / 292 / 325 ms and p99 55 / 135 / 304 / 570 ms (M1 / M2 / S1 / S2). Large samples (1 MiB at 4-30 Mbit/s = 0.3-2.2 s) amortise the ramp; small and 128 KiB samples do not.

## 3. Oracle gap and datagram ratio (A5, A8; against the Stage C `tickle-p95` confirmation, 10 runs per cell)
| cell | p50 ad v p95 | p99 ad v p95 | tx datagrams ad / p95 | retransmissions ad / p95 | delivered |
|---|---|---|---|---|---|
| 128 KiB M1 | 75 v 44 ms (x1.70) | 494 v 55 (x9.0) | 1487 / 1585 (x0.94) | 16 / 16 | 15/15 |
| 128 KiB M2 | 225 v 79 (x2.85) | 658 v 135 (x4.9) | 1744 / 1658 (x1.05) | 34 / 44 | 15/15 |
| 128 KiB S1 | 280 v 292 (x0.96) | 579 v 304 (x1.9) | 1815 / 1560 (x1.16) | 18 / 16 | 15/15 |
| 128 KiB S2 | 488 v 325 (x1.50) | 698 v 570 (x1.2) | 1829 / 1693 (x1.08) | 92 / 80 | 15/15 |
| 1 MiB M1 | 363 v 323 (x1.12) | 792 v 334 (x2.4) | 13 658 / 11 156 (x1.22) | 2 797 / 122 | 15/15 |
| 1 MiB M2 | 520 v 367 (x1.42) | 1 640 v 488 (x3.4) | 17 006 / 11 396 (x1.49) | 5 748 / 298 | 15/15 |
| 1 MiB S1 | 2 248 v 2 374 (x0.95) | 2 569 v 2 402 (x1.1) | 13 660 / 10 838 (x1.26) | 126 / 122 | 15/15 |
| 1 MiB S2 | 2 625 v 2 497 (x1.05) | 4 266 v 2 660 (x1.6) | 16 552 / 11 682 (x1.42) | 4 870 / 580 | 14-15 of 15 |
Against the proposed thresholds (X = 10 % on p50, 25 % on p99; A8 1.25x datagrams): **A5 passes only at 1 MiB S1 (p50 -5 %, p99 +10 %) and 128 KiB S1 p50 (-4 %); fails elsewhere** (p50 +5 % to +185 %, p99 +10 % to +800 %). **A8: 128 KiB all within 1.25x; 1 MiB S1 x1.26 (borderline), M1 x1.22, M2 x1.49, S2 x1.42 (retransmissions 8-23x the oracle's at M1, M2 and S2).** A6 (no LOSE) fails at 128 KiB (5 LOSEs), passes at 1 MiB and in the small cells. A7 (small cells) passes: 0 LOSE, no regression against Stage A/C counts.

## 4. Rate swing and round-trip estimate (RATETRACE of the publisher, 10 runs per cell; swing = max/min of the per-round rate after START)
| cell | rtprop median of medians (path 10 / 40 ms) | swing after START, median / max |
|---|---|---|
| 128 KiB M1 | 11.6 ms | 21x / 61x |
| 128 KiB M2 | 41.3 | 21x / 25x |
| 128 KiB S1 | 22.0 | 2x / 3x |
| 128 KiB S2 | 50.6 | 7x / 8x |
| 1 MiB M1 | 11.7 | 46x / 78x |
| 1 MiB M2 | 42.4 | 46x / 167x |
| 1 MiB S1 | 13.5 | 3x / 8x |
| 1 MiB S2 | 61.2 | 30x / 43x |
The round-trip estimate is now sane (path to +50 %; S1 13.5 ms against 10 ms, S2 61 ms against 40 ms are consistently high, M1 and M2 within 17 % and 6 %); the failure of 323cc282 (microsecond `rtprop`) is gone. The rate still swings 20-170x on the medium links (M1, M2) and 30-43x on S2 over a run, above the design's 5-10x target; the swing, not the estimate, is what produces the retransmissions at 1 MiB (2 800-5 700 on M against 122-298 for the fixed rate) and the 1 MiB M2 p99 (1 640 ms against 488).

## 5. Against the failed run (323cc282, b9587eaa) and the fixed-rate oracle
- Failed run: 1 MiB delivered 0-2 of 15 in 40 of 40 runs, rate at the 100 kbit/s floor with `btlbw` 0, `rtprop` microseconds. Now: 79 of 80 runs deliver 15/15, no run at the floor, `rtprop` within 6-50 % of the path.
- Against the fixed 95 % rate: 1 MiB p50 +12 % M1, +42 % M2, -5 % S1, +5 % S2 (a WIN or TIE against both DDS stacks everywhere except S2 p50 v CycloneDDS, which is VOID because CycloneDDS's own runs spread); the datagram count is 1.22-1.49x at 1 MiB.
- Against the DDS stacks, 1 MiB: p50 WIN against both on M1, M2, S1 (and S2 against Fast DDS); delivery TIE (CycloneDDS) or WIN (Fast DDS at 4 Mbit/s: 53-60 %).

## 6. Honest verdict and what remains
- **ADAPTIVE is now usable and beats or ties both DDS stacks on 1 MiB and the small cells; it does not beat CycloneDDS and Fast DDS on 128 KiB at M1 and M2**, where its start-up ramp from 1 Mbit/s dominates the p50 (M2) and the tail (M1, S1 p99). At 1 MiB it is an improvement over the shipped default of Stage A/B (which lost to CycloneDDS there) and at 128 KiB it is not yet at the fixed-rate oracle.
- Acceptance (plan A4-A8): A6 FAIL (5 LOSE at 128 KiB); A5 FAIL except S1; A7 PASS; A8 fails on the retransmission count at M1/M2/S2 1 MiB. A4 (rate step) and the reader cells were not part of this run.
- Levers for the implementer (not done here): a higher or remembered start rate (carry the estimate across samples and idle periods; start from the last good estimate or a quarter of it), damping the swing (limit the per-round change), and the fall-back-to-floor in round 6 seen on M2.
- Not changed: no library, wire, API, default, merge or tag.
