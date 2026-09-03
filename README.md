# TickLE: Real-Time ROS2 communication middleware optimized for 10Base-T1S

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

`perf_client` takes two flags to control what it sends, on top of `make runperf_client`'s defaults:

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
loop can even check whether a send is due, independent of `-i` - in this environment it idles
at roughly 500 calls/sec (~2ms/call) rather than the library's 1ms `tt_NODE_TX_INTERVAL` would
suggest, so a requested `-i` much smaller than ~2ms won't be hit exactly (it'll just behave
like `-i 0`). `perf_client`'s interval reports and final summary always show the throughput it
actually achieved, not just what `-s`/`-i` imply; a `-i` well above ~2ms (e.g. `0.01`) is
paced accurately since it's comfortably larger than that ceiling.

## License
GPLv3 or proprietary license on request
