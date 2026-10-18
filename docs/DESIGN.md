# TickLE design

**TL;DR**
- A small pub/sub and RPC middleware in C for controlled links and microcontrollers; `rmw_tickle` puts ROS 2 on it.
- Core never allocates, starts no thread, does not depend on ROS 2, and counts every drop.
- Own UDP wire (`tt_VERSION` 13), RTPS-like: per-datagram seq_nos, NACK repair, broadcast discovery.
- Same-host peers use a shared-memory ring; large samples use caller buffers, a shared region, or IP-fragmented runs.
- Every constant with its default and reason: section 15. Where this text and the code disagree, the code is right.

Older text: `git show 0a7926a0:docs/DESIGN.md` (and before the 2026-10-17 rewrite, section 16).
Figures: [RESULTS.md](RESULTS.md). ROS layer: [RMW.md](RMW.md). Tests: [TESTING.md](TESTING.md). Plans: [ROADMAP.md](ROADMAP.md).

## 1. Overview

TickLE targets controlled links (10BASE-T1S, TSN, one trusted L2 segment), on FreeRTOS and Linux.

| principle | what it means |
|---|---|
| The core never allocates | no `malloc` in `src/`; tables are compile-time sized or attached by the caller |
| No ROS 2 dependency | ROS concepts live in `rmw_tickle` |
| No threads of its own (the user's rule) | work runs on callers' threads, from the poll loop or in the kernel |
| Own wire, not DDS | UDP/IPv4, RTPS-like ideas, smaller framing, not DDS-compatible |
| Every drop is counted | a silent drop looks like network loss |
| Wire changes leave every test better (the user, 2026-09-26) | targeted metrics improve beyond 2xSE, none regress; each bumps `tt_VERSION` |

## 2. Context, nodes, endpoints and the poll loop

**Contexts and nodes.**

| object | owns |
|---|---|
| `tt_Context` | sockets, scheduler, poll loop, discovery, liveliness, segment; its id is the wire's `source` |
| `tt_Node` | name, namespace, index (up to `tt_MAX_NODES`); index 0 is the default node `tickle_<id>`, made on first `tt_Context_create_*()` shorthand |
| `tt_Publisher`, `tt_Subscriber`, `tt_Client`, `tt_Server` | endpoint state, embedding `tt_Endpoint` and the node index |
| `tt_Topic`, `tt_Service` | the type: sizes, encode/decode |

`tt_Node_destroy()` refuses while an endpoint lives on the node.

**The poll loop.** One thread at a time calls `tt_Context_poll(ctx, timeout)`; a second poller gets `tt_RET_BUSY`.

| rule | detail |
|---|---|
| `timeout = 0` | one non-blocking pass; a high-rate publisher calls it between sends |
| `timeout < 0` | wait for the next scheduler entry, a datagram or `tt_Context_interrupt()`; nothing scheduled = wait forever |
| the wait | `epoll_pwait2()` over both sockets, an `eventfd`, the segment bell and a deadline `timerfd`; `ppoll()` fallback (relative timeout) |
| **a wait for a scheduler entry ends at its deadline** | over `TT_RX_TIMER_LONG_NS`: the slack-free `timerfd` at the absolute deadline; shorter: the thread's timer slack is lowered to `TT_RX_TIMER_SLACK_NS` |
| I/O interleave | at most `tt_SCHEDULER_IO_INTERLEAVE` due entries before a receive check, so ACKNACKs are not starved |

**Threading (concurrency).** `tt_THREAD_SAFE=1`: any `tt_*` call from any thread. One re-entrant state lock per
context; callbacks run inside it and may call core. Nothing is held while the poll waits; the lock is taken per
datagram (or per `tt_RX_LOCK_CHUNK`) and per scheduler entry. Timers from other threads go through a lock-free inbox
and wake the poll if earlier. `tt_Context_lock()` gives consistent compound reads.

**Receive path (Linux HAL).**

- `recvmmsg()` of up to `tt_RX_BATCH`; the two sockets alternate first refusal; the clock is read once per `tt_RX_CLOCK_REFRESH` datagrams.
- Both sockets `O_NONBLOCK`: the checksum is then made during the copy, not in a cold pass (`TT_RX_NONBLOCK`).
- **Lazy deadline timer:** after 8 data-ended waits in a row the deadline stays in the `timerfd` and the wait has no
  timeout (`TT_RX_LAZY_TIMER`, `TT_RX_LAZY_ADAPT`).
- **io_uring receive hint** (`tt_HAL_RX_HINT`): a busy loop reads a completion queue instead of an empty `recvmmsg()`.
  AUTO (0): used if allowed, else one warning (see "Containers: allowing io_uring"); READ (1): compiled out; URING (2):
  required, else `tt_RET_UNSUPPORTED`.

**Receive path (FreeRTOS HAL).** lwIP netconn (`tt_HAL_FREERTOS_NETCONN`). Netconn callbacks count arrivals
(`RCVPLUS`/`RCVMINUS`) and give a semaphore; the hint is "no" only when both counts are 0 (queued before counted, so
none is missed). `tt_receive()` waits on the semaphore, which `tt_wake_signal()` also gives.

**Time.** Two clocks, so a date step moves no deadline.

| HAL call | Linux / FreeRTOS | used for |
|---|---|---|
| `tt_get_ns()` | `CLOCK_MONOTONIC` / CLINT | every deadline and duration |
| `tt_get_wall_ns()` | `CLOCK_REALTIME` / same counter (or SNTP) | DATA timestamp, source timestamp, per-launch id draws |

Dates are the monotonic reading plus `wall_offset_ns`, re-read every second. A publish reads one clock. The 32-bit
stamp is rebuilt within a 71.6-minute span, so clocks must agree within +-35.8 min. rmw_tickle's lifespan takes a
sample's age once on arrival, then counts on the monotonic clock.

## 3. Wire format (`tt_VERSION` 13)

**Transport.** Two sockets per context: the **well-known socket** (wildcard, `_tt_CONTEXT_PORT` 8282, + ROS domain id)
receives broadcasts; the **data socket** (own address, ephemeral port) sends everything and receives unicast. A peer's
address is its datagrams' source. Broadcast goes to `_tt_CONTEXT_BROADCAST` (set the link's directed broadcast);
discovery is broadcast, never multicast (the user's decision). Up to `tt_MAX_LINK_COUNT` links. A datagram is at most
`tt_MAX_BUFFER_LENGTH` (1472: no IP fragmentation except as section 8 says).

**Datagram layout.**

```
classic: tt_Header        magic[2] "KT"/"TK" | version | source
         tt_SubmessageHeader  type | receiver | length (bytes, header included)
         body ...         (repeat, 4-byte aligned)
single:  tt_SingleHeader  marker 'k'/'t' | version | source | type
         body ...         (to the end of the datagram)
```

Native byte order; the magic tells the receiver whether to swap. Another `tt_VERSION` is dropped and logged once per
source. `source` is the context id (1-254); `receiver` an id or `tt_SUBMESSAGE_ID_ALL` (0xff). Padding is zeroed.

**Submessages.**

| type | name | body header | purpose |
|---:|---|---|---|
| 1, 7 | *retired* (UPDATE, UPDATE_PART) | - | never reused |
| 2 | `DATA` | `tt_DataHeader`: endpoint_id, seq_no, timestamp (u32 us), entity_id - 16 B | one sample in one datagram |
| 3 | `ACKNACK` | endpoint_id, entity_id, sender_entity_id, seq_no, bitmap_words, bitmap[] - 20 + 8/word B | reader's cumulative ack + missing bitmap |
| 4 | `CALLREQUEST` | endpoint_id, seq_no (u16), retry, client_tag - 8 B | service request; client_tag: which Client of its context (0 = untold) |
| 5 | `CALLRESPONSE` | endpoint_id, seq_no, retry, return_code - 8 B | service response |
| 6 | `HEARTBEAT` | endpoint_id, first_available_seq_no, last_seq_no, entity_id, flags - 20 B | writer's range; `tt_HEARTBEAT_FLAG_LIVELINESS` = manual liveliness assertion |
| 8 | `FRAG_FIRST` | `tt_DataHeader` + frag_count (u8) - 17 B | first fragment of a sample |
| 9 | `FRAG_CONT` | entity_id, seq_no, frag_index, frag_count (u8) - 10 B | later fragments |
| 10 | `SHM_DATA` | - | reserved for good; nothing produces it; refused from a socket |
| 11, 12 | `FRAG_FIRST_L`, `FRAG_CONT_L` | as 8 and 9 with u16 index and count - 18 B, 12 B | large samples (section 8) |
| 13 | `LARGE_REF` | `tt_DataHeader`, incarnation, generation, cdr_len, chunk, frag_count - 32 B | same-host large sample by reference; segment only (section 8, "Step B") |
| 14 | `FRAG_RUN_L` | `tt_DataHeader`, frag_index, frag_count, run_count (u16), 2 B zero - 24 B | hybrid run, large geometry (section 8) |
| 15 | `FRAG_RUN` | `tt_DataHeader`, frag_index, frag_count, run_count (u8), 1 B zero - 20 B | hybrid run, 8/9 geometry |

**Identifiers.** `endpoint_id`: FNV-1a hash of the type and endpoint names. `entity_id`: one endpoint instance;
(source, entity_id) names a writer. `seq_no`: per writer and **per datagram**, compared modulo 2^32. `timestamp`: low
32 bits of the sender's date in us. A lone sample costs 20 B of framing (24 B with rmw's psn).

