# TickLE design

How TickLE works today, and why. Measured results are in [RESULTS.md](RESULTS.md), the ROS 2 layer in
[RMW.md](RMW.md), the test strategy in [TESTING.md](TESTING.md), and parked or future work in [ROADMAP.md](ROADMAP.md).
Struct and constant names refer to `include/tickle/tickle.h`, `config.h` and `hal*.h`; where this text and the code
disagree, the code is right.

This document describes the design only. Its history - pre-registered criteria, A/B and PC evidence, dated incidents,
options tried and dropped - is in the text before the 2026-10-17 rewrite, `git show 228da18d:docs/DESIGN.md`, with the
same section numbers. Older planning documents: section 16.

## 1. Overview

TickLE is a small real-time publish/subscribe and RPC middleware in C, aimed at controlled links (10BASE-T1S, TSN, a
single trusted L2 segment) and at microcontrollers as well as Linux. `rmw_tickle` puts ROS 2 on top of it.

Principles that shape everything below:

- **The core never allocates.** `src/` has no `malloc`/`free`. Every table is sized at compile time or attached by
  the caller, who also frees it. This is what lets one core run on FreeRTOS with no heap and inside a ROS 2 process.
- **The core does not depend on ROS 2.** ROS-shaped concepts live in `rmw_tickle`.
- **No threads of its own** (the user's rule). Core runs on the threads that call it; what a DDS vendor does in a
  receive thread is done from the poll loop or by the kernel.
- **Own wire protocol, not DDS.** UDP over IPv4, RTPS-like concepts (HEARTBEAT, ACKNACK, a built-in discovery
  endpoint), but smaller framing and not wire-compatible with any DDS vendor.
- **Every drop is counted.** A silent drop looks exactly like network loss, so each refusal path has a counter.
- **Wire changes must leave every test better** (the user's rule, 2026-09-26): targeted metrics improve beyond
  2xSE and nothing regresses beyond 2xSE. Every wire change bumps `tt_VERSION`.

## 2. Context, nodes, endpoints and the poll loop

A **context** is one TickLE participant: it owns the sockets, the timers and the poll loop. **Nodes** group endpoints
inside a context, as ROS nodes do, without each paying for a transport. Core never starts a thread, so the application
drives a context by calling its poll loop, while any other thread may call the API.

| object | owns | notes |
|---|---|---|
| `tt_Context` | sockets, scheduler, poll loop, discovery, liveliness, shared-memory segment | its id is the wire's `source` |
| `tt_Node` | a name, a namespace, an index in its context | up to `tt_MAX_NODES` per context |
| `tt_Publisher`, `tt_Subscriber`, `tt_Client`, `tt_Server` | per-endpoint state | each embeds `tt_Endpoint` and records its node's index |
| `tt_Topic`, `tt_Service` | type: sizes and encode/decode functions | shared by endpoints |

Index 0 is the default node, created on first use of a `tt_Context_create_*()` shorthand and named
`tickle_<context id>`; a context that creates only explicit nodes (every rmw_tickle context) has none.
`tt_Node_destroy()` refuses while an endpoint still lives on the node.

**The poll loop.** One thread at a time drives a context with `tt_Context_poll(ctx, timeout)`:

- `timeout = 0`: one non-blocking pass - run what is due, process what is already received. A high-rate publisher
  calls this between sends.
- `timeout < 0`: wait until the next scheduler entry is due, a datagram arrives, or `tt_Context_interrupt()`. With
  nothing scheduled it waits indefinitely: an idle node does not wake up.
- The wait is `epoll_pwait2()` (never `SO_RCVTIMEO`) over a set registered once: both sockets, an `eventfd` for
  wake-ups, the segment doorbell FIFO, and a deadline `timerfd`. `ppoll()` is the fallback without epoll or Linux 5.11.
- At most `tt_SCHEDULER_IO_INTERLEAVE` (8) due scheduler entries run back to back before a receive check, so a
  max-rate publisher cannot starve ACKNACK processing.

**Threading (concurrency).** With `tt_THREAD_SAFE=1` (default) every public `tt_*` call may come from any thread,
concurrently with the poll on another; `tt_THREAD_SAFE=0` compiles the locks out for a single-task microcontroller.

- One re-entrant state lock per context guards the context, its endpoints and its scheduler (re-entry is a thread-id
  compare). User callbacks run inside it and may call back into core.
- Nothing is held while the poll waits; the lock is taken per datagram (or per `tt_RX_LOCK_CHUNK`, 8, buffered
  datagrams) and per due scheduler entry.
- Timers from other threads go through a lock-free inbox (`tt_SCHED_INBOX_LENGTH`, 32) and wake the poll if earlier
  than what it waits for.
- A second concurrent poller gets `tt_RET_BUSY`. `tt_Context_lock()`/`_unlock()` give callers consistent compound reads.

**Receive path (Linux HAL).** Each wake-up should cost as few system calls as possible.

- `recvmmsg()` reads up to `tt_RX_BATCH` datagrams (32 at the 1472-byte datagram, 1 when the buffer is larger); the
  two sockets alternate first refusal. "Now" is read once per `tt_RX_CLOCK_REFRESH` (16) datagrams.
- **Both sockets are `O_NONBLOCK`**: Linux checksums a blocking UDP socket's first datagram, in a cold pass of its own,
  before reporting it readable; non-blocking, the checksum is made during the copy. Blocked sends wait for `POLLOUT`.
- **Lazy deadline timer.** After 8 waits in a row ended by data, the deadline is held in the `timerfd` (re-armed only
  when earlier) and the wait has no timeout, so a data wake-up starts no hrtimer; a fire restarts the count. It pays
  only on a stream subscriber's data wake-ups, not in a ping-pong. `TT_RX_LAZY_ADAPT=0`: the timer after every wait.
- **io_uring receive hint** (`tt_HAL_RX_HINT`): a busy loop's `tt_rx_maybe_ready()` reads an io_uring completion queue
  in shared memory instead of making an empty `recvmmsg()` (one-shot `POLLIN` only; datagrams are read normally; no
  thread). `tt_RX_HINT_AUTO` (0, default): used if the kernel allows it, else one warning (container seccomp,
  `kernel.io_uring_disabled`, Linux < 5.1; README.md has the seccomp recipe). `tt_RX_HINT_READ` (1): compiled out.
  `tt_RX_HINT_URING` (2): required, else context creation fails with `tt_RET_UNSUPPORTED`.

**Receive path (FreeRTOS HAL).** lwIP's socket layer keeps its netconn callback to itself, so the HAL is on the netconn
API (`tt_HAL_FREERTOS_NETCONN=1`, default; `0` = BSD sockets and `select()`, kept as the test control).

- Each of the two receive netconns has a callback, run by lwIP itself (no TickLE thread): `RCVPLUS` (tcpip thread,
  after the datagram is queued) adds 1 to that netconn's atomic arrival count and gives one binary semaphore;
  `RCVMINUS` (the reading task, on the take) subtracts 1, never below 0.
- `tt_rx_maybe_ready()` is "no" only when both counts are 0: two loads, no lwIP call. A datagram is queued before it
  is counted, and a count falls only on a take or on a read that finds the mailbox empty (and then only if no arrival
  was counted meanwhile), so no queued datagram is left uncounted; a stale count is a false "maybe", which that empty
  `NETCONN_DONTBLOCK` read clears.
- `tt_receive()` waits on the semaphore, which `tt_wake_signal()` also gives (no loopback wake socket).
- Tests: the QEMU selftest (`platform/freertos/main.c`, `make test-freertos`) checks the hint false once drained, true
  with a datagram queued and false straight after it is read; `test.sh` runs it against three HAL mutants (arrival
  not counted, take not counted, hint always "maybe"), each of which must fail its own check.

**Time.** Two clocks, as in DDS, so that a step of the date (NTP, `date -s`) moves no deadline: on one clock a forward
step expired leases and lifespans early and a backward step stalled every timer.

| HAL call | Linux | FreeRTOS | used for |
|---|---|---|---|
| `tt_get_ns()` | `CLOCK_MONOTONIC` | the CLINT counter | every deadline and duration: scheduler, retries, heartbeats, liveliness and leases, lifespan, waits (`timerfd`, `tt_Context_lock_timed()`), G |
| `tt_get_wall_ns()` | `CLOCK_REALTIME` | the same counter; a board with SNTP returns the date here | the DATA timestamp, the source timestamp a subscriber is handed, the per-launch `entity_id`/incarnation draws |

- Dates are derived, not read: the monotonic reading plus `tt_Context.wall_offset_ns` (date minus monotonic, re-read
  every liveliness run, 1 s). A publish reads one clock (`publish_clock()`); a receiver rebuilds the 32-bit stamp with
  it, choosing only the 71.6-minute span, so a slew changes nothing and a step beyond +-35.8 min can misplace stamps
  for up to that second.
- rmw_tickle: `received_timestamp` is the date; a subscription's lifespan takes a sample's age once on arrival (date
  minus source timestamp, at least 0) and counts the rest on the monotonic clock, on which its condition variables wait.
- Tests: `tests/test_clock_step.c` (steps the date +-3 h; fails a single-clock build), `platform/linux/clock_check.c`,
  `tests/mutants_clock_split.py`.

## 3. Wire format (`tt_VERSION` 11)

Every datagram is UDP: a 4-byte header and one or more small submessages, RTPS-like but with less framing - a sample
alone in its datagram carries 20 bytes of TickLE header. The version is checked strictly, so two builds either speak
the same protocol or ignore each other. (The title keeps the version this section was written for; the current
`tt_VERSION` is **13**, and the versions list below says what 12 and 13 added.)

**Transport.**

- Each context has two sockets: the **well-known socket** (wildcard address, port `_tt_CONTEXT_PORT` 8282; rmw_tickle
  adds the ROS domain id) receives broadcasts; the **data socket** (own address, ephemeral port) sends everything and
  receives unicast. A peer's unicast address is the source address of its datagrams.
- Broadcast goes to `_tt_CONTEXT_BROADCAST` (default `255.255.255.255`, which follows the default route: set the
  link's directed broadcast). Discovery is broadcast, never multicast (the user's decision).
- Up to `tt_MAX_LINK_COUNT` (4) links per context, each with its own broadcast and unicast threshold.
- A datagram is at most `tt_MAX_BUFFER_LENGTH` bytes of UDP payload (1472 by default: no IP fragmentation; section 8
  says when large samples may use it).

**Datagram layout.** A `tt_Header` followed by submessages (type-length-value, each padded to 4 bytes), or, for exactly
one submessage addressed to everyone, a single 4-byte `tt_SingleHeader`:

```
classic: tt_Header        magic[2] "KT"/"TK" | version | source
         tt_SubmessageHeader  type | receiver | length (bytes, header included)
         body ...         (repeat, 4-byte aligned)
single:  tt_SingleHeader  marker 'k'/'t' | version | source | type
         body ...         (to the end of the datagram)
```

- Senders write native byte order; the magic/marker tells the receiver whether to swap. Receivers swap every field
  core interprets; payload decoders get an `is_native_endian` flag.
- A datagram of another `tt_VERSION` is dropped and logged once per source: no partial compatibility.
- `source` is the sending context's id (1-254, unique on the link, section 12); `receiver` a context id or
  `tt_SUBMESSAGE_ID_ALL` (0xff). Padding bytes are zeroed.

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

**Identifiers.**

- `endpoint_id`: 32-bit FNV-1a hash of topic/service name and endpoint name, computed locally on each node.
- `entity_id`: one endpoint instance within its context; (source, entity_id) names a writer network-wide.
- `seq_no`: per writer and **per datagram** (section 8), compared modulo 2^32 (`seq_delta()`).
- `timestamp`: low 32 bits of the sender's date in microseconds; rebuilt assuming the clocks agree within +-35.8 min.

**Framing cost** of a sample in one datagram: 4 B header + 16 B `DATA` header = 20 B (24 B with rmw_tickle's 4-byte
publication sequence number), against 42 B of Ethernet/IPv4/UDP.

**Payload encoding (TickLE CDR-4, the interface serialization).** Not OMG CDR. Host-native byte order; each primitive
aligned to `min(size, 4)` from the payload start; strings `u16 length` (including the NUL) + bytes + pad to 4;
variable arrays `u16 count` + elements; nested messages inlined with no header. Alignment is capped at 4 so batched
submessages, padded to 4, keep every payload aligned. Generated structs use `#pragma pack(push, 4)`, so an
all-fixed-size message is encoded and decoded in place. Decoded strings alias the receive buffer and are valid only
during the callback. rmw's direct codec writes a sequence count of 65,535 or more as the escape 0xFFFF plus a uint32.

**Versions that changed the wire:** 5 ACKNACK bitmap widened; 6 per-subscriber ack identity; 7 discovery became a
DATA of a built-in endpoint; 8 periodic summary + pulled list; 9 liveliness flag; 10 32-bit timestamp, one-word rmw
psn, single header; 11 nodes in the announce; 12 large-sample types 11/12; 13 hybrid runs (14/15) and the announce's
`ip_reassembly_max` and `loss_p`. `LARGE_REF` never crosses the network and changed no version.

## 4. Sending: flush, batching and unicast peers

A publish goes out at once, by unicast to the few readers that want it or by broadcast when there are more, and a
writer with no matched reader sends nothing, as a DDS writer does. Batching is opt-in, for bursts of small samples.

- **RPC always flushes immediately**: a caller is waiting.
- **Publish flushes immediately by default.** `pub->batch = true` defers to the `tt_CONTEXT_TX_INTERVAL` (1 ms) tick
  and coalesces samples into one datagram.
- A shared datagram (a batch, an announce) never exceeds `tt_CONTROL_MAX_LENGTH` (1472), so a node on core defaults
  can always receive it.
- **Unicast to a few, broadcast to the rest.** Each Publisher and Client keeps a `peers[]` table (`tt_MAX_PEER_COUNT`
  8) of remote contexts with a matching endpoint, learned from discovery. More than the link's
  `tt_UNICAST_PEER_THRESHOLD` (2) means broadcast, otherwise one unicast per peer. A full table counts as more than
  the threshold, so a reader with no slot still gets the broadcast. A Server answers unicast to the request's source;
  a Client with no known Server broadcasts its call.
- **A Publisher with no matched reader sends nothing** (the user's decision, 2026-10-15). A reader is matched from its
  announce, or for a RELIABLE writer from its ACKNACK, and gets what is published from then on.
  - A BEST_EFFORT sample is dropped (`tx_unmatched_dropped`); a RELIABLE or durable one still takes its seq_nos and
    enters the history (`tx_unmatched_kept`), so a VOLATILE reader starts at its match point and a TRANSIENT_LOCAL one
    gets the history.
  - It covers every path (small, fragmented, large, zero-copy, batched) and every HEARTBEAT sent unprompted. Local
    delivery, discovery (still broadcast) and answers to an ACKNACK (section 6) are unaffected.
  - The cost: the first sample after a reader starts waits for its announce - one `tt_CONTEXT_TX_INTERVAL`, or one
    `tt_CONTEXT_UPDATE_INTERVAL` if that announce is lost. A reader presumed gone gets nothing until its next summary;
    one that never announces (muted by an id collision) gets nothing. A program that must not lose its first samples
    waits for `tt_Publisher_matched_count()` to be non-zero (the uint64 examples wait up to 5 s).
- Datagrams ready at once (a sample's fragments, one datagram to several peers) go in one `tt_send_batch()`, one
  `sendmmsg()`.

**UDP offload (Linux HAL).** A large sample is hundreds of same-size datagrams to one destination, each paying the
socket layer on both sides; the kernel can pay it once per run with the wire unchanged. On by default (the user's
decision, 2026-10-12); `TT_UDP_OFFLOAD=0` turns both halves off. Compiled out without `tt_LARGE_SAMPLES`.

- **Send, `UDP_SEGMENT`.** In one `tt_send_batch()`, a run of a large sample's MTU fragments (types 11/12, each at
  most 1472 B, the same size but the last) to one destination goes as one message of at most 64 datagrams / 65507 B,
  cut back into exactly those datagrams by the kernel or NIC (`platform/linux/udp_offload_check.sh`). Large fragments
  only: for a run of two it costs more than it saves.
- **Receive, `UDP_GRO`.** Where the buffer holds a merged read (65507, rmw builds), one read may return a run, handed
  to core one datagram at a time in place (`tt_Context.rx_offset`, 4-aligned; at 4 mod 8 a loaned take decodes instead
  of reading in place). `UDP_GRO` is on only from a large sample's fragment until 128 reads without one (a GRO read
  costs more even unmerged), and goes off only when both queues are seen empty, so no merged read is taken without its
  size (`hal_linux.c`, "When receive offload is on"). Refused by the kernel or after `EIO`: off.
- With the hybrid transport (section 8) both apply to MTU-mode traffic only: a run is one IP-fragmented datagram,
  never segmented and never merged (IPv4 fragments bypass GRO), and a sample's lone MTU tail neither turns `UDP_GRO`
  on nor keeps it on.
- Traffic line: `udp_offload`, `gro_*`, `gso_*`.

## 5. Discovery

Discovery tells each context which endpoints exist on the link. In steady state it costs one ~28-byte summary per
node per second whatever its endpoint count: a node pulls a peer's full list only when the summary says it changed.

Discovery is ordinary traffic on a built-in endpoint: endpoint_id `tt_DISCOVERY_ENDPOINT_ID` (0), entity_id
`tt_DISCOVERY_ENTITY_ID` (all ones).

| role | submessage | addressed | contents |
|---|---|---|---|
| summary, every `tt_CONTEXT_UPDATE_INTERVAL` (1 s), or a sixth of the shortest own lease | HEARTBEAT | broadcast | first = last = the context's **generation** (low 32 bits of `last_modified`) |
| request for the list | ACKNACK | unicast to the summary's sender | the generation wanted |
| the list (announce) | DATA, or FRAG_FIRST + FRAG_CONT | unicast to the requester | `tt_AnnounceHeader` + `tt_UpdateEntity` records |
| a change (endpoint created/destroyed) | the list | broadcast, at once | the new generation |

- The generation changes exactly when the endpoint list does and differs across restarts. A summary with an applied
  generation is liveliness only.
- Any other generation draws one request, retried after the measured round trip plus `tt_CONTEXT_TX_INTERVAL`
  (`tt_DISCOVERY_REQUEST_RETRY`, 10 ms, until one is measured), up to `tt_DISCOVERY_REQUEST_ATTEMPTS` (4) times, in
  `tt_DISCOVERY_PENDING_REQUESTS` (8) slots.
- Up to `tt_UNICAST_PEER_THRESHOLD` requests per tick are answered unicast; more get one broadcast, so a burst of
  joining nodes costs one broadcast. A changed list heard by broadcast, or an unknown node's, is answered once with our
  own list, unicast.
- **A large list is fragmented at entity boundaries.** Each fragment has its own `tt_AnnounceHeader` and whole
  entities and is applied as it arrives: no reassembly memory. A new generation replaces the old list on its first
  fragment; a lost fragment is filled by the next request. At most `tt_UPDATE_MAX_PARTS` fragments (32, 255 in rmw
  builds; about 13 ROS endpoints a fragment); a context whose list needs more sends no announce and is invisible.
- **Nodes are announced too** (v11): one `tt_UpdateEntity` per explicit node, `kind = tt_KIND_NODE`, the namespace as
  type and the node name as name. Every entry carries its node's index in 8 spare bits of `kind` and `qos`, masked off
  by receivers (`tt_UPDATE_KIND_MASK`, `tt_UPDATE_QOS_MASK`). About 40 B per node.
- **The announce header** (v13) also carries the hybrid transport's `ip_reassembly_max` and `loss_p` (section 8).
- **The discovery table** (`tt_MAX_DISCOVERED_ENTITIES`, 552 B per entry) holds every remote entity. RxO checks,
  KEEP_ALL writer classification, per-entity leases and rmw graph queries read it, and for an entity it has no room
  for RxO **fails open** - so size it to the remote entities a context will see. Drops are counted
  (`entities_dropped`) and warned once. Above 64 entries it is indexed by open addressing (`tt_DISCOVERY_INDEXED`).
- **Discovery range** (`tt_DISCOVERY_OPTIONS`, rmw builds): `SUBNET`, `LOCALHOST` or `OFF`, and links marked `peer` for
  static unicast peers (`ROS_AUTOMATIC_DISCOVERY_RANGE` / `ROS_STATIC_PEERS`). Same-host discovery is UDP too, on Linux
  and FreeRTOS (the user's decision, 2026-10-09).

## 6. Reliability

RELIABLE works as in DDS/RTPS: the writer keeps what it sent, the reader asks for the gaps it sees, the writer resends
from its history. Repair is per datagram, so a lost fragment costs one datagram, not a sample. The timers are
measured (round trip, timer lateness), not fixed, so one build works on a microcontroller and on a loaded PC.

**Writer side.**

- A Publisher with `reliable_cache` (caller-owned `index[]` + byte arena) retains each sent datagram as a record;
  `reliable` (RELIABLE) and `durable` (TRANSIENT_LOCAL) share it. `capacity`/`arena_size` are its RESOURCE_LIMITS;
  `sample_depth` makes KEEP_LAST count whole samples; `tt_RELIABLE_CACHE_ARENA_BYTES(depth, max_record)` sizes the
  arena with one record of slack.
- **No resend cap**, as in RTPS: a datagram in the history is resent for as long as a reader asks. The first
  `tt_RELIABLE_RETRY` (3) resends answer every request at once; after that a datagram goes at most once per retry
  interval (nack suppression; a reader re-asks for repairs in flight). KEEP_ALL and large samples have no such bound;
  the reader's back-off bounds them.
- An ACKNACK naming an evicted seq_no is answered with a HEARTBEAT carrying the real `first_available_seq_no`, so the
  reader skips exactly what is gone; a RELIABLE Publisher with no history answers the same way.
- Repairs and these HEARTBEATs go to the ACKNACK's sender alone, matched or not: the no-send rule (section 4) stops
  only what is sent unprompted. An ACKNACK matches a reader only to a Publisher with a history (`claim_from_acknack()`).
- Ack state is per remote Subscriber entity: `tt_MAX_ACK_ENTRIES` (16) fixed entries (24 B each, 40 more with the
  hybrid transport), which is also how many readers a RELIABLE writer can have; the next is not matched and is logged
  (rmw_tickle: `RMW_TICKLE_MAX_RELIABLE_READERS`).

**Reader side.**

- A writer proxy per writer holds a received bitmap: `tt_RELIABLE_BITMAP_BITS` (256 datagrams) by default, a wider
  attached window up to `tt_RELIABLE_BITMAP_MAX_BITS` (4096; 8192 with large samples). Out-of-order datagrams wait in
  a caller-owned reorder buffer; delivery is in order.
- A gap is NACKed at once; unanswered requests repeat on a per-proxy retry timer.
- **Retry interval** (`tt_RELIABLE_RETRY_INTERVAL` 0 = dynamic, default; non-zero is used as given), RFC 6298 over
  request-to-recovery times, from `tt_RELIABLE_RETRY_INITIAL` (1 ms):

  `interval = srtt + max(G, 4 x rttvar)`, at most `tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE` (64) x max(srtt, G).

- **G, the host's timer lateness, measured** (`timer_lateness_fold()`, per context). A sample is how late a retry-timer
  wait (`acknack_retry`, `call_retry`) returned past its deadline, taken only from waits that ran to that deadline and
  came back no later than they were long (other deadlines are excluded: Linux's slack grows with the wait,
  max(50 us, 0.1%)). Smoothed with RFC 6298's gains (1/8, 1/4), `G = mean + 4 x mean deviation`, floored at
  `tt_timer_resolution_ns()`; `tt_TIMER_LATENESS_INITIAL` (100 us) before the first sample.
  `tt_RELIABLE_RETRY_GRANULARITY` non-zero fixes it.
- **A busy writer's send is not the path.** A writer in a large send answers only behind it, so the reader keeps the
  path's `recovery_srtt_ns`, which leaves out recoveries whose wait saw the writer's original fragments arrive after
  the request (`tt_WriterProxy.probe_busy`, Karn's rule in spirit), and `sending_srtt_ns`, which takes all. While
  originals still arrive, the timer ticks at the path's interval but asks no oftener than the sending estimate
  (`retry_sent_ns`), names only what was asked longer ago than the repair transit time (`repair_in_flight_ns()`), and
  asks nothing if no repair came within the interval (`repair_arrival_ns`). The first tick after the send asks for
  every gap.
- **No give-up on a count.** A gap ends when its datagram arrives, when a HEARTBEAT says it is gone (`gap_evicted`),
  or with the writer (departure or a lapsed lease drops the proxy). A window overflow still jumps (`gap_abandoned`).
- **Back-off, from a writer gone quiet only.** After the first `tt_RELIABLE_RETRY` plain retries, each retry that finds
  the writer's stream no further than the last doubles the wait, up to 2^`tt_RELIABLE_RETRY_BACKOFF_SHIFT` (2^4 = 16)
  times: ~60 requests a second, not 1000, when everything is lost. While the writer's DATA still arrives the wait
  stays plain. A recovery past the plain retries is not folded into srtt. KEEP_ALL gaps keep the plain interval.
- A gap open past its plain retries warns every `tt_RELIABLE_STUCK_WARN_INTERVAL` (5 s); KEEP_ALL from the first retry.
- Early re-requests cause some duplicate repairs (about 0.2 per loss on veth); they hedge against lost repairs and are
  kept on purpose.

**KEEP_ALL flow control.** `pub->keep_all` never evicts an unacknowledged datagram: the write is refused with
`tt_RET_WOULD_BLOCK` (nothing sent or cached) and `writable_callback` / `tt_Publisher_writable()` says when to retry.
The bound is `min(cache depth, narrowest tracking window any matched Subscriber announced)`.

**ACK solicitation.** A reader ACKNACKs only on a gap, so on a lossless link a KEEP_ALL writer would stop at its bound;
it solicits acks with a non-FINAL HEARTBEAT when unacked reaches half the bound and again after each further
half-bound, one in flight at a time, the next as soon as an ACKNACK answers (the retry interval only presumes an
unanswered one lost). A refused write solicits too, and `keep_all_resolicit()` repeats it every retry interval while
refused, so a lost request cannot block the publisher for good. Other writers opt in with `ack_solicit_watermark_pct`;
`ack_solicit_period_ns` adds a periodic solicitation.

**Idle HEARTBEAT** (the user's decision, 2026-10-16; `tt_IDLE_HEARTBEAT_PERIOD`, `tt_Publisher.idle_heartbeat_ns`). A
reader learns its writer's last sample was lost only from something sent after it. So while a matched RELIABLE reader
has not acknowledged everything and nothing has told the readers everything for a period P, the writer sends a
non-FINAL HEARTBEAT (with the match point ahead of it while one is owed), and again every P while something stays
unacknowledged.

- "Told everything" is a HEARTBEAT that announced everything sent to every matched reader, or a published sample.
- One scheduler entry per writer, moved in place by a publish past half the period (`sched_postpone_locked()`), so
  samples at most P/2 apart never let it run and a burst's last sample gets its HEARTBEAT P/2 to P after it.
- Silent when all have acknowledged, with no matched reader, and for a BEST_EFFORT reader (`tt_PeerAck.reliable_reader`).
- **P = 250 ms**, floored at 4 retry intervals: a 10 Hz request/reply stream never pays it, and it is short next to the
  DDS defaults (Cyclone DDS 100 ms, OpenDDS 1 s, Fast DDS and RTI Connext 3 s). Tests: `tests/test_idle_heartbeat.c`.

## 7. Services (RPC)

A Client sends a request and the Server's callback answers. The client retries on a measured timer, and the server
caches its answers so a retry never runs the callback twice. A Client may have a few calls outstanding, each in a
fixed slot, because core has no malloc.

- **Up to `tt_CLIENT_MAX_OUTSTANDING` (4) calls per Client** (the user's decision, 2026-10-16). Each has its own slot -
  seq_no, retry timer, and its encoded request in an even share of `cache_buf` (or attached storage) - so calls are
  answered in any order; the callback names its call (`tt_Client.callback_seq_no`, as `tt_Client_call_id()` gave it).
  Call N + 1 is refused with `tt_RET_ILLEGAL_STATUS`, nothing sent. 1 gives the one-call behaviour exactly (the unit
  tests also run so, `tests-one-call`).
- Slot memory: `tt_CLIENT_CACHE_LENGTH` (2 x buffer: 2,944 B at core's datagram, 11.8 KB a Client at 4) + 24 B.
  rmw_tickle sets the inline area to 8 B and attaches a slot per call sized for the request type's largest encoding
  (65,512 B for an unbounded type).
- Seq_nos come from the context's one counter, so several Clients of one service in one context, and several calls of
  one Client, are distinct, and each answer reaches the call it names. On wrap, a seq_no a call still waits on is
  skipped. Each Client of a service in a context has its own `client_tag` (1..255). The round trip is timed from
  before the send.
- **Retries:** `call_retry_interval` if set. Otherwise RFC 6298 over call-to-answer times, shared by the Client's calls:
  - first wait `srtt + max(G, 4 x rttvar)`, each retry twice the one before, each at most
    `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` (64) x max(srtt, G); the seed `tt_CALL_RETRY_INTERVAL` (5 ms) stands in for srtt
    until a first answer;
  - a call gives up after `(2^(count+1) - 1)` first waits (15 at `tt_CALL_RETRY_COUNT` 3), and never later than
    `(count + 1) x tt_CALL_DEADLINE_PER_SEND` (4 x 250 ms = 1 s);
  - a timeout doubles srtt (up to that deadline), once for calls lost together; the next answer replaces the estimate;
  - G is twice the reliable retry's (section 6): this host's timer plus the server's dispatch, which cannot be measured
    from here. `tt_CALL_RETRY_GRANULARITY` non-zero fixes it.
- **Response cache.** `tt_MAX_SERVER_CACHE_COUNT` (64) slots keyed by (source context, `client_tag`, seq_no), so a
  retry gets the same answer without the callback.
  - A new call proves nothing about which earlier answer arrived, so a **call window** stands in: a Server keeps
    `tt_CLIENT_CALL_WINDOW` (2 x `tt_CLIENT_MAX_OUTSTANDING`, 8) answers per calling Client, oldest call first out, and
    a Client makes no call while one of its outstanding calls is that many calls old - so the answer let go is always
    to a finished call. Both ends must agree on the constant (the server's no smaller).
  - An answer is kept for the longer of the client's seed schedule (or `call_retry_interval x (count + 1)`) and
    `tt_SERVER_CACHE_GAP_MULTIPLE` (4) x the longest recent response-to-retry gap of that client; a served retry
    re-arms it; a retry after expiry teaches the server that gap.
  - Dropped when its client turns out to be a new incarnation. A full cache evicts the response sent longest ago, or
    the caller's own when it already holds a full depth, so a fast Client cannot push out a slow one's answer. 64 slots
    hold 16 Clients calling at full depth. A late duplicate older than every kept answer is answered, not kept.
- **Deferred responses:** a callback returns `tt_CALL_DEFERRED` and answers later from any thread with
  `tt_Server_send_response()`, which encodes and sends before it returns. Up to `tt_MAX_SERVER_CACHE_COUNT` requests
  are held deferred; one that finds the table full is dropped and its retry brings it back. An unanswered slot is
  reclaimed after `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` (5 s); a retry of a pending request does not re-invoke the
  callback.
- **Turned away:** `tt_CALL_IGNORED` sends, caches and keeps nothing; the next retry asks again. rmw_tickle returns it
  when its queue of untaken requests is full.
- Requests and responses never fragment: they are bounded by `tt_MAX_BUFFER_LENGTH`.

## 8. Fragmentation and large samples

A sample larger than one datagram is split by TickLE, not by IP, into fragments that are repaired one at a time: OS IP
fragmentation failed 97.4% of reassemblies at 5% loss. Up to 65507 B a sample takes the small path below. Larger
samples (stage 2, to about 11.3 MiB) take their own wire types and caller-supplied buffers, go to same-host readers
through shared memory (step B), and may travel as IP-fragmented runs while a peer's path is clean (the hybrid
transport). The pre-registrations (L1-L4, B1-B2, the hybrid criteria) and the A/B records behind this section are in
the pre-rewrite text (section 16).

**Sizes.**

| constant | meaning | core default | rmw build |
|---|---|---:|---:|
| `tt_MAX_BUFFER_LENGTH` | largest datagram (service messages may use it whole) | 1472 | 65507 |
| `tt_CONTROL_MAX_LENGTH` | largest shared datagram and fragment size | 1472 | 1472 |
| `tt_MAX_SAMPLE_LENGTH` | largest sample on the small path | = buffer | = buffer |
| `tt_FRAG_ENABLED` | compiled in when sample > control | 0 | 1 |
| `tt_LARGE_SAMPLES` | stage 2 compiled in | 0 | 1 |

At core defaults fragmentation compiles out (no reassembly pool, no larger `tx_buffer`) and an oversized sample is
refused. `-Dtt_FRAG_ENABLED=0` forces IP fragmentation (a benchmark arm only).

**Wire.** Fragments are alone in their datagram and unpadded. Every fragment but the last is full and fragment 0
carries `tt_FRAG_FIRST_SHORTFALL` (7) bytes less, so position follows from the index and no offset field is needed. At
most `tt_FRAG_MAX_COUNT` (64) fragments.

**Every datagram takes its own seq_no** (the user's decision, 2026-09-26). A k-fragment sample consumes k consecutive
seq_nos and is named by its first; a fragment's sample is `seq_no - frag_index`. So a lost fragment is resent alone
(`k / 0.95` datagrams at 5% loss instead of `k / 0.95^k`); the callback's seq_no is monotonic, not contiguous (rmw
carries its own contiguous counter); the reliable cache holds one record per datagram, KEEP_LAST evicts whole samples
and KEEP_ALL admits a sample only if all its datagrams fit.

**Sender.** The sample is encoded once in `tx_buffer` and its fragments sent from there in one `sendmmsg()`, after
anything batched ahead. Retransmissions send cached fragments unpadded.

**Receiver.**

- RELIABLE: a fragment in order (its seq_no is the writer's watermark and the subscriber holds nothing) is assembled
  in the node's `frag_scratch` and delivered from there without the reorder buffer, one sample at a time per node
  (`frag_fast_take()`). Any other fragment goes into the subscriber's reorder buffer, and anything that would disturb
  the sample in the scratch first moves its fragments into their (free by construction) reorder slots. A fragment is
  recorded as received only once kept. A sample that can never be whole is dropped and counted
  (`reorder_abandoned`); a torn sample is never delivered.
- BEST_EFFORT: `tt_FRAG_REASSEMBLY_SLOTS` (8) slots keyed by (source, entity_id, sample seq_no); when full, a
  completed slot is reused first, then the oldest reassembly abandoned (`frag_duplicate`, `frag_abandoned`,
  `frag_dropped`).
- A node built without fragmentation skips types 8 and 9. A fragment-assembled small sample cannot be lent (section
  10): it is retained by copying.

### Stage 2: samples above 64 KB (designed 2026-10-09; step A on main 2026-10-10; step B built 2026-10-11, rig pending)

Stage 2 carries ROS images and point clouds of 1-8 MB (1080p rgb8 is 6.2 MB, a 64-beam lidar cloud about 3 MB)
without any static buffer that size. Its shape comes from user decisions: TickLE still splits, never IP, and each
datagram keeps its own seq_no (2026-09-26); the new wire types, per-datagram seq_no at this size, the 8 MiB default
and the ~11.3 MiB ceiling (2026-10-09). Step A (the socket path) and step B (the same-host region) are both on main.

**Bounds.** The reader window must hold one sample's datagrams, so it sets the ceiling: 8192 bits with stage 2 (its
ACKNACK, 1,052 B, still fits one control datagram), so at most about **11.3 MiB** (1,446 + 1,452 x 8,191 B). rmw
defaults to 8 MiB (`RMW_TICKLE_MAX_SAMPLE_BYTES`; refused above it with an error naming the limit) and checks a topic
type, a fixed-size one too (perf_test's `Array1m`), against it rather than one datagram; services keep the datagram.
Datagrams per sample: 1 MB 723, 4 MB 2,889, 8 MiB 5,778.

**Wire (`tt_VERSION` 12).** Types 11/12 are 8/9 with 16-bit `frag_index`/`frag_count` (shortfall 6). **Every sample
above `tt_MAX_SAMPLE_LENGTH` uses them, and only those**, so the receiver routes by type and never guesses, and every
sample that could be sent before is byte-identical. A node without stage 2 skips them and counts
`frag_large_skipped`. Fragment positions use the receiver's own `tt_CONTROL_MAX_LENGTH`: a large sample between nodes
with different control datagrams is dropped and counted (`large.dropped`).

**Storage: caller-acquired, sized per sample.** A context takes `large_acquire(user, bytes) -> void *` and
`large_release(user, ptr)` (`tt_Context_set_large_buffers()`). rmw backs them with `malloc` and a free list, a core-only
user with a static pool. `NULL` is "no room", never a crash.

**Publisher.**

- Core acquires a buffer of `data_encode_size`, encodes into it ("Encoding while sending") and sends straight from it:
  `sendmmsg()` of up to 64 datagrams, two iovecs each (framing, slice), no copy into `tx_buffer`.
- It **retains the buffer by reference** (`tt_Publisher.large[]`, up to `tt_LARGE_RETAINED` 16; one index entry for
  the sample's k seq_nos) and resends a fragment by rebuilding its framing; copying into the arena would cost `depth`
  x 8 MiB. Released: BEST_EFFORT after the last fragment; VOLATILE when every matched reader has acknowledged; DURABLE
  when KEEP_LAST evicts it. KEEP_LAST's depth counts small and large samples together, oldest first
  (`keep_last_bound_across_kinds()`).
- **Send cursor.** A 4 MB burst overruns any socket buffer, so on `EAGAIN` (`tt_send_batch_nonblocking()`) a
  per-Publisher cursor sends the rest from `tt_Context_poll()` once the socket reports room (`tt_tx_await_writable()`:
  on Linux a duplicate of the data socket armed one-shot for `EPOLLOUT`, reported when half the buffer is free),
  behind a 20 ms fallback timer (`tt_LARGE_SEND_WRITABLE_FALLBACK_NS`, longer than half a buffer takes at 1 GbE).
  Without that report (`timeout == 0` polling, FreeRTOS, `-Dtt_LARGE_SEND_ON_WRITABLE=0`) it retries on a timer from
  50 us, doubling to 1 ms (`tt_LARGE_SEND_RETRY_NS`, `_MAX_NS`).
- **A publish while a send is in progress** waits behind it without seq_nos (`large_pending`), and a newer one replaces
  it before any of it goes (`large.tail_abandoned`); KEEP_ALL gets `tt_RET_WOULD_BLOCK`. The sample in flight is never
  abandoned, so a link-bound stream delivers at the link's rate and no gap is left for readers.
- **End-of-sample HEARTBEAT** (`tt_LARGE_END_HEARTBEAT`): a RELIABLE writer sends a non-FINAL HEARTBEAT after each
  large sample's last fragment, so a lost tail is NACKed one round trip later. It can be lost with the tail, so while
  the sample is unacknowledged it is chased: resent one retry interval on, doubling to 100 ms. It and its resends
  carry the match HEARTBEAT ahead of them while a reader is owed one; otherwise, with the match HEARTBEAT and unit 0
  both lost, it would be a VOLATILE reader's first contact and set its baseline past the sample. The periodic
  HEARTBEAT (`tt_Publisher_set_heartbeat_period()`, off by default) carries the match point for the same reason.
- **A sample longer to send than a reader waits is repaired while it goes:** a HEARTBEAT announces only seq_nos sent,
  and a requested datagram that has gone is resent at once.

**Subscriber.** The first fragment that arrives (both types carry `frag_count`) acquires `frag_count x 1452` bytes in
one of `tt_LARGE_ASSEMBLIES` (4), keyed by (source, entity_id, sample seq_no). Each fragment is copied to its offset,
so order does not matter and neither the reorder ring (which keeps its size) nor `frag_scratch` is used. A fragment is
recorded only once stored; delivery is in order. No buffer: RELIABLE leaves the fragments unrecorded (NACKed later -
the reader's back-pressure), BEST_EFFORT drops the sample (`large_no_buffer`); never torn. Copies: kernel to receive
buffer, to the large buffer (none with "Receiving runs in place"), then rmw's decode.

**Lending** (`tt_LEND_LARGE`). The callback's sample points into the large buffer; `tt_Sample_retain()` keeps it and
`release` hands it to `large_release`, otherwise core releases it when the callback returns. It blocks no ring and
counts against `tt_SAMPLE_RETAIN_MAX`. A claimed-slot publish (section 10) never covers a large sample, and commits one
waiting behind a send before taking its own seq_no.

**Flow control (KEEP_ALL).** The bound stays `min(cache depth, narrowest window)` in datagrams. A matched reader whose
window is below one sample's count could never admit it, so the publish fails with `tt_RET_TOO_LARGE`
(`large_window_too_small`), not an endless `WOULD_BLOCK`. rmw gives a type with an unbounded sequence the 8192-bit
window (1 KiB per tracked writer) and KEEP_ALL byte budgets of at least two max samples. Comparison cells keep the
equal-bound rule (the user, 2026-10-06): DDS N samples, TickLE (N + 1) x the sample's bytes.

**Where it is compiled.** Only where asked (`tt_LARGE_SAMPLES`; rmw_tickle sets it), since it widens every
window-sized array. FreeRTOS has no buffer callbacks and too small a heap, so it compiles out there. Out of scope:
services above 64 KB, rmw loans of `Image` (its wire bytes are not its message), large samples in a local durable
backlog (`tt_Subscriber_deliver_local_backlog()`).

#### Step B: the same-host large path

Over the shared-memory ring (section 10) a 4 MB sample would be 2,889 records, each a claim, a copy in and a copy out,
and the 512-slot ring cannot hold one, so a full ring tears every BEST_EFFORT sample. Step B encodes the sample once
into a chunk of a **Publisher-owned shared region** and sends each same-host reader one descriptor; the reader takes
the sample in place and gives the chunk back. The descriptor's `seq_span` is the sample's datagram count, so the seq
space is the socket path's exactly.

**The region** (per Publisher, opt-in, sized by the caller, because its sizes are a Publisher's: the type's largest
sample and the depth).

- `tt_Publisher_set_large_region(pub, chunks, chunk_bytes)` before the first large publish; `tt_LARGE_CHUNK_BYTES(max_cdr)`
  and `tt_LARGE_REGION_BYTES(chunks, chunk_bytes)` size it; at most `tt_LARGE_REGION_CHUNKS_MAX` (64) chunks. Not
  set: step A's fragments. rmw (`rmw_tickle_large_region_attach()`, off with `RMW_TICKLE_LARGE_REGION=0`) uses
  `chunks = min(depth, tt_LARGE_RETAINED) + tt_LARGE_REGION_SPARE` (3: one being sent, one waiting, one in a reader's
  callback) and `chunk_bytes` from the type's bound, else `RMW_TICKLE_MAX_SAMPLE_BYTES`.
- A reader in another process maps it by name, so core creates it through the HAL like the segment
  (`tt_segment_create()`: fresh, owner's `flock()` held by the mapping) at the first large publish to a peer that
  takes descriptors, as `/dev/shm/tickle-large-<ip>-<port>-<context id>-<entity id>` (within `tt_SEGMENT_PATH_LENGTH`,
  64), and unlinks it with its Publisher or context. Pages become resident only when written.
- Layout: a 64-byte header (magic `TKLG`, `tt_LARGE_REGION_VERSION` 1, owner ip, port, context id, entity id, a
  per-creation `incarnation`, `chunks`, `chunk_bytes`), a 64-byte entry per chunk (`generation`, and `readers`, one bit
  per context id), then the chunks, 64-aligned so the CDR lands at 4 mod 8 as in a receive buffer.

**Writer.**

- A chunk is free when the writer holds no record in it and `readers` reads 0 (acquire). The lowest free chunk is
  taken (the resident set stays small) and its `generation` bumped before the encode. None free: KEEP_LAST evicts its
  oldest record and looks again; still none, or a sample above `chunk_bytes`: fragments (`large_ref.region_full`).
- The chunk is the record's buffer, so retention, resends, release and fragments to remote peers are step A's.
- A peer whose segment is attached and advertises descriptors gets one `LARGE_REF` record - its bit set first, then
  the record published (the slot's release store orders them) - once the sample is fully encoded. A full ring drops
  the record (`segment_full_dropped`) and the writer clears the bit (`large_ref.ref_dropped`).
- A NACK into a chunked sample from such a peer gets one descriptor per ACKNACK (`acknack_serial`); a NACK into a sample
  still being encoded is left to the send in progress.
- **Dead reader:** when liveliness removes a peer (or another peer appears behind its id), the writer clears that id's
  bit in every chunk. Not before: a slow reader keeps its chunks.

**Reader.**

- `LARGE_REF` (13) is accepted only from the segment and refused from a socket (`rx_shm_only_on_socket`): it names
  memory. It is handled in `process_submessage()`'s default case, so the DATA switch is unchanged.
- The reader maps the region by name, checks header, owner and incarnation, keeps up to `tt_LARGE_PEER_REGIONS` (8)
  mappings (LRU among the unreferenced), and checks each descriptor: chunk in range, length within it, `generation`
  equal, its own bit set. A failure is dropped and counted by reason (`large_ref.ref_refused`); RELIABLE asks again.
- RELIABLE: the chunk becomes an already-whole assembly under the record's span, delivered in order by the ordinary
  drain (behind a gap it waits, without a copy). BEST_EFFORT: delivered at once. Lent as `tt_LEND_LARGE`.
- The bit is cleared when nothing of this context references the chunk: no assembly, lent sample, delivery or
  descriptor in process (`tt_LargeRefState.ref_processing`, so several Subscribers share one bit). A sample dropped
  unfinished gives its chunk back. The KEEP_LAST skip planner never passes over a descriptor (it is OTHER), so no
  bit is left set.

**Compatibility.** The segment header's reserved `uint16_t` is `features`; bit 0 (`tt_SEGMENT_FEATURE_LARGE_REF`)
says "takes descriptors". Older builds wrote 0, so a reader without the feature (FreeRTOS, core without
`tt_LARGE_SAMPLES`) gets fragments and no version bump is needed. Compiled in with `tt_LARGE_SAMPLES` and
`tt_SEGMENT_ENABLED` (`tt_LARGE_SHM`). Step B's fields sit last in their structs, so no hot field moves. The span bound
keeps clear of `tt_SEGMENT_SPAN_FENCE` (static assertion).

**Accepted limits.** A reader that cannot map the region holds its bit until liveliness removes it (the writer runs on
fewer chunks, then fragments). A peer removed by liveliness while alive can have a chunk reused under it - the trust
the segment's dead-reader rule already places in liveliness. A durable late joiner's backlog gets fragments
(`large_send_backlog_before()`). The region's pages count in both processes' RSS.

Tests (stage 2 and step B): `tests/test_large_sample.c`, `test_large_skip.c`, `test_large_shm.c`,
`test_large_region.c`, `wire_identity_largemsg.sh`, mutants `tests/mutants_large_sample.py`, `mutants_large_region.py`.

### Encoding while sending (2026-10-11, `ab/interleave-encode`)

Nothing needs the whole sample encoded before its first fragment goes, so a large publish encodes each part just
before the send call that carries it. That takes one whole-sample copy (about 0.7 ms for 4 MB on a Pi 5) off the
latency path, with a byte-identical wire.

- **API.** `tt_Topic.data_encode_range(data, payload, offset, len)` (optional, `tt_DATA_ENCODE_RANGE`) writes bytes
  `[offset, offset + len)` of what `data_encode` writes; a topic that sets it promises `data_encode_size` is exact and
  the bytes are `data_encode`'s. rmw_tickle offers it where the wire bytes already sit in memory - a serialized
  message, or a type whose wire bytes are its message (`inplace_bytes`: perf_test's `Array*`) - as the psn header
  plus a `memcpy`; other types decline and are encoded whole.
- **Sender.** Core asks for fragment 0's bytes before committing the sample (a decline there is the whole-encode
  path). Each send call - up to `LARGE_BATCH` (64) units to the first destination; with runs, runs until 64 units -
  first encodes through its last byte. Later destinations, resends and the backlog read what is encoded. On `EAGAIN`
  the rest is encoded at once, while the buffer drains, so when the publish returns the record is whole (the caller's
  sample is valid only within the publish) and retention, eviction, resends and LIFESPAN are unchanged.
- **Encoded whole** when the sample waits behind a send (`large_pending`), goes by descriptor (step B), or is taken
  in-process (`tt_LOCAL_DELIVERY`). A range declined part way is made up by `data_encode` of the whole sample; if that
  fails, the sample is dropped (`tt_RET_PROTOCOL_ERROR`, `large.encode_failed`), its buffer goes back and its seq_nos
  read as gone, so nothing torn is delivered.
- Tests: `tests/test_large_sample.c` (the wire with and without it), `tests/wire_identity_hybrid.sh`.

### Hybrid transport for multi-datagram samples (designed 2026-10-11, revised the same day; the rig campaign not run)

TickLE fragments survive loss but cost CPU per datagram; a 64 KB datagram the kernel IP-fragments costs far less CPU
but is lost whole if any of its ~45 frames is (`(1 - p)^45`: 64% arrive at 1% loss). The hybrid transport sends the
cheap form while the path is clean and the safe one while it is not: per peer, the writer chooses how many MTU
fragments (**units**) go in one IP-fragmented datagram (a **run**), from the loss its reader measures. The user lifted
the IP-fragmentation park for this (2026-10-11), set the objective to network bytes only, and required every input to
be measured by TickLE itself. On main (`tt_VERSION` 13); the rig campaign is not run.

Scope: every sample of more than one datagram, on 8/9 and on 11/12. Each unit keeps its own seq_no. `n = 1` is the MTU
fragment; `n_max = min(45, floor(peer's advertised reassembly bytes / unit))` (45 units fill a 65,372-B payload).

**The objective and the control law (the user, 2026-10-11).**

1. *Network only.* `n` minimises wire bytes per delivered unit (CPU and memory are not in the objective). RELIABLE,
   repairs as MTU units:

   `Cost(n) = (u + h / n) + (1 - q(n)) (u + h)` per delivered unit, `q(n) = (1 - p)^n`,

   `u` the unit payload (1,454 B on 8/9), `h` an MTU unit's overhead (TickLE framing and UDP, 26 B). For small `p`,
   `n* ~= sqrt(h / (p (u + h)))`; the writer takes the exact minimum over `1..n_max`:

   | frame loss `p` | 0 | 0.001% | 0.01% | 0.1% | 0.5% | 1% |
   |---|---:|---:|---:|---:|---:|---:|
   | `n*` | 45 | 42 | 13 | 4 | 2 | 1 |
   | `Cost(n*) / u` | 1.0004 | 1.0009 | 1.0027 | 1.0085 | 1.0191 | 1.0281 |
   | `Cost(1) / u`, `Cost(45) / u` | 1.0179, 1.0004 | 1.0179, 1.0009 | 1.0180, 1.0050 | 1.0189, 1.0452 | 1.0230, 1.2059 | 1.0281, 1.3707 |

   BEST_EFFORT: while `q(n) = (1 - p)^n` a sample arrives with probability `(1 - p)^k` whatever `n`, so it uses the
   largest `n` the reader's cap allows.
2. *Only what TickLE measures.* The reader measures frame loss `p` and run survival `q(n)` from its own reception (no
   `/proc`, no sysctl), owns the cap (reassembly memory is the host's, shared by every writer to it), and advertises
   both in the announce (`ip_reassembly_max`, `loss_p`). The writer takes `n <= cap` minimising `Cost(n)` with that
   `p`. Not in the ACKNACK: best effort has none, and the cap and `p` are the reader's, not a pair's.
3. *The control law* (after TCP CUBIC and BBR, not halving), run by the reader once per announce interval:
   - (a) It remembers `n_fail`, where runs collapsed (the collapse test below), and drops the cap at once to
     `max(1, round(0.7 n_fail))` (`n_fail - 1` up to 5).
   - (b) It re-probes above that ceiling only on time: `2 tau` after the collapse (doubling per failed probe, to
     `16 tau`), `tau` the reassembly-queue lifetime. In between it grows back fast (half the remaining distance a tick)
     and plateaus at `n_fail - 1`. A probe holds `ceil(1.25 n_fail)` for `tau`: clean, the ceiling moves up to
     `ceil(1.25 x` the probed `n)` (at most `n_max`); collapsed, it is a collapse like any other. A one-unit cap has
     no runs and so no window: time alone moves it on.
   - (c) The collapse point scales as `1 / (R p)` (`R` units a second reaching the reader). When `R p` has fallen by
     `tt_HYBRID_SCALE_AT` (1.9x) from its anchor for three ticks in a row, `n_fail` scales up in proportion (at most
     4x; past `n_max`, no ceiling) and a probe may go at once; when it has doubled, `n_fail` and the cap scale down.
     `p` enters only once `p_mtu` rests on 10 events at both ends.
   - (d) Frame loss consistent with `(1 - p)^n` never moves the cap; only excess whole-run loss does.
   - (e) A path that drops IP fragments goes to `n = 1` at once (below, on both sides).

   The estimator sits behind `loss_estimate_observe()` / `loss_estimate_p()` and the run window beside it, so a
   time-varying estimator (ROADMAP, Later) replaces only those.

**What the reader measures.** From each fragment-path datagram (writer, sample, first unit, unit count, run or not) the
reader rebuilds what was sent: units `[0, k - 1)` in runs of the writer's `n` (learned from a full run), unit `k - 1`
alone. A unit past the next expected position leaves a gap, which is loss; one below it is a repair and counts for
nothing; a repair addressed to the reader (`rx_targeted`) is never a first transmission. A datagram's layout is learned
before the gap in front of it is judged. A sample lost whole (runs and tail) shows no gap of its own, so it is counted
from the writer's seq_no: a forward jump of whole samples of the size on both sides, at most `HYBRID_MISSED_MAX` (8),
is that many samples lost; anything else (sizes differ, small samples between, longer, backwards) is left alone.

- `p_mtu`: lost over sent MTU units (tails, `n = 1` samples) - the frame-loss baseline, which reassembly cannot touch.
- **Estimating p** (advertised). Every frame is exposure; a lost MTU unit is one event on one frame, a lost run one
  event on its `n` frames. `p = (events + 1) / frames`, the posterior mean under a flat prior: `Cost(n)` is nearly
  linear in `p` where `n` is chosen, so that `p` gives the least expected wire bytes (an upper bound would hold `n`
  below `n_max` at 0% loss). The window halves at 2^20 frames. A collapse makes `p` read high, which only shortens a
  RELIABLE writer's `n`.
  - **None seen:** no event in `tt_HYBRID_NONE_SEEN_FRAMES` (500) frames is announced as "none seen" (`n_max`), the
    first time at the sample's tail. The 500 frames must be independent chances to lose, so to a reader with no `p` yet
    (`hybrid_peer_unmeasured()`, including one claimed from its ACKNACK) a RELIABLE AUTO writer sends MTU units one
    datagram a send call, never as a `UDP_SEGMENT` train, and the first estimate goes at the first tail.
  - **Stale events are forgotten** (the user's decision, 2026-10-15): up to `tt_HYBRID_STALE_EVENTS` (8) events, once
    the clean frames `C` since the last, against the frames `F` before it, give `(1 + C / F)^k >= 1e4`
    (`tt_HYBRID_STALE_FALSE_FORGET`), or `C >= 9.21 / p_min` (`tt_HYBRID_STALE_EXPOSURE / tt_HYBRID_STALE_P_MIN`,
    9,210 frames). A steady path has `P(C >= c F) = (1 + c)^-k` at any rate, and leaves `9.21 / p` frames clean with
    probability `exp(-9.21)`, so each rule forgets a real loss rate (at `p >= p_min`) at most 1e-4 of the time per `k`
    (up to 2e-4 together at `p = p_min`). Below
    `p_min` forgetting costs little (`Cost(45)` 1.0050 `u` against `Cost(n*)` 1.0027 at 0.01%).
- `q(n)`: runs arrived over runs sent, with their sizes, since the last decision; per writer too (read by rule (e)).
- `R`: units a second in multi-unit samples, arrived or inferred lost; `D_s`: run datagrams a second per source.

**The collapse test.** Over a window of `R` runs (at least 16), `X` lost, mean run size `s`, with
`L0 = 1 - (1 - p_hi)^s` the most run loss frame loss alone allows (`p_hi` an upper bound of `p_mtu`):

- **collapse** when `z = (X - R L0 - 0.5) / sqrt(R L0 (1 - L0)) >= 3.09` (one-sided 0.1%) with `L0` at `p_mtu`'s
  **99.9%** bound, **and** `X >= 4` and `X >= 2 R L0` with `L0` at the **95%** bound; where `2 L0 > 1` the second part
  reads on the survival side, `1 - X / R <= (1 - L0) / 2` (amended 2026-10-17: a 95% bound in the z part fired falsely
  on young baselines; `examples/perf_hil/experiments/collapse_fp_*`);
- without an MTU baseline `L0` is 1 and nothing is decided;
- **clean** when the window has 16 runs and `X <= R L0` (95%): only a clean window grows the cap; a window that is
  neither gathers more runs (up to 1,024).

It is a run-level test because a 1 MB sample has one MTU unit for 17 runs: per frame it would need hundreds of tails.

**The remembered ceiling is the collapse point, not the overshoot.** Reassembly memory fills over up to `tau`, so a
fast-grown cap can be far past the collapse point when it shows. The kernel then admits about the share that keeps it
at its threshold, so `n_c ~= n q / q0` and `n_fail = max(2, min(n, ceil(n q / q0)))`. The cap starts at `n_max`
(`tt_HYBRID_SLOW_START` 0). Right after the memory fills `q` reads near 0, so the ceiling errs low and probes climb back.

**Rule (e), reader side.** A sample that arrives to its tail with every run lost marks a path that drops IP fragments
when frame loss alone would do that with probability below `1e-6` (`L0^runs` over the writer's streak of such samples)
and no run of that writer got through for `tau` (runs that did mean a squeeze, which the window's test judges). Then:
one unit at once, announced at once, the law frozen, and after `2 tau` (doubling while it fails) a re-probe at the old
cap. Any cap change makes every writer's layout unknown again.

**Rule (e), writer side (RELIABLE).** Every run of a sample first-NACKed while the tail was not - on a sample of two
runs or more, a second single-run sample in a row, or a re-probe - is a failed path: that reader gets MTU units for
`tt_HYBRID_PATH_CLEAN_NS` (60 s, 2 tau at the default), then one single-run sample re-probes. `p` alone would not find
it, since "every run lost" reads as frame loss near `1 / n`. Re-NACKs are not events and repairs are not exposure.

**The queue lifetime `tau`.** An incomplete reassembly lives `min(ipfrag_time, 65536 / datagram rate)` (the IP ID
wraps). TickLE cannot read `ipfrag_time`, so `tau = min(30 s, 65536 / D_s)` over the sources sending runs, 30 s
(`tt_HYBRID_QUEUE_LIFETIME_NS`, the kernel's default) when none is. Every wait of the law derives from it. The model
(`q(n) = (1 - p)^n` below `ipfrag_high_thresh`, collapse above it; lwIP's 8 pbufs for 15 s on FreeRTOS) was checked on
a Linux veth; the evidence is in the pre-rewrite text.

**Settings** (not QoS: delivery semantics do not change, and rmw has no such field). In `_tt_CONFIG` until
`tt_InitOptions` exists (ROADMAP "QoS reshaped like rmw").

| setting | where | values | default |
|---|---|---|---|
| `large_fragmentation` | `_tt_CONFIG` | `tt_LARGE_FRAGMENTATION_AUTO` (computes `n`), `_MTU` (`n = 1`), `_IP` (`n = n_max`) | `AUTO` |
| `ip_reassembly_max` | `_tt_CONFIG`, advertised as a ceiling | largest datagram accepted from IP reassembly; 0 = none | Linux 65,507; FreeRTOS 0 |
| `reassembly_cap_off` | `_tt_CONFIG` | 1: advertise the ceiling uncapped (a forced arm's reader) | 0 |
| `large_fragmentation`, `ip_datagram_max` | publisher | override, or inherit (0) | inherit |

The sender takes the more conservative of its setting and the peer's advertisement. `n = 1` for an undiscovered peer,
one advertising 0 or under two units, a same-host peer reached through its segment, and every repair and backlog. What
a context advertises is capped by its receive buffer, so a core-default build advertises 1 unit. FreeRTOS advertises 0
and builds without the hybrid sender. rmw_tickle reads `RMW_TICKLE_LARGE_FRAGMENTATION`,
`RMW_TICKLE_REASSEMBLY_CAP_OFF` and `RMW_TICKLE_RUN_UNITS` for forced arms.

**Wire (`tt_VERSION` 13).** Types 2, 8, 9, 11, 12 unchanged; one run type per geometry, so that a repair is exactly the
MTU fragment of its pair: `FRAG_RUN_L` (14) for 11/12 and `FRAG_RUN` (15) for 8/9 (headers in section 3; `seq_no` is
the first unit's).

- The payload is the units concatenated, copied to the offset `frag_index` gives. A run is self-describing, so a reader
  can acquire on it, and marks `run_count` bits. Type 15 is received into what 8/9 use. The bitmap, window and KEEP_ALL
  bound stay in units; a lost run is `n` missing units, repaired as MTU fragments.
- **A sample's last unit always goes as an MTU fragment**: units `[0, k - 1)` in runs of `n` (the last shorter), unit
  `k - 1` alone, runs only when `k >= 3`. At `n = 45` 1 MB is 18 datagrams instead of 723. The tail needs no
  reassembly, so it tells a reader its runs were lost and gives the frame-loss baseline.
- **Announce:** `ip_reassembly_max` (the cap in bytes: framing plus `cap` units of 8/9, read back as `cap` units of
  either pair), `loss_p` (0 = no estimate, read as 1; else exponent `e` in the high byte, mantissa `m` in the low,
  `p = (256 + m) / 2^(e + 9)`, rounded up), 2 B reserved. A change of the cap, or of `p` by a factor of 2, is a new
  generation, at most once per announce interval.
- By unicast each peer gets its own `n` from the one buffer; a broadcast uses the minimum `n` over the matched peers.
  Repairs keep their addressing; discovery stays broadcast and MTU-sized. A node without stage 2 skips 14
  (`frag_large_skipped`); one without fragmentation skips 15. UDP offload never applies to runs.

Tests: `tests/test_hybrid_send.c` (objective, rebuilding, loss code, collapse test, every rule, and a simulated link
with the kernel's reassembly modelled), `tests/mutants_hybrid_send.py`, `examples/perf_hil/experiments/hybrid_send_netns.sh`.

#### Saturation: where I4s lost its samples, and the receive drain (2026-10-11)

With no receive thread, nobody reads the socket while the callback decodes a 4 MB sample, and the next sample at line
rate overflows it (the Pis cap the buffer at 425,984 B, ~2 ms of 1 GbE). DDS vendors read from a thread of their own;
TickLE has the kernel do it: the **receive drain** (`tt_rx_drain_start()`, hal.h; Linux, compiled with io_uring and
`tt_LARGE_SAMPLES`).

- While core is away from the sockets in a large stream (before a large sample's callback, and when a poll that
  delivered one returns), an io_uring multishot `recvmsg` per socket copies arriving datagrams into HAL buffers. The
  kernel runs it as task work of the polling thread whenever that thread is interrupted or enters the kernel, so a
  thread busy in user space still has its socket emptied. No thread of ours; a preempted thread overflows as a
  descheduled vendor thread would.
- Buffers: `tt_RX_DRAIN_BYTES` (4,352 KiB) as 2,048 x 1,536 B for fragments or 64 x 65,600 B for reassembled
  datagrams, allocated at the first drain. Core asks for the reassembly ceiling it advertises (`large_rx_drain()`), so
  a run is never cut.
- `tt_receive()`/`tt_try_receive()` hand out what it took, oldest first: order is kept. A drain pass leaves a fully
  armed drain running; only a wait stops it, its cancellations going with that wait's `io_uring_enter()`, and it stops
  only when it holds nothing, before any socket read.
- A datagram longer than a buffer is dropped, counted, and holds drains off for 100 ms so its retry is read whole.
  ENOBUFS (every buffer held) re-arms once an eighth of the buffers is back (`drain_rearms`), and a drain that overran
  starts next time with twice the buffers, up to `tt_RX_DRAIN_BYTES_MAX` (4x).
- `UDP_GRO`'s state moves only on core's own reads; the drain asks for GRO's size cmsg and hands a merged read out one
  datagram at a time (`hal_linux.c`, "The drain and receive offload").
- Without io_uring or multishot `recvmsg` (before 6.0) the context is unchanged, said once. FreeRTOS: no drain. The
  default core build's `tt_receive()`/`tt_try_receive()` are instruction-identical without it.

**Receiver drops are not path loss.** A datagram the receiving host dropped (a full socket, or cut by the drain) is a
receiver behind, which a smaller `n` does not cure. The HAL's count (`tt_rx_receiver_drops()`) is put against the
reader's gaps, one drop per lost datagram; those gaps reach neither `p`, `p_mtu`, the run window nor rule (e)
(`hybrid: receiver_drops= receiver_lost=`). A drop unmatched for two announce intervals is let go.

**An excused gap is not clean.** The count cannot say whose datagrams were dropped, so an excused gap may hide path
loss. Before the reader's first `p`, one restarts the estimate and that first `p` waits for 500 frames after it
(`hybrid_rx_announce_code()`; the writer sends MTU units meanwhile). After, it is a doubt
(`tt_LossEstimate.frames_at_doubt`): "none seen" counts from it, and the stale rule takes it as the `k`-th event's
point, so its 1e-4 bounds still hold.

**Instruments.** The HAL's "Node N rx:" line: `socket_drops` per socket (SO_MEMINFO), the drain's starts, datagrams,
cut datagrams and overruns. `rmw_keepall_rig.sh KCOUNTERS=1` adds each Pi's kernel counters. Test:
`platform/linux/rx_drain_check.c`.

#### Receiving runs in place (2026-10-13, `ab/rx-gap`)

A run's payload is one stretch of the sample's assembly, and in order the next run is predictable to the byte, so the
HAL reads it straight into the assembly: the kernel's copy lands where the payload stays and core's own copy
(`large_place()`) disappears.

- After a run lands core hints the next (`large_direct_aim()`, `struct tt_LargeState.direct_*`), and the HAL reads the
  next datagram with one three-piece `recvmsg()`: the 28 framing bytes to the receive buffer, the hinted payload to the
  assembly, any excess after it (`direct_read()`). The hinted run, or a shorter run of the same sample from the same
  place, is handed over (`direct_split`) and only marked; anything else is put back together and processed as before.
- **Only over units not landed:** the hint is withdrawn when a unit lands another way (a repair, a drained datagram).
- **Not across threads:** an assembly freed by another thread during an aimed read (`direct_busy`) keeps its buffer
  (`direct_parked`) until the read ends, and that run is dropped (`direct_cancelled`; RELIABLE resends it).
- **A miss backs off:** no hint for 2, 4, ... 64 runs after consecutive misses; native-endian writers only (framing is
  compared byte for byte). `gro_read()` takes aimed reads too, never in place for a merged read. `TT_RX_DIRECT=0` turns
  it off. Rx line: `direct_reads`, `direct_runs`, `direct_missed`, `direct_cancelled`.
- **No empty read after a run:** after any run read the socket is marked idle for the drain; the next wait
  (level-triggered) reports anything that came.
- **A run's units at once:** a run is routed once, not per unit, and a RELIABLE run past the watermark inside the window
  is recorded as one span (`update_reliable_ack()` with `rx_seq_span`) with one drain walk. Units are stamped from the
  poll's clock reading.
- Tests: `tests/test_rx_direct.c`, `platform/linux/rx_direct_check.c`, `tests/mutants_rx_direct.py`.

#### UDP checksum 0, on by default (2026-10-14, `ab/udp-csum0`; the default since 2026-10-15)

No NIC checks an IP fragment, so a run's UDP checksum is computed in software at the receiver in a pass of its own.
With checksum 0 the receiver skips it; the Ethernet CRC still covers every frame. On by default (the user's decision,
2026-10-15, on the rig A/B in RESULTS.md); `TT_UDP_CSUM0=0` keeps the checksum - do so on a path through a router or a
link without a frame CRC.

- A `sendmmsg()` call carrying a datagram above 1472 B sets `SO_NO_CHECK` on the data socket, kept until a call carries
  a `UDP_SEGMENT` run, which the kernel refuses with it (EINVAL): a batch is cut where the kind changes. While it is
  on, one-frame datagrams may go with checksum 0 too. Only the receiver gains: the sender checksums during its copy.
- Rx line: `csum0`, `csum0_switches`, `csum0_calls`, `csum0_datagrams`, `csum0_retries`. Not on FreeRTOS.

## 9. QoS: liveliness, deadline, lifespan, durability

TickLE implements the DDS QoS policies ROS 2 uses. Each endpoint announces its QoS in discovery and incompatible pairs
are not matched (request vs offered, RxO); liveliness is a lease renewed by any sign of life, so flowing data needs no
extra traffic.

**RxO matching.** `tt_UpdateEntity` carries `qos` bits (reliable, durable, manual liveliness) and the deadline and lease
durations. A Subscriber drops DATA from an incompatible writer and counts it (`rxo_drops`).

**Liveliness** (DDS semantics, `tt_VERSION` 9): a lease runs from the last sign of life.

- AUTOMATIC: any datagram from the entity's context. MANUAL_BY_TOPIC: only that writer's DATA or a HEARTBEAT with
  `tt_HEARTBEAT_FLAG_LIVELINESS` (`tt_Publisher_assert_liveliness()`).
- One scheduler entry per context, `check_liveliness()`, runs at the earliest expiry and re-arms; a refresh never
  touches the timer. A lapsed entity is tombstoned (discovery callback with `departed`) and revives on its next sign
  of life.
- A context is presumed dead after `tt_LIVELINESS_SILENCE_NS` = (2 x `tt_LIVELINESS_MISS_THRESHOLD` + 1) / 2 intervals
  = 3.5 s (three missed summaries), or its longest announced lease, up to `tt_CONTEXT_MAX_LEASE_NS` (10 s). Its peer,
  ack and writer-proxy state is dropped and re-learned from its next summary.
- An idle node's summary is its only sign of life, so it goes every interval or every lease /
  `tt_LIVELINESS_LEASE_DIVISOR` (6), whichever is sooner; under traffic extra short-lease summaries are skipped when
  every peer has heard from us since the last tick (`summaries_skipped`).
- While datagrams are still unread the check defers, up to `tt_LIVELINESS_MAX_DEFERRALS` (4) x
  `tt_LIVELINESS_DEFER_NS` (1 ms), so a descheduled process does not declare live peers dead.

**Deadline.** `deadline_duration_ns` is announced and used for RxO; rmw_tickle, not core, enforces it.

**Lifespan.** `lifespan_duration_ns`: a cached sample older than this (monotonic clock) is neither retransmitted nor
replayed to a late joiner. Readers can apply their own expiry from the DATA timestamp, the writer's date.

**Durability (TRANSIENT_LOCAL).** A `durable` Publisher unicasts its retained cache, oldest first, to a newly
discovered Subscriber, recorded per (context id, announce generation): a restarted peer gets the backlog again, a
continuing one does not. A durable Subscriber refuses a VOLATILE writer.

**Local delivery** (`tt_LOCAL_DELIVERY`, rmw builds): a publish is also handed in-process to the context's own matching
Subscribers under the same QoS rules; otherwise a context drops its own DATA as self-sent.

## 10. Shared-memory transport

Same-host peers exchange datagrams through a shared-memory ring instead of loopback UDP. It is a second carrier, not a
second protocol: a slot holds the datagram that would have gone on the wire, through the same acceptance path
(version, RxO, context-id rules). Core picks the carrier per peer. On top of it a subscriber may keep a sample without
copying it (lending) and a publisher may build a sample in the reader's ring (claimed-slot publish). On by default on
Linux (`tt_SEGMENT_ENABLED`), off on FreeRTOS.

**The segment.**

- **One per receiving context**, read by its owner and written by every same-host peer sending to it, named
  `/dev/shm/tickle-seg-<ip>-<port>-<context id>` (the address separates network namespaces, which share `/dev/shm`).
- **The header is the authority, not the name:** magic, `tt_SEGMENT_VERSION` (6), the owner's (ip, port, id), a
  per-launch `incarnation`, the geometry (`slots`, `slot_bytes`) and `features` (section 8, step B). A writer validates
  the header, then maps the region with the owner's geometry; any mismatch falls back to UDP, counted by reason.
- **Fixed-size slots**, a power of two, indexed with a mask: a 16-byte header (`length`, sender address and port,
  `seq_span`, `sequence`) and `slot_bytes` of payload. A writer claims an index by compare-and-exchange, fills the slot
  and publishes `index + 1` with release; the reader takes it once its sequence says finished and frees it with
  `index + slots`. A record never straddles the wrap.
- **Geometry:** `tt_SEGMENT_BYTES` (768 KiB) / (16 + `tt_SEGMENT_SLOT_BYTES`), rounded down to a power of two: 512 slots
  of 1472 B. Size it as `slots >= publish rate x worst reader stall`. `_tt_CONFIG.segment_slot_bytes` sets the slot at
  run time (0 = default, at most a control datagram); a type that does not fit an explicit slot fails
  `tt_Context_create_publisher()`, and on the default goes over UDP (`segment_oversized_to_udp`).
- **`seq_span`** is how many seq_nos a record covers, so a record carried whole where the network would fragment keeps
  the seq space identical. It lives in the slot header and so cannot come from the network. Whole-record sends need a
  slot larger than a control datagram (build-time only; no faster). `tt_SEGMENT_SPAN_FENCE` (0x8000) marks a fence.

**Lifecycle (lazy).**

- A context builds its segment when the first same-host peer appears (or it delivers to itself) and releases it when
  liveliness removes the last one: no same-host peer, no cost. `tt_segment_create()` unlinks any stale file first;
  teardown unlinks.
- **No writer uses a dead owner's segment.** The owner holds an exclusive `flock()` for as long as the region is mapped
  (the mapping holds it, so a crash releases it), and `tt_segment_attach()` refuses a file nobody holds
  (`tt_SEGMENT_ORPHANED`, `shm_attach_orphaned`); the peer is reached over UDP meanwhile. One `flock()` per attach and
  revalidation, none per datagram. Writers never unlink an orphan, since `unlink()` could remove a successor's file.
- A writer re-asks every `tt_SEGMENT_ATTACH_RETRY_SENDS` (256) sends when absent and `tt_SEGMENT_REVALIDATE_SENDS`
  (4096) when attached, so a late peer is found and a replaced owner noticed (counted in sends, not time: ROADMAP 5a).
- A reader that takes nothing for `tt_SEGMENT_DEAD_READER_NS` (2/7 of the liveliness silence, 1 s, kept below it by a
  static_assert) while records wait is given up: UDP until the next recheck.
- A writer that dies between claim and publish wedges the head; the owner warns after `tt_SEGMENT_STALL_PASSES` (1000).

**One path per peer, and drop on full.** Once attached, every datagram to the peer goes through its segment. A full
ring **drops** it (`segment_full_dropped`) rather than reroute it to UDP, where it would overtake the queued records and
make the reader discard them as older. So under load the segment loses samples where UDP slows the sender;
`shm_full_dropped` sees what a subscriber's loss figure cannot, and a larger ring only moves the threshold. UDP still
carries broadcasts, datagrams larger than a slot (service messages in rmw builds) and traffic before the attach;
`tx_udp = broadcast + oversize + unattached`. The reader drains at most `tt_SEGMENT_DRAIN_PER_POLL` (4 x slots) records
per poll so the sockets are not starved.

**The switch from UDP is fenced.** A writer that reached a peer by UDP and then attaches still has datagrams in the
peer's socket, and the reader drains its ring first, so it would discard those queued BEST_EFFORT samples as older.

- On that attach the writer sends a 3-byte fence (marker, context id, generation; `tt_SEGMENT_FENCE_LENGTH`) to the
  peer's data socket and puts the same bytes in the ring as a record with `seq_span = tt_SEGMENT_SPAN_FENCE` before its
  first ring datagram (`tt_SEGMENT_VERSION` 6). A revalidation or a first attach sends none.
- The reader does not pass a fence until its datagram has come out of the socket, which it reads next; one read first
  is remembered (`tt_SEGMENT_FENCES_SEEN`, 8); one never seen is given up after `tt_SEGMENT_FENCE_HOLD_NS` (1 ms), a
  scheduler entry bounding the sleep. Counted: `shm_fences_sent`, `_passed`, `_expired`.
- It is tested only on failing or stall branches (the span check, the drain's stall path, `process_packet()` finding no
  header), and its fields sit at the end of `tt_Context`, so ordinary traffic pays nothing. Landed on the user's
  decision as a correctness fix. Test: `tests/test_segment_switch.c`.

**Doorbell: a FIFO, rung once per reader sleep.**

- Before blocking, the owner writes a new non-zero **sleep generation** to `reader_waiting` and drains once more; it
  writes 0 on waking. A writer reads `reader_waiting` after publishing a record, with a full fence between store and
  load on both sides so no record sits unseen (`platform/linux/bell_wake_check.c`), and rings once if it is non-zero and
  not the generation it last rang for. A busy reader costs no doorbells; a dead one is rung once.
- The bell is a named FIFO beside the segment (`<segment>.bell`, `tt_SEGMENT_BELL_FIFO=1`), opened on attach, rung with
  a one-byte non-blocking write, in the owner's wait set. A peer without one is rung with a zero-length UDP datagram
  (never a valid TickLE datagram).
- The bell is edge-triggered in epoll, so a ring costs the reader no `read()`; the pipe is emptied every capacity /
  (4 x `tt_MAX_CONTEXT_IDS`) generations (64 at 64 KiB), keeping it under half full. A kernel that does not report every
  write to an unread pipe (before 5.14) gets a level-triggered bell, read on every ring.
- Before announcing a sleep the reader may wait instead, at most what a sleep costs, for a record being filled or the
  next of a fast stream. Whether it does is measured per epoch of one ring of records, on against off (writers' pace
  plus the reader's CPU per record); the cheaper wins by more than 2 SE, and the other is re-measured at doubling
  intervals up to `tt_SEGMENT_PROBE_EVERY_MAX` (512) epochs - waiting pays only where cores share cache lines cheaply.

**What the ring cannot do:** be the RELIABLE retention cache. Its memory is the receiver's and delivery frees the slot.

**Receive-buffer lending (`tt_Sample_retain` / `tt_Sample_release`, SHM stage 2).** A callback may keep its sample
beyond the callback, without a copy, until it gives it back.

- **API.** In a Subscriber's callback, `tt_Sample_retain(sub, &sample)` fills a `struct tt_Sample` (`payload`,
  `length`, `is_native_endian`, an opaque non-zero `handle`) valid until `tt_Sample_release(ctx, &sample)`; a
  `data_decode_inplace` topic's `tt_Data*` stays valid with it. Elsewhere, retain is `tt_RET_ILLEGAL_STATUS`.
- **What is lent:** bytes that sit in the datagram they arrived in, or in a large buffer (section 8).
  - Segment: the **ring slot**. The drain processes a record in its slot, and a retained slot is left unreleased
    (`read_index` moves past it, its `sequence` does not) until release frees it with `index + slots`.
  - Socket: the **receive buffer**. The inline `rx_buffer` and a caller pool (`tt_Context_set_rx_pool()`,
    `tt_RX_POOL_BUFFER_BYTES` each, at most `tt_RX_POOL_MAX` 32) form one set; retaining one hands the next datagram a
    free buffer. None free: `tt_RET_OUT_OF_BUFFER`, the caller copies (`lend_exhausted`). Buffers are 8-aligned, so a
    lone DATA's payload is at 4 mod 8 and a 64-bit struct behind rmw's 4-byte psn lands aligned.
  - Several samples of one datagram may be retained; the slot or buffer goes back after the last release.
- **Not lent** (`tt_RET_UNSUPPORTED`, `lend_unlendable`; the caller copies): samples assembled from small fragments,
  released from a reorder buffer, delivered locally or from a backlog. They already sit in core's own storage, which the
  receive path needs back (`frag_scratch` is one per context).
- **Handles:** at most `tt_SAMPLE_RETAIN_MAX` (16) per context, each with a generation; a bad release (0, unknown,
  already released) is refused with `tt_RET_INVALID_ARGUMENT` (`lend_bad_releases`) and touches nothing; a full table
  is `tt_RET_OUT_OF_BUFFER`. Release may come from any thread (it takes the state lock); one inside the same callback
  leaves the slot to the drain. A writer touching a published record would be a same-user process with write access
  to the segment, already trusted as much as the content.
- **A held slot blocks only its own ring, one lap later**, when writers' claims (in index order) reach it again; then
  that ring is full (`segment_full_dropped`, `segment_full_retained`) and writers fall back to UDP after
  `tt_SEGMENT_DEAD_READER_NS`. **Hold a slot for less than one lap at the stream's rate** (512 slots at 100k records/s
  is 5 ms), or copy. Skip-to-newest and the stall check never touch a held slot (it is behind `read_index`).
- **Lifetime:** the lazy segment release waits for the last release. `tt_Context_destroy()` unmaps and forgets every
  handle, so a later release is refused. Pool storage must outlive the context.
- `tt_SAMPLE_LENDING`: 1 on Linux, 0 on FreeRTOS. Unused it costs a few stores per datagram and saves the drain's copy
  of every record. Counters `lend_*`, `shm_full_retained`.

**Claimed-slot publish (`tt_Publisher_claim` / `_publish_claimed` / `_abandon_claim`).** The send half of zero-copy: the
caller builds a sample's CDR in the subscriber's ring slot and the publish copies nothing. It is encode-in-slot
(`try_publish_into_slot()`) split at the encoder; the caller's fill is the encode.

- **API.** `tt_Publisher_claim(pub, capacity, &payload)` claims room for `capacity` CDR bytes at 4 mod 8;
  `tt_Publisher_publish_claimed(pub, length)` sends `length` bytes; `tt_Publisher_abandon_claim(pub)` gives the slot
  back. A refused claim says why, and each answer means "publish ordinarily": `UNSUPPORTED` (not a lone DATA to one
  attached same-host peer - broadcast, several or remote peers, local subscribers, batching, a Heartbeat due, too large
  for a slot or a datagram, a large sample), `OUT_OF_BUFFER` (ring full), `WOULD_BLOCK` (KEEP_ALL), `ILLEGAL_STATUS`
  (a claim already out).
- **Rules.** One claim per publisher, and while it is out its ordinary publish is refused (`ILLEGAL_STATUS`): the claim
  takes its seq_no at its publish, and the ring and the reliable cache keep seq order. A claimed slot stops its ring
  for every writer until published or abandoned; hold it for the fill only. KEEP_ALL is asked at the claim. A claim is
  spent whatever the publish says.
- **The destination is asked twice.** If discovery moved the peer, added a local subscriber or owes a match Heartbeat
  since the claim, the publish copies the bytes out and publishes them ordinarily, and the slot goes back empty
  (`shm_claims_copied`).
- **The mapping is pinned** (`tt_SegmentPeer.claims`) against revalidation and the dead-reader rule;
  `tt_Publisher_destroy()` and `tt_Context_destroy()` abandon an outstanding claim before unmapping.
- RELIABLE still copies the record into the writer's cache at the publish.
- Counters `shm_claims_published`, `_copied`, `_abandoned`. Tests: `test_publish_claim`, `tests/mutants_publish_claim.py`.

**Cost where unused.** Compiled out, ~560 B of counters and drain-plan state stay in `struct tt_Context`. Compiled in
with every peer remote, a context that never attached a segment declines the slot path in one load
(`segment_slot_ceiling == 0`), and the 14 KB peer table is zeroed per entry on first use, so in zero-allocated memory
(rmw_tickle's context) the rest never becomes resident. The per-datagram segment question (`peer_segment()`) remains.

**Accounting.** `publish_zerocopy()`'s datagrams count in `tx_udp` (`experiments/g15_zerocopy_e2e.sh`). The one send
outside `tx_datagrams` is the zero-length UDP doorbell, counted as `segment_doorbells_sent` minus `segment_bells_rung`.

## 11. Memory model

Core never allocates (section 1): every byte it uses is a fixed table sized by a macro, or storage the caller
attaches and frees.

- **Embedded storage:** server response cache (`tt_SERVER_CACHE_ENTRY_LENGTH` x 64), client call cache, scheduler
  (`tt_MAX_SCHEDULER_LENGTH` 128, 512 in rmw_tickle; 24 B an entry on 64-bit, 16 on rv32), peer and ack tables,
  per-peer tables indexed by context id (`tt_MAX_CONTEXT_IDS` 256). A full scheduler refuses the entry, counts it
  (`sched_full`, beside `sched_peak`) and warns at most every 5 s; a Server turns a request away before its callback
  when it or its answer would find no entry.
- **Caller-attached storage** where the size depends on use: `reliable_cache` (index + arena), a Subscriber's tracking
  window and reorder buffer, `tt_Server_set_storage()` / `tt_Client_set_storage()`, the lending pool
  (`tt_Context_set_rx_pool()`), the large-sample buffer callbacks (`tt_Context_set_large_buffers()`). Core holds
  pointers only.
- rmw_tickle allocates with the application's `rcl` allocator, so a static-pool allocator keeps TickLE heap-free.

## 12. Context ids and same-host peers

Every per-peer table is indexed by the sender's 8-bit context id, which keeps lookups O(1) and memory fixed but makes
the id unique per link. On Linux a context claims its id in a host registry and moves away from collisions.

- Without claiming: the last octet of the context's address, or `_tt_CONFIG.context_id`.
- **`tt_CONTEXT_ID_CLAIM`** (default on Linux, off on FreeRTOS, which has no host registry):
  - the id is claimed in a host registry file under `/dev/shm`, keyed per network: the preferred id if no live process
    holds it, else a free one;
  - a datagram is our own only if it comes from our own data socket;
  - our id from any other address is a collision, and the newer context moves to an id nobody on the link uses. An
    explicitly set id never moves.
- The wire is the same either way. Two contexts on one host differ by address and port, which also name their segments.

## 13. Security model

TickLE has no cryptography - no authentication, access control or encryption, a plaintext wire - so it is safe only on
a controlled link. What it does is refuse inputs that could reach shared memory or overrun a buffer.

- rmw_tickle refuses to start when `ROS_SECURITY_ENFORCEMENT=Enforce` is requested, and logs once when security is
  enabled but permissive, so a user who asked for security never silently runs without it.
- `SHM_DATA` (10) and `LARGE_REF` (13) from a socket are refused: counted (`rx_shm_only_on_socket`), never delivered,
  logged at the 1st, 10th, 100th ... refusal. The seq span is read only from a segment slot. A zero-length datagram
  (the UDP doorbell) from anyone only wakes the reader to drain its own ring; it can make writers ring again, never
  suppress a ring. `test_hostile_datagram` covers these through the socket, with plain DATA as the control.
- Hostile datagram lengths and ACKNACK word counts are bounds-checked before indexing.
- The HIL CI never runs on `pull_request`, and the rig accounts are unprivileged.

**What is parked** ([ROADMAP.md](ROADMAP.md); not to be started until the user says so): SROS2-file-compatible
security, with MACsec for link confidentiality and a core module, **TickLE Security**, for per-process authentication,
access control and optional per-topic protection. Agreed shape: compile-time seams (authentication at peer
association, authorization at endpoint creation and match, protection per datagram inside the acceptance path, so the
segment cannot bypass it); keystore parsing stays on the rmw side.

## 14. Wireless and zenoh-pico notes

- **Wireless is parked** ([ROADMAP.md](ROADMAP.md)); nothing has been measured on a wireless link. TickLE has no
  congestion control or pacing, no NAT traversal and no encryption, so an internet path is not supported. Per-datagram
  repair and small framing should help on lossy links; batching (`tt_send_batch`) is the cheapest candidate win. Wire
  changes such as a run-header merge, FEC or range NACKs wait for the wireless stage (the user, 2026-10-15).
- **zenoh-pico** does not retransmit over UDP (its RELIABLE only orders). It is compared only on best-effort cells
  over multicast; its reliable p2-p4 cells are recorded as not measurable.

## 15. Configuration reference

Compile-time `-D` overrides unless noted; times in nanoseconds. A blank rmw column means the core default.

| knob | default | rmw build | meaning |
|---|---:|---:|---|
| `tt_MAX_BUFFER_LENGTH` | 1472 | 65507 | largest datagram |
| `tt_CONTROL_MAX_LENGTH` | 1472 | 1472 | largest shared datagram / fragment |
| `tt_MAX_SAMPLE_LENGTH` | = buffer | = buffer | largest sample on the small path |
| `tt_FRAG_ENABLED` | derived | 1 | fragmentation compiled in |
| `tt_FRAG_REASSEMBLY_SLOTS` | 8 | 8 | best-effort reassemblies in flight |
| `tt_LARGE_SAMPLES` | 0 | 1 | stage 2, UDP offload, receive drain |
| `tt_LARGE_RETAINED` / `tt_LARGE_ASSEMBLIES` | 16 / 4 | | large samples kept by a Publisher / assembled by a context |
| `tt_LARGE_END_HEARTBEAT` | 1 | | HEARTBEAT after each large sample |
| `tt_LARGE_SEND_ON_WRITABLE` | 1 | | send cursor waits for writability (0 = timer only) |
| `tt_LARGE_SEND_RETRY_NS` / `_MAX_NS` / `_WRITABLE_FALLBACK_NS` | 50 us / 1 ms / 20 ms | | send cursor timers |
| `tt_LARGE_REGION_SPARE` / `tt_LARGE_PEER_REGIONS` | 3 / 8 | | step B spare chunks (rmw) / regions a reader maps |
| `tt_HYBRID_QUEUE_LIFETIME_NS` | 30 s | | `tau` when no source sends runs |
| `tt_HYBRID_NONE_SEEN_FRAMES` | 500 | | clean frames before "none seen" |
| `tt_HYBRID_STALE_EVENTS` / `_FALSE_FORGET` / `_P_MIN` | 8 / 1e-4 / 1e-3 | | stale-event forgetting |
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
| `tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE` | 64 | | ceiling in max(srtt, G) |
| `tt_RELIABLE_RETRY` | 3 | | retries at the plain interval before the back-off |
| `tt_RELIABLE_RETRY_BACKOFF_SHIFT` | 4 | | back-off ceiling, 2^n x the interval |
| `tt_RELIABLE_BITMAP_BITS` / `_MAX_BITS` | 256 / 4096 | 256 / 8192 | reader window default / ceiling |
| `tt_IDLE_HEARTBEAT_PERIOD` | 250 ms | | idle HEARTBEAT period P; 0 = off |
| `tt_MAX_RELIABLE_HISTORY` | 64 | | reference cache depth for examples |
| `tt_CLIENT_MAX_OUTSTANDING` | 4 | | calls a Client may have outstanding |
| `tt_CALL_RETRY_INTERVAL` | 5 ms | | RPC auto retry seed, until a first answer |
| `tt_CALL_RETRY_GRANULARITY` | 0 (2 x G) | | two hosts' event lateness term; non-zero = fixed |
| `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` | 64 | | a wait's ceiling in max(srtt, G) |
| `tt_CALL_DEADLINE_PER_SEND` | 250 ms | | auto call fails within (count + 1) x this (1 s) |
| `tt_CALL_RETRY_COUNT` | 3 | | RPC retries |
| `tt_SERVER_CACHE_GAP_MULTIPLE` | 4 | | answered-response cache, in observed retry gaps |
| `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` | 5 s | | deferred response slot lifetime |
| `tt_MAX_SERVER_CACHE_COUNT` | 64 | | server response slots |
| `tt_SERVER_CACHE_ENTRY_LENGTH`, `tt_CLIENT_CACHE_LENGTH` | 2 x buffer | 8 | inline storage (rmw attaches its own) |
| `tt_MAX_SCHEDULER_LENGTH` | 128 | 512 | scheduler entries |
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

Linux HAL environment switches (A/B controls, each on unless set to `0`): `TT_UDP_OFFLOAD`, `TT_UDP_CSUM0`,
`TT_RX_NONBLOCK`, `TT_RX_DIRECT`, `TT_RX_LAZY_TIMER`, `TT_RX_LAZY_ADAPT`.

## 16. Where the older documents went

On 2026-10-05 the planning documents were condensed into `docs/`. Their full text, with every measurement narrative
and decision record, is in git at `3c0c505b`: `git show 3c0c505b:<old path>`. Source comments that cite an old
document (for example "SHM_PLAN 6c") resolve the same way.

On 2026-10-17 this document was cut to the design alone. Everything it held before - the pre-registered criteria
(stage 2's L1-L4, step B's B1-B2, the hybrid campaign and its steps), the A/B, PC and rig evidence, the step B rig
control, dated incident records, and the superseded first hybrid control law - is `git show 228da18d:docs/DESIGN.md`.
A citation of a DESIGN.md heading or criterion not found here resolves there.

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
