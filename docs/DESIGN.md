# TickLE design

How TickLE works today, and why. Measured results are in [RESULTS.md](RESULTS.md), the ROS 2 layer in
[RMW.md](RMW.md), the test strategy in [TESTING.md](TESTING.md), and parked or future work in [ROADMAP.md](ROADMAP.md).
Struct and constant names refer to `include/tickle/tickle.h`, `config.h` and `hal*.h`; where this text and the code
disagree, the code is right.

## 1. Overview

TickLE is a small real-time publish/subscribe and RPC middleware in C, aimed at controlled links (10BASE-T1S, TSN,
a single trusted L2 segment) and at microcontrollers as well as Linux. `rmw_tickle` puts ROS 2 on top of it.

Principles that shape everything below:

- **The core never allocates.** `src/` has no `malloc`/`free`. Every table is sized at compile time or attached by
  the caller, who also frees it. This is what lets one core run on FreeRTOS with no heap and inside a ROS 2 process.
- **The core does not depend on ROS 2.** ROS-shaped concepts live in `rmw_tickle`.
- **Own wire protocol, not DDS.** UDP over IPv4, RTPS-like concepts (HEARTBEAT, ACKNACK, built-in discovery
  endpoint), but smaller framing and not wire-compatible with any DDS vendor.