**Payload encoding (TickLE CDR-4, the interface serialization).** Not OMG CDR. Native byte order; primitives aligned to
`min(size, 4)` (so batched payloads stay aligned); variable arrays `u16 count` + elements; nested messages inline.
**Strings:** `u16 length` (with the NUL) + bytes + pad to 4; decoded strings alias the receive buffer, valid only in the
callback. Structs use `#pragma pack(push, 4)`, so fixed-size messages decode in place. rmw writes a sequence count of
65,535 or more as 0xFFFF plus a uint32.

**Versions:** 5 wider ACKNACK bitmap; 6 per-subscriber ack identity; 7 discovery as DATA; 8 summary + pulled list;
9 liveliness flag; 10 32-bit timestamp, rmw psn, single header; 11 nodes announced; 12 types 11/12; 13 runs (14/15),
`ip_reassembly_max` and `loss_p` in the announce. `LARGE_REF` never crosses the network.

## 4. Sending: flush, batching and unicast peers

| rule | detail |
|---|---|
| RPC flushes at once | a caller is waiting |
| publish flushes at once by default | `pub->batch = true` coalesces until the `tt_CONTEXT_TX_INTERVAL` tick |
| shared datagrams are small | a batch or announce never exceeds `tt_CONTROL_MAX_LENGTH`, so any core build can receive it |
| **unicast to a few, broadcast to the rest** | per-endpoint `peers[]` (`tt_MAX_PEER_COUNT`); more than `tt_UNICAST_PEER_THRESHOLD` peers, or a full table, means broadcast. A Server answers unicast; a Client with no known Server broadcasts |
| **a Publisher with no matched reader sends nothing** (the user, 2026-10-15) | BEST_EFFORT drops (`tx_unmatched_dropped`); RELIABLE/durable keep it in history (`tx_unmatched_kept`). Matched by announce or, for RELIABLE, by ACKNACK. Covers every send path and unprompted HEARTBEAT; not local delivery, discovery or ACKNACK answers. Wait for `tt_Publisher_matched_count()` to keep first samples |
| one system call | datagrams ready together go in one `tt_send_batch()` (`sendmmsg()`) |

**UDP offload (Linux).** On by default (the user, 2026-10-12); `TT_UDP_OFFLOAD=0` turns it off; needs `tt_LARGE_SAMPLES`.

- **Send, `UDP_SEGMENT`:** a run of a large sample's MTU fragments (types 11/12) to one destination goes as one
  message of at most 64 datagrams / 65507 B. Not for runs of two (costs more).
- **Receive, `UDP_GRO`:** where the buffer is 65507 B, one read may return a run, handed to core a datagram at a time in
  place. On from a large fragment until 128 reads without one; off only when both queues are empty; off for good on
  refusal or `EIO`.
- Never for hybrid runs (IP fragments bypass GRO) or a lone MTU tail.

## 5. Discovery

Ordinary traffic on a built-in endpoint (`tt_DISCOVERY_ENDPOINT_ID` 0, `tt_DISCOVERY_ENTITY_ID` all ones). Steady state:
one ~28-byte summary per node per second.

| role | submessage | addressed | contents |
|---|---|---|---|
| summary, every `tt_CONTEXT_UPDATE_INTERVAL` or a sixth of the shortest own lease | HEARTBEAT | broadcast | the context's **generation** |
| request for the list | ACKNACK | unicast to the summary's sender | the generation wanted |
| the list (announce) | DATA, or FRAG_FIRST + FRAG_CONT | unicast to the requester | `tt_AnnounceHeader` + `tt_UpdateEntity` records |
| a change | the list | broadcast, after `tt_CONTEXT_TX_INTERVAL` of quiet | the new generation |

- The generation changes exactly when the endpoint list does, and across restarts. A known generation is liveliness only.
- An unknown generation draws a request, retried after RTT + `tt_CONTEXT_TX_INTERVAL` (seed `tt_DISCOVERY_REQUEST_RETRY`),
  up to `tt_DISCOVERY_REQUEST_ATTEMPTS`, in `tt_DISCOVERY_PENDING_REQUESTS` slots.
- Up to `tt_UNICAST_PEER_THRESHOLD` requests per tick are answered unicast, more by one broadcast.
- **A reply never waits for room:** with a full send buffer (a large sample draining) the announce reply and the list
  answer are skipped and counted (`discovery_replies_skipped`); the peer asks again.
- **A large list is fragmented at entity boundaries**, each part applied on arrival (no reassembly memory), at most
  `tt_UPDATE_MAX_PARTS`; a context needing more is invisible.
- **Nodes are announced** (v11) as `tt_KIND_NODE` entries; every entry carries its node index in spare bits of `kind`/`qos`.
- **The discovery table** (`tt_MAX_DISCOVERED_ENTITIES`, 552 B an entry; indexed above 64) feeds RxO, leases and the
  rmw graph. RxO **fails open** for an entity with no room: size it to what a context will see (`entities_dropped`).
