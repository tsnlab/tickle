# Adaptive rate control, Stage S1 on the tc-emulated link (results, 2026-11-20)

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi.** Branch `ab/wifi-adaptive-s1` (from `origin/ab/adaptive-pacing` `b9587eaa`, library built on rpi-1 by `t1pi_build.sh`, tree `b9587eaa`). Plan: `FULL_MATRIX_PLAN.md` stage S1, `ADAPTIVE_PLAN.md`. TickLE arm `tickle-ad` = `rate_control = ADAPTIVE` (`TT_RATE_CONTROL=adaptive`, WIFI profile, defaults for cap, floor and queue target); control `tickle-lg` in the small cells. Fast DDS 2.14.6 and CycloneDDS 0.10.5 are the Stage A/B runs of the same link, binaries and command lines (reused, as the plan says). 240 runs (1 MiB 40, 128 KiB 40, small cells 160), 10 per arm, profile and cell; 0 lost, 0 aborts, 0 snapshot changes; Pis clean afterwards (no `wtc_*` namespace or tap, `eth0` qdisc and addresses as before, no hwsim-style modules). Raw data `~/rig_results_safe/wtc_adaptive_20261120/D_all`, tables `results/tables_S1_*.md`.
No library, wire, API or default change was made here.

## 1. Verdict: ADAPTIVE as built does not meet the criteria; it fails at 1 MiB
| cell (WIN / TIE / LOSE / VOID over metric x profile x {CycloneDDS, Fast DDS}) | tickle-ad |
|---|---|
| rel_large 1 MiB (24) | **0 / 8 / 6 / 10** |
| rel_large 128 KiB (24) | 1 / 8 / 1 / 14 |
| be_small + rel_small (64) | 38 / 13 / 0 / 13 (no regression: same as Stage A / C) |
**1 MiB: TickLE ADAPTIVE delivers 0-2 of 15 samples in every one of the 40 runs (M1 13 %, M2 13 %, S1 0 %, S2 3 %), against 15/15 for CycloneDDS everywhere and for the shipped default and the fixed 95 % rate of Stage A/B/C.** The 6 LOSEs are all 1 MiB S1 (delivery 0 % v 100 % and 53-60 %; p50/p99 no sample). The TIEs at M1/M2 are p50 over 1-2 delivered samples (not evidence of speed: delivery VOID). 128 KiB: delivery is 15/15 at M1 but 5-15 of 15 at M2, 4-15 at S1 and 2-10 at S2; the one LOSE is M1 p99 against Fast DDS (402 ms against 91.9 ms), and M2/S1/S2 are VOID on A/A (p50 541 ms v CycloneDDS 307 ms at S1, 1250 v 439 ms at S2). So against the plan: A6 (no LOSE) FAILS, A5 (oracle reach) FAILS, A7 passes, A8 not applicable.
Smoke run (128 KiB, S1, one run) looked right (15/15, `btlbw_bps` 4.15 Mbit/s on the 3.97 Mbit/s link) and was not representative: the failure shows after the first few losses.

