# TickLE

Real-time publish/subscribe and RPC middleware for ROS 2, written in C, aimed at 10Base-T1S and other small,
single-purpose networks. No heap allocation, a single-threaded core per context, a fixed-size wire format, and an
optional shared-memory transport for peers on the same host. `rmw_tickle` makes it a ROS 2 middleware (`rmw`).

## Results in one screen

Measured on two Raspberry Pis against FastDDS and CycloneDDS with the same QoS ([docs/RESULTS.md](docs/RESULTS.md)).

| | TickLE | FastDDS | CycloneDDS |
|---|---:|---:|---:|
| Same host, BEST_EFFORT p4 (2800 B), delivered Mbps | **25,066** | 0.70 | 1,854 |
| Same host, RELIABLE p4, delivered Mbps | **14,917** | 1,924 | delivery failed |
| Same host, round trip p2 (1292 B), ms | **0.023** | 0.107 | 0.074 |
| Same host, publisher CPU per sample sent (BEST_EFFORT p4; not scored, no FastDDS receive count) | 1.07 us | 1.69 us | – |
| Same host, publisher memory (BEST_EFFORT p4) | **3.2 MB** | 15.9 MB | – |
| Cross host, native campaign (162 metric cells) | **151 wins**, 11 draws or ties, 0 losses | | |

Read the caveats before quoting a row: throughput is goodput, the samples the receiving application got complete,
and on one host the vendors' default BEST_EFFORT readers take far less than their writers send; CycloneDDS's RELIABLE
runs there delivered 0.02-0.07% fewer samples than they sent and are not scored ([docs/RESULTS.md](docs/RESULTS.md),
"Same host").

## Build

```sh
make all                      # libtickle.a and the examples, in platform/linux/
make all BUILD_TYPE=release   # -O2 -DNDEBUG instead of the default -O0 -g
make install PREFIX=/usr/local
```

Platforms: **Linux** (`src/hal_linux.c`) and **FreeRTOS + lwIP** (`platform/freertos/`, RISC-V under QEMU).
The public headers need C11. Every optional feature is a `#define` in [include/tickle/config.h](include/tickle/config.h).

## Use it

- **The library never allocates.** Every struct and string you pass must outlive the endpoint. Larger buffers
  (a publisher's retained-sample cache, a subscriber's reorder buffer, service storage) are yours to provide and size.
- **Threads.** With `tt_THREAD_SAFE` (the default) any thread may call into a context while one thread polls it.
- **Choose the link explicitly.** Pass `-b` with the directed broadcast of the link you mean (for example
  `192.168.10.255`). The library default `255.255.255.255` follows the host's default route, which is often the wrong
  network. Under ROS 2 set `TICKLE_BROADCAST_ADDR` the same way.
- **Delivery.** BEST_EFFORT may miss samples but never reorders or duplicates them. RELIABLE delivers in order per
  writer and recovers losses; KEEP_LAST gives up on samples the writer has evicted, KEEP_ALL refuses a write rather than
  drop a sample a reader has not acknowledged.

## Shared memory and io_uring (Linux)

Two processes on one host talk through a shared-memory ring instead of the kernel's UDP path, chosen automatically
when they discover each other. On Linux a busy context also learns of arriving datagrams from **io_uring** instead of
an empty read on every check; no thread is added.

| `tt_HAL_RX_HINT` | behaviour |
|---|---|
| `tt_RX_HINT_AUTO` (0, default) | io_uring when allowed; one warning and a read per check when refused |
| `tt_RX_HINT_READ` (1) | never io_uring; about 1.5 KB less code |
| `tt_RX_HINT_URING` (2) | io_uring or `tt_Context_create()` fails with `tt_RET_UNSUPPORTED` |

**Containers refuse io_uring by default.** Docker (25.0 and later) and Kubernetes' `RuntimeDefault` seccomp profile
block it, because io_uring has been a major source of Linux privilege-escalation bugs and its operations bypass
seccomp's per-call filter; Linux 6.6+ also has `kernel.io_uring_disabled`. TickLE still works when it is refused, only
slower for a busy publisher. Allowing it widens the kernel surface the container can reach - decide that per
deployment. TickLE needs only `io_uring_setup` and `io_uring_enter`:

```sh
curl -sSLo seccomp-default.json https://raw.githubusercontent.com/moby/moby/master/profiles/seccomp/default.json
jq '.syscalls += [{"names": ["io_uring_setup", "io_uring_enter"], "action": "SCMP_ACT_ALLOW"}]' \
    seccomp-default.json > seccomp-tickle.json
docker run --security-opt seccomp=seccomp-tickle.json ...
```

On Kubernetes use the same profile as `seccompProfile: {type: Localhost, localhostProfile: <path>}`. Avoid
`seccomp=unconfined`.

