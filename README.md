# TickLE

TickLE is a small publish/subscribe and RPC middleware written in C. It works like DDS, the middleware under ROS 2,
but is much lighter. `rmw_tickle` plugs it into ROS 2 as an `rmw` (the ROS 2 middleware layer).

- **Where it runs:** Linux, and FreeRTOS with lwIP (RISC-V under QEMU).
- **What it is for:** real-time traffic on 10Base-T1S and other small, single-purpose networks.
- **How it stays small:** the core never allocates memory on the heap, each context runs on one thread, and the wire
  format is fixed-size. Peers on the same host talk through shared memory instead of the network.

The sections up to "Things to know before you use it" are a five-minute read. The rest is reference: Linux
defaults, tuning, and how TickLE differs from DDS.

## Why use it

Measured on two Raspberry Pi 5s over 1 GbE against FastDDS and CycloneDDS, same QoS on all three, no process pinned
to a core. *Goodput* is the data the receiving application actually got, complete; throughput has been scored as
goodput since 2026-10-17. P1 to P4 are payload sizes: 76, 1292, 1424 and 2800 bytes.

| | TickLE | FastDDS | CycloneDDS | source |
|---|---:|---:|---:|---|
| Cross host, round trip, P2 (ms, lower is better) | **0.231** | 0.315 | 0.275 | row 3, `09a0437d` |
| Cross host, RELIABLE goodput, P2 (Mbps) | **937** | 591 | 864 | row 8, `09a0437d` |
| Cross host, RELIABLE throughput kept at 5% loss, P1 | **86.9%** | 43.1% | 4.8% | row 16, `09a0437d` |
| Cross host, throughput client peak memory, P1 (KB) | **2,087** | 15,947 | 5,407 | row 28, `09a0437d` |
| Same host, BEST_EFFORT goodput, P4 (delivered Mbps) | **25,066** | 0.70 | 1,854 | S3, `e521bbba` |
| Same host, RELIABLE goodput, P4 (delivered Mbps) | **14,917** | 1,924 | delivery failed | S6, `e521bbba` |
| Same host, round trip, P2 (ms) | **0.023** | 0.107 | 0.074 | S7, `e521bbba` |

- **Across the network** TickLE is best in every scored row of latency, throughput, CPU, memory and bandwidth: 151
  wins, 11 draws or ties, **0 losses** in 162 metric cells (`09a0437d`; the goodput re-score changed no verdict).
- **On one host,** at the DDS default (BEST_EFFORT, KEEP_LAST 1), TickLE delivers every sample at the maximum rate,
  CycloneDDS about a fifth and FastDDS almost none. "Delivery failed": in every same-host RELIABLE run CycloneDDS
  delivered 0.02-0.07% fewer samples than it sent, so it is not scored.
- **Through ROS 2,** rmw_tickle wins all 52 scored same-host rows (`06dd78d8`). For large messages (1 MB and 4 MB
  arrays across the network) it scores WIN 52 / DRAW 23 / LOSE 5 of 80.

Every figure, its build and raw data, and where TickLE does not come first, are in [docs/RESULTS.md](docs/RESULTS.md).

## Build

```sh
make all                      # libtickle.a and the examples, in platform/linux/
make all BUILD_TYPE=release   # -O2 -DNDEBUG instead of the default -O0 -g
make install PREFIX=/usr/local
```

The public headers need C11. Linux uses `src/hal_linux.c`; FreeRTOS + lwIP is in `platform/freertos/`. Every optional
feature is a `#define` in [include/tickle/config.h](include/tickle/config.h). To build the ROS 2 layer, see
[docs/RMW.md](docs/RMW.md).

## Run the examples

```sh
make createns         # two network namespaces, ns1 and ns2 (needs sudo for ip)
make runsubscriber    # in one terminal (ns2)
make runpublisher     # in another (ns1)
```

Other pairs: `runserver`/`runclient` (a SetBool service call), `runpong`/`runping` (round-trip time) and
`runperf_server`/`runperf_client` (throughput). Every example takes `-b` broadcast address, `-a` bind address,
`-p` port, `-I` context id, `-n` name and `-l` log level, and ends with a `RESULT:` line.

## Test

```sh
make test            # unit tests against a mock HAL, no privileges
make test-samehost   # two nodes on one host: shared memory, io_uring and its fallback
make test-linux      # two nodes over the real Linux HAL in network namespaces (sudo for ip)
make test-freertos   # two nodes under QEMU (RISC-V toolchain and qemu-system-riscv32)
make check-gates     # every local gate CI also runs; push only when it exits 0
```

