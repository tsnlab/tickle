/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h> // size_t, for struct tt_SegmentPeer.mapped_bytes
#include <stdint.h>

#include <tickle/config.h>
#include <tickle/hal.h>

// _Alignas is a C11 keyword, not a C++ one (C++11's own equivalent is the unprefixed `alignas`,
// a real keyword there rather than a macro) - a plain `_Alignas` fails to parse under a C++
// compiler (misread as a function declarator). This header is otherwise plain C11, safe to
// #include from C++ (rmw_tickle/rosidl_typesupport_tickle_c both do) except for this one spot -
// tt_ALIGNAS keeps it that way rather than requiring every C++ includer to pre-define _Alignas
// itself.
#ifdef __cplusplus
#define tt_ALIGNAS(n) alignas(n)
#else
#include <stdalign.h> // _Alignas (C11) - already a keyword on most compilers, but not guaranteed
#define tt_ALIGNAS(n) _Alignas(n)
#endif

// Library version (semantic). tt_VERSION further down is the on-the-wire *protocol* version and
// moves independently. TICKLE_VERSION packs major/minor/patch for a single #if comparison, e.g.
//   #if TICKLE_VERSION >= TICKLE_VERSION_MAKE(1, 2, 0)
#define TICKLE_VERSION_MAJOR 1
#define TICKLE_VERSION_MINOR 0
#define TICKLE_VERSION_PATCH 0
#define TICKLE_VERSION_STRING "1.0.0"
#define TICKLE_VERSION_MAKE(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define TICKLE_VERSION TICKLE_VERSION_MAKE(TICKLE_VERSION_MAJOR, TICKLE_VERSION_MINOR, TICKLE_VERSION_PATCH)

// Same string as TICKLE_VERSION_STRING, but from the compiled library - lets a caller check what
// a prebuilt libtickle.a actually is, not just what its headers say.
const char* tt_version(void);

#define tt_KIND_NONE 0x00
#define tt_KIND_TOPIC 0x01
#define tt_KIND_SERVICE 0x02
#define tt_KIND_SENDER 0x10
#define tt_KIND_RECEIVER 0x20
#define tt_KIND_TOPIC_SUBSCRIBER (tt_KIND_RECEIVER | tt_KIND_TOPIC)
#define tt_KIND_TOPIC_PUBLISHER (tt_KIND_SENDER | tt_KIND_TOPIC)
#define tt_KIND_SERVICE_CLIENT (tt_KIND_RECEIVER | tt_KIND_SERVICE)
#define tt_KIND_SERVICE_SERVER (tt_KIND_SENDER | tt_KIND_SERVICE)
// A node's entry in an announce (CONTEXT_NODE_PLAN.md stage 3): TOPIC and SERVICE together, which no endpoint is.
#define tt_KIND_NODE (tt_KIND_TOPIC | tt_KIND_SERVICE)

struct tt_Endpoint;
struct tt_Context;
struct tt_Node;
struct tt_Discovery;

// Task Control Block
struct tt_TCB {
    uint64_t time;
    void (*function)(struct tt_Context* node, uint64_t time, void* param);
    void* param;
};

// Fired by a registered struct tt_Discovery (tt_Context_set_discovery()) whenever a remote entity
// appears, is refreshed (a repeat announce - harmless to ignore if a caller only cares about
// appear/depart), or departs (`departed` true - either an explicit farewell UPDATE or
// check_liveliness()'s own timeout). Deliberately minimal (DESIGN.md's "Concurrency" neighbor,
// rmw_tickle/PLAN.md's Milestone 0(c)): `type`/`name` aren't passed here at all - look them up
// via tt_Discovery_find(discovery, context_id, endpoint_id) if/when actually needed, rather than
// paying to decode/copy them for every caller whether they want them or not.
typedef void (*tt_DISCOVERY_CALLBACK)(struct tt_Context* node, uint8_t context_id, uint32_t endpoint_id, uint8_t kind,
                                      bool departed, void* param);

// How often a node's lock was taken, how often a caller found it already held, and for how long those
// callers waited in total (2026-09-25). Kept per lock so the rig can say where contention actually is
// before any lock is split further - see "Threading" at tt_Context_lock(). Updated by whoever holds the
// lock, so a reader on another thread may see a value one update stale, never a torn one.
struct tt_LockStats {
    uint64_t acquisitions;
    uint64_t contended;
    uint64_t wait_ns;
    // The part of contended/wait_ns the polling thread did - the one running tt_Context_poll() - so a wait
    // can be told apart by who waited: the poll thread for an application thread, or the other way round
    // (2026-09-26, rmw_tickle/RMW_PERF_PLAN.md H1). The rest was other threads.
    uint64_t poller_contended;
    uint64_t poller_wait_ns;
};

// struct tt_Context.sched_inbox_state[] values.
#define tt_SCHED_SLOT_EMPTY 0
#define tt_SCHED_SLOT_WRITING 1
#define tt_SCHED_SLOT_READY 2

#if tt_FRAG_ENABLED
// Bytes of struct tt_DataHeader, which is defined further down with the rest of the wire format;
// tickle.c checks the two agree.
#define tt_FRAG_DATA_HEADER_LENGTH 16

// One sample being put back together from its fragments (tt_SUBMESSAGE_TYPE_FRAG_FIRST/_CONT) - see
// process_frag() in tickle.c. A node holds tt_FRAG_REASSEMBLY_SLOTS of these, shared by every sender
// and every Subscriber, rather than one set per Subscriber or per remote writer: which samples are in
// flight at once is a property of the traffic, not of how many endpoints happen to exist.
//
// bytes[] holds the sample exactly as a DATA submessage body would - DataHeader, then CDR - so a
// completed slot is handed to process_data() as if it had arrived whole. 8-aligned, which puts the CDR
// 16 bytes in, at 0 mod 8, and keeps generated codecs' 4-aligned reads valid (a reassembled sample is never lent,
// so it need not match rx_buffer's 4 mod 8).
//
// Where a fragment goes follows from its index and the continuation payload size, which every
// fragment but the last shares (fragment 0 carries 11 bytes fewer, for its longer header). That size
// is learned from the first non-last fragment to arrive. The last fragment's length alone says
// nothing about its position, so when it arrives first it is parked at the very end of bytes[] and
// moved once the size is known.
struct tt_FragSlot {
    uint64_t received;  // bit i: fragment i has landed. 0: the slot is free
    uint32_t entity_id; // with source and seq_no, which sample this is
    uint32_t seq_no;
    uint32_t claimed;     // tt_Context.frag_clock when claimed; the lowest is abandoned first
    uint16_t cont_length; // payload bytes in each non-last FRAG_CONT, 0 until known
    uint16_t last_length; // payload bytes in the last fragment, 0 until it lands
    uint8_t source;
    uint8_t frag_count;
    // Free, but still naming the sample it last completed: a fragment of that sample arriving afterwards is
    // a duplicate, not the start of a new reassembly. A retransmission resends every fragment, and the one
    // that completes the sample is not always the last to arrive - without this, the rest opened a slot
    // that could never complete, one for every sample whose first fragment was lost (2026-09-26: 26,711
    // in a 5 s veth run at 5% loss, the same at 8, 32 and 128 slots).
    bool done;
    // + 4: a retransmission is cut from the cached record, which is padded to a multiple of 4
    tt_ALIGNAS(8) uint8_t bytes[tt_FRAG_DATA_HEADER_LENGTH + tt_MAX_SAMPLE_LENGTH + 4];
};
#endif

// A request for a peer's endpoint list not yet answered (struct tt_Context.discovery_requests); attempts == 0
// marks a free slot.
struct tt_DiscoveryRequest {
    uint32_t generation; // the one the peer's summary showed
    uint32_t ip;
    uint16_t port;
    uint8_t source;
    uint8_t attempts;       // requests sent so far, tt_DISCOVERY_REQUEST_ATTEMPTS at most
    uint64_t sent_ns;       // the latest of them, which the retry delay runs from
    uint64_t first_sent_ns; // the first, which the round trip is timed from (tt_Context.discovery_rtt_ns)
};

struct tt_Endpoint {
    uint8_t kind;
    // The index, in its context, of the node that owns this endpoint (CONTEXT_NODE_PLAN.md stage 2): the one it was
    // created on with tt_Node_create_*(), or 0 - the context's default node - for the tt_Context_create_*()
    // shorthands. An index, not a pointer: it sits in the padding after `kind`, so every endpoint struct keeps its
    // layout and size (a pointer here cost -R's receive 1 ns a sample), and it is what stage 3 puts on the wire.
    // tt_Endpoint_node() finds the node.
    uint8_t node_index;
    // hash(topic/service name + endpoint name) - a pure function of the name alone, deliberately:
    // this is how a Publisher and Subscriber (or Client and Server) on two different, otherwise-
    // unacquainted nodes agree on "the same" topic/service with zero negotiation, each computing
    // this independently from the shared name. NOT necessarily unique within one tt_Context any more
    // (Milestone 35, rmw_tickle/PLAN.md) - two local endpoints of the same kind can legitimately
    // share an id if they share a name, see add_endpoint_to_node()'s own doc comment (tickle.c).
    uint32_t id;
    const char* name;

    // Milestone 47 - this specific entity *instance*'s own identity, distinct from id above (a
    // pure name hash, shared by every entity - local or remote - with the same kind+topic/
    // service+endpoint name, by design). Assigned once, in add_endpoint_to_node() (tickle.c), as
    // node->entity_id_base + node->next_entity_id++ - see struct tt_Context's own entity_id_base/
    // next_entity_id doc comment for why that specific combination (a per-launch random base plus
    // a per-node counter, not pure-random or pure-linear alone). Carried on the wire as the
    // *sender's* own identity in struct tt_DataHeader/tt_HeartbeatHeader (both always
    // Publisher-emitted) and as the *target's* own identity in struct tt_AckNackHeader (mirroring
    // that header's own existing endpoint_id "target Publisher" convention) - see each field's own
    // doc comment. This is the real, root-caused fix for a confirmed cross-instance data-mixing
    // gap: before this field existed, a Subscriber's only way to tell two Publishers apart was
    // `id` above, which is identical for any two Publishers sharing a name - two different
    // tt_Context launches, or even two local Publishers on one tt_Context sharing a name (Milestone 35) -
    // see rmw_tickle/PLAN.md's own Milestone 47 for the full incident/design writeup.
    uint32_t entity_id;
};

// A node, as rmw means one: a name and a namespace within a context, owning endpoints (CONTEXT_NODE_PLAN.md stage 2,
// 2026-09-27). The context owns the transport - sockets, scheduler, liveliness, discovery - and hosts up to
// tt_MAX_NODES nodes, each with its index in the context. Index 0 is the context's default node, which the
// tt_Context_create_* shorthands create endpoints on; it comes into being the first time one is used (or
// tt_Context_default_node() is called), so a context that only ever creates nodes itself - rmw_tickle's - has none.
// Every endpoint records its node's index (tt_Endpoint.node_index). Nothing about nodes is on the wire yet (stage 3).
struct tt_Node {
    // Stage 3 (wire v11): the node as an entry of its context's announce - kind tt_KIND_NODE, id hash(namespace,
    // name), name the node's, entity_id its own random id, node_index its index; type string, the namespace (tickle.c's
    // endpoint_type_name()). Never registered as an endpoint: only listed in the announce. First, so a node can be
    // reached from its entry.
    struct tt_Endpoint entry;
    struct tt_Context* context; // NULL until tt_Node_create(), and again after tt_Node_destroy()
    const char* name;           // not copied - the caller's, as an endpoint's name is
    const char* namespace_name; // likewise
    uint8_t index;              // < tt_MAX_NODES; 0 is the default node's unless an explicit node took it
};

// Which transport carried a datagram (SHM_PLAN.md stage 0). Core decides this per peer; the module
// behind the seam only carries bytes. UDP is 0 so a zeroed context starts counting on the transport
// that always exists, and the enum is what the per-transport counters are indexed by - a third
// transport needs a value here and no new fields anywhere.
enum tt_Transport { tt_TRANSPORT_UDP = 0, tt_TRANSPORT_SHM, tt_TRANSPORT_COUNT };

// A shared-memory segment's own header, validated by a reader after it attaches (SHM_PLAN.md stage
// 1). The name a reader computes - (peer address, peer port, peer context id), all from ordinary
// discovery - is enough to be unique across network namespaces, because the address is exactly what
// separates them. It is NOT enough to be certain, and this header is the difference.
//
// What the name cannot see: a context id is re-handed once its holder dies, so a segment left by a
// dead context legitimately carries the name a new reader computes, and the triple has nothing in
// it that tells one incarnation from the next. Attaching to that segment would not fail - it would
// read a dead peer's records as the live peer's. The header turns that into a detected mismatch and
// a fall back to UDP, which is the same move the wire already makes with its version check, in the
// one place a wire check cannot reach.
//
// `incarnation` is drawn per launch from the same clock as tt_Context.entity_id_base. It is not
// derivable by the peer, so it travels here rather than on the wire: stage 0's claim is that the
// wire is untouched, and the address already distinguishes what a wire token would.
#define tt_SEGMENT_MAGIC 0x544b5347U // "TKSG", checked before anything else in the mapping is read
// 2 since 2026-09-29: the header gained reader_waiting. A context built against version 1 reads the
// ring correctly and never sets the flag, so its peers would publish to a sleeping owner and never
// ring the doorbell - added latency with nothing to see, which is what the version is for.
// 3 since 2026-10-03: a slot's `reserved` became `seq_span` (below). The dangerous direction is a new
// writer and an old reader - the writer puts 2 in a field the reader ignores, and the reader advances one
// seq_no where two were consumed, which is a permanent gap between two processes on one host. The other
// direction is safe on its own (an old writer writes 0, and cannot produce a multi-seq record anyway), but
// the version check covers both and that reasoning covers one. segment_attach() refuses a mismatch and the
// peer falls back to UDP, so the bump is the whole fix.
// 4 since 2026-10-05: reader_waiting carries the generation of the owner's sleep instead of 1, and a writer rings
// once per generation rather than once per read_index. A new writer with an old owner would see the generation stuck
// at 1 and ring once ever, so the bump - refused attach, UDP between the two - is again the whole compatibility story.
// 5 since 2026-10-05: write_index, read_index and reader_waiting each on a cache line of their own, which moves them.
#define tt_SEGMENT_VERSION 5
// The line size the segment header pads its shared indices to. 64 on the rig's Cortex-A76 and on x86-64; a target with
// a larger line still works, only with the false sharing this exists to remove.
#ifndef tt_SEGMENT_CACHE_LINE
#define tt_SEGMENT_CACHE_LINE 64
#endif
// Longest segment path this build can form: "/dev/shm/tickle-seg-255.255.255.255-65535-255" and a NUL.
#define tt_SEGMENT_PATH_LENGTH 64

struct tt_SegmentHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t owner_ip; // the three fields the reader computed the name from, echoed back to be checked
    uint16_t owner_port;
    uint8_t owner_context_id;
    uint8_t reserved2;
    uint32_t incarnation; // per launch: distinguishes a re-handed context id from its predecessor

    // The ring: many writers - every peer that wants to reach the owner - and one reader, the owner
    // itself. (This comment said "single writer, single reader" until 2026-09-29; that was the
    // topology an earlier draft of SHM_PLAN.md described and not the one built, and the description
    // being wrong here is how `write_index` came to be claimed with a plain load and store.)
    // Fixed-size slots rather than a byte stream: a datagram never straddles the wrap, so a reader
    // never sees half a record and there is no partial-write state to recover from after a writer
    // dies mid-record. The cost is the slack in a slot larger than its datagram, which is memory and
    // not correctness.
    //
    // The indices are plain uint32_t accessed with __atomic_* builtins rather than _Atomic, for the
    // reason this header already gives elsewhere: _Atomic is not a C++ type and these structures
    // are read from C++ too. They are free-running counters, not offsets - `slots` is a power of
    // two and the slot is index & (slots - 1), so "full" is a subtraction that stays correct across
    // the wrap where a comparison of masked offsets would not.
    uint32_t slots;      // power of two
    uint32_t slot_bytes; // payload capacity of one slot
    // One cache line each (2026-10-05, segment version 5). They were adjacent, so the writers' CAS on write_index,
    // the owner's store to read_index on every drain, and every writer's load of reader_waiting after every batch all
    // moved the same line between cores: perf on the rig put 8.3% of a p3 publisher's user cycles in the
    // reader_waiting load and 4% in the claim CAS (experiments/perf_publisher_profile.sh).
    tt_ALIGNAS(tt_SEGMENT_CACHE_LINE) uint32_t write_index;
    tt_ALIGNAS(tt_SEGMENT_CACHE_LINE) uint32_t read_index;

    // Set by the owner immediately before it blocks on its socket, cleared when it wakes. Shared
    // memory cannot wake a thread that is inside a socket wait, and once the segment carries nearly
    // all the traffic there is nothing left on the socket to wake it either: measured, the reader
    // then ran only on its timers and delivered 73,081 records where UDP delivered 12,463,222, at
    // 241 ms of latency. The module had been getting away with it only because a separate defect was
    // still pushing millions of datagrams down the socket.
    //
    // Checked by the writer AFTER it publishes, which closes the window where the owner sets the flag
    // between the writer's check and the writer's write; the owner drains once more after setting it,
    // which closes the window the other way. Under load the owner is never inside a wait, so the flag
    // is never set and the doorbell is never rung.
    // 0 while the owner is awake; while it is about to sleep or asleep, the generation of that sleep (never 0), so a
    // writer can tell a reader that went back to sleep from one that never woke (struct tt_SegmentPeer).
    tt_ALIGNAS(tt_SEGMENT_CACHE_LINE) uint32_t reader_waiting;
};

// One slot. `length` is the datagram's own length; the payload follows, and the slot is
// header->slot_bytes of payload capacity whatever the datagram's length.
struct tt_SegmentSlot {
    uint32_t length;
    // Who sent it. A socket arrival carries this from recvfrom(), and the acceptance path uses it
    // for more than diagnostics - discovery learns where a peer lives from the address its announce
    // arrived on. A record without it forces the reader to invent one, and a peer learned from an
    // invented address is recorded at that address and can never be reached again: it was 0.0.0.0:0
    // here, which made every later send to that peer fall out of the unicast path entirely.
    //
    // The rule this slot enforced until 2026-10-03 was that a record carries what the datagram would
    // have carried at the socket - attribution being right is not enough if a value inside it means the
    // wrong thing. seq_span below is the first field that is deliberately NOT that: it is information
    // only the shared-memory path has, and a datagram has nowhere to put it. So the rule now reads: a
    // record carries what the datagram would have carried, PLUS what only this path can say - and the
    // second kind must be unable to arrive from the network, which is why it lives here in the slot
    // header rather than in the submessage. A socket datagram has no slot header, so a span cannot be
    // injected; that is a structural guarantee rather than a check that could be forgotten.
    uint32_t sender_ip;
    uint16_t sender_port;
    // How many seq_nos this record covers (SHM_PLAN 6e). A sample consumes the number of seq_nos the
    // NETWORK form would need whatever path it takes, so a record carried whole in one slot where the
    // wire would have fragmented it covers more than one, and the reader must advance by this many.
    //
    // 0 means 1. Every record written before this field had a meaning wrote 0 here, and a reader that
    // treated 0 as "advance nothing" would stall on the first one.
    //
    // Bounded by tt_FRAG_MAX_COUNT: a span larger than that is a record the network form could not have
    // carried at all, since frag_count is a uint8_t counting the same datagrams. Asserted where it is
    // written rather than trusted from the arithmetic that produced it.
    uint16_t seq_span;
    // Which record this slot holds, and whether it is finished. A context's segment is written by
    // EVERY peer that wants to reach it and read by one - many writers, one reader - so the write
    // index alone cannot say a slot is ready: a writer that claims a later slot may finish before
    // one that claimed an earlier slot, and a reader trusting the index would read a slot nobody
    // had written yet.
    //
    // A writer claims an index with a compare-and-exchange, fills the slot, then stores index + 1
    // here with release. The reader takes a slot only when this equals its own read index + 1,
    // which is true exactly when that slot's writer has finished. On release it stores
    // index + slots, marking the slot free for the writer one lap later.
    uint32_t sequence;
};
// config.h cannot use sizeof(), so it spells the stride as a literal; this is where the two are
// held together. Nothing else would notice them drifting apart, and the symptom would be a reader
// indexing slots at a different pitch from the writer - every record after the first one wrong.
static_assert(tt_SEGMENT_SLOT_STRIDE == sizeof(struct tt_SegmentSlot) + tt_SEGMENT_SLOT_BYTES,
              "tt_SEGMENT_SLOT_STRIDE must match struct tt_SegmentSlot");

// Why an attach failed. Counted rather than collapsed into a boolean because all of these fall back
// to UDP safely and therefore look identical from outside - which means a module that is
// permanently inert in the field is indistinguishable from one correctly deciding "not same host".
// S2 catches an inert module in a two-namespace pair; only a counter catches it in a deployment.
// Same shape as the zero-copy counter that reported nothing while working (g15).
enum tt_SegmentAttach {
    tt_SEGMENT_ATTACHED = 0,
    tt_SEGMENT_ABSENT,      // no such segment: a different host, or a peer built without the module
    tt_SEGMENT_REFUSED,     // present but not openable: permissions, or another user's segment
    tt_SEGMENT_BAD_HEADER,  // magic or version wrong: not ours, or a version we cannot read
    tt_SEGMENT_WRONG_OWNER, // header's triple is not the peer we computed the name for: a collision
    tt_SEGMENT_STALE,       // right owner, different incarnation: the peer we knew has been replaced
    tt_SEGMENT_ORPHANED,    // a file no live context owns: left by a dead one, a ring nobody drains
    tt_SEGMENT_ATTACH_COUNT
};

// A delivered sample kept past its callback (receive-buffer lending, DESIGN.md section 10): tt_Sample_retain() fills
// it, tt_Sample_release() gives it back. `payload` is the sample's CDR exactly as it arrived - what the topic's decode
// functions are handed - and stays valid, unmoved and unchanged, until the release. `handle` is opaque and never 0.
struct tt_Sample {
    const uint8_t* payload;
    uint32_t length;
    uint32_t handle;
    bool is_native_endian;
};

#if tt_LARGE_SAMPLES
// Large-message stage 2 (DESIGN.md section 8): the caller's buffers, one per large sample. acquire returns `bytes` of
// memory core may write and keep until it hands the pointer back to release, or NULL for "no room" - never a crash.
// 8-aligned, as every receive buffer is (tt_Context.rx_buffer): a sample lent from it then reads in place.
// Core calls them with the context locked, from the publishing thread, the polling thread, or (release only) whichever
// thread calls tt_Sample_release(); they must not call back into TickLE. rmw_tickle backs them with malloc() and a free
// list; a core-only user can back them with a static pool. Set with tt_Context_set_large_buffers().
typedef void* (*tt_LARGE_ACQUIRE)(void* user, uint32_t bytes);
typedef void (*tt_LARGE_RELEASE)(void* user, void* buffer);

// Where a large sample's bytes sit in its buffer, on both sides: the DataHeader 4 bytes in, the CDR right after it - at
// 4 mod 8, as rx_buffer places a DATA's CDR, so a generated codec's aligned reads stay valid.
#define tt_LARGE_HEADER_OFFSET 4U
#define tt_LARGE_CDR_OFFSET (tt_LARGE_HEADER_OFFSET + tt_FRAG_DATA_HEADER_LENGTH)

// One large sample being put back together (process_frag(), tickle.c): its fragments are copied to their offsets in an
// acquired buffer as they land, in any order, and the sample is delivered from there - lent, when a Subscriber keeps
// it. Keyed by (source, entity_id, the sample's seq_no) and, for a RELIABLE Subscriber, that Subscriber: a RELIABLE
// one takes each fragment under its own seq_no and delivers in order, while best-effort ones share one assembly that
// is handed to all of them once whole.
struct tt_LargeAssembly {
    uint64_t landed[tt_LARGE_MAX_FRAGMENTS / tt_RELIABLE_BITMAP_WORD_BITS]; // bit i: fragment i is in the buffer
    uint8_t* buffer;                                                        // NULL: this entry is free
    struct tt_Subscriber* sub; // the RELIABLE Subscriber it is for; NULL: best effort
    uint64_t timestamp;        // from fragment 0, once it has landed
    uint32_t capacity;         // bytes acquired
    uint32_t entity_id;
    uint32_t seq_no;      // the sample's: its fragment 0's
    uint32_t endpoint_id; // from fragment 0
    uint32_t received;    // fragments landed
    uint32_t last_length; // payload bytes of the last fragment, 0 until it lands
    uint32_t claimed;     // tt_LargeState.clock at the claim: the best-effort reassembly claimed longest ago goes first
    uint16_t frag_count;
    uint8_t source;
    bool is_native;
    bool via_data_port;
};

// A context's large-sample state (tt_Context.large).
struct tt_LargeState {
    tt_LARGE_ACQUIRE acquire;
    tt_LARGE_RELEASE release;
    void* user;
    struct tt_LargeAssembly assemblies[tt_LARGE_ASSEMBLIES];
    uint32_t clock;
    // Counters, on the traffic line as large_*. reassembled: delivered whole. abandoned: given up whole or in part (a
    // fragment lost for good, a newer sample needing the room, its writer gone). no_buffer: acquire said no - a
    // best-effort sample is then lost, a RELIABLE fragment is left unrecorded and asked for again. dropped: fragments
    // refused as malformed or inconsistent with their sample. duplicate: fragments already landed. published: large
    // samples published. tail_abandoned: KEEP_LAST samples a newer publish replaced before all of them had gone - one
    // still waiting behind a send in progress, or (the ring full) one being sent.
    // window_too_small: KEEP_ALL publishes refused because a matched reader's window is narrower than the sample.
    // send_waits: large sends that found the socket's send buffer full and continued later.
    uint64_t reassembled;
    uint64_t abandoned;
    uint64_t no_buffer;
    uint64_t dropped;
    uint64_t duplicate;
    uint64_t published;
    uint64_t tail_abandoned;
    uint64_t window_too_small;
    uint64_t send_waits;
};
#endif

#if tt_SAMPLE_LENDING
// Where a lent sample's bytes live. FREE also means "not lendable": the datagram being processed (if any) is not one
// whose memory can be kept. LARGE: a large sample's own buffer (`region`), handed back to the context's large release
// once no entry names it.
enum tt_LendKind { tt_LEND_FREE = 0, tt_LEND_BUFFER = 1, tt_LEND_SLOT = 2, tt_LEND_LARGE = 3 };

// One retained sample. BUFFER: `index` is the receive buffer (0 the context's rx_buffer, k the pool's buffer k - 1).
// SLOT: `index` is the record's ring index and `region` the segment header it was read from.
struct tt_LendEntry {
    const void* region;
    uint32_t index;
    uint32_t generation; // the handle's upper 24 bits: a released or reused entry no longer answers to an old handle
    uint8_t kind;        // enum tt_LendKind
};

// The sample a Subscriber callback is being handed right now (deliver_payload(), tickle.c), on the delivering
// thread's stack. A nested delivery (a callback that publishes locally) puts the outer one back when it returns.
struct tt_LendDelivery {
    const struct tt_Subscriber* sub;
    const uint8_t* payload;
    uint32_t length;
    bool is_native;
};

struct tt_Lending {
    const struct tt_LendDelivery* delivery; // NULL outside a Subscriber callback
    // The datagram being processed, when its memory can be kept: [rx_base, rx_base + rx_length), in buffer or slot
    // rx_index. rx_kind is FREE outside process_datagram_at() and for anything else.
    const uint8_t* rx_base;
    uint32_t rx_length;
    uint32_t rx_index;
    uint8_t rx_kind;
    // The receive buffer the socket reads into next (0: rx_buffer). Changed only by a retain, on the polling thread.
    uint8_t landing;
    uint8_t pool_count; // buffers attached by tt_Context_set_rx_pool(), each tt_RX_POOL_BUFFER_BYTES
    // The lazy release of the own segment (forget_same_host_peer()) was put off because a slot was held or a record
    // was being read in place; the polling thread finishes it once neither is true.
    bool segment_release_deferred;
    uint8_t* pool;
    uint32_t held;       // entries in use
    uint32_t held_slots; // of which SLOT
    struct tt_LendEntry entries[tt_SAMPLE_RETAIN_MAX];
    // Counters, on the traffic line as lend_*. full_retained is the WRITER's: datagrams it dropped because the slot it
    // was refused is one its reader has read and still holds (shm_full_retained; also in segment_full_dropped).
    uint64_t retains;
    uint64_t releases;
    uint64_t unlendable;   // retain of a sample not in its datagram's memory: tt_RET_UNSUPPORTED
    uint64_t exhausted;    // no handle entry, or no spare receive buffer: tt_RET_OUT_OF_BUFFER
    uint64_t bad_releases; // handle 0, unknown, or already released: tt_RET_INVALID_ARGUMENT
    uint64_t full_retained;
    // While rx_kind is LARGE: the large sample's own buffer, which a retain keeps (large-message stage 2), and the kind
    // the datagram being processed had before the delivery replaced it - SLOT while the last fragment's record is still
    // being read from the ring, which must stay mapped under it. Last, so no field above moves.
#if tt_LARGE_SAMPLES
    uint8_t* rx_large;
    uint8_t rx_outer_kind;
#endif
};
#endif

struct tt_Context {
    uint8_t id;
    uint32_t endpoint_count;
    struct tt_Endpoint* endpoints[tt_MAX_ENDPOINT_COUNT];
    // Open-addressed index into endpoints[] keyed by endpoint id, for O(1) find_endpoint()
    // instead of an O(endpoint_count) scan. Rebuilt lazily from endpoints[] on the next lookup
    // after any add/remove (endpoint_index_valid = false marks it stale) - endpoints change
    // rarely, lookups happen per received packet.
    struct tt_Endpoint* endpoint_index[tt_ENDPOINT_INDEX_SIZE];
    bool endpoint_index_valid;
    uint64_t last_modified; // Last modified timestamp in ns to announce other nodes e.g. server, publisher