- **Discovery range** (`tt_DISCOVERY_OPTIONS`): `SUBNET`, `LOCALHOST`, `OFF`, static `peer` links. Same-host discovery
  stays UDP on Linux and FreeRTOS (the user, 2026-10-09).

## 6. Reliability

As DDS/RTPS: the writer keeps what it sent, the reader NACKs gaps, the writer resends. Repair is per datagram. Timers
are measured, not fixed.

**Writer side.**

| rule | detail |
|---|---|
| history | caller-owned `reliable_cache` (index + arena), one record per datagram; `capacity`/`arena_size` are its RESOURCE_LIMITS; `sample_depth` makes KEEP_LAST count samples |
| no resend cap | the first `tt_RELIABLE_RETRY` resends answer at once, then at most once per retry interval (nack suppression) |
| evicted seq_no | answered with a HEARTBEAT carrying the real `first_available_seq_no` |
| repairs go to the asker alone | matched or not; an ACKNACK matches a reader only to a writer with history (`claim_from_acknack()`) |
| ack state per remote reader | `tt_MAX_ACK_ENTRIES` entries (24 B, + 40 B hybrid); the next reader is not matched (logged) |

**Reader side.**

| rule | detail |
|---|---|
| window | received bitmap per writer proxy, `tt_RELIABLE_BITMAP_BITS`, attachable up to `_MAX_BITS`; out-of-order datagrams wait in a caller reorder buffer; delivery in order |
| NACK | at once on a gap; repeated on the retry timer |
| retry interval | `srtt + max(G, 4 x rttvar)` (RFC 6298 over request-to-recovery), at most `tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE` x max(srtt, G); `tt_RELIABLE_RETRY_INTERVAL` non-zero fixes it |
| **G, measured timer lateness** | how late retry-timer waits return past deadline, RFC 6298 gains, `G = mean + 4 x deviation`, floored at timer resolution; `tt_RELIABLE_RETRY_GRANULARITY` non-zero fixes it |
| a busy writer is not the path | `recovery_srtt_ns` (Karn-like) apart from `sending_srtt_ns`; during a send, ask only what a repair could have answered |
| no give-up on a count | a gap ends on arrival, on HEARTBEAT eviction (`gap_evicted`) or with the writer; window overflow jumps (`gap_abandoned`) |
| back-off | only from a writer gone quiet: after the plain retries, doubling up to 2^`tt_RELIABLE_RETRY_BACKOFF_SHIFT`; KEEP_ALL keeps the plain interval |
| stuck warning | every `tt_RELIABLE_STUCK_WARN_INTERVAL` |

**KEEP_ALL flow control.** Never evicts unacknowledged data: the write returns `tt_RET_WOULD_BLOCK` (nothing sent) and
`writable_callback` says when to retry. Bound: `min(cache depth, narrowest announced reader window)`.

**ACK solicitation.** Readers ACK only on gaps, so a KEEP_ALL writer sends a non-FINAL HEARTBEAT at half the bound and
each further half, one in flight; a refused write solicits and `keep_all_resolicit()` repeats every retry interval.
Others opt in (`ack_solicit_watermark_pct`, `ack_solicit_period_ns`).

**Idle HEARTBEAT** (the user, 2026-10-16; `tt_IDLE_HEARTBEAT_PERIOD`). While a matched RELIABLE reader has not
acknowledged everything and nothing told it everything for P, send a non-FINAL HEARTBEAT, and every P after. One
scheduler entry per writer, postponed by a publish past P/2, so a burst's last sample gets its HEARTBEAT P/2 to P
later. Silent with nothing unacknowledged, no matched reader, or BEST_EFFORT readers. P is floored at 4 retry intervals.

## 7. Services (RPC)

| rule | detail |
|---|---|
| calls per Client | up to `tt_CLIENT_MAX_OUTSTANDING` (the user, 2026-10-16), each in its own slot, answered in any order; call N + 1 gets `tt_RET_ILLEGAL_STATUS` |
| seq_nos | from the context's one counter; each Client of a service has its own `client_tag` (1..255) |
| retry | `call_retry_interval` if set; else first wait `srtt + max(G, 4 x rttvar)`, doubling, each at most `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` x max(srtt, G); seed `tt_CALL_RETRY_INTERVAL`; G is twice the reliable one (`tt_CALL_RETRY_GRANULARITY` fixes it) |
| give-up | after `2^(count+1) - 1` first waits, never later than `(count + 1) x tt_CALL_DEADLINE_PER_SEND`; a timeout doubles srtt once |
| BEST_EFFORT Client (D6) | sends each request once; a lost call ends with `tt_CALL_TIMEOUT` at the RELIABLE give-up, so a late answer still lands. Decided in `call_retry()`: the call path compiles unchanged. `tt_Client_end_oldest_call()` frees a slot for the next call |
| response cache | `tt_MAX_SERVER_CACHE_COUNT` slots keyed by (source, `client_tag`, seq_no): a retry never runs the callback twice. Kept by a BEST_EFFORT Server too: the retries come from RELIABLE clients |
| call window | a Server keeps `tt_CLIENT_CALL_WINDOW` (2 x outstanding) answers per Client; a Client makes no call while one is that many calls old; both ends must agree |
| answer lifetime | the longer of the client's seed schedule and `tt_SERVER_CACHE_GAP_MULTIPLE` x its longest response-to-retry gap; dropped for a new incarnation; a full cache evicts the oldest-sent |
| deferred | `tt_CALL_DEFERRED`, then `tt_Server_send_response()` from any thread; reclaimed after `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` |
| turned away | `tt_CALL_IGNORED` keeps nothing; the retry asks again |
| size | never fragmented: bounded by `tt_MAX_BUFFER_LENGTH` |

## 8. Fragmentation and large samples

TickLE splits, not IP, so fragments are repaired one at a time. Up to 65507 B: the small path. Larger: stage 2,
step B (same host) and the hybrid transport. Pre-registrations (L1-L4, B1-B2): section 16.

**Sizes:** `tt_MAX_BUFFER_LENGTH`, `tt_CONTROL_MAX_LENGTH`, `tt_MAX_SAMPLE_LENGTH`, `tt_FRAG_ENABLED`,
`tt_LARGE_SAMPLES` (section 15). At core defaults fragmentation compiles out and an oversized sample is refused.

**Small-path rules.**

| rule | detail |
|---|---|
| wire | fragments alone in their datagram, unpadded; all full but the last; fragment 0 is `tt_FRAG_FIRST_SHORTFALL` (7) bytes shorter, so no offset field; at most `tt_FRAG_MAX_COUNT` |
| **every datagram takes its own seq_no** (the user, 2026-09-26) | a k-fragment sample uses k seq_nos, named by its first; a lost fragment is resent alone; KEEP_LAST evicts whole samples; KEEP_ALL admits a sample only if all of it fits |
| sender | encode once in `tx_buffer`, one `sendmmsg()` |
| RELIABLE receiver | in-order fragments assemble in `frag_scratch` (`frag_fast_take()`); others go to the reorder buffer; never torn (`reorder_abandoned`) |
| BEST_EFFORT receiver | `tt_FRAG_REASSEMBLY_SLOTS` keyed by (source, entity, sample); full: reuse a completed one, then abandon the oldest |

