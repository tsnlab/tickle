# ADAPTIVE (no prior) on synthetic 5G-like links, N1-N4 (2026-11-27)

**These are synthetic schedules on an emulated link (`tc netem` in netns on two Raspberry Pi 5 joined by a GRE tap over the rig's Ethernet), not real 5G and not Wi-Fi.** The parameters follow the ranges given in the task and typical literature figures; they were not measured. Library `448b685f` (tree `5a0fb18e`; the harness tree is `8874b11f`, no library change), branch `ab/wifi-adaptive-s1`. 96 runs (4 patterns x 4 arms x 2 sizes x 3 runs), 0 lost, 0 aborts; the Pis' real `eth0`/`wlan0` were not touched (addresses unchanged afterwards, no `wtc_*` namespaces left). Raw data `~/rig_results_safe/wtc_5g_20261126`; tables `results/five_g_tables.txt`; schedules and seeds `results/5g_schedules/N1-N4.json` (seeds 20261127-20261130).
**Method:** a netem player on each Pi's tap applies the schedule (rate, one-way delay, loss) at millisecond timing (smoke: lag 3-6 ms), the same rate and delay in both directions; the applied steps are logged on the sender Pi's clock and every sample, of every arm, is placed by its send time (TickLE `large_trace`, DDS `-L` traces). 0.25 Hz, 180 s per run, RELIABLE KEEP_LAST 10, one publisher and one subscriber.
**Rig headroom (calibration, plain UDP):** the rig carried 99.1 Mbit/s at a 100 Mbit/s netem rate and 198.2 Mbit/s at 200 Mbit/s, no reordering, no loss on a paced flow; on the smoke the Pis had headroom (the Fast DDS sender used 2.1 s of CPU in 60 s at 200 Mbit/s, ADAPTIVE 0.2 s).
**Patterns (all 180 s; queue in packets):** N1 mid-band: random walk 20-150 Mbit/s (log-normal step, sigma 0.25, a step every 200-500 ms, 516 steps), delay 10 ms, 0.5 % loss, queue 500; N2 mmWave blockage: 200 Mbit/s with blockages of 0.2-2 s at 5-10 Mbit/s and short near-outages of 50-100 ms (100 % loss), recovery in two steps within 100 ms, 8 outages in the run, delay 8 ms, 0.1 % loss, queue 500; N3 handover: 100 Mbit/s, every 10-15 s an outage of 50-100 ms (14 in the run) after which the delay becomes a new value drawn from 10-25 ms, 0.1 % loss, queue 500; N4 cell edge / congested: 10-40 Mbit/s slow drift (a step every 1 s, 10-32 Mbit/s reached, mean 17), Gilbert-Elliott loss (1 % / 15 % / 60 % / 0.5 %, about 4 % mean), delay 15 ms, queue 200 (bufferbloat).
**Arms:** `tickle-ad` (`rate_control = ADAPTIVE`, no prior, START 8 Mbit/s, cap 1 Gbit/s); **fixed** = `pacing_rate` 95 % of the schedule's time-weighted mean rate (N1 75.3, N2 156.7, N3 95.0, N4 16.3 Mbit/s; **the fixed arm is told the mean of a schedule a real user could not know, an advantage**); CycloneDDS 0.10.5 and Fast DDS 2.14.6 with the profiles of the earlier stages (UDPv4, maxMessageSize 1472; the 1472 limit is needed for the 1500-byte MTU, so not literally the vendors' defaults). The CycloneDDS / Fast DDS `scored=0` on the near side is the publisher's own line; the far-side lines read all samples delivered.
**Verdict rule:** strict, on per-run p50 (every ADAPTIVE run below every rival run = WIN, the reverse = LOSE, else TIE; a run with fewer than 3 delivered samples counts as infinity); the A/A VOID rule does not apply (one copy per arm, 3 runs), no cell is marked VOID.