    // Milestone 47 - together, this node's own launch-scoped identity source for every locally-
    // created entity's own struct tt_Endpoint.entity_id (see its own doc comment for the full
    // rationale): entity_id_base is drawn once, at tt_Context_create() time, from tt_get_ns()'s own
    // low 32 bits (no separate RNG primitive needed - this node's own launch instant already is
    // one); next_entity_id starts at 0 and increments once per add_endpoint_to_node() call, one
    // shared counter across every entity kind (Publisher/Subscriber/Client/Server) on this node -
    // functionally equivalent to a per-kind counter (kind is already a separate dispatch
    // dimension elsewhere), just simpler to implement.
    uint32_t entity_id_base;
    uint32_t next_entity_id;

    // The seq_no of this context's next call, one counter across every Client on it. A CallRequest names its client
    // only by (source context, service endpoint_id, seq_no), and every Client of one service in one context has the
    // same endpoint_id, so a per-client counter gave two of them the same requests - the server answered them as one
    // and the second never got an answer (2026-10-09). Drawn per call, seq_no tells them apart: the server's cache
    // keys on it, and process_callresponse() hands the answer to the Client whose outstanding call it names. A value
    // another Client of the same service still has outstanding is skipped when the counter comes round to it
    // (client_call_locked()); the server's cache also names the Client (tt_CallRequestHeader.client_tag).
    uint16_t call_seq_no;

    // Per remote node (indexed by its node id), the last discovery announce we've acted on: its
    // generation (tt_DataHeader.seq_no of the announce, see tt_DISCOVERY_ENDPOINT_ID), and whether we've
    // seen it at all. Only these two facts are ever read back (dedup + first-contact detection - see
    // process_announce()), so there's no need to keep a copy of the whole variable-length announce.
    uint32_t update_generation[tt_MAX_CONTEXT_IDS];
    bool update_seen[tt_MAX_CONTEXT_IDS];
    // Per remote node, that its last acted-on announce must be acted on again: a local Publisher or Client was
    // created since, and learns its peers only from an announce (reprocess_known_announces(), tickle.c). A flag,
    // so marking twice is marking once; it was the stored generation inverted, which a second creation inverted
    // back (2026-10-09).
    bool update_reprocess[tt_MAX_CONTEXT_IDS];
    // Per remote node, an announce arriving in fragments that is not complete yet: its generation, how
    // many fragments it has, and which have arrived (bit i = fragment i; 0 = none in progress). Once
    // every bit is set the announce is complete and moves into update_generation[]/update_seen[] above,
    // exactly as an announce in one datagram would.
    uint32_t update_part_generation[tt_MAX_CONTEXT_IDS];
    uint32_t update_part_received[tt_MAX_CONTEXT_IDS][tt_UPDATE_PART_WORDS];
    uint8_t update_part_count[tt_MAX_CONTEXT_IDS];
    // Phase 2 (rmw_tickle/PLAN.md) - the tt_VERSION last logged as mismatched for each remote node
    // (0 = nothing logged yet), so a peer speaking a different protocol version is reported once
    // rather than once per packet. At max rate an unfiltered log line per rejected packet would be
    // its own denial of service.
    uint8_t version_mismatch_logged[tt_MAX_CONTEXT_IDS];
    // Per remote node (indexed the same way), the wall-clock time (tt_get_ns()) its most recent
    // UPDATE announce was received - unlike update_last_modified[] above, this moves on *every*
    // announce, including one whose content is unchanged from the last one acted on. Liveliness
    // (check_liveliness() in tickle.c) is judged from this, not update_last_modified[]: a node
    // whose endpoints never change still has to be heard from periodically, or it's presumed
    // gone once tt_LIVELINESS_MISS_THRESHOLD announce intervals pass with nothing heard.
    uint64_t update_last_seen[tt_MAX_CONTEXT_IDS];

    // The same index, but the most recent time ANY validated packet was received from that node -
    // DATA, ACKNACK, Heartbeat, UPDATE, anything whose header passed validate_packet_header().
    // Refreshed at one site in process_packet() rather than per submessage type, so it cannot
    // drift and so a type added later is covered without being remembered.
    //
    // It exists because one timestamp cannot answer two different questions. "Has this node
    // announced itself lately" is what the liveliness *schedule* is calibrated against; "is this
    // node alive at all" is what a peer streaming DATA answers plainly even when its announces are
    // the packets being dropped. Using traffic as the single clock fixes the second and slides the
    // first later, because detection fires at last_seen + threshold and traffic is always at least
    // as recent as an announce; measured at about +290ms on the HIL rig. So the two are kept apart
    // and a node is presumed dead only when BOTH have gone quiet - see check_liveliness() and
    // tt_Context_entity_alive() for each one's own guard.
    //
    // A retransmit or a duplicate counts, deliberately: it carries no new data, but it is proof
    // the peer's stack is running and transmitting, which is the only question being asked - and
    // under loss, retransmits may be most of what arrives.
    uint64_t traffic_last_seen[tt_MAX_CONTEXT_IDS];

    // 4-byte aligned so a decoded/encoded message payload (which sits at a fixed 4-multiple
    // offset past the framing headers) is itself 4-aligned - see "Interface serialization
    // (TickLE CDR-4)" in DESIGN.md. tt_Context already has >= 8-byte alignment (it holds uint64_t
    // members); _Alignas keeps that true for these buffers regardless of member reordering.
    tt_ALIGNAS(4) uint8_t tx_buffer[tt_TX_BUFFER_LENGTH];
    uint32_t tx_tail;
    uint32_t tx_size;
    // How many seq_nos the datagram now in tx_buffer consumes (SHM_PLAN 6e). 1 for every datagram the
    // wire can carry; N for a record carried whole in one segment slot where the wire would have sent N
    // fragments. Carried here rather than threaded because this is the structure that already carries
    // this datagram: flush_tx() is not handed the bytes either, it reads them from tx_buffer/tx_tail.
    //
    // It is the PUBLISHER's number. It is not recomputed further down - two independent deciders of how
    // many seq_nos a sample consumes, disagreeing after one of them changed, is the defect 6e exists to
    // remove, and re-deriving it at the write site would keep two deciders and only move the second one.
    //
    // Only set_tx_tail() may move tx_tail, and it returns this to 1 whenever the buffer empties, so a
    // span cannot outlive the datagram it belongs to. A stale one would be invisible: it would ride out
    // on the NEXT record, which has no reason to have a span at all.
    uint16_t tx_seq_span;
    // The receive-side counterpart, and the half that was missing until 2026-10-03. A record that
    // travelled whole carries the span its publisher allocated (tt_SegmentSlot.seq_span), and the
    // seq_nos after its own are consumed by that same sample - no separate record will ever carry
    // them. Until this existed the reader advanced its watermark by one per record, waited for a
    // number nothing would ever send, and ACKNACK-requested it forever: on the rig at slot_bytes
    // 4096 that turned 15,401 perfectly delivered records into 1 delivered sample, with every loss
    // counter reading zero because nothing was lost. The field was carried and no code read it,
    // which compiles, passes its tests, and looks exactly like working code.
    //
    // Set at the single entry point every datagram passes through (process_datagram_locked()), so
    // no path can forget to set it and no arrival can inherit the previous one's span. It is read
    // rather than consumed: one DATA can match several Subscribers, each with its own WriterProxy
    // and each needing the same span, so clearing it on first use would be correct for the first
    // reader and silently wrong for every other one. The socket path sets it to 1 - not as a
    // fallback but as the right answer, since there a sample's fragments really are separate
    // datagrams that each carry their own seq_no.
    uint16_t rx_seq_span;
    // Set whenever node_update()'s always-broadcast UPDATE announce is sitting batched,
    // unflushed, in tx_buffer (cleared once a flush actually sends it) - node_flush() must not
    // unicast while this is true, since tx_buffer is one shared buffer flushed as a unit and an
    // UPDATE has to reach the whole segment, not just a couple of known peers. See node_flush().
    bool tx_has_pending_update;
    // Core-owned: whether node_flush() is armed (2026-09-25). It used to reschedule itself every
    // tt_CONTEXT_TX_INTERVAL whether or not anything was waiting, so a node was never idle - a poll
    // waiting for the next scheduler entry still woke a thousand times a second for it. It is now
    // armed only when a submessage is left batched in tx_buffer, and on the same grid it always ran
    // on, so a batched datagram leaves at exactly the instant it would have before.
    bool flush_scheduled;
    // An announce owed for endpoints created since the last one (2026-09-26): armed by the first creation,
    // sent once no endpoint has been created for tt_CONTEXT_TX_INTERVAL, so a burst - rclcpp creates several
    // at node start - costs one announce. Without it a new endpoint waited for node_update()'s next
    // periodic tick, up to tt_CONTEXT_UPDATE_INTERVAL, before any remote node heard of it.
    bool announce_soon_scheduled;
    uint64_t endpoints_changed_ns;
    // Requests for this node's endpoint list answered in the current tt_CONTEXT_TX_INTERVAL tick (rmw_tickle/
    // DISCOVERY_PLAN.md rule 4): up to tt_UNICAST_PEER_THRESHOLD are answered unicast, one more turns them
    // into a single broadcast, and the rest wait for it.
    uint64_t discovery_reply_tick;
    uint8_t discovery_reply_count;
    // Requests for a peer's list not yet answered (tt_DISCOVERY_REQUEST_RETRY, config.h). One scheduler
    // entry, discovery_request_retry(), serves them all while any is open.
    struct tt_DiscoveryRequest discovery_requests[tt_DISCOVERY_PENDING_REQUESTS];
    bool discovery_retry_scheduled;
    uint64_t discovery_retry_ns; // when that entry is due, while discovery_retry_scheduled
    // Per source node, the smoothed round trip of our requests for its list (ns, 0 = none measured yet):
    // from a request's first send to the list it asked for being applied, so it includes the flush tick
    // the answer may have waited for. Timed from the FIRST send, as the reliable path times its recovery
    // (tt_WriterProxy.recovery_srtt_ns): a retried request's sample is biased upward, which errs toward
    // fewer premature retries, and a link slower than the seed is measured at all. A retry is sent this
    // plus tt_CONTEXT_TX_INTERVAL after the request (discovery_retry_after(), tickle.c). Kept across a
    // peer's death: it describes the path to that node id, and the next answer corrects it.
    uint32_t discovery_rtt_ns[tt_MAX_CONTEXT_IDS];
    // LIVELINESS (rmw_tickle/LIVELINESS_PLAN.md). One scheduler entry, check_liveliness(), armed at the
    // earliest expiry among the remote nodes and leased entities this node tracks (liveliness_check_ns),
    // re-armed when it fires and moved earlier only for something new that expires sooner.
    bool liveliness_check_scheduled;
    uint64_t liveliness_check_ns;
    // Per source node: what its traffic must be looked at for (tt_LIVELINESS_SOURCE_* in tickle.c) - a
    // MANUAL_BY_TOPIC Publisher whose DATA asserts it, or an entity lapsed on its lease that traffic revives.
    // Zero for every node with neither, so their traffic costs nothing extra.
    uint8_t liveliness_flags[tt_MAX_CONTEXT_IDS];
    // When node_update() next sends the summary; the interval shrinks to a tt_LIVELINESS_LEASE_DIVISOR-th of the
    // shortest lease any
    // of this node's own endpoints announce (summary_interval()).
    uint64_t next_summary_ns;
    // When a summary last went out. A summary at the short-lease cadence is skipped when every known peer has
    // had a datagram from this node since the last tick (LIVELINESS_PLAN.md 10), but never so that two are
    // more than tt_CONTEXT_UPDATE_INTERVAL apart.
    uint64_t summary_sent_ns;
    // Who has had a datagram from this node since the last summary tick: a broadcast reaches everyone, an
    // addressed send its peers (a bit per node id). Every send and node_update() run under the state lock, so
    // plain accesses: an atomic read-modify-write here cost ~8 ns a send on the PC (core_cost_bench.c).
    uint8_t reached_everyone;
    // Whether the summaries run at the short-lease cadence, the only case with anything to skip: while false,
    // sends record nothing (note_reached()), so a node without short leases pays one branch a send.
    uint8_t summary_skip_armed;
    // tx_tail when tx_buffer holds a summary and nothing else, 0 otherwise: a flush of exactly that is not
    // traffic for the skip, or each summary would cancel the next and an idle node's cadence would halve.
    uint32_t tx_summary_alone_len;
    // Set when the tt_CONTEXT_UPDATE_INTERVAL summary falls due while the node's traffic reaches every peer: it
    // then goes just ahead of the next send instead of on its own, so a node that stops under traffic stops
    // on its data (LIVELINESS_PLAN.md 10). node_update() sends it on its own if no send came by the next tick.
    // Under the state lock, as every send is.
    uint8_t summary_rides;
    uint32_t reached_nodes[tt_MAX_CONTEXT_IDS / 32];

    // 8-aligned, as every receive buffer lending hands out is (the rx pool's, tt_Context_set_rx_pool()): a DATA alone
    // in its datagram - the single form, 4-byte header + 16-byte DataHeader - then has its payload at 4 mod 8, and what
    // follows a 4-byte prefix there (rmw_tickle's psn) at 0 mod 8, aligned for a struct with 64-bit members, so a
    // loaned take can read it in place (docs/RMW.md "Loaned messages"). It was 4-aligned and fell at 4 mod 8 in the
    // rmw build, which put every such message off its alignment: 8 of 300 socket-path samples were read in place.
    tt_ALIGNAS(8) uint8_t rx_buffer[tt_MAX_BUFFER_LENGTH * 2];
    uint32_t rx_tail;
    uint32_t rx_size;

    struct tt_TCB scheduler[tt_MAX_SCHEDULER_LENGTH];
    int32_t scheduler_tail;

    // tt_hal is defined indirectly via <tickle/hal.h>, which includes the
    // platform-specific HAL header (<tickle/hal_linux.h> or <tickle/hal_freertos.h>).
    struct tt_hal hal; // NOLINT(misc-include-cleaner)
#if tt_CONTEXT_ID_CLAIM
    // (g8, config.h's tt_CONTEXT_ID_CLAIM) Keeping this context's id its own on the link. id_explicit: set from
    // _tt_CONFIG.context_id, so it never moves. id_muted: a collision left no free id, so this context sends nothing.
    // created_ns starts its startup window, in which it yields on a collision. collision_since_ns / _last_ns bound
    // the current collision (0: none). ids_seen: every source heard on the link, which a move avoids.
    // collision_logged_*: the last foreign holder of an explicit id reported, so each is reported once.
    // id_muted_drops: what a muted context was asked to send and refused - publishes, and datagrams at the HAL.
    bool id_explicit;
    bool id_muted;
    uint64_t id_muted_drops;
    uint64_t created_ns;
    uint64_t collision_since_ns;
    uint64_t collision_last_ns;
    uint8_t ids_seen[tt_MAX_CONTEXT_IDS / 8];
    uint32_t collision_logged_ip;
    uint16_t collision_logged_port;
#endif
#if tt_DISCOVERY_OPTIONS
    uint64_t rx_out_of_range; // (g6) datagrams dropped because their sender is outside _tt_CONFIG.discovery_range
#endif
    // (g11, RMW_GAPS_PLAN.md) Datagrams dropped because nothing in them could be processed: a wire version this
    // build does not speak, a bad magic, a header or submessage that runs past the datagram. A peer must never be
    // able to end a local poll loop, so these are counted and dropped rather than returned as an error - one v10
    // datagram used to end a v11 node outright. version_mismatch_drops is the named subset of rx_malformed_drops
    // that a rolling upgrade produces, which is worth telling apart from an attack or a corrupt link.
    uint64_t rx_malformed_drops;
    uint64_t version_mismatch_drops;
    // The named subset that is not a version skew or a corrupt link but a record that could only have
    // been built by something with write access to a segment, arriving from the network instead
    // (tt_SUBMESSAGE_TYPE_SHM_DATA). Counted apart because an upgrade produces the one and nothing
    // legitimate produces the other.
    uint64_t rx_shm_only_on_socket;
    // Seq positions a whole record's span covered beyond its own, absorbed by the reader because
    // nothing will ever send them (absorb_seq_span(), rx_seq_span above). It exists because its
    // absence has no shape: a span that is carried but never applied, and a span that is not
    // carried at all, produce the identical picture from outside - every record delivered, every
    // loss counter zero, and no samples. If records are arriving over shared memory and this stays
    // at zero, the span is not reaching the reader, whatever the writer thinks it put in the slot.
    uint64_t rx_span_absorbed;
    // Samples the segment drain passed over unread because a KEEP_LAST Subscriber already had `keep_last_depth`
    // newer complete samples of the same writer waiting behind them in the ring (plan_segment_skips(), tickle.c).
    // One per sample, however many records it took. Each one was going to be delivered into a history that would
    // have overwritten it before anyone took it; the counter is what says the drain chose not to pay for that,
    // rather than the samples having been lost.
    uint64_t rx_shm_skipped_superseded;
    // The drain's plan for the records it is about to read: segment_plan_count records from segment_plan_base,
    // a bit set for each one that is superseded. Valid only inside one drain_own_segment() call.
    uint32_t segment_plan_base;
    uint32_t segment_plan_count;
    // Samples handed to a Subscriber with keep_last_depth != 0, every path; and its value when the drain began. A
    // plan whose records changed it is where the drain hands back to its caller (segment_skip_head(), tickle.c).
    uint64_t rx_keep_last_delivered;
    uint64_t segment_plan_mark;
    uint8_t segment_plan_skip[(tt_SEGMENT_SLOTS / 8) + 1]; // one bit per slot, rounded up
    // The plan's leading run: its first `count` records are all superseded whole samples of one writer's endpoint,
    // which the drain can pass over together - one permission check, one accounting, one release - before the
    // sample it hands over is read (segment_skip_run(), tickle.c). count == 0: no such run, or it is spent.
    struct tt_SegmentPlanRun {
        uint32_t entity_id;
        uint32_t endpoint_id;
        uint32_t count;
        uint8_t source;
    } segment_plan_run;
    // Fragmented samples whose FRAG_FIRST the drain skipped and whose continuations are still to be read: each
    // continuation that belongs to one is skipped with it, so a skipped sample never leaves a fragment to be
    // reassembled on its own. Survives between drains (a sample can straddle the drain's bound); count == 0 is free.
#define tt_SEGMENT_SKIPPING_SAMPLES 4
    struct tt_SegmentSkippedSample {
        uint32_t entity_id;
        uint32_t endpoint_id;
        uint32_t next_seq_no; // the seq_no the next continuation of this sample carries
        uint8_t source;
        uint8_t next_index;
        uint8_t count;
    } segment_skipping[tt_SEGMENT_SKIPPING_SAMPLES];
#if tt_LOCAL_DELIVERY
    // (g9, config.h's tt_LOCAL_DELIVERY) Where a published sample's bytes wait while this context's own Subscribers
    // take it: sending reuses tx_buffer. Owned here rather than on the publishing thread's stack, which a sample of
    // tt_MAX_SAMPLE_LENGTH would strain. local_delivery_depth > 0: a callback is publishing from inside a local
    // delivery, whose bytes still sit in local_scratch - such a nested sample takes a copy of its own on the stack.
    uint8_t local_scratch[tt_MAX_SAMPLE_LENGTH];
    uint8_t local_delivery_depth;
#endif
    // Threading (tt_THREAD_SAFE, config.h) - see "Threading" at tt_Context_lock(). One lock, guarding the
    // node, its entities and the scheduler heap. User callbacks run with it held and may call back into
    // core, so it is re-entrant - not through a recursive mutex but by recording its owner: re-entry by the
    // owning thread is then a compare, not an atomic, which matters on the per-sample publish path.
    tt_lock_t state_lock;    // NOLINT(misc-include-cleaner) - from the platform header hal.h selects, like hal
    uintptr_t state_owner;   // tt_thread_self() of the holder, 0 when free; accessed through __atomic builtins
    uintptr_t poller_thread; // tt_thread_self() of the thread inside tt_Context_poll(), 0 when none; __atomic
    // Non-zero while the poller is blocked waiting for state_lock; other threads then let it in first (state_lock(),
    // tickle.c). __atomic.
    uint32_t poller_waiting;
    // The time the running tt_Context_poll() last read (tt_get_ns()), 0 outside one: the receive path's "now" - a
    // received DATA's 32-bit timestamp is rebuilt against it (timestamp_from_wire()), a peer's last sign of
    // life is stamped with it (traffic_last_seen) - so receiving a datagram reads no clock of its own. Read
    // again when a wait returns with data and every tt_RX_CLOCK_REFRESH datagrams of a drain
    // (OPTIMIZATION_PLAN.md 11, D1). Raw nanoseconds, beside poller_thread: the poll writes that line anyway, and a
    // division per poll cost a sender polling once a sample ~3 ns (WIRE_PLAN.md 8). Only the poller touches it.
    uint64_t rx_clock_ns;
    uint32_t state_depth; // how many times the owner has taken it; only the owner reads or writes it
    struct tt_LockStats state_lock_stats;
    // The scheduler inbox: tt_Context_schedule() from a thread that does not hold the state lock puts its entry
    // here, without a lock, and the next look at the heap moves it in - see sched_inbox_push() (tickle.c).
    // Each slot is claimed by compare-and-swap, as struct tt_Server.slot_state is.
    struct tt_TCB sched_inbox[tt_SCHED_INBOX_LENGTH];
    uint8_t sched_inbox_state[tt_SCHED_INBOX_LENGTH]; // tt_SCHED_SLOT_*, through __atomic builtins
    uint32_t sched_inbox_pending;                     // READY slots, so an empty inbox costs one load
    // Set while a tt_Context_poll() is running, so a second concurrent one fails with tt_RET_BUSY instead
    // of sharing rx_buffer with the first. Accessed only through __atomic builtins.
    uint8_t poller_active;
    // What a poll blocked in tt_receive() is waiting until (UINT64_MAX: indefinitely), or 0 when no poll
    // is waiting, so a scheduler insert earlier than that wakes it. Two 32-bit halves under a sequence
    // counter, because a 32-bit target has no 64-bit atomics - see wait_until_store() and poll_wait_io()
    // (tickle.c) for the ordering that makes the wake race-free.
    uint32_t wait_seq;
    uint32_t wait_until_hi;
    uint32_t wait_until_lo;
    // This context's own timer lateness (2026-10-08, DESIGN.md 6): how much later than the deadline it slept to a
    // wait in tt_Context_poll() for a retry timer actually returned, smoothed as RFC 6298 smooths a round trip (mean
    // and mean deviation, timer_lateness_fold() in tickle.c). Only the poller writes the two estimates.
    // timer_lateness_ns is what both retry timers read as G, the term RFC 6298 calls the clock granularity: mean + 4 x
    // deviation, at least timer_resolution_ns, and 0 until the first sample, which reads as the cold-start
    // tt_TIMER_LATENESS_INITIAL. It is read from any thread through __atomic builtins, and 32 bits wide so that a
    // 32-bit target needs no 64-bit atomic.
    uint32_t timer_lateness_mean_ns;
    uint32_t timer_lateness_var_ns;
    uint32_t timer_lateness_ns;
    uint32_t timer_resolution_ns; // tt_timer_resolution_ns() at tt_Context_create(): G's floor

    // Opt-in graph introspection (tt_Context_set_discovery(), rmw_tickle/PLAN.md's Milestone 0(c)) -
    // NULL (the default - see reset_node_state()) unless a caller attaches its own, externally-
    // owned struct tt_Discovery. Deliberately *not* an embedded struct tt_Discovery the way
    // update_seen[]/peers[] etc. are: that table can hold real name/type strings for
    // tt_MAX_DISCOVERED_ENTITIES entities, easily several KB, and every tt_Context pays for its own
    // fields whether or not anything ever uses them - a FreeRTOS target with no rmw layer (today,
    // or a future micro-ROS-style thin client whose *agent* - not the constrained device itself -
    // would be the one wanting this) shouldn't carry that weight. Unset, this costs 3 pointers.
    struct tt_Discovery* discovery;
    tt_DISCOVERY_CALLBACK discovery_callback;
    void* discovery_callback_param;

    // Diagnostic counters, not protocol state, printed once by tt_Context_destroy(). They exist to
    // split one specific question that nothing else can answer from outside: when a Subscriber
    // delivers nothing, did its socket receive the datagrams at all? If tx_datagrams on the
    // sending node is large while rx_datagrams on the receiving one is ~0, the datagrams never
    // arrived and the problem is below TickLE. If they arrived and nothing was delivered, it is
    // above the socket. rx_self_sent separates a third case that same-host deployments can hit:
    // every node binds the same well-known port, so a unicast addressed to a host with two nodes
    // on it can be handed by the kernel to the sender's own socket, which shows up here as a
    // sender receiving its own traffic back.
    uint64_t tx_datagrams;
    // The same totals split by the transport that carried them (SHM_PLAN.md stage 0), indexed by
    // enum tt_Transport. They are maintained in the send/receive seam rather than at the call
    // sites, which is the whole point: tx_datagrams itself was incremented at three of the twelve
    // send sites' worth of paths and missed publish_zerocopy() entirely (g15), so a counter that
    // has to be remembered at each site is a counter that will be forgotten at one. Counting where
    // the datagram actually reaches the transport makes coverage a property of the code's shape.
    //
    // What they are for: a test cannot ask "which transport was selected" and learn anything - a
    // partially wired seam would answer correctly and still send most datagrams the old way. It has
    // to ask how many went each way, per shape of send, which is what these answer.
    uint64_t tx_datagrams_by_transport[tt_TRANSPORT_COUNT];
    uint64_t rx_datagrams_by_transport[tt_TRANSPORT_COUNT];
    // Why segment attaches failed, indexed by enum tt_SegmentAttach (SHM_PLAN.md stage 1). See that
    // enum for why the reason is kept rather than a yes/no: every failure here falls back to UDP
    // and works, so a module that never attaches anywhere is invisible without it.
    uint32_t segment_attach[tt_SEGMENT_ATTACH_COUNT];
    // Datagrams that went over UDP although the segment was working, because they were larger than a
    // slot - a service request or response, which do not fragment (config.h's tt_SEGMENT_BYTES).
    // Kept apart from segment_attach[] deliberately: "this datagram was not for the segment" and
    // "the segment could not be attached" are different events, and one number for both would cost
    // exactly the distinction the attach reasons were added for. A row carrying this and an empty
    // attach table says the module is working and this shape is out of its scope.
    // Datagrams that went over UDP because they were broadcast - no single peer, so no name to
    // compute and no segment even in principle. By design rather than by failure, and the largest
    // of the four in any running context, because announces and summaries are broadcast.
    //
    // It exists because without it these were counted as unattached: a running module would have
    // reported a large discovery-failure count for traffic that was never a candidate. The
    // assertion would still have summed correctly and the diagnosis would have been wrong, which
    // is worse than a failure.
    uint64_t segment_broadcast_to_udp;
    uint64_t segment_oversized_to_udp;
    // Datagrams that went over UDP because no segment could be attached for that peer. The pair to
    // the above: same outcome on the wire, different cause, and only the pair distinguishes a
    // module doing its job from one that never attaches anywhere.
    uint64_t segment_unattached_to_udp;
    // Datagrams DROPPED because the peer's ring was full - not rerouted, dropped, the way a full
    // socket buffer drops. This counter said "to_udp" until 2026-09-29 and the behaviour matched the
    // name, and that was the defect: a datagram rerouted to UDP overtakes the records still sitting
    // in the ring ahead of it, so the reader delivers the newer one first and discards every older
    // one behind it. Measured on CI's same-host cell: 36,125,590 datagrams arrived over the segment
    // and 36,123,071 were discarded as out of order, throughput 1,018 -> 147 Mbps. One logical
    // stream cannot be carried over two paths of different latency, so a full ring now costs the
    // datagram rather than its ordering, and the reliable path's own retransmission covers it.
    //
    // A run where this moves still says what it always said - tt_SEGMENT_BYTES is too small for the
    // offered load - and nothing else can say it.
    uint64_t segment_full_dropped;