The hardware rig and the comparison harnesses are in [docs/TESTING.md](docs/TESTING.md). Live status:
<https://tsnlab.github.io/tickle/dev/bench/>.

## Things to know before you use it

- **Choose the network explicitly.** Pass `-b` with the directed broadcast address of the link you mean (for example
  `192.168.10.255`); under ROS 2 set `TICKLE_BROADCAST_ADDR`. The default, `255.255.255.255`, follows the host's
  default route, which is often the wrong network.
- **The library never allocates.** Every struct and string you pass must outlive the endpoint. You provide and size
  the larger buffers: a publisher's cache of retained samples, a subscriber's reorder buffer, service storage.
- **Threads.** With `tt_THREAD_SAFE` (the default) any thread may call into a context while one thread polls it.
- **Delivery.** BEST_EFFORT may miss samples but never reorders or duplicates them. RELIABLE delivers in order per
  writer and repairs losses. KEEP_LAST gives up on samples the writer has already evicted; KEEP_ALL refuses a write
  rather than drop a sample a reader has not acknowledged.
- **No security.** There is no authentication or encryption: any node on the network can claim any identity. Run
  TickLE only on an isolated segment, such as a dedicated VLAN or physical link. Security work is parked until
  requested ([docs/ROADMAP.md](docs/ROADMAP.md); DESIGN.md, "Security model").

## Linux defaults: shared memory, io_uring, UDP checksum 0

**Shared memory.** Two processes on one host switch from UDP to a shared-memory ring automatically when they discover
each other. There is nothing to configure and no daemon.

**io_uring.** On Linux a busy context learns of arriving datagrams from io_uring, the kernel's asynchronous I/O
interface, instead of trying an empty read on every check. No thread is added. Choose with `tt_HAL_RX_HINT`:

| `tt_HAL_RX_HINT` | behaviour |
|---|---|
| `tt_RX_HINT_AUTO` (0, default) | io_uring when allowed; when refused, one warning and a read per check |
| `tt_RX_HINT_READ` (1) | never io_uring; about 1.5 KB less code |
| `tt_RX_HINT_URING` (2) | io_uring, or `tt_Context_create()` fails with `tt_RET_UNSUPPORTED` |

**Containers refuse io_uring by default.** Docker (25.0 and later) and Kubernetes' `RuntimeDefault` seccomp profile
block it, because io_uring has been a major source of Linux privilege-escalation bugs and its operations bypass
seccomp's per-call filter. Linux 6.6+ also has `kernel.io_uring_disabled`. TickLE still works when it is refused,
only slower for a busy publisher. Allowing it widens the kernel surface the container can reach, so decide per
deployment. TickLE needs only `io_uring_setup` and `io_uring_enter`:

```sh
curl -sSLo seccomp-default.json https://raw.githubusercontent.com/moby/moby/master/profiles/seccomp/default.json
jq '.syscalls += [{"names": ["io_uring_setup", "io_uring_enter"], "action": "SCMP_ACT_ALLOW"}]' \
    seccomp-default.json > seccomp-tickle.json
docker run --security-opt seccomp=seccomp-tickle.json ...
```

On Kubernetes use the same profile as `seccompProfile: {type: Localhost, localhostProfile: <path>}`. Avoid
`seccomp=unconfined`.

