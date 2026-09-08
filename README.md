# TickLE: Real-Time ROS2 communication middleware optimized for 10Base-T1S

See [DESIGN.md](DESIGN.md) for the wire protocol, class/sequence diagrams, and the reasoning
behind TickLE's performance/reliability tradeoffs (fixed-size caches instead of `malloc`/`free`,
`poll()`-based I/O, single-threaded-per-node concurrency, ...) and its hardware-in-the-loop CI
architecture. See [CONTRIBUTING.md](CONTRIBUTING.md) for build/test/style expectations before
opening a PR.

## Platforms

TickLE has a real HAL (`include/tickle/hal_<platform>.h` + `src/hal_<platform>.c`) for two
platforms:

- **Linux** - the native build below, over real kernel UDP sockets (`src/hal_linux.c`).
- **FreeRTOS + lwIP**, cross-built for RISC-V and run under QEMU (`platform/freertos/`, own
  [Makefile](platform/freertos/Makefile)) - over lwIP's socket API against a from-scratch
  virtio-net driver (`platform/freertos/board/virtio_net.c`). See
  [platform/freertos/run_pair.sh](platform/freertos/run_pair.sh) for a real two-instance
  ping/pong round trip under QEMU, or `make test-freertos` below to run it.

There's no fallback HAL for any other platform - `include/tickle/hal.h` fails to compile with a
clear `#error` naming these two instead of silently offering a HAL that doesn't exist.

`examples/` is organized by platform: `examples/linux/<protocol>/` holds each protocol's generated
codec (e.g. `PingPong.{c,h}`) together with its argv-parsed POSIX driver (see "Run examples"
below), and `examples/freertos/<protocol>/` holds just that protocol's FreeRTOS driver (a task with
compile-time-fixed config - there's no argv on a flashed embedded target), cross-compiling the same
codec straight out of `examples/linux/<protocol>/` rather than duplicating it. Not every protocol
has a FreeRTOS driver yet - only `ping_pong` and `uint64`, the two `test-freertos` exercises. The loose
`.msg`/`.srv` files directly under `examples/` are platform-neutral interface definitions, shared
by every driver of every protocol.

## Security & concurrency model

TickLE has no authentication or encryption: any node on the broadcast domain can send a
packet claiming any `source` ID, and there's no way to verify a peer's identity. This is an
intentional tradeoff for its target (10Base-T1S, typically a physically isolated,
single-purpose automotive/industrial segment) - don't run it on a shared or untrusted network
without your own isolation (a dedicated VLAN/physical segment).

A `struct tt_Node` and its endpoints are not thread-safe: create, poll, and destroy a given
node from a single thread. Sharing one node across threads needs external locking of your
own - none is provided internally.

## Build

```sh
$ make all        # Build the library, then build all examples
$ make library    # Build libtickle.a only
$ make examples   # Build all examples
$ make set_bool   # Build SetBool client/server examples
$ make uint64     # Build UInt64 publisher/subscriber examples
$ make ping_pong  # Build the ping/pong latency-measurement example
$ make perf       # Build the perf_client/perf_server throughput example
```

Add `BUILD_TYPE=release` for an optimized build (`-O2 -DNDEBUG`) instead of the default
debug build (`-O0 -g`); each mode keeps its own object cache under `obj/<type>/`, so
switching between them doesn't need a `make clean` in between:

```sh
$ make all BUILD_TYPE=release
```

## Tests

```sh
$ make test           # Unit tests only (mock HAL - no real sockets, no network namespaces, no QEMU)
$ make test-linux     # Real RPC and pub/sub round trips over Linux network namespaces (src/hal_linux.c)
$ make test-freertos  # Real RPC and pub/sub round trips under QEMU (platform/freertos, src/hal_freertos.c)
$ make test-all       # All three of the above, in order - what CI runs (test-all.yml)
```

Each `tests/test_*.c` is a small, framework-free, whitebox unit test: it `#include`s
`src/tickle.c` directly (to reach its `static` functions) and links against a mock HAL
(`tests/test_mock.h`) instead of `hal_linux.c`, so it runs with no real sockets/network and no
timing dependency. `make test` builds and runs every one, stopping at the first failure.