    // Warnings actually emitted, not events seen. A warning that repeats per datagram buries the
    // one line worth reading, so each of these is latched - and it is a count rather than a bool so
    // that "warned once" is a number a test can assert, and a latch that fires twice is a failure a
    // bool could not report.
    uint32_t segment_full_warnings;
    // Drain passes that found the head of the ring claimed by a writer that never published it.
    // `write_index` moves on the claim and the slot's sequence only on the publish, so a claimed
    // head is indistinguishable from an empty one by the slot alone - the question is whether
    // anything is outstanding at all. Transient under load, since a writer mid-copy looks the same;
    // what matters is persistence, which is what segment_head_stall_passes measures. A writer that
    // dies between its claim and its publish stops the segment for good, and without this the only
    // symptom is every peer's datagrams being dropped with segment_full_dropped - a sizing signal - as
    // the sole evidence, which points at the wrong cause.
    uint64_t segment_head_stalls;
    uint32_t segment_head_stall_passes; // consecutive; reset the moment a record is read
    uint32_t segment_stall_warnings;
    // Doorbells sent to a sleeping owner's data port, and received. Counted because "never needed"
    // and "never rung" look identical from a latency figure, and only one of them is good news.
    // Consecutive times the liveliness check declined to judge because datagrams were still unread -
    // see tt_LIVELINESS_DEFER_NS. Read to decide whether to defer again, so it must be initialised;
    // reset the moment a run actually judges.
    uint16_t liveliness_deferrals;
    uint64_t liveliness_deferrals_total; // how often that happened at all, for a reader of the log
    uint64_t segment_doorbells_sent;
    uint64_t segment_bells_rung;
    uint32_t segment_sleep_generation; // the last generation this context wrote into its own reader_waiting
    uint32_t segment_slot_ceiling; // the largest slot_bytes of any peer segment attached (record_size_limit()) // of
                                   // segment_doorbells_sent, the ones rung through the FIFO rather than UDP
    uint64_t segment_doorbells_received;
    // A sleep announced and then called off by the drain that follows the announcement (poll_wait_io()) keeps its
    // generation for the next announcement, unless a doorbell arrived in between (segment_reader_waiting()): set when
    // one is called off, with segment_doorbells_received at that moment. Polling thread only.
    uint8_t segment_generation_unspent;
    uint64_t segment_unspent_doorbells;
    uint64_t segment_generations_kept; // announcements that kept the last generation instead of starting one
    uint64_t segment_sleeps;           // waits entered after an announcement (the rest were called off)
    // The mean of the recent sleeps taken on a record already claimed and ended by its doorbell, decision to resume:
    // what a sleep costs this reader, with no idle time in it (segment_resumed()), and how long it may wait for a
    // record instead (segment_await_claim(), segment_await_next()). 0 until the first. Polling thread only.
    uint64_t segment_sleep_cost_ns;
    uint32_t segment_sleep_cost_n;  // how many sleeps that mean is over (halved at tt_SEGMENT_PROBE_EVERY_MAX)
    uint8_t segment_sleep_on_claim; // the sleep being entered is one of those: set by poll_wait_io(), read on resume
    // How many times this context's own ring was waited on without announcing a sleep, and how many of those waits
    // ended with the record published - the rest slept as before.
    uint64_t segment_claim_waits;
    uint64_t segment_claim_waits_published;
    // The current awake stretch, for the writers' pace while this reader was awake (segment_await_next()): when it
    // began - a resume, or the last time the ring was found empty - and the ring's write_index then.
    uint64_t segment_stretch_ns;
    uint64_t segment_gap_ns; // the writers' last measured gap between records while awake (segment_await_next())
    uint32_t segment_stretch_index;
    // How many times an empty ring was watched for the next claim instead of slept on, and how many a claim ended.
    uint64_t segment_watches;
    uint64_t segment_watch_hits;
    // Whether the two waits above pay for themselves (segment_epoch_turn()): what the pair spent per record - the
    // writers' time per record, read from their pace, plus this thread's CPU time per record - in epochs of one ring's
    // worth of records with the waits on, and with them off. The mode measured cheaper is used, and the other is
    // measured again after segment_probe_every epochs. Polling thread only.
    uint8_t segment_watching;       // the mode of the current epoch: 1 waits (claim and watch), 0 sleeps as before
    uint8_t segment_preferred;      // the mode measured cheaper; the current epoch differs from it only when probing
    uint8_t segment_epoch_settling; // the current epoch is the first in its mode, and is not measured
    uint32_t segment_epoch_index;
    uint64_t segment_epoch_ns;      // 0: no epoch begun - the first decision on the polling thread begins one
    uint64_t segment_epoch_cpu_ns;  // the polling thread's CPU clock at the epoch's start
    uintptr_t segment_epoch_thread; // and that thread (poller_thread): another's CPU clock measures nothing
    // Per mode (index: segment_watching), over its recent epochs: how many, their mean cost per record in ns, and the
    // sum of squared deviations from it (Welford), for the standard error the choice is tested against.
    uint32_t segment_cost_n[2];
    uint64_t segment_cost_mean_ns[2];
    uint64_t segment_cost_m2[2];
    uint32_t segment_probe_every; // epochs in the preferred mode between two of the other, doubling to a bound
    uint32_t segment_probe_in;    // epochs left until the next probe
    uint64_t segment_epochs[2];   // epochs measured, by mode
    // Samples encoded straight into a peer's slot rather than into tx_buffer and then copied (SHM_PLAN 6e(b),
    // tt_SEGMENT_ENCODE_IN_SLOT). Each is also one of tx_shm. The arm's own report of its treatment: an A/B whose
    // treated arm shows 0 here measured the two-copy path twice.
    uint64_t segment_encoded_in_slot;
    // tt_Publisher_claim(): claims published where the application built them, claims whose sample had to be copied out
    // of the slot at publish (its destination had changed), and claims abandoned (published empty).
    uint64_t segment_claims_published;
    uint64_t segment_claims_copied;
    uint64_t segment_claims_abandoned;
    // drain_rx() passes ended because the own ring held a record after tt_RX_CLOCK_REFRESH datagrams of one drain (the
    // ring-turn rule: a socket that never empties cannot starve the ring). A drain that empties the socket sooner never
    // asks, so 0 is the common case; a build without the rule prints no such field.
    uint64_t rx_drain_ring_turns;
    // How many times this context built its own segment and gave it up again, and how many peers it
    // currently believes share its host. Out here with the other counters rather than behind
    // tt_SEGMENT_ENABLED because the traffic line that prints them is compiled either way; they stay
    // zero when the module is off, which is the truth.
    //
    // Deferred creation is otherwise invisible from outside: "no segment yet because no peer could
    // use one" and "a segment was wanted and could not be built" leave the same absence behind, and
    // only a counter that rose tells them apart. same_host_peer_count is uint16_t, not uint8_t:
    // tt_MAX_CONTEXT_IDS is 256, so a byte would wrap to zero with every peer still present.
    uint64_t segments_created;
    uint64_t segments_released;
    uint16_t same_host_peer_count;

#if tt_SEGMENT_ENABLED
    // What this context has mapped, indexed by the remote context id - the same index
    // traffic_last_seen[] uses, because within one context's view a remote context id is which peer
    // this is. The address and port are kept beside the mapping because the name was computed from
    // them: a peer that reappears at a different address is a different segment, not this one.
    //
    // `own_segment` is this context's own, the one peers attach to. It is created lazily by
    // ensure_own_segment(), when a same-host peer first appears, after the address and context id it is named
    // from are known.
    struct tt_SegmentPeer {
        struct tt_SegmentHeader* mapping;
        // How many bytes `mapping` covers, which is the OWNER's geometry and not ours. Every detach has to
        // unmap exactly what was mapped, and until the attach became two-step every site recomputed the length
        // from our own tt_SEGMENT_SLOTS/tt_SEGMENT_SLOT_BYTES - correct only while every context agreed.
        size_t mapped_bytes;
        uint32_t ip;
        uint16_t port;
        uint32_t incarnation; // what was in the header when we attached; a change means a new peer
        // Slots of this mapping claimed by a publisher of ours and not yet published or abandoned
        // (tt_Publisher_claim()): an application is writing into them, so nothing unmaps this region - not the
        // revalidation, not the dead-reader rule - until it is 0 again.
        uint8_t claims;
        // The negative answer, remembered. A peer on another host has no segment and never will,
        // and without this the question is asked again for every datagram - an open() that walks
        // /dev/shm and fails, about 87,000 times a second per sender, which halved cross-host
        // throughput and doubled CPU per sample when it shipped. `missing` says this (ip, port) was
        // asked about and had nothing for us.
        bool missing;
        // Sends before this entry is asked about again, whichever way it was answered. A "no" must
        // expire so a peer that binds later becomes reachable. A "yes" must expire too, and that is
        // less obvious: an owner that was killed leaves its region mapped, intact, with the same
        // incarnation in it, so nothing *inside* the mapping can ever say it is orphaned - only
        // asking the name again can, and a peer that never asks writes into a ring no one drains.
        uint32_t recheck_in;
        // When we last managed to put anything in this peer's ring, or 0 if we have just managed it.
        // A reader that has taken nothing for tt_SEGMENT_DEAD_READER_NS, while we have had records
        // for it the whole time, has stopped; one that is merely behind still frees a slot now and
        // then, and any success sets this back to 0.
        //
        // **Time, and nothing else.** Two earlier versions of this rule each added a condition that
        // could not decide anything, and each was found only by its mutant surviving:
        //
        //   - "N consecutive refusals" measured the writer, not the reader. At same-host rates a
        //     healthy reader goes thousands of our sends between two of its own poll passes, so the
        //     rule fired constantly on a live peer - 2,446,848 "unattached" datagrams in one run -
        //     and the re-attach that followed put one logical stream on two paths, which is the
        //     defect drop-on-full exists to prevent.
        //   - "...and read_index has not moved" was added to fix that and was unreachable: any
        //     movement by the reader frees a slot, so the next write succeeds and resets the count
        //     anyway. The extra condition could never be what decided. Adding a second, timed
        //     version of the same idea repeated the mistake exactly.
        //
        // The success path is the whole discriminator, and it always was.
        uint64_t last_progress_ns;
        // What this peer's read_index was when we last rang its doorbell. A reader killed while
        // blocked leaves reader_waiting set in its own header and nobody clears it, so without this
        // every writer rings - a real sendto() - for every datagram until it gives that peer up.
        // Measured on a SIGKILL run: 2,853,609 doorbells into a socket nobody was reading.
        //
        // Once per SLEEP, not once per reader advance (2026-10-05). A reader that sets reader_waiting writes the
        // generation of that sleep there, and a writer rings each generation once: a dead reader leaves its last
        // generation behind and is rung once, not per datagram, which is what the per-advance rule was for. That rule
        // assumed a reader that had taken nothing since our ring was dead or about to wake for it. On the rig a live
        // reader went back to sleep without taking anything, with up to 512 records unread, and the writer - seeing
        // no advance - never rang again: p4 RELIABLE stalled 400-500 ms at a time until the reader woke for its own
        // timer (diagnosed with the ring's indices printed at each stall; ringing on every write ended the stalls and
        // cost the reader 50% more CPU). A new sleep is a new generation, so it is rung again.
        uint32_t doorbell_generation;
        // The peer's FIFO doorbell, opened when its segment is attached (tt_segment_bell_open()), stored PLUS ONE so
        // that the memset(0) every teardown path ends with means "none" rather than standard input. 0: ring over UDP.
        int32_t bell_fd_plus1;
    } segment_peers[tt_MAX_CONTEXT_IDS];
    // Which segment_peers[] entries have been set up (nonzero), by context id. An entry is zeroed the first time it is
    // wanted (segment_peer(), tickle.c), not at reset, and an entry whose flag is 0 is never read: it means "all zero".
    // Pay as you go (stage 1 / S1, 2026-10-09): the table is 14 KB, and zeroing it at create wrote every page of it in
    // every context, including one whose peers are all on other hosts and never attaches anything. Now a context
    // writes only the entries of the peers it sends to, so in memory nothing had touched - rmw_tickle zero-allocates
    // its context - the rest of the table never becomes resident (tests/test_transport_seam.c checks it with
    // mincore()). One byte per id rather than a bitmap: two threads setting up two different ids then write two
    // different bytes, never one shared word.
    uint8_t segment_peer_live[tt_MAX_CONTEXT_IDS];
    struct tt_SegmentHeader* own_segment;
    // One bit per tt_WholeRefusal cause, so the reason a whole-record send was refused is stated once per
    // cause rather than per sample. Added 2026-10-03 after four rig campaigns inferred it from throughput,
    // which cannot tell "no effect" from "never ran" - then widened from a single bool the same day, because
    // one flag for seven causes reports whichever is hit first and hides the rest for the life of the node.
    uint16_t whole_refusals_logged;
    // Which peers share this host, indexed by remote context id. Only a peer at our own address can
    // ever open the file we create - the name is built from (ip, port, context id) - so the segment
    // needs to exist exactly while at least one such peer does. Discovery reports both edges of that:
    // an announce says one appeared, and forget_peers_from_source() that one has gone. That symmetry
    // is the whole reason the segment can be built on demand rather than at bind.
    //
    // The count is kept rather than derived because the departure path asks only "is there still
    // one", and deriving it would put a 256-entry scan on every departure. uint16_t, not uint8_t:
    // tt_MAX_CONTEXT_IDS is 256, so a byte would wrap to zero with every peer still present and
    // release the segment out from under all of them.
    bool same_host_peer[tt_MAX_CONTEXT_IDS];
#endif
    uint64_t summaries_skipped; // short-lease summaries not sent: the node's traffic had reached every peer
    uint64_t summaries_ridden;  // once-a-second summaries sent just ahead of a data send
    uint64_t rx_datagrams;
    uint64_t rx_self_sent;
    // Submessages refused because no datagram could ever carry them (larger than
    // tt_MAX_BUFFER_LENGTH once framed - see end_encode()), plus any whole buffer flush_tx() had to
    // drop for the same reason. Nonzero means something was not sent: most likely this node's own
    // discovery UPDATE, whose endpoint list outgrew one datagram, so peers cannot see it.
    uint64_t tx_dropped_oversize;
    // Set by the HAL on each receive: true when the datagram came in on this node's own data
    // port, false when it came in on the shared well-known port (where broadcasts land). Lives
    // here rather than in a tt_receive() out-parameter so the HAL contract in hal.h stays as it
    // is; both backends set it, and nothing outside the self-sent accounting reads it.
    bool rx_via_data_port;
    // Set by the HAL on each socket receive, like rx_via_data_port: where in the buffer core passed the datagram
    // begins. 0 except on a HAL that reads several datagrams into that buffer at once and hands each out where it lies
    // (Linux UDP_GRO, hal_linux.c "UDP offload"); then a multiple of 4, and of 8 unless counted in udp_gro_off8.
    uint32_t rx_offset;
    // UDP offload (hal_linux.c "UDP offload"), set and counted by the HAL, on the traffic line. udp_offload: bit 0
    // receive offload (UDP_GRO) in use, bit 1 send offload (UDP_SEGMENT). gro_reads: reads that returned several
    // datagrams at once; gro_merged: the datagrams those carried; gro_copied: of those, the ones not handed out in
    // place (a segment off its 4-alignment, or the landing buffer moved by a retain); gro_off8: handed out in place at
    // 4 mod 8, where a lone DATA's payload is not where a loaned take reads it in place. 0 on a HAL without them.
    uint8_t udp_offload;
    uint64_t udp_gro_reads;
    uint64_t udp_gro_merged;
    uint64_t udp_gro_copied;
    uint64_t udp_gro_off8;
    // Whether the submessage being processed right now was addressed to this node by id, rather than to
    // everyone (tt_SUBMESSAGE_ID_ALL). A retransmission is addressed to the node that asked for it
    // (retransmit_one_sample()), so this is how a reliable Subscriber tells the sample its ACKNACK
    // brought back from the original that was merely late - see note_recovery_sample(). Set per
    // submessage on the poll thread, under the state lock.
    bool rx_targeted;
    // Of rx_self_sent, the ones carrying a DATA submessage rather than only an announce.
    uint64_t rx_self_sent_data;
    // Of those, the ones that arrived as unicast - addressed to this node's own data port rather
    // than to the broadcast address. This is the counter with no legitimate non-zero case. A data
    // sample published before any peer is known goes out as a broadcast and comes back to its own
    // sender, which is correct and does happen (observed on the very first sample of a run), so
    // rx_self_sent_data alone still needs a margin. A node's own *unicast* data is addressed to
    // somebody else by construction, so receiving it back means the kernel handed a sender its own
    // stream - the same-host failure 82a6a02d fixed - and nothing else.
    uint64_t rx_self_sent_data_unicast;
    // Of rx_datagrams, how many arrived on each of this node's two sockets. Not a refinement of
    // the counters above - a precondition for reading them, and for reading the delivery-order
    // counters on tt_Subscriber.
    //
    // Why (2026-09-24): the question those exist to answer is whether interleaving two sockets
    // reorders delivery. A run in which one of these two is zero did not interleave anything, so
    // it does not test that at all - and it reads exactly like a run that interleaved and stayed
    // in order. Two arms of such runs would compare zero against zero and look like a fix. The
    // same shape cost a packet capture on the same day: it was taken on an interface the broadcast
    // half of the stream never touched, and contained zero broadcast datagrams, so "the wire was
    // ordered" would have been concluded from a recording of half the wire.
    //
    // So these are the run's own statement about whether the experiment was live. A run with
    // either at zero is void rather than negative, and must be said to be void rather than
    // reported as evidence of anything.
    uint64_t rx_via_data_datagrams;
    uint64_t rx_via_well_known_datagrams;

#if tt_FRAG_ENABLED
    // DATA_FRAG reassembly - struct tt_FragSlot above.
    struct tt_FragSlot frag_slots[tt_FRAG_REASSEMBLY_SLOTS];
    uint32_t frag_clock; // counts claims, so the oldest slot can be found without a clock read
    // Samples put back together and handed on; reassemblies given up to make room for a newer sample (a
    // fragment lost, or a sender outrunning tt_FRAG_REASSEMBLY_SLOTS); and fragments refused as
    // malformed or inconsistent with the rest of their sample. The last two are losses, counted so that
    // they cannot pass for network loss.
    uint64_t frag_reassembled;
    uint64_t frag_abandoned;
    uint64_t frag_dropped;
    // Where a RELIABLE Subscriber's sample is put together from the fragments held in its reorder buffer,
    // at the moment the last one is in order - filled and delivered in one step, so one per node serves
    // every Subscriber. 8-aligned, with the payload placed at +4, as rx_buffer places a DATA's CDR.
    tt_ALIGNAS(8) uint8_t frag_scratch[tt_MAX_SAMPLE_LENGTH + 8];
    // The in-order fast path (2026-10-06): a RELIABLE Subscriber's fragmented sample arriving in order is
    // put together in frag_scratch as its fragments arrive, never touching the reorder buffer - which held
    // every fragment first, and at the bench's 11.6 MB of untouched slots cost first-touch page faults on
    // every sample of the first lap. frag_fast_sub is the one Subscriber whose sample frag_scratch holds,
    // NULL when idle. Anything that would disturb it - another use of frag_scratch, a store into that
    // Subscriber's reorder buffer, or the watermark moving under it - first moves the fragments already
    // placed into their own reorder slots (frag_fast_spill(), tickle.c), which are free by construction.
    struct tt_Subscriber* frag_fast_sub;
    uint64_t frag_fast_timestamp;
    uint32_t frag_fast_entity_id;
    uint32_t frag_fast_seq_no;       // the sample's first datagram
    uint32_t frag_fast_length;       // payload bytes placed so far
    uint16_t frag_fast_first_length; // payload bytes of fragment 0
    uint16_t frag_fast_cont_length;  // payload bytes of every non-last continuation, 0 until fragment 1
    uint8_t frag_fast_context_id;
    uint8_t frag_fast_count;
    uint8_t frag_fast_placed; // fragments 0..placed-1 are in frag_scratch
    bool frag_fast_is_native;
    bool frag_fast_via_data_port;
    // Fragments of a sample already reassembled (struct tt_FragSlot.done) - the rest of a retransmission
    // that another fragment already completed. Not a loss; counted so it is not mistaken for one.
    uint64_t frag_duplicate;
#endif
    // Stage 2 (CONTEXT_NODE_PLAN.md), kept last - cold data, and every hot field keeps its offset: the nodes hosted
    // here, by index; NULL where none. [0] is default_node once it is in use. default_node_name holds its
    // "tickle_<context id>", unique per context: a shared "/tickle" would appear once per process in `ros2 node list`.
    struct tt_Node* nodes[tt_MAX_NODES];
    struct tt_Node default_node;
    char default_node_name[16];
#if tt_SAMPLE_LENDING
    // Receive-buffer lending (DESIGN.md section 10), last so that no field before it moves when it is compiled in.
    struct tt_Lending lend;
#endif
    // Large-sample fragments (types 11/12) a node built without stage 2 passed over (DESIGN.md section 8).
    uint64_t frag_large_skipped;
#if tt_LARGE_SAMPLES
    // Large-message stage 2 (DESIGN.md section 8), after everything else for the same reason as lend.
    struct tt_LargeState large;
#endif
};

// A destination this node has learned it can reach directly (see decode_update_entities()'s
// peer-matching, upsert_peer() in tickle.c). context_id doubles as the "slot occupied" flag -
// tt_CONTEXT_ID_INVALID (0) means empty, the same sentinel struct tt_Context's own id already uses
// (valid node ids are 1..254).
struct tt_Peer {
    uint8_t context_id;
    uint32_t ip;   // host byte order, matching tt_receive()'s own sender_ip out-param
    uint16_t port; // host byte order, matching tt_receive()'s own sender_port out-param
};

// One remote entity (a Publisher/Subscriber/Client/Server hosted by some *other* node) this
// node's discovery has recorded - see struct tt_Discovery. context_id doubles as the "slot
// occupied" flag, the same convention struct tt_Peer above uses.
struct tt_DiscoveredEntity {
    uint8_t context_id;
    uint32_t endpoint_id;
    // This entity's own tt_Endpoint.entity_id, as its announce carried it (tt_UpdateEntity.entity_id,
    // Phase 2). Beside endpoint_id rather than instead of it, because they answer different questions:
    // endpoint_id is hash(topic/service name + endpoint name), which two endpoints of one topic in one
    // process SHARE by construction, while entity_id identifies the instance.
    //
    // Recorded because a remote endpoint's identity has to outlive the announce that carried it:
    // rmw_graph.c encodes a gid from this table, and encoding endpoint_id there gave two remote
    // publishers of one topic the same gid - the collision Milestone 47 removed from
    // rmw_get_gid_for_publisher(). The local half of that was fixed first because it was what the
    // acceptance case could measure; this is the half that needed the value kept here.
    uint32_t entity_id;
    uint8_t kind; // tt_KIND_TOPIC_PUBLISHER / _SUBSCRIBER / SERVICE_CLIENT / _SERVER, or tt_KIND_NODE (stage 3)
    // Stage 3: the index, in its context, of the node the entity belongs to - for a tt_KIND_NODE entry, its own. A
    // remote endpoint's node is the tt_KIND_NODE entry with the same context_id and node_index. For a node entry,
    // `type` holds its namespace and `name` its name.
    uint8_t node_index;
    char type[tt_MAX_NAME_LENGTH + 1];
    char name[tt_MAX_NAME_LENGTH + 1];

    // true (set whenever this slot is written, tickle.c's own upsert_discovered_entity()): known
    // and currently believed alive. false: a tombstone - this entity's own source node missed
    // check_liveliness()'s own timeout (tickle.c) and is presumed dead, but is *remembered* here
    // rather than the slot being freed outright, unlike an explicit farewell/dropped-from-a-fresh-
    // announce departure (forget_discovered_entities_from_source(), tickle.c - a real removal, not
    // a tombstone, since that's a normal, intentional departure, not a liveliness failure) - QoS
    // roadmap #3 (LIVELINESS)'s own RMW_EVENT_LIVELINESS_CHANGED.not_alive_count (rmw_tickle/
    // PLAN.md) needs exactly this distinction to report a real live snapshot instead of always 0.
    // tt_Discovery_count() only counts alive entities (matching its own "topic list"-style
    // introspection use); tt_Discovery_find() returns a tombstoned entity too, unlike NULL for one
    // never seen at all - callers that care check .alive themselves. Reasserted (a later UPDATE
    // naming the same context_id/entity_id) flips this back to true, same slot, no separate "was a
    // tombstone" signal - the discovery callback's own existing "appeared, refreshed, or departed"
    // framing (tickle.h's own tt_DISCOVERY_CALLBACK doc comment) already covers a reassert as a
    // refresh, nothing new for a caller to handle. A slot search that finds no truly-empty slot
    // (context_id == tt_CONTEXT_ID_INVALID) falls back to reclaiming the first tombstoned one rather than
    // dropping a genuinely new entity on the floor - tombstones are remembered on a best-effort
    // basis, not guaranteed to survive table pressure.
    bool alive;

    // QoS roadmap #1 (RxO matching, Milestone 31) - a copy of this entity's own announced
    // struct tt_UpdateEntity.qos (tt_UPDATE_QOS_RELIABLE/_DURABLE/_LIVELINESS_MANUAL, the last one
    // added by Milestone 49 below), refreshed on every UPDATE (upsert_discovered_entity(),
    // tickle.c) the same way type/name are. Lets process_data()'s own subscriber_incompatible_
    // with_publisher() look up what a remote Publisher offers via this same discovery table,
    // rather than a second, separate cache - real DDS's own equivalent ("Publications" built-in
    // topic) is exactly this kind of discovery-table row too.
    uint8_t qos;

    // QoS roadmap #2 (DEADLINE) / #3 (LIVELINESS) RxO, Milestone 49 - a copy of this entity's own
    // announced struct tt_UpdateEntity.deadline_duration_ns/.liveliness_lease_duration_ns, same
    // "refreshed on every UPDATE" reasoning as qos above. 0 means "no requirement/infinite" on
    // either side for both, the same convention every other 0-disabled duration field in this
    // codebase already uses (tt_Publisher.lifespan_duration_ns etc.) - see deadline_liveliness_
    // incompatible()'s own doc comment (tickle.c) for the actual comparison these back.
    uint64_t deadline_duration_ns;
    uint64_t liveliness_lease_duration_ns;

    // The last sign of life of a MANUAL_BY_TOPIC Publisher (qos has tt_UPDATE_QOS_LIVELINESS_MANUAL): its own
    // DATA, or a HEARTBEAT with tt_HEARTBEAT_FLAG_LIVELINESS, found by (context_id, endpoint_id) - so two
    // writers of one endpoint on one node share it. Set when the entity is (re)announced. Unused for any
    // other entity, whose lease runs from the last datagram of its node (tt_Context.traffic_last_seen).
    uint64_t last_asserted_ns;
};

// Fixed-capacity table a caller opts a struct tt_Context into via tt_Context_set_discovery() - every remote entity any
// attached node has announced (not just ones matching a local endpoint the way struct tt_Peer's unicast-address
// tracking is scoped to). Owned by the caller (e.g. embedded in an rmw wrapper's own node struct), not by TickLE - see
// struct tt_Context's own "discovery" field comment on why.
//
// Not only introspection (CONTEXT_NODE_PLAN.md 4a, 2026-09-27). What reads it, and so goes unchecked for a remote
// entity the table has no room for: a subscriber's RxO check on a publisher's DATA (it fails open: the DATA is
// delivered), a reliable subscriber's KEEP_ALL classification of a writer (UNKNOWN), per-entity liveliness leases,
// rmw_tickle's BEST_AVAILABLE resolution, and every graph query. What does not: delivery between compatible endpoints,
// and unicast peer selection. Size it to the remote entities a context will see - tt_MAX_DISCOVERED_ENTITIES.
struct tt_Discovery {
    struct tt_DiscoveredEntity entities[tt_MAX_DISCOVERED_ENTITIES];
    uint32_t entities_dropped; // new remote entities there was no room for, counted since the table was attached
    bool full_warned;          // the one warning a full table draws has been logged
#if tt_DISCOVERY_INDEXED
    // (context id, endpoint id) -> slot + 1 (0: empty), open addressing (CONTEXT_NODE_PLAN.md 4b), in builds with a
    // large table (config.h): every lookup - an announced entity's, and each received DATA's RxO check - goes through
    // it, not a scan of the table. Kept by core; a caller that changes an entry's context_id or endpoint_id directly
    // must call tt_Discovery_reindex(). A zeroed struct is an empty table with a valid index.
    uint16_t index[tt_DISCOVERY_INDEX_SIZE];
    uint16_t free_cursor; // where the search for an empty slot starts
#endif
};

struct tt_Service;
struct tt_Client;
struct tt_Response;
struct tt_SubmessageHeader;

// Passed as `return_code` to a client callback when the RPC got no answer at all - every retry
// went unanswered (tt_CALL_RETRY_COUNT) - as opposed to the server replying with its own code.
// `response` is NULL in that case. A tt_SERVER_CALLBACK must not return this value itself.
#define tt_CALL_TIMEOUT ((int8_t)-128)

typedef void (*tt_CLIENT_CALLBACK)(struct tt_Client* client, int8_t return_code, struct tt_Response* response);

struct tt_Client { // extends endpoint
    struct tt_Endpoint endpoint;
    struct tt_Context* node;
    struct tt_Service* service;
    tt_CLIENT_CALLBACK callback;

    // Cache: fixed-size backing storage for the one outstanding call (tt_Client_call refuses a
    // second call while one is already pending), so a call/retry cycle never has to malloc/free.
    tt_ALIGNAS(8) uint8_t cache_buf[tt_CLIENT_CACHE_LENGTH];
    // Where the outstanding request is kept, and its size, when tt_Client_set_storage() attached the
    // caller's own; NULL (create's default, and a zeroed struct's) means cache_buf above.
    uint8_t* cache_storage;
    uint32_t cache_length;
    struct tt_SubmessageHeader* cache; // NULL when idle, else points into cache_buf
    uint64_t cache_time;               // When the call was sent: read before the send (client_call_locked())
    uint32_t latency;        // Call latency estimate (srtt), ns: an EMA of accepted answers, doubled on a timeout
    uint32_t latency_var;    // Its mean deviation (rttvar), ns - RFC 6298's, as for the reliable retry
    bool latency_backed_off; // The last call timed out; the next accepted answer replaces the estimate
    // Tells this Client from the context's other Clients of the same service, which share its endpoint_id: the
    // smallest of 1..255 none of them holds, given at create, 0 when all are taken. Carried in every CallRequest
    // (tt_CallRequestHeader.client_tag) so the server keeps an answer per Client, not per context.
    uint8_t client_tag;

    // Known Servers matching this Client's service, learned via UPDATE announces - see
    // tt_UNICAST_PEER_THRESHOLD.
    struct tt_Peer peers[tt_MAX_PEER_COUNT];
};

struct tt_Server;
struct tt_Request;

// Identifies one specific request a tt_SERVER_CALLBACK was invoked for - opaque to the callback
// beyond stashing it away and handing it back to tt_Server_send_response() later (Milestone 17,
// rmw_tickle/PLAN.md). Deliberately just (receiver, seq_no): the same pair get_server_cache()
// already keys a *finished* response by, so a deferred (not yet answered) request reuses that
// same identification instead of inventing a second, parallel handle concept.
typedef struct tt_RequestId {
    uint8_t receiver;
    uint16_t seq_no;
} tt_RequestId;

// Passed as `return_code` from a tt_SERVER_CALLBACK to mean "don't encode/send a response for
// this request yet - I'll answer it later via tt_Server_send_response(), possibly from a
// different thread." `response` is ignored in that case (the callback hasn't filled it in).
// Reserved the same way tt_CALL_TIMEOUT is (see its own comment) - not a real application return
// code.
#define tt_CALL_DEFERRED ((int8_t)-127)

