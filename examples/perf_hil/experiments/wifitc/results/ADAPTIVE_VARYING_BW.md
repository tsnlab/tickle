# ADAPTIVE under time-varying bandwidth (2026-11-26)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi** (netns + GRE tap; the Pis' real `eth0`/`wlan0` untouched; addresses unchanged afterwards; Pis clean). Library `448b685f` (tree `5a0fb18e`, branch `ab/wifi-adaptive-s1`); harness commit `9114b42a`. 144 runs (6 patterns x 2 sizes x 4 arms x 3 runs), 0 lost, 0 aborts, 12 clean session ends. Raw data `~/rig_results_safe/wtc_varybw_20261126`; full per-pattern tables `results/varying_bw_tables.txt`; schedules `schedules/<pattern>.txt` (P7 seed 20261126). No library change.
**Link:** M1 (5 ms one way, 1 % loss, queue 100 packets); the netem rate follows a schedule applied on both taps by the rate-step driver, step markers on the sender Pi's clock; 0.25 Hz; per-sample traces for every arm (TickLE `large_trace`, DDS `-L`), every sample placed by its send time. **Patterns:** P7 random walk (integer Mbit/s uniform 5..30 every 5 s, seed 20261126), 180 s; P5a / P5b square wave 30 <-> 10 Mbit/s with a 20 s / 60 s period, 180 s; P6 30 Mbit/s with a 5 s dip to 4 at t = 60 s, 150 s; P3 30 -> 4 linearly over 150 s (a step every 5 s) then 30 s hold, 180 s; P4 4 -> 30 linearly over 150 s then 30 s hold, 180 s. **Arms:** `tickle-ad` (ADAPTIVE, no prior), `tickle-p95` (fixed 28.2 Mbit/s = 95 % of 30), CycloneDDS 0.10.5, Fast DDS 2.14.6.
**Verdict rule:** strict, on per-run p50 (every ADAPTIVE run below every rival run = WIN; the reverse = LOSE; else TIE; a run with fewer than 3 delivered samples counts as infinity). The A/A VOID rule does not apply (one copy per arm, 3 runs); no cell is marked VOID.

## 1. p50 / p90 / max (ms, pooled over 3 runs) and delivered, 1 MiB
| pattern | tickle-ad | tickle-p95 (fixed) | CycloneDDS | Fast DDS | ad vs Cyclone / Fast / fixed |
|---|---|---|---|---|---|
| P7 random walk | 133/135; 557 / 1,311 / **12,503** | 135/135; 520 / 1,303 / 3,034 | 135/135; 819 / 1,122 / 1,707 | 135/135; 2,220 / 4,192 / 5,817 | WIN / WIN / **LOSE** |
| P5a sawtooth 20 s | 135/135; 890 / 1,227 / 1,603 | 135/135; 790 / 884 / 1,311 | 135/135; 873 / 1,030 / 1,222 | 132/135; 1,304 / 4,369 / 8,088 | TIE / WIN / **LOSE** |
| P5b sawtooth 60 s | 135/135; 884 / 1,005 / 1,638 | 135/135; 787 / 882 / 1,864 | 135/135; 918 / 1,032 / 1,199 | 132/135; 3,946 / 8,358 / 9,376 | WIN / WIN / **LOSE** |
| P6 5 s dip | 114/114; 328 / 473 / 3,686 | 114/114; 324 / 334 / 5,606 | 111/111; 818 / 917 / 2,385 | 111/111; 1,088 / 1,117 / 6,074 | WIN / WIN / TIE |
| P3 gradual fall | 135/135; 634 / 2,258 / 2,355 | 135/135; 680 / 3,432 / 5,995 | 135/135; 822 / 2,540 / 3,557 | 117/135; 2,095 / 22,439 / 30,768 | WIN / WIN / **WIN** |
| P4 gradual rise | 133/135; 521 / 1,440 / **21,769** | 135/135; 448 / 1,692 / 6,469 | 135/135; 820 / 1,516 / 2,673 | 135/135; 3,921 / 14,838 / 18,458 | WIN / WIN / **LOSE** |
tx / retx per run (median): P7 ad 39,000 / 4,465 against fixed 53,133 / 19,787; P5a 40,446 / 4,446 against 51,309 / 18,003; P5b 37,547 / 1,916 against 50,006 / 17,085; P6 31,982 / 2,597 against 29,146 / 2,257; P3 37,788 / **940** against 82,925 / **49,294**; P4 36,505 / 1,280 against 54,401 / 20,974.

