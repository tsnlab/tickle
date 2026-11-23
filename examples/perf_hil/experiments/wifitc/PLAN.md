# Wi-Fi-like link emulated with tc netem: TickLE against Fast DDS and CycloneDDS (plan, registered 2026-11-17, committed before any run)

**What this is.** An emulation of Wi-Fi-like conditions made with `tc netem` on the two rig Raspberry Pi 5 (rate, delay, loss, queue).
It is **not Wi-Fi and not a Wi-Fi performance figure**: it has no MAC retries, no airtime sharing and no rate adaptation. It is a
controlled, reproducible link whose settings are stated in every result row, used as a relative comparison between frameworks. No real wireless
hardware is used (the user, 2026-11-08). Decision of the user, 2026-11-17: the earlier radio-simulation runs are dropped from the documentation
(reason recorded in `docs/CHANGELOG.md`); this plan is the scored comparison.
Branch `ab/wifi-tc` (from `ab/release-2.2.0`). **No library, wire, API or default change.** Findings that suggest one are reported to the user for
a decision (the user, 2026-11-17: analyse first, implement only after discussion).

## 1. Why
The diagnosis of 2026-11-17 (`WIFI_DIAG_RESULTS_2026-11-17.md`) found that the earlier simulated link came up in one of two rate states by chance,
which decided the large-sample results (slow state: no protocol can deliver; fast state: TickLE equals Fast DDS). A link whose rate, delay and loss we
set ourselves has no such state, so the comparison can be repeated and every cell compared like for like.

## 2. Topology and safety
Namespace `wtc_sta` on rpi-1 (client, publisher / pinger) and `wtc_srv` on rpi-0 (server, subscriber / echo), each holding one GRE tap
(`wtcgc` / `wtcgs`) whose outer packets travel over the rig's Ethernet (eth0, addresses 192.168.10.3 / .2); inner subnet 10.90.0.0/24, inner
MTU 1500 (as Wi-Fi), DF cleared on the inner packet so that 1500 B datagrams pass the tunnel. The link is `tc qdisc ... root netem <profile>` on the
egress of **both** taps: every figure is per direction, the round trip is twice the delay. The Pis' `eth0` and `wlan0` are never configured;
`wtc_topo.sh snap` (addresses, routes, link state, qdiscs, modules, namespaces) is compared before and after **every** run; the session aborts on a
difference and ends by removing every namespace and tap and every module it loaded (`sch_netem` was already loaded and stays).
Runs take `flock /tmp/rig.lock` and the rig lock script, are launched detached, and each run uses fresh namespaces and processes.

## 3. Link profiles (arms)
| profile | rate | delay (one way) | loss | queue limit |
|---|---:|---:|---|---:|
| M1 | 30 Mbit/s | 5 ms | 1 % independent | 100 packets |
| M2 | 30 Mbit/s | 20 ms | bursty, `gemodel 1% 25% 70% 0.1%` (mean about 2.7 %, bad runs of about 4 packets) | 100 |
| S1 | 4 Mbit/s | 5 ms | 1 % independent | 40 |
| S2 | 4 Mbit/s | 20 ms | bursty, `gemodel 2% 25% 70% 0.1%` (mean about 5 %) | 40 |
| C1 | 30 Mbit/s | 5 ms | none | 100 (calibration control only, not scored) |

The exact `netem` argument string of each is in `wtc_lib.sh` (`profile_netem`) and is written into the `.meta` of every run and into the header
of every result table. Fixed delay, no jitter: netem jitter reorders packets (a Wi-Fi MAC delivers in order), which would score reordering handling
instead of the protocols. The small queue limit gives the burst tail-drop an access point shows. Medium = the order of a good 2.4/5 GHz link's
goodput, slow = a poor one; the numbers are a choice, not a measurement of a real network.

