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
  - Kept off main after rig A/Bs (`ab/hotpath-split`): segment attach backoff by time, revalidate by time, timed
    receive check. Each needs a redesign; the 64-bit clock does not link on RV32 FreeRTOS.
  - A client restarted under the same context id can get its predecessor's cached answer. Fixing it needs a per-launch
    tag in CallRequest, a wire change.

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
Array1m KEEP_LAST and KEEP_ALL equal). Next: 10 large tables into caller storage. Rules for every bundle: the default
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

### Other open items

- rmw receive return on the Pi ~1.3-2.4 us behind the vendors' (RESULTS `KH2`, after the wait ring; ~4-5 us in `KH`).
  The poll-to-executor hand-off (~21 us) is gone: executor-driven receive, 3.3 us receive -> callback on the PC
  (`rmw_wake_split.py`, `9d87885c`), 8.3-10.5 us on the Pi against 25.5-40.9 for the vendors (`KH`). A C++ subscriber
  segfault seen once in ~370 runs (2026-09-25), not reproduced since: 0 in 1,200 runs of the same `-r` shape on
  2026-10-22, in a netns, half under ASan + UBSan and half pinned to one CPU (`flaky_repeat_netns.sh`,
  `rclcpp_r_shape_once.sh`).
- Measurement debt: none left (2026-10-20: c6, the 12-rep cross-host cells, 4-fragment DATA_FRAG, M3, row 47 and
  rows 43-44 with the reshaped scenario 6 harness; RESULTS `D`, `D2`, `LS`, `X2`).
- Throughput: the poll loop's clock reused in scheduled publishes, done 2026-10-23 (rig: client -1.4% to -4.1%
  user instructions a sample, CHANGELOG); the unmerged `experiment/poll-loop-io-interleave-v2`;
  in-place recvmmsg rows; decide what `rmw-perf.yml` becomes.

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
