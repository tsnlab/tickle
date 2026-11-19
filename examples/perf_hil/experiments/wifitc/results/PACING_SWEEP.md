# Stage C: does TickLE with the documented pacing options beat both DDS stacks on the emulated link? (2026-11-19)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Plan: `WIFI_TC_PLAN.md` section 10 and 10.1 (committed before the sweep and before the confirmation). No library, wire, API or default change: only `tt_LinkOptions.pacing_rate` (and one `pacing_burst` arm) on the WIFI profile, as DESIGN.md "Wi-Fi tuning" documents. Branch `ab/wifi-tc`. Link profiles M1 M2 S1 S2 and their exact netem arguments are as in `WIFI_TC_RESULTS_2026-11-19.md` section 2 (carried 29.73 / 3.97 Mbit/s); every table below names them.

## 1. Answer
**With `pacing_rate` = 95 % of the carried rate TickLE has no LOSE against either DDS stack in any cell (0 of 112 verdicts), but it does not WIN every cell: 65 WIN, 31 TIE, 16 VOID.** The shipped unpaced default (Stage A/B) had 7 LOSE. Every large-sample p50 is a WIN or TIE against both stacks except one VOID (1 MiB S2 against CycloneDDS, whose own runs are noisy); every 1 MiB p50 against Fast DDS is a WIN. The remaining non-WINs are of three kinds that pacing cannot turn into wins (section 5): delivery ties at 100 % on both sides, p99 VOIDs/TIEs caused by rare tail outliers, and 128 KiB p50 TIEs where CycloneDDS's fastest runs already sit at the link's serialization floor.

## 2. Phase 1: the sweep (tuning data, 288 runs, 0 lost; not scored)
Arms `tickle-p75/-p85/-p90/-p95` (`pacing_rate` = that percent of the carried rate: M 22.30 / 25.27 / 26.76 / 28.24 Mbit/s, S 2.98 / 3.37 / 3.57 / 3.77), default burst, plus `tickle-p90-b24608` (16-frame burst) at 1 MiB; 8 runs per arm, profile and size. All arms delivered 15/15. Median p50 / p99 (ms):
| cell | p75 | p85 | p90 | p95 | p90 + 24 608 B burst |
|---|---:|---:|---:|---:|---:|
| 128 KiB M1 | 54 / 64 | 48 / 58 | 46 / 56 | **44 / 54** | - |
| 128 KiB M2 | 77 / 148 | 83 / 176 | 74 / 100 | **69 / 107** | - |
| 128 KiB S1 | 368 / 381 | 326 / 336 | 308 / 324 | **293 / 302** | - |
| 128 KiB S2 | 402 / 537 | 361 / 501 | 335 / 463 | **321 / 503** | - |
| 1 MiB M1 | 408 / 417 | 361 / 370 | 341 / 350 | **324 / 334** | 335 / 345 |
| 1 MiB M2 | 434 / 569 | 397 / 555 | 376 / 480 | 367 / 568 | **362 / 475** |
| 1 MiB S1 | 3004 / 3058 | 2652 / 2705 | 2505 / 2525 | **2374 / 2398** | 2464 / 2496 |
| 1 MiB S2 | 3159 / 3351 | 2791 / 3002 | 2646 / 2899 | **2513 / 2673** | 2602 / 2858 |
**Selection (rule fixed in the plan):** score (mean normalised p50/p99 median, 1.0 = best arm everywhere) class M: p75 1.239, p85 1.203, p90 1.039, **p95 1.034**, p90-burst 1.017 (not 10 % below p90: no replacement); class S: 1.248, 1.114, 1.053, **p95 1.011**, 1.046. **Chosen: p95 for both classes.** The burst arm did nothing beyond noise (largest gain 3 % at M2 1 MiB p50).
**The sweep did not bracket the optimum:** latency falls monotonically with the rate in every cell, so 95 % is the top of the tested range, not a found optimum. 97-100 % was not tested.