typedef int8_t (*tt_SERVER_CALLBACK)(struct tt_Server* server, struct tt_Request* request, struct tt_Response* response,
                                     tt_RequestId request_id);

// Identifies which server/slot a scheduled timer (server_cache_clean, or Milestone 17's own
// pending-response timeout) belongs to. Embedded (one per slot) in struct tt_Server so scheduling
// either timer never needs a malloc.
struct server_cache_clean_config {
    struct tt_Server* server;
    int slot;
};

struct tt_Server { // extends endpoint
    struct tt_Endpoint endpoint;
    struct tt_Context* node;
    struct tt_Service* service;
    tt_SERVER_CALLBACK callback;

    // Fixed backing storage for cached responses (resent as-is if a client retries before
    // seeing one), so caching a response never has to malloc/free on the RPC hot path.
    tt_ALIGNAS(8) uint8_t cache_buf[tt_MAX_SERVER_CACHE_COUNT][tt_SERVER_CACHE_ENTRY_LENGTH];
    // Where cached responses are kept - tt_MAX_SERVER_CACHE_COUNT entries of cache_entry_length - when
    // tt_Server_set_storage() attached the caller's own; NULL (create's default, and a zeroed
    // struct's) means cache_buf above. A response larger than an entry is still sent, just not
    // cached (set_server_cache(), tickle.c).
    uint8_t* cache_storage;
    uint32_t cache_entry_length;
    struct tt_SubmessageHeader* cache[tt_MAX_SERVER_CACHE_COUNT]; // NULL when slot i is unused
    struct server_cache_clean_config clean_config[tt_MAX_SERVER_CACHE_COUNT];
    bool clean_scheduled[tt_MAX_SERVER_CACHE_COUNT]; // Whether clean_config[i]'s timer is pending
    // When slot i's response last went out (cached, or resent to a retry); 0 when the slot holds nothing. An expired
    // entry keeps it: cache[i] is NULL but the slot still names (client, seq_no), so a retry arriving after the
    // expiry measures how much longer that client waits than this server kept its answer (server_cache_lifetime()).
    uint64_t cache_sent_at[tt_MAX_SERVER_CACHE_COUNT];
    // Which incarnation of the client slot i's response is for: the client's entity_id as discovery knew it when the
    // response was cached (struct tt_DiscoveredEntity.entity_id, drawn per launch), 0 when discovery did not know it.
    // A request whose client discovery now knows under another entity_id comes from a new client - a restarted
    // process reusing the context id, whose seq_no starts at 0 again - and must not get this response.
    uint32_t cache_client_entity[tt_MAX_SERVER_CACHE_COUNT];
    // Which Client of its context slot i's response is for (tt_CallRequestHeader.client_tag): an entry is named by
    // (receiver, client_tag, seq_no), and a new answer replaces only its own Client's previous one - one live answer
    // per calling Client, so a retry from one Client of a context still finds its answer after another Client of the
    // same context was answered (before 2026-10-09 the second answer replaced the first and the retry re-ran the
    // callback).
    uint8_t cache_client_tag[tt_MAX_SERVER_CACHE_COUNT];
    // The longest recent gap, ns, between a response going out and the same client asking again - a decaying
    // maximum of what retries have shown this server. 0 until the first retry arrives.
    uint64_t client_retry_gap;

    // Milestone 17: one slot per request whose callback returned tt_CALL_DEFERRED - tracked
    // separately from cache[]/cache_buf[] above, which only ever holds an *already-answered*
    // response kept around for retry resends; a deferred request has no encoded response yet, so
    // it needs its own (receiver, seq_no) key instead of one recovered from encoded bytes.
    //
    // slot_state[] is the only field here tt_Server_send_response() (callable from *any* thread,
    // not just the one driving this node's own tt_Context_poll() loop - see that function's own doc
    // comment) ever touches, and only through __atomic_* builtins, never a plain read/write - it
    // is deliberately declared as plain uint8_t, not C11 _Atomic, because this header must stay
    // includable from C++ (rosidl_typesupport_tickle_c/_cpp both do - see tt_ALIGNAS's own
    // comment above for the identical constraint) and <stdatomic.h> is not a C++ header at all.
    // GCC/Clang's __atomic builtins need no special header or type qualifier on either side to
    // work correctly, unlike <stdatomic.h>'s own _Atomic(T) wrapper type.
    uint8_t slot_state[tt_MAX_SERVER_CACHE_COUNT]; // tt_SERVER_SLOT_EMPTY/_PENDING/_READY
    tt_RequestId pending_request_id[tt_MAX_SERVER_CACHE_COUNT];
    uint8_t pending_client_tag[tt_MAX_SERVER_CACHE_COUNT];   // the request's client_tag, for its cache entry
    uint32_t pending_sender_ip[tt_MAX_SERVER_CACHE_COUNT];   // for the same unicast-the-response
    uint16_t pending_sender_port[tt_MAX_SERVER_CACHE_COUNT]; // optimization process_callrequest() uses
    int8_t pending_return_code[tt_MAX_SERVER_CACHE_COUNT];   // tt_Server_send_response()'s own return_code arg
    // No copy of a deferred response is kept: tt_Server_send_response() encodes from the caller's struct
    // before it returns (2026-09-27), so the pending_response_buf / pending_storage that held one are gone.
    struct server_cache_clean_config pending_timeout_config[tt_MAX_SERVER_CACHE_COUNT];
    bool pending_timeout_scheduled[tt_MAX_SERVER_CACHE_COUNT];
};

// slot_state[] values - see struct tt_Server's own field comment on why these guard a plain
// uint8_t via __atomic builtins instead of a C11 _Atomic-qualified type.
#define tt_SERVER_SLOT_EMPTY 0   // unused, available for a new deferred request
#define tt_SERVER_SLOT_PENDING 1 // received, callback returned tt_CALL_DEFERRED, no response yet
#define tt_SERVER_SLOT_READY 2   // tt_Server_send_response() filled it and is sending it (under the state lock)

// Answers a request whose tt_SERVER_CALLBACK previously returned tt_CALL_DEFERRED for the given
// request_id - typically called later, from a different thread than the one driving this node's
// own tt_Context_poll() loop (e.g. whatever thread a ROS 2 executor happens to run a service handler
// on), which is the entire reason this function exists rather than just answering synchronously
// like a non-deferred tt_SERVER_CALLBACK already can. See rmw_tickle/PLAN.md's Milestone 17 for
// the full design rationale (why TickLE core, not just rmw_tickle, needs this primitive).
//
// **The response is encoded and sent before this returns** (2026-09-27), on the caller's thread and under the
// node's state lock, as tt_Publisher_publish() does - re-entrant, so a callback may call it too. **The caller may
// free or reuse response, and everything it points at, as soon as this returns.** Until 2026-09-27 the struct was
// copied shallowly into a per-slot buffer and encoded later by the poll thread: a response pointing at data it did
// not own (a string - every generated TickLE struct holds strings as pointers) was then encoded from whatever
// that data had become, and rmw_tickle's service responses, which alias the ROS response rclcpp frees as soon as
// rmw_send_response() returns, arrived with every string past std::string's inline 15 bytes as garbage.
//
// return_code is whatever a synchronous tt_SERVER_CALLBACK would otherwise have returned itself
// (it couldn't - it returned tt_CALL_DEFERRED instead, precisely so the real answer could be
// computed later, possibly on another thread, which is what this call now provides).
//
// Returns tt_RET_OK once the response has been handed to the network, tt_RET_NOT_FOUND if request_id doesn't
// match any request still waiting on a response (already answered by a previous call, already timed out, or was
// never deferred). The response is encoded from the caller's own struct: no copy of it is kept anywhere.
tt_ret_t tt_Server_send_response(struct tt_Server* server, tt_RequestId request_id, int8_t return_code,
                                 struct tt_Response* response);

// tt_Request / tt_Response / tt_Data are opaque bases: the application defines the real,
// generated struct for its own message type and hands the library a pointer to it, which the
// per-type encode/decode callbacks cast back. The single reserved byte is only there so these
// are valid ISO C (an empty struct is a GNU extension - a consumer building the public headers
// with -std=c99 -pedantic-errors would otherwise fail to compile them).
struct tt_Request {
    char reserved;
};

struct tt_Response {
    char reserved;
};

typedef int32_t (*tt_REQUEST_ENCODE_SIZE)(struct tt_Request* request);
typedef int32_t (*tt_REQUEST_ENCODE)(struct tt_Request* request, uint8_t* payload, const uint32_t len);
typedef int32_t (*tt_REQUEST_DECODE)(struct tt_Request* request, const uint8_t* payload, const uint32_t len,
                                     bool is_native_endian);
typedef void (*tt_REQUEST_FREE)(struct tt_Request* request);
typedef int32_t (*tt_RESPONSE_ENCODE_SIZE)(struct tt_Response* response);
typedef int32_t (*tt_RESPONSE_ENCODE)(struct tt_Response* response, uint8_t* payload, const uint32_t len);
typedef int32_t (*tt_RESPONSE_DECODE)(struct tt_Response* response, const uint8_t* payload, const uint32_t len,
                                      bool is_native_endian);
typedef void (*tt_RESPONSE_FREE)(struct tt_Response* response);

struct tt_Service {
    const char* name;
    uint32_t request_size;
    uint32_t response_size;
    tt_REQUEST_ENCODE_SIZE request_encode_size;
    tt_REQUEST_ENCODE request_encode;
    tt_REQUEST_DECODE request_decode;
    tt_REQUEST_FREE request_free;
    tt_RESPONSE_ENCODE_SIZE response_encode_size;
    tt_RESPONSE_ENCODE response_encode;
    tt_RESPONSE_DECODE response_decode;
    tt_RESPONSE_FREE response_free;

    // QoS
    uint32_t call_retry_interval; // 0 means auto
    uint32_t call_retry_count;    // 0 means tt_CALL_RETRY_COUNT
};

struct tt_Topic;

struct tt_Data {
    char reserved; // see tt_Request's own note - opaque base, one byte only to stay valid ISO C
};

// Opt-in per-Publisher retained-sample cache, shared by two independent QoS policies - QoS
// roadmap #5 (RELIABILITY/RELIABLE: retransmission on ACKNACK) and QoS roadmap #4 (DURABILITY/
// TRANSIENT_LOCAL: backlog delivery to a newly-discovered Subscriber, tt_Publisher.durable below).
// Caller-owned, the same convention as struct tt_Discovery (tt_Context_set_discovery()): a
// best-effort Publisher (today's only default) leaves tt_Publisher.reliable_cache NULL and pays
// nothing for this; one that wants either policy provides a zeroed struct tt_ReliableCache of its
// own (stack/static/wherever, must stay valid and unmoved until tt_Publisher_destroy() - same
// lifetime rule as every other tt_* struct) and points reliable_cache at it - set directly any
// time after tt_Context_create_publisher() returns, same "caller-owned, plain field access"
// convention as pub->batch. tt_Publisher_publish() appends the raw encoded DATA submessage bytes
// here after every successful send (KEEP_LAST eviction once `depth` slots are full); an incoming
// ACKNACK (process_submessage()) looks samples up here by seq_no to retransmit, and a newly-
// discovered Subscriber (decode_update_entities()) gets every currently-retained entry unicast
// straight to it, oldest first, when tt_Publisher.durable is set.
//
// index[]/capacity/arena, not a fixed tt_MAX_RELIABLE_HISTORY-sized array embedded here directly
// (rmw_tickle/PLAN.md's own "DDS semantic-parity backlog" row 2, and B1's own byte arena) - the
// caller owns both the index array (any struct tt_ReliableCacheIndex[N] it likes: stack, static,
// or - for a caller that already accepts dynamic allocation elsewhere, like rmw_tickle - heap)
// and the byte arena the records themselves live in, the same
// "caller decides the size, no malloc inside TickLE core itself" idiom struct tt_Discovery already
// uses for tt_MAX_DISCOVERED_ENTITIES. This is what actually makes `depth` a real, *per-Publisher*
// DDS RESOURCE_LIMITS/HISTORY.depth equivalent instead of a single build-wide ceiling every
// Publisher paid for or was capped by alike: an embedded-target Publisher that only ever needs a
// handful of retries can size its own array tiny, while a high-throughput Linux one (or a real
// ROS 2 caller's own requested QoS depth, rmw_tickle) can size it far past the old
// tt_MAX_RELIABLE_HISTORY=64 default without recompiling TickLE core itself or wasting memory on
// every *other* Publisher that doesn't want it. `depth` (1..capacity) is still the real in-use
// ring size within that array - kept a separate field from `capacity` on purpose, mirroring DDS's
// own separate HISTORY.depth vs RESOURCE_LIMITS.max_samples: a caller can shrink `depth` at
// runtime (a plain field write) without touching the backing array at all, the same way DDS lets
// HISTORY.depth vary independently of a fixed resource ceiling.
//
// Honest, load-bearing limitation, found while making depth caller-configurable and worth stating
// plainly rather than implying "just raise depth" fixes every retention-window problem: raising a
// specific Publisher's own `depth`/`capacity` past tt_RELIABLE_BITMAP_BITS only ever helps
// DURABILITY's own one-shot backlog push (deliver_durability_backlog() walks entries[] directly,
// no ACKNACK/bitmap involved at all) - it does NOT, by itself, improve RELIABLE's own ACKNACK-
// driven recovery under sustained loss at high throughput, because struct tt_WriterProxy.
// received_bitmap (the *Subscriber's* own out-of-order tracking) is a fixed-width field: once more
// than tt_RELIABLE_BITMAP_BITS newer samples arrive while one gap stays unresolved, update_
// reliable_ack() is forced to jump_ack_baseline() and abandon that gap outright, regardless of how
// deep the Publisher's own cache still reaches back. At TickLE's own real measured throughput
// (~1.2M msg/s), the original 64-bit width passed in roughly 53 microseconds - almost certainly
// shorter than one real ACKNACK round trip on any real network - so this Subscriber-side ceiling,
// not the Publisher-side cache depth fixed here, was very likely the actual bottleneck behind
// TickLE's own comparatively poor tc-loss recovery at high throughput (rmw_tickle/COMPARISON.md).
// **Update, rmw_tickle/PLAN.md's "TickLE-native performance" plan**: tt_RELIABLE_BITMAP_BITS was
// since widened 64 -> 256 (a real wire-protocol change, tt_VERSION bumped) once this same
// diagnosis pointed at it directly - raises the tolerable gap ~4x, likely closing most or all of
// the measured residual loss at TickLE's own real ACKNACK RTT, but honestly still not a guaranteed
// full fix: the real link's own RTT, not this window alone, sets the actual ceiling, and a
// genuinely different Subscriber-side tracking mechanism (e.g. more than one simultaneously-open
// gap window) would be needed to remove the ceiling concept entirely, out of this change's scope.
//
// One cache for both, not two (PLAN.md's Milestone 24) - matches real DDS/RTPS, where DURABILITY
// at the TRANSIENT_LOCAL level this package implements isn't a separately-sized cache at all: a
// late-joining reader just gets whatever's currently in the Writer's own single History Cache,
// the same one HISTORY.depth/RESOURCE_LIMITS already size for RELIABILITY's own retransmission.
// TickLE used to keep two independent caches here (struct tt_DurableCache, since removed) with
// their own separately-tunable depths, requiring a _Static_assert (tickle.c) to keep the two in
// sync whenever either changed - unified into this one struct/depth instead, since RELIABILITY and
// DURABILITY may still independently be requested (either, both, or neither - tt_Publisher.durable
// below), they just now read from the same underlying storage rather than duplicating it.
// B1 (rmw_tickle/PLAN.md) - one metadata slot per retained sample. The encoded bytes themselves
// live in the caller's own byte arena (struct tt_ReliableCache.arena below), packed back to back,
// instead of a fixed tt_MAX_BUFFER_LENGTH buffer per slot: a 76-byte sample used to cost 1488
// bytes of slot regardless, ~93% of it dead space (depth 1024 = 1.45MB, and rmw_tickle's own
// KEEP_ALL depth 8192 = 12.2MB, per Publisher).
struct tt_ReliableCacheIndex {
    // QoS roadmap #6 (LIFESPAN, rmw_tickle/PLAN.md) - tt_get_ns() at cache_reliable_sample() time.
    // reliable_cache_entry_expired() (tickle.c) compares this against tt_Publisher.lifespan_
    // duration_ns to decide whether this entry may still be retransmitted (process_acknack()) or
    // handed to a newly-discovered Subscriber (deliver_durability_backlog()) - past that age it's
    // treated "as if it had never been sent" (real DDS's own LIFESPAN wording), same as an evicted
    // or never-populated slot, even though the bytes are still physically sitting in the arena.
    uint64_t timestamp;
    uint32_t seq_no; // which sample this slot currently describes
    uint32_t offset; // where its encoded bytes start in arena[]; meaningless when len == 0
    uint16_t len;    // encoded submessage length in arena[]; 0 = empty slot, or evicted/not-cached
                     // (a "tombstone": the slot still names seq_no, but its bytes are gone - the
                     // only thing that distinguishes it from a live entry, see find_resendable_
                     // cache_entry(), tickle.c)
    uint8_t retry;   // ACKNACK retransmit count - not consulted by DURABILITY's own one-shot push
    uint8_t reserved;
};

// Milestone 58 (rmw_tickle/PLAN.md) - remembers which remote node_ids have already received this
// Publisher's own DURABLE backlog, keyed by context_id *and* the generation of the announcing node's
// endpoint list as of that delivery (tt_VERSION 7: the low 32 bits of its last_modified, see
// tt_DISCOVERY_ENDPOINT_ID) - not just by tt_Publisher.peers[]'s own array position, which check_
// liveliness()'s own presumed-dead cleanup (a load-induced false positive, not necessarily a real
// departure) wipes and lets a later upsert_peer() call reuse for an unrelated context_id. Without
// this, the exact same still-alive peer's very next (entirely unchanged) announce looks like a
// brand-new match to register_subscriber_peer_on_publisher() - re-triggering a full backlog
// re-delivery of data that peer already has (observed for real: durability_late_join delivering a
// 20-sample backlog 7 times over, 140 total, correlated with "presumed dead" warnings under load).
// last_modified, not context_id alone, is what tells a genuinely-restarted instance of the same
// context_id (a real new match - its own local subscription state was wiped too, it needs the
// backlog again) apart from the same continuous instance recovering from a transient gap
// (last_modified unchanged, since nothing about its own Publisher/Subscriber set actually
// changed) - tt_Context.last_modified is a tt_get_ns() (monotonic-clock) reading, refreshed every
// time a Publisher/Subscriber/Client/Server is created or destroyed on that node (tickle.c), so a
// genuine process restart reliably lands on a different value than whatever this table last saw,
// while an unchanged, still-running instance keeps announcing the exact same one.
struct tt_DurableDeliveryRecord {
    uint8_t context_id;  // tt_CONTEXT_ID_INVALID (0, matching zero-init) = empty slot
    uint32_t generation; // the announcing node's announce generation as of the delivery below
};
struct tt_ReliableCache {
    // The actual size of the caller-provided index[] array below, in element count - the real
    // per-Publisher ceiling depth is clamped against everywhere in tickle.c (replaces every former
    // bare tt_MAX_RELIABLE_HISTORY reference). 0 (this struct's own zero-init default, before a
    // caller sets index/capacity) is a legitimate, safe "nothing usable yet" state - every depth-
    // clamping call site treats it exactly like "no cache" (see cache_reliable_sample()/deliver_
    // durability_backlog()/process_acknack()'s own shared clamp expression).
    uint16_t capacity;
    // Counted in seq_no, which is in DATAGRAMS once samples fragment: a fragmented sample is retained one
    // record per datagram, so a depth of N keeps N / k samples of k fragments (DATAFRAG_PLAN.md section
    // 13). KEEP_LAST evicts whole samples, never a sample's first datagram without the rest; size depth as
    // samples x fragments per sample to keep that many samples.
    uint16_t depth; // in-use ring size, 1..capacity - see this struct's own doc comment for why
                    // this stays a separate field from capacity rather than always equaling it.
                    // Fixed once the first sample has been cached: slot (seq_no - 1) % depth is
                    // how every lookup finds a sample (find_resendable_cache_entry(), tickle.c),
                    // so changing depth mid-stream would silently mis-address everything retained.
    // Caller-owned backing storage, capacity entries long (stack/static/heap - caller's choice,
    // see this struct's own doc comment) - NOT embedded here directly, unlike almost every other
    // fixed-size table in this file. NULL (this struct's own zero-init default) is the "not set up
    // yet" / "capacity is still 0" state, handled the same safe way capacity's own doc comment
    // describes.
    struct tt_ReliableCacheIndex* index;
    // B1 - caller-owned byte arena the index above points into, arena_size bytes long. Records are
    // packed back to back and never split: a record that doesn't fit before the end wraps to
    // offset 0, wasting the tail fragment until the ring passes it, so every retransmit/backlog
    // send stays one contiguous memcpy. Size it with tt_RELIABLE_CACHE_ARENA_BYTES() (config.h) -
    // that includes the one record of wrap slack KEEP_LAST-by-count needs, so `depth` samples
    // always fit regardless of where a wrap lands.
    //
    // Core only ever memcpy()s into and out of this memory, never casts a struct pointer into it,
    // so it needs no particular alignment - keep it that way.
    uint8_t* arena;
    uint32_t arena_size;
    // The largest arena_size this cache may ever reach, or 0 for "arena_size is final" (2026-09-25).
    // Only tt_ReliableCache_grow() reads it, and nothing ever writes it after init: a caller that
    // wants to reserve memory lazily sets it to the size it is willing to reach, hands over a
    // smaller arena to start with, and grows into it. That the limit itself cannot be raised is the
    // point - a KEEP_ALL Publisher that refuses a write must stay refused once its arena is at the
    // limit, or back-pressure would turn into unbounded growth (see tt_Publisher_publish()).
    uint32_t arena_limit;
    // Core-private bookkeeping (a caller sets only the six fields above; zero-init = empty).
    // KEEP_LAST's bound in SAMPLES, when depth - which counts seq_no, and so datagrams once samples
    // fragment - is not the bound meant (DATAFRAG_PLAN.md section 13). 0: depth alone bounds the cache, as
    // before. rmw_tickle sets depth to the history depth times the datagrams its largest message takes and
    // this to the history depth, so smaller messages do not make it keep more of them than the QoS says.
    // KEEP_LAST only: it evicts to honour the bound, which a KEEP_ALL publisher must never do.
    uint16_t sample_depth;
    uint16_t retained_samples; // core-owned: samples whose first record is retained now
    // (g10, rmw_tickle/RMW_GAPS_PLAN.md) core-owned: KEEP_LAST samples cached while the arena could not keep
    // sample_depth of them - an eviction for bytes inside the depth, because the Publisher's cache_grow could not
    // make room (or it has none). A caller reports it; core only counts.
    uint32_t depth_shortfalls;
    uint32_t oldest_seq_no; // oldest retained sample, 0 = nothing retained (the arena is empty)
    uint32_t newest_seq_no; // newest sample handed to cache_reliable_sample(), cached or not
    uint32_t tail;          // arena offset just past the newest retained record
    // Only ever consulted when tt_Publisher.durable is set (register_subscriber_peer_on_
    // publisher(), tickle.c) - costs a best-effort or reliable-only Publisher nothing beyond the
    // unused array slots themselves, no extra allocation or opt-in flag needed. Same capacity as
    // peers[] (tt_MAX_PEER_COUNT) - this only ever needs to remember as many distinct node_ids as
    // could plausibly be *currently* matched at once; a table overflow (durable_delivered_upsert()
    // finding no empty slot and no existing match) falls back to "just re-deliver" - safe, only
    // costs the one-time redundant delivery this milestone exists to avoid, never a correctness
    // problem.
    struct tt_DurableDeliveryRecord durable_delivered[tt_MAX_PEER_COUNT];
};

#if tt_LARGE_SAMPLES
// One large sample a Publisher holds by reference (tt_Publisher.large[], DESIGN.md section 8): sent from its buffer,
// kept there for resends while `retained`, and handed back to the caller's release once every matched reader has
// acknowledged it or it is evicted - or, for a Publisher that retains nothing, once its last fragment has gone.
struct tt_LargeRecord {
    uint8_t* buffer;  // DataHeader at tt_LARGE_HEADER_OFFSET, CDR at tt_LARGE_CDR_OFFSET; NULL: free
    uint64_t sent_ns; // when it was published, for LIFESPAN
    uint32_t seq_no;  // its first datagram's; it takes `count` of them. 0 while it waits as tt_Publisher.large_pending
    uint32_t cdr_len; // padded to 4, as it is sent
    uint16_t count;
    bool retained; // kept for resends once sent: the Publisher has a reliable cache
    bool sent;     // every fragment has gone to every destination once
};

// Where a Publisher's large send stands (tt_Publisher.large_cursor): a burst of thousands of datagrams outruns any
// socket send buffer, so what does not fit is sent from tt_Context_poll() as the buffer drains.
#define tt_LARGE_DESTINATIONS (tt_MAX_LINK_COUNT > tt_MAX_PEER_COUNT ? tt_MAX_LINK_COUNT : tt_MAX_PEER_COUNT)
struct tt_LargeCursor {
    struct tt_Peer dests[tt_LARGE_DESTINATIONS]; // ip 0: the broadcast address
    uint64_t retry_ns;                           // the wait before the next try, doubled while one sends nothing
    uint32_t seq_no;                             // the record being sent; 0: idle
    uint16_t next_index;                         // its next fragment to the current destination
    uint8_t next_dest;
    uint8_t dest_count;
    bool scheduled;
};
#endif

struct tt_Publisher; // so the callback typedef below names this struct, not a prototype-scoped one

// Phase 3 (rmw_tickle/PLAN.md) - tt_Publisher.writable_callback's own type: `pub` is the Publisher
// that just became writable again, `param` is whatever writable_callback_param held. See that
// field's own doc comment for what a callback may legally do (in short: signal and return).
typedef void (*tt_PUBLISHER_WRITABLE_CALLBACK)(struct tt_Publisher* pub, void* param);

// Phase 3 step 4 (rmw_tickle/PLAN.md) - what a Subscriber knows about one remote writer's own
// HISTORY policy, which decides whether it may ever abandon a gap that writer hasn't answered.
// UNKNOWN is 0 so a zero-initialised tt_WriterProxy starts out making no assumption.
enum tt_WriterKeepAll {
    tt_WRITER_KEEP_ALL_UNKNOWN = 0, // nothing heard from this writer yet - never give up (see below)
    tt_WRITER_KEEP_ALL_NO,          // announced without tt_UPDATE_QOS_KEEP_ALL - bounded give-up applies
    tt_WRITER_KEEP_ALL_YES,         // announced KEEP_ALL - never give up
};

// One matched remote node's acknowledgement state on a Publisher - see tt_Publisher.peer_acks.
// struct tt_Publisher.departed_acks: how many recent departures it remembers, and for how long (1 s).
#define tt_DEPARTED_ACKS 8
#define tt_DEPARTED_ACK_NS 1000000000ULL

struct tt_PeerAck {
    uint8_t context_id; // tt_CONTEXT_ID_INVALID (0, matching zero-init) = unused entry
    // Phase 2 (rmw_tickle/PLAN.md) - which Subscriber *entity* on that node, learned from its own
    // announce (tt_UpdateEntity.entity_id) and matched against each ACKNACK's own
    // sender_entity_id. Keyed per entity, not per node, because two Subscribers of one topic in
    // one process are otherwise indistinguishable and the faster one's ack would speak for both -
    // silent loss under Phase 3's KEEP_ALL blocking.
    uint32_t entity_id;
    uint32_t ack_seq_no;
    // Phase 2 - the RELIABLE tracking window this Subscriber announced
    // (tt_UpdateEntity.tracking_words), in 64-bit words; 0 = the protocol default.
    // tt_Publisher_unacked_bound() takes the minimum across matched Subscribers.
    uint16_t tracking_words;
    // The writer owns the match point (2026-10-07). For a VOLATILE Subscriber matched by its announce: the first
    // sample this Publisher owes it, pub->seq_no + 1 at the match, as an RTPS ReaderProxy starts from the changes
    // after matching; 0 otherwise. While match_heartbeats_left is non-zero, each publish puts a Heartbeat whose
    // last_seq_no is first_owed_seq_no - 1 ahead of its DATA, in the same datagram, so the Subscriber's first contact
    // is that Heartbeat whichever sample is lost - and its baseline the owed sample, not the first one to arrive.
    // Cleared by the Subscriber's ack passing first_owed_seq_no, or after tt_MATCH_HEARTBEATS publishes.
    uint32_t first_owed_seq_no;
    uint8_t match_heartbeats_left;
};

struct tt_Publisher { // extends endpoint
    struct tt_Endpoint endpoint;
    struct tt_Context* node;
    struct tt_Topic* topic;
    // (g10, rmw_tickle/RMW_GAPS_PLAN.md) Grows reliable_cache's arena and returns whether it did - called, with the
    // context locked, when caching a KEEP_LAST sample would evict one of the newest sample_depth - 1 for bytes, until
    // it would not or this returns false. NULL (the default): the arena never grows, and such evictions are counted
    // (tt_ReliableCache.depth_shortfalls). Core allocates nothing; the caller that owns the arena does.
    bool (*cache_grow)(struct tt_Publisher* pub);
#if tt_LOCAL_DELIVERY
    // (g9, config.h's tt_LOCAL_DELIVERY) This context's own Subscribers on this endpoint id - the ones a publish also
    // delivers to in-process. Kept by add_endpoint_to_node() / remove_endpoint_from_node(); 0: a publish pays a branch.
    uint16_t local_subscriber_count;
#endif