`test-linux` and `test-freertos` (named for the platform under test, matching examples/linux and
examples/freertos - not the mechanism behind each) instead exercise a real platform HAL end to
end: two independent
processes (or QEMU instances) actually exchanging packets, not a mock, over Linux network
namespaces and emulated virtio-net respectively (see [netns_run_pair.sh](netns_run_pair.sh) /
[platform/freertos/run_pair.sh](platform/freertos/run_pair.sh)). Each runs both a `ping`/`pong`
round trip (RPC - call/response) and a `publisher`/`subscriber` round trip (pub/sub) - the latter
specifically covers `tt_Publisher_publish()`'s batched-not-immediately-flushed send path, which
RPC's always-immediately-flushed `tt_Client_call()` never exercises at all (see
`examples/freertos/uint64/main_publisher.c`'s file-level comment, or DESIGN.md's "RPC flushes
immediately; Publish batches"). Both tiers need `sudo` (namespaces) or the RISC-V toolchain +
`qemu-system-riscv32` (see [platform/freertos/Makefile](platform/freertos/Makefile)'s `lint`
target for the exact packages), so they're not part of plain `make test`. The two-Raspberry-Pi
hardware-in-the-loop performance test below is a separate, fourth tier - it needs real hardware,
so there's no local `make test-*` equivalent for it.

## Run examples
```sh
$ make createns
$ make runserver      # Launch SetBool service server on ns2 namespace
$ make runclient      # Launch SetBool service client on ns1 namespace
$ make runpublisher   # Launch UInt64 topic publisher on ns1 namespace
$ make runsubscriber  # Launch UInt64 topic subscriber on ns2 namespace
$ make runpong        # Launch ping/pong latency responder on ns2 namespace
$ make runping        # Send a ping every second and print its round-trip time on ns1 namespace
$ make runperf_server # Launch the perf_server (receiver) on ns2 namespace
$ make runperf_client # Send perf_client data sized to fill an Ethernet frame on ns1 namespace
```

### Command-line options

Every example binary accepts the same interface-configuration flags, on top of the
compiled-in defaults `make run*` relies on:

- `-b` broadcast address (default `192.168.10.255`)
- `-p` UDP port (default: compiled-in `tt_NODE_PORT`)
- `-a` bind address (default: compiled-in `tt_NODE_ADDRESS`)
- `-n` topic/service name to rendezvous on (default: each example's own hardcoded name, e.g.
  `bulk_topic`, `set_bool_server`) - pass the same `-n` on both sides if you override it, or
  they won't find each other
- `-l` log level: `debug|info|warning|error|none` (default `info`)

Senders (`ping`, `client`, `publisher`, `perf_client`) additionally take:

- `-c` stop after this many sends (default `0` = run until Ctrl+C)
- `-i` seconds between sends (default `1`, except `perf_client` - see below)

Receivers (`pong`, `server`, `subscriber`, `perf_server`) additionally take:

- `-d` exit automatically after this many seconds (default `0` = run until Ctrl+C)

`-c`/`-d` exist mainly so a script (e.g. CI) can run a binary without it hanging forever
waiting on a peer that never shows up.

`perf_client` takes two more flags to control what it sends:

```sh
$ ./perf_client [-s message_size_bytes] [-i interval_seconds]
```

- `-s` payload bytes per message (default/max: see "Message size: filling an Ethernet frame" below)
- `-i` seconds to wait between sends (default: see below; `0` sends as fast as `tt_Node_poll()` allows instead of on a fixed schedule)

### Message size: filling an Ethernet frame

A standard Ethernet frame carries at most 1500 bytes of payload (its MTU) before IP
fragmentation kicks in. Over UDP/IPv4, that leaves:

```
max UDP payload = 1500 (Ethernet MTU) - 20 (IPv4 header) - 8 (UDP header) = 1472 bytes
```