## 2. p50 / p90 / max (ms) and delivered, 128 KiB
| pattern | tickle-ad | tickle-p95 (fixed) | CycloneDDS | Fast DDS | ad vs Cyclone / Fast / fixed |
|---|---|---|---|---|---|
| P7 | 135/135; 85 / 162 / 211 | 135/135; 66 / 142 / 239 | 135/135; 74 / 161 / 271 | 135/135; 131 / 252 / 414 | **LOSE** / WIN / **LOSE** |
| P5a | 135/135; 105 / 116 / 149 | 135/135; 47 / 116 / 230 | 135/135; 114 / 153 / 234 | 135/135; 91 / 252 / 1,192 | WIN / **LOSE** / **LOSE** |
| P5b | 135/135; 104 / 120 / 236 | 135/135; 50 / 116 / 125 | 135/135; 113 / 143 / 246 | 135/135; 92 / 252 / 253 | TIE / **LOSE** / **LOSE** |
| P6 | 114/114; 44 / 78 / 282 | 114/114; 44 / 53 / 281 | 111/111; 55 / 119 / 313 | 111/111; 91 / 92 / 621 | WIN / WIN / TIE |
| P3 | 135/135; 95 / 282 / 306 | 135/135; 80 / 281 / 288 | 135/135; 101 / 278 / 362 | 135/135; 162 / 618 / 721 | WIN / WIN / **LOSE** |
| P4 | 135/135; 76 / 201 / 349 | 135/135; 63 / 166 / 292 | 135/135; 82 / 183 / 316 | 135/135; 131 / 355 / 618 | TIE / WIN / **LOSE** |

## 3. WIN / TIE / LOSE of tickle-ad (12 cells per rival: 6 patterns x 2 sizes)
| pattern | against CycloneDDS (1 MiB, 128 KiB) | against Fast DDS | against fixed 95 % | pattern total (6 comparisons) |
|---|---|---|---|---|
| P7 | WIN, LOSE | WIN, WIN | LOSE, LOSE | 3 W / 0 T / 3 L |
| P5a | TIE, WIN | WIN, LOSE | LOSE, LOSE | 2 / 1 / 3 |
| P5b | WIN, TIE | WIN, LOSE | LOSE, LOSE | 2 / 1 / 3 |
| P6 | WIN, WIN | WIN, WIN | TIE, TIE | 4 / 2 / 0 |
| P3 | WIN, WIN | WIN, WIN | WIN, LOSE | 5 / 0 / 1 |
| P4 | WIN, TIE | WIN, WIN | LOSE, LOSE | 3 / 1 / 2 |
| **overall** | **8 W / 3 T / 1 L** | **10 W / 0 T / 2 L** | **1 W / 2 T / 9 L** | 19 / 5 / 12 of 36 (against the two DDS stacks only: **18 W / 3 T / 3 L of 24**) |

