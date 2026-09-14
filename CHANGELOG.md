# Changelog

All notable changes to TickLE are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) for the library
(`TICKLE_VERSION_*` in `include/tickle/tickle.h`). The on-the-wire protocol has its own
number, `tt_VERSION`, which moves independently.

## [Unreleased]

### Added

- `tt_Node_interrupt()`: wakes a blocking `tt_Node_poll()` call from another thread, returning
  `tt_RET_INTERRUPTED` promptly instead of waiting out the rest of its timeout - the one
  exception to a `tt_Node`'s otherwise-single-threaded rule (DESIGN.md's "Concurrency"). Built for
  `rmw_tickle` (`rmw_tickle/PLAN.md`'s Milestone 0), where a dedicated thread drives
  `tt_Node_poll()` in a loop and every other call (e.g. `rmw_publish()`) needs a way to get that
  thread's attention sooner than its current timeout would otherwise allow, without adding any
  locking inside TickLE itself.
- Liveliness timeout: a remote node that stops sending its periodic UPDATE announce entirely
  (not just an unchanged one - see `tt_Node`'s new `update_last_seen[]`, tracked separately from
  the existing content-change `update_last_modified[]`/`update_seen[]`) is now presumed gone
  after `tt_LIVELINESS_MISS_THRESHOLD` (config.h, default 3) consecutive announce intervals pass
  with nothing heard - same cleanup as an explicit farewell UPDATE (peer-table entries dropped,
  first-contact handling re-armed for a later announce from the same node id). `rmw_tickle/PLAN.md`'s
  Milestone 0(b) - previously a node that crashed or was network-partitioned without sending
  `tt_Node_destroy()`'s own farewell UPDATE lingered in every peer table forever.

## [1.0.0] - 2026-09-14

First tagged release.

### Added

- Library version macros (`TICKLE_VERSION_MAJOR/MINOR/PATCH/STRING`, `TICKLE_VERSION`,
  `TICKLE_VERSION_MAKE`) and a `tt_version()` accessor.
- `make install` / `make uninstall` (static lib + headers + `tickle.pc`); `PREFIX` / `DESTDIR`
  overridable.
- `tt_RET_INVALID_ARGUMENT` and `tt_CALL_TIMEOUT`.
- Full cross-endian support: a receiver now byte-swaps every framing field (submessage length,
  endpoint ids, sequence numbers, timestamps, string-length prefixes) when the peer's
  `tt_Header` magic says it serialized in the opposite byte order. `tt_hash_id()` is now
  endianness-independent (byte-at-a-time FNV-1a).
- `tests/test_cross_endian.c`; `tests/fuzz_process_packet.c` + `make fuzz`; `make sanitize`
  (ASan + UBSan unit-test run).
- `tt_hash_id()` collision test and reverse-endian string-decode test.
- `tools/typesupport`: generates a message/service's `<Name>.c`/`<Name>.h` codec directly from
  its `.msg`/`.srv` (ROS 2's own interface grammar) instead of hand-writing it - see
  `tools/typesupport/README.md` and DESIGN.md's "Interface serialization (TickLE CDR-4)" for the
  wire format it implements. Every one of TickLE's own example interfaces
  (`examples/{uint64,set_bool,ping_pong,perf}/*.msg`/`*.srv`) is generated this way now; `make
  regen` re-runs it in place (see CONTRIBUTING.md's "Generated codecs"). A bounded string
  (`string<=N`, or a plain `string` with an explicit `# @capacity <N>` annotation) gets a real
  `char[N+1]` buffer and a capacity check on encode/decode, the same as a variable array's
  capacity - previously it silently generated the same alias-only `char*` a plain, unbounded
  `string` does (see DESIGN.md's "Capacity" rule).
- Discovery-learned unicast: a server's `CallResponse` unicasts straight back to its
  `CallRequest`'s own sender instead of broadcasting an answer the rest of the segment never
  asked for, and a `tt_Publisher`/`tt_Client` now does the same to each Subscriber/Server it
  learns of via the periodic UPDATE announce, once it knows `tt_UNICAST_PEER_THRESHOLD` (default
  2) or fewer of them - falls back to today's broadcast once there are more, or none yet known.
  New `struct tt_Peer` and `tt_UNICAST_PEER_THRESHOLD` / `tt_MAX_PEER_COUNT` (`config.h`); see
  DESIGN.md's "Discovery-learned peers: unicast to a few, broadcast to the rest".

### Changed

- `tt_NODE_UPDATE_INTERVAL` is 1s (was a 10s "temporary, for debugging" value).
- `tt_CallRequestHeader` is 8 bytes, not 7 (a `reserved` pad byte) - keeps a CALLREQUEST's CDR
  payload 4-byte aligned, matching CALLRESPONSE/DATA. Wire-incompatible with pre-1.0 builds; not
  a concern before the first tagged release.
- A fully unanswered RPC reports `return_code == tt_CALL_TIMEOUT` to the client callback instead
  of `0` (which a server can legitimately return). A `tt_SERVER_CALLBACK` must not return that
  value.
- Every public entry point validates its arguments (NULL pointers, missing required
  `tt_Service`/`tt_Topic` fields, message sizes outside `(0, tt_MAX_BUFFER_LENGTH]`) and returns
  `tt_RET_INVALID_ARGUMENT` instead of dereferencing them.
- The public headers compile clean under `-std=c11 -pedantic-errors` (`tt_Request` / `tt_Response`
  / `tt_Data` are no longer empty structs). C11 is required (anonymous union in `tt_Header`).

### Fixed

- `process_callresponse()` ignores a response that doesn't match the outstanding call
  (`client->cache == NULL`, or a `seq_no` mismatch) - previously a duplicate or late response
  invoked the client callback twice and corrupted the latency estimate.
- `tt_Node_destroy()` now actually transmits its final "leaving" UPDATE; the receive side
  (`process_update()`) drops a source's peer-table entries when its announce no longer lists
  them, so a departed node no longer lingers forever.
- An `ACKNACK` or unknown submessage type is skipped, not treated as fatal to the whole
  datagram - forward-compatible with newer protocol revisions.
- `tt_hash_id()` no longer does unaligned `uint32_t` reads of the name strings (a fault on
  strict-alignment targets).
- The library no longer allocates: `process_update()`'s per-source `malloc()` is replaced by
  two fixed arrays on `tt_Node`. Nothing in `src/` calls `malloc`/`free` now.

### Known limitations

- Delivery is best-effort. `tt_Topic`'s `history_depth` / `deadline_duration` /
  `lifespan_duration` and the `tt_RELIABLE_*` knobs are reserved for a future reliable-QoS
  release and are ignored.
- One `tt_Node` is single-threaded; drive it from one thread.