### Stage 2: samples above 64 KB

User decisions: per-datagram seq_no at this size, wire types 11/12, 8 MiB default, ~11.3 MiB ceiling (2026-10-09).

| rule | detail |
|---|---|
| bounds | the reader window (8192 bits) must hold one sample: max 1,446 + 1,452 x 8,191 B (~11.3 MiB); rmw default 8 MiB (`RMW_TICKLE_MAX_SAMPLE_BYTES`) |
| wire (v12) | types 11/12 = 8/9 with u16 index/count (shortfall 6), used for **every sample above `tt_MAX_SAMPLE_LENGTH` and only those**; a node without stage 2 skips them (`frag_large_skipped`); different control sizes drop (`large.dropped`) |
| storage | caller callbacks `large_acquire`/`large_release` (`tt_Context_set_large_buffers()`); `NULL` = no room |
| publisher | encodes into its buffer and sends from it (two iovecs a datagram); **retains by reference** (`tt_LARGE_RETAINED`), released after the last fragment (BEST_EFFORT), all acks (VOLATILE) or KEEP_LAST eviction (DURABLE) |
| **Send cursor** | on `EAGAIN` the rest goes from the poll when the socket reports half free (`EPOLLOUT` on a dup), with `tt_LARGE_SEND_WRITABLE_FALLBACK_NS`; without that, a timer `tt_LARGE_SEND_RETRY_NS` doubling to `_MAX_NS` |
| a publish during a send | waits without seq_nos (`large_pending`); a newer one replaces it (`large.tail_abandoned`); KEEP_ALL gets `WOULD_BLOCK`; the sample in flight is never abandoned |
| **End-of-sample HEARTBEAT** | `tt_LARGE_END_HEARTBEAT`: non-FINAL after the last fragment, chased (one retry interval, doubling to 100 ms) while unacknowledged, with the match HEARTBEAT ahead of it while owed |
| **Subscriber** | the first fragment acquires `frag_count x 1452` B in one of `tt_LARGE_ASSEMBLIES`; each fragment copied to its offset; no buffer: RELIABLE leaves it unrecorded, BEST_EFFORT drops (`large_no_buffer`) |
| lending | `tt_LEND_LARGE`: the sample stays in its buffer while retained; blocks no ring |
| **Flow control** (KEEP_ALL) | a reader window smaller than one sample fails the publish with `tt_RET_TOO_LARGE`; rmw gives unbounded types the 8192-bit window (1 KiB per tracked writer) and at least two max samples of KEEP_ALL budget |
| scope | only with `tt_LARGE_SAMPLES` (rmw; not FreeRTOS); not services above 64 KB or a local durable backlog |

#### Step B: the same-host large path

The sample is encoded once into a chunk of a **Publisher-owned shared region**; each same-host reader gets one
`LARGE_REF` descriptor whose `seq_span` is the datagram count, so the seq space matches the socket path.

| rule | detail |
|---|---|
| region | `tt_Publisher_set_large_region(pub, chunks, chunk_bytes)`, at most `tt_LARGE_REGION_CHUNKS_MAX`; rmw: `min(depth, tt_LARGE_RETAINED) + tt_LARGE_REGION_SPARE` chunks (off with `RMW_TICKLE_LARGE_REGION=0`) |
| file | `/dev/shm/tickle-large-<ip>-<port>-<context id>-<entity id>`, made through the HAL with the owner's `flock()`, unlinked with its Publisher |
| layout | 64 B header (`TKLG`, version, owner, `incarnation`, geometry), 64 B per chunk (`generation`, `readers` bits), chunks |
| writer | takes the lowest free chunk (no record, `readers` 0), bumps `generation`; none free: evict KEEP_LAST, else fragments (`large_ref.region_full`); sets the reader's bit, then publishes the descriptor; full ring clears it |
| **dead reader** | when liveliness removes a peer, its bits are cleared; not before |
| reader | `LARGE_REF` only from the segment (`rx_shm_only_on_socket`); maps up to `tt_LARGE_PEER_REGIONS`; checks header, owner, incarnation, range, generation, its bit (`large_ref.ref_refused`); clears its bit when nothing references the chunk |
| compatibility | segment header bit `tt_SEGMENT_FEATURE_LARGE_REF`; readers without it get fragments; no version bump |
| accepted limits | an unmappable reader holds its bit until liveliness removes it; a durable backlog gets fragments |

### Encoding while sending

A large publish encodes each part just before the send call that carries it: one whole-sample copy leaves the latency
path; the wire is byte-identical.

- `tt_Topic.data_encode_range(data, payload, offset, len)` (`tt_DATA_ENCODE_RANGE`) writes a byte range of what
  `data_encode` writes. rmw offers it for serialized messages and `inplace_bytes` types.
- Each send call (up to `LARGE_BATCH`, 64 units) first encodes through its last byte; on `EAGAIN` the rest is encoded at
  once, so the record is whole when the publish returns.
- Encoded whole when it waits behind a send, goes by descriptor or is delivered locally. A failed encode drops the
  sample (`large.encode_failed`); nothing torn is delivered.

### Hybrid transport for multi-datagram samples

Per peer, the writer puts `n` MTU fragments (**units**) into one IP-fragmented datagram (a **run**), from the loss the
reader measures. The user lifted the IP-fragmentation park for this (2026-10-11), set the objective to wire bytes, and
required every input to be measured by TickLE. Scope: every sample over one datagram, types 8/9 and 11/12; each unit
keeps its seq_no. `n_max = min(45, peer's reassembly bytes / unit)`.

**The objective and the control law (the user, 2026-10-11).**

1. *Network only.* RELIABLE minimises `Cost(n) = (u + h / n) + (1 - q(n)) (u + h)`, `q(n) = (1 - p)^n`, `h` = 26 B
   (`n*` 45, 13, 4, 1 at `p` 0, 0.01%, 0.1%, 1%). BEST_EFFORT takes the largest allowed `n`.
2. *Only what TickLE measures.* The reader measures `p`, owns the cap, advertises both in the announce.
3. *The control law*, at the reader each announce interval: (a) on a collapse at `n_fail`, cap `max(1, round(0.7
   n_fail))`; (b) re-probe above it only on time (`2 tau`, doubling to `16 tau`), growing back to `n_fail - 1`; (c) scale
   `n_fail` when `R p` moves by `tt_HYBRID_SCALE_AT` for three ticks; (d) loss consistent with `(1 - p)^n` never moves
   the cap; (e) a path that drops IP fragments goes to `n = 1` at once.

