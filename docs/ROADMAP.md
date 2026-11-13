# TickLE roadmap

**TL;DR**
- **Done 2026-10-19:** QoS reshaped like rmw (DESIGN 9, RMW.md); KEEP_LAST rows and L4 latency re-scored by round trip.
- **Done 2026-11-06:** discovery-seeded retry interval landed (`e42185f1`, RESULTS `DSE`).
- **Now:** nothing is open. The wireless stage and the time-varying stage (both simulation only: the user, 2026-11-06 and
  2026-11-08) are **closed for 2.2.0** (2026-11-13); results in RESULTS section 5 and CHANGELOG 2.2.0. **No real wireless
  hardware is used at any stage** (the user, 2026-11-08). **Parked, only on the user's go:** the 32-Pi testbed items
  (slow-reader flow control `C7W`; link broadcast against per-reader unicast above 2 readers), Security (v3.0), rmw_tickle
  at REP-2004 Quality Level 2 (after Security), internet paths and congestion control (not started), rig interrupt
  coalescing and 10BASE-T1S hardware.
- **Release order (the user, 2026-11-01):** performance work done -> v2.1; wireless + wire changes; time-varying
  networks -> v2.2; Security -> v3.0. Each stage starts on the user's go.

Older text: `git show 0a7926a0:docs/ROADMAP.md`. Pre-2026-10-05 documents: `git show 3c0c505b:<path>` (DESIGN.md section 16).

## Now

Nothing is open (2026-11-13). The wireless stage and the time-varying stage are closed for 2.2.0; their sections below are
the record, not a task list. What remains is under "Parked": the 32-Pi testbed items (slow-reader flow control `C7W`,
branch `c7-wire-room`; link broadcast against per-reader unicast above 2 readers, `tt_UNICAST_PEER_THRESHOLD`), Security
for v3.0, rmw_tickle at REP-2004 Quality Level 2 (after Security), and the items that wait on hardware. Next release
order: 3.0 Security, on the user's go.

### Wireless stage (started 2026-11-06; closed for 2.2.0)

- **Environment: T1 only** (the user, 2026-11-06): `mac80211_hwsim` + `wmediumd` on the dev PC.
  Simulation only, no real Wi-Fi, now and later (the user, 2026-11-08: real wireless hardware is not used). T1 is read
  as a relative A/B instrument; no T1 figure is quoted as Wi-Fi performance. T0 (netem) is the earlier baseline.
- **Tooling:** `wmediumd` 0.3.1 + a local Gilbert-Elliott patch (per-link burst loss, `model.ge`);
  `t1_topo.sh up N` builds N lanes of 3 nodes (AP + 2 stations). Scripts, the wmediumd patch and a README (what T1 is and
  is not, rules, known limits) are in `examples/perf_hil/experiments/t1/`.
- **Done:** association, per-link loss, burst loss, one `be_small` smoke run, 2 lanes up. 2026-11-07/08: per-attempt MAC
  retries in `wmediumd` (`model.retry`; the stock `prob` model drew one random value per frame, so retries never helped),
  a Gilbert-Elliott model with a slow quiet/busy envelope fitted to the PSU sniffer traces, a seeded schedule
  (`model.seed`; reproduced on long bursts, not shown on 2 ms ones), per-receiver loss shown independent, CPU pinning.
  Findings: with retries a unicast frame survives bursts shorter than a few ms (residual about p^7, as in the real traces),
  so only long bursts at high loss (`o60`: 60 ms at 0.9, `o200`) lose frames; broadcast gets one attempt and no retry;
  the AP's broadcast path carries about 0.6 Mbit/s, so `rel_large` runs only at 128 KiB and 0.25 Hz. Noise: best-effort
  cell 0.4-1.6%; reliable cell 2-3% with seed, pinning and retries (p90 and retransmissions only for >3x differences).
  `ab/evict-seed`: no gain (-0.5% delivered) twice; latency comparisons VOID.