## 2. Why (counters of the `tickle-ad` publisher, medians of 10 runs; Stage C `tickle-p95` for comparison)
| cell | runs that ended at the floor rate (100 kbit/s, `btlbw_bps` = 0) | tx datagrams (ad / p95) | retransmitted (ad) | rtprop_ns min / median / max |
|---|---:|---:|---:|---|
| 128 KiB M1 | 0 / 10 | 2151 / 1585 (x1.36) | 20 | 0.011 / 0.035 / 3.9 ms |
| 128 KiB M2 | 4 / 10 | 2982 / 1658 (x1.80) | 42 | 0.011 / 0.076 / 4.2 ms |
| 128 KiB S1 | 1 / 10 | 3070 / 1560 (x1.97) | 122 | 0.011 / 0.032 / 3.3 ms |
| 128 KiB S2 | 10 / 10 | 7444 / 1693 (x4.40) | 198 | 4.5 / 4.5 / 4.5 ms |
| 1 MiB M1 | 10 / 10 | 7765 / 11156 (x0.70) | 5376 | 0.010 / 2.2 / 6.9 ms |
| 1 MiB M2 | 10 / 10 | 6562 / 11396 (x0.58) | 2034 | 0.26 / 0.88 / 4.2 ms |
| 1 MiB S1 | 10 / 10 | 2355 / 10838 (x0.22) | 572 | 0.15 / 1.9 / 9.4 ms |
| 1 MiB S2 | 10 / 10 | 5692 / 11682 (x0.49) | 420 | 0.24 / 1.8 / 5.1 ms |
1. **The estimate collapses to the floor and stays there.** In every 1 MiB run, and in 15 of 40 128 KiB runs, the publisher ends with `rate_bps` = 100 000 (the floor, `tt_RATE_MIN_DEFAULT`) and `btlbw_bps` = 0, after hundreds of `rate_drop_trips` and queue trips (e.g. 1 MiB S1: 424 drop trips, 54 queue trips, `pacing_repairs_deferred` 190 912). At 100 kbit/s one 1 MiB sample takes about 84 s; the subscriber stops 8 s after the last publish, so 0-2 samples arrive, and the datagram count is far *below* what is needed (x0.22-0.70 of the oracle's): this is starvation, not a retransmission storm. The controller recovers in no run once it is at the floor.
2. **`rtprop_ns` does not measure the path round trip.** The links' round trips are 10 ms (S1, M1) and 40 ms (M2, S2); the reported minimum is 0.010-0.26 ms (the 11.7 us of the smoke run) in 6 of 8 cells and 4.5 ms (S2) elsewhere: always below the physical 2 x delay, so it cannot be a round trip. By the implementation (`rate_acknack` in `src/tickle.c`): the sample is `now - solicit_ns` for the first ACKNACK after a solicitation, and "an ACKNACK that arrives for another reason before the answer is read as the answer"; the minimum filter keeps the least of them, so any early, unsolicited ACKNACK (or a solicit time that is not the send time of the datagram being answered) sets RTprop to microseconds and it never rises. A too-small RTprop makes the queue target (`rtprop / AP_QUEUE_TARGET_RTT_DIV`, minimum `AP_QUEUE_TARGET_MIN_NS`) and the bandwidth-delay product tiny, which is consistent with the queue trips (hundreds per run) and the collapse in point 1. This is a diagnosis from counters and the source, not a verified root cause; the cheapest check is a pcap of one 1 MiB run against the sender's `solicit_ns`/ACKNACK times.
3. At 128 KiB the same mechanism is partial: M1 is unaffected (45.8 ms p50 against `tickle-p95` 43.8 ms, +4.6 %; p99 402 ms against 54.7 ms because some runs lose a window), M2-S2 show runs that fall to the floor (4, 1 and 10 of 10) and deliver 2-10 of 15.
## 3. Oracle gap and datagram ratio (A5, A8)
- 128 KiB M1 (the only cell in which ADAPTIVE behaves): p50 45.8 ms against the oracle's 43.8 ms (`tickle-p95`, Stage C) = +4.6 % (within the proposed X = 10 %); p99 402 against 54.7 ms = x7.3 (outside 25 %; the p99 of 15 samples is the maximum, A/A noise on ADAPTIVE).
- Every other cell: no gap can be computed (delivery 0-13 % at 1 MiB; 128 KiB median delivery below 15 of 15). Datagrams per run against the oracle: x1.36-4.4 at 128 KiB (retransmission-heavy where the rate fell), x0.22-0.70 at 1 MiB (starved): A8's "within 1.25x" fails at 128 KiB in all four profiles and is meaningless at 1 MiB.
## 4. Small cells (A7)
be_small p50 WIN over both DDS stacks in all four profiles (10.28 / 40.28 / 10.70 / 40.69 ms), delivery TIE; rel_small p50 WIN over CycloneDDS, VOID against Fast DDS (its own spread), delivery TIE or WIN. Counts 38 / 13 / 0 / 13, the same family as Stage A and C: the pacer is bypassed there and nothing regressed. (The `tickle-lg` control and the shipped default were run in the same chain for the small cells and read alike; see the tables.)
## 5. What this means and what is needed
- The adaptive controller needs a fix before any wider stage (S2-S7) is worth running: (a) RTprop must be a true round-trip sample (match the answer to the datagram that was solicited; discard unsolicited ACKNACKs as samples) or be bounded below by something physical; (b) the rate must not stay at the floor after `btlbw` reads 0: a recovery path (probe up after a quiet interval, or never drop below a fraction of the last good estimate); (c) check why loss rounds register as queue/drop trips at M1 (30 Mbit/s, 1 % loss) where the link is not saturated. These are library changes for the implementer; none was made here.
- Nothing in this stage invalidates the earlier result that a fixed 95 % rate (`tickle-p95`) wins or ties every cell; it only shows the estimator does not yet find that rate.
- Stage S1 is not "passed"; A6 and A5 fail. S2-S7 should not start until a fixed build passes a re-run of S1 (the 1 MiB cells are enough to show it: 15/15 delivery first).
## 6. Sample size and limits
10 runs per cell for `tickle-ad`; the DDS arms are the 8-10 runs of Stage A/B (4 in the small cells), reused (same link, binaries, command lines, an earlier session). Many ADAPTIVE cells are A/A VOID because the runs are bimodal (rate collapsed or not); more reps would not change the verdict at 1 MiB (0-2 of 15 in 40 of 40 runs). One fixed-rate, drop-tail, one-flow netem link; not Wi-Fi.