## 4. Calibration (before any framework run)
`MODE=cal ./wtc_run.sh`, every profile once: ping RTT (20); a 5 s unpaced UDP flood (1472 B payload) -> the sustained rate the link carries
(`R_cal`, received IP bytes over the receive window); 3 back-to-back bursts of 91 and of 256 datagrams -> how many of each arrive (the burst
tail-drop); a 40 pps paced flow for 8 s -> loss and reordering of a gentle flow. Output `cal_<profile>.{ping,client,server}`. A profile whose
carried rate differs from its netem rate by more than 10 %, or which reorders more than 0.1 % of the paced flow, is reported and fixed before
anything else runs.

## 5. Cells and arms
* Cells: `be_small` (100 B at 100 Hz, BEST_EFFORT, ping-pong RTT), `rel_small` (same, RELIABLE), `rel_large` 128 KiB and 1 MiB at 0.25 Hz
  (15 samples, RELIABLE KEEP_LAST 10, publisher on the client, subscriber on the server, sub 8 s past the last publish).
  Small cells run 30 s (3000 samples at 100 Hz), large cells 60 s. Same node programs, command lines and readout rules as the earlier Pi comparison
  (`t1pi_fw.sh`, removed from the tree; last at cb9c8606 under experiments/t1pi/), `TT_UDP_OFFLOAD=0` for TickLE.
* Arms per cell: TickLE `tickle` (the library as shipped: profile `wifi` = `tt_LINK_PROFILE_WIFI`, no pacing, `large_fragmentation` AUTO);
  `tickle-pace` (the same plus the documented link options: `pacing_rate` set by the DESIGN.md guideline below, `pacing_burst` default,
  `large_fragmentation` AUTO); CycloneDDS 0.10.5 (`cyclonedds_node`, interface set by address); Fast DDS 2.14.6 (UDPv4 only, `maxMessageSize` 1472);
  zenoh-pico 1.10.2, reference only, never scored, one round of copy A.
