# Wi-Fi-like link: the full experiment matrix (plan, 2026-11-19, committed before any run)

**Scope.** User direction 2026-11-19: the Wi-Fi emulation must use `rate_control = ADAPTIVE` (the docs will tell Wi-Fi users to set it; implementation by the
adaptive-pacing agent on `ab/adaptive-pacing`, not finished) and the campaign must cover **every experiment that was run on the wired rig**, including the
payload sizes P1 76 B, P2 1292 B, P3 1424 B, P4 2800 B and the QoS combinations. This document lists them, says for each whether it runs on the emulated link
as it is, needs an adaptation, or is not meaningful there (with the reason), fixes arms, profiles, reps and scoring, estimates the Pi time stage by stage so
the user can approve or cut, and states what ADAPTIVE must show first. **Plan only: no run is started, no Pi is used, no library, wire, API or default change.**
**The link is an emulation with `tc netem` (rate, delay, loss, queue) on two Raspberry Pi 5, not Wi-Fi** (no MAC retries, no rate adaptation, no airtime sharing);
no real wireless hardware is used (user, 2026-11-08). Sources: `docs/RESULTS.md` sections 1-2 (rows S1-S19, 1c-75, L4, SB2, HY, D2, NS), `docs/TESTING.md` sections 3-6,
`WIFI_TC_PLAN.md`, `WIFI_TC_RESULTS_2026-11-19.md`, `WIFI_TC_PACING_SWEEP_2026-11-19.md`, `examples/perf_hil/experiments/adaptive_pacing/DESIGN.md` (branch `ab/adaptive-pacing`).

## (a)+(b) Every wired cell, and what it becomes on the emulated link
Legend: **RUN** = as is on the link; **ADAPT** = needs the change named; **N/M** = not meaningful on a netem link, reason given (nothing is dropped silently; every N/M row is
reported in the final table as "not run, reason"). "Profiles" are defined in (d).

### Same host, shared memory (RESULTS section 1)
| wired rows | metric | on the emulated link |
|---|---|---|
| S1-S3, S4-S6 | same-host goodput BE/RELIABLE P2-P4 | **N/M**: two processes on one host through shared memory; no link is involved, netem cannot act. Unchanged by this campaign (the wired table stays the evidence). |
| S7c-S10c, S12, S18 | same-host RTT from creation, CPU per round trip | **N/M**, same reason |
| S11, S13, S17, S19 | same-host CPU / RSS | **N/M**, same reason |
| S14 | wire bytes per sample (0) | **N/M** |
| S15, S16 | mixed: one publisher, a subscriber on its host and one across the link | **ADAPT**: the remote half runs on the link (publisher on rpi-1 with a local subscriber on rpi-1 and a remote one on rpi-0 through the emulated link, 90.7 samples/s as wired); the local half is unchanged by the link and is only checked for "local delivery unaffected" (loss 0, latency within the wired figure). Arms: TickLE OFF/ADAPTIVE, Fast DDS, CycloneDDS. Part of stage S3. |

