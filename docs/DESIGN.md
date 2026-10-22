# TickLE design

**TL;DR**
- A small pub/sub and RPC middleware in C for controlled links and microcontrollers; `rmw_tickle` puts ROS 2 on it.
- Core never allocates, starts no thread, does not depend on ROS 2, and counts every drop.
- Own UDP wire (`tt_VERSION` 14), RTPS-like: per-datagram seq_nos, NACK repair, broadcast discovery.
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
| Wire changes leave every test better (the user, 2026-09-26) | targeted metrics improve beyond 2xSE, none regress; each bumps `tt_VERSION`, except a reserved field given a meaning whose zero is the old one (section 3) |

## 2. Context, nodes, endpoints and the poll loop

**Contexts and nodes.**

| object | owns |
|---|---|
| `tt_Context` | sockets, scheduler, poll loop, discovery, liveliness, segment; its id is the wire's `source` |
| `tt_Node` | name, namespace, index (up to `tt_MAX_NODES`); index 0 is the default node `tickle_<id>`, made on first `tt_Context_create_*()` shorthand |
| `tt_Publisher`, `tt_Subscriber`, `tt_Client`, `tt_Server` | endpoint state, embedding `tt_Endpoint` and the node index |
| `tt_Topic`, `tt_Service` | the type: sizes, encode/decode |

`tt_Node_destroy()` refuses while an endpoint lives on the node.

**Context options** (`struct tt_InitOptions`, bundle 1 of ROADMAP "Context options", 2026-10-21). `tt_Context_create_with(ctx, &opts)`
copies the options into the context (`ctx->opts`, each zero resolved to its default, as in `tt_QosProfile`) and checks
them: a value this build cannot honour is `tt_RET_INVALID_ARGUMENT` with the field named in the log, where it used to be a
`static_assert` or a silent clamp. Grouped by subject; only `net` is filled so far - address, broadcast, port, context
id, the links (each with its unicast threshold), discovery range, segment slot size, hybrid mode, reassembly ceiling.
The other groups (discovery, reliable, service, qos, poll, socket, hybrid_transport, segment, capacity) are one-byte placeholders
each later bundle replaces. Every reader - core and both HALs - reads the context's copy, so two contexts in one process
can differ. `tt_Context_create(ctx)` is `tt_Context_create_with()` given the deprecated process-wide `_tt_CONFIG`, read
once; writing `_tt_CONFIG` is deprecated and goes when the last caller has moved. A default context is byte-identical on
the wire to one created before the options existed (`tests/wire_identity_initoptions.sh`).

**Service options** (`tt_InitOptions.service`, bundle 4): the call retry seed, count, granularity, ceiling multiple and
deadline per send, the kept-answer gap multiple and the deferred-response timeout, each 0 = its `config.h` default. A
Client's own `call_retry_*` still override seed and count. The old `static_assert`s between them are creation checks.
The client's schedule is interop-semantic (a server reckons it from its own options), so a non-default one is accepted
with a warning. rmw_tickle's client retry count is the context's deferred timeout / 100 ms. Default behaviour is
trace-identical (`tests/wire_identity_service.sh`).

**Discovery options** (`tt_InitOptions.discovery`, bundle 2). The discovery and liveliness timings of sections 5 and 9
are the context's own: cycle, update interval, tx interval, lease divisor, longest lease, list-request retry and
attempts, liveliness deferral and its cap, miss threshold (the silence is derived per context), the full-scheduler
warning interval. The config.h constants are their defaults. Checked at creation: every duration at most a day and the
tx interval at most the update interval (the silence against the segment's dead-reader time moved to bundle 8). The
summary carries the update interval and miss threshold (section 5), and the peers judge the context's silence by them,
so a context created with either different from the default logs one INFO line, not the WARNING it logged until
2026-10-22: only a peer built before then still judges it by its own. The lease floor (`lease_divisor` x tx interval) is the
context's at an endpoint's creation; `tt_QosProfile_resolve()` and `tt_*_storage_size()`, which have no context, keep
the default's. rmw_tickle reads its watchdog period, stall threshold, MANUAL_BY_TOPIC default lease and lease floor
from its core context.

**Reliable options** (`tt_InitOptions.reliable`, bundle 3): the timing of section 6 - `deadline`, `retry_interval`,
`retry_initial`, `retry_granularity`, `timer_lateness_initial`, `max_srtt_multiple`, `retry`, `backoff_shift`
(`tt_RELIABLE_NO_BACKOFF`: none), `stuck_warn_interval`, `idle_heartbeat_period` (for a Publisher whose own option is 0;
`tt_QOS_DURATION_INFINITE`: off) and `idle_heartbeat_retry_multiple`. Each zero is its section 15 constant. Refused at
creation: `retry` 255 (the saturating counters never pass it), `backoff_shift` above 16, a fixed or starting interval
that overflows 64 bits once backed off, a G above 32 bits. `tt_reliable_retry_interval_configured()` takes the context.
A default context sends the same bytes at the same instants on a lossy exchange that reaches every reader
(`tests/wire_identity_initoptions_reliable.sh`).

**Hybrid options** (`tt_InitOptions.hybrid_transport`, bundle 7): the estimator's and control law's 21 settings
(section 8), each 0 = its `tt_HYBRID_*` default. `tt_HYBRID_N_MAX` stays the build's largest run and `n_max` a runtime
bound under it; a reader below it advertises that many units. The slow start is a runtime `min()`
(`tt_HYBRID_SLOW_START_OFF`); `path_clean_ns` scales with `queue_lifetime_ns`. `tt_HYBRID_COLLAPSE_Z` stays compiled
(the 99.9% table assumes it) and the stale exposure is derived, `-ln(stale_false_forget)`. A build without the hybrid
transport refuses the group set, compared byte by byte (no soft-float on RV32). Cost: none measurable, 0 instructions per 1 MB
sample within spread, VERDICT=PASS on 2cf7d2d0's change before its rebase onto bundles 3 and 6
(`hybrid_options_insn.sh`: the halving threshold and the stale floor are copied into each estimate); default behaviour
is trace-identical (`tests/wire_identity_hybrid_estimator.sh`). On the rig (`rmw_keepall_rig.sh`, A ed1d3a2a, same
build): Array1m 30 Hz and Array4m 15 Hz reliable cross-host, 100% delivered and latency equal at 0, 0.1 and 1% loss; at 1%
CPU per run is bimodal on both arms (about 4.5 / 3.5 or 5.5 / 4.5 ms per MB, sub / pub), and 10 reps per arm gave 2/10
runs in the high mode on each (Fisher p 0.71) and low-mode CPU +0.77% sub, -0.04% pub.

**Segment options** (`tt_InitOptions.segment`, bundle 8; section 10). The ring's bytes and slot size (`slot_bytes`, moved
from `net.segment_slot_bytes`, which no release carried), the fence hold, the dead-reader time, the drain bound per poll,
the reader's re-measure bound, the stall report and the attach-retry and revalidation counts; each 0 = its config.h
constant. `bytes` sets the slot count (largest power of two that fits, 4 to the build's `tt_SEGMENT_SLOTS`, which sizes
the reader's per-slot state); 0 keeps `tt_SEGMENT_SLOTS` slots at any slot size, as before. `dead_reader_ns` 0 is derived
at creation from the context's discovery timing (one update interval; `tt_SEGMENT_DEAD_READER_NS` at the defaults).
Checked: slot holds a datagram, 4 to `tt_SEGMENT_SLOTS` slots, fence hold < dead reader < silence, `probe_every_max` a
power of two (the window is a mask; no 64-bit modulo on rv32). `tt_PUBLISHER_CLAIMS` and `tt_SEGMENT_FENCES_SEEN` stay
compile-time. The send counts are read only when a countdown is reset. Cost, corrected: this paragraph said "per-record
cost unchanged" for the first build, and the rig's same-host gate v2 A/B (A fab14fd9, B that build before its rebase,
`ab_samehost_initoptions8_segment_20261022-003748`, placement control held) read +0.6% to +1.7% server and +0.6%
client user instructions a sample on BE p1/p3, reliable p3 and latency p1/p3. The drain compared every record against the runtime bound, one live value too many on aarch64: drain_own_segment +7.8
instructions a call, the only function that moved (`aarch64_insn/samehost_qemu.sh`, exact counts). Each lock chunk now
folds the bound into its own count, one compare a record: -15 a round trip against main there, and on the PC
(`initoptions8_segment_insn_pc.sh`, A 05118219, B e93346b4 before its rebase, n 6/6/6 a cell) latency p1/p3 -0.17% client and -0.36%
server, BE p1/p3 and reliable p3 clients -0.04% to -0.30%. Its pre-registered verdict is FAIL all the same, on the
BE p1/p3 and reliable p3 servers only: their count on this PC is bimodal - a reader that catches the writer mid-copy
polls the claimed head, 0.13-0.34 head stalls a sample against 0.03-0.04 - and the placement arm itself moved +16% to
+68% there; within one mode B is at or below A. The unfixed build failed on the latency cells (+5 a round trip, server),
which the earlier PC run (user plus kernel instructions, other cells) could not see. The rig decides: same-host gate
v2 A/B of the fixed build (A 05118219, B 8b2f69ed's tree before its rebase, `ab_samehost_initoptions8v2_segment_20261022-050502`,
REPS 4, sentinel control held at -0.20%) reads IMPROVED: 6 comparisons better, 28 held, none worse.

