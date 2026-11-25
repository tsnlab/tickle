# Re-validation of ADAPTIVE commit 1054f2ff (`ab/adaptive-next`, on top of ae5c192a), 2026-11-25

**An emulation with `tc netem` on two Raspberry Pi 5, not Wi-Fi** (netns + GRE tap; the Pis' real `eth0`/`wlan0` untouched; Pis clean afterwards, addresses unchanged). Library: `1054f2ff` merged into `ab/wifi-adaptive-s1` as tree `628a38ec` (no push to the release branch or main). The change is in CRUISE only (DESIGN section 24): a running probe survives a quiet round, and two drop trips within 4 rounds set `fast_probe`. Binaries rebuilt on both Pis (13:25). 46 runs, 0 lost, 0 aborts. Raw data `~/rig_results_safe/wtc_adaptive_reval_20261125/{RS,ST,view}`. No library change in this commit.
**What I ran:** (1) the exact rate-step run (30 -> 4 at 45 s -> 30 Mbit/s at 90 s, M1, 135 s runs) for the **new `tickle-ad` only**, 5 runs at 1 MiB and at 128 KiB; **`tickle-p95`, CycloneDDS, Fast DDS and the old `tickle-ad` are reused from `wtc_ratestep_exact_20261124`** (same harness, same DDS binaries and `large_trace`-format traces; the fixed-rate arm is not touched by a CRUISE-only change, but its binary is from the earlier tree, so it is a reused reference, not a same-session control). (2) A static test, 3 runs per cell and arm: arm A (no prior) and arm B (right prior, `tickle-ad-c30` / `tickle-ad-c4`) on M1 and M2 at 128 KiB and 1 MiB and on S1 and S2 at 1 MiB.

## 1. Before / after, rate step (old = ae5c192a, 5 runs; new = 1054f2ff, 5 runs; p50 / p90 / max ms pooled by phase, send-time attribution)
| item | old ADAPTIVE | new ADAPTIVE | verdict |
|---|---|---|---|
| **128 KiB after the step up**, p50 / p90 / max | 102 / 203 / 281 | **49 / 76 / 172** (fixed 43 / 51 / 55; CycloneDDS 55 / 69 / 218; Fast DDS 90 / 93 / 93) | **improved**: now level with the fixed rate and CycloneDDS, ahead of Fast DDS |
| 128 KiB recovery after the step up (median of 5) | 36 s | **8 s** (fixed 4, DDS 2) | **improved**, still slower than the others |
| 128 KiB before / at 4 Mbit/s | 46 / 281 | 48 / 281 (p90 98, max 404 at 4 Mbit/s) | unchanged |
| 128 KiB recovery after the step down | 1 s | 1 s | unchanged |
| **1 MiB after the step down**, recovery (median) | 17 s | **13 s** (1 of 5 runs never) | median slightly better, **but see the failed run** |
| 1 MiB at 4 Mbit/s: p50 / p90 / max | 2,266 / 9,454 / 17,999 (59/60 delivered) | 2,267 / **12,432 / 42,794** (**51/60** delivered) | **regressed in the tail** |
| 1 MiB after the step up: p50 / p90 / max | 338 / 433 / 868 (55/55) | 349 / 513 / **43,475** (**45/55**) | **regressed** |
| 1 MiB before the step | 329 / 502 / 800 | 332 / 482 / 806 | unchanged |
| tx / retx per 1 MiB run | 30,449 / 2,726 | 30,151 / 2,887 | unchanged |
**Per-run detail, 1 MiB new ADAPTIVE:** runs 1, 3, 4 are clean (p50 in the slow phase 2,261-2,267 ms; slow-phase max 3.2 s, 4.8 s, 7.8 s; after the step up max 0.4-1.1 s). **Run 2 failed: after the step down it delivered 5 of 12 samples in the slow phase and 1 of 11 after the step up (p50 41 s and 43 s), ended at 2.2 Mbit/s, 8,425 retransmissions, 34,331 datagrams.** Its trace shows the queue guard (`qover` = 1) held for most of the run after the step down, `held` trailing `units_sent` by several hundred units, the rate sitting at 4-9 Mbit/s and ending at 4.3 Mbit/s in the 30 Mbit/s phase: it did not climb back after the step up, which is exactly what the change was meant to fix. Run 5 lost 2 samples in the slow phase (max 12.4 s). The old ADAPTIVE had no such run in 5 (one undelivered sample in the slow phase).
Rate-step verdicts of the new ADAPTIVE (strict rule on per-run phase p50): 1 MiB against CycloneDDS WIN / TIE / TIE (before / at 4 Mbit/s / after: the "after" TIE is because of the failed run), against Fast DDS WIN / TIE / TIE, against fixed 95 % of 30 TIE / TIE / LOSE; 128 KiB against CycloneDDS TIE / TIE / TIE (49 against 55 ms after), against Fast DDS WIN / WIN / WIN, against fixed 95 % TIE / LOSE (281 against 279 ms) / TIE. The old ADAPTIVE had 1 MiB WIN / TIE / WIN against both DDS stacks; the failed run turned the "after" WINs into TIEs.

## 2. Static cells (3 runs each; p50 / p99 ms medians; queue-guard trips and stall restarts per run)
| cell | arm | new (1054f2ff) delivered, p50 / p99 [max], queue trips (median), stall | before (v8 / quick10 / quick11) | verdict |
|---|---|---|---|---|
| 128 KiB M1 | A | 15/15, 48 / 98 [108], 1, 0 | v8 52 / 101 | unchanged |
| 128 KiB M2 | A | 15/15, 93 / 221 [**1,196**], 0, 0 | v8 99 / 261 | unchanged (one p99 outlier) |
| 1 MiB M1 | A | 15/15, 357 / 570 [698], **12**, 0 | v8 342 / 535; quick11 A: 321 / 561, 4 queue trips | p50, p99 unchanged; **queue trips about 3x** |
| 1 MiB M2 | A | 15/15, 479 / 1,046 [1,061], 2, 0 | v8 528 / 1,147; quick11 534 / 1,825 | unchanged or slightly better |
| 1 MiB S1 | A | 15/15, 2,267 / **4,024 [6,359]**, 14, 0 | v8 2,263 / 2,447 | p99 **worse** (+64 %) |
| 1 MiB S2 | A | 15/15, 3,172 / **6,734 [8,807]**, 5, 0 | v8 2,797 / 5,944 | p50 +13 %, p99 +13 %: slightly worse |
| all B cells | B | M1 1 MiB 322 / 333, M2 356 / 416, S1 2,388 / 2,429, S2 2,516 / 2,776; 128 KiB M1 44 / 54, M2 81 / 129; swing 1.0x, 0 drop trips, 0 stall | fixed 95 %: 323 / 334, 367 / 488, 2,374 / 2,402, 2,497 / 2,660, 44 / 55, 79 / 135 | **arm B still equals fixed 95 %** (p50 within 1-2 %, p99 within 3-4 % except noise) |
Stall restarts: **0 in all 46 runs** (old rate-step ADAPTIVE: 0 at 1 MiB in v8). Queue-guard trips in arm A static: 12 / 2 / 14 / 5 at 1 MiB (M1 / M2 / S1 / S2), 0-1 at 128 KiB (v8-era quick tests showed 3-6 on M1 and none recorded on S in the same form); the drop trips (30-69 on S1) are of the old order. Delivery 15/15 in all 36 static runs.

## 3. Plain statement
- **Improved:** 128 KiB after the step up: p50 102 -> 49 ms, recovery 36 -> 8 s (the target, met); the new ADAPTIVE is now as fast as CycloneDDS and ahead of Fast DDS there.
- **Did not improve:** 1 MiB recovery after the step down (17 -> 13 s median, within noise of the run-to-run spread, with 1 of 5 runs never recovering); static arm A on M1 / M2 (same latency, more queue-guard trips on M1); 1 MiB S1 / S2 tails (the arm A p99 is worse than v8 by 13-64 %).
- **Regressed:** **1 MiB rate step: 1 of 5 runs failed completely (delivered 5/12 and 1/11 in the two later phases, p50 41-43 s; the rate did not climb back after the step up)**, against 0 failed runs in 5 for the old ADAPTIVE. The pooled slow-phase max went from 18 s to 43 s, the after-step-up max from 0.9 s to 43 s. With 5 runs per arm, 1 failure in 5 is not distinguishable from a lower failure rate, but it is a failure the old build did not show in the same 5 runs and is the one the change should have prevented.
- **Unchanged:** arm B equals the fixed 95 % rate; no stall restarts; the fast-phase 1 MiB latencies (about 330-350 ms); datagram and retransmission counts per 1 MiB rate-step run.
- **Verdict on the commit:** the 128 KiB fix works; the 1 MiB rate step is not yet safe (one catastrophic run in 5), so ADAPTIVE as an opt-in would carry a known failure mode. Repeating the 1 MiB rate step with 10-15 more runs would say whether the failure rate is 1 in 5 or lower; the trace of run 2 (`RS/rl1m/rel_large/M1/r2_tickle-ad_A.near`, RATETRACE lines) shows the queue guard stuck on after the step down and the rate not recovering after the step up. Not changed: no library, wire, API, default, merge or tag here.