### Cross host (RESULTS section 2)
| wired rows | metric | on the emulated link |
|---|---|---|
| 1c, 3c, 6ac, 6bc | RTT p50 from creation, P1 76 B / P2 1292 B / P3 1424 B / P4 2800 B | **RUN** on every profile. P4 is IP-fragmented (2 datagrams), which a tail-drop queue punishes; reported as a result. |
| 2, 4 | RTT max (tail) P1, P2 (send stamp) | **RUN**; the tail over a 15-30 s window is the maximum of ~1500-3000 samples |
| 5c, 6 | +10 ms netem | **ADAPT (folded)**: delay is part of every profile (5 and 20 ms one way); the separate +10 ms cell would measure the profile's delay again. Reported as covered by 1c/2 on M1 vs M2. |
| 7-10 | RELIABLE goodput P1-P4 | **ADAPT**: the wired figures are CPU- or 1 GbE-bound (up to 943 Mbps); on a 30 or 4 Mbit/s link the ceiling is the carried rate (29.73 / 3.97 Mbit/s IP). Same cell, goodput = payload Mbps; a cell where every framework sits at the link ceiling is a DRAW (the wired `‡` rule). The P1 cell stays CPU-bound at ~111 Mbps only if the link allows it, so on S it is a link-ceiling cell too. |
| 11, 11a-c | BEST_EFFORT goodput P1-P4, max rate | **ADAPT**: same; offering the max rate into a drop-tail queue is the test (loss at the queue); goodput = payload of complete unique samples (wired G1-G6). |
| 12 | RELIABLE KEEP_LAST 64, P1 | **RUN/ADAPT** as 7 |
| 13, 14 | RELIABLE P1 / P4 at 5 % loss | **ADAPT**: profiles L5 (30 Mbit/s, 5 ms, 5 % independent loss) and S5 (4 Mbit/s, 5 ms, 5 %) replace the wired `tc` 5 % loss |
| 15 | RELIABLE P1, 5 % reorder | **RUN** on profile M1 + netem reorder as the wired cell's `tc` string (copied from `campaign_sweep.sh` when the harness is written). Reordering by netem is not a Wi-Fi behaviour (a MAC delivers in order); kept because the wired matrix has it, labelled so. |
| 16, 17 | kept at 5 % loss / unshaped, P1, P4 | **ADAPT**: ratio of the L5 (S5) goodput to the same cell on M1 (S1)'s unshaped carried rate, same cells as 13, 14 |
| 18-27 | CPU per Msample, client/server, P1/P2/P4, latency, 5 % loss | **RUN, derived**: measured inside the runs of rows 7-14 and 1c-4 (`getrusage` in the RESULT line), as the wired `D` campaign did; no extra runs. CPU is per delivered sample, so a link-limited run reports retransmission cost, which is the interesting number here. |
| 28-34 | peak RSS | **RUN, derived** from the same runs |
| 35-41 | wire bytes per sample, framing | **RUN, derived**; on the link includes retransmissions, so it reports repair overhead per delivered sample (the wired 5 % loss rows 39, 40 are the nearest) |
| 42 | DURABILITY late join | **RUN** (profiles M1, S2) |
| 43, 44 | HISTORY depth 8 / beyond depth 8: age at take | **RUN** (M1, S2) |
| 45 | DEADLINE 50 ms | **RUN** (M1, S2); at 20 ms one-way delay a 50 ms deadline is near the RTT: reported as is |
| 46 | LIVELINESS lease 2.0 s | **RUN** (M1, S2) |
| 47 | LIFESPAN 100 ms expiry | **RUN** (M1, S2); delay 20 ms is inside the lifespan |
| 48c-51c | rmw RTT, block wait, Bench 64 B and Array1k, BEST_EFFORT / RELIABLE | **RUN** on M1, S2 (rmw_tickle, rmw_fastrtps, rmw_cyclonedds) |
| 52c-67c | rmw RTT with busy poll and 50/100/200 us poll sleeps | **N/M**: a 50-200 us poll sleep or the busy poll changes the RTT by microseconds; the link adds 10-40 ms of round trip, so the cells would measure the link, not the wait mode. Reported as not run; the block-wait cells (48c-51c) carry the rmw-layer comparison. |
| 68-71 | rmw RSS, pong CPU | **RUN, derived** from 48c-49c runs |
| 72-75 | rmw KEEP_ALL max-rate delivered msg/s, Array1k/4k, 0 % / 5 % loss | **ADAPT**: 72, 74 on M1, S1; 73, 75 on L5, S5; link-limited; FastDDS "refused" (write timeout) stays a reported outcome |
| L4 I1, I1L, I1 BE, I4, I4 BE | rmw large messages, apex perf_test, Array1m 30 Hz, Array4m 15 Hz | **ADAPT**: the wired offered rates (30 Hz x 1 MB = 240 Mbit/s) exceed a 30 Mbit/s link many times, so the cell would only measure saturation. Rule: offered load = 50 % of the carried rate: M: Array1m 3 Hz, Array4m 0.75 Hz; S: Array1m 0.25 Hz; **Array4m is not run on S** (one 4 MB sample = 8.5 s at 3.77 Mbit/s; a 60 s window holds 3-4 samples: not decidable; reported as N/M for that reason). I1L 5 % loss on L5/S5. |
| L4 I4s | Array4m 30 Hz saturating | **ADAPT**: on M run as a saturation cell at 1.5x carried (Array4m 1.1 Hz); N/M on S (above) |
| SB2 | large samples step B, same host | **N/M** (same host) |
| HY | hybrid AUTO vs forced MTU, Array1m RELIABLE, 0.01 / 0.1 / 1 % loss | **RUN**, TickLE only (not scored against DDS), profiles M1 variants with those loss values |
| D2 | DATA_FRAG at 4 fragments, 5 % loss (TickLE only) | **RUN** on L5, TickLE only |
| D2 | discovery join M3 (PC netns, N = 8) | **N/M**: a PC netns property of the discovery code, not of the link; unchanged |
| NS | no matched reader sends nothing | **RUN** on M1: wire bytes and writer CPU with no reader (the property does not depend on the link; cheap check that adaptive pacing does not send probes without a reader) |
| c18-c21 | core services cross-host (TickLE only): call round trip at 1 kHz, 10 Hz, max rate, 1 kHz under 5 % loss | **RUN**, TickLE only (M1, S2, L5); not scored |
| 150-cell core campaign (`c1`-`c21`, 3 reps) | the cross-host core comparison | the rows above ARE its cells; the campaign driver's cell list is the template for the runner (one RESULT line per cell, QoS echoed) |
| wired-only checks | `rig_preflight`, interop, gate v2, TickLE-vs-TickLE A/B | **N/M** on the link: they test code, not the network; run unchanged on the rig as release checks (acceptance A2, A3) |
| zenoh-pico, rmw_zenoh | reference | **RUN as reference**, 1 rep per cell, never scored; its TCP session failures are reported as outcomes (earlier tc result: 0/15 delivered at 128 KiB and 1 MiB) |