- **Data sources:** the Gilbert-Elliott profiles (`ug`, `library`, `psu_cs`, `pioneer`) were fitted to the PSU (Portland
  State University) wlan_pcap traces of the CRAWDAD dataset pdx/vwave (v. 2009-07-04; VeriWave WT20 sniffer, Portland, 2006)
  by Caleb Phillips and Suresh Singh. The dataset's README states "This work was partly funded by NSF 0435328." Cite as:
  Caleb Phillips, Suresh Singh, pdx/vwave, https://doi.org/10.15783/C7C303 (CRAWDAD; licence CC BY 4.0, per
  https://ieee-dataport.org/open-access/crawdad-pdxvwave-v-2009-07-04).
- **Done:** T1 harness (`be_small`, `rel_small`, `rel_large`), rules and A/A control registered before runs, parallel-lane
  calibration (L1 failed at 2-3 lanes, so one lane is used), the T0 comparison (C1 note, 2026-11-07), burst length, per-receiver
  loss and time-varying runs. **No wireless-stage task is open.**
- **Candidates:** all closed. Run header merge and range NACK stopped at stage 0 (2026-11-11), FEC dropped (the user,
  2026-11-11), request serial echo dropped (2026-11-02), `ab/evict-seed` no gain. Slow-reader flow control (`c7-wire-room`,
  `C7W`) is parked for the 32-Pi testbed. Small-sample batching: stage 0 did not meet its bar (CHANGELOG 2.2.0); the opt-in stays.

### Time-varying stage (started 2026-11-08, beside the wireless stage; simulation only; closed for 2.2.0)

- Plan, done on netem schedules and the rig (a time-varying `wmediumd` schedule in T1 is not among the recorded results,
  RESULTS N9): test varying loss and delay, then tune the estimator (EWMA, windowed loss and RTT, jitter). Until then the hybrid loss windows halve at `tt_HYBRID_WINDOW_FRAMES`.
- Tooling already on main: `time_varying_netns.sh` and `time_varying_read.py` (V1/V2/V4 fail only beyond the A/A spread;
  D/LD need 8 reps), `trace_replay_mm.sh` (Mahimahi cellular trace replay), `-Dtt_ESTIMATOR_TRACE` (X0d).

### Time-varying stage results, 2026-11-10/11 (rig, simulation otherwise)

- Karn deadlock fix landed on main (`933da69a`, 4 commits). Rig X2 `rel_small`: V1 and NOFALSE FAIL -> PASS; `rel_large`:
  V1 0.48% -> 0.64% (limit 1.00%, A/A spread 0.45-0.90%), V3 100%, no regression. The rig runs used the `ethernet` profile.
- Down-step srtt converges slowly by design (gain 1/8; about 12 s after a 20 ms -> 1 ms step it reads 4-14 ms). Not a bug;
  a faster down gain is a stability trade-off and was left as is (the user, 2026-11-11).
- range NACK stopped at stage 0 (T1 broadcast cells, `RANGE_NACK_DESIGN_2026-11-11.md`): ACKNACK bytes are 0.31-1.05% of
  forward bytes in all 12 loss runs, requests are mostly one bitmap word. Not pursued.
- Run header merge stopped at stage 0 (`RUN_HEADER_MERGE_STAGE0_2026-11-11.md`; encoder in a scratch program, no wire capture):
  the hybrid transport already carries up to 45 units under one FRAG_RUN_L header, so a merge could still remove at most 0.03% of
  forward bytes (0.084% with Ethernet framing) against the 2% bar; all header bytes together are 1.4-3.0% of the wire. The older
  "about 18 B per unit, 1.2% of the wire" is the MTU-mode cost that hybrid already removed. Small-sample batching is a different
  candidate and is not covered.
