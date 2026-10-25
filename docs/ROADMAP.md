# TickLE roadmap

**TL;DR**
- **Done 2026-10-19:** QoS reshaped like rmw (DESIGN 9, RMW.md); KEEP_LAST rows and L4 latency re-scored by round trip.
- **Now:** what is left of items 0 and 5a of the user's list of 2026-10-05.
- **Parked until the user says go:** Security (SROS2 + MACsec), then wireless. Wire changes wait for the wireless stage.

Older text: `git show 0a7926a0:docs/ROADMAP.md`. Pre-2026-10-05 documents: `git show 3c0c505b:<path>` (DESIGN.md section 16).

## Now

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

The user's order (2026-09-29): wired, then Security, then wireless.

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
  already on its way (~0.2 ms) locks Karn's rule for the rep. Two cheap fixes (branches `k5-floor`, `k5-discard`) stop
  it but give back K's ge5b2 tail; the clean fix is a wire change (a request serial echoed in the repair), for the
  wireless stage. K stays held.

### Other open items

- rmw receive return on the Pi +2.0 / +1.1 us behind CycloneDDS / FastDDS (RESULTS `KH2B`, 2026-10-24, after the ring's
  first move at 2 waits; ~1.3-2.4 us in `KH2`, ~4-5 in `KH`); the rest is the ring's DEFER_TASKRUN path itself.
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

## Later (not to be started yet; the user, 2026-10-11)

- **Time-varying networks**: test varying loss and delay (netem schedules), then tune the estimator (EWMA, windowed
  loss and RTT, jitter). Until then the hybrid loss windows halve at `tt_HYBRID_WINDOW_FRAMES`.

## Parked (only on the user's explicit go)

- **Security**: SROS2-file-compatible security on MACsec (parked 2026-09-27). Only g7 is done: refuse to start under
  `ROS_SECURITY_ENFORCEMENT=Enforce`. Five open questions at `git show 0a7926a0:docs/ROADMAP.md`.
- **Wireless** (correlated loss, Wi-Fi, internet, congestion control): after Security.
- **Wire changes** (run header merge, FEC, range NACK): at the wireless stage (the user, 2026-10-15).
- **More than two known readers: link broadcast or per-reader unicast** (`tt_UNICAST_PEER_THRESHOLD` 2): measured and
  decided on the 32-Raspberry-Pi testbed once it is built (the user, 2026-10-16).
- **rmw_tickle at REP-2004 Quality Level 2**, after Security: pull requests with DCO and required CI (the user's
  decision), rmw_tickle coverage, `SECURITY.md`, dependency declarations (the >= 1.0.0 version is met: 2.0.0), Tier 1 platforms.
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