* **Pacing rule (docs/DESIGN.md "Wi-Fi tuning"):** "choose it below the link's sustained throughput, measured with an unpaced UDP flood counted at the
  receiver, take well under that". Here `pacing_rate` = 75 % of the calibration's `R_cal` for the profile (IP bytes on the wire), rounded to the nearest
  whole Mbit/s (the arm's name carries it: `tickle-r<n>` in the bench); `link_rate` declared 54 Mbit/s. Fixed from the calibration before the first
  scored run, not tuned afterwards.
* Small cells use the DDS command lines of the earlier comparison (`-K 100 -B 1`), large `-K 10 -B 10 -Q 3`.

## 6. Reps, noise and verdict rules (the earlier Pi comparison's, unchanged)
* **Stage A:** per (cell, profile, framework) 2 rounds x copies A and A2 = 4 runs; the order inside a (cell, profile) block is the list
  [fw.A for each fw, fw.A2 for each fw] rotated left by 3 x (round - 1). Fresh namespaces per run, three attempts per run before it is lost.
* **Noise (A/A2):** a framework-metric is VOID when |median(A2) - median(A)| > 10 % of median(A); a pair with a VOID side is VOID.
* **Verdict per metric** (delivery %, p50, p90 where scored, p99 ms) TickLE against each scored DDS over the pooled runs of A and A2: WIN when every
  TickLE run strictly beats every DDS run, LOSE when every DDS run strictly beats every TickLE run, else TIE; a framework with no scored sample has
  latency = infinity. Reported for `tickle` and for `tickle-pace` separately; **the score of the product is `tickle`** (library as shipped),
  `tickle-pace` shows what the documented options buy.
* **Stage B (deciding cells):** every (cell, profile) in which `tickle` has a LOSE or a TIE-with-worse-median against either DDS in Stage A gets
  extra rounds up to 10 pooled runs per framework (rounds 3..5), same rules; WIN-everywhere and non-VOID cells are not repeated. VOID cells get the
  same extra rounds once. The list of cells is written into the report before Stage B starts.
* Each table row names the profile's netem arguments and the `tickle-pace` rate; nothing else is mixed in.

## 7. Analysis (after the runs)
Per cell WIN/TIE/LOSE counts for `tickle` and `tickle-pace`; every LOSE and every TIE with a worse median examined: counters
(`retransmitted`, datagrams sent, NACK rounds, `pacing_waits`, discovery traffic) and, where the counters do not explain it, a pcap of the
tap in one pilot run (not scored, under `pilot_*`). Output: ranked levers with the evidence, the expected effect and the wired-network exposure
(wired DDS comparison must show no LOSE, ETHERNET objects byte-identical, rig core gate 0 WORSE) for the user to decide. No library change is made.

## 8. Cost
Stage A about 128 small-cell runs (about 60 s each) + 128 large-cell runs (about 90 s each) + 16 reference runs = about 6 h; Stage B at most
about 3 h. Nothing else uses the Pis meanwhile (rig lock).

## 9. Stage B list (written 2026-11-19 after Stage A, before Stage B started)
Stage A (272 runs, none lost): `tickle` as shipped has LOSE or VOID on rel_large p50/p99 in every profile at 1 MiB and in S1/S2 at 128 KiB;
`tickle-pace` leads. Small cells have no `tickle` LOSE (be_small: no verdict beyond TIE; rel_small: WIN or VOID, the VOID being Fast DDS noise),
so they are not repeated. Stage B: rel_large 1 MiB, all four profiles, rounds 3..5 (10 pooled runs per framework); 128 KiB, all four
profiles, rounds 3..4 (8 pooled). About 4 h, above the 3 h estimate of section 8: the list above is longer than assumed; 128 KiB is capped at
8 runs to bound the cost.

## 10. Stage C pacing sweep (registered 2026-11-19, committed before any Stage C run)
**Question (the user, 2026-11-19: "apply pacing and check that we beat all DDS"):** with the existing documented link options and no library, wire,
API or default change, does TickLE beat BOTH Fast DDS and CycloneDDS in every cell of this emulated link? Stage A/B found the shipped (unpaced)
default loses at 1 MiB and that `pacing_rate` = 75 % of the carried rate mostly wins; its remaining LOSEs were S1 against CycloneDDS (1 MiB p50
2979 v 2514 ms, 128 KiB p50 362 v 307 ms). Stage A used rounded rates (22 / 3 Mbit/s = 74 % / 76 %); Stage C uses exact fractions.

**Arms** (TickLE, WIFI profile as before, `large_fragmentation` AUTO, `link_rate` declared 54 Mbit/s): `tickle-p75`, `-p85`, `-p90`, `-p95` =
`pacing_rate` of 75/85/90/95 % of the calibration's carried rate (M 29.73 Mbit/s -> 22.30 / 25.27 / 26.76 / 28.24; S 3.97 -> 2.98 / 3.37 / 3.57 /
3.77 Mbit/s), `pacing_burst` default (4 MTU frames, 6152 B). One larger-burst arm, `tickle-p90-b24608` (16 frames, 24 608 B), **at 1 MiB only**:
justified because the S queue holds 40 packets (about 59 kB) so 16 frames fit, a bigger bucket could cut the start of each sample's tail; Stage
A's pacing sweep of the earlier radio runs found no burst effect, so one arm, not a sweep. No other option (`large_fragmentation` stays AUTO).
**Profiles** M1 M2 S1 S2 (settings in section 3, unchanged, same calibration). Selection is **per class** (M = M1+M2, S = S1+S2): one rate per
class, because a user sets the rate from the measured carried rate, not per loss pattern.

