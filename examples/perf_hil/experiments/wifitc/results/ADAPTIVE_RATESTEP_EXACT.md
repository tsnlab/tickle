# Adaptive rate control, rate-step stage, exact version: phases by send time, per-sample traces for every arm (2026-11-25)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi** (netns + GRE tap over the rig's Ethernet; the Pis' real `eth0`/`wlan0` untouched, addresses and qdisc unchanged afterwards, Pis clean). Library `ab/adaptive-next` ae5c192a (tree `eaa0b2b6`); harness commit `743724f9` on `ab/wifi-adaptive-s1`. 40 runs (5 per arm, 1 MiB and 128 KiB), 0 lost, 0 aborts. Raw data `~/rig_results_safe/wtc_ratestep_exact_20261124`, scorer `wtc_ratestep_read2.py` (output in `results/ratestep_exact_tables.txt`). No library change.
**Why this replaces the earlier rate-step report** (`WIFI_ADAPTIVE_RATESTEP_2026-11-23.md`): the DDS nodes write the `-l` latency file **sorted**, and the earlier scorer attributed DDS samples to phases by that order, which was wrong. This run adds an unsorted per-sample output to the DDS nodes (`-L`: subscriber `seq send_ns recv_ns`, publisher `seq send_ns`; the `-l` file is unchanged; checked as the same multiset of latencies in smoke runs, max difference 0.0 us) and attributes **every sample of every arm by its send time** (the sender Pi's CLOCK_MONOTONIC) against the step times (the cli-side `tc qdisc change`, `WTC_CLI_STEP_MONO_NS` in `<run>.steps`). TickLE arms use `large_trace` per-sample lines; samples that were never delivered get their send time from the publisher's fixed 4 s period.
**Link / arms:** M1 (5 ms each way, 1 % loss, queue 100) 30 Mbit/s, **-> 4 Mbit/s at 45 s -> 30 Mbit/s at 90 s**, 135 s runs, 0.25 Hz (34 samples; about 12 / 11 / 11 per phase); arms `tickle-ad` (ADAPTIVE, no prior), `tickle-p95` (fixed 28.2 Mbit/s = 95 % of 30), CycloneDDS 0.10.5, Fast DDS 2.14.6 (the DDS binaries were rebuilt for the `-L` option from the same source; the previous binaries are kept as `*_orig_1114` on the Pis). Latency = arrival - send - clock offset.
**`scored=0` check:** CycloneDDS's (and Fast DDS's) `scored=0` is only the **publisher's** RESULT line (role=pub). The subscriber lines read `scored=33` with `published=33` in all 5 runs of both sizes for both stacks: every DDS sample was delivered.

## 1. 1 MiB, per phase (5 runs pooled: delivered / p50 / p90 / max ms)
| arm | before the step (30 Mbit/s) | at 4 Mbit/s | after the step up (30 Mbit/s) | recovery after the step down / up (s, median) | tx / retx per run |
|---|---|---|---|---|---|
| **tickle-ad (ADAPTIVE)** | 55/55 / **329** / 502 / 800 | 59/60 / **2,266** / 9,454 / 17,999 | 55/55 / **338** / 433 / 868 | **17** / 4 | 30,449 / 2,726 |
| tickle-p95 (fixed 95 % of 30) | 55/55 / 324 / 333 / 533 | 60/60 / **6,062** / 10,477 / 15,718 | 55/55 / 324 / 332 / 354 | 1 / 4 | 49,053 / 24,262 |
| CycloneDDS | 60/60 / 818 / 855 / 1,018 | 55/55 / **2,541** / 3,462 / 4,637 | 50/50 / 818 / 918 / 1,018 | 3 / 2 | - |
| Fast DDS | 60/60 / 1,086 / 1,107 / 5,595 | 55/55 / **23,727** / 34,436 / 38,322 | 50/50 / 1,948 / 3,954 / 3,979 | 31 / 16 (3 of 5 never) | - |

## 2. 128 KiB, per phase
| arm | before | at 4 Mbit/s | after the step up | recovery down / up (s) | tx / retx per run |
|---|---|---|---|---|---|
| **tickle-ad (ADAPTIVE)** | 55/55 / 46 / 109 / 233 | 60/60 / 281 / 290 / 307 | 55/55 / **102** / 203 / 281 | 1 / **36** | 3,706 / 50 |
| tickle-p95 | 55/55 / 44 / 47 / 54 | 60/60 / 279 / 282 / 299 | 55/55 / 43 / 51 / 55 | 1 / 4 | 3,230 / 43 |
| CycloneDDS | 60/60 / 56 / 73 / 127 | 55/55 / 301 / 339 / 407 | 50/50 / 55 / 69 / 218 | 3 / 2 | - |
| Fast DDS | 60/60 / 89 / 92 / 93 | 55/55 / 618 / 621 / 621 | 50/50 / 90 / 93 / 93 | 3 / 2 | - |
Recovery = seconds from the step to the send time of the first sample after it whose latency is within 1.5x the steady value of the new phase (last 4 delivered samples of the slow phase for the step down, of the first phase for the step up) and whose next two samples are too; "never" = not within the run. The pooled p90 / max of a phase include the sample in flight at a step.