| term | rule |
|---|---|
| **What the reader measures** | it rebuilds the writer's layout; a skipped unit is loss, a repair counts for nothing; whole lost samples come from seq_no jumps (at most 8). `p_mtu`: MTU-unit loss; `q(n)`: run survival; `R`: units/s; `D_s`: run datagrams/s per source |
| **Estimating p** | `p = (events + 1) / frames` (a lost run is one event on `n` frames); halves at `tt_HYBRID_WINDOW_FRAMES` |
| none seen | no event in `tt_HYBRID_NONE_SEEN_FRAMES` advertises `n_max`; until then a RELIABLE AUTO writer sends MTU units, one datagram a call |
| stale events (the user, 2026-10-15) | up to `tt_HYBRID_STALE_EVENTS` forgotten once `(1 + C / F)^k >= 1e4` or `C >= 9.21 / p_min` clean frames |
| **The collapse test** | over >= 16 runs: `z >= 3.09` against `L0` at `p_mtu`'s 99.9% bound, and `X >= 4`, `X >= 2 R L0` at the 95% bound (amended 2026-10-17); **clean** if `X <= R L0`; else gather up to 1,024 runs; `n_fail = max(2, min(n, ceil(n q / q0)))` |
| rule (e), reader | all runs of a sample lost (below 1e-6 by frame loss) and none through for `tau`: one unit, re-probe after `2 tau` |
| rule (e), writer | all runs first-NACKed but not the tail: MTU units for `tt_HYBRID_PATH_CLEAN_NS`, then one re-probe |
| `tau` | `min(tt_HYBRID_QUEUE_LIFETIME_NS, 65536 / D_s)`: reassembly lifetime, or the IP ID wrap |

**Settings** (not QoS; in `_tt_CONFIG` until `tt_InitOptions`, ROADMAP "QoS reshaped like rmw"):

| setting | values | default |
|---|---|---|
| `large_fragmentation` | `tt_LARGE_FRAGMENTATION_AUTO`, `_MTU` (`n = 1`), `_IP` (`n = n_max`) | `AUTO` |
| `ip_reassembly_max` | largest datagram accepted from IP reassembly, advertised; 0 = none | Linux 65,507; FreeRTOS 0 |
| `reassembly_cap_off` | 1: advertise uncapped (forced arms) | 0 |
| publisher `large_fragmentation`, `ip_datagram_max` | override or inherit (0) | inherit |

`n = 1` for an undiscovered peer, a segment peer, every repair and backlog. rmw's forced-arm variables:
`RMW_TICKLE_LARGE_FRAGMENTATION`, `RMW_TICKLE_REASSEMBLY_CAP_OFF`, `RMW_TICKLE_RUN_UNITS`.

**Wire (v13).** Types 14/15 carry units concatenated; a lost run is repaired as MTU units. **The last unit always goes
alone** (runs only when `k >= 3`), so a reader sees its runs were lost. `loss_p`: 0 = none, else `(256 + m) / 2^(e + 9)`.
Unicast peers get their own `n`; broadcast the minimum.

#### Saturation: where I4s lost its samples, and the receive drain

With no receive thread, the socket overflows while a callback decodes a large sample. The **receive drain**
(`tt_rx_drain_start()`; Linux, io_uring, `tt_LARGE_SAMPLES`) has the kernel read for us.

- While core is away in a large stream, an io_uring multishot `recvmsg` per socket fills HAL buffers as task work of the
  polling thread. No thread of ours.
- Buffers `tt_RX_DRAIN_BYTES` (2,048 x 1,536 B or 64 x 65,600 B), doubling up to `tt_RX_DRAIN_BYTES_MAX` after an overrun.
- Datagrams are handed out oldest first. Too long for a buffer: dropped, drains held off 100 ms. No io_uring
  multishot (before 6.0): unchanged, said once.

**Receiver drops are not path loss:** the HAL's drop count (`tt_rx_receiver_drops()`) excuses that many gaps from `p`,
the run window and rule (e). **An excused gap is not clean:** before the first `p` it restarts the estimate; after, it
is a doubt point for "none seen" and the stale rule.

#### Receiving runs in place

After a run lands, core hints the next (`large_direct_aim()`), and the HAL reads it with one three-piece `recvmsg()`:
framing to the receive buffer, payload straight into the assembly (`direct_read()`).

- Only over units not landed, never across threads; misses back off 2..64 runs; native-endian writers only;
  `TT_RX_DIRECT=0` turns it off. A run is recorded as one span (`rx_seq_span`).

#### UDP checksum 0, on by default

No NIC checks an IP fragment, so a run's checksum costs the receiver a pass. On by default (the user, 2026-10-15): a
`sendmmsg()` carrying a datagram above 1472 B sets `SO_NO_CHECK`, cleared before a `UDP_SEGMENT` batch (the kernel
refuses both). The Ethernet CRC still covers frames. `TT_UDP_CSUM0=0` keeps checksums: use it through a router or a
link without frame CRC. Not on FreeRTOS.

## 9. QoS: liveliness, deadline, lifespan, durability

**API** (2026-10-18). QoS is given at creation and only then: `tt_Node_create_publisher(node, pub, type, topic, qos,
options, storage, size)`, and `_subscriber`, `_client`, `_server` alike. `struct tt_QosProfile` holds one struct per
policy (history, reliability, durability, deadline, lifespan, liveliness): the rmw fields, then TickLE's extensions
(`history.bytes`; `reliability.tracking_words`, `reorder_slots`, `max_blocking_ns`). Not ROS's struct: core has no
ROS dependency, and rmw_tickle converts.

| rule | detail |
|---|---|
| zero field | the computed default, `rmw_qos_profile_default`: RELIABLE, KEEP_LAST 10, VOLATILE, AUTOMATIC, infinite |
| resolution | `tt_QosProfile_resolve()`: SYSTEM_DEFAULT, BEST_AVAILABLE (from the discovery table), and out-of-range values refused, not adjusted |
| hot path | the resolved profile is written into the endpoint's existing fields; only what none held is appended |
| storage | the caller's, `tt_*_storage_size()` bytes; a profile that needs it fails without it, never falls back to BEST_EFFORT. A Publisher's: the cache, an index ring of depth samples' datagrams, an arena of (depth + 1) records on a 64 B boundary. Creation clears only what holds anything (a reorder slot: its header) |
| per sample | nothing the hand-built caches did not pay: a KEEP_LAST cache whose ring and arena keep the depth for samples of its type's largest shape skips the sample count (`depth_assured_*`) until a sample falls outside it |
| depth | counts samples; `history.bytes` caps a Publisher's cache: a warning at creation, the depth kept in `tt_*_get_actual_qos()`. An unbounded type (`max_encoded_size` 0, or a size its binding chose: `max_encoded_size_estimated`) keeps the depth asked for; a sample that does not fit is counted at publish (`depth_shortfalls`) |
| not QoS | `tt_PublisherOptions`, `tt_SubscriberOptions`, `tt_ClientOptions` (call retry), `tt_ServerOptions` |
| services | default `tt_QOS_PROFILE_SERVICES_DEFAULT` (RELIABLE, VOLATILE, KEEP_LAST 10); a Client's reliability decides resending (section 7). TRANSIENT_LOCAL and MANUAL liveliness are refused: no call is retained for a late server, and nothing asserts a service's liveliness |

`tt_Topic` is now `tt_TypeSupport`. Deprecated until callers move: `tt_Context_create_*()`, `tt_Node_create_*_legacy()`,
and QoS written into endpoint fields.

**RxO matching.** `tt_UpdateEntity` carries `qos` bits (reliable, durable, manual liveliness), deadline and lease;
DATA from an incompatible writer is dropped (`rxo_drops`). Compatibility is judged when an announce is applied and when
an endpoint is created, by the same rule: each incompatible pair counts once per endpoint (`tt_QosIncompatibleStatus`,
first failing policy) and fires `tt_Context_set_qos_incompatible_callback()` (D2, 2026-10-19).