- **Every drop is counted.** A silent drop looks exactly like network loss, so each refusal path has a counter.
- **Wire changes must leave every test better** (the user's rule, 2026-09-26): targeted metrics improve beyond
  2xSE and nothing regresses beyond 2xSE. Every wire change bumps `tt_VERSION`.

## 2. Context, nodes, endpoints and the poll loop

**A context owns the transport; nodes own endpoints.**

| object | owns | notes |
|---|---|---|
| `tt_Context` | sockets, scheduler, poll loop, discovery, liveliness, shared-memory segment | its id is the wire's `source` |
| `tt_Node` | a name, a namespace, an index in its context | up to `tt_MAX_NODES` per context |
| `tt_Publisher`, `tt_Subscriber`, `tt_Client`, `tt_Server` | per-endpoint state | each embeds `tt_Endpoint` and records its node's index |
| `tt_Topic`, `tt_Service` | type: sizes and encode/decode functions | shared by endpoints |

- Index 0 is the default node, created on first use of a `tt_Context_create_*()` shorthand and named
  `tickle_<context id>`. A context that creates only explicit nodes (every rmw_tickle context) never has one.
- `tt_Node_destroy()` refuses while an endpoint still lives on the node.

**The poll loop.** One thread at a time drives a context with `tt_Context_poll(ctx, timeout)`:

- `timeout = 0`: one non-blocking pass - run what is due, process what is already received.
- `timeout < 0`: wait until the next scheduler entry is due, a datagram arrives, or `tt_Context_interrupt()`.
  With nothing scheduled it waits indefinitely: an idle node does not wake up.
- Waiting uses `epoll_pwait2()` with the timeout as a timespec (never `SO_RCVTIMEO`), over the well-known socket,
  the data socket, an `eventfd` for wake-ups and, when present, the segment doorbell FIFO. The set is registered
  once, so a sleep no longer joins and leaves four wait queues; `ppoll()` over the same four is the fallback when
  epoll or Linux 5.11 is missing.
- At most `tt_SCHEDULER_IO_INTERLEAVE` (8) due scheduler entries run back to back before a non-blocking receive
  check, so a max-rate publisher cannot starve ACKNACK processing.
- A high-rate publisher must call `tt_Context_poll(ctx, 0)` between sends, not block.

**Threading.** With `tt_THREAD_SAFE=1` (default) every public `tt_*` call may come from any thread, concurrently with
the poll on another.

- One re-entrant state lock per context guards the context, its endpoints and its scheduler. Re-entry by the owner
  is a thread-id compare. User callbacks run inside it and may call back into core.
- Nothing is held while the poll waits; the lock is taken per datagram (or per `tt_RX_LOCK_CHUNK` already-buffered
  datagrams) and per due scheduler entry.
- Timers from other threads go through a lock-free inbox (`tt_SCHED_INBOX_LENGTH`) and wake the poll if earlier than
  what it waits for.
- A second concurrent poller gets `tt_RET_BUSY`. `tt_Context_lock()`/`_unlock()` give callers consistent compound reads.
- `tt_THREAD_SAFE=0` compiles the locks out for a single-task microcontroller.

**Receive path (Linux HAL).**

- `recvmmsg()` reads up to `tt_RX_BATCH` datagrams per call (32 at the 1472-byte datagram, 1 in builds with a larger
  datagram). The two sockets alternate first refusal so neither starves.
- "Now" is read once per `tt_RX_CLOCK_REFRESH` (16) datagrams rather than per datagram.
- **io_uring receive hint** (`tt_HAL_RX_HINT`): a busy loop asks `tt_rx_maybe_ready()` whether anything arrived. With
  io_uring the answer is a read of a completion queue in shared memory instead of an empty `recvmmsg()`. io_uring is
  used only for one-shot `POLLIN` on the two sockets; datagrams are still read normally, and no thread is added.

  | mode | value | behaviour |
  |---|---:|---|
  | `tt_RX_HINT_AUTO` | 0 (default) | io_uring if the kernel allows it; if refused (Docker/Kubernetes default seccomp, `kernel.io_uring_disabled`, Linux < 5.1), one warning and plain reads |
  | `tt_RX_HINT_READ` | 1 | never io_uring; none of its code compiled |
  | `tt_RX_HINT_URING` | 2 | io_uring or fail context creation with `tt_RET_UNSUPPORTED` |

  Measured on the rig (2026-10-04, same-host p3 BEST_EFFORT): 1.31x subscriber rate and 29% less publisher CPU per
  sample. README.md has the seccomp recipe.

## 3. Wire format (`tt_VERSION` 11)

**Transport.**

- Each context has two UDP sockets: the **well-known socket** (wildcard address, port `_tt_CONTEXT_PORT` 8282;
  rmw_tickle offsets it by the ROS domain id) receives broadcasts; the **data socket** (own address, ephemeral port)
  sends everything and receives unicast. A peer's unicast address is the source address of its datagrams.
- Broadcast goes to the configured link broadcast (`_tt_CONTEXT_BROADCAST`, default `255.255.255.255`). Set the
  directed broadcast of the intended link: the limited broadcast follows the default route.
- A context may have up to `tt_MAX_LINK_COUNT` (4) links, each with its own broadcast and unicast threshold.
- A datagram is at most `tt_MAX_BUFFER_LENGTH` bytes of UDP payload (1472 by default, no IP fragmentation).

**Datagram layout.** Either a `tt_Header` followed by submessages (type-length-value, each padded to 4 bytes), or,
when the datagram carries exactly one submessage addressed to everyone, a single 4-byte `tt_SingleHeader`:

```
classic: tt_Header        magic[2] "KT"/"TK" | version | source
         tt_SubmessageHeader  type | receiver | length (bytes, header included)
         body ...         (repeat, 4-byte aligned)
single:  tt_SingleHeader  marker 'k'/'t' | version | source | type
         body ...         (to the end of the datagram)
```

- Senders write native byte order; the magic/marker tells the receiver whether to swap. Receivers swap every field
  core interprets; application payload decoders get an `is_native_endian` flag.
- A datagram of another `tt_VERSION` is dropped and logged once per source. There is no partial compatibility.
- `source` is the sending context's id (1-254), unique on the link (section 12).
- `receiver` is a context id or `tt_SUBMESSAGE_ID_ALL` (0xff).
- Submessage padding bytes are zeroed (stale buffer bytes once leaked into padding).

**Submessages.**

| type | name | body header | purpose |
|---:|---|---|---|
| 1, 7 | *retired* (UPDATE, UPDATE_PART) | - | never reused |
| 2 | `DATA` | `tt_DataHeader`: endpoint_id, seq_no, timestamp (u32 us), entity_id - 16 B | one sample in one datagram |
| 3 | `ACKNACK` | endpoint_id, entity_id, sender_entity_id, seq_no, bitmap_words, bitmap[] - 20 + 8/word B | reader's cumulative ack + missing bitmap |
| 4 | `CALLREQUEST` | endpoint_id, seq_no (u16), retry, reserved - 8 B | service request |
| 5 | `CALLRESPONSE` | endpoint_id, seq_no, retry, return_code - 8 B | service response |
| 6 | `HEARTBEAT` | endpoint_id, first_available_seq_no, last_seq_no, entity_id, flags - 20 B | writer's range; `tt_HEARTBEAT_FLAG_LIVELINESS` = manual liveliness assertion |
| 8 | `FRAG_FIRST` | whole `tt_DataHeader` + frag_count - 17 B | first datagram of a fragmented sample |
| 9 | `FRAG_CONT` | entity_id, seq_no, frag_index, frag_count - 10 B | later fragments |
| 10 | `SHM_DATA` | - | reserved; nothing produces it; refused if it arrives on a socket |

**Identifiers.**

- `endpoint_id`: 32-bit FNV-1a hash of topic/service name and endpoint name, computed locally on each node.
- `entity_id`: one endpoint instance within its context; (source, entity_id) names a writer network-wide.
- `seq_no`: per writer and **per datagram** (section 8).
- `timestamp`: low 32 bits of the sender's clock in microseconds; the receiver rebuilds the rest assuming the clocks
  agree within +-35.8 minutes.

**Framing cost per sample** in one datagram: 4 B header + 16 B `DATA` header = 20 B (rmw_tickle adds a 4-byte
publication sequence number: 24 B), against 42 B of Ethernet/IPv4/UDP.

**Payload encoding (TickLE CDR-4).** Not OMG CDR. Host-native byte order; each primitive aligned to
`min(size, 4)` relative to the payload start; strings are `u16 length` (including the NUL) + bytes + pad to 4;
variable arrays are `u16 count` + elements; nested messages are inlined with no header. Alignment is capped at 4 so
batched submessages, padded to 4, keep every payload aligned. Generated structs use `#pragma pack(push, 4)`, so an
all-fixed-size message can be encoded and decoded in place. Decoded strings alias the receive buffer and are valid
only during the callback.

**Versions that changed the wire:** 5 ACKNACK bitmap widened; 6 per-subscriber ack identity; 7 discovery became a
DATA of a built-in endpoint; 8 periodic summary + pulled list; 9 liveliness flag; 10 32-bit timestamp, one-word rmw
psn, single header; 11 nodes in the announce.

## 4. Sending: flush, batching and unicast peers

- **RPC always flushes immediately**: a caller is waiting (6.7x lower RTT than waiting for the 1 ms tick).
- **Publish flushes immediately by default.** `pub->batch = true` defers to the `tt_CONTEXT_TX_INTERVAL` (1 ms) tick
  and coalesces several samples into one datagram. Use it for bursts of small samples to one destination.
- A shared datagram (a batch, an announce) never exceeds `tt_CONTROL_MAX_LENGTH` (1472), so a node on core defaults
  can always receive it even when the sender's `tt_MAX_BUFFER_LENGTH` is larger.
- **Unicast to a few, broadcast to the rest.** Each Publisher and Client keeps a fixed `peers[]` table
  (`tt_MAX_PEER_COUNT` 8) of remote contexts hosting a matching endpoint, learned from discovery. 0 known peers or
  more than the link's threshold (`tt_UNICAST_PEER_THRESHOLD` 2) means broadcast; otherwise one unicast per peer.
  A server answers unicast to the request's source.
- Several datagrams ready at once (a sample's fragments, one datagram to several peers) go in one
  `tt_send_batch()` = one `sendmmsg()`.

## 5. Discovery

**Discovery is ordinary traffic on a built-in endpoint**: endpoint_id `tt_DISCOVERY_ENDPOINT_ID` (0), entity_id
`tt_DISCOVERY_ENTITY_ID` (all ones). Steady-state cost is a ~28-byte summary per node per interval, whatever the
endpoint count.

| role | submessage | addressed | contents |
|---|---|---|---|
| summary, every `tt_CONTEXT_UPDATE_INTERVAL` (1 s), or a sixth of the shortest own lease | HEARTBEAT | broadcast | first = last = the context's **generation** (low 32 bits of `last_modified`) |
| request for the list | ACKNACK | unicast to the summary's sender | the generation wanted |
| the list (announce) | DATA, or FRAG_FIRST + FRAG_CONT | unicast to the requester | `tt_AnnounceHeader` + `tt_UpdateEntity` records |
| a change (endpoint created/destroyed) | the list | broadcast, at once | the new generation |

- The generation changes exactly when the endpoint list does and differs across restarts.
- A summary with an already-applied generation is liveliness only. Any other generation draws one request, retried
  after the round trip measured to that peer plus `tt_CONTEXT_TX_INTERVAL` (`tt_DISCOVERY_REQUEST_RETRY`, 10 ms, until
  one is measured) up to `tt_DISCOVERY_REQUEST_ATTEMPTS` (4) times, tracked in
  `tt_DISCOVERY_PENDING_REQUESTS` (8) slots.
- Up to `tt_UNICAST_PEER_THRESHOLD` requests per tick are answered unicast; more are answered by one broadcast, so a
  burst of joining nodes costs one broadcast.
- A changed list heard by broadcast, or an unknown node's, is answered once with our own list, unicast.
- **A large list is fragmented at entity boundaries.** Every fragment carries its own `tt_AnnounceHeader` and whole
  entities and is applied as it arrives: no reassembly memory. A new generation replaces the source's old list on its
  first fragment; a lost fragment is filled by the next request. At most `tt_UPDATE_MAX_PARTS` fragments (32; about
  13 ROS-sized endpoints per fragment). A context whose list needs more sends no announce at all and is invisible.
- **Nodes are announced too** (v11): one `tt_UpdateEntity` per explicit node with `kind = tt_KIND_NODE`, the
  namespace as type and the node name as name. Every entry carries its node's index in 8 spare bits of `kind` and
  `qos`; receivers mask them off (`tt_UPDATE_KIND_MASK`, `tt_UPDATE_QOS_MASK`). Cost: about 40 B per node per list.
- **The discovery table** (`tt_MAX_DISCOVERED_ENTITIES`, 552 B per entry) holds every remote entity. It decides more
  than introspection: RxO checks, KEEP_ALL writer classification, per-entity leases and rmw graph queries read it,
  and for an entity it has no room for RxO **fails open**. Size it to the remote entities a context will see. Drops
  are counted (`entities_dropped`) and warned once. Above 64 entries it is indexed by open addressing
  (`tt_DISCOVERY_INDEXED`).
- **Discovery range** (`tt_DISCOVERY_OPTIONS`, rmw builds): `SUBNET`, `LOCALHOST` or `OFF`, and links marked `peer`
  for static unicast peers (ROS's `ROS_AUTOMATIC_DISCOVERY_RANGE` / `ROS_STATIC_PEERS`).

## 6. Reliability

**RELIABLE is RTPS-shaped: the writer retains, the reader NACKs gaps, the writer resends from its cache.**

**Writer side.**

- A Publisher with `reliable_cache` (caller-owned `index[]` + byte arena) retains each sent datagram as a record.
  `reliable` enables RELIABLE traffic; `durable` (TRANSIENT_LOCAL) uses the same cache. One cache backs both.
- `capacity`/`arena_size` are the Publisher's RESOURCE_LIMITS, chosen by the caller. `sample_depth` makes KEEP_LAST
  count whole samples. `tt_RELIABLE_CACHE_ARENA_BYTES(depth, max_record)` sizes the arena with one record of slack.
- An ACKNACK naming an evicted seq_no is answered with a HEARTBEAT carrying the real `first_available_seq_no`, so the
  reader skips exactly what is gone.
- Ack state is tracked per remote Subscriber entity (`tt_MAX_ACK_ENTRIES` 16), not per node.

**Reader side.**

- Each reliable Subscriber keeps a writer proxy per writer with a received bitmap. The default window is
  `tt_RELIABLE_BITMAP_BITS` (256 datagrams); a caller can attach a wider one up to `tt_RELIABLE_BITMAP_MAX_BITS`
  (4096). Out-of-order datagrams wait in a caller-owned reorder buffer; delivery is in order.
- A gap is NACKed at once; unanswered requests are repeated by a per-proxy retry timer.
- **Retry interval** (`tt_RELIABLE_RETRY_INTERVAL` 0 = dynamic, default): RFC 6298 form,
  `srtt + max(G, 4 x rttvar)` over request-to-recovery times, starting at `tt_RELIABLE_RETRY_INITIAL` (1 ms),
  `G = tt_RELIABLE_RETRY_GRANULARITY` (100 us, host timer lateness), capped at
  `tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE` (64) x srtt. Any non-zero value is used as given.
- KEEP_LAST gives up a gap after `tt_RELIABLE_RETRY` (3) retries; KEEP_ALL never gives up and logs a stuck gap every
  `tt_RELIABLE_STUCK_WARN_INTERVAL` (5 s).
- Early re-requests produce some duplicate repairs (about 0.2 per loss on veth). Suppressing them was measured to cut
  throughput 15-35% and was dropped: the duplicates hedge against lost repairs. Revisit on bandwidth-bound links.

**KEEP_ALL flow control.**

- `pub->keep_all`: never evict an unacknowledged datagram; refuse the write with `tt_RET_WOULD_BLOCK` instead (nothing
  sent, nothing cached). `writable_callback` or `tt_Publisher_writable()` says when to retry.
- The bound is `min(cache depth, narrowest tracking window any matched Subscriber announced)`.

**ACK solicitation.** A reader ACKNACKs only on a gap, so on a lossless link nothing would advance the ack and a
KEEP_ALL writer would stop at its bound. So the writer solicits acks itself (a HEARTBEAT without the FINAL flag):

- **Spaced by half a window:** a KEEP_ALL writer solicits when unacked reaches half its bound, and again only after
  another half-bound of new seq_nos - at most two per window, whatever the round trip. Non-KEEP_ALL writers can opt
  in with `ack_solicit_watermark_pct`.
- **Clocked by ACKNACK:** one solicitation in flight; the next may go as soon as an ACKNACK answers it. The retry
  interval is only how long an unanswered one waits before it is presumed lost. (A fixed 1 ms gap had capped
  same-host RELIABLE at one window per millisecond.)
- **Refused-publisher resolicit timer:** a refused write also solicits, and while the Publisher stays refused it arms
  `keep_all_resolicit()` one retry interval after the last solicitation, repeating until writable. Without it a lost
  request or answer left a blocked publisher waiting with nothing to advance it (-87% throughput at p4, 5% loss).
- `ack_solicit_period_ns` adds an optional periodic solicitation.

## 7. Services (RPC)

- A Client has one outstanding call, cached for retry in `cache_buf` (or caller-attached storage).
- Retries: `call_retry_interval` if set, every time. Otherwise (auto) RFC 6298 over call-to-answer times, with bounds
  relative to srtt since 2026-10-05: the first wait is `srtt + max(tt_CALL_RETRY_GRANULARITY, 4 x rttvar)`, each
  retry of the call waits twice the one before, each at most `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` (64) x srtt. The seed
  `tt_CALL_RETRY_INTERVAL` (5 ms) stands in for srtt until a first answer. A call gives up after
  `(2^(count+1) - 1)` first waits (15), and never later than `(count + 1) x tt_CALL_DEADLINE_PER_SEND` (1 s, the old
  worst case): a wait past that is cut there. A timeout doubles srtt, up to that deadline; the next answer replaces
  the estimate.
- A Server caches each answered response in one of `tt_MAX_SERVER_CACHE_COUNT` (64) slots, so a retried request gets
  the same answer without re-running the callback. It keeps it for the longer of the client's seed schedule (or the
  service's explicit `call_retry_interval x (count + 1)`) and `tt_SERVER_CACHE_GAP_MULTIPLE` (4) x the longest
  recent gap between a response and the same client's retry; each retry served re-arms it, and a retry after expiry
  teaches the server that client's gap. A response is dropped when its client turns out to be a new incarnation
  (discovery lists it under another entity_id, or a farewell). A full cache
  evicts the response sent longest ago.
- **Deferred responses:** a callback may return `tt_CALL_DEFERRED` and answer later from any thread with
  `tt_Server_send_response()`, which encodes and sends before it returns (the caller's data need only live for the
  call). An unanswered slot is reclaimed after `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` (5 s); a retry of a pending
  request does not re-invoke the callback.
- Requests and responses never fragment: they are bounded by `tt_MAX_BUFFER_LENGTH`.

## 8. Fragmentation and large samples

**A sample that does not fit one control datagram is split by TickLE, never by IP.** OS IP fragmentation failed
97.4% of reassemblies at 5% loss; TickLE fragments are repaired one datagram at a time.

**Sizes.**

| constant | meaning | core default | rmw build |
|---|---|---:|---:|
| `tt_MAX_BUFFER_LENGTH` | largest datagram (service messages may use it whole) | 1472 | 65507 |
| `tt_CONTROL_MAX_LENGTH` | largest shared datagram and fragment size | 1472 | 1472 |
| `tt_MAX_SAMPLE_LENGTH` | largest sample CDR | = buffer | = buffer |
| `tt_FRAG_ENABLED` | compiled in when sample > control | 0 | 1 |

At core defaults fragmentation compiles out entirely (no reassembly pool, no larger `tx_buffer`), and an oversized
sample is refused. `-Dtt_FRAG_ENABLED=0` forces IP fragmentation (a benchmark arm only).

**Wire.** Fragments are alone in their datagram and unpadded. `FRAG_FIRST` carries the full `tt_DataHeader` and
`frag_count`; `FRAG_CONT` only `entity_id`, its own `seq_no`, `frag_index`, `frag_count` (10 B). Every fragment but the
last is full and fragment 0 carries `tt_FRAG_FIRST_SHORTFALL` (11) bytes less, so position follows from the index and no
offset field is needed. At most `tt_FRAG_MAX_COUNT` (64) fragments.

**Every datagram takes its own seq_no** (the user's decision, 2026-09-26). A k-fragment sample consumes k consecutive
seq_nos and is named by its first; a fragment's sample is `seq_no - frag_index`. Consequences:

- A lost fragment is NACKed and resent alone: cost `k / 0.95` datagrams at 5% loss instead of `k / 0.95^k`.
- The subscriber callback's seq_no is monotonic, not contiguous. rmw_tickle carries its own contiguous counter for
  ROS's publication sequence number.
- The reliable cache holds one record per datagram; KEEP_LAST evicts whole samples; KEEP_ALL admits a sample only if
  all its datagrams fit.

**Sender.** The sample is encoded once in `tx_buffer` and its fragments are sent from there in one `sendmmsg()`.
Anything batched ahead is flushed first. Retransmissions send cached fragments unpadded.

**Receiver.**

- RELIABLE: a fragment in order (its seq_no is the writer's watermark, the subscriber holds nothing) is put
  together in the node's `frag_scratch` and the sample delivered from there, never touching the reorder buffer - one
  sample at a time per node (`frag_fast_take()`). Any other fragment goes into the subscriber's reorder buffer (offset
  per writer), and anything that would disturb the sample in the scratch - a store into that subscriber's buffer,
  another use of the scratch, its watermark moving - first moves its fragments into their slots, which are free by
  construction. Either way a fragment is recorded as received only once kept. A sample that can never be whole is
  dropped and counted (`reorder_abandoned`); a torn sample is never delivered.
- BEST_EFFORT: a pool of `tt_FRAG_REASSEMBLY_SLOTS` (8) keyed by (source, entity_id, sample seq_no). When full, a
  completed slot is reused first, then the oldest reassembly is abandoned. Duplicates, abandons and contradictions
  are counted (`frag_duplicate`, `frag_abandoned`, `frag_dropped`).
- A node built without fragmentation skips types 8 and 9 silently.

**Larger than 65507 B** samples and receive-buffer lending are not implemented yet (see [ROADMAP.md](ROADMAP.md)).

## 9. QoS: liveliness, deadline, lifespan, durability

**RxO matching.** Each endpoint announces its QoS in its `tt_UpdateEntity` (`qos` bits for reliable, durable, manual
liveliness; deadline and lease durations). A Subscriber silently drops DATA from an incompatible writer and counts it
(`rxo_drops`).

**Liveliness: a lease runs from the last sign of life** (DDS semantics, `tt_VERSION` 9).

- AUTOMATIC: any datagram from the entity's context refreshes it.
- MANUAL_BY_TOPIC: only that writer's own DATA or a HEARTBEAT with `tt_HEARTBEAT_FLAG_LIVELINESS`
  (`tt_Publisher_assert_liveliness()`).
- One scheduler entry per context, `check_liveliness()`, runs at the earliest expiry and re-arms; a refresh never
  touches the timer. A lapsed entity is tombstoned (discovery callback with `departed`) and revives on its next sign
  of life.
- A context is presumed dead after `tt_LIVELINESS_SILENCE_NS` (3.5 intervals: exactly three missed summaries), or
  longer if one of its entities announced a longer lease, up to `tt_CONTEXT_MAX_LEASE_NS` (10 s). Its peers, ack and
  writer-proxy state are dropped and it is re-learned from its next summary.
- An idle node's summary is its only sign of life, so it is sent every interval or every lease/
  `tt_LIVELINESS_LEASE_DIVISOR` (6), whichever is sooner. Under traffic, extra short-lease summaries are skipped
  when every known peer has heard from us since the last tick (`summaries_skipped`).
- The check defers (up to `tt_LIVELINESS_MAX_DEFERRALS` x `tt_LIVELINESS_DEFER_NS`, 4 ms) while datagrams are still
  unread, so a descheduled process does not declare live peers dead.

**Deadline.** `deadline_duration_ns` is announced and used for RxO; core does not enforce it (rmw_tickle does).

**Lifespan.** `lifespan_duration_ns` on a Publisher: a cached sample older than this is neither retransmitted nor
replayed to a late joiner. No wire change; readers can enforce their own expiry from the DATA timestamp.

**Durability (TRANSIENT_LOCAL).** A `durable` Publisher unicasts its retained cache, oldest first, to a newly
discovered Subscriber. Delivery is recorded per (context id, announce generation) so a restarted peer gets the
backlog again and a continuing one does not. A durable Subscriber refuses a VOLATILE writer.

**Local delivery** (`tt_LOCAL_DELIVERY`, rmw builds): a publish is also handed in-process to the context's own
matching Subscribers with the same QoS rules; otherwise a context drops its own DATA as self-sent.

## 10. Shared-memory transport

**Same-host peers exchange datagrams through a shared-memory ring instead of loopback UDP.** It is a second carrier,
not a second protocol: the record in a slot is the datagram that would have gone on the wire, and it goes through the
same acceptance path (version, RxO, context-id rules). Core decides the transport per peer; rmw only reports it.
Compiled in by default on Linux (`tt_SEGMENT_ENABLED`), out on FreeRTOS.

**The segment.**

- **One segment per receiving context**, owned and read by it, written by every same-host peer that sends to it
  (many writers, one reader). Named `/dev/shm/tickle-seg-<ip>-<port>-<context id>` from what discovery already
  knows; the address separates network namespaces, which share `/dev/shm`.
- **The header is the authority, not the name.** It carries magic, `tt_SEGMENT_VERSION` (4), the owner's
  (ip, port, id), a per-launch `incarnation`, and the ring geometry (`slots`, `slot_bytes`). A writer maps the header,
  validates it, then maps the whole region with the owner's geometry. Any mismatch falls back to UDP, counted by reason.
- **Fixed-size slots**, a power of two, indexed with a mask. Each slot has a 16-byte header (`length`, sender address
  and port, `seq_span`, `sequence`) and `slot_bytes` of payload. A writer claims an index by compare-and-exchange,
  fills the slot and publishes `index + 1` with release; the reader takes a slot only when its sequence says it is
  finished, and frees it with `index + slots`. A record never straddles the wrap.
- **Geometry.** `tt_SEGMENT_BYTES` (768 KiB) / (16 + `tt_SEGMENT_SLOT_BYTES`), rounded down to a power of two:
  512 slots of 1472 B by default. Size it as `slots >= publish rate x worst reader stall`. The runtime
  `_tt_CONFIG.segment_slot_bytes` may set the slot size (0 = default, ceiling one control datagram); a type that does
  not fit an explicitly set slot fails `tt_Context_create_publisher()`, while on the default it goes over UDP and is
  counted (`segment_oversized_to_udp`).
- **`seq_span`** says how many seq_nos a record covers, so a record carried whole where the network would fragment
  keeps the writer's seq space identical on both paths. It lives in the slot header and therefore cannot arrive from
  the network. Whole-record sends exist but are only reachable with a slot larger than a control datagram, a build-time
  option: a 4096-B slot measured the same rate as 1472 B (2713 vs 2732 Mbps), so the runtime ceiling stays.

**Lifecycle (lazy).**

- A context builds its segment when the first same-host peer appears in discovery (or when it delivers to itself),
  and releases it when the liveliness timeout removes the last one. A host with no same-host peers pays nothing.
- `tt_segment_create()` unlinks any stale file first, so a dead owner's records are never inherited; teardown unlinks.
- A writer re-asks a name every `tt_SEGMENT_ATTACH_RETRY_SENDS` (256) sends when absent and every
  `tt_SEGMENT_REVALIDATE_SENDS` (4096) when attached, so a peer that binds late becomes reachable and a replaced
  owner is noticed. (A time-based version, `c73a22e7`, was reverted on 2026-10-06: ROADMAP 5a.)
- A reader that takes nothing for `tt_SEGMENT_DEAD_READER_NS` (one summary interval of `tt_LIVELINESS_SILENCE_NS`, 1 s;
  a static_assert keeps it below that silence) while records wait is given up; the writer uses UDP
  until the next recheck.
- A writer that dies between claim and publish wedges the head; the owner warns after `tt_SEGMENT_STALL_PASSES`.

**One path per peer, and drop on full.**

- Once attached, a peer's datagrams go through its segment. A full ring **drops** the datagram
  (`segment_full_dropped`) rather than rerouting it to UDP: a rerouted datagram overtakes the records queued in the
  ring and the reader discards everything older (once 97.6% of same-host delivery).
- So, under load, the segment's failure mode is sample loss where UDP's is a slower sender. A subscriber's loss
  figure cannot see these drops; `shm_full_dropped` can. A larger ring moves the threshold, it does not remove it.
- UDP still carries broadcasts, datagrams larger than a slot (service messages in rmw builds), and traffic before the
  attach succeeds. Every UDP send is counted by reason: `tx_udp = broadcast + oversize + unattached`.
- The reader drains at most `tt_SEGMENT_DRAIN_PER_POLL` records per poll so the sockets are not starved.

**Doorbell: a FIFO, rung once per reader sleep.**

- Shared memory cannot end a socket wait, so before blocking the owner writes a new, never-zero **sleep generation**
  into `reader_waiting` and drains once more; it writes 0 on waking.
- A writer reads `reader_waiting` after publishing a record, and each side puts a full fence between its store and
  its load, so no record can sit unseen. Without the writer's fence x86 let the load overtake the slot's release
  store and lost about 3.5 wake-ups per 100,000 round trips (`platform/linux/bell_wake_check.c`). If it is non-zero
  and differs from the last generation it rang for this peer, it rings once.
  A busy reader is never asleep, so a loaded ring costs no doorbells; a reader that goes back to sleep with records
  unread is rung again; a dead reader is rung once, not per datagram.
- The bell is a named FIFO beside the segment (`<segment>.bell`, `tt_SEGMENT_BELL_FIFO=1`), opened by writers on
  attach, rung with a one-byte non-blocking write, and part of the owner's wait set. A peer without one is rung with
  a zero-length UDP datagram (never a valid TickLE datagram).
- In the epoll set the bell is edge-triggered, so a ring costs the reader no `read()`. The pipe's bytes are read
  every capacity / (4 x `tt_MAX_CONTEXT_IDS`) sleep generations (64 at 64 KiB): each peer rings a generation at
  most once, so that keeps the pipe under half full, and a full pipe would refuse a ring. The kernel is asked at
  bell creation whether it reports every write to an unread pipe (Linux 5.14+); if not, the bell is
  level-triggered and read on every ring, as before.
- Measured on `e17b4e6f`: 1.42x p3 BEST_EFFORT throughput over the UDP doorbell, and the same-host p2 round trip now
  34% faster than the kernel path (0.030 vs 0.046 ms at 200/s). Details in [RESULTS.md](RESULTS.md).

**What the ring cannot do.** It cannot serve as the RELIABLE retention cache: the memory belongs to the receiver and
delivery frees the slot. Retention stays in the writer's own cache.

## 11. Memory model

- **Embedded storage** sized by macros: server response cache (`tt_SERVER_CACHE_ENTRY_LENGTH` x 64), client call
  cache, scheduler (`tt_MAX_SCHEDULER_LENGTH` 128), peer and ack tables, per-peer tables indexed by context id
  (`tt_MAX_CONTEXT_IDS` 256).
- **Caller-attached storage** where the size depends on the use: a Publisher's `reliable_cache` (index + arena), a
  Subscriber's tracking window and reorder buffer, `tt_Server_set_storage()` / `tt_Client_set_storage()`.
  The caller allocates and frees; core holds pointers only.
- rmw_tickle allocates with the application's `rcl` allocator, so a static-pool allocator keeps TickLE heap-free.

## 12. Context ids and same-host peers

**Every per-peer table is keyed by the 8-bit context id, so it must be unique on the link.**

- Default without claiming: the last octet of the context's address, or `_tt_CONFIG.context_id`.
- **`tt_CONTEXT_ID_CLAIM`** (default on Linux since 2026-09-29, off on FreeRTOS, which has no host registry):
  - a context claims its id in a host registry file under `/dev/shm`, keyed per network: the preferred id if no live
    process holds it, else a free one;
  - a datagram is our own only if it comes from our own data socket;
  - a datagram carrying our id from any other address is a collision, and the newer context moves to an id nobody on
    the link uses. An explicitly set id never moves.
- The wire is the same either way.
- Two contexts on one host are told apart by address and port, which is also what names their segments (section 10).

## 13. Security model

**What exists:** nothing cryptographic. TickLE has no authentication, access control or encryption; the wire is
plaintext. Treat it as safe only on a controlled link.

- rmw_tickle refuses to start when `ROS_SECURITY_ENFORCEMENT=Enforce` is requested, and logs once when security is
  enabled but permissive, so a user who asked for security never silently runs without it.
- `SHM_DATA` (type 10) arriving on a socket is refused, so a shared-memory-only record cannot be injected from the
  network.
- Hostile datagram lengths and ACKNACK word counts are bounds-checked before indexing.
- The HIL CI never runs on `pull_request`, and the rig accounts are unprivileged.

**What is parked** ([ROADMAP.md](ROADMAP.md); not to be started until the user says so): SROS2-file-compatible
security, with MACsec for link confidentiality and a core module, **TickLE Security**, for per-process
authentication, access control and optional per-topic protection. Agreed shape: compile-time seams (authentication
at peer association, authorization at endpoint creation and match, protection per datagram inside the acceptance
path, so the segment cannot bypass it); keystore parsing stays on the rmw side.

## 14. Wireless and zenoh-pico notes

- **Wireless is parked** ([ROADMAP.md](ROADMAP.md)). Nothing has been measured on a wireless link. TickLE has no
  congestion control or pacing, no NAT traversal and no encryption, so an internet path is not supported. Per-datagram
  repair and small framing should help on lossy links; batching (`tt_send_batch`) is the cheapest candidate win.
- **zenoh-pico** does not retransmit over UDP (its RELIABLE only orders). It is compared only on best-effort cells
  over multicast; its reliable p2-p4 cells are recorded as not measurable.

## 15. Configuration reference

All are compile-time `-D` overrides unless noted. Times in nanoseconds.

| knob | default | rmw build | meaning |
|---|---:|---:|---|
| `tt_MAX_BUFFER_LENGTH` | 1472 | 65507 | largest datagram |
| `tt_CONTROL_MAX_LENGTH` | 1472 | 1472 | largest shared datagram / fragment |
| `tt_MAX_SAMPLE_LENGTH` | = buffer | = buffer | largest sample |
| `tt_FRAG_ENABLED` | derived | 1 | fragmentation compiled in |
| `tt_FRAG_REASSEMBLY_SLOTS` | 8 | 8 | best-effort reassemblies in flight |
| `tt_THREAD_SAFE` | 1 | 1 | locks compiled in |
| `tt_CONTEXT_TX_INTERVAL` | 1 ms | | batch flush tick |
| `tt_CONTEXT_UPDATE_INTERVAL` | 1 s | | discovery summary interval |
| `tt_UNICAST_PEER_THRESHOLD` | 2 | | max peers reached by unicast (per link) |
| `tt_MAX_PEER_COUNT` | 8 | | peers table per Publisher/Client |
| `tt_MAX_ACK_ENTRIES` | 16 | | remote Subscribers tracked per Publisher |
| `tt_MAX_LINK_COUNT` | 4 | | links per context |
| `tt_MAX_ENDPOINT_COUNT` | 256 | 2048 | local endpoints per context |
| `tt_MAX_NODES` | 16 | 256 | nodes per context |
| `tt_MAX_DISCOVERED_ENTITIES` | 16 | 2048 | remote entity table (affects RxO, leases) |
| `tt_UPDATE_MAX_PARTS` | 32 | 255 | fragments of one announce |
| `tt_DISCOVERY_REQUEST_RETRY` / `_ATTEMPTS` | 10 ms / 4 | | list request retry seed (then RTT + tx interval) |
| `tt_LIVELINESS_MISS_THRESHOLD` | 3 | | silence limit = 3.5 intervals |
| `tt_LIVELINESS_LEASE_DIVISOR` | 6 | | summaries per shortest lease |
| `tt_CONTEXT_MAX_LEASE_NS` | 10 s | | longest lease that holds a silent node |
| `tt_RELIABLE_RETRY_INTERVAL` | 0 (dynamic) | | ACKNACK retry; non-zero = fixed |
| `tt_RELIABLE_RETRY_INITIAL` | 1 ms | | dynamic start value |
| `tt_RELIABLE_RETRY_GRANULARITY` | 100 us | | timer lateness term |
| `tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE` | 64 | | ceiling in srtt |
| `tt_RELIABLE_RETRY` | 3 | | KEEP_LAST give-up |
| `tt_RELIABLE_BITMAP_BITS` / `_MAX_BITS` | 256 / 4096 | | reader window default / ceiling |
| `tt_MAX_RELIABLE_HISTORY` | 64 | | reference cache depth for examples |
| `tt_CALL_RETRY_INTERVAL` | 5 ms | | RPC auto retry seed, until a first answer |
| `tt_CALL_RETRY_GRANULARITY` | 200 us | | two hosts' event lateness term |
| `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` | 64 | | a wait's ceiling in srtt |
| `tt_CALL_DEADLINE_PER_SEND` | 250 ms | | auto call fails within (count + 1) x this (1 s) |
| `tt_CALL_RETRY_COUNT` | 3 | | RPC retries |
| `tt_SERVER_CACHE_GAP_MULTIPLE` | 4 | | answered-response cache, in observed retry gaps |
| `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` | 5 s | | deferred response slot lifetime |
| `tt_MAX_SERVER_CACHE_COUNT` | 64 | | server response slots |
| `tt_SERVER_CACHE_ENTRY_LENGTH`, `tt_CLIENT_CACHE_LENGTH` | 2 x buffer | 8 | inline storage (rmw attaches its own) |
| `tt_MAX_SCHEDULER_LENGTH` | 128 | | scheduler entries |
| `tt_SCHED_INBOX_LENGTH` | 32 | | cross-thread timer inbox |
| `tt_SCHEDULER_IO_INTERLEAVE` | 8 | | due entries before an I/O check |
| `tt_RECEIVE_TIMEOUT` | 100 us | | poll slice some callers pass |
| `tt_RX_BATCH` | 32 (1 if buffer > control) | | datagrams per `recvmmsg()` |
| `tt_RX_CLOCK_REFRESH` | 16 | | datagrams per clock read |
| `tt_RX_LOCK_CHUNK` | 8 | | buffered datagrams per lock hold |
| `tt_HAL_RX_HINT` | AUTO (0) | | io_uring receive hint: AUTO / READ / URING |
| `tt_HAL_FREERTOS_NETCONN` | 1 | | FreeRTOS HAL on lwIP netconn with an arrival count (receive hint); 0 = BSD sockets |
| `tt_SOCKET_BUFFER_SIZE` | 1 MiB | 4 MiB | requested SO_SNDBUF/SO_RCVBUF |
| `tt_SEGMENT_ENABLED` | 1 (0 FreeRTOS) | 1 | shared-memory transport |
| `tt_SEGMENT_BYTES` | 768 KiB | | ring budget per context |
| `tt_SEGMENT_SLOT_BYTES` | = control (1472) | | slot payload; runtime `segment_slot_bytes` |
| `tt_SEGMENT_SLOTS` | derived (512) | | power of two |
| `tt_SEGMENT_BELL_FIFO` | 1 | | FIFO doorbell (0 = UDP doorbell) |
| `tt_SEGMENT_DRAIN_PER_POLL` | 4 x slots | | records drained per poll |
| `tt_SEGMENT_DEAD_READER_NS` | silence x 2/7 (1 s) | | reader silence before falling back to UDP |
| `tt_SEGMENT_STALL_PASSES` | 1000 | | wedged-head warning |
| `tt_CONTEXT_ID_CLAIM` | 1 (0 FreeRTOS) | 1 | host-registry id claiming |
| `tt_LOCAL_DELIVERY` | 0 | 1 | in-process delivery within a context |
| `tt_DISCOVERY_OPTIONS` | 0 | 1 | discovery range and static peers |
| `_tt_CONTEXT_PORT` | 8282 | + domain | well-known port |
| `_tt_CONTEXT_BROADCAST` | 255.255.255.255 | | set the link's directed broadcast |

## 16. Where the older documents went

On 2026-10-05 the planning documents were condensed into `docs/`. Their full text, with every measurement narrative
and decision record, is in git at `3c0c505b`: `git show 3c0c505b:<old path>`. Source comments that cite an old
document (for example "SHM_PLAN 6c") resolve the same way.

| Old path | Now in |
|---|---|
| `DESIGN.md` | this document |
| `rmw_tickle/SHM_PLAN.md`, `WIRE_PLAN.md`, `DATAFRAG_PLAN.md`, `CONTEXT_NODE_PLAN.md`, `DISCOVERY_PLAN.md`, `LIVELINESS_PLAN.md`, `MODULE_PLAN.md`, `WIRELESS_PLAN.md` | this document; open items in [ROADMAP.md](ROADMAP.md) |
| `rmw_tickle/COMPARISON.md` | [RESULTS.md](RESULTS.md); open items in [ROADMAP.md](ROADMAP.md) |
| `rmw_tickle/PLAN.md`, `RMW_GAPS_PLAN.md`, `LARGE_MESSAGE_PLAN.md`, `tools/typesupport/PLAN.md` | [RMW.md](RMW.md), [ROADMAP.md](ROADMAP.md) |
| `rmw_tickle/RMW_PERF_PLAN.md`, `ZENOH_PICO_PLAN.md`, `examples/perf_hil/OPTIMIZATION_PLAN.md`, `CORE_HEADROOM.md`, `CONSTANTS_AUDIT.md`, `.github/scripts/README.md` | [TESTING.md](TESTING.md), [RESULTS.md](RESULTS.md), [ROADMAP.md](ROADMAP.md) |
| `rmw_tickle/SECURITY_PLAN.md` | [ROADMAP.md](ROADMAP.md), "The parked security plan, in short" |
| `.github/scripts/README-rmw-perf.md` | [RMW_PERF_SETUP.md](RMW_PERF_SETUP.md) |
| `CONTRIBUTING.md`, `CHANGELOG.md` | [CONTRIBUTING.md](CONTRIBUTING.md), [CHANGELOG.md](CHANGELOG.md) |
| `MORNING_2026-09-26.md` | removed (a one-night report) |