## 1. 1 MiB: delivered / p50 / p90 / max ms (pooled over 3 runs); tx / retx per run (median)
| pattern | tickle-ad | fixed 95 % of the mean | CycloneDDS | Fast DDS | ad vs Cyclone / Fast / fixed |
|---|---|---|---|---|---|
| N1 mid-band | 135/135; 243 / 457 / 955; 18,944 / 433 | 135/135; 149 / 310 / 1,671; 23,944 / 453 | 135/135; 232 / 402 / 588 | 135/135; 391 / 1,216 / 2,042 | TIE / WIN / **LOSE** |
| N2 mmWave | 135/135; 221 / 1,137 / 2,121; 31,818 / 1,773 | 135/135; **67** / 1,224 / 2,025; 32,683 / 2,033 | 135/135; 160 / 1,081 / 1,665 | 135/135; 146 / 1,811 / 2,814 | **LOSE / LOSE / LOSE** |
| N3 handover | **134/135**; 193 / 992 / **18,685**; 14,472 / 357 | 135/135; 114 / 190 / 515; 9,577 / 138 | 135/135; 214 / 271 / 342 | 135/135; 341 / 381 / 408 | TIE / WIN / **LOSE** |
| N4 cell edge | 135/135; 891 / 1,206 / 2,971; 36,981 / 2,578 | 135/135; 656 / 956 / 1,663; 36,066 / 2,783 | 135/135; 848 / 1,297 / 1,651 | 135/135; 3,731 / 7,491 / 9,545 | TIE / WIN / **LOSE** |

## 2. 128 KiB: delivered / p50 / p90 / max ms; tx / retx per run (median)
| pattern | tickle-ad | fixed 95 % of the mean | CycloneDDS | Fast DDS | ad vs Cyclone / Fast / fixed |
|---|---|---|---|---|---|
| N1 | 135/135; 37 / 67 / 116; 3,002 / 40 | 135/135; 35 / 58 / 122; 2,898 / 37 | 135/135; 38 / 84 / 164 | 135/135; 42 / 62 / 75 | TIE / TIE / TIE |
| N2 | 135/135; **55** / 144 / 322; 2,134 / 47 | 135/135; **15** / 127 / 304; 1,793 / 55 | 135/135; **14** / 143 / 244 | 135/135; **14** / 180 / 250 | **LOSE / LOSE / LOSE** |
| N3 | 135/135; **73** / 115 / 250; 2,125 / 50 | 135/135; **24** / 35 / 147; 1,998 / 55 | 135/135; 27 / 45 / 170 | 135/135; 27 / 37 / 83 | **LOSE / LOSE / LOSE** |
| N4 | 135/135; 147 / 252 / 609; 5,142 / 180 | 135/135; 109 / 143 / 839; 4,922 / 151 | 135/135; 123 / 194 / 548 | 135/135; 116 / 197 / 259 | **LOSE / LOSE / LOSE** |

## 3. Totals (8 cells per rival: 4 patterns x 2 sizes)
| against | WIN | TIE | LOSE |
|---|---|---|---|
| CycloneDDS | 0 | 4 | 4 |
| Fast DDS | 3 (1 MiB N1, N3, N4) | 1 | 4 |
| both DDS stacks | **3** | **5** | **8** (of 16) |
| fixed 95 % of the mean | 0 | 1 | **7** |
By pattern (6 comparisons each): N1 2 W / 3 T / 1 L; N2 0 / 0 / 6; N3 1 / 1 / 4; N4 1 / 1 / 4. **Where the data say ADAPTIVE wins: only against Fast DDS at 1 MiB (3 of 4 patterns); it never wins against CycloneDDS or the fixed rate, and loses to all three rivals on every 128 KiB cell of N2-N4 and on 1 MiB N2.**