- Shared reorder ring over-send with 4 writers at 20% loss (+10.8% DATA): two variants (ring x4, even placement) did not
  meet the registered bar; neither merged. Setting `reorder_slots` to 1,024-2,048 for multi-writer topics cuts it with no code change.
  Stage 0 loss sweep (1-20%, 1-8 writers, 3 seeds, in-process): no over-send at 5% or less (0 overflow in 36 runs), a trace at 10%
  (overflow in 4 of 12 runs at 4-8 writers, DATA within 1%), real only at 20%. Closed (the user, 2026-11-11); advice only.

### Discovery seed: landed

- 2026-11-06 (`e42185f1`, `5dfb8030`; RESULTS `DSE`): the first announce echo seeds the retry timer;
  timer ACKNACKs per gap 25-58 -> 0.5-1.0 at the 8 treatment points.
- Registered A/B verdict stays FAIL (1 of 70 cells WORSE: 50 ms/2% delivery -0.90 pt);
  a 15-rep rerun of that cell is equal.
- `ab/evict-seed` parked to the wireless stage. S3 hold-off: every verdict VOID;
  reopen only on the user's go.

### Earlier items

- **0. A fair testbed.** Done 2026-10-19: `s6_transport_cells.sh` rotates the framework order per rep (`b83e2513`),
  and the same-host rows were re-measured that way (`60f49f6a`, RESULTS `SB`). S9c re-run the same night (`SB9`).
- **5a. Testbed-independent constants.** The fitted values that stay are in RMW.md "Options you may tune". Open:
  - Landed 2026-10-23 from `ab/segment-time-limits` (the first attempt, `ab/hotpath-split`, was kept off main after
    rig A/Bs): segment attach backoff by time, revalidate by time, timed receive check, redesigned with no clock read
    on a send and no 64-bit atomic (DESIGN 10 "Counts made times"). Rig: core gate v2 PASS (pieces 1, 2 and 3), same-host
    gate v2 formally WORSE (piece 1 user instructions +0.03..+0.06% on latency cells, piece 2 a latency server's kernel
    instructions +1.95%), landed on the user's overall judgement.
  - Done 2026-10-23 without a wire change: a client restarted under the same context id no longer gets its
    predecessor's cached answer. The server tells launches apart by the request's source port, and each launch starts
    its seq_no at a drawn value (DESIGN.md section 7, "launch identity"; its limits are listed there).

## Next

### QoS reshaped like rmw: done

Landed 2026-10-18/19 (`e4fc0683` to `47eb05e8`): per-policy `struct tt_QosProfile`, rmw defaults, creation
API, wire `tt_VERSION` 14. Rows 43-44 re-scored by freshness (`014343f9`), L4 latency by round trip (`01addd79`).
Cross-host latency is a round trip on one clock (the user, 2026-10-18).

### Context options

Static configuration moves into `struct tt_InitOptions` (the user, 2026-10-21; DESIGN.md section 2), before the
per-network profiles. On main: 1 the API and `net` (`_tt_CONFIG`, 48754486); 2 discovery and liveliness timings
(580a5ec8); 3 reliability timings (e7dc662f); 4 service timings (ed1d3a2a); 5 QoS defaults (4eeeb1ab); 6 poll, socket
and HAL settings (35ea2d58); 7 hybrid estimators (05118219); 8 segment settings (143a58c7); README "Configure a
context". Rig A/B verdicts: 3 and 6 PASS (core gate v2, 0 CPU/latency WORSE; 6 lists user instructions +0.04 to +0.08%
on 7 of 88 rows); 7 PASS (`rmw_keepall_rig.sh`, Array1m/4m); 8 IMPROVED after its drain fix (same-host gate v2; the
first build was WORSE, DESIGN section 2). 9 runtime capacities up to the compiled maxima (1c3f4227; core A/B PASS,
Array1m KEEP_LAST and KEEP_ALL equal). 10 done without a caller-storage API (the user, 2026-10-22, after resident
memory showed the large tables barely resident): a context no longer writes its large tables at creation (2590bdd9;
core A/B PASS, DESIGN section 11) and the FreeRTOS service caches are sized to their use (cf022a84). Rules for every bundle: the default
context byte-identical on the wire, each check refused with a test and a mutant, each field shown to change behaviour,
no hot-path cost (rig A/B for 3, 6, 7, 9, 10). `tt_*_storage_size()` keeps its signature; interop timings stay identical
by default and warn on a mismatch; rmw_tickle keeps its environment variables.