**Liveliness** (v9): a lease runs from the last sign of life.

| rule | detail |
|---|---|
| sign of life | AUTOMATIC: any datagram from the context; MANUAL_BY_TOPIC: that writer's DATA or a liveliness HEARTBEAT (`tt_Publisher_assert_liveliness()`) |
| timer | one `check_liveliness()` entry at the earliest expiry; a lapsed entity is tombstoned and revives on its next sign |
| context silence | `tt_LIVELINESS_SILENCE_NS` = 3.5 summary intervals, or the longest announced lease up to `tt_CONTEXT_MAX_LEASE_NS`; its state is then dropped |
| summaries | every interval or lease / `tt_LIVELINESS_LEASE_DIVISOR`; extra ones skipped when every peer heard us |
| descheduled host | the check defers up to `tt_LIVELINESS_MAX_DEFERRALS` x `tt_LIVELINESS_DEFER_NS` while datagrams wait |

**Deadline:** announced for RxO; rmw_tickle enforces it: one miss per period P with no activity, checked at the last
activity + P (a timestamp store per sample, no scheduler call). **Lifespan:** an older cached sample is neither resent
nor replayed. **Durability (TRANSIENT_LOCAL):** a durable Publisher unicasts its cache to a new Subscriber, once per
(context id, generation); a durable Subscriber refuses a VOLATILE writer. **Local delivery** (`tt_LOCAL_DELIVERY`):
in-process delivery under the same rules; otherwise own DATA is dropped.

## 10. Shared-memory transport

A second carrier, not a protocol: a ring slot holds the datagram that would have gone on the wire, through the same
acceptance path. Chosen per peer. On by default on Linux (`tt_SEGMENT_ENABLED`), off on FreeRTOS.

**The segment.**

| rule | detail |
|---|---|
| one per receiving context | `/dev/shm/tickle-seg-<ip>-<port>-<context id>`, written by every same-host peer |
| the header is the authority | magic, `tt_SEGMENT_VERSION` (6), owner, `incarnation`, geometry, `features`; any mismatch: UDP, counted |
| slots | power of two, 16 B header (`length`, sender, `seq_span`, `sequence`) + payload; claim by CAS, publish `index + 1` with release, free with `index + slots`; no wrap straddle |
| geometry | `tt_SEGMENT_BYTES` / (16 + `tt_SEGMENT_SLOT_BYTES`) rounded down; runtime `segment_slot_bytes`; too large: UDP (`segment_oversized_to_udp`) |
| `seq_span` | seq_nos a record covers; only from a slot, never the network; `tt_SEGMENT_SPAN_FENCE` marks a fence |

**Lifecycle (lazy).** Built when the first same-host peer appears, released when the last goes. **No writer uses a dead
owner's segment:** the owner's mapping holds a `flock()`; `tt_segment_attach()` refuses an unheld file
(`tt_SEGMENT_ORPHANED`); writers never unlink. Writers re-ask every `tt_SEGMENT_ATTACH_RETRY_SENDS` sends when absent,
`tt_SEGMENT_REVALIDATE_SENDS` when attached. A reader idle for `tt_SEGMENT_DEAD_READER_NS` with records waiting is
given up (UDP until recheck). A wedged head warns after `tt_SEGMENT_STALL_PASSES`.

**One path per peer, and drop on full.** Once attached, every datagram to the peer goes through the ring; a full ring
**drops** (`segment_full_dropped`), since rerouting to UDP would overtake queued records. UDP still carries broadcasts,
oversize datagrams and pre-attach traffic. The reader drains at most `tt_SEGMENT_DRAIN_PER_POLL` per poll.

**The switch from UDP is fenced.** On attach after UDP, the writer sends a 3-byte fence to the peer's socket and puts the
same bytes in the ring (`seq_span = tt_SEGMENT_SPAN_FENCE`) first. The reader does not pass the ring fence until the
socket one arrives (remembered `tt_SEGMENT_FENCES_SEEN`), or after `tt_SEGMENT_FENCE_HOLD_NS`.

**Doorbell: a FIFO, rung once per reader sleep.** The sleeping owner publishes a sleep generation in `reader_waiting`
and drains again; a writer checks it after publishing (full fences) and rings `<segment>.bell` once per generation
(edge-triggered; level before Linux 5.14; a zero-length UDP datagram for peers without a FIFO). Spinning instead of
sleeping is chosen per epoch by measured cost.

The ring cannot be the RELIABLE retention cache: its memory is the receiver's.

**Receive-buffer lending** (`tt_Sample_retain` / `tt_Sample_release`). A callback keeps its sample without a copy.

| rule | detail |
|---|---|
| what is lent | the ring slot (left unreleased until release), the receive buffer (`rx_buffer` plus a pool, `tt_Context_set_rx_pool()`, `tt_RX_POOL_MAX`), or a large buffer; buffers 8-aligned |
| not lent | samples from small fragments, a reorder buffer, local delivery or a backlog (`tt_RET_UNSUPPORTED`; copy) |
| handles | at most `tt_SAMPLE_RETAIN_MAX`, with generations; a bad release is refused and touches nothing; release from any thread |
| **a held slot blocks only its own ring, one lap later** | hold for less than one lap at the stream's rate, or copy |

**Claimed-slot publish** (`tt_Publisher_claim` / `_publish_claimed` / `_abandon_claim`): the caller builds the CDR in
the subscriber's slot.

| rule | detail |
|---|---|
| when | a lone DATA to one attached same-host peer; otherwise refused with a reason (`UNSUPPORTED`, `OUT_OF_BUFFER`, `WOULD_BLOCK`, `ILLEGAL_STATUS`), meaning "publish ordinarily" |
| one claim per publisher | while out, ordinary publishes are refused; the seq_no is taken at the publish; the slot stops its ring until published |
| re-checked | if the destination changed since the claim, the bytes are copied and published ordinarily (`shm_claims_copied`) |
| RELIABLE | still copies into its cache; destroy abandons an outstanding claim |

## 11. Memory model

- **Embedded:** server and client caches, scheduler (`tt_MAX_SCHEDULER_LENGTH`; full: refused, `sched_full`), peer and
  ack tables, per-id tables (`tt_MAX_CONTEXT_IDS`).
- **Caller-attached:** `reliable_cache`, reader window and reorder buffer, server/client storage, the lending pool, the
  large-buffer callbacks. Core holds pointers only.
- rmw_tickle allocates with the application's `rcl` allocator.

## 12. Context ids and same-host peers

Per-peer tables are indexed by the 8-bit context id, unique per link.

- Without claiming: the address's last octet, or `_tt_CONFIG.context_id`.
- **`tt_CONTEXT_ID_CLAIM`** (Linux): claimed in a registry under `/dev/shm` per network; a datagram is our own only from
  our data socket; our id from elsewhere is a collision and the newer context moves (an explicit id never does).

## 13. Security model

No cryptography: safe only on a controlled link. What it does:

- rmw_tickle refuses `ROS_SECURITY_ENFORCEMENT=Enforce` and warns once in permissive mode.
- `SHM_DATA` (10) and `LARGE_REF` (13) from a socket are refused (`rx_shm_only_on_socket`). A zero-length datagram only
  wakes the reader. Lengths and ACKNACK word counts are bounds-checked.