**Phase 1, selection (the tuning set):** `rel_large` 128 KiB and 1 MiB, 4 rounds x copies A and A2 = **8 runs per arm** and profile, fresh
runs, written into the Stage A directories (`A/rl128k`, `A/rl1m`) so the unchanged DDS runs sit beside them. 128 KiB: 4 arms x 4 profiles x 8 =
128 runs; 1 MiB: 5 arms x 4 x 8 = 160 runs (about 85-95 s each, about 7 h). The small cells are not swept (a 100 B sample at 100 Hz is far below any
pacing rate; checked in Phase 2).
**Selection rule (fixed now):** for each class, arm score = mean over (its two profiles x both sizes x {p50, p99}) of (arm median / smallest arm
median in that profile, size, metric); only arms with median delivery 15/15 and no run below 14/15 in every cell of the class qualify (else the
class has none and is reported); lowest score wins, ties to the lower rate; the burst arm may replace its rate-matched `p90` only if its score is
at least 10 % lower. Per-arm VOIDs (A/A: |median A2 - median A| > 10 % of median A) are shown and count as their medians but are listed.
**Phase 2, confirmation (the scoring set, fresh runs):** the chosen arm of each class, again with the 4 profiles, in **fresh runs never used for
the selection**: rel_large 128 KiB and 1 MiB and rel_small and be_small, **10 runs** (5 copies A + 5 copies A2) per cell and profile, directory
`C/confirm_*`. 4 profiles x 4 cells x 10 = 160 runs (about 3 h). Only Phase 2 may support a WIN/TIE/LOSE claim; Phase 1 numbers are tuning data.
**DDS arms are reused, not rerun:** CycloneDDS 0.10.5 and Fast DDS 2.14.6 runs of Stage A/B (same link, same binaries, same command lines, same
profiles; 1 MiB 10 runs, 128 KiB 8, rel_small and be_small 4 per profile). The verdict code is `wtc_read.py` with `EXTRA_ROOTS` pointing at the
Stage A/B directories; a verdict against the 4-run small-cell DDS samples is stated as resting on 4 runs. Rules as section 6 (WIN = every TickLE
run strictly beats every DDS run; A/A noise VOID; a sample with fewer than 8 runs per side is labelled "few reps").
**Analysis:** every cell where the confirmed arm is not a WIN against both DDS stacks: counters (tx datagrams, `retransmitted`, `pacing_waits`,
`pacing_repairs_deferred`, `large_duplicate`) of its runs against the best DDS, and whether pacing alone can win it; if not, what a library-side
adaptive limiter would have to do. A cell that stays VOID with 10 runs is reported as not decidable at this sample size, not as a result.
**Cost:** about 10 h in all. Nothing else uses the Pis (rig lock).

### 10.1 Selection result (written 2026-11-19 after phase 1, before any phase 2 run; `wtc_select.py`, output `~/rig_results_safe/wtc_20261117/C1_selection.txt`)
Phase 1: 288 runs (128 KiB 128, 1 MiB 160), 0 lost, 0 aborts. Rule applied as registered. Scores (mean normalised p50/p99 median, 1.0 = best arm
everywhere): class M: p75 1.239, p85 1.203, p90 1.039, **p95 1.034**, p90-b24608 1.017 (the burst arm needs a score at least 10 % below p90's:
1.017 v 1.039 is 2 %, so it does not replace it); class S: p75 1.248, p85 1.114, p90 1.053, **p95 1.011**, p90-b24608 1.046. All arms deliver 15/15 in
every cell. **Chosen: `tickle-p95` for class M (28.24 Mbit/s) and class S (3.77 Mbit/s).** The p50/p99 fall monotonically with the rate in every
cell, so the best rate is at the edge of the swept range (95 %), not bracketed: the sweep does not show that 95 % is the optimum, only that nothing
tested beats it; a higher rate (97-100 %) is not tested here and is reported as an open question. Phase 2 confirms `tickle-p95` on fresh runs
(5 rounds x copies A and A2 = 10 runs per cell and profile; be_small, rel_small, rel_large 128 KiB and 1 MiB; 160 runs) against the unchanged
Stage A/B DDS runs.