**UDP checksum 0.** A large sample is sent as IP-fragmented datagrams, and on Linux these carry UDP checksum 0 (none)
by default. No NIC checks the checksum of an IP fragment, so the receiver would otherwise checksum every byte in
software (about 0.1 ms per MB on the rig's subscriber). Each frame is still covered by the link's CRC. Set
`TT_UDP_CSUM0=0` in the environment to keep every checksum, for example on a path through a router or over a link
without a frame CRC (DESIGN.md, "UDP checksum 0").

## Options you may tune

The defaults were checked on the test rig (two Raspberry Pi 5s, 1 GbE). They depend on the hardware: derive yours
rather than copy ours. Core constants are `-D` build overrides (each is `#ifndef`-guarded in
`include/tickle/config.h`); `(env)` marks an environment variable.

| option | default | derive it as |
|---|---|---|
| `tt_SEGMENT_BYTES` (shared-memory ring) | 768 KiB | slots = peak datagram rate x longest reader stall; bytes = slots x (16 + `tt_SEGMENT_SLOT_BYTES`). Slots round **down** to a power of two: 768 KiB is 512 slots of 1472 B. |
| `tt_SOCKET_BUFFER_SIZE` | 1 MiB | peak receive rate (bytes/s) x longest drain stall. The kernel caps it at `net.core.rmem_max` / `wmem_max`; raise those too. |
| `tt_RX_BATCH` (Linux HAL) | 32 | datagrams one wake-up drains at peak rate (p95); the rig drained about 30. rmw_tickle uses 1 (64 KB datagrams), not yet measured. |
| `tt_RX_CLOCK_REFRESH` | 16 | accepted clock staleness of receive stamps / time per datagram (16 x ~85 ns = ~1.4 us on the dev PC). |
| `tt_RX_LOCK_CHUNK` | 8 | longest accepted wait for a publishing thread while the poll thread receives / time per datagram. |
| `tt_RELIABLE_RETRY_INITIAL` | 1 ms | about 4 x the link's round trip; used only until the first round trip is measured. |
| `tt_DISCOVERY_REQUEST_RETRY` | 10 ms | above round trip + `tt_CONTEXT_TX_INTERVAL`; used only until a peer's first list answer is timed. |
| `RMW_TICKLE_KEEP_ALL_BYTES` (env) | 512 KiB | link rate x the longest acknowledgement stall a KEEP_ALL writer should ride out (42 ms at 100 Mbit/s, 420 ms at 10 Mbit/s). |
| `RMW_TICKLE_READER_KEEP_ALL_BYTES` (env) | 512 KiB | sample size x the samples an application may leave untaken before its writers are pushed back. |
| `RMW_TICKLE_TRACKING_WORDS` | 16 (1,024 samples) | 64 x words >= peak sample rate x time to repair a lost sample. Tracking beyond what the writer retains recovers nothing. |
| `RMW_TICKLE_HEARTBEATS_PER_WINDOW` | 16 | heartbeats piggybacked per tracking window; the interval is window / this (64 at 1,024). Chosen by a rig sweep at max rate and 8% loss (every 64 left 0-1 window jumps a run, as a 1 ms periodic heartbeat did); raise it if a reader still jumps its window under your loss. |

Constants that stand in for a time or a rate are being replaced by algorithms (ROADMAP.md, "Now" 5a) and are not
listed. The retry timers' granularity G in `srtt + max(G, 4 x rttvar)` (`tt_RELIABLE_RETRY_GRANULARITY`,
`tt_CALL_RETRY_GRANULARITY`) is measured by the context itself since 2026-10-08 (twice it for a call); set one
non-zero only to fix G (DESIGN.md, "Reliability"). More rmw settings are in [docs/RMW.md](docs/RMW.md).

## How TickLE differs from DDS

TickLE borrows DDS/RTPS ideas: writers keep samples, readers report gaps with a NACK (negative acknowledgement), and
QoS is matched requested-against-offered. But it is its own protocol and does **not** interoperate with any DDS
vendor. "DDS" below is the default behaviour of Fast DDS and CycloneDDS.

