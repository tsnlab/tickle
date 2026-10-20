# TickLE

**TL;DR**
- A small C publish/subscribe and RPC middleware, like DDS but much lighter; `rmw_tickle` plugs it into ROS 2.
- Runs on Linux and on FreeRTOS + lwIP. Made for 10Base-T1S and other small, single-purpose networks.
- No heap allocation, one thread per context, fixed-size wire format; same-host peers use shared memory.
- Does not talk to DDS nodes, and has no security: use an isolated network.

## Why use it

Two Raspberry Pi 5s over 1 GbE, same QoS for all, nothing pinned. Goodput = data the receiving application got
complete. P1-P4 = 76, 1292, 1424, 2800-byte payloads.

| | TickLE | FastDDS | CycloneDDS | source |
|---|---:|---:|---:|---|
| Cross host, round trip from creation, P2 (ms) | **0.237** | 0.378 | 0.328 | row 3c, `596e9c9c` |
| Cross host, RELIABLE goodput, P2 (Mbps) | **937** | 591 | 864 | row 8, `09a0437d` |
| Cross host, RELIABLE throughput kept at 5% loss, P1 | **86.9%** | 43.1% | 4.8% | row 16, `09a0437d` |
| Cross host, client peak memory, P1 (KB) | **2,087** | 15,947 | 5,407 | row 28, `09a0437d` |
| Same host, BEST_EFFORT goodput, P4 (Mbps) | **25,066** | 0.70 | 1,854 | S3, `e521bbba` |
| Same host, RELIABLE goodput, P4 (Mbps) | **14,917** | 1,924 | delivery failed | S6, `e521bbba` |
| Same host, round trip from creation, P3 (ms) | **0.037** | 0.164 | 0.130 | S9c, `2579e822` |

Across the network TickLE scores 139 wins, 11 draws, 0 losses in 150 cells; through ROS 2 it wins all 52 same-host
rows. It still loses 2 rows: subscriber CPU per MB against FastDDS for 4 MB messages at 15 Hz (+4.5%), and one
100 us poll round trip against CycloneDDS through ROS 2 (+1.2%, row 63c). Every figure, and where TickLE does not come first: [docs/RESULTS.md](docs/RESULTS.md).

## Build

```sh
make all                      # libtickle.a and the examples, in platform/linux/
make all BUILD_TYPE=release   # -O2 -DNDEBUG instead of -O0 -g
make install PREFIX=/usr/local
```

C11 headers. Options: [include/tickle/config.h](include/tickle/config.h). ROS 2 layer: [docs/RMW.md](docs/RMW.md).

## Run an example

```sh
make createns         # network namespaces ns1 and ns2 (sudo for ip)
make runsubscriber    # terminal 1
make runpublisher     # terminal 2
```

Also `runserver`/`runclient`, `runpong`/`runping`, `runperf_server`/`runperf_client`. Flags: `-b` broadcast, `-a`
bind address, `-p` port, `-I` context id, `-n` name, `-l` log level.

## Test

```sh
make test          # unit tests, no privileges
make test-linux    # two nodes in network namespaces (sudo for ip)
make check-gates   # every local gate; push only when it exits 0
```

All tests and the rig: [docs/TESTING.md](docs/TESTING.md). Live status: <https://tsnlab.github.io/tickle/dev/bench/>.

## Know before you use it

| | |
|---|---|
| **Network** | Pass `-b` (or `TICKLE_BROADCAST_ADDR`) with your link's directed broadcast, e.g. `192.168.10.255`. The default `255.255.255.255` follows the default route, often the wrong network. |
| **Memory** | The library never allocates. An endpoint is created with a QoS profile and storage of `tt_*_storage_size()` bytes, which must outlive it. |
| **Delivery** | A zeroed QoS profile is rmw's default: RELIABLE, KEEP_LAST 10. BEST_EFFORT may miss samples, never reorders or duplicates. RELIABLE delivers in order per writer; KEEP_ALL refuses a write rather than drop an unacknowledged sample. Any thread may call a context while one thread polls it (`tt_THREAD_SAFE`). |
| **Security** | None: any node can claim any identity. Use a dedicated VLAN or link ([docs/ROADMAP.md](docs/ROADMAP.md)). |
| **Linux** | Shared memory on one host is automatic. io_uring is used when allowed; containers refuse it by default and TickLE then runs a little slower ([recipe](docs/DESIGN.md#containers-allowing-io_uring)). Large samples go with UDP checksum 0; `TT_UDP_CSUM0=0` keeps checksums. |
| **FreeRTOS** | Shared memory is off by default (nothing allocated, contexts as small as before). Turn it on only when several TickLE contexts in one image exchange data: build with `-Dtt_SEGMENT_ENABLED=1` (each context grows ~12.8 KB on RV32). Give it memory with `tt_freertos_set_segment_memory()` (a static region, `hal_freertos.h`) before creating contexts, or it takes 24 KiB per context from the FreeRTOS heap. |

Tuning and how TickLE differs from DDS: [docs/RMW.md](docs/RMW.md#tuning-and-differences-from-dds).

## Read more

| | |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | How it works and why. |
| [docs/RESULTS.md](docs/RESULTS.md) | Every measurement. |
| [docs/RMW.md](docs/RMW.md) | `rmw_tickle` and ROS 2. |
| [docs/TESTING.md](docs/TESTING.md) | Tests, gates, rig, scoring. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Next and parked work. |
| [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md), [docs/CHANGELOG.md](docs/CHANGELOG.md) | Contributing; history. |

`rmw_tickle` is Quality Level 4 ([QUALITY_DECLARATION.md](rmw_tickle/rmw_tickle/QUALITY_DECLARATION.md)).

## License

GPL-3.0-or-later, or a proprietary license from TSN Lab, Inc. on request.
