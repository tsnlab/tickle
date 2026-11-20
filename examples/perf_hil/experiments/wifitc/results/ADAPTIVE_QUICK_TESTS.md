# Adaptive rate control: the quick tests between S1 and the re-run (1 MiB, M1 M2 S1 S2, 1 run per cell, 2 rounds of copy A)
An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi. Raw data `~/rig_results_safe/wtc_adaptive_quick{2,3,4,5}`. Library builds on `ab/adaptive-pacing`; no library change in this commit.
| test | library | delivered of 15 (8 runs) | notes |
|---|---|---|---|
| S1 full run (323cc282 report) | b9587eaa | 0-2 in all 40 runs | rate collapsed to the 100 kbit/s floor, `rtprop_ns` below the physical round trip |
| quick 2 | 7f9aff89 | 15, 6, 8, 5, 6, 11, 9, 7 | floor/cap swings, `rtprop` still below the path in 4 runs |
| quick 3 | a4db29f4 | 15, 14, 15, 15, 15, 15, 12, 14 | `rtprop` microseconds in 4 runs, rate 250 kbit/s-1 Gbit/s |
| quick 4 | 04cd1473 | 15, 15, 15, 15, 15, 15, 12, 13 | rate swing 429-6536x, p99 8-15 s |
| **quick 5** | **68d4174a** | **15 in all 8** | table below |
Quick 5 (delivered / rate_bps / btlbw_bps / final rtprop ms / p50, p99 ms / tx, retx; after-START rate swing max/min):
| run | delivered | rate_bps | btlbw_bps | rtprop (ms) | p50 / p99 | tx / retx | swing after START |
|---|---:|---:|---:|---:|---|---|---:|
| M1 r1 | 15 | 29.7 M | 29.7 M | 11.9 | 357 / 789 | 12,150 / 774 | 15.7x |
| M1 r2 | 15 | 30.5 M | 30.5 M | 21.2 | 355 / 1,056 | 12,268 / 757 | 22.8x |
| M2 r1 | 15 | 62.2 M | 33.8 M | 42.2 | 496 / 1,254 | 13,006 / 1,766 | 21.9x |
| M2 r2 | 15 | 65.8 M | 38.7 M | 85.4 | 644 / 2,349 | 15,339 / 4,089 | 43.5x |
| S1 r1 | 15 | 3.00 M | 4.00 M | 13.49 | 2,242 / 2,537 | 11,240 / 149 | 1.8x |
| S1 r2 | 15 | 4.04 M | 4.04 M | 13.49 | 2,250 / 2,574 | 13,700 / 155 | 2.2x |
| S2 r1 | 15 | 5.79 M | 5.79 M | 103.3 | 2,684 / 15,770 | 16,853 / 4,848 | 31.5x |
| S2 r2 | 15 | 2.09 M | 2.09 M | 56.3 | 2,843 / 5,355 | 14,146 / 2,417 | 15.9x |
Read: the 15/15 criterion is met in 8 of 8. The p50 is at (S1) or near (M1) the fixed-rate oracle's (S1 2,242-2,250 ms against 2,374; M1 355-357 against 324 ms); tails and the M2/S2 round-trip medians (42-85 ms against the 40 ms path, S2 56-103 ms) and the rate swings (16-44x on M and S2, 2x on S1) are not yet at the design's target of 5-10x. One run per cell: indicative only; the full S1 re-run (10 per cell) follows.