## 4. How well tickle-ad follows the capacity (RATETRACE `rate_kbit`, time-weighted by round length, against capacity = nominal netem rate x 0.991)
| pattern, size | median rate / capacity | within +-30 % | over capacity | over 1.3x | under 0.7x |
|---|---|---|---|---|---|
| P7, 1 MiB / 128 KiB | 0.97 / 1.02 | 52 % / 50 % | 46 % / 55 % | 16 % / 27 % | 32 % / 23 % |
| P5a, 1 MiB / 128 KiB | **0.77 / 1.15** | 49 % / **31 %** | 39 % / 63 % | 8 % / **44 %** | **43 %** / 25 % |
| P5b | 1.02 / 1.03 | 67 % / 58 % | 57 % / 66 % | 6 % / 24 % | 27 % / 18 % |
| P6 | 0.95 / 1.04 | 57 % / 64 % | 47 % / 63 % | 13 % / 16 % | 31 % / 20 % |
| P3 | 1.02 / 1.02 | **77 % / 76 %** | 60 % / 62 % | 5 % / 9 % | 18 % / 15 % |
| P4 | 1.00 / 0.92 | 72 % / 72 % | 50 % / 33 % | 7 % / 9 % | 21 % / 19 % |
The rate follows slow changes best (P3 gradual fall, P4 gradual rise: 72-77 % of the time within +-30 %, 5-9 % over 1.3x) and is poorest on the fast square wave P5a at 128 KiB (31 % within +-30 %, 44 % of the time over 1.3x the capacity) and, at 1 MiB, on P5a it runs under 0.7x of the capacity 43 % of the time (median 0.77). Time over capacity is 33-66 % in every pattern (the estimate sits around the capacity, above it about half the time); small samples spend long idle rounds at an old rate (the 128 KiB figures weight idle rounds by time).

## 5. Where ADAPTIVE loses, plainly
1. **Against the fixed 95 % of 30 Mbit/s in 9 of 12 cells** (P7, P5a, P5b, P4 at both sizes, P3 at 128 KiB): its p50 is 5-15 % higher at 1 MiB (e.g. P7 561 against 520 ms, P4 521 against 448 ms) and **2x at 128 KiB on the sawtooth (P5a 107 against 48 ms, P5b 104 against 50 ms)**. The fixed rate pays for it in repair traffic (retransmissions 4-50x ADAPTIVE's: 17-20 k against 1.9-4.5 k per 1 MiB run), which p50 does not show: in these patterns the capacity is at or above 10 Mbit/s and the queue absorbs the bursts of an over-paced sender. ADAPTIVE wins against it only where the capacity falls steadily (P3 at 1 MiB, 638 against 682 ms, with 940 retransmissions against 49,294) and ties on the 5 s dip.
2. **Against the DDS stacks, 3 of 24 comparisons:** 128 KiB P7 against CycloneDDS (84 against 74 ms), 128 KiB P5a and P5b against Fast DDS (107 against 91 ms and 104 against 92 ms). Everywhere else ADAPTIVE wins or ties both DDS stacks; at 1 MiB it wins every one of the 12 comparisons against Fast DDS and 5 of 6 against CycloneDDS (a TIE on P5a).
3. **Worst pattern: the sawtooth P5a / P5b at 128 KiB** (loses to fixed, to Fast DDS and ties or beats CycloneDDS only by 8 ms; the estimate is 44 % of the time over 1.3x the capacity on P5a); **worst tail: P4 and P7 at 1 MiB**, each with one run with a long tail and undelivered samples: `P4/rl1m/r2_tickle-ad_A` (2 of 45 samples undelivered, max 21.8 s) and `P7/rl1m/r3_tickle-ad_A` (2 of 45 undelivered, max 12.5 s). No other ADAPTIVE run lost a sample or exceeded 10 s; no CycloneDDS or fixed-rate run did either.
4. Failed or lossy runs of the others (for the record): Fast DDS undelivered samples inside the run window in P5a and P5b at 1 MiB (1 of 45 in every run, max 7.7-9.4 s), P3 at 1 MiB (6 of 45 in every run, max 26.7-30.8 s) and a max of 17.7-18.5 s on P4; CycloneDDS and the fixed rate: none.
Sample size: 3 runs per cell (strict-rule verdicts over 3 vs 3 runs; the per-run p50 differences on the 128 KiB sawtooth are large against the run-to-run spread, the 1 MiB differences of 5-10 % are not always); one emulated link and loss pattern; the patterns' slow-sample latency includes the in-flight sample at each change.
Not changed: no library, wire, API, default, merge or tag in this stage.
