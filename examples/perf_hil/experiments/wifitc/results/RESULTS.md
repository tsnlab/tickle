# TickLE against Fast DDS and CycloneDDS on a tc-emulated Wi-Fi-like link (results, 2026-11-19)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi, no real wireless hardware.** Relative figures. Plan: `WIFI_TC_PLAN.md` (committed before the first run, a0393bd2; Stage B list amended in section 9 before Stage B ran). Branch `ab/wifi-tc`. No library, wire, API or default change.
Raw data `~/rig_results_safe/wtc_20261117/{cal,A/*}`; tables `examples/perf_hil/experiments/wifitc/results/`.

## 1. Verdict in one paragraph
**TickLE as shipped (WIFI profile, unpaced) does not beat the DDS stacks on large samples on a rate-limited link.** At 1 MiB it LOSES to CycloneDDS on p50 and p99 in the profiles where the verdict is not VOID (6 LOSE, 10 VOID of 24); at 128 KiB it ties or is VOID (1 LOSE). On small samples it is never worse (be_small and rel_small: 0 LOSE; p50 a WIN or TIE everywhere). **With the documented link options (`tickle-pace`: `pacing_rate` = 75 % of the carried rate) TickLE wins or ties nearly everything**, including 1 MiB (12 WIN, 7 TIE, 2 LOSE, 3 VOID of 24). The cause is understood from the counters (section 4); the fix that makes the shipped default behave like the paced arm is the adaptive limiting of the ROADMAP, profile-scoped (section 5).

## 2. Link settings (every table row names them; per direction, round trip = 2 x delay)
| profile | netem arguments | carried (calibration, plain UDP flood, IP bytes) | `tickle-pace` rate |
|---|---|---:|---:|
| M1 | `limit 100 delay 5ms rate 30mbit loss 1%` | 29.73 Mbit/s | 22 Mbit/s |
| M2 | `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%` | 29.73 | 22 |
| S1 | `limit 40 delay 5ms rate 4mbit loss 1%` | 3.97 | 3 |
| S2 | `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%` | 3.97 | 3 |
Calibration (`cal/`): no reordering in any profile; a 91-datagram burst arrives whole on M, 40 of 91 on S (queue tail-drop); paced 40 pps flow loses 0-7 of 320. Arms: `tickle` = library as shipped (profile `wifi`, no pacing, large_fragmentation AUTO); `tickle-pace` = the same plus `pacing_rate` as above (`pacing_burst` default, `link_rate` declared 54 Mbit/s, the rule of DESIGN.md "Wi-Fi tuning": below the measured sustained rate); CycloneDDS 0.10.5; Fast DDS 2.14.6; zenoh-pico 1.10.2 reference only.
Runs: Stage A 272 (4 profiles x cells x 4 frameworks x A/A2 x 2 rounds, 0 lost); Stage B rounds 3-5 (1 MiB, 10 pooled runs per framework) and 3-4 (128 KiB, 8 pooled). 0 aborts, wlan0/eth0/qdisc/module snapshot unchanged after every run, Pis left clean.

## 3. Scores (WIN/TIE/LOSE/VOID counts over metric x profile x {CycloneDDS, Fast DDS}; A/A rule, VOID = |median A2 - median A| > 10 %)
| cell | tickle | tickle-pace |
|---|---|---|
| be_small, rel_small (64 verdicts) | WIN 40, TIE 13, LOSE 0, VOID 11 | WIN 40, TIE 13, LOSE 0, VOID 11 |
| rel_large 128 KiB (24) | WIN 3, TIE 12, LOSE 1, VOID 8 | WIN 5, TIE 14, LOSE 1, VOID 4 |
| rel_large 1 MiB (24) | WIN 3, TIE 5, LOSE 6, VOID 10 | WIN 12, TIE 7, LOSE 2, VOID 3 |
| total (112) | WIN 46, TIE 30, LOSE 7, VOID 29 | WIN 57, TIE 34, LOSE 3, VOID 18 |