    // This Publisher's own send-side sample counter (transcation) - wire seq_no is this + 1
    // (tt_Publisher_publish()'s own data_header->seq_no = pub->seq_no + 1), starting from 1.
    // uint32_t, matching every downstream 32-bit seq_no field that ultimately derives from it
    // (tt_DataHeader.seq_no, tt_AckNackHeader.seq_no, tt_ReliableCacheIndex.seq_no, peer_ack_
    // seq_no[] above) - a real bug found on real HIL (TickLE Plan, 2026-09-22): this field used
    // to be uint16_t, silently wrapping to 0 every 65536 publishes regardless of the wire's own
    // 32-bit capacity - at TickLE's own real max throughput (~150-190K msg/s), reliable_
    // throughput's own 8-second max-rate runs wrap more than 20 times *per run*, reusing already-
    // delivered sequence numbers mid-stream and corrupting RELIABLE ack tracking/DURABILITY
    // backlog indexing for the rest of that run. At uint32_t width the same wraparound is still
    // structurally possible, just past ~6.3 continuous hours at that same real max rate - not
    // specially handled here, the same "an edge case far enough out not to need explicit handling
    // yet" pragmatism this file already applies elsewhere (e.g. struct tt_Context.entity_id_base's
    // own doc comment). Distinct from struct tt_Client.seq_no/struct tt_Subscriber.seq_no below -
    // those pair with their own genuinely-16-bit wire counterparts (tt_CallRequestHeader.seq_no)
    // or are unused, not affected by this same bug.
    uint32_t seq_no;

    // The ring slot tt_Publisher_claim() lent this Publisher's caller, NULL when none: in `claim_segment` (peer
    // context `claim_context_id`), at ring index `claim_index`, with room for `claim_length` CDR bytes. One at a time.
    struct tt_SegmentSlot* claim_slot;
    struct tt_SegmentHeader* claim_segment;
    uint32_t claim_index;
    uint32_t claim_length;
    uint8_t claim_context_id;

    // Known Subscribers matching this Publisher's topic, learned via UPDATE announces - see
    // tt_UNICAST_PEER_THRESHOLD.
    struct tt_Peer peers[tt_MAX_PEER_COUNT];

    // QoS roadmap #5 (RELIABILITY) follow-up, tt_Publisher_wait_for_all_acked() - what each
    // matched remote node has acknowledged, keyed by context_id rather than index-aligned with
    // peers[] above (Phase 3 prerequisite (c), rmw_tickle/PLAN.md). Index alignment used to mean
    // process_announce()'s own forget-then-re-add cycle (a remote node changing *any* endpoint
    // re-announces, and forget_peers_from_source() cleared the slot) threw away ack state for
    // Subscribers that never went anywhere - harmless while nothing depended on it, but Phase 3's
    // KEEP_ALL blocking does: a writer that has to wait for acks must not have them silently reset
    // by an unrelated announce.
    //
    // ack_seq_no is that node's own most recently seen struct tt_AckNackHeader.seq_no - "every
    // seq_no below this has been received" (that struct's own doc comment) - only ever advanced,
    // never regressed by a stale/reordered ACKNACK. 0 means "no ACKNACK seen yet", which seq_no
    // never is on the wire (tt_Publisher_publish() starts at 1). context_id == tt_CONTEXT_ID_INVALID (0)
    // marks an unused entry.
    //
    // Still one entry per remote *node*, not per remote Subscriber: an ACKNACK carries no
    // identifier of the Subscriber that sent it (struct tt_AckNackHeader.entity_id names the
    // target Publisher), so two Subscribers of one topic on one node share this entry and the
    // faster one's ack speaks for both. Harmless for tt_Publisher_wait_for_all_acked()'s own
    // advisory use; a real gap for Phase 3's KEEP_ALL blocking, which needs per-Subscriber
    // identity on the wire - see rmw_tickle/PLAN.md's Phase 3 prerequisite (b).
    struct tt_PeerAck peer_acks[tt_MAX_ACK_ENTRIES];
    // The last tt_DEPARTED_ACKS ack entries dropped by a real departure, so a straggling ACKNACK from a reader that has
    // just left cannot re-claim it (claim_from_acknack(), tickle.c). entity_id 0 = every entity of context_id. A
    // record older than tt_DEPARTED_ACK_NS is ignored: it only has to outlast ACKNACKs already in flight.
    struct tt_DepartedAck {
        uint64_t at_ns;
        uint32_t entity_id;
        uint8_t context_id; // tt_CONTEXT_ID_INVALID = unused
    } departed_acks[tt_DEPARTED_ACKS];
    uint8_t departed_next;

    // false (tt_Context_create_publisher()'s own default): tt_Publisher_publish() flushes every call
    // immediately, same as RPC already does (DESIGN.md's "RPC and Publish flush immediately by
    // default; batching is opt-in") - lowest latency, and the right default for the common case of
    // one message per publish() call, not several back-to-back to the same destination. true:
    // batch instead, deferring to node_flush()'s own tt_CONTEXT_TX_INTERVAL tick, exactly how every
    // Publisher behaved before this field existed - set this on a specific Publisher that really
    // does call tt_Publisher_publish() several times in a row (a real but unusual pattern) and
    // would rather coalesce those into fewer, larger packets than minimize any one message's own
    // latency. Set directly on the struct any time after tt_Context_create_publisher() returns it -
    // same "caller-owned, plain field access" convention as peers[]/seq_no above.
    bool batch;

    // QoS roadmap #5 / Phase 3 (rmw_tickle/PLAN.md) - DDS HISTORY KEEP_ALL: never evict a sample no
    // matched Subscriber has acknowledged yet; refuse the write instead (tt_Publisher_publish()
    // returns tt_RET_WOULD_BLOCK, having sent and cached nothing). false (the default) is KEEP_LAST,
    // today's behaviour, where the oldest unacknowledged sample is simply overwritten.
    //
    // The bound is min(this Publisher's own cache depth, tt_Publisher_unacked_bound()) - the
    // narrowest RELIABLE tracking window any matched Subscriber announced. A Subscriber cannot ask
    // about a gap older than its own window, so an unacknowledged run deeper than that is
    // unrecoverable however much this Publisher retains (measured, Phase 2: a window wider than the
    // Publisher's depth recovers strictly less, not more).
    //
    // Requires a reliable_cache; on a Publisher without one it is ignored, since there is nothing to
    // retain and so nothing to refuse for.
    //
    // Setting this also makes the Publisher solicit acknowledgements on its own - at half of its
    // blocking bound, and again on every refusal, both under the ack_solicit_watermark_pct rate
    // limit - without ack_solicit_watermark_pct being set. That is not a convenience: a Subscriber
    // only sends an ACKNACK when it sees a gap, so on a link that loses nothing there is nothing to
    // advance the acknowledgement, and a Publisher that blocks on acknowledgements would stop at
    // its bound and never resume. Measured on real hardware before this was fixed (rmw_tickle/
    // PLAN.md Phase 3 step 4): a lossless run published exactly its 1024-sample window and then
    // nothing further, 0.125 Mbps where the same run reaches 109 Mbps once acknowledgements flow.
    // Zero loss is the worst case for KEEP_ALL, not the easiest one.
    bool keep_all;
    // Until when a KEEP_ALL Publisher that has matched no Subscriber yet grows its arena rather than evict (0: not
    // started). Set at its first cached publish to one tt_CONTEXT_UPDATE_INTERVAL later (keep_all_room_before_match()).
    uint64_t keep_all_unmatched_until_ns;

    // Phase 3 - fired when a KEEP_ALL Publisher that had to refuse a write becomes writable again,
    // i.e. when an incoming ACKNACK advances the slowest matched Subscriber far enough. NULL (the
    // default) means "poll tt_Publisher_writable() instead"; both are offered deliberately.
    //
    // Runs on the node's own thread, from inside tt_Context_poll(), so TickLE's single-threaded-per-node
    // discipline holds. It must not publish, create or destroy endpoints, or otherwise re-enter
    // TickLE - signal and return (rmw_tickle wakes a condvar and lets its blocked rmw_publish() do
    // the work). Fired once per refusal-to-writable transition, not once per ACKNACK.
    tt_PUBLISHER_WRITABLE_CALLBACK writable_callback;
    void* writable_callback_param;
    // Core-private: set when a publish was refused, cleared when the callback fires.
    // The record size a publish was already refused for, or 0 (2026-09-25). KEEP_ALL's promise has
    // two bounds - the unacknowledged COUNT, which keep_all_bound() sets, and the arena's BYTES -
    // and only the first can be checked before a sample is encoded. When the second refuses one,
    // this remembers how big it was, so tt_Publisher_writable() and the writable callback answer
    // about the sample the caller will actually retry rather than about the count alone. Cleared by
    // the next publish that is admitted.
    uint32_t blocked_record_bytes;
    // The same for the count bound: how many datagrams - each its own seq_no - a sample refused for its
    // fragment count needed (DATAFRAG_PLAN.md section 13), 0 when none was. Cleared with the above.
    uint16_t blocked_datagrams;
    // Some peer_acks[] entry still has match_heartbeats_left (tt_PeerAck.first_owed_seq_no): one test per publish.
    bool match_heartbeat_pending;
    bool writable_pending;

    // NULL (tt_Context_create_publisher()'s own default): no retained-sample storage at all - both
    // reliable/durable below must stay false, nothing for either policy to work from. Non-NULL:
    // storage for whichever of the two policies below is set - see struct tt_ReliableCache's own
    // doc comment above for why one cache backs both.
    struct tt_ReliableCache* reliable_cache;

    // false (tt_Context_create_publisher()'s own default): BEST_EFFORT, today's only default. true:
    // RELIABLE - requires reliable_cache to already be non-NULL too (nothing to retransmit from
    // otherwise). process_acknack()'s own retransmit loop doesn't actually consult this flag (it
    // answers any ACKNACK it can, straight off reliable_cache, regardless - matched Subscribers
    // only ever send one if they themselves opted into sub->reliable, so this can't fire
    // unprompted); what this flag *does* gate is Publisher-*initiated* RELIABLE traffic that has
    // no per-message opt-in of its own to lean on - specifically send_initial_heartbeat()'s own
    // automatic, unprompted announce on discovering a new peer (tickle.c) - without this flag, a
    // durable-only Publisher (durable below, reliable_cache set purely for backlog storage) would
    // also silently start emitting Heartbeats nobody asked for, the instant the shared cache
    // exists. tt_Publisher_set_heartbeat_period() below needs no separate check against this flag
    // - calling it at all is already the caller's own explicit opt-in for *that* Heartbeat.
    bool reliable;

    // false (tt_Context_create_publisher()'s own default): VOLATILE, today's only default. true:
    // DURABLE/TRANSIENT_LOCAL - a newly-discovered Subscriber gets every currently-retained entry
    // in reliable_cache above unicast to it (deliver_durability_backlog(), tickle.c); requires
    // reliable_cache to already be non-NULL too (nothing to deliver from otherwise). Independent
    // of reliable above - a Publisher may set either, both, or neither, the same VOLATILE/
    // TRANSIENT_LOCAL-vs-BEST_EFFORT/RELIABLE independence real DDS QoS allows, just now sharing
    // one cache underneath instead of two.
    bool durable;

    // 0 (tt_Context_create_publisher()'s own default): no periodic Heartbeat, today's only behavior.
    // Non-zero: a struct tt_HeartbeatHeader announce goes out every this-many nanoseconds - see
    // its own doc comment (tickle.h) and tt_Publisher_set_heartbeat_period()'s own doc comment
    // (below) for why this needs that explicit call, not just setting this field directly the way
    // reliable_cache/durable above are. Requires reliable_cache to already be set (nothing to
    // announce for a best-effort Publisher).
    uint64_t heartbeat_period_ns;

    // 0 (tt_Context_create_publisher()'s own default): off. Non-zero N: every Nth published sample
    // carries a Heartbeat in the same datagram - "everything below first_available_seq_no is gone",
    // for a reader that is waiting on a gap the publisher can no longer fill (2026-09-24).
    //
    // The alternative to heartbeat_period_ns above, and measured against it. A periodic Heartbeat
    // costs a whole datagram per period whatever the data rate, which on a quiet publisher is most
    // of its traffic. Piggybacked, it adds no datagram and no syscall - one small submessage on a
    // datagram being sent anyway - and its frequency follows the data rate, which is when a gap can
    // open at all. What it cannot do is speak once the publisher stops: a reader waiting on a gap
    // near the end of a burst hears nothing further, which periodic still covers.
    //
    // Appended AFTER the DATA, never before: a publisher unicasts only when nothing was already
    // pending in tx_buffer ahead of its DATA, so a Heartbeat placed first would silently turn every
    // piggybacked datagram into a broadcast. Set it directly, like reliable_cache/durable - it
    // schedules nothing, so unlike heartbeat_period_ns it needs no call.
    uint32_t heartbeat_piggyback_every;
    // Core-owned: samples published since the last piggybacked Heartbeat.
    uint32_t heartbeat_piggyback_count;
    // Diagnostic, core-owned: samples re-sent in answer to ACKNACKs (2026-09-25). Against seq_no -
    // how many were published - it is the retransmission ratio, which is how a Publisher can tell
    // it is working far harder than its loss rate explains. The rig case that prompted it ran at
    // ~47 transmissions per sample under 5% loss, and until this existed the only place that
    // showed was an interface counter on the host: the kernel was failing IP reassembly of
    // fragmented datagrams, and every failure came back as another request.
    uint32_t retransmitted;

    // 0 (tt_Context_create_publisher()'s own default): no periodic ACK solicitation, today's only
    // behavior. Non-zero: tt_Publisher_request_ack() (below) fires automatically every this-many
    // nanoseconds, instead of only when a caller happens to invoke it directly - see tt_Publisher_
    // set_ack_solicit_period()'s own doc comment (below) for why this needs that explicit call,
    // same "active scheduler operation, not a passive field" reasoning as heartbeat_period_ns
    // above. Requires reliable_cache to already be set (nothing to solicit an ack against
    // otherwise). Distinct from heartbeat_period_ns above, not a duplicate of it: that periodic
    // Heartbeat always sets tt_HEARTBEAT_FLAG_FINAL (its own doc comment, tickle.h), so a
    // Subscriber with no actual gap has no reason to ever reply - peer_acks[] (above) can
    // stay stale indefinitely on a fully healthy, loss-free link, since it only ever advances on
    // an ACKNACK reply. This field exists specifically to keep that value fresh regardless of gap
    // state, the same clear-tt_HEARTBEAT_FLAG_FINAL mechanism tt_Publisher_wait_for_all_acked()'s
    // own one-shot solicitation already uses, just on a recurring timer instead of a single call -
    // useful for anything that needs a near-real-time read of a peer's own ack position (e.g. a
    // RELIABLE Publisher's own send-side flow control, deciding whether to keep publishing or
    // pause based on how far a slow peer has fallen behind).
    uint64_t ack_solicit_period_ns;

    // Phase 3 prerequisite (d), rmw_tickle/PLAN.md - solicit an ACK as soon as this Publisher's own
    // retained-sample cache is this percent full of unacknowledged samples, instead of only on a
    // fixed timer (ack_solicit_period_ns above) or when loss is detected. 0 (the default) is off,
    // so nothing changes for an existing caller. 50 is the suggested starting point.
    //
    // Not consulted at all when keep_all is set: such a Publisher solicits on its own, at half of
    // its blocking bound, because it has no choice (see keep_all's own doc comment). This field
    // stays purely opt-in for everyone else.
    //
    // Rate-limited by last_ack_solicit_ns below, shared with the periodic path - at max throughput
    // the watermark is crossed continuously (a depth-64 cache turns over in well under a
    // millisecond), so without that limit this would be one Heartbeat per publish.
    uint8_t ack_solicit_watermark_pct;
    // tt_get_ns() of the most recent ACK solicitation from either path (watermark or periodic
    // timer), or 0 if none yet. Core-private bookkeeping, not a caller-set field.
    uint64_t last_ack_solicit_ns;
    // Whether a solicitation is waiting for its ACKNACK (solicit_ack_throttled()). Core-private.
    bool ack_solicit_outstanding;
    // seq_no when the last solicitation was sent: the watermark path asks again only a threshold of new seq_nos on.
    uint32_t ack_solicit_seq_no;
    // keep_all_resolicit() is in the scheduler for this Publisher. Core-private.
    bool resolicit_armed;

    // QoS roadmap #6 (LIFESPAN, rmw_tickle/PLAN.md). 0 (tt_Context_create_publisher()'s own default):
    // disabled, today's only behavior - reliable_cache entries never expire on their own (only
    // KEEP_LAST eviction removes them). Non-zero: the maximum age, in nanoseconds since tt_
    // ReliableCacheEntry.timestamp, that a cached sample may still be retransmitted (process_
    // acknack()) or handed to a newly-discovered Subscriber (deliver_durability_backlog()) - past
    // that, reliable_cache_entry_expired() (tickle.c) treats it "as if it had never been sent",
    // matching real DDS's own LIFESPAN semantics exactly (a Writer-side QoS - a plain caller-owned
    // field, no function call needed, same convention as reliable/durable above; independent of
    // both, may be combined with either, neither, or both). Plain age-based, no wire change: the
    // entry's own timestamp already comes from data_header->timestamp (tt_Publisher_publish()),
    // which every Subscriber already receives regardless of this field, so a Subscriber wanting
    // its *own* reader-side expiry enforces it independently, straight off that same timestamp
    // (see rmw_tickle_subscriber_t.lifespan_ns's own doc comment) - no coordination needed between
    // the two.
    uint64_t lifespan_duration_ns;

    // QoS roadmap #2 (DEADLINE) RxO, Milestone 49. 0 (tt_Context_create_publisher()'s own default):
    // no DEADLINE offered. Non-zero: what this Publisher announces on the wire (tt_UpdateEntity.
    // deadline_duration_ns) as its own maximum inter-publish gap - purely a wire-announcement
    // field, TickLE core itself never enforces or checks this on its own (the rmw layer already
    // does that independently, e.g. rmw_tickle's own check_publisher_deadline()); this only feeds
    // decode_update_entities()'s own Publisher-side gate and subscriber_incompatible_with_
    // publisher()'s own comparison (both tickle.c) on the *receiving* side. A plain caller-owned
    // field, same convention as reliable/durable/lifespan_duration_ns above.
    uint64_t deadline_duration_ns;
    // QoS roadmap #3 (LIVELINESS) RxO, Milestone 49 - this Publisher's own offered liveliness
    // lease duration, in nanoseconds; 0 = no specific lease requirement announced. Independent of
    // liveliness_manual below - see tt_UpdateEntity.liveliness_lease_duration_ns's own doc comment
    // (tickle.h) for why kind and lease duration are two separate pieces of information, not one.
    uint64_t liveliness_lease_duration_ns;
    // QoS roadmap #3 (LIVELINESS) RxO, Milestone 49. false (tt_Context_create_publisher()'s own
    // default): AUTOMATIC. true: MANUAL_BY_TOPIC (the only manual kind this package's own rmw
    // layer still supports, Milestone 32's own finding) - see tt_UPDATE_QOS_LIVELINESS_MANUAL's
    // own doc comment (tickle.h) for the wire bit this becomes.
    bool liveliness_manual;
    // When this Publisher last asserted its liveliness on the wire - a publish, or tt_Publisher_assert_
    // liveliness() sending a HEARTBEAT with tt_HEARTBEAT_FLAG_LIVELINESS. That function sends nothing more
    // within a tt_LIVELINESS_LEASE_DIVISOR-th of the lease of this. 0: never.
    uint64_t liveliness_asserted_ns;
#if tt_LARGE_SAMPLES
    // Large-message stage 2 (DESIGN.md section 8), core-owned, last so that no field above moves. The large samples
    // held by reference, oldest at large_head, in seq_no order; the send in progress; the newest sample published
    // while a send was in progress, waiting for it without seq_nos yet - a newer one replaces it, KEEP_LAST, before
    // anything of it has gone; and the datagram count of a large publish KEEP_ALL refused, which
    // tt_Publisher_writable() answers about (0: none).
    struct tt_LargeRecord large[tt_LARGE_RETAINED];
    struct tt_LargeCursor large_cursor;
    struct tt_LargeRecord large_pending;
    // The end-of-sample HEARTBEAT asked again while a sent large sample stays unacknowledged (large_ack_chase(),
    // tickle.c): armed, and the wait before the next ask.
    uint64_t large_ack_wait_ns;
    bool large_ack_armed;
    uint8_t large_head;
    uint8_t large_count;
    uint16_t large_blocked;
#endif
};

// Arms (or re-arms, or disables with period_ns == 0) pub's own periodic Heartbeat announce - see
// struct tt_HeartbeatHeader's own doc comment (tickle.h) for what it's for. Unlike reliable_cache/
// durable (plain caller-owned fields, no function call needed to "activate" them), arming a
// periodic tt_Context_schedule() entry is an active operation with no passive-field equivalent - call
// this any time after tt_Context_create_publisher() returns, once pub->reliable_cache is already set.
// B1 (rmw_tickle/PLAN.md) - fills in a struct tt_ReliableCache from the caller's own two pieces of
// storage: an index array (capacity slots) and a byte arena. Optional - a caller may still set the
// five public fields itself - but it validates what a hand-written setup can silently get wrong,
// and zeroes the core-private bookkeeping. depth starts equal to capacity; a caller wanting a
// smaller in-use ring sets cache->depth afterwards, before the first publish (see that field's own
// doc comment).
//
// Size the arena with tt_RELIABLE_CACHE_ARENA_BYTES(depth, max_record) (config.h) so `depth`
// samples always fit. Returns tt_RET_INVALID_ARGUMENT for a NULL argument, capacity 0, arena_size
// 0, or an arena too small for even one maximum-size record when the caller may publish one.
tt_ret_t tt_ReliableCache_init(struct tt_ReliableCache* cache, struct tt_ReliableCacheIndex* index, uint16_t capacity,
                               uint8_t* arena, uint32_t arena_size);

// Moves this cache onto a larger caller-owned arena, keeping every retained sample (2026-09-25).
// For a caller that would rather reserve its full budget only if the traffic asks for it: start
// small, set arena_limit at init, and call this when the arena fills.
//
// new_arena must be new_arena_size bytes and must not overlap the current one. The retained records
// are copied into it in sequence order, packed from offset 0, so the wrap fragment the old ring may
// have been wasting is recovered too. The old arena is untouched and still the caller's to free
// once this returns tt_RET_OK.
//
// The index array does not move and its slots do not change: `depth` is fixed for the life of the
// cache (see it above), so a sample keeps the slot it had and only the bytes behind it relocate.
//
// Refuses (tt_RET_INVALID_ARGUMENT, changing nothing) a new_arena_size that is smaller than the
// current one - shrinking would have to drop samples - or larger than arena_limit. Call it only
// from the same context as the Publisher's other calls; it is not safe against a concurrent
// publish or poll.
tt_ret_t tt_ReliableCache_grow(struct tt_ReliableCache* cache, uint8_t* new_arena, uint32_t new_arena_size);

// Returns tt_RET_INVALID_ARGUMENT if pub->reliable_cache is still NULL (period_ns == 0 is always
// accepted regardless, since disabling never needs a cache).
tt_ret_t tt_Publisher_set_heartbeat_period(struct tt_Publisher* pub, uint64_t period_ns);

// QoS roadmap #5 (RELIABILITY) follow-up - tt_Publisher_wait_for_all_acked() (rmw_tickle's own
// rmw_publisher_wait_for_all_acked() is built on this). Sends one Heartbeat, straight to every
// currently-matched peer (pub->peers[]) - not broadcast, unlike send_heartbeat()'s own threshold-
// based choice, since wait_for_all_acked() only ever cares about peers currently known to exist -
// with tt_HEARTBEAT_FLAG_FINAL clear (see its own doc comment, tickle.h), forcing each one to
// reply with an ACKNACK regardless of gap state. That reply is what actually advances this
// Publisher's own peer_acks[] (process_acknack(), tickle.c) - this function only solicits
// it, asynchronously; the caller polls tt_Publisher_is_acked_by_all_peers() afterward to see the answer, same
// "encode/send now, observe the effect later via already-existing state" split every other
// RELIABLE mechanism in this file already uses. No-op (tt_RET_OK, nothing to solicit) if peers[]
// is currently empty. Returns tt_RET_INVALID_ARGUMENT if pub->reliable_cache is still NULL (best-
// effort has no ack state to solicit in the first place) or pub->reliable_cache is empty (nothing
// published yet - same "nothing retained yet" case send_heartbeat()/send_initial_heartbeat()
// already skip, but reported back here rather than silently doing nothing, since unlike those two
// this isn't on a schedule that will just try again next period).
tt_ret_t tt_Publisher_request_ack(struct tt_Publisher* pub);

// Asserts a MANUAL_BY_TOPIC Publisher's liveliness without publishing (rmw_tickle/LIVELINESS_PLAN.md): a
// HEARTBEAT with tt_HEARTBEAT_FLAG_LIVELINESS, broadcast, which every receiver takes as this writer's sign of
// life. A publish asserts it too, so nothing is sent within liveliness_lease_duration_ns /
// tt_LIVELINESS_LEASE_DIVISOR of the
// last publish or assertion; nothing either for a Publisher with no lease. tt_RET_INVALID_ARGUMENT for a
// NULL or unregistered Publisher.
tt_ret_t tt_Publisher_assert_liveliness(struct tt_Publisher* pub);

// True once every currently-matched peer (peers[]) has acknowledged seq_no - i.e. each one's own
// tt_PeerAck.ack_seq_no is strictly greater than it ("every seq_no below this was received", struct
// tt_AckNackHeader's own doc comment). No matched peers at all is vacuously true, matching
// tt_Publisher_request_ack()'s own "nothing to solicit" no-op. A peer that has never sent an
// ACKNACK counts as not having acknowledged anything.
//
// The supported way to read peer_acks[] from outside core (rmw_tickle's own
// rmw_publisher_wait_for_all_acked() is built on this): the table is keyed by context_id, not
// index-aligned with peers[], so a caller must not pair the two arrays by index.
bool tt_Publisher_is_acked_by_all_peers(const struct tt_Publisher* pub, uint32_t seq_no);

// Phase 2 (rmw_tickle/PLAN.md) - the largest number of unacknowledged samples this Publisher may
// safely hold: the narrowest RELIABLE tracking window across its currently-matched Subscribers
// (each announced in tt_UpdateEntity.tracking_words), in samples. A Subscriber cannot ask about a
// gap older than its own window, so anything beyond this is unrecoverable however deep the
// Publisher's own cache is - which is what Phase 3's KEEP_ALL blocking bound has to be.
//
// tt_RELIABLE_BITMAP_BITS (the protocol default) when nothing is matched yet, or when a matched
// Subscriber announced no window of its own. A Subscriber matching later with a *narrower* window
// lowers this; it never shrinks what the Publisher has already retained - the cache keeps its
// depth, only the blocking bound moves.
uint32_t tt_Publisher_unacked_bound(const struct tt_Publisher* pub);

// Phase 3 (rmw_tickle/PLAN.md) - whether the next tt_Publisher_publish() would be accepted rather
// than refused with tt_RET_WOULD_BLOCK. Always true for a Publisher that isn't in KEEP_ALL mode (or
// has no reliable_cache), which is the default - KEEP_LAST never refuses a write.
//
// The polling half of the pair tt_Publisher.writable_callback is the notification half of; a caller
// may use either or both. Reads the same state publish() itself checks, so "writable now" is only a
// snapshot: on a single-threaded node nothing can change it between this call and the publish, but
// nothing stops a *later* Subscriber matching and lowering the bound.
bool tt_Publisher_writable(const struct tt_Publisher* pub);

// The lowest cumulative ack across every currently-matched peer - "every seq_no below this has
// been acknowledged by all of them". 0 when no peer is matched, or when any matched peer has yet
// to send its first ACKNACK (tt_PeerAck.ack_seq_no's own "unknown" value), so a caller measuring
// how far ahead it has run must treat 0 as "nothing confirmed yet", not "confirmed up to 0".
//
// The supported way to read peer_acks[] from outside core, alongside
// tt_Publisher_is_acked_by_all_peers() above: that table is keyed by context_id, not index-aligned
// with peers[], so a caller must not pair the two arrays by index.
uint32_t tt_Publisher_min_acked_seq_no(const struct tt_Publisher* pub);

// The ACKNACK retry interval this library was built with (2026-09-25): tt_RELIABLE_DEADLINE when set,
// else tt_RELIABLE_RETRY_INTERVAL - 0 meaning dynamic. A function rather than the macro because it
// answers for how libtickle itself was compiled, which a caller's own view of config.h cannot: an
// application built with one -D against a library built with another would otherwise report the
// mode it asked for, not the mode it ran.
uint64_t tt_reliable_retry_interval_configured(void);

// Arms (or re-arms, or disables with period_ns == 0) pub's own periodic ACK solicitation - see
// struct tt_Publisher.ack_solicit_period_ns's own doc comment (tickle.h) for what it's for and how
// it differs from tt_Publisher_set_heartbeat_period() above. Same "active scheduler operation, no
// passive-field equivalent" reasoning as that function - call this any time after tt_Context_create_
// publisher() returns, once pub->reliable_cache is already set. Once armed, each tick simply calls
// tt_Publisher_request_ack() on this Publisher's own behalf (its own return value is not
// surfaced - a transient "nothing to solicit yet" is expected during normal periodic operation,
// not an error). Returns tt_RET_INVALID_ARGUMENT if pub->reliable_cache is still NULL (period_ns
// == 0 is always accepted regardless, since disabling never needs a cache).
tt_ret_t tt_Publisher_set_ack_solicit_period(struct tt_Publisher* pub, uint64_t period_ns);

struct tt_Subscriber;

