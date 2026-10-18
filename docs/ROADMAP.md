# TickLE roadmap

**TL;DR**
- **Next:** QoS reshaped like rmw. The plan goes to the user for agreement before any code.
- **Now:** what is left of items 0 and 5a of the user's list of 2026-10-05.
- **Parked until the user says go:** Security (SROS2 + MACsec), then wireless. Wire changes wait for the wireless stage.

Older text: `git show 0a7926a0:docs/ROADMAP.md`. Pre-2026-10-05 documents: `git show 3c0c505b:<path>` (DESIGN.md section 16).

## Now

- **0. A fair testbed.** Open (b): `s6_transport_cells.sh` runs each framework's repetitions back to back, not interleaved.
- **5a. Testbed-independent constants.** The fitted values that stay are in RMW.md "Options you may tune". Open:
  - Kept off main after rig A/Bs (`ab/hotpath-split`): segment attach backoff by time, revalidate by time, timed
    receive check. Each needs a redesign; the 64-bit clock does not link on RV32 FreeRTOS.
  - A client restarted under the same context id can get its predecessor's cached answer. Fixing it needs a per-launch
    tag in CallRequest, a wire change.

## Next

### QoS reshaped like rmw

Agreed with the user 2026-10-08. **Agree the concrete plan (types, profile, API, wire, rmw migration) with the user
first.** Decided:

- rmw's model: QoS per endpoint as `rmw_qos_profile_t`, context settings in `tt_InitOptions` as `rmw_init_options_t`.
  No topic or group QoS. rmw's defaults (`tt_QOS_PROFILE_DEFAULT` = RELIABLE, KEEP_LAST 10, VOLATILE).
- `tt_Topic` becomes `tt_TypeSupport` (type name and codec only); the topic name is a create argument. Retry settings
  move from `tt_Service` to client and server. QoS is given at creation.
- One flat `tt_QosProfile`: rmw fields first, in rmw's order, then TickLE extensions as plain fields; 0 means
  "computed default". Non-QoS settings go in options structs. Memory stays the caller's (no malloc in core).
- Includes the writer's LIFESPAN applied at the reader (the user, 2026-10-16).
- Wire and API changes allowed; the wire rule (no test worse) still applies. Comes before Security.

### Wired work

The user's order (2026-09-29): wired, then Security, then wireless.

- **Zero-copy slot loans**: a segment ring that skips held slots; off by default (the user, 2026-10-10).
- **Large samples step B on the rig**: the last session went VOID on perf_test's own spin (`bcf8624d`); needs a better control.
- **Stage 1 / S1 CPU-time half on the rig** (PC half done, `d131d82e`).
- **FreeRTOS segment** (HAL-provided memory; discovery stays UDP).
- **rmw shared-memory kill test** (two ROS processes, subscriber killed mid-stream).

### Other open items

- rmw kernel wake (~6 us) and poll-to-executor handoff (~21 us); a C++ subscriber segfault seen once in ~370 runs.
- A reorder buffer smaller than the window causes a re-request storm: add a size check.
- Measurement debt: c6 with the `retransmitted` figure; 11 ◊ cells in RESULTS (re-measure unpinned); DATA_FRAG
  under-loss throughput in the full campaign; discovery join time (M3); rmw_zenoh QoS rows 42-44, 47; harness
  residuals (scenarios 6, 9, LIFESPAN); a data race in the Fast DDS liveliness server.
- Throughput: reuse the poll loop's clock in scheduled publishes; the unmerged `experiment/poll-loop-io-interleave-v2`;
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
  decision), rmw_tickle coverage, `SECURITY.md`, dependency declarations, 1.0.0, Tier 1 platforms.
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