### Wired work

The user's order: wired first (2026-09-29); after it wireless, then Security (2026-11-01, replacing "Security, then
wireless").

- **Zero-copy slot loans**: a segment ring that skips held slots; off by default (the user, 2026-10-10).
- **Large samples step B on the rig**: done 2026-10-20, every B2 rule PASS with a control that held (RESULTS `SB2`).
- **Stage 1 / S1 CPU-time half on the rig**: done 2026-10-22 (RESULTS `S1R`): no CPU-time cost shown, one server cell
  (p4, +0.98%) a CANDIDATE; the instruction residual (publisher user +11-13%) stays open.
- **FreeRTOS segment**: done 2026-10-21 (`23064e69`), off by default, README says when and how to turn it on.
- **rmw shared-memory kill test**: done 2026-10-22 (`test_shm_kill`, in the rmw suite); it found two leaks, both fixed
  (DESIGN.md section 10, "Lifecycle").
- **Large-sample repair storm on a queued path** (found 2026-10-23, RESULTS `LRF`, item (a)): on a 1 Gbit/s netem path
  at 1% loss one rep in three resent 251,028 units, 83% duplicates, 75 of 900 1 MiB samples lost; the sim reproduces it
  (`test_large_sample`, queued-path case). Each ACKNACK names units whose repairs still queue behind the drain and the
  writer resends every named unit each time. A fixed per-unit hold-down cuts it (explored: 20 ms, 475,436 -> 9,144) but
  is a testbed constant; the candidate is time-ordered (RACK-like): resend a unit again only once the reader has
  received something sent after its last resend - per-unit send order kept by the writer, or request order by the
  reader. The same bursts overflow a slow reader's socket (564,402 resent for 10 samples in the sim). Needs its own
  design, A/B and rig cells (Array1m 0.1/1%, whose 0.1% head arm lost samples in 2 of 10 reps on 2026-10-23).
  **Landed 2026-10-24** (DESIGN.md section 4, "Large-sample resend order"; RESULTS `LRO`, `LRT`): storms stopped in the
  sim and netns, WIFI NOT WORSE; RACK's time rule took back two thirds of the netns 1% p99 cost; rig core gate v2 PASS
  0/88, Array1m 1% p99 +0.4-0.8 ms against main within each hybrid mode, CPU equal. Left: the slow reader's repair
  bursts (36,999 resent for 10 samples) - flow control; and the hybrid transport's mode at 1% loss (IP-fragment runs in
  4-9 of 12 reps on every build, carrying the CPU), not yet explained.
  **2026-10-25** (RESULTS `TVS`, landed 2026-10-26, `8d0d2eed`): the order's repair timeout still stormed on the
  Wi-Fi `tv` arm (about 1 rep in 30-60, up to 845,268 resent): its srtt took microsecond samples from units an
  ACKNACK left out. Fixed: PC A/B, rig core gate PASS 0/88, Array1m/4m p99 equal; not the slow reader's bursts, which stay open. Also left: the
  reader's own retry estimate collapsing (timer ACKNACKs up to 25,558 a rep against ~80, `R4K`'s K2 note), which the
  writer's order contains.
  **2026-10-26** (RESULTS `R4K`, `K5`): K's ~2x-resend reps are such a collapse. A seed from a probe whose repair was
  already on its way (~0.2 ms) locks Karn's rule for the rep. Two cheap fixes (branches `experiment/k5-floor`, `experiment/k5-discard`) stop
  it but give back K's ge5b2 tail; the clean fix is a wire change (a request serial echoed in the repair), for the
  wireless stage. K stays held for that wire change (the user, 2026-10-26). **2026-11-02** (RESULTS `KR`): K gains
  nothing on main any more and its 2x-resend reps no longer occur; K and the wire change are dropped.
  **2026-10-27** (RESULTS `EWS`, `EWF`): ETHERNET amp's writer storm (a whole sample's resend batch outlasting a
  2 x srtt timeout). Landed: the timeout waits while the reader progresses (forced loop 0 of 20 against 10 of 20; rig
  core gate PASS 0/88, Array delivery 100%, 1% p99 +0.5 ms within the rule). The stall with no progress: landed the same
  day (RESULTS `EWB`, a per-unit back-off while the reader shows nothing; forced stall 2 of 20 against 14 of 20; rig PASS).
  **2026-10-27** (RESULTS `RLC`, `RLF`): the least-transit floor stops the reader's collapse on every link (0 of 840
  PC reps against 22 of 840). It costs Wi-Fi latency, which the user accepted; it landed after the rig check passed.
  **2026-10-31** (RESULTS `C7W`): stopped, a documented limit. The slow reader's repair bursts are flow control: the
  reader advertising its free socket room in the ACKNACK cuts lossless repair waste from 46-94% to 0.2% in the sim, but
  four contracts in five rounds each left a cell WORSE (the closest, a writer GRANT naming its cut, one sample 11 ms
  late in 77 cells, and 2 of 6 netns slow reps in duplicate storms). The wire rule is not met, so nothing landed; the
  branch `c7-wire-room` is kept for the wireless stage, with the other parked wire changes (the user, 2026-10-31).