## How TickLE differs from DDS

TickLE borrows DDS/RTPS concepts (writers retain, readers NACK gaps, QoS matched request-versus-offer) but is its own
protocol. It does not interoperate with any DDS vendor, and some behaviour differs from what a Fast DDS or CycloneDDS
user expects. The DDS column is the default behaviour of those two.

| Behaviour | TickLE | DDS (Fast DDS / CycloneDDS default) | Why / what to expect |
|---|---|---|---|
| Wire protocol | Own UDP framing; payload in TickLE CDR-4 (4-byte max alignment) | RTPS; CDR / XCDR | No mixing with DDS nodes; 20 B framing per sample. [DESIGN 3](docs/DESIGN.md#3-wire-format-tt_version-11) |
| Discovery | Broadcast on the configured link, 1 s summaries, endpoint lists pulled on change; UDP even on one host | Multicast SPDP/SEDP | Set `TICKLE_BROADCAST_ADDR` to the link's directed broadcast. [DESIGN 5](docs/DESIGN.md#5-discovery) |
| Peer departure | Detected after 3.5 s of silence (up to 10 s with long leases) | ~10 s (Cyclone) / ~20 s (Fast DDS) lease | Faster failover; a host stalled for seconds is presumed gone and re-learned. [DESIGN 9](docs/DESIGN.md#9-qos-liveliness-deadline-lifespan-durability) |
| Contexts per link | One 8-bit context id per process, at most 254 on a link | 12-byte GUID prefix | Plan larger systems with this limit. [DESIGN 12](docs/DESIGN.md#12-context-ids-and-same-host-peers) |
| Several readers | Unicast to up to 2 readers, link broadcast to more | Unicast per reader, or multicast | One send reaches many readers; readers not subscribed still receive and discard the broadcast. [DESIGN 4](docs/DESIGN.md#4-sending-flush-batching-and-unicast-peers) |
| RELIABLE readers per publisher | 16 by default, a build setting (`-DRMW_TICKLE_MAX_RELIABLE_READERS=N`, core's `tt_MAX_ACK_ENTRIES`; 64 bytes per reader in every publisher); the next reader is not matched (logged) | Unlimited | Raise it at build time for topics every node subscribes to (e.g. `/clock`). [RMW build](docs/RMW.md#build-and-install) |
| RELIABLE heartbeats | While data is unacknowledged, a heartbeat 250 ms after the last sample or heartbeat of any kind (`RMW_TICKLE_IDLE_HEARTBEAT_NS`), repeated while unanswered; none in a stream of 8 Hz and up, none once everything is acknowledged. Also on every 64th sample, at match and after each large sample | Periodic while data is unacknowledged (Cyclone 100 ms, Fast DDS 3 s) | A lost **last** sample of a burst is repaired 125-250 ms after it was sent; a stream slower than 4 Hz pays one heartbeat and its acknowledgements per sample. [DESIGN 6](docs/DESIGN.md#6-reliability), [RMW QoS](docs/RMW.md#qos) |
| Repair timing | Immediate NACK and resend, retry timer from the measured round trip | Fixed response delays (ms range) | Much better throughput under loss; a few duplicate repairs. [DESIGN 6](docs/DESIGN.md#6-reliability) |
| Large samples | MTU-sized units, each with its own sequence number; while the path is clean up to 45 go in one IP-fragmented datagram, on loss they go one per datagram and only a lost unit is resent; up to 8 MiB in rmw (`RMW_TICKLE_MAX_SAMPLE_BYTES`) | IP-fragmented 64 KB datagrams plus DATA_FRAG | Robust under loss without paying per-datagram cost on a clean link. [DESIGN 8](docs/DESIGN.md#8-fragmentation-and-large-samples) |
| UDP checksum | 0 (none) on a large sample's IP-fragmented datagrams by default (Linux) | Kernel default (on) | No NIC checks an IP fragment, so this saves the receiver a software checksum; each frame keeps the link's CRC. `TT_UDP_CSUM0=0` keeps every checksum (see Tuning). |
| KEEP_ALL bound | Bytes: 512 KiB per publisher and per subscription (`RMW_TICKLE_KEEP_ALL_BYTES`, `RMW_TICKLE_READER_KEEP_ALL_BYTES`) | Samples (`max_samples`; Fast DDS 5,000, CycloneDDS unlimited) | A KEEP_ALL publish blocks up to `RMW_TICKLE_MAX_BLOCKING_MS` (100) when the bound is reached. [RMW QoS](docs/RMW.md#qos) |
| KEEP_LAST publisher history | Capped at 1 MiB (`RMW_TICKLE_CACHE_BYTES`) | `depth` samples | With large samples fewer than `depth` are kept for repair and late joiners (a warning says so). Raise the budget for latched maps or images. |
| TRANSIENT_LOCAL | The publisher pushes its history to a new subscriber at discovery | Reader pulls it through HEARTBEAT/ACKNACK | Late joiners get the backlog sooner. TRANSIENT and PERSISTENT are not supported. |
| LIFESPAN | A publisher's lifespan stops resends and replay; a subscription expires queued samples by **its own** lifespan | The writer's lifespan expires samples at the reader | Set lifespan on the subscription too if readers must drop stale samples. |
| Source timestamp | 32-bit microseconds; hosts' clocks must agree within ±35 minutes | 64-bit, sub-ns | Keep clocks synchronised (NTP/PTP) if you use source timestamps or a subscription's lifespan. As in DDS, only these use the date: timers, retries, liveliness and the publisher's lifespan run on the monotonic clock, so a step of the date does not disturb them. [DESIGN 2](docs/DESIGN.md#2-context-nodes-endpoints-and-the-poll-loop) |
| Same-host transport | Automatic shared-memory ring per peer, no daemon; a full ring drops the arriving datagram (RELIABLE repairs it) | Fast DDS SHM / data-sharing; CycloneDDS via iceoryx (opt-in, RouDi daemon) | Nothing to configure. [DESIGN 10](docs/DESIGN.md#10-shared-memory-transport) |
| Loaned messages | Plain fixed-size types; also lent from socket receive buffers | Plain types, shared memory only | [RMW loaned messages](docs/RMW.md#loaned-messages) |
| Services | Up to 4 requests in flight per client (`tt_CLIENT_MAX_OUTSTANDING`, compile time), answered in any order; with all 4 in flight `rmw_send_request` waits up to 100 ms, then fails. Requests and responses are not split by TickLE (IP-fragmented above 1472 B, at most 64 KB); an unanswered request is dropped after 5 s | Requests in flight not bounded (KEEP_LAST history), fragmented like topics, no timeout | More than 4 calls at once from one client wait or fail; raise the constant if an application needs more. Keep service messages small on lossy links. A client with no known server still finds one. [DESIGN 7](docs/DESIGN.md#7-services-rpc) |
| Unbounded sequences | Fixed capacity per field (e.g. `Image` 64,000 B, `LaserScan` 4,096) unless overridden with a `.capacities` file | Unbounded | A longer sequence fails to publish. [RMW what works](docs/RMW.md#what-works-and-what-does-not) |
| QoS policies | RELIABILITY, DURABILITY, HISTORY, DEADLINE, LIFESPAN, LIVELINESS (AUTOMATIC, MANUAL_BY_TOPIC; lease ≥ 6 ms) | All DDS policies | Ownership, partitions, content filters, keyed instances, `wstring` and security are not supported (`RMW_RET_UNSUPPORTED`). |

- Everything TickLE cannot honour is refused with an error, never silently downgraded.
- QoS compatibility (reliability, durability, deadline, liveliness) is checked as in DDS, and both sides get the
  incompatible-QoS event.
- No authentication or encryption: use TickLE only on a controlled link ([DESIGN 13](docs/DESIGN.md#13-security-model)).

## Tuning for your platform

The defaults were checked on the test rig (two Raspberry Pi 5s, 1 GbE). The values below depend on the hardware, so
derive them for yours rather than copying ours. Constants are `-D` overrides at build time (every one in
`include/tickle/config.h` is `#ifndef`-guarded); rmw_tickle's two byte budgets are environment variables.

**UDP checksum.** On Linux, a large sample's IP-fragmented datagrams go with UDP checksum 0 by default: no NIC checks
an IP fragment, so the receiver would otherwise checksum every byte in software (about 0.1 ms per MB on the rig's
subscriber). Each frame is still covered by the link's CRC. Set `TT_UDP_CSUM0=0` in the environment to keep every
checksum, e.g. on a path through a router or a link without a frame CRC (DESIGN.md, "UDP checksum 0").

| constant | default | derive it as |
|---|---|---|
| `tt_SEGMENT_BYTES` | 768 KiB | slots = peak datagram rate x longest reader stall; bytes = slots x (16 + `tt_SEGMENT_SLOT_BYTES`). Slots round **down** to a power of two, so size for a power of two: 768 KiB is 512 slots of 1472 B. |
| `tt_SOCKET_BUFFER_SIZE` | 1 MiB | peak receive rate (bytes/s) x longest drain stall. The kernel clamps it to `net.core.rmem_max` / `wmem_max`, so raise those too. |
| `tt_RX_BATCH` (Linux HAL) | 32 | the datagrams one wake-up drains at your peak rate (p95). The rig drained about 30. rmw_tickle builds with 1 (its datagram is 64 KB), not yet measured. |
| `tt_RX_CLOCK_REFRESH` | 16 | the clock staleness you accept for receive stamps / processing time per datagram. 16 x ~85 ns = ~1.4 us on the dev PC. |
| `tt_RX_LOCK_CHUNK` | 8 | the longest wait you accept for a thread that publishes while the poll thread receives / processing time per datagram. |
| `tt_RELIABLE_RETRY_INITIAL` | 1 ms | about 4 x the link's round trip. It is used only until the first RTT sample, after which the retry follows the measured RTT. |
| `tt_DISCOVERY_REQUEST_RETRY` | 10 ms | above the link's round trip + `tt_CONTEXT_TX_INTERVAL`. Used only until a peer's first list answer is timed; after that the retry is that measured round trip + `tt_CONTEXT_TX_INTERVAL`. |
| `RMW_TICKLE_KEEP_ALL_BYTES` (env) | 512 KiB | link rate x the longest acknowledgement stall a KEEP_ALL writer should ride out: 42 ms at 100 Mbit/s, 420 ms at 10 Mbit/s. |
| `RMW_TICKLE_READER_KEEP_ALL_BYTES` (env) | 512 KiB | sample size x the samples an application may leave untaken before its writers are pushed back. |
| `RMW_TICKLE_TRACKING_WORDS` | 16 (1,024 samples) | 64 x words >= peak sample rate x the time a lost sample takes to repair. Tracking further back than the writer retains recovers nothing. |
| `RMW_TICKLE_HEARTBEATS_PER_WINDOW` | 16 | piggybacked Heartbeats per tracking window; the interval is window / this (64 at 1,024). Chosen by a rig sweep at max rate and 8% loss (every 64 left 0-1 window jumps a run, as a 1 ms periodic Heartbeat did); raise it if a reader still jumps its window under your loss. |

Constants that stand in for a time or a rate are being replaced by algorithms (docs/ROADMAP.md, "Now" 5a) and are
not listed here. Already derived from what the running system measures, so there is nothing to tune:
`tt_RELIABLE_RETRY_GRANULARITY` and `tt_CALL_RETRY_GRANULARITY`, the G of the retry timers' `srtt + max(G, 4 x
rttvar)`, are the context's own measured timer lateness (and twice it for a call) since 2026-10-08 - set one non-zero
only to fix G (docs/DESIGN.md, section 6).

## Run the examples

```sh
make createns         # two network namespaces, ns1 and ns2 (needs sudo for ip)
make runsubscriber    # in one terminal (ns2)
make runpublisher     # in another (ns1)
```

Also `runserver`/`runclient` (SetBool RPC), `runpong`/`runping` (round-trip time) and `runperf_server`/`runperf_client`
(throughput). Every example takes `-b` broadcast, `-a` bind address, `-p` port, `-I` context id, `-n` name and
`-l` log level, and prints a final `RESULT:` line.

## Test

```sh
make test            # unit tests against a mock HAL, no privileges
make test-samehost   # two nodes on one host: shared memory, io_uring and its fallback
make test-linux      # two nodes over the real Linux HAL in network namespaces (sudo for ip)
make test-freertos   # two nodes under QEMU (RISC-V toolchain and qemu-system-riscv32)
make check-gates     # every local gate CI also runs; push only when it exits 0
```

The hardware-in-the-loop rig and the comparison harnesses are described in [docs/TESTING.md](docs/TESTING.md).
Live status: <https://tsnlab.github.io/tickle/dev/bench/>.

## Security

TickLE has no authentication or encryption: any node on the network can claim any identity. Run it on an isolated
segment (a dedicated VLAN or physical link). Security work is parked until requested ([docs/ROADMAP.md](docs/ROADMAP.md)).

## Documents

| | |
|---|---|
| [docs/RESULTS.md](docs/RESULTS.md) | every measured comparison, with its build and raw data |
| [docs/DESIGN.md](docs/DESIGN.md) | how TickLE works and why: wire format, reliability, shared memory, configuration |
| [docs/RMW.md](docs/RMW.md) | `rmw_tickle`: what of ROS 2 is supported and how |
| [docs/TESTING.md](docs/TESTING.md) | test tiers, gates, the rig, and how measurements are kept fair |
| [docs/ROADMAP.md](docs/ROADMAP.md) | what is next, and what is parked |
| [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md), [docs/CHANGELOG.md](docs/CHANGELOG.md) | contributing, history |

`rmw_tickle` is Quality Level 4 ([QUALITY_DECLARATION.md](rmw_tickle/rmw_tickle/QUALITY_DECLARATION.md)).

## License

GPL-3.0-or-later, or a proprietary license from TSN Lab, Inc. on request.
