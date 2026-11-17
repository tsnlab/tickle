# T1-Pi pacing sweep: plan (registered 2026-11-15, committed before any measured run)

**Simulation only.** `mac80211_hwsim` + the patched `wmediumd` on the two rig Raspberry Pi 5 (topology, safety rules, arms, noise rules:
`T1_PI_PLAN.md`, unchanged). No real wireless hardware (the user, 2026-11-08). Every figure is relative inside this model; hwsim has no
airtime, so a "good pacing rate" here is a property of the simulated link's frame queue, not of any real channel.
Branch `ab/t1-pi-pacing` (from `ab/t1-pi`, merged with `ab/rel-large-t1` for the `TT_PACING_BPS` knob). **No library, wire, API or default
change.** The user's proposal (2026-11-15): instead of a library change, the user sets the existing Wi-Fi link options (`pacing_rate`,
`pacing_burst`, `large_fragmentation`) and the project documents it. This plan measures what that advice should say.

## 1. Why
Seen so far (PC two-hop T1, not Pi): a 128 KiB sample sent unpaced is 91 back-to-back datagrams; the simulated link tail-drops back-to-back
bursts (about 11-15 of 91 arrive), TickLE's repair rounds are bursts too, so p50 was 4.5-8.7 s against Cyclone 1.0-1.6 s; with
`pacing_rate` 8 Mbit/s p50 was 0.14 s. A single unreplicated sweep: 3: 1.39 s, 5: 0.21, 8: 0.14, 12: 1.21, 24: 0.45 (non-monotonic).
On the Pi (11-14 matrix) 1 MiB `rel_large` delivered 6 of 15 for TickLE in some runs at o60/o200 while Fast DDS gave 15/15; unexplained.

## 2. Sweep (stage 1 and 2)
Cell = (size, arm, configuration). Sizes: **1 MiB and 128 KiB**, 0.25 Hz, 15 samples, RELIABLE KEEP_LAST 10, `-P wifi` (WIFI profile, one link),
`TT_UDP_OFFLOAD=0` (as the 11-14 matrix), sub started first, publisher on the STA (rpi-1), subscriber on the wired server (rpi-0). Arms
`ctl`, `o60`, `o200` (Gilbert-Elliott profiles of `T1_PI_PLAN.md` section 2, `RETRY=1 SEED=7`, wmediumd on core 2). Metrics per run: delivered of
15 (the sub runs 8 s past the last publish, as in the 11-14 matrix), one-way latency p50 and p99 over the delivered samples (clock offset measured
over the wire before and after, `t1pi_clock.py`), `retransmitted`, `large_send_waits`, `pacing_waits`, `pacing_repairs_deferred`, rx/tx datagrams.
Undelivered samples count as "not delivered", never as a latency; a run with no delivered sample has p50 = infinity.

**Reps:** 3 rounds x copies A and A2 = **6 runs per cell** (>= 5). Order inside a (size, arm) block: the list of [cfg.A for every cfg, cfg.A2
for every cfg] rotated left by 5 x (round - 1) (the t1pi_fw.sh rule, ROTATE=5), a fresh topology and fresh processes per run.

**Stage 1 (rate, burst default = 4 frames of the MTU = 6152 B):** configurations `off` (= `tickle`, the library default, as 11-14) and
`pacing_rate` = 2, 4, 6, 8, 12, 16, 24 Mbit/s (`tickle-r2` ... `tickle-r24`; the bench declares `link_rate` 54 Mbit/s so the value is valid).
8 configurations x 3 arms x 2 sizes x 6 = 288 runs.

**Stage 2 (chosen from stage 1 by the rule of section 3, fixed now):** the two best rates R1, R2 (lowest "score", section 3), then
* **burst:** `pacing_burst` = 1538 B (the minimum, one frame) and 24608 B (16 frames) at R1 and at R2 (the default 6152 B is stage 1): 4 configurations;
* **fragmentation:** `tt_LARGE_FRAGMENTATION_IP` (`-F ip`, as many units a datagram as the peer allows, IP-fragmented) and `_MTU` (`-F mtu`, one unit a
  datagram) at `off` and at R1: 4 configurations (the default AUTO resolved to MTU units for these samples in the 11-14 runs, hybrid_mtu_units>0).
8 configurations x 3 arms x 2 sizes x 6 = 288 runs. Estimate: about 85 s a run, stage 1 about 6.8 h, stage 2 about 6.8 h.

## 3. Reading rules (fixed before the run)
* **Cell statistics:** median over the 6 runs of delivered, p50, p99; min/max shown. **Noise (the Pi plan):** a cell is VOID for a metric when
  |median(A2) - median(A)| > 10% of median(A) (delivery: 10% of the delivered percentage). A VOID cell is shown and may not make a configuration
  "best"; it does not by itself exclude a configuration from the band if the other copy set and the pooled runs satisfy the band rule (reported).
