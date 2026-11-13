# TickLE 2.2.0 in brief

Terse digest of [README](../README.md), [DESIGN](DESIGN.md), [ROADMAP](ROADMAP.md), [TESTING](TESTING.md),
[CHANGELOG](CHANGELOG.md) and [RESULTS](RESULTS.md). Where this and the code disagree, the code (and DESIGN) is right.

## What it is

- Small C pub/sub + RPC middleware, like DDS but much lighter; `rmw_tickle` plugs it into ROS 2 (QL4).
- Targets controlled links (10BASE-T1S, TSN, one trusted L2 segment); same-host peers use shared memory.
- Core: no heap allocation, no threads of its own (one thread polls a context), counts every drop, no ROS 2 dependency.
- Own UDP/IPv4 wire, RTPS-like, smaller framing; does not talk to DDS nodes.
- License: GPL-3.0-or-later, or proprietary from TSN Lab, Inc.

## Wire and version

- Library 2.2.0 (`TICKLE_VERSION_*`); wire `tt_VERSION` 14 (unchanged since 2.1.0; 2.0.0 is incompatible).
- 2.1.0 and 2.2.0 interoperate both ways (checked 2026-11-12, 76 runs).
- 2.2.0 gave 2 formerly zero bytes of `tt_AnnounceHeader` a meaning (`marks`: announce tag and echo); 2.1.0 ignores them.
- Another `tt_VERSION` is dropped and logged once.

## Reliability and QoS

- QoS given at creation only, rmw-shaped (`struct tt_QosProfile`); zeroed profile = RELIABLE, KEEP_LAST 10, VOLATILE.
- BEST_EFFORT: may miss samples, never reorders or duplicates.
- RELIABLE: in order per writer; reader NACKs gaps per datagram, writer resends; idle HEARTBEAT.
- KEEP_ALL: refuses a write (`tt_RET_WOULD_BLOCK`) rather than drop an unacknowledged sample.
- Retry timer is measured, not fixed: `srtt + max(G, 4 x rttvar)` (RFC 6298, Karn-like recovery estimate).
- Compatibility judged on announce; a reader's lifespan is the shorter of its writer's and its own.
- Any thread may call a context while one thread polls it (`tt_THREAD_SAFE`).

## Key mechanisms

- **Hybrid transport** (large samples): writer packs `n` MTU units into one IP-fragmented run, `n` from loss the reader
  measures (`n_max` 45); a collapse caps `n`; 2.2.0: reordering no longer counted as loss.
  Correction: DESIGN has no FEC; FEC was dropped (user, 2026-11-11). Recovery is ARQ only.
- **Discovery seed (2.2.0)**: announce tag/echo times the round trip; first retry interval = RTT + flush tick (never below
  `retry_initial`). RESULTS `DSE`: timer requests per gap 25-58 -> 0.51-1.00; `ESD`: copies after delivery 24.1-37.2% -> 0.70-0.91%.
- **Karn fix (2.2.0)**: after a delay step up, recoveries reached by back-off raise a stall count and feed the estimate.
  RESULTS `KS`: copies after delivery 6.45% -> 0.63% (V1 FAIL -> PASS). Down-step converges slowly (~12 s, gain 1/8), by design.
- **Batching** (`pub->batch`): off by default (publish flushes at once); 2.2.0 fix: the room-making flush goes where its
  tick sends (RESULTS `BF`: 1000 B at 1 kHz delivered 50.0% before, ~100% after).
- **Link profiles** ETHERNET / T1S / TSN / WIFI fill per-link defaults; T1S paces sends at 75% of link rate; WIFI unicasts discovery.
- **Runtime options**: `struct tt_InitOptions`, zero field = `config.h` default.
- **Large-sample resend order** (RACK-like time rule) stops repair storms on queued paths.
- Shared memory on one host is automatic; FreeRTOS segment is opt-in (`-Dtt_SEGMENT_ENABLED=1`).

## Platforms

- Linux (UDP, io_uring when the container allows it, shared memory); FreeRTOS + lwIP (RISC-V tested under QEMU).
- ROS 2 through `rmw_tickle` (docs/RMW.md).
- Measured on two Raspberry Pi 5 over 1 GbE; wireless only in simulation (`mac80211_hwsim` + `wmediumd`, netem); no real Wi-Fi hardware.

## Build and run