### Cells added by this campaign (no wired counterpart)
rate step 30 -> 4 -> 30 Mbit/s; 3 readers on one link; large-sample cells rel_large 128 KiB / 1 MiB at 0.25 Hz (the Stage A/B/C cells, re-run with ADAPTIVE).

## (c) Arms and scoring
| arm | meaning | where |
|---|---|---|
| TickLE **OFF** | WIFI profile, `rate_control` off: the shipped default | every cell |
| TickLE **ADAPTIVE** | `rate_control = ADAPTIVE` (headline arm) | every cell |
| TickLE **FIXED p95** | `pacing_rate` = 95 % of the carried rate (the Stage C oracle) | only where pacing acts: large samples, max-rate goodput, KEEP_ALL max rate, L4. Small-sample cells (a sample below one datagram, RTT, QoS mechanics) bypass the pacer by design (DESIGN A7), so FIXED would be the OFF path: not run there, stated per row. |
| Fast DDS 2.14.6 | UDPv4, `maxMessageSize` 1472 (as Stage A) | every cell |
| CycloneDDS 0.10.5 | as Stage A | every cell |
| zenoh-pico 1.10.2 | reference, 1 rep per cell, never scored | every non-rmw cell |
Rules: those of `docs/TESTING.md` section 4 and Stage A/B/C: same QoS echoed, interleaved rotated order, fresh namespaces, creation-time latency, goodput, WIN only when every
TickLE run beats every scored vendor run, A/A (copies A and A2) noise rule (VOID at 10 %), DELIVERY FAILED for a short vendor in RELIABLE, TickLE short = LOSE, VOID never zero, no pinning.
Verdicts reported separately for OFF, ADAPTIVE and FIXED; **the product claim is OFF and ADAPTIVE; FIXED is the oracle**. Every table row states the exact netem string and the arm's
`rate_control` and rate.