// Milestone 47 - one remote Publisher's own reliable-ack tracking state, the real WriterProxy
// equivalent this package was missing (real RTPS keeps exactly this, per matched Writer GUID).
// Before this milestone, struct tt_Subscriber held these as single flat fields instead of a
// table, an explicitly documented limitation ("one Publisher per reliable Subscriber") - two
// Publishers on one topic (two different tt_Context launches, or even two local Publishers sharing a
// name, Milestone 35) would silently interleave their independent seq_no streams through one
// shared watermark. Keyed by (context_id, entity_id) - see struct tt_Endpoint.entity_id's own doc
// comment - not just context_id, so two Publisher *instances* on the very same remote node are also
// tracked independently. context_id doubles as the "slot occupied" flag, the same convention struct
// tt_Peer already uses. Fixed capacity (tt_MAX_PEER_COUNT, embedded in struct tt_Subscriber below,
// no malloc) - a table this full already means more matched reliable Publishers than this build
// is sized for; find_or_create_writer_proxy() (tickle.c) silently drops a writer past that count,
// the same "silently drop past a fixed table's capacity" convention forget_peer()/upsert_peer()
// already use elsewhere in this file.
// LIMITATION - where a Subscriber's sequence numbering begins, and what is unreachable before it.
//
// A WriterProxy is created by the first DATA (or Heartbeat) this Subscriber actually receives from
// that writer, and its ack_seq_no baseline is that sample. A sequence number it never saw is not
// merely lost to it: it is unknown to it. Nothing requests such a sample, no gap is ever recorded
// for it, and every recovery counter on both sides reads zero, because from the Subscriber's point
// of view the stream simply began where it began.
//
// This can bite even when the Publisher already considers the peer matched - matching is not
// symmetric here. A Publisher claims its ack state on hearing the Subscriber's announce and may
// start publishing immediately, while the Subscriber only begins tracking when a packet from that
// writer survives the network. DDS differs: a sample written after a reader matches is owed to that
// reader. Here it is owed only from the first one that arrives.
//
// Under KEEP_ALL this is the one window where the zero-loss guarantee does not hold, and it is the
// only loss that remained once everything else was fixed. Measured on the HIL rig (rmw_tickle/
// PLAN.md Phase 3 step 4, 10-second runs at ~900K samples): 0 samples on a healthy link and at 20%
// injected loss, 0-5 per run at 50%, in every case exactly the samples published before the
// Subscriber's first arrival and never any later one. TRANSIENT_LOCAL is the policy that closes it
// - a durable writer replays its retained range to a late joiner (deliver_durability_backlog()) -
// so a VOLATILE writer behaves here as DDS's own VOLATILE does about history, just with a wider
// window for it to apply in. Documented rather than changed, at the user's own direction
// (2026-09-23): closing it means establishing the baseline at match time via discovery rather than
// from the first packet, which reopens Milestone 60's VOLATILE semantics for a handful of samples.
// One ACKNACK request a reliable Subscriber remembers (tt_WriterProxy.requests): when it went out, and the first and
// last seq_no it named. sent_ns 0 = an empty slot.
struct tt_RepairRequest {
    uint64_t sent_ns;
    uint32_t first_seq_no;
    uint32_t last_seq_no;
};

struct tt_WriterProxy {
    uint8_t context_id;
    uint32_t entity_id;
    // Samples of this writer the segment drain passed over as superseded since the last one delivered from it
    // (tt_Subscriber.keep_last_depth). Handed to the Subscriber at the next delivery as delivering_superseded.
    uint32_t superseded_pending;
    // Address an outstanding-gap ACKNACK retry (acknack_retry(), tickle.c) resends to - the most
    // recent reliable DATA/Heartbeat sender for this specific writer, since a scheduled retry
    // fires outside process_packet()'s own call stack and so no longer has that packet's own
    // sender_ip/sender_port at hand.
    uint32_t sender_ip;
    uint16_t sender_port;
    // Cumulative-ack watermark: the next wire seq_no not yet confirmed delivered to this
    // Subscriber's own `callback` for this specific Publisher - every seq_no < ack_seq_no has
    // been. Set to 1 when this entry is first claimed (find_or_create_writer_proxy(), tickle.c)
    // since a Publisher's own seq_no starts posting from 1, never 0 (tt_Publisher_publish()'s
    // data_header->seq_no = pub->seq_no + 1).
    uint32_t ack_seq_no;
    // bit j set: sample (ack_seq_no + j) has already been received out of order, ahead of the
    // cumulative watermark - matches tt_AckNackHeader's own "bit j: seq_no + j" wire convention
    // exactly (bit 0 is ack_seq_no itself, always 0 here since ack_seq_no only ever advances once
    // confirmed received - see update_reliable_ack()'s own comment on why that still needs its
    // own explicit realigning shift, not just a plain compare), so building the wire "please
    // resend" bitmap is a straight ~received_bitmap, no additional offset. tt_RELIABLE_BITMAP_
    // WORDS-word array (config.h), word 0 holding bits 0-63, word 1 bits 64-127, and so on - widened
    // from a single bare uint64_t (rmw_tickle/PLAN.md's "TickLE-native performance" plan) since the
    // old 64-bit width was the real bottleneck behind RELIABLE's own measured tc-loss recovery gap
    // vs. FastDDS/CycloneDDS (COMPARISON.md §3/§6 item 7), not the Publisher's own retained-cache
    // depth (Milestone 61 already ruled that out on real HIL). tickle.c's own small, fixed set of
    // bitmap_*() helpers (next to highest_received_bit()) are the only code that manipulates this
    // array directly - every call site here goes through one of them, not raw per-word arithmetic,
    // matching the "stay O(word-count), not O(bit-count)" discipline tt_RELIABLE_BITMAP_WORDS's own
    // doc comment (config.h) explains. Honest, not a guaranteed full fix: raises the tolerable gap
    // ~4x (64 -> 256 bits), likely closing most or all of the measured residual loss at TickLE's own
    // real ACKNACK RTT on the `tickle-hil` rig - but the real link's own RTT, not this window alone,
    // sets the actual ceiling; a slower/lossier link could still exceed even a 256-bit window.
    // Phase 2 (rmw_tickle/PLAN.md) - points into this Subscriber's own tracking storage (its
    // builtin_tracking[], or the caller-provided wider buffer - see tt_Subscriber.tracking_bitmaps),
    // tracking_words() words wide. A pointer rather than an embedded array so the window can be
    // sized per Subscriber: core stays embedded-first at tt_RELIABLE_BITMAP_BITS, a Linux-class
    // caller opts into more. Set when this slot is claimed (find_or_create_writer_proxy()).
    uint64_t* received_bitmap;
    // How many ACKNACK retries have been sent for the *current* outstanding gap against this
    // writer - reset to 0 when a new gap first opens, capped at tt_RELIABLE_RETRY (mirrors
    // call_retry()'s own client->service->call_retry_count check) before this Subscriber gives up
    // on that sample.
    uint8_t retry;
    // Whether acknack_retry() (tickle.c) currently has a tt_Context_schedule() entry pending for
    // this specific writer proxy (scheduled with `this` entry's own address as its param, so
    // several writers' independent retry timers never collide - see acknack_retry()'s own doc
    // comment) - mirrors struct tt_Client.cache's own "is a retry timer armed right now" role,
    // needed so a burst of DATA packets while a gap is open doesn't schedule a new timer per
    // packet.
    bool acknack_scheduled;
    // 0 (this entry's own creation default): no Heartbeat seen yet from this writer - the
    // Subscriber falls back to inferring gaps purely from received_bitmap, today's only behavior
    // (a Publisher that never calls tt_Publisher_set_heartbeat_period() never sends one, so this
    // stays 0 forever and nothing here changes for it). Non-zero: the highest seq_no the most
    // recent struct tt_HeartbeatHeader from this writer claimed the Publisher has published -
    // used by tickle.c's own highest_relevant_bit() to widen send_acknack()'s/maybe_arm_acknack_
    // retry()'s own "how far ahead does anything need attention" reach beyond received_bitmap's
    // own highest *confirmed* bit alone, since a Heartbeat can reveal the Subscriber is behind
    // even with zero out-of-order DATA arrivals yet (received_bitmap is blind to that on its own).
    uint32_t heartbeat_last_seq_no;
    // Phase 3 (rmw_tickle/PLAN.md) - this writer announced tt_UPDATE_QOS_KEEP_ALL, i.e. it will
    // block rather than evict an unacknowledged sample, so this Subscriber must not give up on a
    // gap either: acknack_retry()'s own tt_RELIABLE_RETRY budget is disabled for this writer alone.
    // Per writer, not per Subscriber - one Subscriber can be matched to a KEEP_ALL writer and a
    // KEEP_LAST one at the same time. Cached here from the writer's own announce rather than looked
    // up in the discovery table per DATA, which is the hot path.
    //
    // Three states, not two, and tt_WRITER_KEEP_ALL_UNKNOWN is the zero-init default deliberately:
    // a WriterProxy is claimed on the first DATA from a writer, which can arrive before that
    // writer's own announce has been seen (always, if no discovery table is attached - it's
    // opt-in). Treating "not known yet" as KEEP_LAST meant abandoning samples under a policy the
    // writer never asked for. Measured (rmw_tickle/PLAN.md Phase 3 step 4): at 20% injected loss a
    // KEEP_ALL stream lost exactly as many samples as the Subscriber gave up on - retry_giveups ==
    // lost, with the Publisher's own null_evicted and publish_refused both zero - all of it inside
    // the first announce interval, and reported as ordinary transport loss rather than a refusal.
    enum tt_WriterKeepAll keep_all;
    // Whether this reader has told the writer it exists with a pure acknowledgement (ack_writer_presence(),
    // tickle.c): once per writer, whichever of its first DATA or its KEEP_ALL announce comes first.
    bool presence_acked;
    // RELIABLE in-order delivery - the next sequence number this writer's samples may be released
    // from the reorder buffer at. Only ever moves forward, and drain_reorder() visits each value
    // once, which is what makes draining O(released) overall rather than a scan of the whole
    // buffer per sample. Kept equal to ack_seq_no at every point a sample can be held, so a held
    // sample always lies in [reorder_cursor, reorder_cursor + window) - the range drain walks.
    uint32_t reorder_cursor;
    // RELIABLE strict order - the highest sequence number from this writer handed to the
    // application, 0 before the first. Distinct from ack_seq_no on purpose: the watermark moves
    // past an abandoned range WITHOUT delivering it (jump_ack_baseline()), so after a jump the two
    // diverge by exactly the samples that must not be delivered late. Anything at or below this is
    // discarded rather than handed up - the user's ruling (2026-09-24) that RELIABLE is strictly
    // ordered, matching DDS once a gap has been declared lost.
    uint32_t highest_delivered;
    // Phase 3 - tt_get_ns() of the last "still waiting" warning for this writer, so a stuck
    // KEEP_ALL gap is visible in a log at a fixed cadence rather than per retry or never.
    uint64_t stuck_warned_ns;
#if tt_LARGE_SAMPLES
    // tt_get_ns() of the last fragment of a large sample recorded from this writer (large-message stage 2). While they
    // keep arriving the writer is still sending, and may not answer an ACKNACK before the send ends: a retry then
    // does not count against a KEEP_LAST writer's give-up budget (acknack_retry()).
    uint64_t large_arrival_ns;
#endif
    // Request-to-recovery estimate for the dynamic ACKNACK retry interval (tt_RELIABLE_RETRY_INTERVAL
    // 0, config.h), RFC 6298-style, in nanoseconds. Maintained in every build - so the estimate can
    // be read, and tested, whether or not it is steering the timer. 0/0 = no sample yet.
    //
    // The probe is the watermark sample: ack_seq_no is named in every ACKNACK that requests
    // anything, so it is always a sample that was definitely asked for. The time runs from the
    // FIRST ACKNACK that named it to its arrival - a retry of the same request keeps the first
    // timestamp. There is no Karn's rule here, and none is needed rather than none is used: Karn
    // exists because a TCP retransmission is indistinguishable from the original segment, so an ACK
    // cannot be attributed to either send. Here the ACKNACK names the sequence number and the DATA
    // that comes back carries it, so the timing is of a named sample and nothing is ambiguous.
    // (Discarding repeated-request samples would also have starved the estimate on exactly the
    // links where the timer fires several times per recovery.) Timing from the first request
    // biases upward, which errs toward fewer premature retries - the direction that cannot
    // re-create the storm. probe_ns 0 = no probe outstanding.
    //
    // Read in a FIXED-interval build, the estimate is an upper bound on what dynamic mode settles at,
    // not a prediction of it: a shorter interval makes recoveries complete sooner, which shortens the
    // estimate again. Measured on the rig: the fixed 1ms build implied 1.23ms, dynamic settled at
    // 0.35ms.
    uint32_t recovery_srtt_ns;
    uint32_t recovery_rttvar_ns;
    uint32_t probe_seq_no;
    uint64_t probe_ns;
    // This reader's latest ACKNACK requests to this writer (2026-10-07): when each went out and the first and last
    // seq_no it named, a ring of tt_RELIABLE_REQUEST_HISTORY (config.h), request_next the slot the next one takes,
    // sent_ns 0 an empty slot. answer_ack_request() (tickle.c) leaves out what a request younger than the repair
    // transit time named - naming a repair in flight had the writer send it twice: c6, 5% loss, +11% wire bytes a
    // sample with f3451cd8, +6.5% with an srtt window (d603d369), because a repair queued behind the writer's data
    // takes longer than srtt.
    struct tt_RepairRequest requests[tt_RELIABLE_REQUEST_HISTORY];
    uint8_t request_next;
    // Repair transit, RFC 6298-style like recovery_srtt_ns but timed from the OLDEST remembered request naming a
    // repaired sample to that repair's arrival - the time a requested repair actually takes, writer queue included,
    // and not the whole recovery (which counts lost repairs and a bounded reader's declines too). 0/0 = no sample.
    uint32_t transit_srtt_ns;
    uint32_t transit_rttvar_ns;
    // Back-pointer to the owning Subscriber - this entry's own stable address (never moves once
    // claimed; embedded in struct tt_Subscriber.writers[], which lives as long as the Subscriber
    // itself) is what acknack_retry() is scheduled against (tt_Context_schedule(..., acknack_retry,
    // proxy)), so the callback needs a way back to sub->node/sub->endpoint - same {owner, self}
    // pattern struct server_cache_clean_config already uses for an identical reason.
    struct tt_Subscriber* sub;
};

// g13 (rmw_tickle/RMW_GAPS_PLAN.md) - flow control, the reader's half. Consulted for one arriving
// sample before anything at all is recorded about it; return false to decline it, meaning this
// Subscriber has nowhere to put it right now. `seq_no` is the sample's full tracking sequence
// number rather than the 16-bit one the delivery callback is handed, because a decision about which
// sample to refuse has to name the same sample the ACKNACK machinery does.
//
// Declining is not dropping, and the difference is the whole point. A declined sample is never
// received: no bit is set, the ack watermark does not move, and nothing this Subscriber sends
// afterwards claims it. A RELIABLE writer therefore still holds it, and the ordinary gap exchange
// fetches it once this Subscriber starts accepting again - which is what lets a bounded reader
// refuse to overwrite an unread sample without losing one (DDS HISTORY KEEP_ALL). A BEST_EFFORT
// stream has no retransmission, so there a decline IS a drop; accept_declines counts both, and
// which one it was is decided by the Subscriber's own `reliable`.
typedef bool (*tt_SUBSCRIBER_ACCEPT_CALLBACK)(struct tt_Subscriber* subscriber, uint32_t seq_no, void* param);

// seq_no is the sample's sequence number from its writer: increasing, but not contiguous. Every datagram a
// writer sends takes its own seq_no (DATAFRAG_PLAN.md section 13), so a sample that went as k fragments is
// named by its first datagram's and the next sample's is k higher. Contiguous only while nothing fragments.
// It says which sample this is, not how many were missed.
typedef void (*tt_SUBSCRIBER_CALLBACK)(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                       struct tt_Data* data);

// One held sample in a RELIABLE Subscriber's reorder buffer (2026-09-24).
//
// A RELIABLE reader must deliver in order, so a sample that arrives ahead of a gap has to be held
// until the gap fills. This is the header of one such held sample; its wire payload follows
// immediately after it inside the same slot, and the stride between slots is
// tt_Subscriber.reorder_slot_bytes.
//
// The payload is kept as received rather than decoded, with its own endianness flag, because
// decoding needs the topic's caller-owned scratch and there is exactly one of those - it belongs
// to whichever sample is being delivered right now, not to a queue of samples waiting their turn.
struct tt_ReorderSlot {
    uint64_t timestamp;
    uint32_t seq_no;
    uint32_t entity_id;
    uint16_t length;
    uint8_t context_id;
    bool occupied;
    bool is_native;
    // Which socket the sample arrived on. Recorded here because by the time it is released the
    // packet being processed is a different one - a later DATA, a Heartbeat, or none at all when
    // the retry timer releases it - and attributing it to that trigger's socket made
    // via_socket_flips count the trigger, not the stream.
    bool via_data_port;
    // A fragment's place in its sample (DATAFRAG_PLAN.md section 13): frag_count is 0 for a whole
    // sample, else how many datagrams its sample has, and frag_index which one this is.
    uint8_t frag_index;
    uint8_t frag_count;
};

// Bytes one reorder slot needs for a payload of `payload_bytes`, rounded UP to a multiple of 8.
// Callers size their storage as tt_REORDER_SLOT_SIZE(largest payload) * slots and pass the same
// value as reorder_slot_bytes.
//
// The rounding is here, on the caller's side, so that it only ever makes the allocation larger.
// Core rounds the stride DOWN (see reorder_stride(), tickle.c) so that it can never address past
// what the caller allocated - a caller who uses this macro loses nothing to that, and one who does
// not loses at most 7 bytes of payload per slot rather than getting memory corruption.
#define tt_REORDER_SLOT_SIZE(payload_bytes) \
    ((sizeof(struct tt_ReorderSlot) + (payload_bytes) + sizeof(uint64_t) - 1U) & ~(sizeof(uint64_t) - 1U))

struct tt_Subscriber { // extends endpoint
    struct tt_Endpoint endpoint;
    struct tt_Context* node;
    struct tt_Topic* topic;
    tt_SUBSCRIBER_CALLBACK callback;

    // g13 (rmw_tickle/RMW_GAPS_PLAN.md) - optional flow control, consulted before this sample is
    // recorded or delivered (see tt_SUBSCRIBER_ACCEPT_CALLBACK for what declining means). NULL
    // (tt_Context_create_subscriber()'s own default) accepts everything, which is exactly what
    // every caller written before this hook existed already did.
    //
    // THE FAILURE MODE IT BUYS, said here because whoever meets it will be reading this field: a
    // Subscriber that keeps declining stalls its RELIABLE writers. That is not a defect in the
    // hook, it is what a history that refuses to destroy an unread sample means - the writer is
    // held back instead of the reader losing data - but it presents as a publisher that has stopped
    // making progress, which is easy to mistake for a hang. accept_declines and its throttled
    // warning are there so it reads as a stalled reader rather than a mystery.
    tt_SUBSCRIBER_ACCEPT_CALLBACK accept_callback;
    void* accept_callback_param;
    // Diagnostic counter, not protocol state: how many arriving samples accept_callback has
    // declined. Same reasoning as rxo_drops just below - the decline is otherwise silent.
    uint32_t accept_declines;

    // KEEP_LAST depth this Subscriber's history keeps, as the segment drain may rely on it: when the ring holds more
    // than this many complete samples of one writer, the older ones would only be delivered to be overwritten, and
    // the drain passes over them without reading them (counted in `superseded`, and in the context's
    // rx_shm_skipped_superseded). 0 - the default - never skips anything, and is what KEEP_ALL must be.
    uint16_t keep_last_depth;
    uint32_t superseded;
    // Superseded samples not yet handed over (the sum of the writers' superseded_pending), and - valid inside the
    // callback only, like tt_Subscriber_delivering_writer() - how many samples of the delivering writer were passed
    // over just before this one. A layer that counts lost samples from sequence gaps subtracts it: those samples
    // were received and replaced by newer ones, which KEEP_LAST allows, not lost.
    uint32_t superseded_pending;
    uint32_t delivering_superseded;

    // QoS roadmap #5 (RELIABILITY) / Phase 2 (rmw_tickle/PLAN.md) - how wide a gap this Subscriber
    // can track per matched Publisher, i.e. how far ahead of its own oldest missing sample it may
    // keep receiving before it has to give up on that sample (update_reliable_ack()'s own
    // jump_ack_baseline()). At TickLE's own max rate a 256-sample window lasts ~1.35ms, shorter
    // than one retry interval plus a round trip, which is what leaves an occasional burst
    // unrecoverable; a wider window is what fixes that.
    //
    // Sizing rule, measured on real HIL (Phase 2, 2026-09-23) and not a bigger-is-better knob:
    // **keep the window at or below the matched Publisher's own retained depth**
    // (struct tt_ReliableCache.depth). Tracking further back than the Publisher still holds cannot
    // recover anything - those samples are already evicted, so each one is answered with an
    // eviction Heartbeat and skipped - while a Subscriber that keeps waiting on them recovers
    // *less* than a narrower window would. Measured against a depth-1024 Publisher at maximum
    // rate: a 1024-sample window lost nothing across 6 runs, a 4096-sample one lost 191-368 per
    // run, and the loss equalled the Publisher's own evicted-request count exactly. A Publisher
    // logs a warning when a matching Subscriber announces a window deeper than it retains.
    //
    // NULL/0 (tt_Context_create_subscriber()'s own default) uses builtin_tracking[] below,
    // tt_RELIABLE_BITMAP_BITS wide - the embedded-first default (PLAN.md's Project Goal 1): a
    // microcontroller nowhere near that rate shouldn't pay for a window it can't use. A Linux-class
    // caller (rmw_tickle, Goal 5; the perf_hil examples via their own flag) hands a wider
    // caller-owned buffer instead: tt_MAX_PEER_COUNT * tracking_words words, i.e. one window per
    // simultaneously-tracked remote Publisher, with tracking_words <= tt_RELIABLE_BITMAP_MAX_WORDS
    // (config.h). Set both fields together, before the first sample arrives; core clamps anything
    // out of range back to the builtin default rather than trusting it.
    uint64_t* tracking_bitmaps;
    uint16_t tracking_words;

    // transcation
    uint16_t seq_no;

    // QoS roadmap #5 (RELIABILITY/RELIABLE) - false (tt_Context_create_subscriber()'s own default):
    // best-effort, today's only behavior, process_data() doesn't touch writers[] at all. true:
    // process_data() tracks delivery per matched Publisher and sends ACKNACK back to the sending
    // Publisher on a gap - set directly any time after tt_Context_create_subscriber() returns, same
    // convention as tt_Publisher.batch/.reliable_cache.
    bool reliable;

    // Milestone 47 - one entry per currently-tracked remote Publisher (see struct tt_WriterProxy's
    // own doc comment for the full rationale/history) - replaces this struct's own single flat
    // ack_seq_no/received_bitmap/reliable_sender_*/reliable_retry/reliable_acknack_scheduled/
    // reliable_heartbeat_last_seq_no fields it used to carry directly. All-empty (every slot's
    // context_id == tt_CONTEXT_ID_INVALID) by default - tt_Context_create_subscriber() zeroes this the same
    // way it zeroes/invalidates every other fixed table in this file - entries are claimed lazily,
    // one per distinct (context_id, entity_id) actually heard from, via find_or_create_writer_proxy()
    // (tickle.c).
    struct tt_WriterProxy writers[tt_MAX_PEER_COUNT];
    // The default per-writer tracking windows, used unless tracking_bitmaps above points somewhere
    // wider - exactly the storage each writers[] entry used to embed directly.
    uint64_t builtin_tracking[tt_MAX_PEER_COUNT * tt_RELIABLE_BITMAP_WORDS];

    // QoS roadmap #1 (RxO matching, Milestone 31, rmw_tickle/PLAN.md) - false (tt_Context_create_
    // subscriber()'s own default): this Subscriber accepts a VOLATILE Publisher, today's only
    // behavior. true: requires TRANSIENT_LOCAL - a discovered remote Publisher on this topic whose
    // own announced tt_UpdateEntity.qos doesn't offer tt_UPDATE_QOS_DURABLE is treated as
    // incompatible (process_data()'s own subscriber_incompatible_with_writer() check) and its
    // DATA is silently never delivered to `callback`, matching real DDS's own "an incompatible
    // pair simply never connects" semantics rather than TickLE's previous "everything matches,
    // durability is just an extra a VOLATILE reader happens to also receive if offered" behavior.
    // Mirrors tt_Publisher.durable's own "offered" half - this is the "requested" half, which
    // (unlike reliable just above) didn't exist on this struct at all before this milestone, since
    // backlog delivery itself was always purely a Publisher-side decision with no reader opt-out.
    bool durable;

    // QoS roadmap #2 (DEADLINE) RxO, Milestone 49 - see tt_Publisher.deadline_duration_ns's own
    // doc comment (tickle.h) for the full reasoning, mirrored here as the "requested" half: 0
    // (tt_Context_create_subscriber()'s own default) means no DEADLINE required.
    uint64_t deadline_duration_ns;
    // QoS roadmap #3 (LIVELINESS) RxO, Milestone 49 - see tt_Publisher.liveliness_lease_duration_ns's
    // own doc comment, mirrored here as the "requested" half: 0 means no specific lease requirement.
    uint64_t liveliness_lease_duration_ns;
    // QoS roadmap #3 (LIVELINESS) RxO, Milestone 49 - see tt_Publisher.liveliness_manual's own doc
    // comment, mirrored here as the "requested" half: false (tt_Context_create_subscriber()'s own
    // default) means this Subscriber accepts AUTOMATIC liveliness; true means it requires
    // MANUAL_BY_TOPIC specifically.
    bool liveliness_manual;
    // Diagnostic counter, not protocol state: how many arriving samples this Subscriber has
    // dropped because RxO matching found the Publisher incompatible. Exists because that drop is
    // otherwise completely silent - deliver_data_to_subscriber() returns, no callback, no ACKNACK,
    // no log - so a Subscriber that is discovered, matched in the graph, and receiving nothing
    // looks identical to one nobody is publishing to. That shape (total, silent, no error) is
    // exactly the rmw_tickle zero-delivery failure under investigation on 2026-09-23, and it was
    // impossible to tell the two apart from outside.
    uint32_t rxo_drops;

    // Delivery-order diagnostics, not protocol state (2026-09-24). What the application actually
    // saw, in the order it saw it - which no other record in the system holds.
    //
    // Why: rmw_tickle's benchmark aborts with "Data consistency violated. Received sample with not
    // strictly older timestamp", and every instrument we have looks at one side or the other of
    // that sentence. A packet capture shows what arrived on the wire; the abort message shows that
    // one comparison failed. Neither shows the sequence of samples this Subscriber handed up, so
    // "the wire was in order and delivery reordered it" and "the wire was already out of order"
    // are indistinguishable from outside. These fields make that difference visible, and if a
    // capture and this disagree, the disagreement is itself the finding.
    //
    // The three are deliberately separate, because they fail for different reasons:
    //   - writer_switches: the sample came from a different (node, entity) than the previous one.
    //     seq_no is per-writer, so it is NOT compared across a switch - counting that as disorder
    //     would report every legitimate change of speaker. This is also the counter that would
    //     expose a stale writer from an earlier run still being delivered to.
    //   - out_of_order: same writer, seq_no not strictly greater. Best-effort delivery has no
    //     per-writer de-duplication at all (update_reliable_ack() is a no-op unless `reliable`),
    //     so nothing upstream of here would have caught it.
    //   - timestamp_not_newer: the abort's own predicate, checked regardless of writer, because
    //     that is how the application checks it.
    // last_via_data_port records which socket the offending sample came in on, since a node reads
    // its well-known and data sockets alternately (hal_linux.c) and a reordering across that
    // alternation would show up here as a flip.
    uint32_t delivered;
    uint32_t writer_switches;
    uint32_t out_of_order;
    uint32_t timestamp_not_newer;
    // How many times the delivered stream changed socket - the covariate that actually matters,
    // and not the same thing as how much of the stream was broadcast (2026-09-24).
    //
    // The objection this answers: the broadcast path is a minority everywhere (the most
    // broadcast-heavy run measured still sent ~72% of its samples as unicast), so how could it
    // produce a failure in a third of runs? Because the abort needs ONE comparison to go
    // backwards, not a majority of them. A reader that interleaves two sockets can only misorder
    // samples at a transition between them, so the number of chances a run gets is the number of
    // transitions - not the volume on either side. A run that is 10% broadcast finely interleaved
    // has far more of them than a run that is 100% broadcast in one contiguous block, and by
    // volume those two are ranked the wrong way round.
    //
    // So this is the quantity to correlate a failure against. If failures track flips and not
    // broadcast volume, that is the interleaving hypothesis surviving a test that volume alone
    // would have failed it on.
    uint32_t via_socket_flips;
    // BEST_EFFORT ordering: samples dropped because they were no newer than the last delivered
    // from the same writer. Counted rather than silent, because this drop is a deliberate policy
    // and an application seeing a gap deserves to be able to tell a discarded reorder from a
    // sample that never arrived - which is the same reason COMPARISON.md reports raw and
    // post-match loss as separate columns rather than one number nobody can take apart.
    uint32_t out_of_order_discarded;