## 4. Capacity tracking of tickle-ad (RATETRACE `rate_kbit` against the capacity, time-weighted, outage intervals excluded)
| pattern, size | median rate / capacity | within +-30 % | over capacity | over 1.3x | under 0.7x |
|---|---|---|---|---|---|
| N1, 1 MiB / 128 KiB | 0.64 / 0.60 | 27 % / 27 % | 24 % / 20 % | 15 % / 14 % | 59 % / 59 % |
| N2 | **0.43 / 0.16** | 9 % / **0 %** | 27 % / 16 % | 24 % / 16 % | **68 % / 84 %** |
| N3 | **0.50 / 0.19** | 29 % / **0 %** | 11 % / 0 % | 2 % / 0 % | **68 % / 100 %** |
| N4 | 0.74 / 0.62 | 42 % / 33 % | 30 % / 17 % | 15 % / 10 % | 43 % / 57 % |
Unlike the Wi-Fi-like patterns, where the estimate sat around the capacity, here it sits **below** the capacity most of the time: at 128 KiB on the high-rate links (N2 200 Mbit/s, N3 100 Mbit/s) the pacing rate is 16-19 % of the capacity (84-100 % of the time under 0.7x), i.e. the controller is still near its 8 Mbit/s start rate; a 128 KiB sample is about 1 Mbit and the sample is gone before the estimate climbs. That matches the p50 gap on the 128 KiB cells of N2 and N3 (55 and 73 ms against 14-27 ms for every rival).

## 5. Repair traffic and tails
- **Retransmissions, ADAPTIVE against the fixed arm (1 MiB, per run):** N1 433 against 453, N2 1,773 against 2,033, N3 **357 against 138** (ADAPTIVE higher), N4 2,578 against 2,783. Unlike the Wi-Fi-like runs there is **no repair-traffic advantage** for ADAPTIVE: the fixed rate at 95 % of the mean does not flood these links (their capacity rarely falls far below the mean) and ADAPTIVE sends the same or more.
- **Worst tails of ADAPTIVE:** `N3/rl1m/r1_tickle-ad_A`: 1 of 45 samples undelivered and a max of **18.7 s** (the only ADAPTIVE run with undelivered samples; N3's outages with delay changes). Otherwise ADAPTIVE's 1 MiB max is 1.0-3.0 s, 128 KiB 0.1-0.6 s.
- **Others:** the fixed arm and CycloneDDS: no undelivered sample and no latency over 5 s in any of the 24 runs each; Fast DDS: no undelivered sample; N4 1 MiB had a max of 9.5 s, 6.3 s and 8.7 s in its three runs (p50 3.7 s).
- **The 128 KiB N2 / N3 gap is not a tail problem:** it is the median (55 / 73 ms against 14-27 ms).

## 6. Limits, plainly
- Synthetic schedules, not real 5G; the numbers are an emulation of rates, delay changes, outages and loss on one link between two hosts: no radio scheduling, no per-user sharing, no carrier aggregation, no real handover signalling. The delay changes of N3 can reorder packets in flight (netem sorts by departure time); not a 5G behaviour.
- Symmetric: the same rate, delay and loss in both directions (a real 5G link is asymmetric). One publisher, one subscriber (no multi-reader). ADAPTIVE without a prior: the only mode that makes no assumption about the link; a user who knew the link could set the prior, which these runs do not test.
- The fixed arm is given the mean of the schedule (an advantage over any real configuration); 3 runs per cell (verdicts rest on 3 vs 3 per-run p50 values; the 128 KiB N2-N4 gaps are large against the run-to-run spread, the N1 and the 1 MiB TIEs are not); the A/A noise check could not be applied; the Fast DDS and CycloneDDS profiles are the earlier stages' (maxMessageSize 1472).
- Conclusion supported by these data: on these high-rate patterns ADAPTIVE (no prior) is **not** competitive: it is worse than CycloneDDS and Fast DDS in 8 of 16 comparisons (better only against Fast DDS at 1 MiB in 3 patterns) and worse than a fixed rate set from the mean in 7 of 8, mainly because it starts at 8 Mbit/s and ramps too slowly for 100-200 Mbit/s links and 128 KiB samples (and, on N2, 1 MiB). Whether a prior (the user's link rate) or a higher no-prior start rate would change this was not tested. Not changed: no library, wire, API, default, merge or tag in this stage.
