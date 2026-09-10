# Changelog

All notable changes to TickLE are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) for the library
(`TICKLE_VERSION_*` in `include/tickle/tickle.h`). The on-the-wire protocol has its own
number, `tt_VERSION`, which moves independently.

## [Unreleased]

Targeting the first tagged release, `v1.0.0`.

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

### Changed

- `tt_NODE_UPDATE_INTERVAL` is 1s (was a 10s "temporary, for debugging" value).
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