## (d) Link profiles
| profile | netem | use |
|---|---|---|
| M1 | `limit 100 delay 5ms rate 30mbit loss 1%` | all families |
| M2 | `limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%` | all families |
| S1 | `limit 40 delay 5ms rate 4mbit loss 1%` | all families |
| S2 | `limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%` | all families |
| L5 / S5 | M1 / S1 with `loss 5%` | the wired 5 % loss rows (13, 14, 16, 17, 26, 27, 34, 39, 40, 73, 75, I1L, D2) |
| M1-reorder | M1 plus the wired row 15's reorder string | row 15 only |
| STEP | M1 -> S1 -> M1 rate schedule (30 -> 4 -> 30 Mbit/s, steps at 20 s and 40 s of a 60-70 s window, tc `change` on both taps) | adaptive acceptance A4, large-sample cells |
| M1-R3, S1-R3 | M1 / S1 with **three readers on one link**: on rpi-0 the tap and a bridge in one namespace with the netem on the tap egress, three reader namespaces on veth ports of the bridge (the radio medium is shared; the netem is applied once per direction, not per reader); publisher on rpi-1 | be_small, rel_small, rel_large 128 KiB / 1 MiB, all arms |
Carried rates (calibration, plain UDP): M 29.73 Mbit/s, S 3.97 Mbit/s; calibration is repeated once per Pi session and its numbers written in each result.
Reduced option: **two profiles per family (M2 and S1)** instead of four, flagged where used.

## (e) Reps, stages and Pi-time
Reps: 6 per arm and cell (3 copies A + 3 A2) in breadth families; **10** (5 + 5) in the deciding large-sample and step cells, as Stage C; 3 for QoS mechanics; zenoh-pico 1. Run times
used: 45 s (small window + start-up), 60 s (QoS, rmw start-up, readers), 90 s (large), 110 s (rate step). "Pi-time" = wall hours the rig is busy; both Pis run at once. A retry/overhead factor of
1.15 is added to the total. Estimates from run counts x run time, checked at 1.5x as the rig rules say, not from a pilot.
| stage | content | runs | Pi-hours |
|---|---|---:|---:|
| S0 | prerequisites (below): unit tests, wire identity, wired gates; rate-step and oracle checks A4-A8 are in S1 | - | 0 (PC, Pis for the gates that already exist) |
| **S1** | **the acceptance cells of ADAPTIVE**: Stage A/B/C cells (rel_large 128 KiB, 1 MiB; be_small; rel_small) x M1 M2 S1 S2 with ADAPTIVE (OFF, FIXED p95 and both DDS reused from Stage A/B/C: same link, binaries, command lines), 10 reps; plus the STEP schedule x 6 arms x 10 reps | 262 | 6.1 |
| S2 | 3 readers on one link (M1-R3, S1-R3) x 4 cells x 5 arms x 6 reps (+ zenoh-pico) | 248 | 4.1 |
| S3 | payloads P1-P4 and QoS combinations: RTT (rows 1c-6), goodput RELIABLE/BE max rate/KEEP_LAST 64 (7-12), 5 % loss and reorder (13-17), CPU/RSS/wire derived, S15/S16 mixed. **Full 4 profiles: 1 733 runs, 21.7 h; reduced (M2, S1): 975 runs, 12.2 h** | 1 733 / 975 | 21.7 / 12.2 |
| S4 | QoS mechanics 42-47 (M1, S2, 4 arms x 3 reps) | 144 | 2.4 |
| S5 | rmw layer: block-wait RTT (48c-51c, 4 cells x M1, S2 x 4 arms x 6) and KEEP_ALL (72-75, 8 cell-profiles) | 400 | 6.7 |
| S6 | large messages L4 adapted: 13 cell-profiles x 6 arms x 5 reps | 390 | 9.8 |
| S7 | TickLE only: hybrid HY, DATA_FRAG D2, NS, services c18-c21 | 112 | 1.6 |
| | **sum, S3 full** | 3 289 | 52.4 -> **about 60 with the 1.15 factor** |
| | **sum, S3 reduced** | 2 531 | 42.9 -> **about 49** |
Cut points for the user: S3 reduced (-9.5 h), drop S6 (-9.8 h; large messages through the rmw layer are covered in the core cells by the large-sample rows), drop S5 (-6.7 h; rmw-layer ping-pong
is dominated by the link), drop S2 (-4.1 h). Minimum useful set: S1 + S3 reduced + S4 = about 20.7 h x 1.15 = **about 24 h**. S1 alone, about 7 h, is what A4-A8 need.
Order: S1, S3, S4, S2, S6, S5, S7; the 1.5x check after the first 10 % of each stage; every stage ends with a committed table and report before the next starts.