**Parked** until the user says so ([ROADMAP.md](ROADMAP.md)): SROS2-file-compatible security on MACsec, with a core
**TickLE Security** module (authentication at association, authorization at creation and match, protection inside the
acceptance path); keystore parsing in rmw.

## 14. Wireless and zenoh-pico notes

- **Wireless is parked.** No congestion control, NAT traversal or encryption: no internet paths. Run-header merge, FEC
  and range NACKs wait for the wireless stage (the user, 2026-10-15).
- **zenoh-pico** does not retransmit over UDP; its reliable p2-p4 cells are not measurable.

## 15. Configuration reference

Compile-time `-D` overrides unless noted; blank rmw column = core default.

| knob | default | rmw | meaning, and why this value |
|---|---:|---:|---|
| `tt_MAX_BUFFER_LENGTH` | 1472 | 65507 | largest datagram; 1472 avoids IP fragmentation |
| `tt_CONTROL_MAX_LENGTH` | 1472 | 1472 | shared datagram and fragment size; any build can receive it |
| `tt_MAX_SAMPLE_LENGTH` | = buffer | = buffer | largest small-path sample |
| `tt_FRAG_ENABLED` | derived | 1 | fragmentation, when sample > control; 0 forces IP fragmentation (benchmark arm) |
| `tt_FRAG_MAX_COUNT` | 64 | | fragments per small-path sample |
| `tt_FRAG_REASSEMBLY_SLOTS` | 8 | 8 | best-effort reassemblies in flight |
| `tt_LARGE_SAMPLES` | 0 | 1 | stage 2, offload, drain (widens window arrays) |
| `tt_LARGE_RETAINED` / `tt_LARGE_ASSEMBLIES` | 16 / 4 | | large samples retained per Publisher / assembled per context |
| `tt_LARGE_END_HEARTBEAT` | 1 | | lost tail NACKed one round trip later |
| `tt_LARGE_SEND_ON_WRITABLE` | 1 | | send cursor waits for writability (0 = timer) |
| `tt_LARGE_SEND_RETRY_NS` / `_MAX_NS` / `_WRITABLE_FALLBACK_NS` | 50 us / 1 ms / 20 ms | | cursor timers; 20 ms exceeds half a buffer at 1 GbE |
| `tt_LARGE_REGION_SPARE` / `tt_LARGE_PEER_REGIONS` | 3 / 8 | | one sending, one waiting, one in a callback / regions a reader maps |
| `tt_LARGE_REGION_CHUNKS_MAX` | 64 | | chunks per region |
| `tt_HYBRID_QUEUE_LIFETIME_NS` | 30 s | | `tau` default = kernel `ipfrag_time` |
| `tt_HYBRID_NONE_SEEN_FRAMES` | 500 | | clean frames before "none seen" |
| `tt_HYBRID_STALE_EVENTS` / `_FALSE_FORGET` / `_P_MIN` | 8 / 1e-4 / 1e-3 | | forget a real loss rate at most 1e-4 of the time |
| `tt_HYBRID_SCALE_AT` | 1.9x | | `R p` change that rescales `n_fail` |
| `tt_HYBRID_PATH_CLEAN_NS` | 60 s | | MTU-only after a failed path (2 tau) |
| `tt_HYBRID_WINDOW_FRAMES` | 2^20 | | loss window halving |
| `tt_RX_DRAIN_BYTES` / `_MAX` | 4,352 KiB / 4x | | drain buffers |
| `tt_THREAD_SAFE` | 1 | 1 | locks; 0 for a single-task MCU |
| `tt_CONTEXT_TX_INTERVAL` | 1 ms | | batch flush tick |
| `tt_CONTEXT_UPDATE_INTERVAL` | 1 s | | discovery summary interval |
| `tt_UNICAST_PEER_THRESHOLD` | 2 | | peers reached by unicast per link; more = one broadcast |
| `tt_MAX_PEER_COUNT` | 8 | | peers table per Publisher/Client |
| `tt_MAX_ACK_ENTRIES` | 16 | | RELIABLE readers per Publisher (rmw: `RMW_TICKLE_MAX_RELIABLE_READERS`) |
| `tt_MAX_LINK_COUNT` | 4 | | links per context |
| `tt_MAX_CONTEXT_IDS` | 256 | | per-id tables: the 8-bit context id |
| `tt_MAX_ENDPOINT_COUNT` | 256 | 2048 | local endpoints |
| `tt_MAX_NODES` | 16 | 256 | nodes per context |
| `tt_MAX_DISCOVERED_ENTITIES` | 16 | 2048 | remote entities; RxO fails open past it |
| `tt_UPDATE_MAX_PARTS` | 32 | 255 | announce fragments |
| `tt_DISCOVERY_REQUEST_RETRY` / `_ATTEMPTS` | 10 ms / 4 | | seed until a round trip is timed |
| `tt_DISCOVERY_PENDING_REQUESTS` | 8 | | list requests in flight |
| `tt_LIVELINESS_MISS_THRESHOLD` | 3 | | silence = 3.5 intervals (three missed summaries) |
| `tt_LIVELINESS_LEASE_DIVISOR` | 6 | | summaries per shortest lease |
| `tt_LIVELINESS_MAX_DEFERRALS` / `_DEFER_NS` | 4 / 1 ms | | a descheduled process does not kill live peers |
| `tt_CONTEXT_MAX_LEASE_NS` | 10 s | | longest lease that holds a silent node |
| `tt_RELIABLE_RETRY_INTERVAL` | 0 (dynamic) | | non-zero = fixed |
| `tt_RELIABLE_RETRY_INITIAL` | 1 ms | | until the first round trip |
| `tt_RELIABLE_RETRY_GRANULARITY` | 0 (measured) | | G; non-zero = fixed |
| `tt_TIMER_LATENESS_INITIAL` | 100 us | | G before its first sample |
| `tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE` | 64 | | interval ceiling in max(srtt, G) |
| `tt_RELIABLE_RETRY` | 3 | | immediate resends / plain retries before back-off |
| `tt_RELIABLE_RETRY_BACKOFF_SHIFT` | 4 | | back-off to 16x (~60 requests/s when all is lost) |
| `tt_RELIABLE_STUCK_WARN_INTERVAL` | 5 s | | stuck-gap warning |
| `tt_RELIABLE_BITMAP_BITS` / `_MAX_BITS` | 256 / 4096 | 256 / 8192 | reader window; 8192 holds an 11.3 MiB sample |
| `tt_IDLE_HEARTBEAT_PERIOD` | 250 ms | | P; a 10 Hz stream never pays it; 0 = off |
| `tt_MAX_RELIABLE_HISTORY` | 64 | | example cache depth |
| `tt_QOS_DEPTH_DEFAULT` / `tt_QOS_KEEP_ALL_DEPTH_DEFAULT` | 10 / 256 | | a zero `history.depth`: rmw's / one ACK window |
| `tt_QOS_HISTORY_BYTES_DEFAULT` / `tt_QOS_KEEP_ALL_BYTES_DEFAULT` | 1 MiB / 512 KiB | | a zero `history.bytes`, as rmw_tickle's |
| `tt_QOS_MAX_BLOCKING_DEFAULT_NS` / `tt_QOS_REORDER_SLOTS_DEFAULT` | 100 ms / 16 | | zero `max_blocking_ns` / `reorder_slots` |
| `tt_CLIENT_MAX_OUTSTANDING` | 4 | | calls per Client |
| `tt_CLIENT_CALL_WINDOW` | 8 | | answers kept per Client (2 x outstanding) |
| `tt_CALL_RETRY_INTERVAL` | 5 ms | | seed until a first answer |
| `tt_CALL_RETRY_GRANULARITY` | 0 (2 x G) | | adds the server's dispatch |
| `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` | 64 | | wait ceiling |
| `tt_CALL_DEADLINE_PER_SEND` | 250 ms | | a call fails within (count + 1) x this (1 s) |
| `tt_CALL_RETRY_COUNT` | 3 | | retries |
| `tt_SERVER_CACHE_GAP_MULTIPLE` | 4 | | answer lifetime in observed retry gaps |
| `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` | 5 s | | deferred slot lifetime |
| `tt_MAX_SERVER_CACHE_COUNT` | 64 | | 16 Clients at full depth |
| `tt_SERVER_CACHE_ENTRY_LENGTH`, `tt_CLIENT_CACHE_LENGTH` | 2 x buffer | 8 | inline storage (rmw attaches its own) |
| `tt_MAX_SCHEDULER_LENGTH` | 128 | 512 | scheduler entries |
| `tt_SCHED_INBOX_LENGTH` | 32 | | cross-thread timer inbox |
| `tt_SCHEDULER_IO_INTERLEAVE` | 8 | | due entries before an I/O check |
| `tt_RECEIVE_TIMEOUT` | 100 us | | poll slice some callers pass |
| `tt_RX_BATCH` | 32 (1 if buffer > control) | | datagrams per `recvmmsg()` |
| `tt_RX_CLOCK_REFRESH` | 16 | | datagrams per clock read |
| `tt_RX_LOCK_CHUNK` | 8 | | datagrams per lock hold, bounds a publisher's wait |
| `TT_RX_TIMER_LONG_NS` | 10 ms | | longer waits end on the `timerfd`; 0 = never |
| `TT_RX_TIMER_LONG_EXACT` | 1 | | re-arm whenever the deadline differs (0 = only earlier) |
| `TT_RX_TIMER_EDGE` | 1 | | edge-triggered `timerfd`, no `read()` |
| `TT_RX_TIMER_SLACK_NS` | 1 | | polling thread timer slack; 0 = leave it |
| `tt_HAL_RX_HINT` | AUTO (0) | | io_uring receive hint |
| `tt_HAL_FREERTOS_NETCONN` | 1 | | FreeRTOS netconn receive hint; 0 = BSD sockets |
| `tt_SOCKET_BUFFER_SIZE` | 1 MiB | 4 MiB | SO_SNDBUF/SO_RCVBUF request |
| `tt_SEGMENT_ENABLED` | 1 (0 FreeRTOS) | 1 | shared-memory transport |
| `tt_SEGMENT_BYTES` | 768 KiB | | ring budget: 512 slots of 1472 B |
| `tt_SEGMENT_SLOT_BYTES` | = control | | slot payload |
| `tt_SEGMENT_SLOTS` | derived (512) | | power of two |
| `tt_SEGMENT_BELL_FIFO` | 1 | | FIFO doorbell (0 = UDP) |
| `tt_SEGMENT_DRAIN_PER_POLL` | 4 x slots | | sockets not starved |
| `tt_SEGMENT_DEAD_READER_NS` | 1 s | | 2/7 of the silence, kept below it |
| `tt_SEGMENT_ATTACH_RETRY_SENDS` / `_REVALIDATE_SENDS` | 256 / 4096 | | counted in sends (ROADMAP 5a) |
| `tt_SEGMENT_STALL_PASSES` | 1000 | | wedged-head warning |
| `tt_SEGMENT_FENCE_HOLD_NS` / `_FENCES_SEEN` | 1 ms / 8 | | fence wait / remembered |
| `tt_SEGMENT_PROBE_EVERY_MAX` | 512 | | spin re-probe ceiling, epochs |
| `tt_SAMPLE_LENDING` | 1 (0 FreeRTOS) | 1 | receive-buffer lending |
| `tt_SAMPLE_RETAIN_MAX` | 16 | | samples held per context |
| `tt_RX_POOL_MAX` | 32 | | attached receive buffers |
| `tt_CONTEXT_ID_CLAIM` | 1 (0 FreeRTOS) | 1 | host-registry id claiming |
| `tt_LOCAL_DELIVERY` | 0 | 1 | in-process delivery |
| `tt_DISCOVERY_OPTIONS` | 0 | 1 | discovery range, static peers |
| `_tt_CONTEXT_PORT` | 8282 | + domain | well-known port |
| `_tt_CONTEXT_BROADCAST` | 255.255.255.255 | | set the link's directed broadcast |