## 3. Oracle numbers for the adaptive-pacing design (best fixed rate per profile; sweep medians, 8 runs; the confirmed p95 values are in section 4)
Best fixed arm per profile and size, and its p50 / p99 (ms):
| profile | 128 KiB | 1 MiB |
|---|---|---|
| M1 (30 Mbit/s, 5 ms, 1 %) | p95 (28.24 Mbit/s): 44 / 54 | p95: 324 / 334 |
| M2 (30 Mbit/s, 20 ms, bursty) | p95: 69 / 107 | p90 + 24 608 B burst: 362 / 475 (p95: 367 / 568) |
| S1 (4 Mbit/s, 5 ms, 1 %) | p95 (3.77 Mbit/s): 293 / 302 | p95: 2374 / 2398 |
| S2 (4 Mbit/s, 20 ms, bursty) | p95: 321 / 503 | p95: 2513 / 2673 |
The rate is a fraction of the *carried* rate, so for a design the oracle is "0.95 x the link's sustained rate" with the caveat above (upper end not tested). Floor for context (serialisation of the sample's IP bytes at the carried rate plus one-way delay): 1 MiB about 0.29 s (M) and 2.16 s (S), 128 KiB about 0.04 s (M) and 0.27 s (S), plus 5 or 20 ms; the best fixed arm is within about 5-10 % of it (S1 1 MiB 2374 ms against about 2.17 s), so at most 5-10 % is left for any rate controller on this link. Unpaced reference (Stage A/B, 10 runs): 1 MiB p50 M1 1127, M2 3255, S1 5907, S2 14108 ms; 128 KiB 42 / 72 / 349 / 538 ms.
For the placeholders: an estimator that settles at 0.90-0.95 x carried is within 3-8 % of the best fixed rate in every cell above; settling at 0.75 costs 20-30 % (1 MiB S1: 3004 v 2374 ms).

## 4. Phase 2: confirmation of `tickle-p95` on fresh runs (the scored set, 160 runs, 0 lost, 0 aborts)
10 runs per cell and profile (5 x copy A + 5 x copy A2), compared with the unchanged Stage A/B DDS runs of the same link, binaries and command lines (1 MiB: 10 runs per stack, 128 KiB: 8, rel_small/be_small: 4 per profile). Rules as the plan (WIN only if every TickLE run strictly beats every DDS run; A/A VOID at 10 %). Counts over metric x profile x {CycloneDDS, Fast DDS}:
| cell | WIN | TIE | LOSE | VOID |
|---|---:|---:|---:|---:|
| 1 MiB (24) | 15 | 6 | 0 | 3 |
| 128 KiB (24) | 10 | 12 | 0 | 2 |
| be_small + rel_small (64) | 40 | 13 | 0 | 11 |
| total (112) | 65 | 31 | 0 | 16 |
p50 / p99 (ms, median [min..max]) against CycloneDDS and Fast DDS, with the p50 verdicts (v Cyclone / v Fast DDS):
| cell | tickle-p95 | CycloneDDS | Fast DDS | p50 verdicts |
|---|---|---|---|---|
| 1 MiB M1 | 323 [322..326] / 334 [330..357] | 818 / 919 | 1082 / 1101 | WIN / WIN |
| 1 MiB M2 | 367 [345..376] / 488 [397..893] V | 866 / 1313 | 1220 / 1272 | WIN / WIN |
| 1 MiB S1 | 2374 [2368..2385] / 2402 | 2514 / 2662 | 31069 / 32906 (8-9 of 15 delivered) | WIN / WIN |
| 1 MiB S2 | 2497 [2466..2523] / 2660 | 3803 V / 4658 | 29326 / 31829 (8-9 of 15) | VOID / WIN |
| 128 KiB M1 | 43.8 [43.4..44.1] / 54.7 | 55.2 [41.9..55.8] / 126 | 91.2 / 91.9 | TIE / WIN |
| 128 KiB M2 | 78.9 [70.6..86.8] / 135 V | 99.7 [56.5..103] / 257 | 106 / 119 V | TIE / WIN |
| 128 KiB S1 | 292 [289..293] / 304 | 307 [306..336] / 439 | 3562 / 3848 | WIN / WIN |
| 128 KiB S2 | 325 [308..361] / 570 | 439 [411..470] / 944 | 2327 / 2749 | WIN / WIN |
Small cells (tickle-p95, node program honours `TT_PACING_BPS`): be_small p50 10.28 / 40.28 / 10.70 / 40.69 ms = WIN over both stacks in all four profiles, delivery TIE; rel_small p50 WIN over CycloneDDS in all four, VOID against Fast DDS (its own p50 runs spread 31-211 ms), delivery TIE or WIN; **no regression against Stage A's unpaced `tickle`** (same 40 WIN / 13 TIE / 0 LOSE / 11 VOID; be_small delivery 98.0 / 94.1 / 98.1 / 90.4 % v 98.0 / 94.4 / 98.0 / 89.8 % unpaced).
Counters (median over the 10 runs, publisher; unpaced Stage A in brackets): 1 MiB tx datagrams M1 11 157 (49 961), M2 11 397 (45 575), S1 10 838 (219 573), S2 11 682 (274 902); retransmissions 122 (38 689), 298 (33 998), 122 (207 874), 580 (263 191); `pacing_waits` about 10 600-11 100, `pacing_repairs_deferred` 0 / 25 / 0 / 64. 1 MiB is 10 845 datagrams for 15 samples: paced TickLE sends 1.0x-1.08x what it needs. At 128 KiB (1 560-1 690 datagrams) pacing changes M little (queue 100 holds a whole sample) and cuts S from about 2 660 to 1 560 datagrams (retransmissions 990 to 16).

## 5. Where pacing cannot win, and why
1. **Delivery: 100 % against 100 %** is a TIE by the rule; nothing can beat it. Fast DDS's 53-60 % at 4 Mbit/s 1 MiB is a TickLE WIN.
2. **128 KiB p50 against CycloneDDS at M1 and M2 (TIE):** TickLE sits at the link floor (43.8 ms; floor about 42 ms) while CycloneDDS's fastest runs reach 41.9 ms (its runs are bimodal, 41.9-55.8); a strict-WIN needs every TickLE run below every Cyclone run, which cannot happen when the best Cyclone run is at the floor. What is left is at most 2 ms (about 5 %); pacing at 100 % of the carried rate is the only knob, and it is untested.
3. **p99 TIE/VOID (128 KiB S1/S2 against CycloneDDS, M2 1 MiB and 128 KiB):** the p99 of 15 samples is the largest sample, and one run in ten has an outlier (128 KiB S1 max 627 ms, S2 1044 ms; M2 1 MiB 893 ms) in the runs with a burst loss: a lost tail datagram waits a repair round (about 100 ms) and the next sample queues behind it. Pacing does not shorten a repair round. These cells have per-sample spread too large for a 10-run verdict (A/A VOID in M2).
4. **1 MiB S2 p50 against CycloneDDS: VOID** because CycloneDDS's own copies disagree by more than 10 % (3696-5456 ms); TickLE 2497 ms is better than every Cyclone run, so the data point to a WIN, but the plan's rule makes it not decidable at 10 runs.
What a library-side adaptive limiter would have to do: reach 0.90-0.95 x the link's sustained rate without being told it (items 2-4 are not its job: they need a finer repair/tail-loss mechanism, e.g. a tail-loss probe that does not wait a full round, which is a different change); pace fresh and repair data from one budget, so that repair bursts cannot exceed what the link carries (the unpaced default's failure: 4x-23x datagrams); keep the estimate from collapsing on a single loss burst (M2/S2 `pacing_repairs_deferred` up to 64 per run show repair pressure under bursty loss); and cost nothing on wired profiles (scoped to the WIFI profile).

## 6. Sample-size honesty
- Phase 2 has 10 TickLE runs per cell against 8-10 DDS runs for the large cells but only **4 DDS runs for the small cells**: those WIN/TIE verdicts rest on 4 runs per stack and per profile and are labelled so; they were not re-run.
- p99 over 15 samples is a maximum: the p99 columns are noisy and the VOIDs are real, not a lack of effort; more reps would show the outliers more clearly, not remove them.
- The sweep used 8 runs per arm; differences between neighbouring arms below about 3 % (p90 v p95 at 1 MiB M1: 341 v 324) are within what 8 runs can resolve; the monotone trend over four arms and eight cells is what the selection relies on, not any single pair.
- The optimum rate is at the edge of the range (95 %); "95 % is best" is **not** established, only "nothing tested is better".
- One fixed-rate, fixed-delay netem link with a drop-tail queue and one flow; a real Wi-Fi link's rate changes were never tested; the pacing rate here is set from a calibration that a user cannot always make.
Raw data `~/rig_results_safe/wtc_20261117/{A,C}`; selection output `results/C1_selection.txt`; tables `results/tables_C_*.md`; this report `results/PACING_SWEEP.md`.
