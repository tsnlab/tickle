# TickLE

Real-time publish/subscribe and RPC middleware for ROS 2, written in C, aimed at 10Base-T1S and other small,
single-purpose networks. No heap allocation, a single-threaded core per context, a fixed-size wire format, and an
optional shared-memory transport for peers on the same host. `rmw_tickle` makes it a ROS 2 middleware (`rmw`).

## Results in one screen

Measured on two Raspberry Pis against FastDDS and CycloneDDS with the same QoS ([docs/RESULTS.md](docs/RESULTS.md)).

| | TickLE | FastDDS | CycloneDDS |
|---|---:|---:|---:|
| Same host, BEST_EFFORT p4 (2800 B), send Mbps | **24,696** | 12,853 | 9,449 |
| Same host, RELIABLE p4, send Mbps | **14,900** | 2,041 | 4,651 |
| Same host, round trip p2 (1292 B), ms | **0.023** | 0.100 | 0.067 |
| Same host, publisher CPU per sample (BEST_EFFORT p4) | **1.07 us** | 1.69 us | – |
| Same host, publisher memory (BEST_EFFORT p4) | **3.2 MB** | 15.9 MB | – |
| Cross host, native campaign (162 metric cells) | **151 wins**, 11 draws or ties, 0 losses | | |

Read the caveats before quoting a row: the same-host BEST_EFFORT rows compare send rates, and the vendors' default
readers deliver far less than they send on one host ([docs/RESULTS.md](docs/RESULTS.md), "Same host").

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

## Tuning for your platform

The defaults were checked on the test rig (two Raspberry Pi 5s, 1 GbE). The values below depend on the hardware, so
derive them for yours rather than copying ours. Constants are `-D` overrides at build time (every one in
`include/tickle/config.h` is `#ifndef`-guarded); rmw_tickle's two byte budgets are environment variables.

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
not listed here.

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