    // RELIABLE in-order delivery (2026-09-24) - caller-owned storage for samples that arrived
    // ahead of a gap and must wait for it.
    //
    // NULL (the default) does not mean "deliver out of order". It means this Subscriber waits
    // without holding: a sample ahead of the gap is not delivered and is not recorded as
    // received, so the ordinary ACKNACK exchange fetches it again once the gap has filled.
    // Ordering is correct either way - what the buffer buys is not having to re-request
    // everything that arrived after a single lost sample, which is the common case under loss
    // and the one COMPARISON.md section 3b measures.
    //
    // There is deliberately no builtin default, unlike tracking_bitmaps' own builtin_tracking[].
    // A useful builtin would have to hold whole payloads - 8 slots at tt_MAX_BUFFER_LENGTH is
    // ~11.8KB against a 920-byte tt_Subscriber - and an embedded-first library (PLAN.md's Project
    // Goal 1) cannot put that in every Subscriber for a case a microcontroller stream may never
    // hit. A Linux-class caller (rmw_tickle, the perf_hil examples) hands storage in; a small
    // target leaves it NULL and pays in retransmissions instead of RAM.
    //
    // Set all three together before the first sample arrives. reorder_slot_bytes is the stride
    // and must be at least tt_REORDER_SLOT_SIZE(largest payload this topic can carry); a payload
    // too big for the stride is treated exactly like a full buffer.
    //
    // Not optional for a RELIABLE Subscriber of a topic whose samples fragment (DATAFRAG_PLAN.md
    // section 13): each fragment is held in a slot, one datagram per slot, until its whole sample is in
    // order, and is acknowledged only once it is held. Without room it is left unacknowledged and asked
    // for again, never lost - but with no buffer at all that is every time. A slot needs a fragment's
    // payload, at most tt_CONTROL_MAX_LENGTH, and the buffer at least one window of datagrams.
    // uint64_t*, not uint8_t*, and for the same reason tracking_bitmaps is: a slot header starts
    // with a uint64_t timestamp, so the storage has to be 8-byte aligned. A uint8_t array gives no
    // such guarantee - it would be undefined behaviour everywhere and an alignment fault on the
    // Arm targets this library exists for. The type makes the caller's declaration carry it.
    uint64_t* reorder_storage;
    uint16_t reorder_slots;
    // Stride in BYTES, not in uint64_t, and rounded up to a multiple of 8 internally so slot n
    // stays aligned however the caller sized it.
    uint16_t reorder_slot_bytes;
    // Diagnostics, not protocol state. reorder_overflow rising means the buffer is too small for
    // this stream's loss pattern and the Subscriber is paying for it in retransmissions - the one
    // number that says "make this bigger". reorder_abandoned counts samples given up on because
    // the gap in front of them was declared unrecoverable, which is loss, not disorder.
    // How many samples are held right now, maintained on store and release rather than counted by
    // walking the buffer. Counting by walking was both O(capacity) per sample and wrong: past the
    // end of a mis-sized buffer it counted neighbouring memory as held (see a3a1bea1).
    uint32_t reorder_held;
    uint32_t reorder_held_peak;
    uint32_t reorder_delivered;
    uint32_t reorder_overflow;
    uint32_t reorder_abandoned;
    // RELIABLE samples this Subscriber stopped waiting for without ever delivering them - the gap
    // itself, where reorder_abandoned above counts the samples held behind one (2026-09-25).
    // Counted in every build: before these existed, the only record outside tt_RELIABLE_STATS was
    // a throttled WARNING that undercounts by design, so a RELIABLE Subscriber could drop samples
    // while every production counter read zero - verified, not supposed.
    //   gap_abandoned - this side gave up while the Publisher may still hold the sample: the gap
    //                   fell out of the tracking window (jump_ack_baseline()), or ran out of
    //                   ACKNACK retries.
    //   gap_evicted   - the Publisher said it no longer holds it (an eviction Heartbeat), which
    //                   under KEEP_LAST is that writer's HISTORY working as specified.
    // Kept apart because they point at different fixes: a rising gap_abandoned says widen this
    // Subscriber's window or its retry budget, a rising gap_evicted says the writer's depth is too
    // shallow for the loss it sees.
    uint32_t gap_abandoned;
    uint32_t gap_evicted;
    uint32_t last_seq_no;
    uint32_t last_source;
    uint32_t last_entity_id;
    uint64_t last_timestamp;
    bool last_via_data_port;
    // Which writer the sample being handed to .callback came from, read through
    // tt_Subscriber_delivering_writer(). Meaningful ONLY for the duration of that call: outside it
    // they name whatever arrived last, which is not a question anyone is asking.
    //
    // Set at the two callback sites rather than read off last_source/last_entity_id above, which
    // hold the same values by the time the callback runs because record_delivery_order() assigns
    // them on its way out. That would work today and would go stale the moment anything moved or
    // conditionalised that call - and a gid that is confidently wrong is the defect this exists to
    // fix (rmw_tickle reported sixteen zero bytes for every sample until 2026-10-02). Two scalars
    // of duplication buys a dependency that is visible at the line that depends on it.
    uint8_t delivering_source;
    uint32_t delivering_entity_id;
#if tt_LARGE_SAMPLES
    // Core-owned: the context's large assemblies (struct tt_LargeAssembly) held for this RELIABLE Subscriber - while
    // any are, its drain looks for a large sample at each seq_no it walks (DESIGN.md section 8).
    uint16_t large_held;
#endif
};

typedef int32_t (*tt_DATA_ENCODE_SIZE)(struct tt_Data* data);
typedef int32_t (*tt_DATA_ENCODE)(struct tt_Data* data, uint8_t* payload, const uint32_t len);
// Optional zero-copy encode: instead of writing the CDR into a caller buffer, set *payload_out
// to a pointer to the already-serialized CDR bytes (native byte order) living in the caller's
// own tt_Data, and return their length. tt_Publisher_publish() then sends framing + that memory
// via one sendmsg() with no staging copy. Return -1 to decline (fall back to data_encode). Only
// definable for a topic whose in-memory layout already is its own native-endian wire form.
typedef int32_t (*tt_DATA_ENCODE_INPLACE)(struct tt_Data* data, const uint8_t** payload_out);
typedef int32_t (*tt_DATA_DECODE)(struct tt_Data* data, const uint8_t* payload, const uint32_t len,
                                  bool is_native_endian);
typedef void (*tt_DATA_FREE)(struct tt_Data* data);

// Optional zero-copy decode: instead of unpacking the wire payload into a caller-owned tt_Data
// (a full copy), return a tt_Data* that aliases `payload` directly - valid only for the duration
// of the subscriber callback, and never passed to data_free. Return NULL to fall back to
// data_decode's copy path (e.g. when the wire is byte-swapped, or the layout needs fixups a
// bare cast can't do). process_data() prefers this when it's set. Only worth defining for a
// topic whose wire layout already matches its in-memory struct in native byte order.
typedef struct tt_Data* (*tt_DATA_DECODE_INPLACE)(const uint8_t* payload, uint32_t len, bool is_native_endian);

struct tt_Topic {
    const char* name;
    uint32_t data_size;
    tt_DATA_ENCODE_SIZE data_encode_size;
    tt_DATA_ENCODE data_encode;
    tt_DATA_ENCODE_INPLACE data_encode_inplace; // optional, see typedef
    tt_DATA_DECODE data_decode;
    tt_DATA_DECODE_INPLACE data_decode_inplace; // optional, see typedef
    tt_DATA_FREE data_free;

    // QoS - reserved for a future reliable-delivery release (ACKNACK); ignored in this one, which
    // is best-effort only. Left in the struct so setting them now stays source-compatible later.
    uint16_t history_depth;
    uint32_t deadline_duration;
    uint32_t lifespan_duration;
};

uint32_t tt_hash_id(const char* type, const char* name);
struct tt_Header;
bool tt_is_native_endian(struct tt_Header* header);
bool tt_is_reverse_endian(struct tt_Header* header);

// Lifetime / ownership (applies to every tt_Context_create* below):
//   - The library never allocates or copies. Every struct you pass - the tt_Context, the
//     tt_Client/tt_Server/tt_Publisher/tt_Subscriber, its tt_Service or tt_Topic - and every
//     string (endpoint_name, service->name, topic->name) must stay valid and unmoved until the
//     matching tt_*_destroy() (and tt_Context_destroy() for the node). String literals are fine;
//     a stack buffer or one you free() is not.
//   - One tt_Context is single-threaded: all its calls (create/destroy/publish/call/poll) must come
//     from one thread. See DESIGN.md, "Concurrency".
//
// Returns tt_RET_OK on success. tt_Context_create() can also return tt_RET_IILEGAL_NODE_ID (address
// auto-detection found no usable id and none was set in _tt_CONFIG), tt_RET_IO_ERROR (socket
// bind), tt_RET_NO_SUCH_LINK (a configured link's broadcast address is owned by no local
// interface - worth retrying, since an interface brought up by DHCP or a network manager may
// simply not exist yet when a service starts; see that code's own comment in hal.h), or
// tt_RET_OUT_OF_SCHEDULE. The create_* helpers return tt_RET_OUT_OF_BUFFER /
// tt_RET_OUT_OF_SCHEDULE when the node's fixed endpoint table or scheduler is full.
tt_ret_t tt_Context_create(struct tt_Context* node);
tt_ret_t tt_Context_create_client(struct tt_Context* node, struct tt_Client* client, struct tt_Service* service,
                                  const char* endpoint_name, tt_CLIENT_CALLBACK callback);
tt_ret_t tt_Context_create_server(struct tt_Context* node, struct tt_Server* server, struct tt_Service* service,
                                  const char* endpoint_name, tt_SERVER_CALLBACK callback);

// Cache storage sized for this server's own service, in place of the inline default (config.h's
// tt_SERVER_CACHE_ENTRY_LENGTH, sized for any message). Call after tt_Context_create_server() - which resets
// the server to its inline storage - and before it has cached a response. The area holds
// tt_MAX_SERVER_CACHE_COUNT entries of cache_entry_length, each one already-encoded response kept for a
// retrying client. A response larger than an entry is still sent, just not cached, so a retry re-runs the
// callback - as it does once a cached response has expired (server_cache_lifetime(), tickle.c).
// The length must be a multiple of 8 and the area 8-byte aligned. A NULL area goes back to the inline one.
// tt_RET_INVALID_ARGUMENT for a misfit, tt_RET_ILLEGAL_STATUS if an entry is already cached.
// (It also took a deferred-response area until 2026-09-27; deferred responses are no longer copied.)
tt_ret_t tt_Server_set_storage(struct tt_Server* server, uint8_t* cache_storage, uint32_t cache_entry_length);
// The client's counterpart: where its one outstanding request is kept for retries, at least as
// large as the largest request it will send (a larger one is refused by tt_Client_call() with
// tt_RET_OUT_OF_BUFFER). Same rules: after creation, not during a call, 8-byte aligned, NULL for
// the inline default.
tt_ret_t tt_Client_set_storage(struct tt_Client* client, uint8_t* cache_storage, uint32_t cache_length);
tt_ret_t tt_Context_create_publisher(struct tt_Context* node, struct tt_Publisher* pub, struct tt_Topic* topic,
                                     const char* endpoint_name);
tt_ret_t tt_Context_create_subscriber(struct tt_Context* node, struct tt_Subscriber* sub, struct tt_Topic* topic,
                                      const char* endpoint_name, tt_SUBSCRIBER_CALLBACK callback);

// Nodes (CONTEXT_NODE_PLAN.md stage 2) - see struct tt_Node. The tt_Context_create_*() shorthands above create
// their endpoint on the context's default node; these create it on `node`, which must have been created on a
// context with tt_Node_create(). Same arguments and results otherwise, and tt_RET_INVALID_ARGUMENT for a node
// that is not (or no longer) created.
//
// tt_Node_create(): `name` and `namespace_name` are kept, not copied, and must outlive the node. tt_RET_OUT_OF_BUFFER
// when the context already hosts tt_MAX_NODES nodes, the default node included once it is in use. Index 0 is taken
// last, and only while the default node is not in use - after which the shorthands fail with tt_RET_OUT_OF_BUFFER.
// tt_RET_ILLEGAL_STATUS if `node` is already created.
// tt_Node_destroy(): tt_RET_ILLEGAL_STATUS while any endpoint is still created on the node - destroy those first.
// Frees the node's index for a later tt_Node_create().
// tt_Context_default_node(): the context's default node, brought into use if it is not yet: named
// "tickle_<context id>" in "/", index 0. NULL for a NULL context, or when an explicit node took index 0.
tt_ret_t tt_Node_create(struct tt_Context* context, struct tt_Node* node, const char* name, const char* namespace_name);
tt_ret_t tt_Node_destroy(struct tt_Node* node);
struct tt_Node* tt_Context_default_node(struct tt_Context* context);
// The node that owns `endpoint`, created on `context`: context->nodes[endpoint->node_index]. NULL for NULL arguments.
struct tt_Node* tt_Endpoint_node(const struct tt_Context* context, const struct tt_Endpoint* endpoint);
tt_ret_t tt_Node_create_publisher(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Topic* topic,
                                  const char* endpoint_name);
tt_ret_t tt_Node_create_subscriber(struct tt_Node* node, struct tt_Subscriber* sub, struct tt_Topic* topic,
                                   const char* endpoint_name, tt_SUBSCRIBER_CALLBACK callback);
tt_ret_t tt_Node_create_client(struct tt_Node* node, struct tt_Client* client, struct tt_Service* service,
                               const char* endpoint_name, tt_CLIENT_CALLBACK callback);
tt_ret_t tt_Node_create_server(struct tt_Node* node, struct tt_Server* server, struct tt_Service* service,
                               const char* endpoint_name, tt_SERVER_CALLBACK callback);
// Runs `function` at `time` from inside tt_Context_poll(). Callable from any thread. When a poll is blocked
// waiting for something later than `time`, this wakes it (that poll returns tt_RET_INTERRUPTED and the
// caller's next poll runs the entry on time); no tt_Context_interrupt() is needed.
bool tt_Context_schedule(struct tt_Context* node, uint64_t time,
                         void (*function)(struct tt_Context* node, uint64_t time, void* param), void* param);
// Cancels every pending schedule entry matching (function, param) exactly. Returns true if any were removed.
// Callable from any thread. On return, `function` is neither pending nor running for `param`: an entry
// the poll thread is running at that moment finishes first, so `param` may be freed afterwards.
bool tt_Context_unschedule(struct tt_Context* node,
                           void (*function)(struct tt_Context* node, uint64_t time, void* param), void* param);

tt_ret_t tt_Client_call(struct tt_Client* client, struct tt_Request* request);
tt_ret_t tt_Client_destroy(struct tt_Client* client);

tt_ret_t tt_Server_destroy(struct tt_Server* server);

tt_ret_t tt_Publisher_publish(struct tt_Publisher* pub, struct tt_Data* data);
tt_ret_t tt_Publisher_destroy(struct tt_Publisher* pub);

// Claimed-slot publish (DESIGN.md section 10, "Claimed-slot publish"): the caller builds a sample's CDR straight in the
// same-host subscriber's ring slot, so the publish copies nothing. borrow -> fill -> publish, or abandon:
//
//   tt_Publisher_claim(pub, capacity, &payload) claims a slot with room for `capacity` CDR bytes; *payload is where
//                                              they go, at 4 mod 8 (8-aligned after a 4-byte prefix). Returns
//     tt_RET_OK               claimed: fill *payload, then tt_Publisher_publish_claimed() or
//     tt_Publisher_abandon_claim() tt_RET_UNSUPPORTED      this publish has no slot to build in - not a lone DATA to
//     one same-host peer whose ring
//                             is attached (a broadcast, several or remote peers, local subscribers, batching, a sample
//                             that would not fit a slot or a datagram, a Heartbeat due alongside): publish ordinarily
//     tt_RET_OUT_OF_BUFFER    the ring is full: publish ordinarily, which drops and counts it as a full ring does
//     tt_RET_WOULD_BLOCK      KEEP_ALL with nothing acknowledged to make room, as tt_Publisher_publish() would say
//     tt_RET_ILLEGAL_STATUS   this Publisher holds a claim already
//   tt_Publisher_publish_claimed(pub, length)  sends the first `length` bytes written: tt_Publisher_publish()'s
//                                              results, and tt_RET_INVALID_ARGUMENT for a length over the capacity.
//                                              The claim is spent whatever it returns. If the destination changed
//                                              since the claim, the sample is copied out of the slot and published
//                                              the ordinary way.
//   tt_Publisher_abandon_claim(pub)            gives the slot back, sending nothing.
//
// The rules. ONE CLAIM PER PUBLISHER, and while it is held that Publisher's tt_Publisher_publish() is refused with
// tt_RET_ILLEGAL_STATUS (the claim's seq_no is taken at its publish, so a sample published in between would land
// behind it in the ring with an earlier seq_no). A CLAIMED SLOT STOPS ITS RING: the subscriber's context reads its
// ring in order, so every writer into that context waits behind the claim, and a ring that fills meanwhile drops - hold
// a claim for the time it takes to fill it, never across a wait. KEEP_ALL is checked at the claim, and nothing the
// claim holds back can unblock it later. tt_Publisher_destroy() and tt_Context_destroy() abandon an outstanding
// claim; after tt_Context_destroy() the slot is unmapped and its payload pointer is no longer valid. Callable from
// any thread, as tt_Publisher_publish() is; the fill needs no lock. Without the segment (tt_SEGMENT_ENABLED or
// tt_SEGMENT_ENCODE_IN_SLOT 0) every claim is tt_RET_UNSUPPORTED.
tt_ret_t tt_Publisher_claim(struct tt_Publisher* pub, uint32_t capacity, uint8_t** payload);
tt_ret_t tt_Publisher_publish_claimed(struct tt_Publisher* pub, uint32_t length);
tt_ret_t tt_Publisher_abandon_claim(struct tt_Publisher* pub);

tt_ret_t tt_Subscriber_destroy(struct tt_Subscriber* sub);

// Which writer sent the sample currently being delivered: the sending context's id and that endpoint's own
// entity_id, the pair that identifies one Publisher INSTANCE rather than one topic name (struct tt_Endpoint.id is a
// name hash and two Publishers on one topic share it - Milestone 47).
//
// Call ONLY from inside a tt_Subscriber callback. Outside one these name whatever arrived last, which answers a
// question nobody asked, so there is nothing useful to return and the contract is the caller's to keep - the same
// convention as every other plain-field access on these structs.
//
// Exists because rmw_tickle has to hand rmw_message_info_t.publisher_gid to anything matching a received sample to
// the writer that sent it, and until 2026-10-02 it reported sixteen zero bytes on every sample while
// rmw_get_gid_for_publisher() returned a real per-instance id - so a tool could learn a writer's gid from the graph
// and never match a sample to it.
static inline void tt_Subscriber_delivering_writer(const struct tt_Subscriber* sub, uint8_t* out_source,
                                                   uint32_t* out_entity_id) {
    *out_source = sub->delivering_source;
    *out_entity_id = sub->delivering_entity_id;
}

#if tt_LOCAL_DELIVERY
// (g9) Hands a durable Subscriber the durable backlog of this context's own durable Publishers on its topic, oldest
// first, as a late-joining remote Subscriber gets it over the link. For a caller that sets the Subscriber's QoS after
// creating it (rmw_tickle), so it is a call of its own rather than part of creation. A no-op for a Subscriber that is
// not durable.
void tt_Subscriber_deliver_local_backlog(struct tt_Subscriber* sub);

#endif

// Receive-buffer lending (DESIGN.md section 10). Called from inside `sub`'s own callback, keeps the sample being
// delivered where it arrived - its ring slot, or its receive buffer - and fills *out; the bytes stay valid until
// tt_Sample_release(). Returns:
//   tt_RET_OK               kept, *out filled
//   tt_RET_ILLEGAL_STATUS   not inside `sub`'s callback
//   tt_RET_UNSUPPORTED      this sample cannot be lent - it was put together from fragments, released from a reorder
//                           buffer, delivered locally, or the build has tt_SAMPLE_LENDING 0: copy it if it is needed
//   tt_RET_OUT_OF_BUFFER    tt_SAMPLE_RETAIN_MAX samples are held already, or (socket path) no spare receive buffer
//                           is attached or free (tt_Context_set_rx_pool()): copy it if it is needed
//   tt_RET_INVALID_ARGUMENT NULL argument
// A held ring slot stops its ring one lap later (tt_SEGMENT_SLOTS records): hold it for less than that.
tt_ret_t tt_Sample_retain(struct tt_Subscriber* sub, struct tt_Sample* out);
// Gives a retained sample back; from any thread. Clears *sample. tt_RET_INVALID_ARGUMENT, touching nothing, for a
// handle this context does not hold - 0, unknown, or already released. Every held sample is forgotten by
// tt_Context_destroy(), after which its bytes are no longer valid.
tt_ret_t tt_Sample_release(struct tt_Context* node, struct tt_Sample* sample);
// Receive buffers for socket-path lending: `count` (at most tt_RX_POOL_MAX) buffers of tt_RX_POOL_BUFFER_BYTES each,
// 8-aligned, owned by the caller and kept until tt_Context_destroy(). A sample retained from the socket keeps its
// buffer and the next datagram is received into a free one. NULL / 0 detaches. tt_RET_ILLEGAL_STATUS while a sample
// is held in a receive buffer or the socket is reading into the pool; tt_RET_UNSUPPORTED with tt_SAMPLE_LENDING 0.
tt_ret_t tt_Context_set_rx_pool(struct tt_Context* node, uint64_t* storage, uint8_t count);

#if tt_LARGE_SAMPLES
// Large-message stage 2 (DESIGN.md section 8): the buffers samples above tt_MAX_SAMPLE_LENGTH are published from and
// put back together in, one per sample (tt_LARGE_ACQUIRE above says what they must do). Without them a large publish
// fails with tt_RET_TOO_LARGE and a large sample arriving is counted in large.no_buffer. A large sample delivered to a
// Subscriber can be kept with tt_Sample_retain() past its callback; its buffer goes back to `release` at
// tt_Sample_release(). Set it before creating endpoints; NULL acquire and release detach (only while nothing is held).
// tt_RET_ILLEGAL_STATUS while a large buffer is out, tt_RET_INVALID_ARGUMENT for a NULL context or a half-set pair.
tt_ret_t tt_Context_set_large_buffers(struct tt_Context* node, tt_LARGE_ACQUIRE acquire, tt_LARGE_RELEASE release,
                                      void* user);
#endif

/**
 * @node node to poll
 * @timeout nanoseconds to wait, with two special values:
 *            0 - one non-blocking pass: run whatever is due, take whatever is already received.
 *            negative - wait exactly until the next scheduler entry is due and run it, or until a
 *                       datagram arrives, or until tt_Context_interrupt() - and return after the first of
 *                       those. With nothing scheduled, wait indefinitely (2026-09-25, the user's
 *                       decision: the scheduler already knows when the next thing is due, so there is
 *                       nothing to wake up for in between; this used to be a fixed 100us slice, about
 *                       10,000 wakes a second on an idle node). A signal also ends the wait, so Ctrl-C
 *                       still reaches a caller's loop at once.
 *
 * One thread polls a node at a time; a second concurrent tt_Context_poll() returns tt_RET_BUSY. Work
 * raised from another thread wakes a waiting poll by itself: tt_Context_schedule() - and so every core
 * call that arms a timer, a client call's retry, a batching publisher's flush - wakes it when the new
 * entry is earlier than what it is waiting for, and tt_Server_send_response() always does. A wake
 * ends that poll with tt_RET_INTERRUPTED. Inserts made on the poll thread itself never wake anything.
 *
 * No lock is held while the poll waits. The node's state lock is taken per datagram and per due
 * scheduler entry, and user callbacks run inside it - see "Threading" at tt_Context_lock().
 * @return tt_RET_OK after processing a datagram, tt_RET_TIMEOUT when the wait ended without one
 *         (including after running a due scheduler entry), tt_RET_INTERRUPTED, tt_RET_BUSY, or an
 *         error.
 */
tt_ret_t tt_Context_poll(struct tt_Context* node, int64_t timeout);

// The one exception to every other tt_Context_*/tt_Publisher_*/... call needing to come from the
// same single thread (see this file's own "Concurrency" note, and DESIGN.md's) - this one is
// specifically meant to be called from a *different* thread than whichever one is currently
// blocked in tt_Context_poll(), to make that call return tt_RET_INTERRUPTED right away instead of
// waiting out the rest of its timeout. Meant for a caller that drives tt_Context_poll() from a
// dedicated thread with a long timeout, but sometimes needs that thread to come back and yield to
// other work (e.g. a lock the poll thread also needs) sooner than the timeout would otherwise
// allow. "At least once, at or after this call" - not "only if currently blocked": if nothing is
// blocked in tt_Context_poll() right now, the signal is queued and delivered to whichever
// tt_Context_poll() call comes *next* instead (even one that starts well after this call returns),
// not silently dropped. A caller driving tt_Context_poll() in a continuous loop (the intended usage)
// sees no difference either way; one that calls tt_Context_poll() only occasionally should account
// for an earlier tt_Context_interrupt() still being able to cut its next, unrelated wait short.
tt_ret_t tt_Context_interrupt(struct tt_Context* node);

// Threading (2026-09-25, the user's decision: core is thread-safe, lock-free where it can be and with
// fine-grained locks where it cannot). With tt_THREAD_SAFE (config.h, default 1):
//
// - Every public tt_* function may be called from any thread, concurrently with tt_Context_poll() on
//   another. tt_Server_send_response() and tt_Context_interrupt() take no lock at all; tt_Context_schedule()
//   takes the node's lock only if it is free (or already held by the caller), and otherwise hands its
//   entry to the poll thread through a lock-free inbox; everything else takes the node's lock for the
//   length of the call. tt_Context_poll() holds nothing while it waits.
// - User callbacks (subscriber, client, server, discovery, writable, scheduled functions) run on the
//   polling thread with the state lock held, as they always ran inside the one thread that drove the
//   node. They may call back into core. A slow callback delays every other thread's call on this node
//   by as long as it takes, so keep them short.
// - Creating and destroying the node itself is not concurrent-safe: no other thread may be using a node
//   while tt_Context_create() or tt_Context_destroy() runs.
// - Reading several node-owned fields that must agree with each other - the tt_Discovery table the
//   node fills in, a Publisher's counters - needs the state lock held across the reads:
//   tt_Context_lock()/tt_Context_unlock(). They nest, and any tt_* call may be made while holding them.
//   tt_ReliableCache_grow() and tt_Discovery_count() take a cache or table rather than a node, so the
//   caller holds tt_Context_lock() around them when the cache or table belongs to a live node.
void tt_Context_lock(struct tt_Context* node);
// tt_Context_lock() giving up after timeout_ns; true if the lock was taken (and must be released with
// tt_Context_unlock()). For an observer that must never block behind a wedged callback on the poll
// thread - rmw_tickle's liveliness watchdog is one.
bool tt_Context_lock_timed(struct tt_Context* node, uint64_t timeout_ns);
void tt_Context_unlock(struct tt_Context* node);

// Opts `node` into graph introspection: every UPDATE it processes from here on also records the
// announcing entity into `*discovery` (an otherwise-inert struct the caller owns - see its own
// comment) and fires `callback` for an appearance, refresh, or (check_liveliness()/an explicit
// farewell UPDATE) departure. `discovery` must outlive `node`, and must already be zeroed
// (`memset` or `= {0}`) - this does not initialize its contents itself, only points `node` at it.
// `callback`/`param` may be NULL to record without being notified (poll tt_Discovery_find()
// yourself instead). Pass `discovery == NULL` to detach again.
tt_ret_t tt_Context_set_discovery(struct tt_Context* node, struct tt_Discovery* discovery,
                                  tt_DISCOVERY_CALLBACK callback, void* param);

// Number of currently-alive occupied slots in `discovery` - for iterating/sizing a snapshot
// without walking the full tt_MAX_DISCOVERED_ENTITIES capacity by hand. Excludes tombstoned
// entries (struct tt_DiscoveredEntity.alive's own doc comment) - a presumed-dead entity is still
// findable via tt_Discovery_find() below, just not counted here, matching this function's own
// "topic list"-style use (you wouldn't want a dead node's own topic still listed).
uint32_t tt_Discovery_count(const struct tt_Discovery* discovery);

// Looks up a remote entity by (context_id, endpoint_id): the first one recorded when that context hosts several
// endpoints of one topic or service (two publishers, or a publisher and a subscriber, share the endpoint_id), so it
// answers "does that context have one", not "which one" - tt_Discovery_find_entity() names one. NULL if none is
// currently known - never announced, or *normally* departed (an explicit farewell, or dropped from a fresh
// announce). A presumed-dead entity (struct tt_DiscoveredEntity.alive's own doc comment) is still
// returned, with .alive == false, not NULL - check that field to tell the two "not currently
// alive" shapes apart. The returned pointer is only valid until the next UPDATE this node
// processes - copy out anything needed past that point.
// Rebuilds the table's index (struct tt_Discovery.index) from its entries - needed only after changing an entry's
// context_id or endpoint_id directly; core keeps it current itself. Nothing to do in a build without the index.
#if tt_DISCOVERY_INDEXED
void tt_Discovery_reindex(struct tt_Discovery* discovery);
#else
static inline void tt_Discovery_reindex(struct tt_Discovery* discovery) {
    (void)discovery;
}
#endif
const struct tt_DiscoveredEntity* tt_Discovery_find(const struct tt_Discovery* discovery, uint8_t context_id,
                                                    uint32_t endpoint_id);

// The one remote entity (context_id, entity_id) names - its identity network-wide - looked up under its endpoint_id
// (the table's index key). Otherwise as tt_Discovery_find(): NULL if not known, a tombstone returned with .alive false.
const struct tt_DiscoveredEntity* tt_Discovery_find_entity(const struct tt_Discovery* discovery, uint8_t context_id,
                                                           uint32_t endpoint_id, uint32_t entity_id);