- `make all` (add `BUILD_TYPE=release` for -O2); `make install PREFIX=/usr/local`; C11 headers; options in `include/tickle/config.h`.
- Examples: `make createns`, then `make runsubscriber` / `make runpublisher` (also server/client, ping/pong, perf).
- Set `-b` / `TICKLE_BROADCAST_ADDR` to the link's directed broadcast; the default 255.255.255.255 often hits the wrong network.

## Tests and gates

- `make test` (unit, mock HAL); `make test-linux` (two netns, `sudo ip`); `make test-freertos` (QEMU); `make test-all`.
- Also `make sanitize`, `make tsan`, `make fuzz`, `make headers-cpp`, `make test-rmw`, `make test-samehost`.
- Before a push: clang 19 format, `make check-gates`, read the table by shape, push only on exit 0.
- Gate rows: lint, lint-shell, check-doc-shas, check-bench-shapes, check-unsupported-list, check-context-reset,
  check-results-provenance, test, tsan, sanitize, test-typesupport, lint-freertos-hal, lint-rmw, rmw suite, build configs, build freertos.
- Not covered by gates: `make test-linux`, CI's interface set, C++17 for ROS `.cpp`, `tt_CONTEXT_ID_CLAIM` / `tt_LOCAL_DELIVERY` code.
- Scoring vs FastDDS/CycloneDDS: same QoS, same setup, goodput; WIN only outside every vendor's spread; VOID is never zero.
- TickLE-vs-TickLE rig A/B decides by gate v2 (instructions decide, layout-stable builds, placement floor).

## Results (RESULTS.md)

- Across the network: 139 wins, 11 draws, 0 losses in 150 cells vs DDS (October campaign, `09a0437d` to `d58873cf`,
  RESULTS tables); through ROS 2 wins all 52 same-host rows.
- Rerun against DDS on 2026-11-10 (rig, 3 reps, RESULTS `KS`): main `d2de7c3d` WIN 136, TIE 14, LOSE 0 of 150; `836665bb`
  (the branch that became `933da69a`) WIN 137, TIE 13, LOSE 0; rmw cross-host round trip fastest in all 8 combinations measured.
  The two counts are different campaigns, not a regression: the draws are not from the change (`d2de7c3d` has more).
- Still lost: subscriber CPU per MB vs FastDDS at 4 MB / 15 Hz (+4.5%); one 100 us poll round trip vs CycloneDDS via ROS 2 (+1.2%, row 63c).
- Trade-offs shipped: `ESD` one Wi-Fi-like cell one-way p99 57.6 -> 65.7 ms; `DSE` registered verdict stays FAIL (1 of 70 cells).

## ROADMAP state

- Done: performance stage (v2.1); QoS reshape; `tt_InitOptions`; link profiles; wireless stage and time-varying stage on simulation (v2.2).
- Closed without landing: FEC (dropped), range NACK and run header merge (stopped at stage 0), shared reorder ring
  over-send (advice: `reorder_slots` 1,024-2,048 for 4+ writers above ~10% loss), small-sample batching (stage 0 missed its bar; opt-in stays).
- Parked, only on the user's go:
  - 32-Pi testbed items: slow-reader flow control (`C7W`, branch `c7-wire-room`); link broadcast vs per-reader unicast above 2 readers (`tt_UNICAST_PEER_THRESHOLD`).
  - Security (v3.0): SROS2-compatible, on MACsec; only refusing `ROS_SECURITY_ENFORCEMENT=Enforce` exists.
  - rmw_tickle at REP-2004 Quality Level 2 (after Security).
  - Internet paths and congestion control (not started); rig interrupt coalescing and 10BASE-T1S hardware.
- Release order (user, 2026-11-01): 2.1 performance, 2.2 wireless + time-varying, 3.0 Security.

## Known limits

- No security: any node can claim any identity; use an isolated VLAN or link.
- No DDS interop; no congestion control, NAT traversal or encryption, so no internet paths.
- T1 wireless simulation is a relative A/B instrument; no figure is Wi-Fi performance.
- Slow-reader repair bursts (flow control) are a documented limit (`C7W`).
- rmw receive return on the Pi is +0.7 to +2.4 us behind CycloneDDS / FastDDS (io_uring deferred completion; `KH2W`).
- Delay step down is learned slowly (~12 s from 20 to 1 ms).
- Caller supplies endpoint storage (`tt_*_storage_size()`), which must outlive the endpoint.
- zenoh-pico reliable p2-p4 cells are not measurable; never quote them.