Linux HAL environment switches (A/B controls, on unless `0`): `TT_UDP_OFFLOAD`, `TT_UDP_CSUM0`, `TT_RX_NONBLOCK`,
`TT_RX_DIRECT`, `TT_RX_LAZY_TIMER`, `TT_RX_LAZY_ADAPT`.

## 16. Where the older documents went

- Before 2026-10-05, planning documents (`SHM_PLAN.md`, `COMPARISON.md`, `PLAN.md`, ...): `git show 3c0c505b:<old path>`.
  Source comments citing them ("SHM_PLAN 6c") resolve there.
- Before 2026-10-17, this document with its pre-registrations (L1-L4, B1-B2, hybrid steps), evidence and incidents:
  `git show 228da18d:docs/DESIGN.md`. Before 2026-10-18: `git show 0a7926a0:docs/DESIGN.md`. A cited heading or
  criterion not found here resolves there.

- Where they live now: core plans (`SHM_PLAN`, `WIRE_PLAN`, `DATAFRAG_PLAN`, ...) here; `COMPARISON` in RESULTS.md; rmw
  plans in RMW.md; perf plans in TESTING.md and RESULTS.md; `SECURITY_PLAN` in ROADMAP.md, Parked; open items in
  ROADMAP.md; `README-rmw-perf.md` is RMW_PERF_SETUP.md.

## Containers: allowing io_uring

Docker (25.0+) and Kubernetes' `RuntimeDefault` seccomp block io_uring (and Linux 6.6+ has `kernel.io_uring_disabled`);
TickLE then runs a little slower for a busy publisher. Allowing it widens the kernel surface, so decide per deployment.
TickLE needs only `io_uring_setup` and `io_uring_enter`:

```sh
curl -sSLo seccomp-default.json https://raw.githubusercontent.com/moby/moby/master/profiles/seccomp/default.json
jq '.syscalls += [{"names": ["io_uring_setup", "io_uring_enter"], "action": "SCMP_ACT_ALLOW"}]' \
    seccomp-default.json > seccomp-tickle.json
docker run --security-opt seccomp=seccomp-tickle.json ...
```

Kubernetes: `seccompProfile: {type: Localhost, localhostProfile: <path>}`. Avoid `seccomp=unconfined`.