| Behaviour | TickLE | DDS | What to expect |
|---|---|---|---|
| Wire protocol | Own UDP framing; payload in TickLE CDR-4 (4-byte max alignment) | RTPS; CDR / XCDR | No mixing with DDS nodes; 20 B framing per sample. |
| Discovery | Link broadcast, 1 s summaries, endpoint lists pulled on change; UDP even on one host | Multicast SPDP/SEDP | Set `TICKLE_BROADCAST_ADDR` to the link's directed broadcast. |
| Peer departure | 3.5 s of silence (up to 10 s with long leases) | ~10 s (Cyclone), ~20 s (Fast DDS) | Faster failover; a host stalled for seconds is dropped and re-learned. |
| Contexts per link | One 8-bit id per process, at most 254 per link | 12-byte GUID prefix | Plan large systems around it. |
| Several readers | Unicast to up to 2 readers, broadcast to more | Unicast per reader, or multicast | One send reaches many; hosts not subscribed discard it. |
| RELIABLE readers per publisher | 16; the next is not matched (logged). `-DRMW_TICKLE_MAX_RELIABLE_READERS=N` (core `tt_MAX_ACK_ENTRIES`), 64 B each per publisher | Unlimited | Raise it for topics every node subscribes to, e.g. `/clock`. |
| RELIABLE heartbeats | While unacknowledged, 250 ms after the last send (`RMW_TICKLE_IDLE_HEARTBEAT_NS`), repeated while unanswered; none at 8 Hz and up. Also every 64th sample, at match, after a large sample | Periodic while unacknowledged (Cyclone 100 ms, Fast DDS 3 s) | A lost **last** sample of a burst is repaired 125-250 ms after sending; below 4 Hz each sample costs a heartbeat and its acknowledgements. |
| Repair timing | Immediate NACK and resend; retry from the measured round trip | Fixed response delays (ms) | Far better throughput under loss; a few duplicate repairs. |
| Large samples | MTU-sized units with their own sequence numbers: up to 45 per IP-fragmented datagram on a clean path, one per datagram under loss; only lost units resent. Max 8 MiB in rmw (`RMW_TICKLE_MAX_SAMPLE_BYTES`) | IP-fragmented 64 KB datagrams + DATA_FRAG | Robust under loss, cheap on a clean link. |
| UDP checksum | 0 on a large sample's IP fragments (Linux default) | On | See "Linux defaults" above; `TT_UDP_CSUM0=0` restores it. |
| KEEP_ALL bound | 512 KiB per publisher and per subscription (`RMW_TICKLE_KEEP_ALL_BYTES`, `RMW_TICKLE_READER_KEEP_ALL_BYTES`) | Samples (`max_samples`: Fast DDS 5,000, Cyclone unlimited) | At the bound a publish blocks up to `RMW_TICKLE_MAX_BLOCKING_MS` (100). |
| KEEP_LAST publisher history | At most 1 MiB (`RMW_TICKLE_CACHE_BYTES`) | `depth` samples | Large samples: fewer than `depth` kept for repair and late joiners (warned). Raise it for latched maps or images. |
| TRANSIENT_LOCAL | Publisher pushes its history to a new subscriber | Reader pulls it (HEARTBEAT/ACKNACK) | Late joiners get the backlog sooner. No TRANSIENT or PERSISTENT. |
| LIFESPAN | Publisher's lifespan stops resends and replay; a subscription expires queued samples by **its own** lifespan | Writer's lifespan applies at the reader | Set lifespan on the subscription too. |
| Source timestamp | 32-bit microseconds; clocks must agree within ±35 minutes | 64-bit, sub-ns | Sync clocks (NTP/PTP) if you use source timestamps or subscription lifespan. Everything else runs on the monotonic clock. |
| Same-host transport | Automatic shared-memory ring per peer, no daemon; a full ring drops the datagram (RELIABLE repairs it) | Fast DDS SHM / data-sharing; Cyclone via iceoryx (opt-in, RouDi daemon) | Nothing to configure. |
| Loaned messages | Plain fixed-size types, also lent from socket receive buffers | Plain types, shared memory only | RMW.md, "Loaned messages". |
| Services | 4 requests in flight per client (`tt_CLIENT_MAX_OUTSTANDING`), answered in any order; a 5th waits up to 100 ms, then fails. IP-fragmented above 1472 B, max 64 KB; unanswered after 5 s = dropped | Unbounded, fragmented like topics, no timeout | Raise the constant for more concurrent calls; keep service messages small on lossy links. A client with no known server still finds one. |
| Unbounded sequences | Fixed capacity per field (e.g. `Image` 64,000 B, `LaserScan` 4,096) unless set in a `.capacities` file | Unbounded | A longer sequence fails to publish. |
| QoS policies | RELIABILITY, DURABILITY, HISTORY, DEADLINE, LIFESPAN, LIVELINESS (AUTOMATIC, MANUAL_BY_TOPIC; lease ≥ 6 ms) | All | Ownership, partitions, content filters, keyed instances, `wstring` and security return `RMW_RET_UNSUPPORTED`. |

Anything TickLE cannot honour is refused with an error, never silently downgraded. QoS compatibility (reliability,
durability, deadline, liveliness) is checked as in DDS, and both sides get the incompatible-QoS event. Each row is
explained in [docs/DESIGN.md](docs/DESIGN.md) and [docs/RMW.md](docs/RMW.md).

## Read more

| | |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | How TickLE works and why: wire format, reliability, shared memory, configuration. |
| [docs/RESULTS.md](docs/RESULTS.md) | Every measured comparison, with its build and raw data. |
| [docs/RMW.md](docs/RMW.md) | `rmw_tickle`: which parts of ROS 2 are supported, and how. |
| [docs/TESTING.md](docs/TESTING.md) | Test tiers, gates, the rig, and how measurements are kept fair. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | What is next, and what is parked. |
| [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md), [docs/CHANGELOG.md](docs/CHANGELOG.md) | How to contribute; history. |

`rmw_tickle` is Quality Level 4 ([QUALITY_DECLARATION.md](rmw_tickle/rmw_tickle/QUALITY_DECLARATION.md)).

## License

GPL-3.0-or-later, or a proprietary license from TSN Lab, Inc. on request.