**Capacity** (`tt_InitOptions.capacity`, bundle 9): how much of a fixed table a context uses - `ack_entries` (readers a
Publisher tracks), `peers` (a Publisher's or Client's peers, a Subscriber's writer proxies), `request_history`,
`publisher_claims`, `large_retained`, each at most and by default its compiled size (`tt_MAX_ACK_ENTRIES`,
`tt_MAX_PEER_COUNT`, `tt_RELIABLE_REQUEST_HISTORY`, `tt_PUBLISHER_CLAIMS`, `tt_LARGE_RETAINED`). The arrays keep that
size, so no struct and no `tt_*_storage_size()` changes (the six bytes fill padding: `tt_Context` is the same size on
Linux and RV32); memory is bundle 10's. A limit is read only where an entry is taken, never on a walk - the entries past
it stay empty - so the per-sample peer scans and the large ring's index are untouched. A full table refuses as it did at
the compiled size. `peers` must exceed every link's unicast threshold, so a full table still means broadcasting.
`tt_DISCOVERY_PENDING_REQUESTS` stays compiled: a full table only skips retries. Cost: none measurable - user instructions per sample, client, B vs A with A+P the placement control, every
row EQUAL: p1 +0.02% (P -0.01%), p3 +0.01% (-0.01%), t1 +0.01% (+0.02%), t3 0.00%, 1% loss l1 +0.01% (0.00%), l3 0.00%
(-0.01%) (`initoptions_insn_pc.sh`, A 05118219, B 64bcbd1c's tree before its rebase, 3 reps). Default behaviour is
wire-identical (`tests/wire_identity_initoptions_capacity.sh`). Rig, A 05118219: core gate v2 PASS (0 CPU/latency
WORSE; 1 of 88 listed, c3 client user instructions +0.04%); Array1m 30 Hz reliable cross-host, KEEP_LAST 10 and
KEEP_ALL, 0 and 1% loss (`rmw_keepall_rig.sh`, 5 reps, pre/head/aa): 100% delivered, latency equal, CPU equal at 0%
and on the same bimodal levels at 1%.

**Traffic classes and send time** (network profiles items D and E, 2026-10-22; `tt_InitOptions.socket.tx_class[]`,
`.txtime`, Linux HAL; FreeRTOS ignores them). Every datagram is of one class, read from what it carries
(`tt_tx_class()`): CONTROL when nothing of it is a user's (discovery announce, summary and list request, HEARTBEAT,
ACKNACK, a segment's fence and bell), REPAIR when it is a call's retry (the header's retry count) or core is resending
(the NACKed-resend and durability-backlog loops mark `ctx->tx_class`; a resent DATA is byte-identical to the original),
else DATA - so a summary or match HEARTBEAT riding with a sample is DATA. Per class an SO_PRIORITY (a qdisc band;
a VLAN PCP through the interface's `egress-qos-map`, which the user sets) and a DSCP; 0 sets nothing. All classes
share the data socket, so, measured on Linux 7.0 (`tests/test_socket_marks.c`):

| what | how it goes |
|---|---|
| DATA's priority and DSCP | on the socket at bind: `IP_TOS`, then `SO_PRIORITY` (IP_TOS alone sets the priority from the TOS) |
| another class that differs | per datagram: `IP_TOS` and `SO_PRIORITY` control messages; a sendto() becomes a sendmsg() |
| `SO_PRIORITY` per datagram | asked at bind with `MSG_PROBE`; a kernel without it (EINVAL) keeps the socket's priority, the TOS still per datagram |
| priority 7 and above | `SO_PRIORITY` needs CAP_NET_ADMIN; refused (EPERM), the class keeps the socket's, with a warning |
| all classes equal | per socket only, nothing per send |

`txtime` on puts SO_TXTIME (CLOCK_TAI, deadline mode off) on the data socket - CAP_NET_ADMIN, else one warning and no
times; `tt_Publisher_publish_at(pub, data, tai_ns)` then sends that publish's datagrams with SCM_TXTIME. An ETF qdisc
releases them at their time and drops an untimed or late datagram of the socket, so only the DATA class should be
mapped into an ETF queue. With every option 0 the HAL tests one byte per send and the syscalls are those of before
(`test_socket_marks.c`; 41 mutants, `tests/mutants_socket_marks.py`, all killed). Cost at the defaults: user instructions
per round trip, B vs A with A+P the placement control, every row EQUAL (`initoptions_insn_pc.sh`, A 6c355fa2, B 4a1fbda5
before its rebase, 3 reps, ~/rig_results_safe/socket_marks_insn_pc_20261022-144615): client p1 +0.09% (P 0.00%), p3
+0.20% (+0.06%), t1 +0.07% (+0.12%), t3 +0.07% (-0.03%); server p1 +0.12%, p3 +0.23% (bound 0.30%), t1/t3 within their
10-13% spread. A p3 rerun with perf record (2 reps) read -0.05% client and +0.03% server, and no function moved beyond
its sampling noise. netns, the kernel's own counters (`socket_marks_netns.sh`, 400 samples at 2 ms,
~/rig_results_safe/socket_marks_netns_20261022-144525, 4a1fbda5 before its rebase): with DATA (5, DSCP 46), REPAIR (3,
16), CONTROL (2, 8) and 10% loss on DATA's band, the sender's prio bands carried exactly the HAL's counts (DATA 400 = 364
sent + 36 netem drops, CONTROL 8, REPAIR 36) and the receiver saw those TOS bytes in those numbers; with the defaults
every datagram went with priority 0 and TOS 0. A send time 2 ms ahead: under pfifo the sample arrived 1977 us before it
(p50, sent at once), under ETF (delta 200 us) 182 us before (min/p99 192/156 us; software ETF dequeues delta ahead of
the time), 0 ETF drops.

**Link profiles** (`tt_LinkOptions.profile`, network-profiles phase 0 items B and G, 2026-10-22). A link names its
medium, and the profile fills the link's zero fields; an explicit field always wins. ETHERNET (0) is every value a
context had before, WIFI maps to it until the Wi-Fi stage decides its own, and discovery timing does not change per
profile (each context's discovery summary carries its own interval since 2026-10-22, so a later profile may set it).

| field | ETHERNET, WIFI | T1S | TSN | applied |
|---|---|---|---|---|
| `mtu` | 1500 | the interface's (1500 if unknown) | the interface's | context: the smallest |
| `link_rate` | 1 Gbit/s | 10 Mbit/s | 1 Gbit/s | T1S's retry floor |
| `retry_floor_ns` | none | 2 PLCA cycles: 2 x 8 nodes x one MTU frame at `link_rate` (19.7 ms at 1500) | none | context: the longest |
| `batch` / `batch_tick_ns` | off / context's | on / context's (1 ms) | off | context: on only if every link is |
| `idle_heartbeat_ns` | context's (250 ms) | 1 s | context's | context: the shortest |
| `piggyback_divisor` | none | 8 (every window/8 samples) | none | context: the most frequent |
| `unicast_threshold` | 2 | always broadcast | 2 | **per link**, on the send path |
| `large_fragmentation` | context's | MTU units | context's | context: MTU if any link is |
| `pacing_rate` / `pacing_burst` | off | 75% of `link_rate` / 4 MTU frames | off | **per link** ("Send pacing" below) |

Only the unicast threshold is per link: the send path already resolves each destination to its link. Everything else
is one value for the context, because a datagram is encoded once for all the links it goes to and a sample is cut once
and cached for repair; a reader's retry timer does not know its writer's link either. Each is combined in the direction
that is safe for the most demanding link - the smallest MTU, the longest floor, the shortest idle HEARTBEAT - and
written where the context already kept the setting: the tick into `discovery.tx_interval_ns`, the idle HEARTBEAT into
`reliable.idle_heartbeat_period` (each only where the context left it at 0 or its default; a contradicting explicit
pair is refused), the floor under the measured G at its least and its cold start (a fixed `retry_granularity` is the
caller's and stays), the batching and piggyback period into a Publisher whose options leave them to the context
(`tt_PublisherOptions.batching`, `heartbeat_piggyback_every` 0). So no retry, publish or receive path reads a new field
for these.

The MTU (item G): below 1500 it cuts every datagram built from more than one submessage, every announce part and every
fragment (`tt_Context.control_max`); `tt_CONTROL_MAX_LENGTH` stays the ceiling. Compatible with every reader: a reader
learns a sample's continuation length from its fragments (`frag_learn_cont_length()`, the fast path's). Not cut: a
large sample's units (`FRAG_*_L`; the reader places them by index at the full length, so a shorter unit is a wire change),
a service request or response (they never fragment), and anything in a build that does not fragment (core's default,
1472 B), which keeps sending whole what it accepted. A context cut short sends MTU units only, since a run's units are
full-length. An MTU at which `tt_MAX_SAMPLE_LENGTH` needs more than `tt_FRAG_MAX_COUNT` fragments is refused; from
the interface, it falls back to 1500. The zero-copy publish's direct unicast (`publish_zerocopy()`, for an in-place
encoder) compared with the compiled `tt_UNICAST_PEER_THRESHOLD` until 2026-10-22, and unicast on a T1S link; it now
takes each link's share as a flush does (`test_link_profiles.c`, the zero-copy case).

Cost: none measurable - the publish path reads `control_max` where it compared with a constant. User instructions per
sample, client, B vs A (A+P the placement control), 3 reps, every row EQUAL: p1 -0.04% (P -0.04%), p3 +0.04% (-0.01%),
t1 +0.03% (-0.01%), t3 +0.05% (-0.01%), 1% loss l1 +0.02% (0.00%), l3 +0.11% (+0.10%) (`initoptions_insn_pc.sh`,
A 6c355fa2, B 329c7481 before its rebase, `linkprofiles_insn_pc_20261022-141720`). Default behaviour wire-identical
(`tests/wire_identity_link_profiles.sh`, core and fragmenting builds). T1S over a 10 Mbit/s tbf veth of MTU 1000 in two
namespaces (`t1s_profile_netns.sh`, PASS): 907 broadcast datagrams of at most 972 bytes for 6000 samples, none
IP-fragmented, against ETHERNET's 6007 unicasts; ETHERNET batching on that link IP-fragmented 600.
Rig core gate v2 (A 6c355fa2, the branch before its rebase): PASS, 0 CPU/latency WORSE; 7 of 88 rows listed, client
user instructions +0.1% to +3.4% on the p4 cells (c4, c15), server +0.1% to +1.0% (c10, c11, c17).

**Send pacing** (`tt_LinkOptions.pacing_rate`, `pacing_burst`; network-profiles phase 0 item F, 2026-10-22). A paced
link averages at most `pacing_rate` bits per second on the wire (IP, UDP and Ethernet framing counted, a datagram's
frames by the link MTU), through a token bucket of `pacing_burst` bytes. T1S paces at 75% of `link_rate` (7.5 Mbit/s):
the plan's 70-80%, the middle, leaving a quarter of the bus for the other nodes' PLCA turns and for what this node sends
unpaced; a burst of 4 frames of the link MTU (6152 B at 1500) is a few PLCA turns' worth, the large-run cap. ETHERNET,
TSN and WIFI are off (`tt_PACING_OFF`), and so is a context with no links configured.

| rule | detail |
|---|---|
| where | one bucket **per link** (`tt_Context.pacers[]`, as `links[]`): unlike B's combined values, every paced send is addressed to one destination, which the send path already resolves to its link (`link_of_ip()`); a T1S bus and an Ethernet uplink do not slow each other. Kept as the time the link's sends so far finish at the rate (`until_ns`); the backlog `until_ns - now` is the bucket's emptiness |
| counted | every datagram that leaves over UDP, whatever sent it, where it leaves: the seam (`seam_send*()`) and `large_send_batch()`; a same-host segment's records are not |
| waits | a **large sample's units**: the send cursor sends what the bucket holds (`pace_units()`) and stops with `pace_at_ns`; `large_pump()` then schedules `large_send_resume` for when one unit fits (`large_wait_for_pace()`, `pacing_waits`) - the machinery a full socket buffer uses, never a wait or a sleep in the publish. What the publish has not encoded yet is encoded first, as for a full buffer. A datagram longer than the whole burst (a unit IP-fragmented on a narrow link, a hybrid run) goes alone from a full bucket |
| repairs | a **retransmission** (`retransmit_one_sample()`, `large_resend()`) goes while the backlog stays within two bursts: it is answered ahead of the large send, which waits for it, by at most a burst. Past that it is not sent, not marked resent and not gone: the reader's next ACKNACK asks again (`pacing_repairs_deferred`) |
| not held | ordinary DATA, announces, HEARTBEATs, ACKNACKs and a small sample's fragments go at once and are counted. They are what pacing protects - a periodic sample's latency - and core has no queue to hold them in; holding them would add latency where the large send is what fills the bus. On T1S they are already batched on the 1 ms tick |
| off | `tt_Context.pacing` false: each send site pays one branch on it, nothing else |

Refused at creation: a `pacing_rate` below `tt_LINK_RATE_MIN` or above the link's `link_rate`, a `pacing_burst` below
`tt_PACING_BURST_MIN` (one full 1500-byte frame on the wire) or on a link that is not paced. An EPOLLOUT wake for
another Publisher resumes a paced send early; it finds the bucket and waits again.

Cost: none measurable. User instructions per sample, client, B 839eb9cb vs A 9c0b1040, both before their rebase (A+P the
placement control), 3 reps, every row EQUAL: p1 +0.03% (P -0.01%), p3 +0.06% (+0.03%), t1 +0.07% (+0.00%), t3 +0.07%
(+0.00%), 1% loss l1 +0.07% (-0.01%), l3 +0.05% (-0.00%); 8,202 round trips a p run, 1.1-1.3 M samples a t or l run, 0
void (`initoptions_insn_pc.sh`, `pacing_insn_pc_20261022`). Rig core gate v2 (A 9c0b1040, B e64d5a93, both before their
rebase onto D+E and the discovery timing, `coreAB_3f5d210b_20261022-203136`): PASS, 0 CPU/latency WORSE; 5 of 88 rows
listed, user instructions +0.08% to +0.12% (client c2, c3, c10; server c10, c16). Wire- and time-identical when off
(`tests/wire_identity_send_pacing.sh`: core, fragmenting and large builds, parent-vs-parent control identical, ETHERNET
paced at 10 kbit/s differs in each). T1S over a 10 Mbit/s tbf veth (burst 16 kB, latency 100 ms) in two namespaces, a
256 kB RELIABLE sample every 500 ms and a 100-byte sample every 10 ms for 15 s (`t1s_pacing_netns.sh`,
`t1s_pacing_netns_20261022-run2`, PASS): pacing off, the shaper dropped 2,528 datagrams with up to 140 kB queued, the
writer resent 5,047, and the small stream's p99 was 108.3 ms (1,498 of 1,500 received); on, 0 dropped, nothing queued,
no resend, p99 0.21 ms (1,500 of 1,500); every large sample delivered in both. The control - the small stream alone -
p99 0.44 ms off, 0.16 ms on: the 1 ms batch tick's phase against the publish period, which differs run to run (`-run3`,
PASS: control 0.21 ms off, 0.60 ms on; off 2,496 dropped, p99 108.3 ms; on 0, p99 0.68 ms). Again on e81c9ddf before its
rebase (`-run4`, PASS): off 2,505 dropped, 141 kB queued, p99 108.3 ms; on 0, 0 B, p99 0.29 ms; control 0.28 / 0.65 ms.

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
- **Wait ring** (`TT_RX_URING_WAIT`, default the build's `tt_RX_URING_WAIT`, 1): the wait set held as io_uring polls and
  the data socket's read as a recvmsg submitted with the wait - one `io_uring_enter()` for a datagram that wakes the
  reader, where epoll took two calls. The ring is `SINGLE_ISSUER | DEFER_TASKRUN`: the first waiter owns it, a thread
  that waits 64 times in a row while another owns it takes it over (each move doubles that run), and every other wait
  uses epoll. A recvmsg left in flight reads only inside its owner's next wait, which keeps it (same buffer) or cancels
  it first, so no datagram is lost or reordered across owners (`wr_check`). Linux 6.1; refused, the epoll path.
  Rig A/B (2026-10-22, base 6c355fa2, `rig_queue_rx_uring_wait.sh`): core gate v2 PASS (0 CPU/latency WORSE; user
  instructions +2% to +6% on 16 of 88 rows, listed); rmw BEST_EFFORT cross-host, 5 reps of 100 round trips, the pong's
  tap to receive return p50 -2.59 us (14.9-16.7 against 17.9-18.6 us epoll, libc builds), wire control equal, round trip
  not slower; a two-thread executor's round trip not slower.
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

## 3. Wire format (`tt_VERSION` 14)

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
| 6 | `HEARTBEAT` | endpoint_id, first_available_seq_no, last_seq_no, entity_id, flags, summary_interval (2 B), summary_miss_threshold - 20 B | writer's range; `tt_HEARTBEAT_FLAG_LIVELINESS` = manual liveliness assertion; the last 3 B are the sender's discovery timing on a summary (section 5), zero elsewhere |
| 8 | `FRAG_FIRST` | `tt_DataHeader` + frag_count (u8) - 17 B | first fragment of a sample |
| 9 | `FRAG_CONT` | entity_id, seq_no, frag_index, frag_count (u8) - 10 B | later fragments |
| 10 | `SHM_DATA` | - | reserved for good; nothing produces it; refused from a socket |
| 11, 12 | `FRAG_FIRST_L`, `FRAG_CONT_L` | as 8 and 9 with u16 index and count - 18 B, 12 B | large samples (section 8) |
| 13 | `LARGE_REF` | `tt_DataHeader`, incarnation, generation, cdr_len, chunk, frag_count - 32 B | same-host large sample by reference; segment only (section 8, "Step B") |
| 14 | `FRAG_RUN_L` | `tt_DataHeader`, frag_index, frag_count, run_count (u16), 2 B zero - 24 B | hybrid run, large geometry (section 8) |
| 15 | `FRAG_RUN` | `tt_DataHeader`, frag_index, frag_count, run_count (u8), 1 B zero - 20 B | hybrid run, 8/9 geometry |

**Announce** (a DATA or FRAG_FIRST/FRAG_CONT of the discovery endpoint, section 5): `tt_AnnounceHeader` - entity_count
(u8), ip_reassembly_max, loss_p, 2 B zero - 7 B; then per endpoint or node a `tt_UpdateEntity` record and two strings
(type, name):

| offset | field | bytes | meaning |
|---:|---|---:|---|
| 0 | endpoint_id | 4 | as DATA's |
| 4 | entity_id | 4 | the instance |
| 8 | kind | 1 | `tt_KIND_*`; node index bits 4-7 in spare bits (v11) |
| 9 | qos | 1 | RELIABLE, DURABLE, MANUAL liveliness, KEEP_ALL; node index bits 0-3 |
| 10 | tracking_words | 2 | a Subscriber's ACK window, 0 = default |
| 12 | deadline_duration_ns | 8 | 0 = infinite |
| 20 | liveliness_lease_duration_ns | 8 | 0 = infinite |
| 28 | lifespan_duration_ns | 8 | v14; 0 = infinite. A writer's is applied by its readers (section 9) |
| 36 | history_depth | 4 | v14; the depth `tt_*_get_actual_qos()` reports. Graph only |

40 B a record (28 before v14); each string is `u16 length` (with the NUL) + bytes, unpadded. KEEP_ALL is a RELIABLE
writer's blocking promise, which its readers act on; since v14 a Subscriber's or a BEST_EFFORT writer's KEEP_ALL history
sets it too, for the graph. A 1,472 B datagram holds ~13 endpoints with 63 B of names (15 at v13).

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
`ip_reassembly_max` and `loss_p` in the announce; 14 lifespan and history depth in each announce record (+12 B), the
KEEP_ALL bit on Subscribers (QoS D1, D8, D9: announce only, nothing per sample). `LARGE_REF` never crosses the network.
Mixed versions refuse each other's every datagram (`tests/version_mix.sh`).

**Reserved bytes given a meaning, without a version** (the user's decision, 2026-10-22, network-profiles item C). The
rule above has one documented exception: a field that was reserved - written zero by every build and read by none, on
every receive path, Linux and FreeRTOS alike (both run the same `process_heartbeat()`) - may be defined so that zero
keeps the old meaning. An old peer then sends zero, which a new one reads as before, and ignores what a new one sends;
the two interoperate and a default context's bytes do not change, so the version stays. Used so far: the summary's
timing (section 5) in `tt_HeartbeatHeader`'s 3 reserved bytes; the precedent is `client_tag` (a zero pad byte until
2026-10-09). `tt_AnnounceHeader`'s 2 reserved bytes stay zero and unread.

## 4. Sending: flush, batching and unicast peers

| rule | detail |
|---|---|
| RPC flushes at once | a caller is waiting |
| publish flushes at once by default | `pub->batch = true` coalesces until the `tt_CONTEXT_TX_INTERVAL` tick |
| shared datagrams are small | a batch or announce never exceeds `tt_CONTROL_MAX_LENGTH`, so any core build can receive it, nor the smallest link MTU (section 2, "Link profiles") |
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
| summary, every `tt_CONTEXT_UPDATE_INTERVAL` or a sixth of the shortest own lease | HEARTBEAT | broadcast | the context's **generation**, and its update interval and miss threshold |
| request for the list | ACKNACK | unicast to the summary's sender | the generation wanted |
| the list (announce) | DATA, or FRAG_FIRST + FRAG_CONT | unicast to the requester | `tt_AnnounceHeader` + `tt_UpdateEntity` records |
| a change | the list | broadcast, after `tt_CONTEXT_TX_INTERVAL` of quiet | the new generation |

- The generation changes exactly when the endpoint list does, and across restarts. A known generation is liveliness only.
- **The summary carries the sender's timing** (2026-10-22): `summary_interval`, 2 bytes, most significant first - 0 = the
  default, else 3 bits of unit (1, 10, 100 us, 1, 10, 100 ms, 1 s, 100 s) and a 13-bit count, rounded up, so a step is
  at most 1.2% of the value and never shortens it, up to a day - and `summary_miss_threshold`, 1 byte, 0 = the default.
  A receiver judges that sender's silence by them, (2 x threshold + 1) x interval / 2, from every summary; until the
  first one, and again after the sender is presumed dead, by its own. So contexts on different intervals (a T1S or
  Wi-Fi profile beside Ethernet) do not presume each other dead. Default contexts send zeros: their bytes are the
  parent's (`tests/wire_identity_discovery_timing.sh`). A peer built before the change sends zeros, judged as the
  default, and judges by its own as it always did. Two contexts at 1 s and 4 s over a veth pair for 30 s: no death
  either way; with the receiver's use of the value removed, the 1 s side presumed the 4 s side dead 7 times, 3.5 s
  after each summary (`examples/perf_hil/experiments/discovery_interval_netns.sh`, x86 PC, 2026-10-22). Off the hot
  path: user instructions per sample on the PC, p1 p3 t1, 3 reps, against 6c355fa2 with a placement control, every
  row EQUAL (client -0.01%, -0.01%, +0.00%; `initoptions_insn_pc.sh`).
- An unknown generation draws a request, retried after RTT + `tt_CONTEXT_TX_INTERVAL` (seed `tt_DISCOVERY_REQUEST_RETRY`),
  up to `tt_DISCOVERY_REQUEST_ATTEMPTS`, in `tt_DISCOVERY_PENDING_REQUESTS` slots.
- Up to `tt_UNICAST_PEER_THRESHOLD` requests per tick are answered unicast, more by one broadcast.
- **A reply never waits for room:** with a full send buffer (a large sample draining) the announce reply and the list
  answer are skipped and counted (`discovery_replies_skipped`); the peer asks again.
- **A large list is fragmented at entity boundaries**, each part applied on arrival (no reassembly memory), at most
  `tt_UPDATE_MAX_PARTS`; a context needing more is invisible.
- **Nodes are announced** (v11) as `tt_KIND_NODE` entries; every entry carries its node index in spare bits of `kind`/`qos`.
- **The discovery table** (`tt_MAX_DISCOVERED_ENTITIES`, 568 B an entry; indexed above 64) feeds RxO, leases and the
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
| retry | `call_retry_interval` if set; else first wait `srtt + max(G, 4 x rttvar)`, doubling, each at most `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` x max(srtt, G), and after the last send that ceiling; seed `tt_CALL_RETRY_INTERVAL`; G is twice the reliable one (`tt_CALL_RETRY_GRANULARITY` fixes it). srtt/rttvar sample only calls never resent (Karn); a resent call's doublings carry to the next call's first wait until a clean answer (`tt_CALL_BACKOFF_MAX`) |
| give-up | after `2^(count+1) - 1` first waits, never later than `(count + 1) x tt_CALL_DEADLINE_PER_SEND`; a timeout doubles srtt once |
| BEST_EFFORT Client (D6) | sends each request once; a lost call ends with `tt_CALL_TIMEOUT` at the RELIABLE give-up, so a late answer still lands. Decided in `call_retry()`: the call path compiles unchanged. `tt_Client_end_oldest_call()` frees a slot for the next call |
| response cache | `tt_MAX_SERVER_CACHE_COUNT` slots keyed by (source, `client_tag`, seq_no): a retry never runs the callback twice. Kept by a BEST_EFFORT Server too: the retries come from RELIABLE clients |
| call window | a Server keeps `tt_CLIENT_CALL_WINDOW` (2 x outstanding) answers per Client; a Client makes no call while one is that many calls old; both ends must agree |
| answer lifetime | the longer of the client's horizon (auto: its deadline + one `tt_CALL_DEADLINE_PER_SEND`, 1.25 s; explicit: `(count + 1) x call_retry_interval`) and `tt_SERVER_CACHE_GAP_MULTIPLE` x its longest response-to-retry gap; dropped for a new incarnation; with every slot live, a new request is turned away before its callback (its client retries) unless its Client holds a full window of its own; a deferred answer still evicts the oldest-sent |
| deferred | `tt_CALL_DEFERRED`, then `tt_Server_send_response()` from any thread; reclaimed after `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` |
| turned away | `tt_CALL_IGNORED` keeps nothing; the retry asks again |
| size | never fragmented: bounded by `tt_MAX_BUFFER_LENGTH` |

**Sizing a server's kept answers** (the user, 2026-10-22). A smaller cache never runs a callback twice: a request that
finds every slot live for other calls is turned away before its callback, and its retry asks again. It only lowers
how many calls a server takes. With an answer kept L (1.25 s on the auto path), N slots take N / L calls/s summed over
Clients that hold fewer than `tt_CLIENT_CALL_WINDOW` (8) answers each, and any rate from N / 8 Clients, since a server
keeps at most 8 per Client. Each slot must hold the CallResponse as sent, 4 + 8 bytes of headers plus the encoded
answer, rounded to 8; an answer larger than its slot is sent and not kept, and its retry runs the callback again.

| build | slots x entry | `struct tt_Server` (RV32) | takes |
|---|---|---|---|
| core default | 64 x 2,944 B | 191,544 B | 51 calls/s, or 8 Clients at any rate |
| FreeRTOS default | 16 x 2,944 B | 47,976 B | 12.8 calls/s, or 2 Clients at any rate |
| FreeRTOS examples (`set_bool`, `ping_pong`) | 8 x 24 B, caller storage | 560 B + 192 B | 6.4 calls/s, or 1 Client at any rate; they make 2 |

A slower answer lifetime lowers the rate in proportion: an explicit `call_retry_interval` keeps answers `(count + 1) x`
it, and a server that has seen late retries keeps them `tt_SERVER_CACHE_GAP_MULTIPLE` x the gap (up to ~4 s on a lossy
auto path: 16 slots then take 4 calls/s). The client's default stays: its four slots must fit any request
(`tt_CLIENT_MAX_OUTSTANDING` must match the servers'), and they are 11.8 KB, not 188. The examples cut it too
(`max_request_bytes`, 64-96 B). `tests/test_service_small_cache.c` runs the examples' build: one Client at 2 and at 200
calls/s with answers lost, each call run once and none turned away; 3 Clients at 6 calls/s, none turned away; 4 at 8
calls/s, 32 of 160 calls turned away and timed out, none run twice; with 7 slots the 6 calls/s arm turns calls away
too, and with 16 the 8 calls/s arm does not, so the limit sits where the arithmetic puts it.

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
| **Send cursor** | on `EAGAIN` the rest goes from the poll when the socket reports half free (`EPOLLOUT` on a dup), with `tt_LARGE_SEND_WRITABLE_FALLBACK_NS`; without that, a timer `tt_LARGE_SEND_RETRY_NS` doubling to `_MAX_NS`. On a paced link it also stops when the link's bucket is empty and resumes from the scheduler when a unit fits (section 2, "Send pacing") |
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

**Settings** (not QoS; `tt_InitOptions.net`, section 2):

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

## 9. QoS

**TL;DR.** rmw's QoS in core: a profile at creation, zero = rmw's default (RELIABLE, KEEP_LAST 10), depth in
samples under a byte cap, compatibility judged on announce, a reader's lifespan the shorter of its writer's and its own.

**API** (2026-10-18). QoS is given at creation and only then: `tt_Node_create_publisher(node, pub, type, topic, qos,
options, storage, size)`, and `_subscriber`, `_client`, `_server` alike. `struct tt_QosProfile` holds one struct per
policy (history, reliability, durability, deadline, lifespan, liveliness): the rmw fields, then TickLE's extensions
(`history.bytes`; `reliability.tracking_words`, `reorder_slots`, `max_blocking_ns`). Not ROS's struct: core has no
ROS dependency, and rmw_tickle converts. Named profiles: `tt_QOS_PROFILE_DEFAULT`, `_SENSOR_DATA` (BEST_EFFORT,
KEEP_LAST 5), `_SERVICES_DEFAULT`. `tt_*_get_actual_qos()` returns the resolved profile.

| rule | detail |
|---|---|
| zero field | the computed default, `rmw_qos_profile_default`: RELIABLE, KEEP_LAST 10, VOLATILE, AUTOMATIC, infinite. A zero depth, `history.bytes` or `max_blocking_ns` takes the creating context's default (`tt_InitOptions.qos`, bundle 5), unless set config.h's |
| resolution | `tt_QosProfile_resolve()`: SYSTEM_DEFAULT, BEST_AVAILABLE (from the discovery table), and out-of-range values refused, not adjusted |
| reorder buffer | a RELIABLE Subscriber's `reorder_slots`: 0 = its window, else at least the window less one or `tt_QOS_REORDER_NONE`; fewer are refused, since they storm with re-requests under loss (16 of 256 at 20%: 65 of 2,000 samples after 21 M DATA, `tests/test_reorder_window.c`) |
| hot path | the resolved profile is written into the endpoint's existing fields; only what none held is appended |
| storage | the caller's, `tt_*_storage_size()` bytes - `tt_*_storage_size_for(ctx, ...)` on a context whose `qos` raises a default: the former keep config.h's, and short storage is refused with the size and the raising field named, never overrun or answered with a smaller history; a profile that needs it fails without it, never falls back to BEST_EFFORT. A Publisher's: the cache, an index ring of depth samples' datagrams, an arena of (depth + 1) records on a 64 B boundary. Creation clears only what holds anything (a reorder slot: its header) |
| per sample | nothing the hand-built caches did not pay: a KEEP_LAST cache whose ring and arena keep the depth for samples of its type's largest shape skips the sample count (`depth_assured_*`) until a sample falls outside it |
| depth | counts samples; `history.bytes` caps a Publisher's cache: a warning at creation, the depth kept in `tt_*_get_actual_qos()`. An unbounded type (`max_encoded_size` 0, or a size its binding chose: `max_encoded_size_estimated`) keeps the depth asked for; a sample that does not fit is counted at publish (`depth_shortfalls`) |
| not QoS | `tt_PublisherOptions`, `tt_SubscriberOptions`, `tt_ClientOptions` (call retry), `tt_ServerOptions` |
| services | default `tt_QOS_PROFILE_SERVICES_DEFAULT` (RELIABLE, VOLATILE, KEEP_LAST 10); a Client's reliability decides resending (section 7). TRANSIENT_LOCAL and MANUAL liveliness are refused: no call is retained for a late server, and nothing asserts a service's liveliness |

`tt_Topic` is now `tt_TypeSupport`. Deprecated until callers move: `tt_Context_create_*()`, `tt_Node_create_*_legacy()`,
and QoS written into endpoint fields.

**RxO matching.** `tt_UpdateEntity` carries `qos` bits (reliable, durable, manual liveliness), deadline and lease;
DATA from an incompatible writer is dropped (`rxo_drops`). Compatibility is judged when an announce is applied and when
an endpoint is created, by the same rule: each incompatible pair counts once per endpoint (`tt_QosIncompatibleStatus`,
first failing policy) and fires `tt_Context_set_qos_incompatible_callback()` (D2, 2026-10-19). rmw_tickle's
`rmw_qos_profile_check_compatible()` answers OK, WARNING or ERROR by rmw's rules.

**Liveliness** (v9): a lease runs from the last sign of life.

| rule | detail |
|---|---|
| sign of life | AUTOMATIC: any datagram from the context; MANUAL_BY_TOPIC: that writer's DATA or a liveliness HEARTBEAT (`tt_Publisher_assert_liveliness()`) |
| timer | one `check_liveliness()` entry at the earliest expiry; a lapsed entity is tombstoned and revives on its next sign |
| context silence | the sender's 3.5 summary intervals as its summary says (`tt_LIVELINESS_SILENCE_NS` at the defaults; our own before its first summary), or the longest announced lease up to `tt_CONTEXT_MAX_LEASE_NS`; its state is then dropped |
| summaries | every interval or lease / `tt_LIVELINESS_LEASE_DIVISOR`; extra ones skipped when every peer heard us |
| descheduled host | the check defers up to `tt_LIVELINESS_MAX_DEFERRALS` x `tt_LIVELINESS_DEFER_NS` while datagrams wait |

**Deadline:** announced for RxO; rmw_tickle enforces it (D4): one miss per period P with no activity, the timer re-armed
at the last activity + P (a timestamp store per sample, no scheduler call). **Lifespan:** a writer's older cached sample is neither
resent nor replayed. A reader is held to the **shorter of the writer's lifespan and its own** (D1, v14): the writer's
travels in its announce, and `tt_Subscriber_delivering_lifespan()` answers inside the delivery callback. Core delivers
on arrival and keeps no queue, so it drops nothing itself; rmw_tickle drops a queued sample when its age reaches it.
No lookup unless a writer with a lifespan has been seen (`tt_Subscriber.writer_lifespans`). The age is the reader's date
minus the writer's source timestamp, so **across hosts it is off by their clock offset**, as in DDS: a writer clock
behind the reader's shortens a sample's life, one ahead reads as age 0 (`test_lifespan_writer`). **Durability (TRANSIENT_LOCAL):** a durable Publisher unicasts its cache to a new Subscriber, once per
(context id, generation); a durable Subscriber refuses a VOLATILE writer. **Local delivery** (`tt_LOCAL_DELIVERY`):
in-process delivery under the same rules; otherwise own DATA is dropped.

## 10. Shared-memory transport

A second carrier, not a protocol: a ring slot holds the datagram that would have gone on the wire, through the same
acceptance path. Chosen per peer. `tt_SEGMENT_ENABLED`: on by default on Linux, between processes; off by default on
FreeRTOS, where `-Dtt_SEGMENT_ENABLED=1` puts it between the contexts of one image (below).

**The segment.**

| rule | detail |
|---|---|
| one per receiving context | `/dev/shm/tickle-seg-<ip>-<port>-<context id>`, written by every same-host peer |
| the header is the authority | magic, `tt_SEGMENT_VERSION` (6), owner, `incarnation`, geometry, `features`; any mismatch: UDP, counted |
| slots | power of two, 16 B header (`length`, sender, `seq_span`, `sequence`) + payload; claim by CAS, publish `index + 1` with release, free with `index + slots`; no wrap straddle |
| geometry | `tt_SEGMENT_BYTES` / (16 + `tt_SEGMENT_SLOT_BYTES`) rounded down; runtime `segment.bytes` and `segment.slot_bytes`; too large: UDP (`segment_oversized_to_udp`) |
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
| a held slot is passed over | see "Held slots" below; a ring under 4 slots keeps the old rule (full one lap later) |

**Held slots** (2026-10-20). A slot whose record is still in use after it was read carries a held mark instead of
"published": free for index i is `i`, published `i + 1`, held `p + 2` (p: the last index it was published or passed
at). A writer that meets a held slot at index i moves the mark to `i + 2` by CAS, moves `write_index` past i and claims
the next; the reader meets `i + 2` (or `i + slots`: passed, then released) and moves past it, moving `write_index`
first if the marking writer has not. The release CASes `p + 2` to `p + slots`, free for the next index at that
position. Hot paths unchanged: marks are looked at only where a writer or reader used to give up
(`shm_held_passed` counts the reader's passes). Tests: `test_segment_held.c` (control: byte-identical to the old ring
with nothing held), `tests/mutants_segment_held.sh`. Cost with nothing held (PC, `heldslot_samehost_insn_pc.sh`,
2026-10-20, 6 runs an arm, A P B B P A): PASS 16/16 on instructions per sample, writers -0.08..+0.38%; the be readers'
count follows the reader's wait mode (sleeping ~890, watching ~1,500-1,700 a sample), not the build. The two slow paths
are cold and out of line: inlined, the writer's claim carried 3 more instructions (+0.3-0.5% a sample). Rig: pending.
Since 2026-10-21 the reader passes a head marked `i + 2` by CASing it to `i + 3` ("passed by the reader"), a held mark
like `p + 2` everywhere else (writers pass it, the release frees it, a reference may name it), so that a claim's
publish can tell whether the reader got there first (below).

**Claimed-slot publish** (`tt_Publisher_claim` / `_publish_claimed` / `_abandon_claim`): the caller builds the CDR in
the subscriber's slot.

| rule | detail |
|---|---|
| when | a lone DATA to one attached same-host peer whose segment has `tt_SEGMENT_FEATURE_HELD_REF`; otherwise refused with a reason (`UNSUPPORTED`, `OUT_OF_BUFFER`, `WOULD_BLOCK`, `ILLEGAL_STATUS`), meaning "publish ordinarily" |
| in place when it still can be | nothing claimed in the ring after it (`write_index` = its index + 1) and the reader not past it: the publish CASes the mark `i + 2` to `i + 1`, against the reader's `i + 2 -> i + 3`; read at its own index, one ring index and no pass (`shm_claims_in_place`). Why: by reference every claim cost two indices, a pass and an out-of-order read - rig ab_loans slots 2026-10-20 (A `04c71310`, B f588b101 of ab/slot-loans before its rebase onto main, here `b3bcfb62`), loans on, rtt array1k block +2.6 us best effort and +1.85 us reliable, `shm_held_passed` = `shm_refs_read` = claims in every B run. PC (`slotloans_cost_pc.sh`, back-to-back array1k, 6 runs an arm): user-mode instructions a round trip on each side, A 14.9-17.2k, B +~410 (+2.5%), in place +~120-135 (+0.8%), every claim in place; the pre-registered user+kernel reading is SAME for both (bimodal wake-up costs) |
| held, by reference | otherwise: the claimed slot is held at once (the ring passes it); the publish writes a `tt_SEGMENT_SPAN_REF` record (`struct tt_SegmentRef`: index, length) in the next free index, and the reader reads the datagram in the held slot there, then gives it back (`shm_refs_read`; a malformed or foreign one: `shm_refs_refused`). No room for the reference: dropped as a full ring drops |
| several per publisher | up to `tt_PUBLISHER_CLAIMS` (4); ordinary publishes go on meanwhile; the seq_no is taken at the publish, so samples are read in publish order (until 2026-10-20: one claim, other publishes refused, the ring stopped at it) |
| re-checked | if the destination changed since the claim, the bytes are copied and published ordinarily (`shm_claims_copied`); KEEP_ALL is checked at the claim and at the publish (`WOULD_BLOCK` keeps the claim) |
| RELIABLE | still copies into its cache; destroy abandons every outstanding claim |

**The segment on FreeRTOS** (the user, 2026-10-20; off by default, 2026-10-21). Contexts in tasks of one image share an
address space, so the HAL
(`hal_freertos.c`) gives the ring as RAM it finds again by name; core's ring, header checks, fence, incarnation and
sleep generations are unchanged. Discovery and matching stay UDP.

| HAL part | FreeRTOS |
|---|---|
| region | a table of `tt_HAL_FREERTOS_SEGMENTS` names: create / attach (counts a user) / detach / unlink, memory returned with neither name nor user; never `ORPHANED` |
| memory | `tt_freertos_set_segment_memory()`: the application's region, else the FreeRTOS heap; 24 KiB default (`tt_SEGMENT_BYTES`, 16 slots); none: UDP |
| doorbell | a ring sets the owner's flag and gives the semaphore `tt_receive()` waits on, reported as a zero-length datagram; netconn build only (sockets: UDP doorbell) |
| reader | always sleeps (`tt_SEGMENT_READER_WAITS` 0): one core, no time slicing, so spinning would stop the writer |
| same image | one netif address, and lwIP loops back only unicast to it: each broadcast is also sent to every other bound context's data port (`tt_HAL_FREERTOS_LOCAL_CONTEXTS`), arriving as a unicast would |
| cost | 12.8 KB RAM per context (`segment_peers[]`), ~16 KB code (RV32); a poll task's stack peak 4.1 KB, 3.7 KB without (QEMU) |

Shown by `platform/freertos/main_segment.c` in `make test-freertos`, built with `-Dtt_SEGMENT_ENABLED=1`: RELIABLE and
BEST_EFFORT, 200 samples each, all over the ring with no unicast over lwIP; with no memory, and compiled out (the
default), the same samples over UDP. The `freertos link` gate links an RV32 image both ways.

## 11. Memory model

- **Embedded:** server and client caches, scheduler (`tt_MAX_SCHEDULER_LENGTH`; full: refused, `sched_full`), peer and
  ack tables, per-id tables (`tt_MAX_CONTEXT_IDS`).
- **Caller-attached:** `reliable_cache`, reader window and reorder buffer, server/client storage, the lending pool, the
  large-buffer callbacks. Core holds pointers only.
- rmw_tickle allocates with the application's `rcl` allocator.

## 12. Context ids and same-host peers

Per-peer tables are indexed by the 8-bit context id, unique per link.

- Without claiming: the address's last octet, or `tt_InitOptions.net.context_id`.
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

Compile-time `-D` overrides unless noted; blank rmw column = core default. The reliability timing
(`tt_RELIABLE_DEADLINE` to `tt_IDLE_HEARTBEAT_RETRY_MULTIPLE`, but for the request history) is the default of a
context's `tt_InitOptions.reliable` (section 2).

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
| `tt_RX_DRAIN_BYTES` / `_MAX` | 4,352 KiB / 4x | | drain buffers; per context `socket.drain_*`, with `_BUFFERS`, `_HOLDOFF_NS` |
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
| `tt_MAX_RELIABLE_HISTORY` | 64 | | rmw_tickle's and the examples' cache depth; core reads none |
| `tt_QOS_DEPTH_DEFAULT` / `tt_QOS_KEEP_ALL_DEPTH_DEFAULT` | 10 / 256 | | a zero `history.depth`: rmw's / one ACK window; per context `tt_InitOptions.qos.depth` / `keep_all_depth` |
| `tt_QOS_HISTORY_BYTES_DEFAULT` / `tt_QOS_KEEP_ALL_BYTES_DEFAULT` | 1 MiB / 512 KiB | | a zero `history.bytes`, as rmw_tickle's; per context `qos.history_bytes` / `keep_all_bytes` |
| `tt_QOS_MAX_BLOCKING_DEFAULT_NS` | 100 ms | | zero `max_blocking_ns`, per context `qos.max_blocking_ns`; a zero `reorder_slots` is the window |
| `tt_CLIENT_MAX_OUTSTANDING` | 4 | | calls per Client |
| `tt_CLIENT_CALL_WINDOW` | 8 | | answers kept per Client (2 x outstanding) |
| `tt_CALL_RETRY_INTERVAL` | 5 ms | | seed until a first answer |
| `tt_CALL_RETRY_GRANULARITY` | 0 (2 x G) | | adds the server's dispatch |
| `tt_CALL_RETRY_MAX_SRTT_MULTIPLE` | 64 | | wait ceiling |
| `tt_CALL_DEADLINE_PER_SEND` | 250 ms | | a call fails within (count + 1) x this (1 s) |
| `tt_CALL_RETRY_COUNT` | 3 | | retries |
| `tt_SERVER_CACHE_GAP_MULTIPLE` | 4 | | answer lifetime in observed retry gaps |
| `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` | 5 s | | deferred slot lifetime |
| `tt_MAX_SERVER_CACHE_COUNT` | 64 (16 FreeRTOS) | | answers kept; FreeRTOS: 12.8 calls/s shared, or 2 Clients at any rate (section 7) |
| `tt_SERVER_CACHE_ENTRY_LENGTH`, `tt_CLIENT_CACHE_LENGTH` | 2 x buffer | 8 | inline storage (rmw attaches its own) |
| `tt_MAX_SCHEDULER_LENGTH` | 128 | 512 | scheduler entries |
| `tt_SCHED_INBOX_LENGTH` | 32 | | cross-thread timer inbox |
| `tt_SCHEDULER_IO_INTERLEAVE` | 8 | | due entries before an I/O check; per context `poll.scheduler_io_interleave` |
| `tt_RECEIVE_TIMEOUT` | 100 us | | busy-poll slice, idle-socket recheck; per context `poll.receive_timeout` |
| `tt_RX_BATCH` | 32 (1 if buffer > control) | | datagrams per `recvmmsg()` |
| `tt_RX_CLOCK_REFRESH` | 16 | | datagrams per clock read; per context `poll.rx_clock_refresh` |
| `tt_RX_LOCK_CHUNK` | 8 | | datagrams per lock hold, bounds a publisher's wait; per context `poll.rx_lock_chunk` |
| `TT_RX_TIMER_LONG_NS` | 10 ms | | longer waits end on the `timerfd`; 0 = never |
| `TT_RX_TIMER_LONG_EXACT` | 1 | | re-arm whenever the deadline differs (0 = only earlier) |
| `TT_RX_TIMER_EDGE` | 1 | | edge-triggered `timerfd`, no `read()` |
| `TT_RX_TIMER_SLACK_NS` | 1 | | polling thread timer slack; 0 = leave it |
| `tt_HAL_RX_HINT` | AUTO (0) | | io_uring receive hint; AUTO or URING per context `socket.rx_hint`, READ compiled |
| `tt_HAL_FREERTOS_NETCONN` | 1 | | FreeRTOS netconn receive hint; 0 = BSD sockets |
| `tt_SOCKET_BUFFER_SIZE` | 1 MiB | 4 MiB | SO_SNDBUF/SO_RCVBUF request; per context `socket.buffer_size` |
| `tt_SEGMENT_ENABLED` | 1 (0 FreeRTOS) | 1 | shared-memory transport (FreeRTOS: between contexts of one image) |
| `tt_SEGMENT_BYTES` | 768 KiB (24 KiB FreeRTOS) | | ring budget: 512 (16) slots of 1472 B |
| `tt_SEGMENT_READER_WAITS` | 1 (0 FreeRTOS) | | reader may spin for writers instead of sleeping |
| `tt_HAL_FREERTOS_LOCAL_CONTEXTS` / `_SEGMENTS` | 4 / 4 | | contexts of one image that hear each other's broadcasts / segments alive |
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
| `tt_PUBLISHER_CLAIMS` | 4 | | claimed slots one Publisher may hold at once |
| `tt_SAMPLE_RETAIN_MAX` | 16 | | samples held per context |
| `tt_RX_POOL_MAX` | 32 | | attached receive buffers |
| `tt_CONTEXT_ID_CLAIM` | 1 (0 FreeRTOS) | 1 | host-registry id claiming |
| `tt_LOCAL_DELIVERY` | 0 | 1 | in-process delivery |
| `tt_DISCOVERY_OPTIONS` | 0 | 1 | discovery range, static peers |
| `_tt_CONTEXT_PORT` | 8282 | + domain | well-known port |
| `_tt_CONTEXT_BROADCAST` | 255.255.255.255 | | set the link's directed broadcast |

Linux HAL switches, on by default: `TT_UDP_OFFLOAD`, `TT_UDP_CSUM0`, `TT_RX_NONBLOCK`, `TT_RX_DIRECT`,
`TT_RX_LAZY_TIMER`, `TT_RX_LAZY_ADAPT`; `TT_RX_URING_WAIT` defaults to the build's `tt_RX_URING_WAIT`. Each is a context option (`tt_InitOptions.socket`, `tt_SWITCH_ON` / `_OFF`) and
an environment variable (the A/B control): the variable, when set, decides (`0` off, anything else on); unset, the
option does.

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