1 MiB, p50 (ms, median of 10 pooled runs [min..max]):
| profile | tickle | tickle-pace | CycloneDDS | Fast DDS (delivered) |
|---|---:|---:|---:|---:|
| M1 | 1127 [496..1993] V | 413 | 818 | 1082 (15/15) |
| M2 | 3255 [2773..3576] | 453 | 866 | 1220 (15/15) |
| S1 | 5907 [4920..9184] | 2979 | 2514 | 31069 (8-9/15) |
| S2 | 14108 [9079..30411] V | 3160 | 3803 V | 29326 (8-9/15) |
Delivery of unpaced `tickle` at 1 MiB: M2 93-100 %, S1 87-100 %, S2 40-87 % of 15 (CycloneDDS 15/15 everywhere).
128 KiB p50: tickle 42 / 72 / 349 / 538 ms, tickle-pace 55 / 77 / 362 / 401, CycloneDDS 55 / 100 / 307 / 439, Fast DDS 91 / 106 / 3562 / 2327 (M1 / M2 / S1 / S2). LOSE: tickle S2 vs CycloneDDS (538 v 439); tickle-pace S1 vs CycloneDDS (362 v 307). Many 128 KiB p99 cells are VOID (A/A spread over 10 %).
Small cells: TickLE p50 is the link floor (RTT 2 x delay + 0.28 ms) and 0.06-0.5 ms under CycloneDDS, 0.1-1 ms under Fast DDS in be_small; rel_small p50 WIN vs CycloneDDS everywhere; Fast DDS rel_small p50/p99 are VOID (noisy, tails of 0.7-1.5 s). Delivery TIE everywhere (be_small loss is the link's).
zenoh-pico (reference, one run per cell): small cells deliver 88-100 % with p50 at the floor, but rel_small p99 1.6 s (M2) and 7.8 s (S2), p50 91 ms at S2; **0/15 delivered in every rel_large cell (128 KiB and 1 MiB)**; not investigated here (reference harness, TCP session; the earlier campaigns also treated it as reference only).

## 4. Why the shipped default loses (evidence from the counters, run r1 copy A, 1 MiB)
| | tx_datagrams (pub) | retransmitted | delivered |
|---|---:|---:|---:|
| M2 tickle | 45 657 | 34 059 | 15/15 |
| M2 tickle-pace | 11 438 | 345 | 15/15 |
| S1 tickle | 263 602 | 251 915 | 14/15 |
| S1 tickle-pace | 11 187 | 111 | 15/15 |
A 1 MiB sample is 723 datagrams (10 845 for 15 samples). The unpaced sender emits it as one burst and every repair round as another burst; the link's queue (100 / 40 packets) drops most of each burst, so TickLE sends 4x (M2) to 23x (S1) the datagrams it needs, the repair traffic itself keeps the queue full, and latency is seconds. Paced at 75 % of the carried rate there are 345 / 111 retransmissions in total. The same mechanism explains the 128 KiB S2 loss. Nothing else in the protocol was implicated: small cells, whose datagrams never exceed the queue, are at the link floor. CycloneDDS and Fast DDS also send unpaced but in smaller bursts (see the diagnosis report: half-size bursts, 0.1 s repair cycle); at 4 Mbit/s Fast DDS delivers only 8-9 of 15 1 MiB samples (the publisher side prints scored=0 by design; the delivery failure is on the subscriber: 53-60 %), an effect on the DDS side that we did not investigate.

## 5. Ranked improvement levers (for the user's decision; none implemented)
1. **Adaptive send limiting in the WIFI profile (ROADMAP "Later": adaptive repair-burst limiting; user 2026-11-15, 2026-11-17).** The sender estimates what the link carries (for example from the ACK/NACK-reported delivery per round, additive increase, back off on loss rounds) and paces fresh and repair datagrams to it, instead of the user supplying `pacing_rate`. Expected effect: the shipped default behaves like `tickle-pace` here: 1 MiB p50 3255 -> about 450 ms (M2), 5907 -> about 3000 ms (S1), 4-23x fewer datagrams, delivery 15/15; this table says what a *correct* rate buys, not that the estimator will find it. Wired impact: none if scoped to `tt_LINK_PROFILE_WIFI` (ETHERNET objects byte-identical, rig core gate 0 WORSE, wired DDS comparison unchanged; to be checked, not assumed). Risk: a wrong estimate on a link whose rate changes (this emulation has a fixed rate; real Wi-Fi rate adaptation was never tested and cannot be here), startup time of the estimate. Evidence strength: high that pacing is the lever, medium that an estimator reaches it.
2. **Documentation now (no code):** the measured guideline works: `pacing_rate` = 75 % of the measured sustained rate was a WIN or TIE against both DDS stacks in all four profiles at 1 MiB except S1 vs CycloneDDS (p50 2979 v 2514) and nothing worse than TIE elsewhere. DESIGN.md "Wi-Fi tuning" currently says "no robust band" and rests on the dropped radio-simulation runs; it should be rewritten from these tables (and the CHANGELOG/RESULTS entries for the user's hwsim decision still have to be made).
3. **Pacing factor:** 75 % leaves 25 % of the link idle; the remaining 1 MiB S1 gap to CycloneDDS (2979 v 2514 ms) and the 128 KiB S1 gap (362 v 307) suggest 85-90 % may win them. Not tuned here (the plan fixes 75 % before the data); a separate registered sweep (0.6 to 0.95 x carried) would answer it. Wired impact none (an option).
4. **Smaller bursts when unpaced** (cap a repair burst at the queue depth the NACK tells us): subsumed by lever 1, lower payoff alone.
Not a lever: small-sample path (already at the link floor); repair cadence.

## 6. Caveats
- A fixed-rate, fixed-delay netem link with a drop-tail queue: no MAC retries, no rate adaptation, no airtime sharing, one flow. Findings are relative and about burst handling on a rate-limited path.
- Stage B was capped (1 MiB 10 runs, 128 KiB 8) and small cells were not repeated; unpaced 1 MiB p50 stays VOID in M1 and S2 because the runs are genuinely bimodal (0.5-2.0 s; 9-30 s), which more reps would not remove.
- `tickle-pace`'s 22 / 3 Mbit/s are 0.75 x the calibration, fixed before the scored runs, not tuned.
- Clock offset (two hosts) is subtracted for rel_large latencies (about +-10 ms); p99 of 15 samples is the maximum.
- Pending outside this task: removing the radio-simulation wording from DESIGN/RESULTS/CHANGELOG (user decision 2026-11-17, reason to be recorded in CHANGELOG) and rewriting the DESIGN.md Wi-Fi tuning section from this data.