## 3. WIN / TIE / LOSE per phase (every ADAPTIVE run's phase p50 against every run of the other arm; a run with fewer than 3 delivered samples in the phase counts as infinity). The A/A VOID rule is **not applicable** here (5 runs of one copy per arm, no A / A2 split); the verdicts are the strict-order rule only.
| ADAPTIVE against | 1 MiB before / at 4 Mbit/s / after | 128 KiB before / at 4 Mbit/s / after |
|---|---|---|
| CycloneDDS | WIN (321 v 818) / TIE (2,265 v 2,541) / WIN (335 v 818) | TIE (48 v 56) / TIE (281 v 276) / **TIE (104 v 55: slower in the median)** |
| Fast DDS | WIN (321 v 1,090) / WIN (2,265 v 24,001) / WIN (335 v 2,910) | TIE (48 v 88) / WIN (281 v 618) / TIE (104 v 90) |
| fixed 95 % of 30 | TIE (321 v 324) / **WIN (2,265 v 6,623)** / TIE (335 v 324) | TIE (48 v 44) / **LOSE (281 v 279)** / **LOSE (104 v 43)** |

## 4. Where ADAPTIVE loses (plainly)
1. **128 KiB after the step up: ADAPTIVE is the slowest arm** (p50 102 ms against 43 fixed, 55 CycloneDDS, 90 Fast DDS), and it takes **36 s** (median over 5 runs) to get back to within 1.5x of its first-phase latency, against 2 s for both DDS stacks and 4 s for the fixed rate. The strict rule scores it a TIE with both DDS stacks (some ADAPTIVE runs are as fast as theirs) and a LOSE against the fixed rate. A small sample sees the rate controller's slow climb back to 30 Mbit/s.
2. **128 KiB at 4 Mbit/s against the fixed rate:** 281 against 279 ms is a LOSE by the strict rule (every fixed run is below every ADAPTIVE run), a 1 % difference; CycloneDDS is 301, so ADAPTIVE is not behind a DDS stack there.
3. **1 MiB after the step down: ADAPTIVE reacts slowly:** 17 s median to get within 1.5x of the slow-phase steady latency (CycloneDDS 3 s, the fixed 95 % of 30 only 1 s because it was already late, Fast DDS 31 s), with a phase p90 of 9.5 s and max 18 s (fixed rate 10.5 s / 15.7 s; CycloneDDS 3.5 s / 4.6 s). In the steady part of the slow phase ADAPTIVE is at the link's rate (p50 2,266 ms, CycloneDDS 2,541 ms: a TIE), but the first samples after the step are queued behind the old rate's data.
4. **ADAPTIVE costs more datagrams and retransmissions than the DDS stacks' payload**: 30,449 tx, 2,726 retransmissions per 1 MiB run (the fixed 95 % of 30: 49,053 and 24,262; the ideal is about 11,000).
Where ADAPTIVE wins: 1 MiB in the two fast phases against both DDS stacks (329 / 338 ms against Cyclone's 818 and Fast DDS's 1,086 / 1,948 ms), and in the slow phase against Fast DDS (2.3 s against 23.7 s) and the fixed rate (2.3 s against 6.1 s); it ties CycloneDDS in the slow phase (2,266 against 2,541 ms) and in 128 KiB phases 1 and 2.

## 5. Corrected DDS rate-step numbers for `docs/RESULTS.md` section 5 (replacing the rows written from the sorted-order attribution)
p50 in ms, pooled over 5 runs, attributed by send time; before / at 4 Mbit/s / after the step up (30 -> 4 -> 30 Mbit/s):
| arm | 1 MiB | 128 KiB |
|---|---|---|
| CycloneDDS 0.10.5 | 818 / 2,541 / 818 | 56 / 301 / 55 |
| Fast DDS 2.14.6 | 1,086 / 23,727 / 1,948 | 89 / 618 / 90 |
| TickLE ADAPTIVE (no prior) | 329 / 2,266 / 338 | 46 / 281 / 102 |
| TickLE fixed 95 % of 30 | 324 / 6,062 / 324 | 44 / 279 / 43 |
Previous (wrong) DDS rows: CycloneDDS 1 MiB 754 / 820 / 2,541, 128 KiB 41 / 62 / 313; Fast DDS 1 MiB 1,067 / 2,916 / 25,331, 128 KiB 88 / 91 / 618: the earlier statements that the DDS stacks "do not recover after the step up" were an artefact; they recover in about 2-3 s (CycloneDDS) and, at 1 MiB, Fast DDS stays 1.8x slower than before the step in 3 of 5 runs. The earlier "ADAPTIVE beats both DDS stacks after the step up" holds at 1 MiB (338 against 818 and 1,948 ms) but not at 128 KiB (102 against 55 and 90 ms).

## 6. Recommendation
Unchanged in direction, sharper in limits: ADAPTIVE (no prior) is the arm that follows a changing link at 1 MiB, clearly better than the fixed rate in the slow phase and better than both DDS stacks in the fast phases at 1 MiB, and ties CycloneDDS in the slow phase. It loses at 128 KiB after the step up (36 s to recover; slowest arm) and reacts to a step down in 17 s at 1 MiB against CycloneDDS's 3 s. Opt-in experimental (option b) remains defensible, with these two limits stated; fixed pacing remains the choice for a known static link. Not changed: no library, wire, API, default, merge or tag in this stage. Sample size: 5 runs per arm, 11-12 samples per phase per run; one emulated link, one loss pattern, one step pair.