// QoS roadmap #3 (LIVELINESS) RxO, Milestone 62 (rmw_tickle/PLAN.md's own "DDS semantic-parity
// backlog" row 3) - computes whether `entity` is alive right now, freshly, independent of its own
// `.alive` field's own periodic background sweep (check_liveliness(), tickle.c). check_liveliness()
// watches per-*node* UPDATE traffic on one fixed window (tt_LIVELINESS_MISS_THRESHOLD *
// tt_CONTEXT_UPDATE_INTERVAL, ~3s) - correct for a "did this whole node disappear" participant-level
// signal, but too coarse for an entity that requested its own, different `liveliness_lease_
// duration_ns` (a Publisher/Subscriber field, carried on the wire since Milestone 49 and already
// stored per discovery entry, but never actually consulted for detection before this milestone -
// only for RxO compatibility gating). Two cases:
//   - `entity->liveliness_lease_duration_ns == 0` (no specific lease requested, the common/default
//     case): unchanged from before this function existed - returns `entity->alive` verbatim,
//     deferring entirely to check_liveliness()'s own coarser, node-level sweep, since an entity
//     that never asked for a specific lease has no individual timeout of its own to compute.
//   - non-zero: computed fresh from `node->update_last_seen[entity->context_id]` against that lease
//     directly, bypassing `.alive`/the periodic sweep's own timing entirely - accurate to whatever
//     cadence the caller itself polls at (e.g. rmw_tickle's own check_subscription_liveliness(),
//     which already reschedules itself at each Subscription's own liveliness_lease_ns - it previously
//     had nothing faster than the ~3s sweep to poll *for*, this is what makes that already-correct
//     cadence actually pay off). Honest scope limit, not glossed over: this still only distinguishes
//     "how long since ANY UPDATE from this entity's own node", the same underlying signal AUTOMATIC
//     liveliness always used (correct for it) - it does not separately implement MANUAL_BY_TOPIC's
//     own stricter real-DDS semantic (an entity must itself specifically publish/assert within its
//     lease, not merely share a node with other traffic); doing that would need a genuinely new,
//     per-entity activity signal TickLE's wire protocol doesn't carry today, out of this milestone's
//     own scope. `node` must be the same tt_Context `entity`'s own discovery table is attached to.
bool tt_Context_entity_alive(const struct tt_Context* node, const struct tt_DiscoveredEntity* entity, uint64_t now);

tt_ret_t tt_Context_destroy(struct tt_Context* node);

// Bumped 1 -> 2 for QoS roadmap #1 (RxO matching, Milestone 31, rmw_tickle/PLAN.md) - struct tt_
// UpdateEntity below grew a `qos` byte, a real on-the-wire layout change. Safe without any
// version-straddling parsing logic: process_packet() already rejects any packet whose header-
// >version is < this node's own tt_VERSION outright (tickle.c), so by the time decode_update_
// entities() ever reads update_entity->qos, the sender is already guaranteed to be running this
// same version or newer - there is no partial-compatibility case to handle.
// Bumped 2 -> 3 for Milestone 47 - struct tt_DataHeader/tt_AckNackHeader/tt_HeartbeatHeader each
// grew an entity_id field, the same "no partial-compatibility case to handle" reasoning applies.
// Bumped 3 -> 4 for Milestone 49 - struct tt_UpdateEntity grew deadline_duration_ns/liveliness_
// lease_duration_ns and a new tt_UPDATE_QOS_LIVELINESS_MANUAL qos bit, the identical reasoning.
// Bumped 4 -> 5 for the ACKNACK bitmap widening (rmw_tickle/PLAN.md's "TickLE-native performance"
// plan) - struct tt_AckNackHeader.bitmap grew from a single uint64_t to a tt_RELIABLE_BITMAP_WORDS-
// word array (256 bits total, config.h), a real on-the-wire layout change; the identical "no
// partial-compatibility case to handle" reasoning applies.
// Bumped 6 -> 7 for DATA_FRAG step 2 (rmw_tickle/DATAFRAG_PLAN.md section 6): the discovery announce
// became a DATA sample of a built-in endpoint (tt_DISCOVERY_ENDPOINT_ID), and UPDATE/UPDATE_PART were
// retired.
// Bumped 10 -> 11 for CONTEXT_NODE_PLAN.md stage 3: an announce lists the context's nodes (tt_KIND_NODE entries), and
// every entry carries its node's index in spare bits of kind and qos (tt_UPDATE_NODE_INDEX_* below).
// Bumped 11 -> 12 for large-message stage 2 (DESIGN.md section 8): two new submessage types, FRAG_FIRST_L and
// FRAG_CONT_L, for samples above tt_MAX_SAMPLE_LENGTH. Every existing submessage is unchanged byte for byte.
#define tt_VERSION 12

struct tt_Header {
    union {
        char magic[2]; // "TK" for big endian, "KT" for little endian
        uint16_t magic_value;
    };
    uint8_t version; // protocol version
    uint8_t source;  // Sender ID
} __attribute__((packed));

#define tt_SUBMESSAGE_ID_ALL 0xff

// Since tt_VERSION 10 (rmw_tickle/WIRE_PLAN.md W4): a datagram that carries exactly one submessage, addressed to
// everyone (receiver tt_SUBMESSAGE_ID_ALL) - a broadcast or unicast DATA, a fragment, a HEARTBEAT, the discovery
// summary - goes with this 4-byte header in place of tt_Header + tt_SubmessageHeader (8 bytes). The
// submessage's length is the rest of the datagram. `marker` is the first byte of the sender's tt_Header magic
// in lower case - 'k' from a little-endian sender, 't' from a big-endian one - which also says how to read the
// rest; everything else is as in the two headers it replaces. Four bytes, so what follows stays 4-aligned.
struct tt_SingleHeader {
    uint8_t marker;
    uint8_t version;
    uint8_t source;
    uint8_t type;
} __attribute__((packed));
#define tt_SINGLE_MARKER_FLAG 0x20U // what turns the magic's first byte into the marker
#define tt_SINGLE_MARKER_LE 'k'     // 'K' | tt_SINGLE_MARKER_FLAG
#define tt_SINGLE_MARKER_BE 't'     // 'T' | tt_SINGLE_MARKER_FLAG

// Type 1 was UPDATE and type 7 UPDATE_PART, the discovery announce, until tt_VERSION 7 made the
// announce a DATA sample of a built-in endpoint (tt_DISCOVERY_ENDPOINT_ID below). Retired, not free:
// never give either number a new meaning.
#define tt_SUBMESSAGE_TYPE_DATA 2
#define tt_SUBMESSAGE_TYPE_ACKNACK 3
#define tt_SUBMESSAGE_TYPE_CALLREQUEST 4
#define tt_SUBMESSAGE_TYPE_CALLRESPONSE 5
#define tt_SUBMESSAGE_TYPE_HEARTBEAT 6
// A sample too large for one datagram, sent as fragments (DATA_FRAG, rmw_tickle/DATAFRAG_PLAN.md
// section 6). The first carries the sample's whole DataHeader (struct tt_FragFirstHeader); the rest
// carry only what identifies the sample (struct tt_FragContHeader), since entity_id is unique within a
// node and the node is tt_Header.source. Only ever sent when a DATA would not fit. A discovery announce
// uses the same two types, split differently - see tt_DISCOVERY_ENDPOINT_ID.
#define tt_SUBMESSAGE_TYPE_FRAG_FIRST 8
#define tt_SUBMESSAGE_TYPE_FRAG_CONT 9

// RESERVED, AND NOTHING PRODUCES IT. Read this before planning anything around it.
//
// It was defined for a design that is not the one built. SHM_PLAN 6e needed a segment record to
// declare how many seq_nos its sample covers, and the first sketch gave that job to a submessage
// type of its own. The implementation put the span in the SLOT header instead -
// tt_SegmentSlot.seq_span - because the slot is what a segment reader parses before it parses
// anything else, and because the record inside it is then an ordinary DATA, identical on both
// paths. So the span exists and works, and this type is not how it travels. The previous version
// of this comment still described the submessage carrying the span, which stopped being true the
// day the slot field landed and would have sent the next reader looking in the wrong place.
//
// The only code that ever sets this type is tests/test_hostile_datagram.c, which builds one
// deliberately to prove it is refused.
//
// It stays defined, and the guard in process_submessage() stays with it, for two reasons. The
// number must never be given a new meaning - a later build that reused 10 would find this one
// refusing its traffic. And the guard is the cheap half of section 1's safety claim: a segment
// record is safe only because it takes the SAME acceptance path as a datagram, so a record that
// could only have been built by something with write access to a segment has to be refused when it
// arrives from the network instead. rx_shm_only_on_socket counts that, and reaches the RESULT line.
//
// 10, not 7: type 1 (UPDATE) and type 7 (UPDATE_PART) are retired and the comment above forbids
// giving either a new meaning.
#define tt_SUBMESSAGE_TYPE_SHM_DATA 10

// A sample larger than tt_MAX_SAMPLE_LENGTH (large-message stage 2, DESIGN.md section 8, tt_VERSION 12): types 8 and 9
// with frag_index and frag_count widened to 16 bits (struct tt_FragFirstLHeader, struct tt_FragContLHeader), so a
// sample may take up to tt_LARGE_MAX_FRAGMENTS datagrams. Only ever sent for such a sample; everything within
// tt_MAX_SAMPLE_LENGTH keeps types 8/9. 11 and 12, not 10, which tt_SUBMESSAGE_TYPE_SHM_DATA below reserves for good.
// A node built without stage 2 skips both and counts them (tt_Context.frag_large_skipped).
#define tt_SUBMESSAGE_TYPE_FRAG_FIRST_L 11
#define tt_SUBMESSAGE_TYPE_FRAG_CONT_L 12

struct tt_SubmessageHeader {
    uint8_t type;     // tt_SUBMESSAGE_TYPE_* above
    uint8_t receiver; // Receiver ID
    // Length of the whole submessage in BYTES, this header included, normally padded to a multiple of 4
    // (ROUNDUP) - the offset from this header to the next one, which is how the receiver walks a
    // datagram. Not a count of 4-byte words: this comment used to say "in 4 bytes", and a parser
    // written from it misread every datagram as malformed.
    uint16_t length;
} __attribute__((packed));

// Discovery (tt_VERSION 7, rmw_tickle/DATAFRAG_PLAN.md section 6): a node's endpoint list travels as a
// DATA sample of a built-in endpoint - the RTPS arrangement - rather than as a submessage type of its
// own. Its tt_DataHeader carries
//   endpoint_id = tt_DISCOVERY_ENDPOINT_ID, entity_id = tt_DISCOVERY_ENTITY_ID,
//   timestamp   = the node's last_modified,
//   seq_no      = the announce's generation: the low 32 bits of last_modified. It changes exactly when
//                 the endpoint list does, and differs across a restart, which is all the receiver asks
//                 of it; it is what identifies the announce because a FRAG_CONT carries seq_no and
//                 not timestamp.
// and its payload is a struct tt_AnnounceHeader followed by that many struct tt_UpdateEntity records.
//
// An announce too large for one datagram is split into FRAG_FIRST/FRAG_CONT fragments, but at entity
// boundaries: every fragment carries its own tt_AnnounceHeader and whole entities, and is processed as
// it arrives, with no reassembly memory. That keeps what UPDATE_PART gave a node on core defaults - it
// can discover a node whose announce spans datagrams - while user data shares the wire format. A
// continuation is recognised as discovery by its entity_id, which no user entity is ever given.
//
// The receiver refreshes the sender's liveliness on every announce and every fragment before anything
// else, and re-applies the entity list only when the generation changes: a resend of an unchanged list
// carries an already-seen seq_no by design.
//
// Since tt_VERSION 8 the list is not resent periodically (rmw_tickle/DISCOVERY_PLAN.md). Every
// tt_CONTEXT_UPDATE_INTERVAL a node broadcasts a summary instead: a HEARTBEAT of this endpoint whose
// first_available_seq_no and last_seq_no are its current generation, ~28 bytes whatever its endpoint count.
// It refreshes liveliness as an announce does. A receiver that has not applied that generation - it missed
// the change's broadcast, joined later, or never heard the node in full - asks for the list with an ACKNACK
// of this endpoint (seq_no = the generation), unicast, and gets the announce back unicast. A change is
// still pushed at once by broadcast.
#define tt_DISCOVERY_ENDPOINT_ID 0
#define tt_DISCOVERY_ENTITY_ID UINT32_MAX

struct tt_AnnounceHeader {
    uint8_t entity_count;
    /* Dynamically allocated
    struct tt_UpdateEntity entities[];
    */
} __attribute__((packed));

// tt_UPDATE_MAX_PARTS, the fragments one announce may be split into, is a setting in config.h.

// QoS roadmap #1 (RxO matching, Milestone 31) - the bits struct tt_UpdateEntity.qos below
// carries, one per policy this package implements a wire-visible mechanism for (services/
// clients always encode 0 here - RELIABILITY there is already unconditional via tt_Client_call()'s
// own retry, no QoS negotiation needed, and DURABILITY/LIVELINESS-kind have no service/client
// analog at all). Same bit positions regardless of direction: on a TOPIC_PUBLISHER entity this is
// what that Publisher *offers* (tt_Publisher.reliable/.durable/.liveliness_manual); on a TOPIC_
// SUBSCRIBER entity it's what that Subscriber *requests* (tt_Subscriber.reliable/.durable/
// .liveliness_manual) - decode_update_entities()'s own tt_KIND_TOPIC_SUBSCRIBER branch and
// process_data()'s own subscriber_incompatible_with_writer() (both tickle.c) are what actually
// compare the two sides.
#define tt_UPDATE_QOS_RELIABLE (1U << 0)
#define tt_UPDATE_QOS_DURABLE (1U << 1)
// QoS roadmap #3 (LIVELINESS) RxO, Milestone 49 - set iff this entity's own LIVELINESS kind is
// MANUAL_BY_TOPIC (the only manual kind this package's own rmw layer still supports, Milestone
// 32's own finding) rather than AUTOMATIC. The lease duration itself travels separately, as a
// real numeric field (tt_UpdateEntity.liveliness_lease_duration_ns below) - unlike RELIABLE/
// DURABLE, a single bit can't carry "how long", only "which kind".
#define tt_UPDATE_QOS_LIVELINESS_MANUAL (1U << 2)
// Phase 3 (rmw_tickle/PLAN.md) - set by a Publisher announcing DDS HISTORY KEEP_ALL
// (tt_Publisher.keep_all): it will refuse a write rather than evict an unacknowledged sample, so a
// matched Subscriber must not give up on a gap either - acknack_retry()'s own tt_RELIABLE_RETRY
// budget is disabled for that writer (struct tt_WriterProxy.keep_all). Absent means KEEP_LAST, i.e.
// today's bounded give-up on both sides; a Subscriber never sets it (KEEP_ALL is a Publisher-side
// retention policy, and the Subscriber's matching obligation is what this bit conveys).
//
// Neither side may give up under KEEP_ALL, or the Subscriber would abandon a gap, advance its ack,
// and unblock the Publisher having silently dropped a sample - the same hole per-Subscriber ack
// identity closed on the other side. Only two things still end recovery: LIFESPAN expiry (an
// expired sample is "as if never sent", so 1-c's eviction Heartbeat still fires and the Subscriber
// still advances past it) and liveliness (a writer declared not alive loses its WriterProxy, a
// Subscriber declared not alive leaves the Publisher's ack set).
#define tt_UPDATE_QOS_KEEP_ALL (1U << 3)
// Stage 3 (wire v11): an announce entry's node index, 8 bits in bits kind and qos do not use - no bytes added. Index
// bits 0-3 are qos bits 4-7; index bits 4-7 are kind bits 2, 3, 6 and 7. A receiver masks kind and qos with the
// _MASK values before any other use of them.
#define tt_UPDATE_QOS_MASK 0x0FU
#define tt_UPDATE_KIND_MASK (tt_KIND_TOPIC | tt_KIND_SERVICE | tt_KIND_SENDER | tt_KIND_RECEIVER)

struct tt_UpdateEntity {
    uint32_t endpoint_id; // hash(topic/service name + endpoint name)
    // Phase 2 (rmw_tickle/PLAN.md) - this entity's own struct tt_Endpoint.entity_id, which an
    // announce carried no identifier for before. Two Subscribers of one topic in one process share
    // endpoint_id by construction, so without this a matched Publisher could not tell how many
    // distinct Subscriber entities a remote node hosts - and "acked by all" would silently exclude
    // a matched-but-still-silent one, exactly the hazard Phase 3's KEEP_ALL blocking must not have.
    uint32_t entity_id;
    uint8_t kind;
    uint8_t qos; // tt_UPDATE_QOS_RELIABLE / _DURABLE / _LIVELINESS_MANUAL - see their own doc comment above
    // Phase 2 - a Subscriber's own RELIABLE tracking window, in 64-bit words (struct
    // tt_Subscriber.tracking_words; 0 = the tt_RELIABLE_BITMAP_WORDS default, which is also what a
    // non-Subscriber entity always announces). A matched Publisher's safe KEEP_ALL bound is the
    // minimum across its matched Subscribers' windows - it can't retain more unacked samples than
    // the narrowest of them can still ask about. Packed into what used to be reserved[2] (Milestone
    // 49's own padding, there to keep the two uint64_t fields below 4-aligned per TickLE's CDR-4
    // convention), so announcing it costs no extra bytes.
    uint16_t tracking_words;
    // QoS roadmap #2 (DEADLINE) RxO - this entity's own offered (Publisher) or requested
    // (Subscriber) deadline, in nanoseconds; 0 = no DEADLINE requested/offered ("infinite"),
    // matching every other 0-disabled duration field in this codebase. Always 0 for a service/
    // client, same reasoning as the qos bits above.
    uint64_t deadline_duration_ns;
    // QoS roadmap #3 (LIVELINESS) RxO - this entity's own offered/requested liveliness lease
    // duration, in nanoseconds; 0 = no specific lease requirement. Independent of the
    // tt_UPDATE_QOS_LIVELINESS_MANUAL bit above - real DDS's own LIVELINESS policy is (kind,
    // lease_duration) as one combined unit, so e.g. a Subscriber may legitimately request
    // AUTOMATIC with a tight lease requirement, not just MANUAL_BY_TOPIC ones.
    uint64_t liveliness_lease_duration_ns;
    /* Dynamically allocated
    uint16_t type_len;
    char type[];
    uint16_t name_len;
    char name[];
    */
} __attribute__((packed));

struct tt_DataHeader {
    uint32_t endpoint_id; // endpoint id for subscriber lookup
    uint32_t seq_no;
    // Since tt_VERSION 10 (rmw_tickle/WIRE_PLAN.md W2): the low 32 bits of the sender's clock in
    // microseconds, 4 bytes where a 64-bit nanosecond count took 8. The receiver rebuilds the rest from its
    // own clock (timestamp_from_wire(), tickle.c), taking the sender's to be within half the 32-bit range,
    // +-35.8 min, of its own; callbacks still get nanoseconds, at microsecond precision.
    uint32_t timestamp;
    // Milestone 47 - the *sending* Publisher's own struct tt_Endpoint.entity_id (see its own doc
    // comment), distinguishing this specific Publisher instance from any other one sharing
    // endpoint_id above. header->source (struct tt_Header, message-level) already narrows this to
    // one remote node; this narrows it further to one specific entity on that node, closing the
    // real, confirmed cross-instance data-mixing gap this milestone exists for.
    uint32_t entity_id;
    // type + name
    // CDR
} __attribute__((packed));

// Fragments are always alone in their datagram, so neither header below is padded and neither is the
// fragment itself: tt_SubmessageHeader.length is exact, and the receiver takes the payload length from
// it. Every fragment but the last is full, so a fragment's position follows from its index and the
// payload size of a full continuation - see struct tt_FragSlot.

// Fragment 0: the sample's own DataHeader, then the first CDR bytes.
struct tt_FragFirstHeader {
    struct tt_DataHeader data;
    uint8_t frag_count; // 2 .. tt_FRAG_MAX_COUNT
} __attribute__((packed));

// Fragments 1 .. frag_count - 1.
struct tt_FragContHeader {
    uint32_t entity_id; // tt_DataHeader.entity_id of the sample
    uint32_t seq_no;    // this datagram's own; the sample's is seq_no - frag_index (DATAFRAG_PLAN.md 13)
    uint8_t frag_index; // 1 .. frag_count - 1
    uint8_t frag_count; // the same in every fragment of a sample
} __attribute__((packed));

// How much less CDR fragment 0 carries than a full continuation, for its longer header.
#define tt_FRAG_FIRST_SHORTFALL (sizeof(struct tt_FragFirstHeader) - sizeof(struct tt_FragContHeader))

// Large-message stage 2 (tt_SUBMESSAGE_TYPE_FRAG_FIRST_L/_CONT_L): the same two headers with a 16-bit index and count.
// Fragment 0 of a large sample, 18 bytes.
struct tt_FragFirstLHeader {
    struct tt_DataHeader data;
    uint16_t frag_count; // 2 .. tt_LARGE_MAX_FRAGMENTS
} __attribute__((packed));

// Fragments 1 .. frag_count - 1 of a large sample, 12 bytes.
struct tt_FragContLHeader {
    uint32_t entity_id;  // tt_DataHeader.entity_id of the sample
    uint32_t seq_no;     // this datagram's own; the sample's is seq_no - frag_index
    uint16_t frag_index; // 1 .. frag_count - 1
    uint16_t frag_count; // the same in every fragment of a sample
} __attribute__((packed));

// How much less CDR a large sample's fragment 0 carries than its continuations: 6 bytes.
#define tt_FRAG_FIRST_L_SHORTFALL (sizeof(struct tt_FragFirstLHeader) - sizeof(struct tt_FragContLHeader))

// How many datagrams - and so seq_no - a sample of cdr_len encoded bytes takes: 1 when a DATA carries it
// whole, else its fragment count (DATAFRAG_PLAN.md section 13). For sizing what counts seq_no - a reliable
// cache's depth, a tracking window - in samples. Padded as a sample is sent. test_data_frag.c checks it
// against what publishing actually sends.
static inline uint32_t tt_sample_datagrams(uint32_t cdr_len) {
#if tt_FRAG_ENABLED
    const uint32_t framing = (uint32_t)(sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    const uint32_t padded = (cdr_len + 3U) & ~3U;
    if (framing + (uint32_t)sizeof(struct tt_DataHeader) + padded <= (uint32_t)tt_CONTROL_MAX_LENGTH) {
        return 1;
    }
#if tt_LARGE_SAMPLES
    if (cdr_len > (uint32_t)tt_MAX_SAMPLE_LENGTH) { // a large sample: FRAG_FIRST_L/_CONT_L (DESIGN.md section 8)
        const uint32_t first_l =
            (uint32_t)tt_CONTROL_MAX_LENGTH - framing - (uint32_t)sizeof(struct tt_FragFirstLHeader);
        const uint32_t cont_l = (uint32_t)tt_CONTROL_MAX_LENGTH - framing - (uint32_t)sizeof(struct tt_FragContLHeader);
        return 1 + ((padded - first_l + cont_l - 1) / cont_l);
    }
#endif
    const uint32_t first = (uint32_t)tt_CONTROL_MAX_LENGTH - framing - (uint32_t)sizeof(struct tt_FragFirstHeader);
    const uint32_t cont = (uint32_t)tt_CONTROL_MAX_LENGTH - framing - (uint32_t)sizeof(struct tt_FragContHeader);
    return 1 + ((padded - first + cont - 1) / cont);
#else
    (void)cdr_len;
    return 1;
#endif
}

// An upper bound on the reliable-cache arena bytes one sample of cdr_len encoded bytes takes: its one
// record, or one record per fragment, each 4-aligned. For sizing an arena in samples, as
// tt_RELIABLE_RECORD_BYTES() did before samples fragmented. test_data_frag.c checks it against what
// caching actually takes.
static inline uint32_t tt_sample_cache_bytes(uint32_t cdr_len) {
    const uint32_t datagrams = tt_sample_datagrams(cdr_len);
    if (datagrams == 1) {
        return tt_RELIABLE_RECORD_BYTES(cdr_len);
    }
    const uint32_t record_overhead =
        (uint32_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_FragContHeader)) + 3U; // + alignment
    return ((cdr_len + 3U) & ~3U) + (datagrams * record_overhead) + (uint32_t)tt_FRAG_FIRST_SHORTFALL;
}

struct tt_AckNackHeader {
    uint32_t endpoint_id; // target Publisher - same leading-field convention as tt_DataHeader/
                          // tt_CallRequestHeader/tt_CallResponseHeader (one node can host many
                          // endpoints, so the submessage receiver alone isn't enough)
    // Milestone 47 - the *target* Publisher's own struct tt_Endpoint.entity_id, mirroring
    // endpoint_id's own "target Publisher" role above. Lets a Publisher-side receiver pick the
    // exact local Publisher instance among several sharing endpoint_id (find_endpoint_by_entity(),
    // tickle.c), closing Milestone 35's own previously-accepted "first match" ambiguity for ACKNACK
    // routing specifically, the same way entity_id already does for DATA/HEARTBEAT dispatch. 0 is a
    // safe "unknown/not yet learned" sentinel (falls back to plain first-match routing).
    uint32_t entity_id;
    // Phase 2 (rmw_tickle/PLAN.md) - the *sending Subscriber's* own entity_id, which this header
    // carried no identifier for before. A Publisher's ack bookkeeping is per matched Subscriber
    // entity (struct tt_PeerAck, tickle.h), and every Subscriber matching one Publisher shares
    // endpoint_id by construction (it's hash(topic name, endpoint name)), so without this two
    // Subscriptions of one topic in one remote process were indistinguishable: the faster one's
    // ack spoke for both. Harmless while ack state was only advisory; silent loss under Phase 3's
    // KEEP_ALL write blocking, which waits on it. 0 means "unknown sender" (a peer that predates
    // this field can't occur - tt_VERSION gates that - but a zeroed field must not alias a real
    // entity), and such an ACKNACK still routes and retransmits, it just isn't counted as an ack.
    uint32_t sender_entity_id;
    uint32_t seq_no;       // cumulative ack: every seq_no below this was received
    uint16_t bitmap_words; // how many 64-bit words of bitmap[] follow - see below
    uint16_t reserved;
    // bit j set: (seq_no + j) is still missing, please resend - same direction as RTPS's own
    // AckNack SequenceNumberSet. Word 0 holds bits 0-63, word 1 bits 64-127, and so on, each
    // independently rd64()'d on decode like any other 8-byte wire field.
    //
    // Phase 2 - variable length (bitmap_words entries, 0..tt_RELIABLE_BITMAP_MAX_WORDS), where this
    // used to be a fixed tt_RELIABLE_BITMAP_WORDS-wide field. A Subscriber sends only the words its
    // own request actually reaches into, so the common one-gap ACKNACK is 28 bytes rather than 44,
    // and widening the tracking window (struct tt_Subscriber.tracking_bitmaps) costs nothing on the
    // wire until a genuinely spread-out gap needs it. A receiver must validate bitmap_words against
    // both the datagram's own remaining length and its own capacity before indexing - see
    // process_acknack() (tickle.c).
    uint64_t bitmap[];
} __attribute__((packed));

// tt_HeartbeatHeader.flags's own tt_HEARTBEAT_FLAG_FINAL - same name and meaning as RTPS's own
// HEARTBEAT finalFlag. Set: a Subscriber only needs to reply with an ACKNACK if it actually has a
// gap to report (today's only behavior before this bit existed - both send_heartbeat()'s periodic
// announce and send_initial_heartbeat()'s discovery-triggered one-off always set it, so neither
// changes behavior). Clear: the Subscriber must reply with an ACKNACK regardless of gap state -
// tt_Publisher_wait_for_all_acked() (tickle.c)'s own solicited Heartbeat is the only thing that
// ever clears it, to get a positive, provable "caught up" signal out of an already-healthy
// Subscriber, which - unlike a genuine gap - otherwise has no reason of its own to ever send one
// (see maybe_arm_acknack_retry()'s own "a healthy stream needs no ACKNACK at all" doc comment,
// tickle.c). This is the exact mechanism real RTPS's own wait_for_acknowledgments() relies on.
#define tt_HEARTBEAT_FLAG_FINAL (1U << 0)
// Since tt_VERSION 9: this HEARTBEAT asserts the writer's liveliness (tt_Publisher_assert_liveliness()) and
// carries nothing else - receivers refresh the lease of the MANUAL_BY_TOPIC Publisher it names and stop
// there, whatever the seq_no fields say. RTPS's liveliness flag, the same role.
#define tt_HEARTBEAT_FLAG_LIVELINESS (1U << 1)

// QoS roadmap #5 (RELIABILITY) follow-up - a RELIABLE Publisher's own periodic self-announce of
// what it currently has retained, same role as RTPS's own HEARTBEAT submessage (firstSN/lastSN).
// Lets a Subscriber learn the real, currently-retained range directly - independent of whether
// any specific DATA sample's own delivery attempt happened to succeed - rather than only ever
// inferring "something might be missing" reactively from whatever DATA does arrive (this file's
// own struct tt_WriterProxy.heartbeat_last_seq_no doc comment explains the gap this
// closes; opt-in via tt_Publisher_set_heartbeat_period(), tickle.c).
struct tt_HeartbeatHeader {
    uint32_t endpoint_id;            // source Publisher - same leading-field convention as above
    uint32_t first_available_seq_no; // oldest sample still retained in reliable_cache right now
    uint32_t last_seq_no;            // newest published (== pub->seq_no at send time)
    // Milestone 47 - the *sending* Publisher's own struct tt_Endpoint.entity_id, same role/
    // reasoning as struct tt_DataHeader.entity_id's own doc comment (this is the same Publisher,
    // just announcing instead of publishing).
    uint32_t entity_id;
    uint8_t flags;       // tt_HEARTBEAT_FLAG_FINAL - see its own doc comment above
    uint8_t reserved[3]; // pad 17 -> 20 - same "pad the CDR payload to 4-byte alignment"
                         // convention as tt_CallRequestHeader's own reserved byte
} __attribute__((packed));

struct tt_CallRequestHeader {
    uint32_t endpoint_id; // endpoint id for service server lookup
    uint16_t seq_no;      // sequence number
    uint8_t retry;        // retry count from client side
    // Which Client of this service in the source context is calling (struct tt_Client.client_tag), 1..255; 0 =
    // untold. It was a zero pad byte until 2026-10-09 and still pads 7 -> 8 so the CDR that follows starts 4-byte
    // aligned. A server keeps one answer per (source, client_tag) for retries, so two Clients of one service in one
    // context each keep theirs. An older sender writes 0 and gets the old one answer per source; an older server
    // ignores the byte. No tt_VERSION change: neither direction misreads anything.
    uint8_t client_tag;
    // CDR
} __attribute__((packed));

struct tt_CallResponseHeader {
    uint32_t endpoint_id; // endpoint id for client routing
    uint16_t seq_no;      // sequence number
    uint8_t retry;        // retry count from server side
    int8_t return_code;   // return code
    // type + name
    // CDR
} __attribute__((packed));