This 1472 is exactly `tt_MAX_BUFFER_LENGTH` (`config.h`) - the largest packet TickLE itself
will ever flush onto the wire, sized for precisely this reason. (It used to be defined as
1480, 8 bytes over this limit - a message in the 1473-1480 byte range would have passed
TickLE's own buffer check yet still fragmented at the IP layer on a standard network; it's
now 1472 so that can't happen.)

Each `BulkData` message costs a fixed amount of TickLE framing on top of its own payload
before it reaches that UDP payload: `tt_Header` (4B, once per packet) + `SubmessageHeader`
(4B) + `DataHeader` (16B) + `BulkData`'s own `seq`/`size` fields (8B) = **32 bytes**, assuming
one message per packet. So the largest payload that still fits one frame unfragmented is:

```
BULK_MAX_PAYLOAD_SIZE = tt_MAX_BUFFER_LENGTH - 32 = 1472 - 32 = 1440 bytes
```

(`Bulk.h` defines it exactly this way - derived from `tt_MAX_BUFFER_LENGTH` rather than
hardcoded - so the two can't drift apart again.)

`perf_client` defaults `-s` to exactly `BULK_MAX_PAYLOAD_SIZE` (1440) - the biggest packet this
protocol can put on the wire without fragmenting - so every send makes the most of one frame.
`-i` defaults to `0`, meaning no fixed schedule at all: publish as fast as `tt_Node_poll()`
allows, which is the right default for a throughput benchmark. On real 10Base-T1S hardware
the 10 Mbit/s link itself becomes the bottleneck well before max-size, unpaced sending would.

Pass `-i` to target a specific rate instead of maxing out. To hit exactly the line rate at
the default message size, for example:

```
interval_seconds = (message_size + 32) * 8 / line_rate_bps
                  = (1440 + 32) * 8 / 10,000,000 ≈ 0.0011776 sec
```

```sh
$ ./perf_client -i 0.0011776
```

`tt_Node_poll()`'s own call overhead sets a ceiling on how many times per second `perf_client`'s
loop can even check whether a send is due, independent of `-i`. `tt_receive()` waits for
readability with `poll()` rather than blocking on `recvfrom()` with `SO_RCVTIMEO`, so an idle
wait is capped at a real 1ms (`tt_NODE_TX_INTERVAL`) rather than the ~2ms an older,
`SO_RCVTIMEO`-based implementation measured on this hardware regardless of the requested
timeout. In practice it's usually faster than that worst case: `perf_client` also receives its
own broadcast echo, so most `tt_Node_poll()` calls return as soon as that arrives instead of
waiting out the full 1ms - measured at roughly 1,860 calls/sec (~0.54ms/call) with small
messages on this network. A requested `-i` much smaller than that won't be hit exactly (it'll
just behave like `-i 0`). `perf_client`'s interval reports and final summary always show the
throughput it actually achieved, not just what `-s`/`-i` imply; a `-i` well above ~1ms (e.g. `0.01`) is
paced accurately since it's comfortably larger than that ceiling.

## Continuous performance testing

Every push to `main` runs a hardware-in-the-loop latency and throughput test on two real
Raspberry Pi boards connected by an Ethernet link (`rpi#1` as client/sender, `rpi#2` as
server/receiver), via a self-hosted GitHub Actions runner:

- [`.github/workflows/performance.yml`](.github/workflows/performance.yml) - triggers on push
  to `main` (immediately - no debounce) or manually via `workflow_dispatch`; a newer push
  cancels an in-progress run for an older one instead of queuing both
- [`.github/scripts/run_perf.sh`](.github/scripts/run_perf.sh) - checks out the exact commit
  being tested on both Pis, builds, runs `ping`/`pong` for latency and
  `perf_client`/`perf_server` for throughput (using the `-c`/`-d` flags above so a run can never
  hang waiting on a peer), and writes the results to the job summary

Results are tracked over time and charted at **<https://tsnlab.github.io/tickle/dev/bench/>**;
the workflow fails if latency more than doubles, or throughput drops to less than half, versus
the last recorded run (`alert-threshold: "200%"` on each `github-action-benchmark` step).

## License
GPLv3 or proprietary license on request. Every source file carries a
`SPDX-License-Identifier: GPL-3.0-or-later` header; contact TSN Lab, Inc. for a proprietary
license.
