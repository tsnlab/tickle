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
| 4 | `CALLREQUEST` | endpoint_id, seq_no (u16), retry, client_tag - 8 B | service request; client_tag: which Client of its context (0 = untold) |
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
- **UDP offload (Linux HAL).** In that call a run of a large sample's fragments (`FRAG_FIRST_L`/`FRAG_CONT_L`, same
  size but the last) to one destination, each at most 1472 B, goes as one `UDP_SEGMENT` message (at most 64 datagrams
  / 65507 B); the kernel or NIC cuts it back into exactly those datagrams, so the wire is unchanged
  (`platform/linux/udp_offload_check.sh` proves it). Large fragments only, and compiled out without `tt_LARGE_SAMPLES`:
  on the rig (Pi 5) core p4's run of two `DATA_FRAG` datagrams cost +3.5% publisher CPU per sample and left 3% of
  latency pings late past 500 ms, though on the PC it takes fewer instructions (`gso_run_cost.sh`); 1 MB samples' runs
  of 44 paid (publisher -19%). In a
  build whose buffer holds a merged read (65507, rmw_tickle's), `UDP_GRO` on both sockets lets one read return a run,
  handed to core one datagram at a time in place (`tt_Context.rx_offset`, 4-aligned; at 4 mod 8 - every other 1468 B
  large fragment - a loaned take decodes instead of reading in place). `UDP_GRO` is on only from a large sample's
  fragment until 128 reads without one (a GRO read costs more even unmerged: +2.3% at 1 KB), and goes off only across
  a moment both queues are seen empty, so no merged read is ever taken without its size (`hal_linux.c`, "When receive
  offload is on"; arm E of the check). Refused by the kernel or after `EIO`: off.
  `TT_UDP_OFFLOAD=0` in the environment turns both off (the A/B control). Traffic line: `udp_offload`, `gro_*`,
  `gso_*`. With the hybrid transport (section 8) both apply to MTU-mode traffic only: a run (`FRAG_RUN_L`) is one
  IP-fragmented datagram, never a `UDP_SEGMENT` run and never merged by GRO (IPv4 fragments bypass the kernel's GRO;
  the check's pattern 8 reads runs whole with `UDP_GRO` on), and a sample's lone MTU tail after its runs neither turns
  `UDP_GRO` on nor keeps it on - so an AUTO stream of runs reads plain. Whether offload is kept on by default is
  decided by the rig A/B (`~/rig_queue_hybrid.sh`, stage 4).

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
  G the host's timer lateness (below), capped at `tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE` (64) x srtt. Any non-zero
  value is used as given.
- **G, measured** (since 2026-10-08; it was a fixed 100 us fitted to nothing): every wait in `tt_Context_poll()` for
  a retry timer (`acknack_retry`, `call_retry`) that runs to its deadline is a sample of how late it returned (clock on
  waking minus the deadline). A wait ended early by a datagram, a wake or a signal is not, and neither is a wait for
  any other deadline: how late a wait ends depends on its length (Linux's poll timer slack is max(50 us, 0.1% of the
  timeout)), so on the dev PC the context's 500 ms announce and budget waits came back ~520 us late against ~60 us for
  1 ms waits. Nor is a wait that came back later than it was long: it never slept on the timer (a busy loop's
  sub-microsecond wait reads the system call's ~0.5 us, which held G at 1-3 us under traffic and cost +0.03-0.07% wire
  bytes a sample at c5/c6) or was preempted for longer than it lasted. Samples are smoothed with RFC
  6298's gains (mean and mean deviation, 1/8 and 1/4) and G = mean + 4 x deviation: a lateness the timer seldom
  exceeds, not its median. Floor: `tt_timer_resolution_ns()` (HAL; `clock_getres` on Linux, one tick on FreeRTOS).
  Before the first sample G is `tt_TIMER_LATENESS_INITIAL` (100 us, the old constant), so a context whose retry timer
  has not fired from a wait behaves as before. Per context, 32-bit, written only by the poller
  (`timer_lateness_fold()`). `tt_RELIABLE_RETRY_GRANULARITY` 0 (default) is the measured G; non-zero is a fixed G.
  On the dev PC (KVM, `granularity_pc.sh`, branch `ab/granularity`): a 1 ms retry timer gives G ~64 us idle and ~71 us with every
  core busy (its lateness p90 60-83 / 55-58 us); inside the c5/c6 shapes G ends anywhere from ~1 us (a busy loop's
  retry waits of a few microseconds are on time) to ~180 us. The old 100 us also acted as a floor under 4 x rttvar:
  without it c5 cost +0.04% wire bytes a sample (t 2.4, the fixed-G control -0.01%); c5 throughput and c6 held.
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

- A Client has one outstanding call, cached for retry in `cache_buf` (or caller-attached storage). A call's seq_no
  comes from the context's one counter, so several Clients of one service in one context (same endpoint_id) send
  distinct requests and each answer goes to the Client whose call it names (2026-10-09; before, they were answered as
  one). When the counter comes round, a seq_no another Client of the service still waits on is skipped. Each Client
  of a service in a context has its own `client_tag` (1..255), carried in the CallRequest's former pad byte. The
  round trip is timed from before the send, not after it returns.
- Retries: `call_retry_interval` if set, every time. Otherwise (auto) RFC 6298 over call-to-answer times, with bounds
  relative to srtt since 2026-10-05: the first wait is `srtt + max(G, 4 x rttvar)`, each
  retry of the call waits twice the one before, each at most `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` (64) x max(srtt, G). The seed
  `tt_CALL_RETRY_INTERVAL` (5 ms) stands in for srtt until a first answer. A call gives up after
  `(2^(count+1) - 1)` first waits (15), and never later than `(count + 1) x tt_CALL_DEADLINE_PER_SEND` (1 s, the old
  worst case): a wait past that is cut there. A timeout doubles srtt, up to that deadline; the next answer replaces
  the estimate. A call's G is twice the reliable retry's (this host's measured lateness, section 6), standing in for
  two late-running events - this host's timer and the server's dispatch - since the server's cannot be measured from
  here; `tt_CALL_RETRY_GRANULARITY` non-zero fixes it.
- A Server caches each answered response in one of `tt_MAX_SERVER_CACHE_COUNT` (64) slots, so a retried request gets
  the same answer without re-running the callback: one live answer per calling Client - (source context,
  `client_tag`), looked up with the seq_no - replaced when that Client calls again (one per source context until
  2026-10-09, so a retry after another Client of that context was answered re-ran the callback; a sender older than
  the tag sends 0 and still gets that). It keeps it for the longer of the client's seed schedule (or the
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
97.4% of reassemblies at 5% loss; TickLE fragments are repaired one datagram at a time. Samples above 64 KB may also
travel IP-fragmented while a peer's path is clean (the user, 2026-10-11; "Hybrid transport" below, not built).

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

**Larger than 65507 B** (above `tt_MAX_SAMPLE_LENGTH`): stage 2 below - step A, the socket path, is on main; step
B, the same-host region, is not. A fragment-assembled small sample cannot be lent (section 10, receive-buffer lending):
it is retained by copying. A large one is lent from its own buffer.

### Stage 2: samples above 64 KB (designed 2026-10-09; step A on main 2026-10-10, step B not)

**Status (2026-10-10).** Step A landed on main with the zero-copy loans (section 10): a claim never covers a large
sample (`UNSUPPORTED`), a claimed publish commits a large sample waiting behind a send first, and a large sample is lent
from its own 8-aligned buffer (`tests/test_large_shm.c`). Its L4 cross-host rows are RESULTS.md "rmw large messages".
Same-host large samples still go through the 512-slot ring and do not fit (step B).

The user's staged-support decision (2026-09-27, LARGE_MESSAGE_PLAN decision 1 "B"; stage 1 was the rmw direct codec
and serialized messages). Target: rmw `sensor_msgs/Image` and `PointCloud2` of 1-8 MB (VGA rgb8 921,600 B, 720p rgb8
2.76 MB, 1080p rgb8 6.2 MB; a 64-beam lidar cloud about 3 MB). Unchanged: TickLE splits, never IP (section 8's
97.4%), so no 65507-B datagrams and no jumbo datagrams; each datagram keeps its own seq_no (the user, 2026-09-26).

**What bounds the size today, and what stage 2 moves.**

| bound | today | stage 2 |
|---|---|---|
| fragment index and count on the wire | `uint8_t`: 255 fragments, ~370 KB | `uint16_t` in two new types (below): 65,535, ~95 MB |
| `tt_FRAG_MAX_COUNT` | 64 (sized for 65507 B) | unchanged for small samples; large ones do not use the batch arrays it sizes |
| staging: one submessage in `tx_buffer` (`uint16` length), `frag_scratch`, BE pool slots | `tt_MAX_SAMPLE_LENGTH` each, static | not used by large samples: caller-acquired buffers (below) |
| reader window (RELIABLE): one sample's datagrams must fit it | `tt_RELIABLE_BITMAP_MAX_BITS` 4096 (~5.9 MB) | 8192 (~11.3 MB); its ACKNACK (1,052 B) still fits one control datagram, whose ceiling is 11,520 bits |
| rmw per-field capacity (`Image` data 64,000 B) | refuses above it | not applied on the direct codec for a large sample |

So the **maximum sample is about 11.3 MiB** (1,446 + 1,452 x 8,191 B), set by the window, and rmw defaults to 8 MiB
(`RMW_TICKLE_MAX_SAMPLE_BYTES`, refused above it with an error naming the limit). Datagrams per sample: 1 MB 723,
4 MB 2,889, 8 MiB 5,778.

**Wire: two new submessage types, the next `tt_VERSION`.** `FRAG_FIRST_L` (type 10) and `FRAG_CONT_L` (type 11) are
types 8 and 9 with `frag_index`/`frag_count` widened to 16 bits (CONT header 12 B, FIRST 18 B, shortfall 6). They are
used only when a sample needs more than 255 fragments; every smaller sample keeps types 8/9 byte for byte. Widening
8/9 instead would cost 2 B in every fragment of every 1.5-64 KB sample (0.14% of P3/P4's bytes) and break the wire
rule. A node built without stage 2 skips 10/11 and counts them (`frag_large_skipped`).

**Storage: caller-acquired, sized per sample, never static.** Core does not malloc, so a context takes a pair of
callbacks, `large_acquire(user, bytes) -> void *` and `large_release(user, ptr)`. rmw backs them with `malloc` and a
small free list; a core-only user backs them with a static pool. `NULL` from acquire is "no room", never a crash.

- **Publisher.** `data_encode_size` gives the length; above 255 fragments core acquires a buffer, `data_encode`s into
  it once, and sends its fragments straight from it: `sendmmsg()` of up to 64 datagrams, each two iovecs (its framing,
  20 B or 26 B for fragment 0, and a slice of the buffer), so no copy into `tx_buffer`. The reliable cache **retains the buffer
  by reference** (one index entry covering the sample's k seq_nos, like a segment record's `seq_span`), resends a lost
  fragment by rebuilding its framing, and releases the buffer when every matched reader has acked it or KEEP_LAST
  evicts it. BEST_EFFORT releases it when the last fragment is sent.
- **Send cursor.** A 4 MB burst overruns any socket buffer (the rig caps it at 212,992 B). On `EAGAIN` core keeps a
  per-Publisher cursor and sends the rest when the socket is writable, from `tt_Context_poll()`. A publish while
  the cursor is busy: KEEP_ALL returns `tt_RET_WOULD_BLOCK`; KEEP_LAST abandons the older sample's unsent tail (its
  seq_nos are announced gone by HEARTBEAT, as for an evicted sample) and counts `large_tail_abandoned`.
- **End-of-sample HEARTBEAT.** A RELIABLE writer sends a non-FINAL HEARTBEAT after the last fragment of each large
  sample (1 datagram in 723 at 1 MB), so a lost tail is NACKed one round trip later instead of waiting for the next
  sample.
- **Subscriber.** The first fragment that arrives (FIRST or CONT, both carry `frag_count`) acquires
  `frag_count x 1452` bytes keyed by (source, entity_id, sample seq_no); each fragment is copied to its offset (the
  index gives it), so order does not matter and the reorder ring and `frag_scratch` are not touched. A fragment is
  recorded as received only once stored, as today. No buffer: RELIABLE leaves the fragments unrecorded (they are
  NACKed later, which is the reader's back-pressure); BEST_EFFORT drops the sample and counts `large_no_buffer`;
  a torn sample is never delivered. The reorder ring keeps holding only small datagrams, at its present size; a
  wider window does not grow it (a small datagram beyond the ring is not recorded and gets repaired).
- **Copies.** Publish: one encode (rmw's serialize). Receive over a socket: kernel to `rx_buffer`, then fragment to
  the large buffer (one user copy), then rmw's decode. No copy beyond those.

**Delivery without a copy: lending.** The callback's sample points into the large buffer. `tt_Sample_retain()`
treats that address as lendable (section 10's test widens from "in the datagram being processed" to "or in a
large buffer core acquired"), so `release` hands the buffer back to `large_release`; without a retain core releases
it when the callback returns. It is not a ring slot, so a held large sample blocks nothing else. Counts against
`tt_SAMPLE_RETAIN_MAX` like any other.

**Same-host: a writer-owned sample area (step B).** The receiver's ring (768 KiB, 512 slots) cannot hold one 4 MB
sample's 2,889 records, and drop-on-full would tear every BEST_EFFORT sample. So a large sample to same-host peers
goes once into a **writer-owned shared region** (`/dev/shm/tickle-large-<ip>-<port>-<id>`, mapped read-only by
readers), and each reader's ring gets one small descriptor record (offset, length, generation) whose `seq_span` is the
sample's datagram count, so the seq space matches the socket path exactly. The reader lends straight from the region:
publish is one encode, receive is zero core copies. Each area chunk carries a reader bitmask (by context id); a reader
clears its bit on release, the writer reuses the chunk when the mask is clear, and liveliness clearing a dead peer
clears its bit. Ring protocol unchanged; the region is new (`tt_SEGMENT_VERSION` 5). Until step B lands, large
samples to an attached same-host peer go over the ring as fragments, which needs a ring of more than one sample, and
the same-host cells are not published.

**Flow control (KEEP_ALL).** The bound stays `min(cache depth, narrowest window)` in datagrams, and a sample is
admitted only if all its datagrams fit. A matched reader whose window is below one sample's count can never admit it,
so that publish fails with `tt_RET_TOO_LARGE` (counted `large_window_too_small`), not an endless `WOULD_BLOCK`. rmw
gives a subscription of a type with an unbounded sequence the 8192-bit window (1 KiB per tracked writer) and raises
its KEEP_ALL byte budgets to at least two max samples. Comparison cells keep the equal-bound rule (the user,
2026-10-06): DDS N samples, TickLE (N + 1) x the sample's bytes.

**Out of scope.** FreeRTOS: no buffer callbacks, the lwIP heap cannot hold a megabyte sample, so stage 2 compiles out
(`tt_LARGE_SAMPLES` 0) and a FreeRTOS node only skips types 10/11 (it is rebuilt for the version bump all the same).
Services above 64 KB (they are not fragmented at all). rmw loans of `Image` (its wire bytes are not its message).
UDP GSO (it cannot give each segment its own header).

#### Pre-registration (Plan, 2026-10-09, before any code)

Implemented as checks in the tests and harnesses, not as prose. Every A/B runs `main` against `ab/large-stage2`, one
change per commit, ABBA blocks, equal warm-up and cool-down (ROADMAP Now 0a), and reads by the 2 x SE rule.

**L1. Correctness (Dev's tests; each with a mutant that must fail it).**

1. Byte-exact round trip at 65,508 B, 255 and 256 fragments, 1 MB, 4 MB, 8 MiB, over socket and segment, RELIABLE and
   BEST_EFFORT. Mutant: off-by-one in the 16-bit offset arithmetic.
2. **Small samples untouched on the wire:** the datagrams of 64 B, 1,472 B and 64,000 B samples are byte-identical to
   ones captured from the parent before the change (time fields masked; a parent-vs-parent capture is the control).
   Mutant: always use types 10/11 - fails.
3. RELIABLE KEEP_ALL at 5% `tc` loss in a netns: 100% of 1 MB and 4 MB samples, in order. Tail: drop each sample's last
   fragment once; recovered in at most 2 retry intervals. Mutant: no end-of-sample HEARTBEAT - fails the tail bound.
4. BEST_EFFORT with `large_acquire` returning NULL every third call: those samples counted, none delivered torn.
5. A reader with a 4096-bit window and an 8 MiB sample: `tt_RET_TOO_LARGE`, no hang. Mutant: WOULD_BLOCK.
6. `SO_SNDBUF` forced to 64 KiB: every 4 MB sample still delivered (the cursor). Mutant: cursor dropped - fails.
7. Lending: retain a 4 MB sample, release it from another thread; its buffer is not reused before the release
   (content check). Step B: a reader killed while holding a chunk; the writer reuses it after liveliness drops the
   peer, not before.
8. Every build configuration of the gates, `freertos link`, `make test-linux`, and the rmw suite in a netns.

**L2. No cost below 64 KB (A/B, rig and PC netns).** P1-P4 latency, throughput, CPU and RSS, and the rmw rows 52-75:
every metric held or better. Controls: a parent-vs-parent arm in the same session (its delta is the noise), and the
vendor arms (rig drift). A WORSE metric outside the parent-vs-parent spread blocks landing.

**L3. Each design choice earns its place (A/B within the branch).**

- By-reference retention vs copying into the cache arena: publisher CPU per MB lower beyond 2 x SE, or the copy
  (simpler) is kept.
- Step B vs fragments through a ring sized for the sample: subscriber plus publisher CPU per same-host MB lower, and
  latency median not worse. If not, step B is dropped and the ring path stays.
- End-of-sample HEARTBEAT: 1 MB cross-host p99 latency at 5% loss lower; if not, removed.

**L4. The vendor comparison** (rmw_tickle vs rmw_fastrtps_cpp and rmw_cyclonedds_cpp; each DDS at its default
first, then the vendor-favouring setting as a labelled second arm: Fast DDS `maxMessageSize` 1472, CycloneDDS shared
memory).

| cell | size, rate | where | QoS |
|---|---|---|---|
| I1 | `Image` 512x512 rgba8 = 1,048,576 B, 30 Hz | same-host and cross-host | RELIABLE KEEP_LAST 10 (ROS default) and `sensor_data` (BEST_EFFORT KEEP_LAST 5) |
| I4 | `Image` 1024x1024 rgba8 = 4,194,304 B, 30 Hz same-host, 15 Hz cross-host | same-host and cross-host | the same two |
| I4s | 4 MB at 30 Hz cross-host (1.007 Gbps, above 1 GbE): a saturation cell | cross-host | `sensor_data` |
| I1L | I1 cross-host under 5% `tc` loss | cross-host | RELIABLE |

Read per cell: delivered rate (Hz and % of published), publish-to-take latency (median, p99), CPU of each process
per delivered MB, peak RSS. Cross-host latency has a wire floor (about 8.9 ms at 1 MB, 35.5 ms at 4 MB on 1 GbE),
reported beside it. Sample count first: 60 s of 30 Hz is 1,800 samples a run, 5 runs a cell; a delivery comparison
uses the `(1 - p)^n` rule before it is read. A vendor that does not run is written "does not run" with its numbers.
Incomplete TickLE delivery in a cell is a LOSE.

**What would falsify the design.**

- **Fragment-level repair does not scale:** I1L RELIABLE delivers less than Fast DDS, or its p99 is more than twice
  Fast DDS's. Per-datagram seq_no then costs too much at 700+ fragments a sample, and the sample-level numbering the
  user declined on 2026-09-26 comes back to the user as a question, with these numbers.
- **The copy budget is wrong:** TickLE's subscriber CPU per MB cross-host is not below both DDS in I1. The design
  assumes one user copy plus decode; if that does not beat them, the copy is not where the cost is and the receive
  path (recvmmsg rows, scatter-gather; ROADMAP) comes first.
- **Step B pays nothing:** L3's same-host A/B shows no CPU gain, so a second shared region is complexity without a
  return.
- **I4s:** the expected outcome is that every framework delivers below 30 Hz; TickLE delivering fewer complete samples
  than the best DDS means the send cursor's KEEP_LAST abandonment loses whole samples a DDS keeps.

**Decided by the user 2026-10-09 (all four approved).** (1) The wire change: types 10/11 and the next `tt_VERSION` (and step B's
`tt_SEGMENT_VERSION` 5). (2) Keep per-datagram seq_no for large samples (5,778 seq_nos and an 8192-bit window per
8 MiB sample) - the design assumes yes. (3) The 8 MiB rmw default and the ~11.3 MiB ceiling. (4) I4 cross-host at
15 Hz plus the I4s saturation cell, since 4 MB at 30 Hz exceeds the rig's link.

#### Step A as built (2026-10-09), and where it departs from the above

Built as designed: `tt_VERSION` 12; per-datagram seq_no; caller-acquired buffers (`tt_Context_set_large_buffers()`),
encoded once and sent straight from (two iovecs a datagram, `sendmmsg()` of up to 64); the writer keeps the buffer by
reference (`tt_Publisher.large[]`, `tt_LARGE_RETAINED` 16) and resends one datagram at a time from it; a per-Publisher
send cursor resumed from `tt_Context_poll()`; a non-FINAL HEARTBEAT after each large sample's last fragment
(`tt_LARGE_END_HEARTBEAT`); the reader copies each fragment to its offset in an acquired buffer
(`tt_LARGE_ASSEMBLIES` 4), records it only once stored, delivers in order from there and lends it
(`tt_LEND_LARGE`); KEEP_ALL refuses a sample wider than a matched reader's window with `tt_RET_TOO_LARGE`; the window is
8192 bits with stage 2 compiled in; rmw_tickle compiles it in (`-Dtt_LARGE_SAMPLES=1`); rmw
defaults to 8 MiB (`RMW_TICKLE_MAX_SAMPLE_BYTES`) and gives a type without a size bound the widest window. Tests:
`tests/test_large_sample.c` (L1.1, 1.3-1.7, each with its mutant in `tests/mutants_large_sample.py`),
`tests/test_wire_identity_largemsg.c` and `tests/wire_identity_largemsg.sh` (L1.2), `tests/test_large_skip.c`.

Where it departs, and why:

- **Types 11 and 12, not 10 and 11.** 10 is `tt_SUBMESSAGE_TYPE_SHM_DATA`, reserved for good and refused from the
  socket (tickle.h); giving it a second meaning is what that comment forbids.
- **Every sample above `tt_MAX_SAMPLE_LENGTH` uses 11/12, not only those above 255 fragments.** Routing by fragment
  count is ambiguous where the two paths meet: at rmw's 65,507 a sample of 65,508-66,877 B has the same 46 fragments
  as one of 65,507, and the small receiver cannot hold it. By type, the receiver never guesses. Nothing that could be
  sent before is affected - L1.2 holds - and samples of 64-370 KB pay 2 bytes a fragment they did not pay under types
  8/9, samples that could not be sent at all before.
- **KEEP_LAST does not abandon the tail of a sample being sent.** A publish while a send is in progress waits behind
  it, without seq_nos yet (`tt_Publisher.large_pending`); a newer one replaces it before any of it has gone
  (`large.tail_abandoned`). Abandoning the sample in flight, as designed, would deliver nothing at all in I4s - each
  4 MB sample takes longer than the 33 ms to the next - where this delivers at the link's rate; and a waiting sample
  that holds no seq_nos leaves no gap for readers to give up on. The falsification test for I4s still reads it.
- **The writer releases a VOLATILE Publisher's large buffer once every matched reader has acknowledged it**; a DURABLE
  one keeps it until KEEP_LAST evicts it (for late joiners).
- **The send cursor resumes on a timer, not on socket writability**: 50 us after a full send buffer, doubling to 1 ms
  while nothing goes (`tt_LARGE_SEND_RETRY_NS`, `_MAX_NS`). No HAL wait on POLLOUT was needed; one new HAL call,
  `tt_send_batch_nonblocking()` (MSG_DONTWAIT), returns how many datagrams went.
- **The end-of-sample HEARTBEAT is chased**: it is one datagram and can be lost with the tail it reveals, so while a
  large sample stays unacknowledged it is resent, one retry interval on and doubling to 100 ms, restarted per sample.
- **A sample that takes longer to go out than a reader waits is repaired while it goes**: a HEARTBEAT announces only
  seq_nos already sent (an unsent tail would read as a gap), and a datagram asked for that has already gone is resent
  at once instead of after the send. And a writer blocked in one long send reads no ACKNACK until it ends, so a
  reader's retry while that sample's fragments are still arriving does not count against a KEEP_LAST writer's
  budget (its first interval, before any recovery is timed, is 1 ms; a 4 MB send takes ~15). Without these a
  KEEP_LAST reader gave up the first 4 MB sample under 5% loss on every PC run.
- **Stage 2 is compiled in only where it is asked for** (`tt_LARGE_SAMPLES`, default 0; rmw_tickle sets it). On
  wherever fragmentation was, it cost every P4 cell on the rig +15..27 kB peak RSS (L2 stage 1, 2026-10-09): a
  4.4 KB context, 680 B a Publisher, the 8192-bit window widening every window-sized array. Off, a build matches the
  parent but for one 8-byte counter, and P4's RSS on the PC is the parent's to within 4 kB. A build without it skips
  types 11/12 and counts them, as FreeRTOS does.
- **Fragment positions use the receiver's own geometry** (`tt_CONTROL_MAX_LENGTH`), as the sender's: a large sample
  between two nodes built with different control datagrams is dropped and counted (`large.dropped`), where the small
  path learns the sender's continuation size.
- **A fixed-size type above a sample is a topic too** (perf_test's `Array1m`): core takes a C struct up to the
  largest large sample when its Subscriber decodes in place, and rmw checks a topic type against
  `RMW_TICKLE_MAX_SAMPLE_BYTES` rather than one datagram (services keep the datagram). Missed at first because the
  PC evidence used `sensor_msgs/Image`, whose struct is small; the rig's first Array1m runs were all refused.
- **rmw: a sequence count of 65,535 or more is the escape 0xFFFF and a uint32** (the direct codec). A count was a
  uint16, so an `Image` of 1 MB could not be encoded at all; every count below 65,535 keeps its two bytes, and no
  sample within 65,507 B can hold a longer sequence, so no small sample changes.
- **L3's by-reference row has no copy arm to A/B against**: copying a large sample into the cache arena would need
  the arena to hold `depth` samples of up to 8 MiB each (80 MiB at the ROS default depth of 10) and the index one
  record per datagram (57,780 at 8 MiB x 10), which is the memory stage 2 exists not to spend. The end-of-sample
  HEARTBEAT row is runnable (`-Dtt_LARGE_END_HEARTBEAT=0`).
- **Interactions with the claimed-slot publish and lending, found when the two branches met (2026-10-10).** A claimed
  publish took its seq_no ahead of a large sample still waiting behind a send (`large_pending`); it now commits that
  sample first, as an ordinary publish does. A large sample's delivery sets the lending state to LARGE, which hid from
  the ring's release rule that the last fragment's slot was still being read: a departure handled in that callback
  unmapped the ring under the drain. The kind it replaced is kept (`lend.rx_outer_kind`).
- Not yet: large samples in a local durable backlog (`tt_Subscriber_deliver_local_backlog()`), and step B.

### Encoding while sending (2026-10-11, `ab/interleave-encode`)

**Why.** A large publish encoded the whole sample into its buffer and only then sent the first fragment: about 0.7 ms
on a Pi 5 for 4 MB (one copy of it), on the latency path of every sample. Fast DDS pays the same (it serialises
before it sends). Nothing needs the whole sample before the first fragment goes, so the encode is moved alongside
the sends.

**API.** `tt_Topic.data_encode_range(data, payload, offset, len)` (optional, `tt_DATA_ENCODE_RANGE`): writes bytes
`[offset, offset + len)` of what `data_encode` writes. A topic that sets it promises that `data_encode_size` is exact
and that the bytes are `data_encode`'s. Trivial where the wire bytes already sit in memory: rmw_tickle offers it for a
serialized message and for a type whose wire bytes are its message (`inplace_bytes`, the loanable types: perf_test's
`Array*`), as the psn header plus a `memcpy`; every other type declines and is encoded whole, as before.

**Sender.** Core asks for fragment 0's bytes before committing the sample (a decline there is today's path, untouched).
Then each send call - up to `LARGE_BATCH` (64) datagrams to the first destination - first encodes through the last
byte it carries. Later destinations, resends and the durable backlog read what is already encoded. Every range is
asked for within the publish call, because the caller's sample is valid only there:

- when the socket's send buffer fills (`EAGAIN`), the rest is encoded at once, while the buffer drains (it holds
  ~1.7 ms of 1 GbE, more than the ~0.7 ms of encoding), not a send call at a time with a retry after each;
- so when the publish returns, the record is the whole encoded sample: retention, KEEP_LAST eviction, resends, the
  VOLATILE release and LIFESPAN are exactly as before.

Encoded whole, as before, when the sample is not sent within its publish (it waits behind a send in progress,
`large_pending`) or when this context's own Subscribers take it in-process (`tt_LOCAL_DELIVERY`), which needs it
whole at once. A range declined after part of the sample has gone is made up by `data_encode` of the whole sample
(the same bytes by the contract, so what went is unchanged). If that fails too, the sample is dropped - the rest is
never sent, its buffer goes back, the publish returns `tt_RET_PROTOCOL_ERROR`, `large.encode_failed` counts it - and
its seq_nos read as gone to a reader that asks, so a torn sample is never delivered.

**The wire does not change.** Every datagram - fragments, HEARTBEATs, repairs - is byte-identical, in the same order,
with and without the ranged encode (`tests/test_large_sample.c`, against the arm without it; the whole step-A suite
also runs in both modes). Small samples never use it.

**Larger datagrams.** The unit is "the bytes the next send call carries", not a fragment. With the hybrid transport
(below, `ab/hybrid-land`) a send call of runs encodes through the last unit of its last run before its `sendmmsg()`,
exactly as a batch of MTU fragments does; the tail, alone in its own call, is encoded just before it; MTU repairs read
the record, whole by the time the publish returns. The wire is byte-identical with and without the ranged encode in
runs too, and a sample received in runs is lent from the assembly it was put together in (`tests/test_hybrid_send.c`).

**Pre-registration (2026-10-11, before any measurement).**

- *PC* (`experiments/interleave_encode_pc.sh`, read by `interleave_encode_ab.py`): veth + netem 1 Gbit/s, arms parent
  / head without a ranged encode (the control: the change cannot touch that path) / head with it, ABBA. The 4 MB
  latency median must improve beyond the floor by at least half a cold 4 MiB memcpy measured on the same PC; latency
  p99 and publisher and subscriber CPU per MB not worse. Not a published figure.
- *Rig* (I4, `Array4m` 4,194,304 B, 15 Hz cross-host, RELIABLE KEEP_LAST 10 and BEST_EFFORT KEEP_LAST 5): arms
  `tickle@head`, `tickle@pre` (the parent), `fastdds`, `cyclonedds`, by `largemsg_lend_ab.py`'s method (Welch 2 x SE
  and the vendors' drift). Head vs pre: latency median BETTER by at least 0.35 ms (half the 0.7 ms moved) in both
  cells; p99, CPU per MB (both sides) and peak RSS not WORSE; delivery 100% in RELIABLE. Head vs Fast DDS as shipped:
  latency median read by the range rule and printed with the prediction (WIN or DRAW in both cells). Falsified if
  either cell's median is not BETTER: then the first fragment's wait was hidden by the wire queue and the 0.7 ms is not
  on the latency path. Core P1-P4: no regression by the instruction-count gate (no small sample takes this path).

**Result (rig, 2026-10-11).** Head vs pre, 5 reps per cell (`interleave_encode_rig_ab.py`;
`interleaveEnc_s1_20261011-123551` RELIABLE, `interleaveEnc_s2_20261011-130353` best effort): latency median -0.81 and
-0.84 ms, both BETTER beyond the 0.35 ms threshold, nothing WORSE, delivery 100%; against Fast DDS as shipped -0.46 and
-0.45 ms (WIN). The core p1-p4 cross-host campaign (A B B A, `interleaveEnc_l2x_compare.txt`): all 46 primary
comparisons held, none WORSE (its verdict line reads NO VERDICT only because `rtt_p99_ms` matched no cell). Measured on
branch ab/interleave-encode at 5130e3ae against its parent ba324d3b, both carrying the UDP offload commits of
ab/udp-offload-v2 (parent 11cb21e8); landed without UDP offload, not re-measured.
### Hybrid transport for multi-datagram samples (designed 2026-10-11, revised the same day; the rig campaign not run)

**Status.** Steps 1 to 5 are built on `ab/hybrid-step34` (below and "Steps 3 to 5 as built"). The user's second set of
decisions (2026-10-11, "The objective and the control law" below) replaced step 5's CPU cost model, its run-time cost
fit and the reader's kernel-derived cap: n now minimises **wire bytes per delivered unit**, and every input is
something TickLE measures itself. Built on `ab/hybrid-network-objective`; not run on the rig (step 6).

**The decision.** The user lifted the IP fragmentation park on 2026-10-11 and generalised it the same day: each
writer chooses, per peer, how many MTU fragments go into one IP-fragmented UDP datagram, from a cost model and the
loss it measures. Why: at 0% loss step A's L4 rows lose CPU per MB only to vendors that send 64 KB datagrams the kernel
fragments (RESULTS "rmw large messages"): subscriber 5.0-5.6 ms/MB against Fast DDS 2.35 and CycloneDDS 3.0. Fast
DDS's own two settings isolate the cause: 64 KB datagrams 2.35 sub / 4.09 pub, 1472 B datagrams 6.26 / 8.44, and
TickLE at 1472 B already beats the latter. Most of the cost is per datagram. Under loss the 97.4% failure stands: a
64 KB datagram is 45 IP fragments and arrives whole with probability `(1 - p)^45` (5%: 10%; 1%: 64%).

Scope: every sample that takes more than one datagram, through 8/9 (up to 65,507 B) and through 11/12 (stage 2).
A sample that fits one DATA is unchanged.

**Units and runs.** A sample is `k` **units** with today's geometry: the payload of an MTU fragment of its type pair,
each unit with its own seq_no. So "every datagram takes its own seq_no" becomes "every unit takes its own seq_no". A
**run** is `n` consecutive units in one UDP datagram that IP fragments into about `n` frames. `n = 1` is today's MTU
fragment. `n_max = min(45, floor(peer's advertised reassembly bytes / unit))`. 45 units fill a 65,372-B UDP payload,
which is 45 IP fragments.

**The objective and the control law (the user, 2026-10-11, second set).**

1. *Network only.* `n` minimises the wire bytes per delivered unit and the repair-latency tail; CPU and memory are not
   in the objective, and the run-time measurement of per-datagram and per-frame CPU costs (`c_d`, `c_f`, their fit
   and its `getrusage()` check) is removed. RELIABLE, repairs as MTU units:

   `Cost(n) = (u + h / n) + (1 - q(n)) (u + h)` per delivered unit, `q(n) = (1 - p)^n`,

   `u` the unit payload (1,454 B on 8/9) and `h` the bytes an MTU unit adds to it (TickLE framing and UDP, 26 B). For
   small `p`, `n* ~= sqrt(h / (p (u + h)))`; the writer takes the exact minimum over `1..n_max`:

   | frame loss `p` | 0 | 0.001% | 0.01% | 0.1% | 0.5% | 1% |
   |---|---:|---:|---:|---:|---:|---:|
   | `n*` | 45 | 42 | 13 | 4 | 2 | 1 |
   | `Cost(n*) / u` | 1.0004 | 1.0009 | 1.0027 | 1.0085 | 1.0191 | 1.0281 |
   | `Cost(1) / u`, `Cost(45) / u` | 1.0179, 1.0004 | 1.0179, 1.0009 | 1.0180, 1.0050 | 1.0189, 1.0452 | 1.0230, 1.2059 | 1.0281, 1.3707 |

   BEST_EFFORT: while `q(n) = (1 - p)^n` a sample arrives with probability `(1 - p)^k` whatever `n`, so best effort
   uses the largest `n` whose `q(n)` is not worse than the MTU baseline predicts - the reader's cap below, which is
   exactly that.
2. *Only what TickLE collects itself.* The reader measures, from its own reception, the frame loss `p` and the run
   survival `q(n)`, per writer and in total; no `/proc`, no sysctl. The reader owns the cap - reassembly memory is the
   host's, shared by every writer to it - and advertises it in `ip_reassembly_max`. The writer takes `n <= cap`
   minimising `Cost(n)` with the `p` that reader reports. **How `p` reaches the writer:** in the announce, in the
   2-byte field the removed receive costs used (`loss_p`, below), so the wire keeps its size and `tt_VERSION` stays 13
   (not yet on main). Not in the ACKNACK: best effort has none, and the cap and `p` are the reader's, not a pair's.
3. *The control law* (ideas from TCP CUBIC and BBR, not halving), run by the reader once per announce interval:
   - (a) It remembers `n_fail`, the `n` at which runs collapsed - `q(n)` far below `(1 - p)^n` by the test below - and
     on a collapse drops the cap at once to `drop(n_fail) = max(1, round(0.7 n_fail))`, which is `n_fail - 1` up to 5.
   - (b) It re-probes above the remembered ceiling only rarely, on time: `2 tau` after the collapse (doubling per
     failed probe, to `16 tau`), `tau` the reassembly-queue lifetime. Between, it grows back towards the ceiling fast
     (half the remaining distance a tick) and plateaus at `n_fail - 1`. A probe holds `ceil(1.25 n_fail)` for `tau`:
     clean, and the ceiling moves up a step (`ceil(1.25 x` the probed `n)`, none past `n_max`); collapsed, and it is a
     collapse like any other, the old ceiling standing if lower. A cap of one unit allows no run and so no window: time
     alone moves it on, to its probe.
   - (c) The collapse point scales as `1 / (R p)` (`R` the units a second reaching the reader). When `R p` has fallen
     to about half (by 1.9x or more) of its value at the anchor for three ticks in a row, `n_fail` scales up in
     proportion (at most 4x; past `n_max`, no ceiling) and a probe is allowed at once; when it has doubled, `n_fail`
     scales down and the cap with it. `p` enters only once `p_mtu` rests on 10 events at both ends; until then `R` alone
     moves it.
   - (d) Plain frame loss consistent with `(1 - p)^n` never moves the cap; only excess whole-run loss does.
   - (e) The fragment-path rule stays: every run of a sample lost while its tail arrived is a path that drops IP
     fragments, and `n` goes to 1 at once - writer side for RELIABLE, as built; reader side for every writer, new
     (below).
   The estimator stays behind the small interface `loss_estimate_observe()` / `loss_estimate_p()` and the run-survival
   window beside it; a time-varying estimator (ROADMAP, Later) replaces them and nothing else.

**What the reader measures.** From each fragment-path datagram - writer, sample, first unit, unit count, and whether
it is a run - the reader rebuilds what was sent: units `[0, k - 1)` in runs of the writer's `n` (learned from a full
run; an MTU unit at or past the next expected position says MTU), unit `k - 1` alone. A unit past the next expected
leaves a gap, which is loss; a unit below it is a repair and counts for nothing.

- `p_mtu`: lost over sent MTU units (tails, `n = 1` samples) - the frame-loss baseline, which reassembly cannot touch.
- `p` (advertised): every frame is exposure; a lost MTU unit is one event on one frame, a lost run one event on its
  `n` frames. Read as its posterior mean, `(events + 1) / frames` (a flat prior): `Cost(n)` is close to linear in `p`
  where `n` is chosen (`1 - q(n) ~ n p`), so that is the `n` of least expected wire bytes, and `n` leaves 1 as clean
  frames accumulate (`n_max` at about 115,000). A 95% bound (3 / N at no events) would hold `n` below `n_max` at 0%
  loss for as long as the window lasts. The window halves at 2^20 frames, not 2^17, for the same reason. Under a
  collapse `p` reads high, which only shortens a RELIABLE writer's `n`.
- `q(n)`: runs arrived over runs sent, with their sizes, in the window since the last decision; per writer as well
  (printed, and read by rule (e)).
- `R`: units a second in multi-unit samples, arrived or inferred lost; `D_s`: run datagrams a second from each source.

**The collapse test (pre-defined).** Over a window of `R` runs (at least 16) with `X` lost: `L0 = 1 - (1 - p_hi)^s`,
`s` the window's mean run size and `p_hi` the one-sided 95% upper bound of `p_mtu` - the most run loss frame loss
alone allows. A collapse is `z = (X - R L0 - 0.5) / sqrt(R L0 (1 - L0)) >= 3.09` (one-sided 0.1%; binomial runs at
the baseline's upper side, so an underestimated `p` errs towards "no collapse") with `X >= 4` and `X >= 2 R L0` - far
below `(1 - p)^n`, not merely below it. Without an MTU baseline `L0` is 1 and nothing is decided. A window is *clean*
when it has 16 runs and `X <= R L0`; only a clean window grows the cap, and a window that is neither gathers more (up
to 1,024 runs). The test is at the run level, not per frame, because a 1 MB sample has one MTU unit (its tail) for 17
runs: a per-frame comparison of the two loss rates could not tell a fragment path refusing every run from 2% frame
loss until hundreds of tails had come, where the run-level test calls it within seconds.

**The remembered ceiling is the collapse point, not the overshoot.** Reassembly memory fills over up to `tau`, so a
cap grown in a few ticks can be far past the collapse point when it shows. The kernel then admits about the share of
datagrams that keeps it at its threshold, and the memory offered grows as `n` (at fixed `R` and `p`), so the measured
survival says where the collapse began: `n_c ~= n q / q0`. On the PC (below): 30 MB/s, 0.1%: `n` = 8, 16, 45 give 3.7,
2.7, 3.6 and `n = 4` held; 30 MB/s, 1%: 0.3 to 0.4 and `n = 2` held; 3 MB/s, 1%: 3.0 to 3.2 and `n = 4` held;
30 MB/s, 0.01%: 28 and `n = 16` held. So `n_fail = max(2, min(n, ceil(n q / q0)))`, and the cap starts small (2) and
doubles per clean window until the first collapse (slow start). Right after the memory fills, before the kernel admits
anything again, `q` reads near 0 and the ceiling 2: the estimate errs low, and the probes climb back (on the PC at
30 MB/s and 0.1%: ceiling 2, probe 3 at `2 tau`, ceiling 4, probe 5).

**Rule (e) on the reader's side.** A writer's sample that arrives to its tail with every run of its body lost is a
path that drops IP fragments when two things hold: frame loss alone would do that with less than `1e-6`
(`L0^runs` over the writer's streak of such samples, `L0` at `p_hi`, the layout `n` the writer's or, not known yet, the
cap), and no run of that writer has got through for `tau`. Runs that did mean a path that passes fragments, and their
loss now is the reassembly memory's - which the window's test judges - so a squeeze is never taken for a firewall.
Then: one unit at once, announced at once (not at the next tick), the law frozen, and after `2 tau` (doubling while it
fails) the cap it had again as a re-probe, judged by the next such sample. A cap change makes every writer's layout
unknown again, since the writer lays its samples out anew under it.

**The queue lifetime `tau`.** On the PC an incomplete reassembly lived `min(ipfrag_time, 65536 / datagram rate)`: the
IP ID wraps and the next datagram with its ID ends it. TickLE cannot read `ipfrag_time`, so `tau = min(30 s, 65536 /
D_s)`, the longest over the sources sending runs, and 30 s (the kernel's default, the longest it keeps one) when none
is. Every wait - the hold after a collapse (`tau`, during which the full memory drains and no further collapse is
judged), the probe wait, the writer's re-probe of a failed path (`tt_HYBRID_PATH_CLEAN_NS`, 60 s = 2 x 30 s) - is
derived from it.

**Evidence.** Linux PC, two private netns over a veth, netem loss, the kernel's default sysctls (branch
`exp/frag-reasm-model`, commit 29d9a2d1, not on this branch;
`~/rig_results_safe/frag_reasm_model_20261011_full/results.txt`, no TickLE): `q(n)` matches `(1 - p)^n` while the
incomplete reassemblies stay under `ipfrag_high_thresh` (4 MB) - every 3 MB/s cell up to the predicted 4 MB - and
collapses beyond it (30 MB/s, 0.1%, `n = 8`: `q` 0.46 against 0.99; `n = 45`: 0.08); the measured argmin of the wire
cost is 45, 8 and 1 at 0.01%, 0.1% and 1% (3 MB/s); a fragment-dropping filter delivers 0 of 4,149 runs and every MTU
unit. FreeRTOS on QEMU (branch `exp/freertos-frag-q`, commit 31bf7d93): lwIP holds 8 pbufs of reassembly for 15 s, so
any loss makes `n >= 4` useless - it advertises 0.

**Settings (not QoS: delivery semantics do not change, and rmw has no such field).** Until `tt_InitOptions` exists
(ROADMAP "QoS reshaped like rmw", `f6b7d102`), the context settings live in `_tt_CONFIG` and move with the rest.

| setting | where | values | default |
|---|---|---|---|
| `large_fragmentation` | `tt_InitOptions` | `AUTO` (computes `n`), `MTU` (`n = 1`), `IP` (`n = n_max`) | `AUTO` |
| `ip_reassembly_max` | `tt_InitOptions`, advertised as a ceiling | largest datagram accepted from IP reassembly; 0 = none | Linux 65,507; FreeRTOS 0 |
| `reassembly_cap_off` | `_tt_CONFIG` | 1: advertise the ceiling uncapped (a forced arm's reader) | 0 |
| `large_fragmentation`, `ip_datagram_max` | publisher options | override, or inherit (0) | inherit |

The sender takes the more conservative of its own setting and each peer's advertisement. An undiscovered peer gets
`n = 1`, and so does a peer advertising 0. What a context advertises is also capped by its own receive buffer, so a
core-default build (1472 B) advertises 1 unit and never receives a run. FreeRTOS gets `n = 1` from the same rule:
lwIP reassembly is small (`IP_REASS_MAX_PBUFS`) and RAM-hungry, so it advertises 0 and builds without the hybrid
sender.

**Wire (`tt_VERSION` 13).** Types 2, 8, 9, 11 and 12 are unchanged. Two run types, one per geometry, so that a repair
is byte for byte the MTU fragment of its pair (renumbered 13/14 -> 14/15 on 2026-10-11 because large-message step
B took 13 for `LARGE_REF`):

- **`FRAG_RUN_L` (14)**, for samples on 11/12: 24-B header holding the whole `tt_DataHeader` (`seq_no` = the first
  unit's), then `frag_index`, `frag_count`, `run_count` (16-bit each) and 2 B of zero.
- **`FRAG_RUN` (15)**, for samples on 8/9: 20-B header holding `tt_DataHeader`, then `frag_index`, `frag_count`,
  `run_count` (8-bit each) and 1 B of zero. Received into what 8/9 use: `frag_scratch` in order, otherwise one reorder
  slot per unit, or the BE pool.
- The payload is exactly the units concatenated, so the receiver copies it to the offset `frag_index` gives. A run is
  self-describing, so a reader can acquire on it. A stored run marks `run_count` bits. The ACK bitmap, the window and
  the KEEP_ALL bound (all in units) do not change, and a lost run reads as `n` missing units, repaired as MTU fragments.
- **The last unit of a sample always goes as an MTU fragment**: units `[0, k - 1)` go in runs of `n` (the last run
  shorter), unit `k - 1` alone, and runs are used only when `k >= 3`. At `n` = 45: 1 MB is 18 datagrams instead of
  723, 4 MB 66 instead of 2,889, 16 KB 2 instead of 12. Every sample so ends in a datagram that needs no reassembly,
  which tells a reader its runs were lost and gives it the frame-loss baseline.
- **Announce:** `tt_AnnounceHeader` gains `uint16_t ip_reassembly_max` (the cap in bytes: framing plus `cap` units
  of 8/9, which reads back as `cap` units of either pair), `uint16_t loss_p` (the reader's `p`: 0 = no estimate, read
  as 1; else exponent `e` in the high byte and mantissa `m` in the low, `p = (256 + m) / 2^(e + 9)`, rounded up) and
  2 B reserved, zero. A change of the cap, or of `p` by a factor of 2, is a new generation, at most once per announce
  interval, so writers learn it from the ordinary announce.
- Several readers: by unicast (up to the link's threshold) each peer gets its own `n` from the one buffer. By
  broadcast, one transmission uses the minimum `n` over the matched peers. Repairs keep the existing addressing.
  Discovery stays broadcast and MTU-sized.
- A node without stage 2 skips 14 and counts it (`frag_large_skipped`); a node without fragmentation skips 15 as it
  skips 8/9.

#### Pre-registration (rewritten 2026-10-11 with the decisions above, before the revised code)

**Method.** Every A/B runs the parent on main against `ab/hybrid-<step>`, in ABBA blocks with equal warm-up and
cool-down, and with an A/A (parent vs parent) arm in the same session whose spread is **the floor**. A score is WIN
when better than the reference by more than the floor, DRAW within it, LOSE otherwise. Each arm prints its realized
treatment: the histogram of `n` sent per peer, units sent as MTU, the reader's `p`, `p_mtu`, `q(n)`, cap, `n_fail`,
collapses and probes. An AUTO arm at 0% loss whose median `n` is not `n_max` is VOID, and so is a forced arm whose `n`
differs from the one forced. Wire bytes per delivered unit are counted at the reader's interface (`tcpdump`, every
frame of every datagram and repair), p99 is publish-to-take latency.

| cell (cross-host, 1 MB RELIABLE at 30 Hz unless said) | criterion |
|---|---|
| 0% | CPU per MB (pub and sub) against Fast DDS and CycloneDDS - the cell step A lost; delivered 100% |
| 0.01% | AUTO beats forced MTU (`n = 1`) and forced IP (`n = 45`, which collapses: `q` 0.62 on the PC at 30 MB/s) on wire bytes per delivered unit and on p99 |
| 0.1% | AUTO, `n` about 4 to 8, beats both forced arms on the same two |
| 1% | AUTO DRAW or better against forced MTU, WIN against forced IP, on the same two |
| core P1-P4 | no regression by the gate-v2 instruction counts |
| FreeRTOS | `freertos link` passes; it advertises 0, and nothing is sent to it as type 14 or 15 |

**What would falsify the design.** AUTO loses to forced MTU at 0.01% or 0.1% (the gain is not worth the probing); the
cap collapses at 0% or at 0.01% with one writer (the collapse test fires on frame loss); a collapse is not detected
within `tau` of the memory filling (`q` stays below 0.5 for more than 30 s); or AUTO's p99 exceeds forced MTU's beyond
the floor at any loss (the repair tail of lost runs is what the objective has to bound).

**Steps** (each its own `ab/` commit and A/B, with the small-message rows on every one).

1. Option plumbing and the announce fields, `tt_VERSION` 13, types 14/15 defined and skipped.
2. Receive runs (14 into the large assembly, 15 into scratch, reorder slots or BE pool).
3. Send runs with a forced `n` (`MTU`, `IP`, or a fixed `n`), BE first: full runs plus the MTU tail.
4. RELIABLE runs with MTU repairs, forced `n`.
5. AUTO: as revised - `Cost(n)` on the reader's `p`, the reader's `p` / `p_mtu` / `q(n)`, the collapse test and the
   control law, both halves of rule (e). Tests: a simulated link with a modelled reassembly memory (a squeeze provokes
   the collapse), and a netns with `netem` loss and a real squeeze (a high rate); each rule with a mutant.
6. The rig campaign above, then RESULTS.

#### The revision as built (`ab/hybrid-network-objective`, 2026-10-11)

Built as written above, on `a240fe3c`, one change per commit: the objective (the CPU cost model, its fit and
`tt_process_cpu_ns()` removed), the reader's measurements and `loss_p`, the control law (the kernel reads
`tt_ip_reassembly_limits()` / `tt_ip_reassembly_failures()` and the `ReasmFails` withdrawal removed: core reads no
`/proc` and no sysctl), rule (e) on the reader's side. Where it departs from the first text of this revision, the text
above now says what was built: `p` read as its posterior mean (a bound held `n` below `n_max` at 0%), the collapse
test at the run level (a per-frame comparison had no power with one tail per 17 runs), the remembered ceiling without
a floor at the last clean cap (the floor made the overshoot the ceiling), a one-unit cap moving on time, and rule (e)
told from a squeeze by whether that writer's runs got through within `tau`.

- *Tests.* `tests/test_hybrid_send.c`: the objective's table, the reader's rebuilding of what was sent, the loss code,
  the collapse test's cases, the law unit by unit (slow start, the drop, the hold, the timed probe and its backoff, a
  probe that holds, scaling with `R p`, `tau`), and on a simulated link with the kernel's reassembly modelled (4 MB,
  30 s): frame loss at 1% never moving the cap, a fragment-dropping path brought to one unit and re-probed, a squeeze
  detected, dropped, not probed before `2 tau` and rescaled when the rate halves. `tests/mutants_hybrid_send.py`:
  110 mutants, one per rule; 109 caught in the final sweep and the one that survived (the path's re-probe restoring
  its cap) caught since by its own test.
- *Real IP reassembly* (`hybrid_send_netns.sh`, PC, veth, the kernel's defaults, `hybrid_law_log.py` reading the
  reader's log by rules written before the run): best effort 1 MB at 30 Hz through 0.1% loss collapsed at `n = 45`
  (504 of 510 runs lost), the cap went at once to 1 (ceiling 2: the transient reading), the first probe came at
  `2 tau` = 60 s and held, then 5; with the rate halved at 55 s the ceiling went 6 -> 12 (R p fell 2.0x). A filter
  dropping every IP fragment was judged after 4 samples, 356 of 360 delivered as MTU units. 0%: best effort doubled
  2 -> 45 in 5 s, RELIABLE reached 45 in 6 s. The whole suite, 19 cells: 18 PASS; REL_4MB_IP_5 (forced IP, cap off -
  not the law) lost sample 0 once (179 of 180) and passed in two reruns.
- *What it does not settle.* The transient ceiling reads low (2 where the PC's steady state says 4 to 6) and the
  probes climb back at `2 tau` steps; RELIABLE at the pre-registered losses chooses `n*` (13, 4, 1) at or below the
  PC's collapse points, so the law guards best effort and forced arms more than it moves RELIABLE AUTO. Per-writer
  `q` is measured and printed, and read only by rule (e). The rig campaign (step 6) is not run.
- *Found on the way.* `make sanitize` failed at the parent on three tests' own leaks (retained large buffers at a
  test's re-initialisation; 4.2 MB, 16.8 MB, 71 KB); fixed in the tests.
#### Steps 3 to 5 as built (2026-10-11), and where they depart

Superseded the same day by the revision above: the CPU cost model and its run-time fit, the receiver's advertised
costs, the writer's NACK-counted `p`, the cap from `ipfrag_high_thresh` and the `ReasmFails` withdrawal. The bullets on
them below are the record of why they were tried and what they showed.

Built as designed: runs of `n` units from three units up, the last unit alone in a send call of its own; `n` per sample
and destination (by unicast each peer's, by broadcast the minimum over the matched peers), 1 for an undiscovered peer,
one advertising less than two units, a same-host peer reached through its segment, and every repair and backlog; best
effort `n_max`, RELIABLE AUTO the exact minimum of `C(n)` under the reader's cap; the 95% Poisson bound (exact to 10
events, Wilson-Hilferty above) behind `loss_estimate_observe()` / `loss_estimate_p()`, the one interface a time-varying
estimator replaces; sums halved at 2^17 frames; the cost fit judged every 8,192 frames (64 calls, share error under 0.1,
within 2x of `getrusage()`), the platform default otherwise (`tt_HYBRID_TX/RX_DATAGRAM/FRAME_NS`: 6.25/5.6 and
5.6/3.2 us, from the rig's Fast DDS pair); the receiver's accepted costs in its announce; the reader's cap quantised
1/2/4/8/16/32/`n_max`, a change announced at once as a new generation; `ReasmFails + ReasmTimeout` read once per
announce interval, released after 2T still. Tests: `tests/test_hybrid_send.c` (with `tests/mutants_hybrid_send.py`,
79 mutants) and `examples/perf_hil/experiments/hybrid_send_netns.sh` (15 real-IP cells, receiver-side captures read by
`hybrid_pcap.py`). rmw_tickle reads `RMW_TICKLE_LARGE_FRAGMENTATION`, `RMW_TICKLE_REASSEMBLY_CAP_OFF` and
`RMW_TICKLE_RUN_UNITS` for the rig's forced arms.

Where it departs, and why:

- **Re-NACKs are not events, repairs are not exposure.** Counted as written, a veth at 1% read p near 4% for forced IP
  (38 events a 1 MB sample against ~9): repairs queued behind a large sample's runs are re-NACKed while still in flight,
  and the ACKNACK alone cannot tell that from a lost repair. The estimate is the loss of first transmissions, which is
  what `n` is chosen for. Every first transmission to a RELIABLE reader is exposure whatever its `n` (counted only when
  runs were possible, a capped reader's NACKs read 3.4% on a 1% link).
- **U is the units sent to the reader as it sees them** (arrivals and gaps), not arrivals: with arrivals alone a path
  that loses every run reads as no load and keeps `n_max`.
- **The withdrawal fires while runs could arrive** (units came, and the advertisement allowed runs), not on whole runs
  received: a run that lost a fragment never reaches the process, so on the path the withdrawal is for it never fired.
- **The cost fit weights each call by 1 / f^2** (its relative error). Unweighted, a 45-frame call's noise buried c_d,
  which only the one-frame tails measure: share error 0.13 at 5% noise against 0.005.
- **The receiver advertises the platform default, and says why.** Its fit times a datagram from the start of its
  processing, not the receive call (recvmmsg() has none per datagram), and leaves out the datagram that completes a
  sample (its time is the delivery: counted in, every sample's tail read as one frame costing a decode and a callback,
  and the fit gave c_d 128 us and c_f -5.6 us). What remains is consistent (veth, 1 MB RELIABLE at 1%: c_d 0.8 us, c_f
  0.56 us, share error 0.01) and still only 27% of the process's CPU time - the receive system calls, the kernel's copy
  and softirq reassembly are outside it - so the 2x check refuses it every time, logs that once ("receive cost fit not
  used ... predict 27% of the process's CPU time"), and the announce carries 0: the writer uses tt_HYBRID_RX_*. The
  sender's fit, timed around the send call that does the kernel's work, passes.
- **A forced arm's reader needs `reassembly_cap_off`** (no cap, no withdrawal): forced IP is `n_max`, the reader's
  advertisement, which holds the cap. At 1 MB, 30 Hz and 1% that cap is 1 (this section's own figure), and on a veth the
  incomplete reassemblies passed `ipfrag_high_thresh` within a second, so the withdrawal made a forced arm MTU too.
- **I1L1 as pre-registered cannot be won at the kernel's defaults.** At 1 MB, 30 Hz, 1% the reader's cap is 1, so AUTO
  is the MTU arm (on the veth: 211 of 240 samples at `n` = 1) and "AUTO WINs against forced MTU" can only DRAW. The cap
  allows `n` = 8 (`C(n)`'s choice) at `ipfrag_time` 1 s, or at about 1/30 of the rate. A decision for the campaign.
- **A path that drops every IP fragment is judged as such, not as loss.** By `p` alone it never reached 1: a lost run is
  one event on `n` frames, so "every run lost" read as frame loss near 1/n, n fell 45, 17, 11, 8 ... 2 over 25 samples,
  and on the veth RELIABLE ran at 84 of 240 samples' rate and ~20x the CPU. So the writer watches each sample it sends
  a reader in runs: every run first-NACKed while the tail (always an MTU unit) was not is a failed path - on a sample
  of two runs or more, a second single-run sample in a row, or a re-probe - and that reader gets MTU units only for
  `tt_HYBRID_PATH_CLEAN_NS` (60 s, the withdrawal's 2T), then one sample re-probes with a single run: through clears it,
  lost fails it again. Under frame loss at the `n` C(n) picks, every run of a sample lost with the tail through is
  L(n)^runs - not seen in 40 samples at 5%. veth, tc filter dropping every fragment, 4 interleaved reps: AUTO sent one
  sample in runs and 239 as MTU, 240 of 240 delivered at 30 Hz, CPU 3.95 / 2.83 ms/MB (pub / sub) against forced MTU's
  3.94 / 2.81 and 3.72 / 2.63 (its A/A twin, 6-7% apart).
- **PC evidence, not rig figures** (`hybrid_pc_ab.sh`, veth, 5 reps, A/A floor 1-3.5%): instructions per MB against
  the parent at 0% - 1 MB pub -14.1% / sub -11.2% (BE), -14.3% / -9.4% (RELIABLE); 4 MB -12.2% / -16.4% (BE), -12.5% /
  -8.7% (RELIABLE). Two results against the expectation: 4 MB best effort delivered 88.4% of samples against the
  parent's 97.8% (66 reassembled datagrams a sample overrun the receive buffer, which neither `p` nor `ReasmFails`
  sees), and 4 MB RELIABLE at 1% cost +1.6% pub / +1.8% sub (n mostly 1 under the cap, with ramps that lose runs).
  The sender's fit was accepted on the PC (c_d 3.7-6.0 us, c_f 1.5-1.8 us); the receiver's never (above).
  Core P1-P3 binaries are bit-identical to the parent's (fragmentation compiles out); P4's instructions held.
- **GSO is not combined with runs**: a run is one datagram that IP fragments, the opposite of segmentation; the two
  meet only in that a sample's MTU units (n = 1, repairs) go by GSO (section 4, landed with this on `ab/hybrid-land`).
#### Saturation: where I4s lost its samples, and the receive drain (2026-10-11)

RESULTS L4 cell I4s (4 MB `sensor_data` at 30 Hz, about 1.05 Gb/s offered on the rig's point-to-point 1 GbE) was a
LOSE: rmw_tickle published 1,802 samples a run and its subscriber completed 1,655-1,679. **Where they went** (A4 s3a's
own counters, all 5 runs, `~/rig_results_safe/largemsgA4_s3a_20261010-140102.runs`):

- **109 a run at the sender, whole.** The link carries ~28.2 samples/s, so each newer publish replaced the one waiting
  behind the send in progress (`large_pending`) before any fragment of it went out: `tail_abandoned` 109 in every
  run, and published - tail_abandoned = the subscriber's reassembled + abandoned (1,693) in every run. Dropping a
  superseded sample whole rather than a partial one is what the code already does; pacing cannot help a sender that
  is already link-bound (`send_waits` ~302,000 a run).
- **14-38 a run at the subscriber**, from 679-1,236 data datagrams sent and never received (publisher
  `tx_udp_unattached` against subscriber `rx_via_data`), in bursts of ~38: one lost fragment loses its sample.
  There is no switch. The run recorded no kernel counters, so the location was inferred, then reproduced.
- **Mechanism.** rmw decodes a 4 MB message inside the poll callback (`subscriber_callback()`), and the application
  copies it again after the poll: milliseconds away from the socket while the next sample arrives at line rate. The
  Pis' `net.core.rmem_max` caps the receive buffer at 425,984 bytes, ~2 ms of 1 GbE. `rx_drain_pc.sh` (two netns,
  tbf 1 Gbit, the same buffer, a receiver that stops after every 2,889 datagrams): a 3 ms stop loses 2-3% of
  datagrams, 5 ms 7%, 10 ms 18%, and every lost datagram is in the socket's own drop count (SO_MEMINFO). The DDS
  vendors read the socket from a thread of their own; TickLE has none (the user's rule).
- **A residual the drain cannot reach:** the wire. Each 1,472-byte fragment carries its own UDP/IP and TickLE
  headers, so the link carried 1,693 of TickLE's samples a run where CycloneDDS, with kernel-fragmented 64 KB
  datagrams, delivered 1,699-1,700. That ~0.4% is the hybrid IP mode's question (Dev, 2026-10-11), not this one.

**The receive drain** (`tt_rx_drain_start()`, hal.h; Linux, compiled where io_uring and `tt_LARGE_SAMPLES` are).
While core is away from the sockets during a large stream - just before a large sample's callback, and when a poll
that delivered one returns - an io_uring multishot `recvmsg` on each socket copies arriving datagrams into buffers
the HAL owns. The kernel does it as task work of the polling thread, which runs whenever that thread is interrupted
or enters the kernel, so a thread busy in user space still has its socket emptied: no thread of ours. The buffers
are sized from the datagram core expects (2,048 x 1,536 B for fragments, 64 x 65,600 B for kernel-reassembled 64 KB
datagrams; ~4.25 MB, allocated at the first drain). `tt_receive()`/`tt_try_receive()` hand out what the drain took,
oldest first, while it keeps taking, and stop it only when it holds nothing, before any socket read: order is kept.
A datagram longer than a buffer is dropped, counted, stops the drain and holds drains off for 100 ms, so its
sender's retry is read whole. A kernel that refuses io_uring or multishot `recvmsg` (before 6.0) leaves the context
as it was, said once. FreeRTOS: no drain. The default core build (p1-p4) is instruction-identical in `tickle.o` and in
`tt_receive()`/`tt_try_receive()` to its parent: the drain exists only in large-sample builds. Its limit: it drains
only while the thread runs - a preempted thread overflows as before, as a vendor's descheduled receive thread would.

**PC evidence, end to end** (`large_saturation_pc.sh`: `large_sample_bench` 4 MB best effort at 30 Hz through tbf
1 Gbit, buffers granted 425,984 B, the subscriber spending 0/3 ms in each callback and 0/5 ms after each delivering
poll; ABBA, 3 x 20 s; `~/largemsg_pc/saturation{1,2,3}.txt`). First run (load ~7, drain started at every poll exit,
33fb84bb against d99f9a7e): complete samples 285 -> 560 a run in every cell, subscriber CPU per MB -31..-34%, but the
pre-registered fix rule (no socket drop in 2 of 3 runs, abandoned at most 10% of the parent's) FAILED in all three
stalled cells: 0-17,541 drops remained. Two causes were found and fixed: the first read stopped the drain while core
still had up to 2,048 taken datagrams to work through (now it drains until it holds none), and starting at every
poll exit cost ~50,000 starts a second (now only after a poll that delivered a large sample). The runs after those
fixes (615204e9, alone and beside 33fb84bb) ran at load 13-38 from builds in other sessions: the parent's control lost
up to 86,674 datagrams with no stall at all, so they decide nothing. Torn and out-of-order samples: 0 in every run.
**The PC cannot settle this; the rig run below does.**

**Instruments.** The HAL's "Node N rx:" line at close: `socket_drops` per socket (SO_MEMINFO) and the drain's
starts, datagrams, cut datagrams and overruns. `rmw_keepall_rig.sh KCOUNTERS=1` adds each Pi's snmp, NIC, qdisc and
softnet counters before and after every run.

**Rig pre-registration** (implemented in `experiments/i4s_drain_summary.py`, not prose): arms tickle@pre (the parent,
with the rx line and no drain) and tickle@head, beside fastdds and cyclonedds; cells I4s and the unsaturated controls
I4 (Array4m 15 Hz) and I1 (Array1m 30 Hz), best effort, 5 reps of 60 s, KCOUNTERS=1. W0 the treatment shows (drain=1,
starts > 0 on head only; a Pi kernel that refuses is VOID). W1 tickle@pre's I4s losses are placed - SOCKET when the
sender is accounted, every abandoned run has socket drops, and NIC and qdisc drops are 0; anything else says the drain
is not the whole answer. W2 head completes what the wire carried (reassembled >= published - tail_abandoned - 2) with
no socket drop in at least 4 of 5 runs. W2b the best DDS against head, reported, with the wire count beside it. W3/W4
latency median and CPU per MB of head within 2 SE of tickle@pre (the drain should move neither; I4s's queueing
latency against DDS, ~57 ms against 37.5, is the latency session's). W5 the controls complete in every run.
**Falsified** if W1 places the loss in the NIC or the qdisc, or if W2 fails with the treatment shown: then the
socket was not where the samples went, or the polling thread is not running when they arrive (a preempted thread),
and the next candidate is a kernel-side reader (io_uring SQPOLL) - which spends a core.

**The drain with the hybrid transport** (2026-10-12, `ab/hybrid-drain`: the drain's commits on `ab/hybrid-land`). The
rig run of `ab/hybrid-land` alone at 4 MB and 0% loss overflowed the subscriber's socket (RcvbufErrors 58-148 a
best-effort run), and its RELIABLE runs were VOID: AUTO's n stayed at 4-31, not 45, because the reader counted the
socket's drops as path loss. Three rules join the two:
- **Buffer length.** A run is one IP-reassembled datagram of up to 65,507 bytes, so core asks the drain for the
  reassembly ceiling it advertises (`large_rx_drain()`), never a fragment's length: a run is never cut
  (`drain_truncated` stays 0). A reader that advertises no ceiling asks for a fragment's. The cost: an MTU-mode stream
  into a run-capable reader has 64 buffers of 64 KB, not 2,048 of 2 KB - unless receive offload packs it (next).
- **Receive offload.** Only core's own socket reads move UDP_GRO's state, and a settle is not decided while a drain
  runs or holds datagrams; so the option stays as the drain found it. Its recvmsg asks for UDP_GRO's size cmsg and
  hands a merged read out one datagram at a time, in order; unless the sockets read plain its buffers hold a merged
  read whole (`hal_linux.c`, "The drain and receive offload"; `rx_drain_check` 6).
- **An overrun does not end the drain.** A recvmsg that found every buffer held ends (ENOBUFS); it was left so until
  the next start, the socket's few runs taking the rest. Now it is submitted again once an eighth of the buffers is
  back (`drain_rearms`; `rx_drain_check` 7). Not at the first: a recvmsg picks its buffer before it looks for data, so
  one buffer free is one datagram and another ENOBUFS. And a drain that overran has twice the buffers at its next
  start, up to `tt_RX_DRAIN_BYTES_MAX` (4x, 256 run-sized buffers). A buffer holds one datagram, and a run is cut where
  the sender's send buffer filled: saturated at 4 MB, the PC's stream came as ~180 runs a sample of ~15 units
  (`hybrid_n_hist` says n = 45; `hybrid_runs` / `hybrid_run_units` say what went), so 64 buffers held ~12 ms of it.
- **Receiver drops are not path loss.** A datagram dropped by the receiving host - a full socket, or cut by the drain -
  is a receiver that fell behind; a smaller n does not cure it. The HAL's count of them (`tt_rx_receiver_drops()`,
  hal.h) is put against the gaps the reader sees, one drop per lost datagram; those gaps reach neither p, p_mtu, the
  run window nor rule (e), and are counted apart (`hybrid: receiver_drops= receiver_lost=`). A drop is counted before
  its gap is seen, so none is missed; one unmatched for two announce intervals is let go. Not covered: the writer's
  RELIABLE failed-path watch reads NACKs, which a receiver drop causes too; it needs every run of a sample lost with its
  tail through, which a drained socket does not produce. Tests: `test_hybrid_send` (the attribution, and RELIABLE 1 MB
  with every tenth run dropped at the socket - n reaches 45 counted, stays below uncounted), `mutants_hybrid_send.py`;
  the PC A/B is `experiments/hybrid_drain_pc.sh`.

**PC evidence** (`hybrid_drain_pc.sh`, 2026-10-12, 3 reps x 20 s a cell, 4 MB at 30 Hz, AUTO, 0% loss; PARENT is
`ab/hybrid-land` without the drain, HEAD this branch with the growth; `~/largemsg_pc/hybrid_drain/results_v3_*.txt`):

| cell | PARENT delivered / socket drops | HEAD delivered / socket drops | HEAD n |
|---|---|---|---|
| best effort, no stall | 295-297 / 1,408-1,788 | 578-580 / 34-187 | 45 (cap 45) |
| best effort, 3 ms + 5 ms stall | 290 / 10,243-10,619 | 577-579 / 145-243 | 45 |
| RELIABLE, no stall | 569-570 / 3,261-4,789, n 1-3 | 580 / 0 | 45 |
| RELIABLE, stalled | 502-503 / 28,265-33,616, n 1 | 579-580 / 125-140 | 45 |

Every HEAD drop was put down to the receiver (`receiver_lost` equal to `socket_drops`), so n reached 45 in every run;
PARENT's RELIABLE n stayed at 1-3, the rig's failure. Torn and out-of-order: 0. Subscriber CPU per MB: -54% / -29%
best effort, -5% / -2% RELIABLE (PARENT's losses cost it reassembly work). Pre-registered D1 (no socket drop at all)
FAILED. Most of what is left comes before the buffers have grown: one stalled run built with 256 buffers from the
start dropped 2 (an instrumented scratch build). A forced-MTU arm with receive offload (64 buffers, merged reads) had
no drop in any cell. The rig run (`~/rig_queue_hybrid_drain.sh`) decides.

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
- **The header is the authority, not the name.** It carries magic, `tt_SEGMENT_VERSION` (6), the owner's
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
- **A writer never uses a segment whose owner is dead.** The owner holds an exclusive `flock()` on its file for as long
  as its region is mapped (the mapping holds the lock, so a crash releases it); `tt_segment_attach()` refuses a file
  nobody holds (`tt_SEGMENT_ORPHANED`, `shm_attach_orphaned`) and the peer is reached over UDP until its owner builds
  a new one. A killed context's file has a header that still names it, so before 2026-10-09 a writer asking in the
  gap attached to it and wrote into a ring nobody drained (~2% of `test_loaned_messages` runs). One `flock()` per
  attach and per revalidation, none per datagram. Writers do not unlink an orphan: `unlink()` cannot be made
  conditional on which file the name holds, and a successor's file could go instead. The bell needs no check of its
  own (it is opened only after the segment's), and the context-id registry already tests its holders' pids.
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

**The switch from UDP is fenced.** A writer that reached a peer over UDP (its segment absent or still being built,
or given up on) and then attaches has datagrams still queued in the peer's socket, and the reader drains its ring
first: it delivered the first ring record and discarded the queued BEST_EFFORT samples as older (1-224 a run in
`test_thread_safety`, 2026-10-11). So:

- On that attach the writer sends a 3-byte fence (marker, context id, generation) to the peer's data socket, behind
  everything it sent there, and puts the same bytes in the ring as a record with `seq_span = tt_SEGMENT_SPAN_FENCE`
  before its first datagram there (`tt_SEGMENT_VERSION` 6). A revalidation, or a first attach, sends none.
- The reader does not read past a fence until the fence's datagram has come out of its socket; until then the drain
  stops there as on an empty ring, so the socket is read next. A datagram read first is remembered
  (`tt_SEGMENT_FENCES_SEEN`). One never seen is given up at `tt_SEGMENT_FENCE_HOLD_NS` (1 ms); a scheduler entry at
  that deadline bounds the sleep taken while a fence is held, so no sleep tests for one. Counted as `shm_fences_sent`,
  `shm_fences_passed`, `shm_fences_expired`.
- The fence is tested inside the span range check's failing branch, on the drain's stall path and, for its datagram,
  where `process_packet()` finds no header, so a datagram or a record costs nothing for it; only an attach that
  follows UDP does. Its fields sit at the end of `tt_Context`. `tests/test_segment_switch.c` stages the switch
  deterministically (an attach refused until released, a mock clock): the core before the fence delivered 50 of 307,
  discarding the 257 that went over UDP; with it, 307 of 307. In `test_thread_safety`'s stress
  (`fence_stress_ab.sh`, 600 runs an arm, 4 in parallel, on the PC): 24 runs of 600 discarded (2,922 samples) on
  `2c108730`, 0 of 600 with the fence, every one of which passed a fence
  (`~/rig_results_safe/fence_stress_ab_20261011_run2.txt`).
- Cost, measured on the PC against `2c108730`: `fence_ring_insn.sh` (instructions per record on the mock HAL, 10
  rounds) +0.000 for a ring write and +0.125 of 209 for the drain, control unmoved. `fence_samehost_insn_pc.sh`
  (same-host p1-p4, best-effort throughput and latency, A P B B P A): no metric WORSE, but at a resolution of the
  placement floor, up to 12% per sample on the max-rate cells; its arm B sent 49 fences at start-up, all passed - a
  publisher whose subscriber's segment is not built yet is a switch from UDP too. The rig decided otherwise: gate v2
  same-host (`~/rig_results_safe/ab_samehost_segment_fence_20261011-150800.summary.txt`) read the first fence commit
  (measured as ab/segment-fence 2587915b; landed as `e518b00e`) against `5dd8b35c` +20..+27 user instructions a round trip on reliable_latency p1-p4, client and server, and +2..+3 a sample
  on best_effort_throughput. Counted exactly on aarch64 under qemu (`aarch64_insn/samehost_qemu.sh`, lat_p1 client
  4850 a round trip): the fields inserted mid-`tt_Context` moved every later field, and aarch64 reaches a far one with
  an extra `add`; `process_datagram_in` +9 (that and its length test), `tt_Context_poll` +6..+14 (the per-sleep
  deadline test). Fixed by the three points above: lat_p1 client B-A +18.6, fixed-A -0.6 (A/A 1.5); server +9.7 and
  -1.9 (A/A 0.7); lat_p4 fixed-A -15.8 and -4.3; no function above A
  (`~/rig_results_safe/aarch64_insn/fence_ABC_final2/summary.txt`). The max-rate cells under qemu are bimodal (the
  share sent over UDP while the ring is full) and resolve nothing at this size. `qemu_tb_count.c` counted every block
  as one instruction under a qemu built without a disassembler, which Ubuntu's 10.2 is; the figures before this one
  from `aarch64_insn/` are block counts. The fixed fence on the rig (gate v2 same-host, 2026-10-12,
  `~/rig_results_safe/ab_samehost_segment_fence2_20261012-072644.summary.txt`, measured as ab/segment-fence-v2
  8de4100d against `5dd8b35c`): still WORSE by the gate's rule, but at +0.03..+0.13% user instructions (a few a sample
  or round trip) instead of +0.2..+0.5%; landed on the user's decision as a correctness fix (start-up BEST_EFFORT loss
  1-224 samples -> 0).

**Doorbell: a FIFO, rung once per reader sleep.**

- Shared memory cannot end a socket wait, so before blocking the owner writes a new, never-zero **sleep generation**
  into `reader_waiting` and drains once more; it writes 0 on waking.
- A writer reads `reader_waiting` after publishing a record, and each side puts a full fence between its store and
  its load, so no record can sit unseen. Without the writer's fence x86 let the load overtake the slot's release
  store and lost about 3.5 wake-ups per 100,000 round trips (`platform/linux/bell_wake_check.c`; since encode-in-slot,
  cd09e895, the race shows only when the check aims at the announcement and clogs the writer's stores). If it is
  non-zero and differs from the last generation it rang for this peer, it rings once.
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
- Before announcing a sleep the reader may wait instead, at most what a sleep has cost it: for a head record still
  being filled, and for the next record when records have been arriving faster than a sleep costs. Whether those
  waits are on is measured, not assumed: per epoch of one ring's worth of records, the writers' pace plus the
  reader's own CPU time per record, with the waits on and off; the cheaper mode is used (by more than 2 standard
  errors) and the other re-measured at a doubling interval up to `tt_SEGMENT_PROBE_EVERY_MAX` epochs. Waiting keeps
  the reader at its writer's heels, which saves doorbells and sleeps where moving a cache line between the two cores
  is cheap (the rig's Pi 5) and slows the writer where it is not (PC vCPUs ~260 ns apart).
- Measured on `e17b4e6f`: 1.42x p3 BEST_EFFORT throughput over the UDP doorbell, and the same-host p2 round trip now
  34% faster than the kernel path (0.030 vs 0.046 ms at 200/s). Details in [RESULTS.md](RESULTS.md).

**What the ring cannot do.** It cannot serve as the RELIABLE retention cache: the memory belongs to the receiver and
delivery frees the slot. Retention stays in the writer's own cache.

**Receive-buffer lending (`tt_Sample_retain` / `tt_Sample_release`, SHM stage 2).** A subscriber callback may keep
the sample it was handed beyond the callback, without copying it, until it gives it back.

- **API.** Inside a Subscriber's callback, `tt_Sample_retain(sub, &sample)` fills a `struct tt_Sample` (the sample's
  CDR as received: `payload`, `length`, `is_native_endian`, and an opaque non-zero `handle`) whose bytes stay valid
  until `tt_Sample_release(ctx, &sample)`. A `data_decode_inplace` topic's `tt_Data*` aliases those bytes, so it stays
  valid too. Outside a callback, or for another Subscriber than the one being called, retain is `tt_RET_ILLEGAL_STATUS`.
- **What is lent.** Only bytes that sit in the datagram they arrived in:
  - the segment path: the record's **ring slot**. With lending compiled in, the drain processes a record in its slot
    instead of copying it out first (the copy stage 1 kept), and a retained slot is left unreleased - `read_index`
    moves past it, its `sequence` does not - until `tt_Sample_release()` frees it with `index + slots`;
  - the socket path: the **receive buffer**. The context's inline `rx_buffer` and a caller-attached pool
    (`tt_Context_set_rx_pool()`, `tt_RX_POOL_BUFFER_BYTES` each, at most `tt_RX_POOL_MAX`) form one set; the socket
    reads into one of them, and retaining a sample there hands the next datagram a free buffer instead. No pool, or
    every buffer held: retain fails with `tt_RET_OUT_OF_BUFFER` and the caller copies, as it would have without
    lending (`lend_exhausted`). Every one of them is 8-aligned, so a DATA alone in its datagram has its payload at
    4 mod 8 and a struct with 64-bit members behind a 4-byte prefix (rmw_tickle's psn) lands aligned for it
    (`test_sample_lending`, a 64-bit-aligned type read in place from `rx_buffer` and from the pool).
  - Several samples of one datagram (a batch) may be retained; the slot or buffer goes back when the last is released.
- **What is not lent: `tt_RET_UNSUPPORTED`, and the caller copies** (`lend_unlendable`). A sample assembled from
  fragments (the best-effort reassembly slots, the RELIABLE `frag_scratch` fast path), one released from a reorder
  buffer, and one delivered locally or from a backlog: each already sits in a copy core made into its own working
  storage, so lending it would save only the application's copy and would pin storage the receive path needs
  (`frag_scratch` is one per context). The test is the payload's address: lendable exactly when it lies in the
  datagram being processed. Large samples get contiguous lendable storage with large-message stage 2.
- **Handles.** At most `tt_SAMPLE_RETAIN_MAX` (16) samples are held per context, each in a handle-table entry whose
  handle carries a generation. A release of handle 0, of a handle the table does not hold, or of one already released
  is refused with `tt_RET_INVALID_ARGUMENT` and counted (`lend_bad_releases`); it never touches a slot or a buffer.
  The table full is `tt_RET_OUT_OF_BUFFER`.
- **Threads.** Retain runs on the delivering thread, under the state lock it already holds. Release may come from any
  thread: it takes the state lock, so it never interleaves with the drain. A release of the record still being
  processed (retain and release in one callback) leaves the slot to the drain, which frees it after the whole record
  (a batch) is read. `read_index` moves past a record when it is taken, as with the copy; only the slot's `sequence`
  waits.
- **Read in place.** A record is processed from memory its writers can map. A writer never touches a published record
  before its release; one that does is a same-user process with write access to the segment (its permissions), so
  already trusted as much as the record's content.
- **A held slot blocks only its own ring, one lap later.** Writers claim slots in index order, so the ring keeps
  running past a held slot until a writer's claim reaches it again, one lap (`tt_SEGMENT_SLOTS` records) on; from
  there that ring is full for every writer into it, with the existing full-ring behaviour: the datagram is dropped
  and counted (`segment_full_dropped`, and the writer counts `segment_full_retained` when the slot it was refused is
  one its reader has read and still holds), and after `tt_SEGMENT_DEAD_READER_NS` the writer falls back to UDP until
  its next recheck. Other contexts' rings and every socket are unaffected. **So hold a slot for less than one lap of
  the ring at the stream's rate** (512 slots at 100k records/s is 5 ms); a sample needed longer is copied, or the
  ring sized for it. Skipping a held slot would need a new ring protocol (`tt_SEGMENT_VERSION`), which this stage
  does not take.
- **Skip-to-newest never frees a held slot.** The KEEP_LAST drain (above) releases only slots at or after
  `read_index`; a held slot is behind it. Same for the stall check: a held slot is never the head.
- **Lifetime.** The lazy segment release (last same-host peer gone) waits while a slot is held and runs on the
  polling thread's next summary tick after the last release. `tt_Context_destroy()` invalidates every held sample: the
  segment is unmapped and every handle is forgotten, so a later release is refused rather than written into unmapped
  memory. Pool storage is the caller's and must outlive the context, like every other attached storage.
- **No cost when off.** `tt_SAMPLE_LENDING` compiles it in (1 on Linux, 0 on FreeRTOS, where it works on the pool
  alone: no segment). At 0 the receive path is stage 1's. At 1 and unused it costs a few stores per datagram and
  per delivery, and saves the segment drain's copy of every record; its A/B against main is in
  [RESULTS.md](RESULTS.md) once measured. Counters (`lend_*`, `shm_full_retained`) are on the traffic line.

**Claimed-slot publish (`tt_Publisher_claim` / `_publish_claimed` / `_abandon_claim`, 2026-10-09).** The send half of
zero-copy: a caller builds a sample's CDR in the subscriber's ring slot, and the publish copies nothing. It is
encode-in-slot (`try_publish_into_slot()`) split at the encoder: the claim is everything before the encode, the
publish everything after, and the caller's fill is the encode.

- **API.** `tt_Publisher_claim(pub, capacity, &payload)` claims a slot with room for `capacity` CDR bytes; `payload`
  is at 4 mod 8 (an 8-aligned struct behind a 4-byte prefix). `tt_Publisher_publish_claimed(pub, length)` sends the
  first `length` bytes; `tt_Publisher_abandon_claim(pub)` gives the slot back. A claim that cannot be made says why:
  `UNSUPPORTED` (no slot for this shape: not a lone DATA to one attached same-host peer - broadcast, several or remote
  peers, local subscribers, batching, a Heartbeat due alongside, larger than a slot or a datagram), `OUT_OF_BUFFER`
  (ring full), `WOULD_BLOCK` (KEEP_ALL), `ILLEGAL_STATUS` (one claim out already). Each means "publish ordinarily",
  which then behaves as it always did.
- **Rules.** One claim per publisher, and while it is out that publisher's ordinary publish is refused
  (`ILLEGAL_STATUS`): the claim takes its seq_no at its publish, so a sample sent in between would sit behind it in
  the ring with an earlier seq_no, and the reliable cache keeps seq order. A claimed slot stops its ring for every
  writer until it is published or abandoned (the reader takes records in index order); hold it for the fill only.
  KEEP_ALL is asked at the claim; nothing the claim holds back can unblock it later. Spent whatever the publish says.
- **The destination is asked twice.** Discovery can move the peer, add a local subscriber or owe a match Heartbeat
  between claim and publish. The publish asks the same rule again; if it no longer holds, the bytes are copied out of
  the slot and published the ordinary way, and the slot goes back empty - the reader takes the empty record, then the
  copy (`shm_claims_copied`).
- **The mapping is pinned.** A peer mapping with a claim on it (`tt_SegmentPeer.claims`) is not unmapped by the
  revalidation or the dead-reader rule while the caller writes into it; `tt_Publisher_destroy()` abandons an
  outstanding claim, and so does `tt_Context_destroy()`, before it unmaps the mapping - otherwise the claimed index
  stops the peer's ring for good and a later abandon writes into the unmapped page.
- **RELIABLE** still copies the record into the writer's retention cache at the publish, as every publish does.
- Counters `shm_claims_published`, `shm_claims_copied`, `shm_claims_abandoned` are on the traffic line.
  `test_publish_claim` holds a claimed publish byte-identical to the encoder's (ring records, cache, seq_no) and checks
  every rule; `tests/mutants_publish_claim.py` removes each step (19 mutants, all killed).

**What the module costs where it is not used (stage 1 / S1, 2026-10-09).** A PC measurement, not a rig one:
`examples/perf_hil/experiments/stage1_payg.sh` (reading rules in its header), RELIABLE KEEP_ALL p1-p4 between two
private namespaces over a veth with a private `/dev/shm` each, so every datagram crosses the link; one commit
(`2940410c` and the fix on it) built five ways, 5 rotated repetitions, five runs. CPU time and rate could not be
resolved (load 14 on 16 cores; the alignment control alone moved CPU per sample 3-63%), so work is read as
instructions per sample (perf, user + kernel) and memory as resident pages split into code, stack and the rest.

- **Compiled out** (`tt_SEGMENT_ENABLED=0`, `tt_SAMPLE_LENDING=0`): 560 B of counters and drain-plan state stay in
  `struct tt_Context` (60 fields, listed by the script), with their reset and the traffic line. No build that never
  had the module exists to run against, so this is an inventory, not a measurement.
- **Lending compiled in, unused**: +2.0 KB of library text, +480 B of context; instructions per sample within 0.8% on
  both sides (inside the 1% floor) and no RSS difference that repeated.
- **Segment and lending compiled in, every peer on another host**: +25 KB of library text (resident as +24 KB of
  code) and +15.4 KB of context, resident in the bench because its context is on a stack gcc probes page by page.
  The publisher paid +790-950 user instructions a sample (+5.8-6.7% of all its instructions at p1-p3): the
  encode-in-slot path ran its destination, size and KEEP_ALL checks and a `peer_segment()` for every publish before
  finding no segment, then the staging path ran them again, which also asked `/dev/shm` about the remote peer twice
  as often. Fixed: a context that has never attached a segment declines the slot path in one load
  (`segment_slot_ceiling == 0`); and the 14 KB peer table is zeroed an entry at a time on first use rather than at
  reset, so in zero-allocated memory (rmw_tickle's context) the rest of it never becomes resident (mincore() test).
  After the fix the publisher is 3.4-3.8% fewer instructions at p1-p3 (0.8% at p4) and the module still costs
  +1.7-3.1% of its instructions (+290-440 user instructions a sample) and the subscriber +0.3-1.9%: the seam's
  per-datagram segment question (`peer_segment()`, about 95 a datagram) and helpers gcc stops inlining once the slot
  path gives them a second caller. Not removing that question needs `peer_segment()` to skip peers discovery has not
  seen at our address, which also has to detach a peer that moves away; not done.
- **g15** (RMW_GAPS): `publish_zerocopy()`'s datagrams are counted. The interface's own packet count equals `tx_udp`
  on both zero-copy shapes, one datagram and two fragments, and removing either shape's count makes it differ
  (`experiments/g15_zerocopy_e2e.sh`). The one HAL send still outside `tx_datagrams` is the zero-length UDP doorbell
  to a peer without a FIFO bell, counted as `segment_doorbells_sent` minus `segment_bells_rung`.

## 11. Memory model

- **Embedded storage** sized by macros: server response cache (`tt_SERVER_CACHE_ENTRY_LENGTH` x 64), client call
  cache, scheduler (`tt_MAX_SCHEDULER_LENGTH` 128), peer and ack tables, per-peer tables indexed by context id
  (`tt_MAX_CONTEXT_IDS` 256).
- **Caller-attached storage** where the size depends on the use: a Publisher's `reliable_cache` (index + arena), a
  Subscriber's tracking window and reorder buffer, `tt_Server_set_storage()` / `tt_Client_set_storage()`, the
  receive-buffer lending pool (`tt_Context_set_rx_pool()`).
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
  network: counted (`rx_shm_only_on_socket`), never delivered, logged at the 1st, 10th, 100th ... refusal. The seq
  span is read only from a segment slot. A zero-length datagram (the UDP doorbell) can be sent by anyone; it only
  wakes the reader to drain its own ring, and can only make writers ring again, never suppress a ring.
  `test_hostile_datagram` covers all three through the socket, with plain DATA as the control.
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
| `tt_RELIABLE_RETRY_GRANULARITY` | 0 (measured) | | G, timer lateness term; non-zero = fixed |
| `tt_TIMER_LATENESS_INITIAL` | 100 us | | measured G before its first sample |
| `tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE` | 64 | | ceiling in srtt |
| `tt_RELIABLE_RETRY` | 3 | | KEEP_LAST give-up |
| `tt_RELIABLE_BITMAP_BITS` / `_MAX_BITS` | 256 / 4096 | | reader window default / ceiling |
| `tt_MAX_RELIABLE_HISTORY` | 64 | | reference cache depth for examples |
| `tt_CALL_RETRY_INTERVAL` | 5 ms | | RPC auto retry seed, until a first answer |
| `tt_CALL_RETRY_GRANULARITY` | 0 (2 x G) | | two hosts' event lateness term; non-zero = fixed |
| `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` | 64 | | a wait's ceiling in max(srtt, G) |
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
| `tt_SAMPLE_LENDING` | 1 (0 FreeRTOS) | 1 | receive-buffer lending (`tt_Sample_retain`) |
| `tt_SAMPLE_RETAIN_MAX` | 16 | | samples held at once per context |
| `tt_RX_POOL_MAX` | 32 | | caller-attached receive buffers per context |
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