### Other open items

- rmw receive return on the Pi +2.4 / +1.7 us idle and +0.7 / +1.2 us warm behind CycloneDDS / FastDDS (RESULTS
  `KH2W`, 2026-11-01; `KH2B`'s +2.0 / +1.1 was the idle cell). The warm part is io_uring's deferred completion on one
  thread waiting on many sources; fixed files and POLL_FIRST did not move it, so it is a documented limit.
  After ~100 ms idle the receive costs ~1.4 us more than CycloneDDS's (RESULTS `C12I`, 2026-10-31): a diffuse cold kernel
  path. The poll thread avoids it but its hand-off costs more (RTT +13 us), so it stays a documented limit.
  The poll-to-executor hand-off (~21 us) is gone: executor-driven receive, 3.3 us receive -> callback on the PC
  (`rmw_wake_split.py`, `9d87885c`), 8.3-10.5 us on the Pi against 25.5-40.9 for the vendors (`KH`). A C++ subscriber
  segfault seen once in ~370 runs (2026-09-25), not reproduced since: 0 in 1,200 runs of the same `-r` shape on
  2026-10-22, in a netns, half under ASan + UBSan and half pinned to one CPU (`flaky_repeat_netns.sh`,
  `rclcpp_r_shape_once.sh`).
- Measurement debt: none left (2026-10-20: c6, the 12-rep cross-host cells, 4-fragment DATA_FRAG, M3, row 47 and
  rows 43-44 with the reshaped scenario 6 harness; RESULTS `D`, `D2`, `LS`, `X2`).
- Throughput: the poll loop's clock reused in scheduled publishes, done 2026-10-23 (rig: client -1.4% to -4.1%
  user instructions a sample, CHANGELOG). In-place recvmmsg rows measured on 2026-10-25 and not taken: -8.4% server
  user cycles when backlogged, nothing paced (RESULTS `RXIP`; `experiment/rx-rows-in-place`). (The poll-loop I/O
  interleave branch listed here until 2026-10-25 was merged on 2026-09-21, `b7d822ab`, and its entry count made a time
  on 2026-10-23, `9974e40f`.)

## Later

- Nothing waits here at the moment; the time-varying stage moved to Now on 2026-11-08.