## (f) What ADAPTIVE must show before the matrix is run
Acceptance criteria of the adaptive design (`adaptive_pacing/DESIGN.md`, A1-A10), all met before S1 starts; S2-S7 start only after S1's own criteria (A4-A8) pass:
| id | criterion | evidence needed |
|---|---|---|
| A1 | `rate_control` off (default, every profile): wire identical to the parent, ETHERNET/T1S/TSN/WIFI unchanged | `tests/wire_identity_adaptive_pacing.sh` (control, identity, mutant) |
| A2 | rig core gate v2: 0 WORSE with adaptive off | rig gate v2 (158 comparisons) |
| A3 | wired DDS comparison: no LOSE the parent did not have | the cross-host campaign with adaptive off |
| A4 | rate step 30 -> 4 -> 30 Mbit/s: BtlBw within 25 % of the carried rate within `[N]` s after each step; queue delay bounded; no run lost | STEP profile, S1 |
| A5 | within `[X]` % of the Stage C oracle (best fixed rate per class) on 1 MiB / 128 KiB p50 and p99 | the oracle values below, 10 reps, A/A rule |
| A6 | no LOSE against Fast DDS or CycloneDDS in any cell (M1 M2 S1 S2; be_small, rel_small, 128 KiB, 1 MiB) | Stage A/B/C scoring |
| A7 | small samples and be_small: no regression (they bypass the pacer) | ADAPTIVE vs OFF, same cells |
| A8 | datagram count at 1 MiB / S1 within `[X]` x the oracle's (no retransmission storm) | counters |
| A9 | unit tests: estimator converges on synthetic rate steps, queue bounded, floor/cap honoured; every `validate_link_pacing()` bound refused and accepted | `tests/test_adaptive_pacing.c` with mutants |
| A10 | object-size delta reported; objects byte-identical with the code compiled out | build comparison (user decision on the compile switch) |
Numbers the design leaves as placeholders, from the Stage C oracle (`WIFI_TC_PACING_SWEEP_2026-11-19.md`, section 3): best fixed arm (95 % of carried) p50 / p99: 128 KiB M1 44 / 54 ms, M2 69 / 107, S1 293 / 302,
S2 321 / 503; 1 MiB M1 324 / 334, M2 362 / 475 (burst arm; 95 %: 367 / 568), S1 2374 / 2398, S2 2513 / 2673. Datagrams at 1 MiB (15 samples, 10 845 needed): oracle 10 838-11 682. Because the best fixed rate is within
5-10 % of the serialisation floor, a suggested `[X]` is **10 %** on p50 and **25 %** on p99 (p99 of 15 samples is the maximum, noisy; M2/S2 p99 was A/A VOID), and `[X]` = **1.25 x** the oracle datagram count for A8
(OFF sent 4x-23x); a suggested `[N]` is **2 s** after a step for the 128 KiB cell and one sample interval (4 s) for 1 MiB. These are proposals for the user to confirm before any A4-A8 run; the decision belongs to the adaptive design.
Also required first: the implementation exists on a branch with A1, A9 and A10 evidence; the harness gets a `rate_control` arm (`tickle-adaptive`) and the node programs take the option through the environment as they take `TT_PACING_BPS`
today (a bench-side change, not a library change); the calibration is repeated once per session.

## Honest limits
- Every number is from a netem link: fixed rate between steps, drop-tail queue, one flow (three readers in S2), no MAC retries, no rate adaptation. Findings are relative.
- N/M rows are not run, are listed in the result tables as "not run: reason", and are not counted as WIN.
- Small-cell verdicts in Stage A/C rest on 4 DDS runs per profile; this matrix gives 6 per arm in breadth families and 10 in the deciding cells; a cell with fewer than 8 runs per side is labelled "few reps".
- The Pi-time is an estimate from run counts; it will be checked at 1.5x after the first 10 % of every stage.