* **Best of a (size, arm):** the non-VOID configuration with the lowest median p50 among those with median delivered >= 14 and worst run >= 12.
* **In band for a (size, arm):** median p50 <= best median p50 + max(10% of it, half the best cell's (max - min) over its runs); median delivered
  >= best median delivered - 1; **and** worst-run delivered >= 12 (the pilot rule of the Pi plan). A rate is **in the recommended band** only if it is in band
  for **all six** (size, arm) pairs. The **recommended range** = the largest contiguous run of tested rates that are in the band; if the in-band
  set has gaps (non-monotonic) that is stated and the contiguous run is the recommendation, never the single best point. If no rate qualifies, no
  band is recommended and the closest rates and their failures are reported.
* **Score** (for choosing R1, R2): per rate, the mean over the six (size, arm) pairs of (median p50 of the rate) / (median p50 of that pair's best);
  rates not meeting delivered >= 14 median in a pair are scored with a x10 penalty for that pair. Lowest two; ties to the lower rate.
* **Burst / fragmentation** are reported against the same configuration's default (burst 6152 / fragmentation AUTO); a difference counts only when
  it exceeds the cell noise above. They do not change the band; they become advice only if they widen it or make a rate robust that was not.
* **Falsifiers:** (a) if no rate beats `off` on median p50 in `ctl` at 128 KiB by more than 2x, the root-cause claim "bursts are tail-dropped, pacing helps" is
  not confirmed on the Pi topology; (b) if `off` delivers 15/15 in 6/6 runs of every 1 MiB arm, the 6-of-15 finding is not reproduced (reported as such).

## 4. Why 1 MiB under loss delivers 6 of 15 (diagnosis, task 2)
Hypotheses, each with its measurement and what refutes it:
* **H1 burst tail-drop + one repair round a second** (the 128 KiB mechanism, scaled: 712 units a sample, ~11 land per burst, so a sample needs tens
  of rounds, rounds are ~1 s apart; the backlog grows until the run ends): pcap in `t1p_sta` (hwsta) and in `t1p_srv` (t1pgs) of an unpaced
  1 MiB `o200` and `ctl` run, 4 runs, units sent / units received per 100 ms, the NACK times (small datagrams from the subscriber), burst size per NACK;
  node counters. Refuted if the per-burst arrival fraction is high (> 50%) and the repair stalls for another reason.
* **H2 reassembly slot limit** (`tt_LARGE_ASSEMBLIES`): `large_no_buffer`, `large_dropped`, `assemblies_held` on the sub; refuted when 0 in the bad runs.
* **H3 timeout / abandon** (`large_abandoned`, `large_tail_abandoned`, `retry_giveups`, `giveups_suppressed_unknown` on the sub); refuted when 0.
* **H4 repair window** (the writer's history of 10 samples retained, `retransmit` deferrals, `evict_count`, `oversize`): refuted when 0.
* **Run-time truncation** (the sub stops 8 s after the last publish: "delivered 6 of 15" may be a backlog, not a loss): per-sample arrival times from the sub.
The 36 unpaced 1 MiB runs of the sweep (6 per arm, plus 128 KiB) supply the counters of every run, not only the bad ones; pacing "fixes it" if the
in-band rates deliver >= 14 of 15 in every run of the three arms at 1 MiB where `off` has runs below 12.
Diagnostic runs are unscored and use `t1pi_diag.sh` (same topology and lock, tcpdump with `-s 96` in both namespaces, pcaps kept under the results dir).

## 5. Final comparison (stage 3, task 3), after the band is known
Configurations: **TickLE paced** (`tickle-r<R>`, R = the lowest-score tested rate inside the recommended band; if the band is empty the best rate and the
fact is stated), **TickLE unpaced** (`tickle`, same-session reference, never scored), **CycloneDDS 0.10.5**, **Fast DDS 2.14.6** (the Pi rig's ROS 2
jazzy builds, nodes and XML of the 11-14 matrix, unchanged). Cells `rel_large` at 1 MiB (the Pi rig's size by the pilot rule) and at 128 KiB, and
`rel_small` (100 B, 100 Hz, 60 s, RELIABLE KEEP_LAST 100, RTT at the ping), arms ctl/o60/o200, 3 rounds x A/A2 = 6 runs per framework-cell-arm,
rotated order. Verdict rules exactly as the 11-14 matrix (`t1pi_fwread.py`): WIN = every paced-TickLE run strictly beats every vendor run, LOSE = the
reverse, TIE otherwise, VOID when a framework's A2/A medians differ > 10%; metrics: rel_large delivery, p50, p99; rel_small delivery, p50, p90, p99.
216 runs, about 5 h. Paced TickLE is a named configuration with its rate in the table; the unpaced row stays beside it so nothing is claimed
about the default.

## 6. WIFI-profile default (not changed here)
The evidence is reported with a recommendation. Pre-registered criterion for recommending a default change: a band exists that contains the same
rate in all six (size, arm) pairs, paced beats unpaced on rel_large in every arm at both sizes, and rel_small paced is not worse than unpaced
(TIE or better on every metric). Even then a fixed bit rate is link-specific (a Wi-Fi link has no fixed rate; the T1S profile takes 75% of a fixed
10 Mbit/s), so the reported recommendation distinguishes "document the option" from "change the default"; the user decides.

## 7. Safety, data, deliverables
Rig lock first (`flock /tmp/rig.lock` + `rig_lock.sh`, taken inside the detached process); the runner snapshots `wlan0`/`eth0`/modules/phys/namespaces
before the session and compares after every run (any difference stops the list); clean teardown in a trap; Pis left as found (no hwsim, no
`t1p_*`, no `t1pg*`). Scripts: `examples/perf_hil/experiments/t1pi/` (`t1pi_fw.sh` with TickLE variants, `t1pi_pacread.py`, `t1pi_diag.sh`);
raw data `~/rig_results_safe/t1pi_pace_20261115/{stage1_1m,stage1_128k,stage2_*,diag,final_*}`; results `~/claude_reports/T1_PI_PACING_RESULTS_2026-11-15.md`;
docs: a "Wi-Fi tuning" section in `docs/DESIGN.md` (docs only).

## 8. Amendments
1. (2026-11-15, before stage 1 finished.) The diagnosis driver is a PCAP=1 option of t1pi_fw.sh run by t1pi_pace_chain.sh, not a separate t1pi_diag.sh; no rule changed.

2. (2026-11-15, user decision, added while stage 1 runs; no registered rule above is changed.) The documentation will tell Wi-Fi users to set
   `tt_LinkOptions.pacing_rate` and `pacing_burst`, with a **guideline** a user can apply from what they can know or measure. Fixed now, before any
   sweep result is read:
   * **Derivation rule.** The guideline is stated as a fraction of the link rate. The harness declares `link_rate` = 54 Mbit/s, so the recommended band
     (section 3) in Mbit/s divided by 54 is the band as a fraction; the guideline fraction f = the geometric middle of the band's tested rates divided by
     54, rounded to a simple number (a multiple of 5%). If the link rate is unknown the guideline says to measure the link's sustained UDP throughput
     (a short unpaced flood to a receiver on the same link, counting what arrives) and use that as the rate; the lowest sustained-throughput rule and
     any absolute fallback are taken from the stage-1 rates (a fixed Mbit/s that was in band) and stated as a fallback, not as a general value.
     Burst advice: the default (4 frames of the MTU) unless stage 2 shows the minimum or 16-frame burst is in band where the default is not
     (then it follows stage 2); the MTU is the interface MTU.
   * **Literal application.** After stage 2, a further configuration `tickle-r<f x 54 rounded to an integer Mbit/s>[-b<burst by the guideline>]` is run in
     the final comparison's cells as an extra TickLE arm ("TickLE guideline"), using only inputs a user has: the declared link rate 54 Mbit/s and the
     MTU 1500, never the sweep's best point. It runs as a separate block (`final_*_guideline`, same arms, same A/A2 rotated rounds, 6 runs per cell) after
     the chain, and is compared with the same vendor runs.
   * **Check (before the arm is analysed).** The guideline arm passes if, for each of the six (size, arm) pairs, its median p50 and median delivered meet
     the section 3 "in band" test against the sweep's best (the same noise rule) and its worst run delivers >= 12 of 15. If it fails, that is reported as
     a failed guideline and the closest tested rate; the guideline is then not published as such.
   * Because the final paced arm of the chain (`tickle-r$FINAL`) is chosen by the score rule, it stays as the "sweep-best" arm; the guideline arm is a
     third TickLE row. Both are named with their exact settings.
3. (Same date.) `docs/RESULTS.md` rows for this work state, per row, the TickLE settings (link profile, `pacing_rate`, `pacing_burst`,
   `large_fragmentation`, `repair_reveal` where not the profile's) and the DDS settings (Fast DDS 2.14.6 XML: UDPv4 only, maxMessageSize 1472, no
   whitelist; CycloneDDS 0.10.5 XML: interface by address, SPDPInterval 1 s), and say that the unpaced `tickle` row is the library default
   (profile WIFI, pacing off). The WIFI-default question stays a recommendation only. Adaptive repair-burst limiting is in `docs/ROADMAP.md`
   "Later" on `origin/ab/release-2.2.0` (4f5b0474) and is referred to, not duplicated.
4. (2026-11-17, after CHAIN_DONE, before any final row was analysed.) Stage 1 and stage 2 were read with the registered rules: **no rate is in band
   for all six (size, arm) pairs** (no 1 MiB pair has a qualifying best at all), so the amendment-2 derivation has no band to take a fraction of.
   The guideline arm (`final_*_guideline`) is therefore **not run**: a guideline cannot be derived from this sweep, and none is published as
   recommended. Not a failed check of a guideline but no guideline; the reason and the evidence are in the results file and docs. The final
   comparison keeps the three registered rows' two TickLE arms that exist: default (`tickle`) and sweep-best (`tickle-r12`, chosen by the score rule).
   The harness fault rule applied to the final run `rel_large/o60/r3_fastdds_A` (no RESULT after 3 attempts: a lost run, counted as lost). The Fast DDS
   node sources moved to `examples/perf_hil/t1fw_vendor/` (merge of `origin/ab/t1-fw-versions`); `t1pi_fwbuild.sh` builds from there; the
   `examples/perf_hil/fastdds/t1pi_fw/` copy is removed (byte-identical sources).