## Parked (only on the user's explicit go)

- **Security**: SROS2-file-compatible security on MACsec (parked 2026-09-27), last, for v3.0 (the user, 2026-11-01).
  Only g7 is done: refuse to start under `ROS_SECURITY_ENFORCEMENT=Enforce`. Five open questions at `git show 0a7926a0:docs/ROADMAP.md`.
- **Wireless** (correlated loss, Wi-Fi, internet, congestion control): the simulation stage closed for 2.2.0 (2026-11-13,
  see Now); real wireless hardware is not used. Internet and congestion control not started.
- **Wire changes** (run header merge and range NACK stopped at stage 0, 2026-11-11; FEC dropped by the user, 2026-11-11): at the wireless stage (the user, 2026-10-15). On netem (RESULTS
  `WB2`, `KR`, 2026-11-02) none is lifted: R4 and R2 pass on WIFI without them. Revisit on T1 (simulation only, the user 2026-11-06; no real Wi-Fi hardware, the user 2026-11-08); slow-reader
  flow control (`C7W`) is judged on T1 and the 32-Pi testbed (the user, 2026-11-02, 2026-11-08).
- **More than two known readers: link broadcast or per-reader unicast** (`tt_UNICAST_PEER_THRESHOLD` 2): measured and
  decided on the 32-Raspberry-Pi testbed once it is built (the user, 2026-10-16).
- **rmw_tickle at REP-2004 Quality Level 2**, after Security: pull requests with DCO and required CI (the user's
  decision), rmw_tickle coverage, `SECURITY.md`, dependency declarations (the >= 1.0.0 version is met: 2.2.0), Tier 1 platforms.
- The user's calls: O(1) `find_resendable_cache_entry()`; the 1 MiB KEEP_LAST cache default; full segment ring:
  back-pressure or drop.
- zenoh-pico p2-p4 reliable cells stay "not measurable"; never quote the rate-limited figure.
- Rig interrupt coalescing (macb 49 us) and 10BASE-T1S: wait on rig hardware.

## Done (2026-10-05 to 2026-10-18)

The user's list of 2026-10-05, by number:

- **0a.** Same warm-up and cool-down for every framework (`BenchWindow.h`).
- **0.** (a) rmw KEEP_ALL rows bounded alike (`29dff630`).
- **1.** p4 same-host latency gap: first-touch faults on the reorder ring (`bc398ed2`).
- **2. Fill COMPARISON 1a's empty cells**: CPU and peak RSS for all three frameworks (`8f57a512`, `e521bbba`).
- **3.** Publisher user time: encode into the segment slot (`cd09e895`), one clock read per publish (`4de1f0d7`).
- **4.** Reader wake cost (`7f9f7c62`, `8c1e6431`, `e6226bbf`).
- **5.** rmw_tickle KEEP_ALL on the rig: 12 of 12 runs lose nothing (`a8c1cd83`).
- **6.** rmw same-host performance: 52 of 52 rows WIN (`06dd78d8`).
- **7.** FreeRTOS receive hint on lwIP netconn (`b149dce7`, `e5eb4a61`).
- **8.** CycloneDDS arm of the mixed shared-memory + network test (`e4e090cd`).

Also:

- **Fast DDS under KEEP_ALL, from its side** (`6bfda159`): does not run with a normal configuration; closed by the user.
- Large-message stage 2, steps A and B; GSO/GRO on by default (`ab5c3c6a`).
- Hybrid transport, `tt_VERSION` 13 (`dc24c09c`). UDP checksum 0 by default (`f5fcda06`).
- No send without a matched reader (`4fbfe459`).
- RELIABLE repair as RTPS (`763b8d55`), idle HEARTBEAT (`ec13c9de`), several calls per Client (`1014a7ed`).
- Receive-buffer lending (`0be14584`), loaned messages (`324f6ced`); scheduler waits end at their deadline (`f02f5e7b`).
