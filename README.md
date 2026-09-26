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
  virtio-net driver (`platform/freertos/board/virtio_net.c`).

There's no fallback HAL for any other platform - `include/tickle/hal.h` fails to compile with a
clear `#error` naming these two instead of silently offering a HAL that doesn't exist.

`platform/<name>/` holds each platform's own bring-up and dev/test tooling - board support,
network-stack glue, and a linker script for FreeRTOS (it has to do everything the OS would
normally provide); just [`platform/linux/netns.mk`](platform/linux/netns.mk)'s optional manual
helpers for running a single example process inside a network namespace for Linux (which gets a
real kernel, sockets, and process model for free). Each platform has a `test.sh` with the same
job: build and run a real two-instance round trip and assert on the result - `platform/linux/
test.sh` across a veth-joined pair of network namespaces (real distinct addresses, needs sudo for
`ip`), `platform/freertos/test.sh` under QEMU - which `make test-linux`/`make test-freertos` below
run.

`examples/<protocol>/` (`uint64`, `set_bool`, `ping_pong`, `perf`) holds that protocol's
platform-neutral interface definition (`.msg`/`.srv`) together with its generated codec (e.g.
`PingPong.{c,h}`) - see [`tools/typesupport`](tools/typesupport/) and
[CONTRIBUTING.md](CONTRIBUTING.md#generated-codecs); never hand-edit the generated `.c`/`.h`
there, edit the `.msg`/`.srv` and run `make regen` instead. `examples/linux/<protocol>/` holds
just that protocol's argv-parsed POSIX driver (see "Run examples" below), and
`examples/freertos/<protocol>/` holds just its FreeRTOS driver (a task with compile-time-fixed
config - there's no argv on a flashed embedded target) - both cross-compile against the same
`examples/<protocol>/` codec rather than duplicating it. All four protocols have a FreeRTOS
driver, matching what `test-freertos` exercises.

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

These all work from the repo root, but actually build in (and place `libtickle.a` and every
example binary into) [`platform/linux/`](platform/linux/Makefile) - the root `Makefile` is just a
thin forwarding shim to it, matching how `platform/freertos/` owns the FreeRTOS build. Run
examples from there, e.g. `./platform/linux/ping` or `cd platform/linux && ./ping`.

Add `BUILD_TYPE=release` for an optimized build (`-O2 -DNDEBUG`) instead of the default
debug build (`-O0 -g`); each mode keeps its own object cache under `platform/linux/obj/<type>/`,
so switching between them doesn't need a `make clean` in between:

```sh
$ make all BUILD_TYPE=release
```

### Integrating the library

`make library` produces `platform/linux/libtickle.a`; link that and add `include/` to your
include path. `make install` (static lib + headers + a `tickle.pc` pkg-config file; `PREFIX`/
`DESTDIR` overridable, `make uninstall` to remove) installs it properly - or vendor the tree, or
copy `libtickle.a` + `include/tickle/` into your project directly, if you'd rather not.

- **The public headers need C11** (`-std=c11` or newer - they use an anonymous union in
  `tt_Header`). They are otherwise `-pedantic`-clean.
- **The library never allocates or copies on your behalf**, on any path - there is no
  `malloc()`/`free()` anywhere in `src/`. Every struct you hand a `tt_Node_create*` call (the node,
  the client/server/publisher/subscriber, its service/topic) and every string (`endpoint_name`,
  `service->name`, `topic->name`) must outlive the endpoint - string literals are fine, a freed
  buffer is not.
- **Bigger buffers are yours to provide, size, and free.** Most storage is embedded in the structs
  and sized by `config.h`, so it needs nothing from you. Where the embedded size is the wrong
  shape, you attach your own instead: a publisher's retained-sample cache
  (`tt_Publisher.reliable_cache`), a subscriber's reorder buffer, and a server's or client's
  response storage (`tt_Server_set_storage()` / `tt_Client_set_storage()`). A static array on a
  microcontroller and a `malloc()` on Linux both work, because the library only ever holds the
  pointer. **The sizes are yours too**: a cache's sample count and byte size are that publisher's
  own DDS `RESOURCE_LIMITS`, which TickLE enforces and never chooses. See
  [DESIGN.md](DESIGN.md)'s "The library never allocates; the caller owns every buffer".
- **One `tt_Node` is single-threaded**: drive all of its calls from one thread (see
  [DESIGN.md](DESIGN.md), "Concurrency").
- Delivery is BEST_EFFORT unless a publisher is given a reliable cache. What each mode promises
  is under "Delivery guarantees" below.

## Delivery guarantees

Both modes make promises **per writer**. Samples from two different publishers have no order
relative to each other.

**BEST_EFFORT** - a reader may miss samples, but it never receives a sample it has already
moved past. A sample whose sequence number is not newer than the last one delivered from the
same writer is discarded. This is the DDS behaviour applications (and `performance_test`) rely on:
gaps are possible, and duplicates or reordering are not. A publisher that restarts starts a fresh
sequence and is accepted again.

**RELIABLE** - samples are delivered in **strictly increasing order** per writer. The reader
**waits** for a missing sample while it can still be recovered and asks for it again (ACKNACK). It
holds the samples that arrived after the gap until the gap is filled. How long "can still be
recovered" lasts depends on the writer's history:

- **KEEP_LAST(depth)**: the writer keeps its last `depth` samples. If the missing one has already
  been evicted, the writer says so (a Heartbeat carrying the oldest sequence it still has), and
  the reader skips the gap instead of waiting forever. If a sample arrives after its gap was
  given up, the reader discards it and counts it in `out_of_order_discarded`; it is never
  delivered out of order.
- **KEEP_ALL**: nothing is evicted before every matched reader has acknowledged it. When the
  cache is full, the writer blocks and then refuses the write (it reports a timeout) rather than
  drop data it accepted, which is the DDS contract for KEEP_ALL.

**Why a RELIABLE writer also sends Heartbeats.** The "already evicted" answer above travels in
reply to the reader's ACKNACK, and on a lossy link that ACKNACK or its reply can itself be lost.
At a high sample rate the reader's buffer fills before the retry succeeds, and the stream stalls.
A Heartbeat carrying the oldest retained sequence number, sent without waiting to be asked, closes
that hole. Two ways to send it:

| | how | cost | default |
|---|---|---|---|
| piggybacked | appended to every Nth DATA, in the same datagram | ~24 B per N samples, no extra packet | `rmw_tickle`: on, N = 64. Native API: off (`tt_Publisher.heartbeat_piggyback_every`) |
| periodic | its own datagram every period | one packet per period, whatever the data rate | off everywhere (`tt_Publisher_set_heartbeat_period()`) |

The piggybacked form scales with the data rate, which is exactly when the stall can happen. On a
quiet publisher it sends almost nothing, and there the reader's own retries are enough. Measured
through ROS 2 with 8% injected loss at maximum rate: window stalls went from 505-1151 per 15 s run
to 0-1 at N = 64 (`rmw_tickle/PLAN.md`, "RELIABLE under the default ROS 2 profile").

## Tests

```sh
$ make test           # Unit tests only (mock HAL - no real sockets, no QEMU) - no privilege needed
$ make test-linux     # Real RPC and pub/sub round trips across a veth namespace pair (src/hal_linux.c) - sudo for `ip`
$ make test-freertos  # Real RPC and pub/sub round trips under QEMU (platform/freertos, src/hal_freertos.c)
$ make test-all       # All three of the above, in order - what CI runs (test-all.yml)
```

Each `tests/test_*.c` is a small, framework-free, whitebox unit test: it `#include`s
`src/tickle.c` directly (to reach its `static` functions) and links against a mock HAL
(`tests/test_mock.h`) instead of `hal_linux.c`, so it runs with no real sockets/network and no
timing dependency. `make test` builds and runs every one, stopping at the first failure.

`test-linux` and `test-freertos` (named for the platform under test, matching examples/linux and
examples/freertos - not the mechanism behind each) instead exercise a real platform HAL end to
end: two independent processes (or QEMU instances) actually exchanging packets, not a mock, over
a real UDP broadcast between two network namespaces and emulated virtio-net respectively (see
[platform/linux/test.sh](platform/linux/test.sh) /
[platform/freertos/test.sh](platform/freertos/test.sh)). `test-linux` needs passwordless sudo for
`ip` (it creates the namespace pair); `test-freertos` needs the RISC-V toolchain +
`qemu-system-riscv32` (see [platform/freertos/Makefile](platform/freertos/Makefile)'s `lint`
target for the exact packages) - so neither is part of plain `make test`.

Every TickLE example doubles as a functional/performance test of the library itself (see "Run
examples" below), so each tier runs all four example pairs, in this order: `uint64` (pub/sub),
`set_bool` (RPC), `ping_pong` (RPC, latency-flavored - also the pair `tt_Publisher_publish()`'s
batched-not-immediately-flushed send path skips, see DESIGN.md's "RPC flushes immediately;
Publish batches"), `perf` (pub/sub, throughput-flavored). Each example reports its own result
when run - see "Command-line options" below - and each tier's script checks exactly that:
functional pairs (`uint64`, `set_bool`) for a `RESULT: PASS` line, performance pairs (`ping_pong`,
`perf`) for real evidence of at least a handful of genuine round trips (a clean exit alone doesn't
prove anything actually arrived).

The two-Raspberry-Pi hardware-in-the-loop performance test below is a separate, fourth tier and
not part of `test-all` either - it needs real hardware, so there's no local `make test-*`
equivalent for it (see "Continuous performance testing" below).

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
- `-I` explicit node ID `1`-`254`, overriding auto-detection from `-a`/`-b`'s subnet (see
  `_tt_CONFIG.node_id`'s own comment in `config.h`). Auto-detection needs each side to have its
  own distinct address in that subnet - real separate hosts/namespaces give that for free (which
  is why `test-linux`'s veth namespace pair doesn't need `-I`), but two processes sharing one
  network namespace/interface can't be told apart that way, so `-I` fills in for it there.
- `-n` topic/service name to rendezvous on (default: each example's own hardcoded name, e.g.
  `bulk_topic`, `set_bool_server`) - pass the same `-n` on both sides if you override it, or
  they won't find each other
- `-l` log level: `debug|info|warning|error|none` (default `info`)

### Choosing which interface a node talks on

A node sends on one link, configured as a pair: `-b`, the broadcast address it addresses announces
to, and `-a`, the local address it sends from. (One pair today; per-link configuration on a
multi-homed node is a separate, later question.) Getting this right matters more than it looks on
a machine with more than one interface, and the three defaults in play are not the same value:

| | broadcast default | scoped by the routing table? |
|---|---|---|
| These example binaries | `192.168.10.255` (each example sets it) | yes |
| The library itself (`_tt_NODE_BROADCAST`, `config.h`) | `255.255.255.255` | **no** |
| `rmw_tickle` under ROS 2 | the library's, unless `TICKLE_BROADCAST_ADDR` is set | **no** |

That difference is not academic: it is why these examples have never left their link and why a
`rmw_tickle` deployment did.

- **`255.255.255.255` is the *limited* broadcast.** It has no subnet, so there is
  nothing for the routing table to scope it by, and the kernel sends it out **whatever the host's
  default route points at**. On a machine whose default route is a management network and whose
  real traffic belongs on a separate segment, that is the wrong interface - and nothing reports
  it, because the send succeeds either way. On 2026-09-23 this put a benchmark's traffic onto a
  shared lab network for a day.
- **A *directed* broadcast is scoped by the routing table on its own.** For a node on
  `192.168.10.0/24`, `-b 192.168.10.255` leaves on the interface owning that subnet with no socket
  binding involved - `ip route get 192.168.10.255` will name it. This is the normal answer and it
  is sufficient by itself: set `-b` to the directed broadcast of the link you mean.
- **`-a` pins sends to an interface when you cannot use a directed broadcast.** Set it to this
  node's own address on that link. Measured on a two-interface host, 20 samples per run, three
  runs each, counted with `tcpdump` on *both* interfaces: with `-a` unset, 0 datagrams on the test
  link and 22-23 on the management one; with `-a` set to the test link's address, 22-23 on the
  test link and **0** on the management one.
- `-a` binds the data socket only. The well-known port stays bound to the wildcard, and that is
  deliberate: a socket bound to a unicast address receives no broadcasts at all, so binding that
  one would silently stop discovery while unicast kept working - a node that hears nobody and is
  heard by nobody, with every send reporting success.

Under ROS 2 there is no argv to pass, so `rmw_tickle` reads the same two settings from the
environment at `rmw_init()`: `TICKLE_BROADCAST_ADDR` for `-b` and `TICKLE_NODE_ID` for `-I`. A ROS
deployment that leaves `TICKLE_BROADCAST_ADDR` unset gets the limited broadcast and the default
route with it, which is exactly the case that leaked above. Set it.

The other `rmw_tickle` environment variables are tuning knobs. An unparseable value falls back to
the default rather than stopping the node:

| variable | effect | default |
|---|---|---|
| `RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY` | piggyback a Heartbeat on every Nth RELIABLE sample (see "Delivery guarantees"); `0` turns it off | `64` |
| `RMW_TICKLE_HEARTBEAT_PERIOD_NS` | also send a periodic Heartbeat at this period | unset (off) |
| `RMW_TICKLE_MAX_BLOCKING_MS` | how long a KEEP_ALL publish may block before it fails; `0` fails at once | `100` |
| `RMW_TICKLE_CACHE_BYTES` | byte budget for a KEEP_LAST publisher's retained samples; a publisher of large samples then keeps fewer than its depth | 1 MiB |
| `RMW_TICKLE_KEEP_ALL_BYTES` | byte budget for what a VOLATILE KEEP_ALL publisher holds unacknowledged; past it the writer blocks, it never drops. The same order as CycloneDDS's own default writer bound (500 kB) | 512 KiB |
| `RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES` | storage reserved per retained KEEP_ALL sample, for types whose size the generator cannot bound; a larger sample fills the cache faster, so the publisher blocks sooner | 1472 bytes (one standard datagram) |
| `RMW_TICKLE_REORDER_SLOTS` | how many out-of-order RELIABLE samples a subscription may hold; fewer saves memory and costs retransmissions | the tracking window |

Those two storage knobs can also be set for one publisher instead of the whole process, which
matters when a node has one large-sample topic among forty: fill in an
`rmw_tickle_publisher_payload_t` (`rmw_tickle_c/publisher_payload.h`) and point
`rmw_publisher_options_t.rmw_specific_publisher_payload` at it - through `rclcpp`, by subclassing
`rclcpp::detail::RMWImplementationSpecificPublisherPayload`. It must outlive the publishers created
with it, and a field left `0` keeps that knob's environment-wide value. A payload meant for another
rmw implementation is ignored with a warning rather than misread.

Its `max_sample_bytes` is worth knowing about even if nothing else is: it says how large this
publisher's samples really get, and is clamped up by what the type can produce, so its use is to go
*lower*. A type bounded at 64 KB whose images are really 8 KB costs eleven 64 KB records at depth
10; saying `8192` keeps the same ten samples for an eighth of the memory. Reserving too little is
safe - a KEEP_LAST publisher keeps fewer than its depth, a KEEP_ALL one blocks sooner, and neither
drops a sample a reader has not acknowledged.

**Standard ROS 2 messages need a one-time build.** The interface packages installed with ROS 2
(`std_msgs`, `geometry_msgs`, `sensor_msgs`, `rcl_interfaces`, ...) carry typesupport for FastDDS
and CycloneDDS, not for TickLE. Without it, creating a publisher or subscription fails with "no
rmw_tickle typesupport for this message type", and an ordinary `rclcpp::Node` cannot even start,
because it creates its own `/rosout`, parameter services and `~/get_type_description`. Build them
once into a workspace of your own:

```sh
source /opt/ros/$ROS_DISTRO/setup.bash
source <rmw_tickle install>/setup.bash
rmw_tickle/scripts/build_ros2_interfaces.sh -w ~/tickle_ifaces_ws -a   # every standard package
source ~/tickle_ifaces_ws/install/setup.bash
```

`-a` builds all 23 jazzy interface packages that TickLE ships capacities for (about 7 minutes on a
CI runner). To build only some, name them instead, and the packages they depend on are added. After
that, a default `rclcpp::Node` starts and runs with no extra parameters, and actions work through
`rclcpp_action` (`example_interfaces/action/Fibonacci` is checked in CI). The one type that is
declined is `example_interfaces/msg/WString`, since TickLE has no `wstring` (below). Your own interface
packages get TickLE typesupport the same way, by building them with `rmw_tickle`'s install sourced. A package of
yours that was configured before the workspace existed has the installed interface packages cached
in its CMake cache and keeps linking them; build it afresh once, with the workspace sourced.

- **No `wstring`.** A message with a `wstring` field gets no TickLE typesupport: it is declined,
  the reason is written into the generated file, and every other type in its package still builds.
  That is a decision, not a gap waiting to be filled. Of all the standard jazzy interfaces exactly
  one uses a `wstring` (`example_interfaces/msg/WString`, a demo type), and the DDS implementations
  do not encode one the same way - FastDDS and CycloneDDS's legacy CDR spend four bytes per
  character for interoperability with each other, CycloneDDS's XCDR2 two. ROS 2 messages carry text
  as UTF-8 in a plain `string`, which TickLE does support; use that.
- **Unbounded arrays get a fixed capacity.** TickLE stores a sequence in a fixed buffer, so every
  unbounded array has a default capacity, chosen per message: a `LaserScan` holds 4096 beams, an
  `Image` 64000 bytes of pixels, a `JointState` 24 joints. Your own values take precedence: put a
  `<package>.capacities` file (rows like `sensor_msgs/msg/LaserScan ranges 8192`) in a directory on
  `TICKLE_CAPACITIES_PATH` when running the build script. Capacities are compiled in, so changing
  them means rebuilding that package. Publishing a sequence longer than its capacity fails with an
  error that names the type.
- **Messages up to 64 KB.** `rmw_tickle` builds with a 65507-byte maximum datagram (the UDP limit)
  and lets the OS fragment larger datagrams at the IP layer. One lost fragment loses the whole
  message, and a RELIABLE writer then resends all of it. To lower the maximum, build `rmw_tickle`'s
  packages with `--cmake-args -DTICKLE_MAX_BUFFER_LENGTH=<N>` (a CMake cache variable, so it stays
  until you set it again or build afresh), and rebuild the interface packages, because `rmw_tickle`
  refuses a type generated for a different value. Discovery and other control traffic
  always stays within 1472 bytes, so TickLE nodes built with the core default still see an
  `rmw_tickle` node.
- **Your allocator is used.** `rmw_tickle` allocates its own storage with the allocator your
  application passed in its init options (`rclcpp::InitOptions`), threaded through to every
  publisher and subscription. An application that supplies a static-pool allocator therefore gets
  TickLE's storage out of that pool, with no `malloc()` on any TickLE path. What `rmw_tickle` still
  chooses is how much, from the QoS depth and the message type, because ROS 2's QoS has no
  resource-limits field to carry it.
- **Socket buffers.** `rmw_tickle` asks for 4 MiB receive buffers, which hold enough full-size
  datagrams. A stock kernel caps the request at `net.core.rmem_max` (about 208 KB), and
  `rmw_tickle` logs a warning naming that sysctl when the buffer it gets holds fewer than 16
  full-size datagrams (about 1 MiB). Raise the limit if you send large messages.

Senders (`ping`, `client`, `publisher`, `perf_client`) additionally take:

- `-c` stop after this many sends (default `0` = unlimited - `ping` also accepts `-d` instead/as
  well, see below)
- `-i` seconds between sends (default `1`, except `perf_client` - see below)

Receivers (`pong`, `server`, `subscriber`, `perf_server`) - and `ping`, which is a sender but (see
below) can be duration-bound too - additionally take:

- `-d` exit automatically after this many seconds (default `0` = run until `-c`/Ctrl+C)

`ping` and `perf_server` - the two pairs that report real statistics (latency, throughput; see
"Result output" below) - additionally take:

- `-w` exclude this many initial seconds from the statistics (default `0`)
- `-W` on the normal stop trigger (`-c`/`-d` reached, or Ctrl+C), don't exit immediately - keep
  running this many more seconds first, excluded from the statistics, then actually exit
  (default `0` = stop immediately, matching every other example)

Startup/shutdown transients (first-packet allocation overhead, ARP/socket warm-up, a run cut off
mid-burst) skew a latency or throughput number more than they'd ever show up as a functional
failure, so `-w`/`-W` trim them from the two pairs where that actually matters. `-W` deliberately
*extends* the run rather than reserving the last few seconds out of a known `-c`/`-d` - that's what
makes it work the same way whether the run was bounded or stopped with Ctrl+C, since neither needs
to know in advance when the run will end. `-d` itself always means exactly the counted-data span -
`-d 60 -w 5` measures a real 60 seconds, not `-w` eating into a fixed 60-second window - so `ping`
(warm-up and the stop trigger both live in the one process that's also doing the sending) just
keeps pinging through `-w`+`-d`+`-W` = 70 real seconds for `-d 60 -w 5 -W 5`. `perf` splits sender
and counter across two processes, so `perf_server -d 60 -w 5 -W 5` only ever sees real traffic
throughout that same 70-second span if `perf_client` is *also* told to send for the full 70
seconds, not just the 60 that end up counted - `make test-linux` passes `perf_client` `-d 70` and
`perf_server -d 60 -w 5 -W 5` for exactly this reason (see
[platform/linux/test.sh](platform/linux/test.sh)'s own comment). `perf_client` itself never takes
`-w`/`-W` - it isn't the authoritative side (`perf_server` is the one that can see loss), so
there's nothing on its side to exclude from a statistic it doesn't compute.

`-c`/`-d` exist mainly so a script (e.g. CI) can run a binary without it hanging forever
waiting on a peer that never shows up.

### Result output

Every example is also a functional or performance test of the library itself (see "Tests"
above), so the side of each pair that can actually tell whether the round trip worked prints a
final `RESULT:` line when it stops (`-c`/`-d` elapsing, or Ctrl+C):

- **Functional pairs** (`client` for set_bool, `subscriber` for uint64) print `RESULT: PASS` or
  `RESULT: FAIL (...)` - did every call get answered / did every published message arrive, in
  order, with none dropped.
- **Performance pairs** (`ping` for ping_pong, `perf_server` for perf) print the measurement
  itself (`RESULT: rtt_avg_ms=... loss_pct=...` / `RESULT: recv=... avg_mbps=... avg_latency_ms=...`),
  not a pass/fail verdict - there's no single right answer to compare against, only numbers to
  judge by eye or trend over time. `avg_latency_ms` is a one-way delivery latency (the receiver's
  clock minus the sender's own wire timestamp), so it's only as accurate as the two sides' clock
  sync (NTP) - treat it as a same-rig relative comparison (e.g. BEST_EFFORT vs RELIABLE, or across
  loss levels - see "Continuous performance testing" below) rather than an absolute number unless
  the two machines are known to be tightly synced. The other side of each pair (`server`, `pong`,
  `publisher`, `perf_client`) has no verdict of its own to report - it just logs a plain
  completion count.

`perf_client` takes four more flags to control what it sends:

```sh
$ ./perf_client [-s message_size_bytes] [-i interval_seconds] [-B] [-R]
```

- `-s` payload bytes per message (default/max: see "Message size: filling an Ethernet frame" below)
- `-i` seconds to wait between sends (default: see below; `0` sends as fast as `tt_Node_poll()` allows instead of on a fixed schedule)
- `-B` batch sends instead of flushing each one immediately (default: flush immediately, same as a
  real `tt_Publisher` - `pub->batch` in `include/tickle/tickle.h`). Worth passing for an uncapped
  (`-i 0`), small (`-s`) flood specifically: measured on real hardware, that combination without
  `-B` doesn't just send slower once discovery switches the Publisher to per-message unicast, it
  can lose nearly everything (see DESIGN.md's "RPC and Publish flush immediately by default;
  batching is opt-in" and "Discovery-learned peers: unicast to a few, broadcast to the rest").
- `-R` RELIABLE instead of BEST_EFFORT delivery (QoS roadmap #5, `rmw_tickle/PLAN.md`) - retains
  published samples in a `struct tt_ReliableCache` for retransmission when the matching
  `perf_server` (also needs its own `-R`) ACKNACKs a gap.

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

Each `BulkData` message costs a fixed amount of TickLE framing on top of its own payload before
it reaches that UDP payload: `tt_Header` (4B, once per packet) + `SubmessageHeader` (4B) +
`DataHeader` (16B) + `BulkData`'s own `seq` (4B) + its `payload` array's own `uint16` length
prefix (2B) = **30 bytes**, assuming one message per packet. So the largest payload that still
fits one frame unfragmented is:

```
BULKDATA__PAYLOAD_CAPACITY = tt_MAX_BUFFER_LENGTH - 30 = 1472 - 30 = 1442 bytes
```

(`examples/perf/Bulk.msg`'s `payload` field has no explicit bound - `tools/typesupport` derives
this capacity for it automatically from `tt_MAX_BUFFER_LENGTH` and `BulkData`'s other fields,
emitting it as `examples/perf/Bulk.h`'s own `#define BULKDATA__PAYLOAD_CAPACITY`, so the two
can't drift apart. See "Interface serialization (TickLE CDR-4)" in DESIGN.md and
`tools/typesupport/PLAN.md`'s "Capacity" rule for how.)

`perf_client` defaults `-s` to exactly `BULKDATA__PAYLOAD_CAPACITY` (1442) - the biggest packet
this protocol can put on the wire without fragmenting - so every send makes the most of one
frame. `-i` defaults to `0`, meaning no fixed schedule at all: publish as fast as
`tt_Node_poll()` allows, which is the right default for a throughput benchmark. On real
10Base-T1S hardware the 10 Mbit/s link itself becomes the bottleneck well before max-size,
unpaced sending would.

Pass `-i` to target a specific rate instead of maxing out. To hit exactly the line rate at
the default message size, for example:

```
interval_seconds = (message_size + 30) * 8 / line_rate_bps
                  = (1442 + 30) * 8 / 10,000,000 ≈ 0.0011776 sec
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

**<https://tsnlab.github.io/tickle/dev/bench/>** carries a single per-platform status table -
did it compile, did each test tier pass, and (for the hardware-in-the-loop row) the latest
measured throughput / latency / small-message rate / RELIABLE throughput under 1%/5%/10% packet
loss - refreshed whenever the hardware workflow is dispatched:

| Platform | Build | Unit tests | Integration test | Throughput | Latency RTT | Small-msg rate | Reliable Tput@1%/5%/10% loss |
|---|---|---|---|---|---|---|---|
| Linux x86-64 | ✅ | ✅ | ✅ (HAL over network namespaces) | – | – | – | – |
| FreeRTOS RISC-V (QEMU) | ✅ | – | ✅ (HAL over emulated virtio-net) | – | – | – | – |
| Raspberry Pi (HIL, arm64) | ✅ | – | ✅ (ping/pong over real Ethernet) | measured | measured | measured | measured (needs `sudo tc` on rpi#1 - see `.github/scripts/README.md`) |

The Linux / FreeRTOS rows come from [`test-all.yml`](.github/workflows/test-all.yml) (each tier
is its own step now, so the table can pinpoint which broke); the Raspberry Pi row and all the
numbers come from [`performance.yml`](.github/workflows/performance.yml). Both feed
[`.github/scripts/publish_dashboard.sh`](.github/scripts/publish_dashboard.sh), which folds its
section into `dev/bench/status.json` and re-renders the table above the benchmark charts.

The hardware-in-the-loop latency and throughput test runs on two real Raspberry Pi boards
connected by an Ethernet link (`rpi#1` as client/sender, `rpi#2` as server/receiver), via a
self-hosted GitHub Actions runner. **It runs on manual dispatch only.** Until 2026-09-24 it ran on
every push to `main`, but in practice most runs were cancelled by the next push while holding the
rig, and the published performance figures (`rmw_tickle/COMPARISON.md`) are taken by hand anyway.
A push now runs only the hosted `Check all` and `Test all` workflows.

- [`.github/workflows/performance.yml`](.github/workflows/performance.yml) - `workflow_dispatch`
  only
- [`.github/scripts/run_perf.sh`](.github/scripts/run_perf.sh) - checks out the exact commit
  being tested on both Pis, builds, runs `ping`/`pong` for latency and
  `perf_client`/`perf_server` for throughput (using the `-c`/`-d` flags above so a run can never
  hang waiting on a peer), and writes the results to the job summary

Results are tracked over time and charted at the same page (below the status table);
the workflow fails if latency more than doubles, or throughput drops to less than half, versus
the last recorded run (`alert-threshold: "200%"` on each `github-action-benchmark` step).

**RELIABLE vs BEST_EFFORT under real packet loss** (QoS roadmap #5, `rmw_tickle/PLAN.md`):
alongside the clean-link runs above, `run_perf.sh` also runs `perf_client`/`perf_server` at
1%/5%/10% loss injected with Linux's own `tc`/`netem` on rpi#1's egress toward rpi#2 - once for
each of BEST_EFFORT and RELIABLE at every level - and reports throughput plus `perf_server`'s new
one-way delivery latency (`avg_latency_ms`) for each combination, both in the job summary's own
comparison table and as further `github-action-benchmark` history. Paced at
`LOSS_TEST_INTERVAL_SEC` (20ms by default, not the clean-link runs' own firehose "as fast as
`poll()` allows") specifically so a NACKed sample is still likely to be in the reliable
Publisher's own retained-sample cache (`tt_MAX_RELIABLE_HISTORY`, 64 by default) by the time a
retry actually asks for it - at firehose rates that cache gets overwritten many times over before
one ACKNACK round trip can complete, so RELIABLE's own retransmission never gets a real chance to
recover anything (confirmed the hard way: an earlier version of this scenario ran at firehose
rate and reported RELIABLE's own loss *higher* than BEST_EFFORT's, not lower). This is where
RELIABLE's own retransmission is actually expected to cost something to measure - the clean-link
"reliable" run above has nothing to retransmit. Needs passwordless `sudo tc` on rpi#1
(`.github/scripts/README.md`'s own setup note); skipped (not a failure) on a runner where that
isn't configured - the status table's own three loss-level columns just stay "·" (pending) until
it is.

## Quality declarations
- [`rmw_tickle`](rmw_tickle/rmw_tickle/QUALITY_DECLARATION.md) - Quality Level 4 ([REP-2004](https://www.ros.org/reps/rep-2004.html))

## License
GPLv3 or proprietary license on request. Every source file carries a
`SPDX-License-Identifier: GPL-3.0-or-later` header; contact TSN Lab, Inc. for a proprietary
license.
