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

struct tt_Endpoint;
struct tt_UpdateHeader;
struct tt_Node;
struct tt_Discovery;

// Task Control Block
struct tt_TCB {
    uint64_t time;
    void (*function)(struct tt_Node* node, uint64_t time, void* param);
    void* param;
};

// Fired by a registered struct tt_Discovery (tt_Node_set_discovery()) whenever a remote entity
// appears, is refreshed (a repeat announce - harmless to ignore if a caller only cares about
// appear/depart), or departs (`departed` true - either an explicit farewell UPDATE or
// check_liveliness()'s own timeout). Deliberately minimal (DESIGN.md's "Concurrency" neighbor,
// rmw_tickle/PLAN.md's Milestone 0(c)): `type`/`name` aren't passed here at all - look them up
// via tt_Discovery_find(discovery, node_id, endpoint_id) if/when actually needed, rather than
// paying to decode/copy them for every caller whether they want them or not.
typedef void (*tt_DISCOVERY_CALLBACK)(struct tt_Node* node, uint8_t node_id, uint32_t endpoint_id, uint8_t kind,
                                      bool departed, void* param);

struct tt_Node {
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
    // rationale): entity_id_base is drawn once, at tt_Node_create() time, from tt_get_ns()'s own
    // low 32 bits (no separate RNG primitive needed - this node's own launch instant already is
    // one); next_entity_id starts at 0 and increments once per add_endpoint_to_node() call, one
    // shared counter across every entity kind (Publisher/Subscriber/Client/Server) on this node -
    // functionally equivalent to a per-kind counter (kind is already a separate dispatch
    // dimension elsewhere), just simpler to implement.
    uint32_t entity_id_base;
    uint32_t next_entity_id;

    // Per remote node (indexed by its node id), the last UPDATE announce we've acted on: its
    // last_modified, and whether we've seen it at all. Only these two facts are ever read back
    // (dedup + first-contact detection - see process_update()), so there's no need to keep a
    // malloc'd copy of the whole variable-length announce the way earlier versions did.
    uint64_t update_last_modified[tt_MAX_ENDPOINT_COUNT];
    bool update_seen[tt_MAX_ENDPOINT_COUNT];
    // Phase 2 (rmw_tickle/PLAN.md) - the tt_VERSION last logged as mismatched for each remote node
    // (0 = nothing logged yet), so a peer speaking a different protocol version is reported once
    // rather than once per packet. At max rate an unfiltered log line per rejected packet would be
    // its own denial of service.
    uint8_t version_mismatch_logged[tt_MAX_ENDPOINT_COUNT];
    // Per remote node (indexed the same way), the wall-clock time (tt_get_ns()) its most recent
    // UPDATE announce was received - unlike update_last_modified[] above, this moves on *every*
    // announce, including one whose content is unchanged from the last one acted on. Liveliness
    // (check_liveliness() in tickle.c) is judged from this, not update_last_modified[]: a node
    // whose endpoints never change still has to be heard from periodically, or it's presumed
    // gone once tt_LIVELINESS_MISS_THRESHOLD announce intervals pass with nothing heard.
    uint64_t update_last_seen[tt_MAX_ENDPOINT_COUNT];

    // 4-byte aligned so a decoded/encoded message payload (which sits at a fixed 4-multiple
    // offset past the framing headers) is itself 4-aligned - see "Interface serialization
    // (TickLE CDR-4)" in DESIGN.md. tt_Node already has >= 8-byte alignment (it holds uint64_t
    // members); _Alignas keeps that true for these buffers regardless of member reordering.
    tt_ALIGNAS(4) uint8_t tx_buffer[tt_MAX_BUFFER_LENGTH * 2];
    uint32_t tx_tail;
    uint32_t tx_size;
    // Set whenever node_update()'s always-broadcast UPDATE announce is sitting batched,
    // unflushed, in tx_buffer (cleared once a flush actually sends it) - node_flush() must not
    // unicast while this is true, since tx_buffer is one shared buffer flushed as a unit and an
    // UPDATE has to reach the whole segment, not just a couple of known peers. See node_flush().
    bool tx_has_pending_update;

    tt_ALIGNAS(4) uint8_t rx_buffer[tt_MAX_BUFFER_LENGTH * 2];
    uint32_t rx_tail;
    uint32_t rx_size;

    struct tt_TCB scheduler[tt_MAX_SCHEDULER_LENGTH];
    int32_t scheduler_tail;

    // tt_hal is defined indirectly via <tickle/hal.h>, which includes the
    // platform-specific HAL header (<tickle/hal_linux.h> or <tickle/hal_freertos.h>).
    struct tt_hal hal; // NOLINT(misc-include-cleaner)

    // Opt-in graph introspection (tt_Node_set_discovery(), rmw_tickle/PLAN.md's Milestone 0(c)) -
    // NULL (the default - see reset_node_state()) unless a caller attaches its own, externally-
    // owned struct tt_Discovery. Deliberately *not* an embedded struct tt_Discovery the way
    // update_seen[]/peers[] etc. are: that table can hold real name/type strings for
    // tt_MAX_DISCOVERED_ENTITIES entities, easily several KB, and every tt_Node pays for its own
    // fields whether or not anything ever uses them - a FreeRTOS target with no rmw layer (today,
    // or a future micro-ROS-style thin client whose *agent* - not the constrained device itself -
    // would be the one wanting this) shouldn't carry that weight. Unset, this costs 3 pointers.
    struct tt_Discovery* discovery;
    tt_DISCOVERY_CALLBACK discovery_callback;
    void* discovery_callback_param;
};

struct tt_Endpoint {
    uint8_t kind;
    // hash(topic/service name + endpoint name) - a pure function of the name alone, deliberately:
    // this is how a Publisher and Subscriber (or Client and Server) on two different, otherwise-
    // unacquainted nodes agree on "the same" topic/service with zero negotiation, each computing
    // this independently from the shared name. NOT necessarily unique within one tt_Node any more
    // (Milestone 35, rmw_tickle/PLAN.md) - two local endpoints of the same kind can legitimately
    // share an id if they share a name, see add_endpoint_to_node()'s own doc comment (tickle.c).
    uint32_t id;
    const char* name;

    // Milestone 47 - this specific entity *instance*'s own identity, distinct from id above (a
    // pure name hash, shared by every entity - local or remote - with the same kind+topic/
    // service+endpoint name, by design). Assigned once, in add_endpoint_to_node() (tickle.c), as
    // node->entity_id_base + node->next_entity_id++ - see struct tt_Node's own entity_id_base/
    // next_entity_id doc comment for why that specific combination (a per-launch random base plus
    // a per-node counter, not pure-random or pure-linear alone). Carried on the wire as the
    // *sender's* own identity in struct tt_DataHeader/tt_HeartbeatHeader (both always
    // Publisher-emitted) and as the *target's* own identity in struct tt_AckNackHeader (mirroring
    // that header's own existing endpoint_id "target Publisher" convention) - see each field's own
    // doc comment. This is the real, root-caused fix for a confirmed cross-instance data-mixing
    // gap: before this field existed, a Subscriber's only way to tell two Publishers apart was
    // `id` above, which is identical for any two Publishers sharing a name - two different
    // tt_Node launches, or even two local Publishers on one tt_Node sharing a name (Milestone 35) -
    // see rmw_tickle/PLAN.md's own Milestone 47 for the full incident/design writeup.
    uint32_t entity_id;
};

// A destination this node has learned it can reach directly (see decode_update_entities()'s
// peer-matching, upsert_peer() in tickle.c). node_id doubles as the "slot occupied" flag -
// tt_NODE_ID_INVALID (0) means empty, the same sentinel struct tt_Node's own id already uses
// (valid node ids are 1..254).
struct tt_Peer {
    uint8_t node_id;
    uint32_t ip;   // host byte order, matching tt_receive()'s own sender_ip out-param
    uint16_t port; // host byte order, matching tt_receive()'s own sender_port out-param
};

// One remote entity (a Publisher/Subscriber/Client/Server hosted by some *other* node) this
// node's discovery has recorded - see struct tt_Discovery. node_id doubles as the "slot
// occupied" flag, the same convention struct tt_Peer above uses.
struct tt_DiscoveredEntity {
    uint8_t node_id;
    uint32_t endpoint_id;
    uint8_t kind; // tt_KIND_TOPIC_PUBLISHER / _SUBSCRIBER / SERVICE_CLIENT / _SERVER
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
    // from the same node_id/endpoint_id) flips this back to true, same slot, no separate "was a
    // tombstone" signal - the discovery callback's own existing "appeared, refreshed, or departed"
    // framing (tickle.h's own tt_DISCOVERY_CALLBACK doc comment) already covers a reassert as a
    // refresh, nothing new for a caller to handle. A slot search that finds no truly-empty slot
    // (node_id == tt_NODE_ID_INVALID) falls back to reclaiming the first tombstoned one rather than
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
};

// Fixed-capacity graph cache a caller opts a struct tt_Node into via tt_Node_set_discovery() -
// every remote entity any attached node has announced (not just ones matching a local endpoint
// the way struct tt_Peer's unicast-address tracking is scoped to), for `ros2 topic list`-style
// introspection. Owned by the caller (e.g. embedded in an rmw wrapper's own node struct), not by
// TickLE - see struct tt_Node's own "discovery" field comment on why.
struct tt_Discovery {
    struct tt_DiscoveredEntity entities[tt_MAX_DISCOVERED_ENTITIES];
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
    struct tt_Node* node;
    struct tt_Service* service;
    tt_CLIENT_CALLBACK callback;

    // transcation
    uint16_t seq_no;

    // Cache: fixed-size backing storage for the one outstanding call (tt_Client_call refuses a
    // second call while one is already pending), so a call/retry cycle never has to malloc/free.
    uint8_t cache_buf[tt_MAX_BUFFER_LENGTH * 2];
    struct tt_SubmessageHeader* cache; // NULL when idle, else points into cache_buf
    uint64_t cache_time;               // Cache time
    uint32_t latency;                  // Call latency

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
    struct tt_Node* node;
    struct tt_Service* service;
    tt_SERVER_CALLBACK callback;

    // Fixed backing storage for cached responses (resent as-is if a client retries before
    // seeing one), so caching a response never has to malloc/free on the RPC hot path.
    uint8_t cache_buf[tt_MAX_SERVER_CACHE_COUNT][tt_MAX_BUFFER_LENGTH * 2];
    struct tt_SubmessageHeader* cache[tt_MAX_SERVER_CACHE_COUNT]; // NULL when slot i is unused
    struct server_cache_clean_config clean_config[tt_MAX_SERVER_CACHE_COUNT];
    bool clean_scheduled[tt_MAX_SERVER_CACHE_COUNT]; // Whether clean_config[i]'s timer is pending

    // Milestone 17: one slot per request whose callback returned tt_CALL_DEFERRED - tracked
    // separately from cache[]/cache_buf[] above, which only ever holds an *already-answered*
    // response kept around for retry resends; a deferred request has no encoded response yet, so
    // it needs its own (receiver, seq_no) key instead of one recovered from encoded bytes.
    //
    // slot_state[] is the only field here tt_Server_send_response() (callable from *any* thread,
    // not just the one driving this node's own tt_Node_poll() loop - see that function's own doc
    // comment) ever touches, and only through __atomic_* builtins, never a plain read/write - it
    // is deliberately declared as plain uint8_t, not C11 _Atomic, because this header must stay
    // includable from C++ (rosidl_typesupport_tickle_c/_cpp both do - see tt_ALIGNAS's own
    // comment above for the identical constraint) and <stdatomic.h> is not a C++ header at all.
    // GCC/Clang's __atomic builtins need no special header or type qualifier on either side to
    // work correctly, unlike <stdatomic.h>'s own _Atomic(T) wrapper type.
    uint8_t slot_state[tt_MAX_SERVER_CACHE_COUNT]; // tt_SERVER_SLOT_EMPTY/_PENDING/_READY
    tt_RequestId pending_request_id[tt_MAX_SERVER_CACHE_COUNT];
    uint32_t pending_sender_ip[tt_MAX_SERVER_CACHE_COUNT];   // for the same unicast-the-response
    uint16_t pending_sender_port[tt_MAX_SERVER_CACHE_COUNT]; // optimization process_callrequest() uses
    int8_t pending_return_code[tt_MAX_SERVER_CACHE_COUNT];   // tt_Server_send_response()'s own return_code arg
    uint8_t pending_response_buf[tt_MAX_SERVER_CACHE_COUNT][tt_MAX_BUFFER_LENGTH]; // raw tt_Response bytes
    struct server_cache_clean_config pending_timeout_config[tt_MAX_SERVER_CACHE_COUNT];
    bool pending_timeout_scheduled[tt_MAX_SERVER_CACHE_COUNT];
};

// slot_state[] values - see struct tt_Server's own field comment on why these guard a plain
// uint8_t via __atomic builtins instead of a C11 _Atomic-qualified type.
#define tt_SERVER_SLOT_EMPTY 0   // unused, available for a new deferred request
#define tt_SERVER_SLOT_PENDING 1 // received, callback returned tt_CALL_DEFERRED, no response yet
#define tt_SERVER_SLOT_READY 2   // tt_Server_send_response() filled pending_response_buf[i]

// Answers a request whose tt_SERVER_CALLBACK previously returned tt_CALL_DEFERRED for the given
// request_id - typically called later, from a different thread than the one driving this node's
// own tt_Node_poll() loop (e.g. whatever thread a ROS 2 executor happens to run a service handler
// on), which is the entire reason this function exists rather than just answering synchronously
// like a non-deferred tt_SERVER_CALLBACK already can. See rmw_tickle/PLAN.md's Milestone 17 for
// the full design rationale (why TickLE core, not just rmw_tickle, needs this primitive) and
// DESIGN.md's "Concurrency" section for why this can still be called from another thread without
// adding a lock to struct tt_Node/tt_Server anywhere else: this function, and *only* this
// function among every other tt_Server_*/tt_Node_* entry point, is allowed to touch server-owned
// state from a thread other than the one driving tt_Node_poll() - and even then, only slot_state[]
// itself (via __atomic builtins) and the one slot's own pending_response_buf[i]/pending_request_id[i]
// (safe to write racily-with-respect-to-the-poll-thread because that thread never reads them
// until it has *itself* observed slot_state[i] == tt_SERVER_SLOT_READY via an acquire load, which
// synchronizes-with this function's own release store of that same value - the standard C11
// release/acquire handoff pattern). It never touches node->tx_buffer/tx_tail or any other
// node-owned encode state directly - the actual CDR encode-and-send happens later, from the poll
// thread itself, the next time tt_Node_poll() runs (interrupted early via tt_Node_interrupt(),
// the same primitive Phase 0 already added for the analogous "wake the poll thread promptly from
// another thread" need).
//
// return_code is whatever a synchronous tt_SERVER_CALLBACK would otherwise have returned itself
// (it couldn't - it returned tt_CALL_DEFERRED instead, precisely so the real answer could be
// computed later, possibly on another thread, which is what this call now provides). response is
// only ever *copied* here (a plain byte copy, sized server->service->response_size, into a fixed
// per-slot buffer - never malloc'd) - the real CDR encode into node->tx_buffer happens later, on
// the poll thread, so the caller does not need to keep response alive past this call returning,
// matching every other *_encode-style callback argument in this library.
//
// Returns tt_RET_OK once the response is queued for the poll thread to send (not once it has
// actually gone out - matching tt_Publisher_publish()'s own "queued, not yet necessarily
// flushed" contract), or tt_RET_NOT_FOUND if request_id doesn't match any request still waiting
// on a response (already answered by a previous call, already timed out, or was never deferred).
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
// Caller-owned, the same convention as struct tt_Discovery (tt_Node_set_discovery()): a
// best-effort Publisher (today's only default) leaves tt_Publisher.reliable_cache NULL and pays
// nothing for this; one that wants either policy provides a zeroed struct tt_ReliableCache of its
// own (stack/static/wherever, must stay valid and unmoved until tt_Publisher_destroy() - same
// lifetime rule as every other tt_* struct) and points reliable_cache at it - set directly any
// time after tt_Node_create_publisher() returns, same "caller-owned, plain field access"
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
// TickLE's own comparatively poor tc-loss recovery at high throughput (rmw_tickle/COMPARISON.MD).
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
// Publisher's own DURABLE backlog, keyed by node_id *and* the announcing node's own last_modified
// value as of that delivery - not just by tt_Publisher.peers[]'s own array position, which check_
// liveliness()'s own presumed-dead cleanup (a load-induced false positive, not necessarily a real
// departure) wipes and lets a later upsert_peer() call reuse for an unrelated node_id. Without
// this, the exact same still-alive peer's very next (entirely unchanged) announce looks like a
// brand-new match to register_subscriber_peer_on_publisher() - re-triggering a full backlog
// re-delivery of data that peer already has (observed for real: durability_late_join delivering a
// 20-sample backlog 7 times over, 140 total, correlated with "presumed dead" warnings under load).
// last_modified, not node_id alone, is what tells a genuinely-restarted instance of the same
// node_id (a real new match - its own local subscription state was wiped too, it needs the
// backlog again) apart from the same continuous instance recovering from a transient gap
// (last_modified unchanged, since nothing about its own Publisher/Subscriber set actually
// changed) - tt_Node.last_modified is a tt_get_ns() (monotonic-clock) reading, refreshed every
// time a Publisher/Subscriber/Client/Server is created or destroyed on that node (tickle.c), so a
// genuine process restart reliably lands on a different value than whatever this table last saw,
// while an unchanged, still-running instance keeps announcing the exact same one.
struct tt_DurableDeliveryRecord {
    uint8_t node_id;        // tt_NODE_ID_INVALID (0, matching zero-init) = empty slot
    uint64_t last_modified; // the announcing node's own last_modified as of the delivery below
};
struct tt_ReliableCache {
    // The actual size of the caller-provided index[] array below, in element count - the real
    // per-Publisher ceiling depth is clamped against everywhere in tickle.c (replaces every former
    // bare tt_MAX_RELIABLE_HISTORY reference). 0 (this struct's own zero-init default, before a
    // caller sets index/capacity) is a legitimate, safe "nothing usable yet" state - every depth-
    // clamping call site treats it exactly like "no cache" (see cache_reliable_sample()/deliver_
    // durability_backlog()/process_acknack()'s own shared clamp expression).
    uint16_t capacity;
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
    // Core-private bookkeeping (a caller sets only the five fields above; zero-init = empty).
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
struct tt_PeerAck {
    uint8_t node_id; // tt_NODE_ID_INVALID (0, matching zero-init) = unused entry
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
};

struct tt_Publisher { // extends endpoint
    struct tt_Endpoint endpoint;
    struct tt_Node* node;
    struct tt_Topic* topic;

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
    // yet" pragmatism this file already applies elsewhere (e.g. struct tt_Node.entity_id_base's
    // own doc comment). Distinct from struct tt_Client.seq_no/struct tt_Subscriber.seq_no below -
    // those pair with their own genuinely-16-bit wire counterparts (tt_CallRequestHeader.seq_no)
    // or are unused, not affected by this same bug.
    uint32_t seq_no;

    // Known Subscribers matching this Publisher's topic, learned via UPDATE announces - see
    // tt_UNICAST_PEER_THRESHOLD.
    struct tt_Peer peers[tt_MAX_PEER_COUNT];

    // QoS roadmap #5 (RELIABILITY) follow-up, tt_Publisher_wait_for_all_acked() - what each
    // matched remote node has acknowledged, keyed by node_id rather than index-aligned with
    // peers[] above (Phase 3 prerequisite (c), rmw_tickle/PLAN.md). Index alignment used to mean
    // process_update()'s own forget-then-re-add cycle (a remote node changing *any* endpoint
    // re-announces, and forget_peers_from_source() cleared the slot) threw away ack state for
    // Subscribers that never went anywhere - harmless while nothing depended on it, but Phase 3's
    // KEEP_ALL blocking does: a writer that has to wait for acks must not have them silently reset
    // by an unrelated announce.
    //
    // ack_seq_no is that node's own most recently seen struct tt_AckNackHeader.seq_no - "every
    // seq_no below this has been received" (that struct's own doc comment) - only ever advanced,
    // never regressed by a stale/reordered ACKNACK. 0 means "no ACKNACK seen yet", which seq_no
    // never is on the wire (tt_Publisher_publish() starts at 1). node_id == tt_NODE_ID_INVALID (0)
    // marks an unused entry.
    //
    // Still one entry per remote *node*, not per remote Subscriber: an ACKNACK carries no
    // identifier of the Subscriber that sent it (struct tt_AckNackHeader.entity_id names the
    // target Publisher), so two Subscribers of one topic on one node share this entry and the
    // faster one's ack speaks for both. Harmless for tt_Publisher_wait_for_all_acked()'s own
    // advisory use; a real gap for Phase 3's KEEP_ALL blocking, which needs per-Subscriber
    // identity on the wire - see rmw_tickle/PLAN.md's Phase 3 prerequisite (b).
    struct tt_PeerAck peer_acks[tt_MAX_ACK_ENTRIES];

    // false (tt_Node_create_publisher()'s own default): tt_Publisher_publish() flushes every call
    // immediately, same as RPC already does (DESIGN.md's "RPC and Publish flush immediately by
    // default; batching is opt-in") - lowest latency, and the right default for the common case of
    // one message per publish() call, not several back-to-back to the same destination. true:
    // batch instead, deferring to node_flush()'s own tt_NODE_TX_INTERVAL tick, exactly how every
    // Publisher behaved before this field existed - set this on a specific Publisher that really
    // does call tt_Publisher_publish() several times in a row (a real but unusual pattern) and
    // would rather coalesce those into fewer, larger packets than minimize any one message's own
    // latency. Set directly on the struct any time after tt_Node_create_publisher() returns it -
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

    // Phase 3 - fired when a KEEP_ALL Publisher that had to refuse a write becomes writable again,
    // i.e. when an incoming ACKNACK advances the slowest matched Subscriber far enough. NULL (the
    // default) means "poll tt_Publisher_writable() instead"; both are offered deliberately.
    //
    // Runs on the node's own thread, from inside tt_Node_poll(), so TickLE's single-threaded-per-node
    // discipline holds. It must not publish, create or destroy endpoints, or otherwise re-enter
    // TickLE - signal and return (rmw_tickle wakes a condvar and lets its blocked rmw_publish() do
    // the work). Fired once per refusal-to-writable transition, not once per ACKNACK.
    tt_PUBLISHER_WRITABLE_CALLBACK writable_callback;
    void* writable_callback_param;
    // Core-private: set when a publish was refused, cleared when the callback fires.
    bool writable_pending;

    // NULL (tt_Node_create_publisher()'s own default): no retained-sample storage at all - both
    // reliable/durable below must stay false, nothing for either policy to work from. Non-NULL:
    // storage for whichever of the two policies below is set - see struct tt_ReliableCache's own
    // doc comment above for why one cache backs both.
    struct tt_ReliableCache* reliable_cache;

    // false (tt_Node_create_publisher()'s own default): BEST_EFFORT, today's only default. true:
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

    // false (tt_Node_create_publisher()'s own default): VOLATILE, today's only default. true:
    // DURABLE/TRANSIENT_LOCAL - a newly-discovered Subscriber gets every currently-retained entry
    // in reliable_cache above unicast to it (deliver_durability_backlog(), tickle.c); requires
    // reliable_cache to already be non-NULL too (nothing to deliver from otherwise). Independent
    // of reliable above - a Publisher may set either, both, or neither, the same VOLATILE/
    // TRANSIENT_LOCAL-vs-BEST_EFFORT/RELIABLE independence real DDS QoS allows, just now sharing
    // one cache underneath instead of two.
    bool durable;

    // 0 (tt_Node_create_publisher()'s own default): no periodic Heartbeat, today's only behavior.
    // Non-zero: a struct tt_HeartbeatHeader announce goes out every this-many nanoseconds - see
    // its own doc comment (tickle.h) and tt_Publisher_set_heartbeat_period()'s own doc comment
    // (below) for why this needs that explicit call, not just setting this field directly the way
    // reliable_cache/durable above are. Requires reliable_cache to already be set (nothing to
    // announce for a best-effort Publisher).
    uint64_t heartbeat_period_ns;

    // 0 (tt_Node_create_publisher()'s own default): no periodic ACK solicitation, today's only
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

    // QoS roadmap #6 (LIFESPAN, rmw_tickle/PLAN.md). 0 (tt_Node_create_publisher()'s own default):
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

    // QoS roadmap #2 (DEADLINE) RxO, Milestone 49. 0 (tt_Node_create_publisher()'s own default):
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
    // QoS roadmap #3 (LIVELINESS) RxO, Milestone 49. false (tt_Node_create_publisher()'s own
    // default): AUTOMATIC. true: MANUAL_BY_TOPIC (the only manual kind this package's own rmw
    // layer still supports, Milestone 32's own finding) - see tt_UPDATE_QOS_LIVELINESS_MANUAL's
    // own doc comment (tickle.h) for the wire bit this becomes.
    bool liveliness_manual;
};

// Arms (or re-arms, or disables with period_ns == 0) pub's own periodic Heartbeat announce - see
// struct tt_HeartbeatHeader's own doc comment (tickle.h) for what it's for. Unlike reliable_cache/
// durable (plain caller-owned fields, no function call needed to "activate" them), arming a
// periodic tt_Node_schedule() entry is an active operation with no passive-field equivalent - call
// this any time after tt_Node_create_publisher() returns, once pub->reliable_cache is already set.
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

// True once every currently-matched peer (peers[]) has acknowledged seq_no - i.e. each one's own
// tt_PeerAck.ack_seq_no is strictly greater than it ("every seq_no below this was received", struct
// tt_AckNackHeader's own doc comment). No matched peers at all is vacuously true, matching
// tt_Publisher_request_ack()'s own "nothing to solicit" no-op. A peer that has never sent an
// ACKNACK counts as not having acknowledged anything.
//
// The supported way to read peer_acks[] from outside core (rmw_tickle's own
// rmw_publisher_wait_for_all_acked() is built on this): the table is keyed by node_id, not
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
// tt_Publisher_is_acked_by_all_peers() above: that table is keyed by node_id, not index-aligned
// with peers[], so a caller must not pair the two arrays by index.
uint32_t tt_Publisher_min_acked_seq_no(const struct tt_Publisher* pub);

// Arms (or re-arms, or disables with period_ns == 0) pub's own periodic ACK solicitation - see
// struct tt_Publisher.ack_solicit_period_ns's own doc comment (tickle.h) for what it's for and how
// it differs from tt_Publisher_set_heartbeat_period() above. Same "active scheduler operation, no
// passive-field equivalent" reasoning as that function - call this any time after tt_Node_create_
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
// Publishers on one topic (two different tt_Node launches, or even two local Publishers sharing a
// name, Milestone 35) would silently interleave their independent seq_no streams through one
// shared watermark. Keyed by (node_id, entity_id) - see struct tt_Endpoint.entity_id's own doc
// comment - not just node_id, so two Publisher *instances* on the very same remote node are also
// tracked independently. node_id doubles as the "slot occupied" flag, the same convention struct
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
struct tt_WriterProxy {
    uint8_t node_id;
    uint32_t entity_id;
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
    // vs. FastDDS/CycloneDDS (COMPARISON.MD §3/§6 item 7), not the Publisher's own retained-cache
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
    // Whether acknack_retry() (tickle.c) currently has a tt_Node_schedule() entry pending for
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
    // Phase 3 - tt_get_ns() of the last "still waiting" warning for this writer, so a stuck
    // KEEP_ALL gap is visible in a log at a fixed cadence rather than per retry or never.
    uint64_t stuck_warned_ns;
    // Back-pointer to the owning Subscriber - this entry's own stable address (never moves once
    // claimed; embedded in struct tt_Subscriber.writers[], which lives as long as the Subscriber
    // itself) is what acknack_retry() is scheduled against (tt_Node_schedule(..., acknack_retry,
    // proxy)), so the callback needs a way back to sub->node/sub->endpoint - same {owner, self}
    // pattern struct server_cache_clean_config already uses for an identical reason.
    struct tt_Subscriber* sub;
};

typedef void (*tt_SUBSCRIBER_CALLBACK)(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                       struct tt_Data* data);

struct tt_Subscriber { // extends endpoint
    struct tt_Endpoint endpoint;
    struct tt_Node* node;
    struct tt_Topic* topic;
    tt_SUBSCRIBER_CALLBACK callback;

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
    // NULL/0 (tt_Node_create_subscriber()'s own default) uses builtin_tracking[] below,
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

    // QoS roadmap #5 (RELIABILITY/RELIABLE) - false (tt_Node_create_subscriber()'s own default):
    // best-effort, today's only behavior, process_data() doesn't touch writers[] at all. true:
    // process_data() tracks delivery per matched Publisher and sends ACKNACK back to the sending
    // Publisher on a gap - set directly any time after tt_Node_create_subscriber() returns, same
    // convention as tt_Publisher.batch/.reliable_cache.
    bool reliable;

    // Milestone 47 - one entry per currently-tracked remote Publisher (see struct tt_WriterProxy's
    // own doc comment for the full rationale/history) - replaces this struct's own single flat
    // ack_seq_no/received_bitmap/reliable_sender_*/reliable_retry/reliable_acknack_scheduled/
    // reliable_heartbeat_last_seq_no fields it used to carry directly. All-empty (every slot's
    // node_id == tt_NODE_ID_INVALID) by default - tt_Node_create_subscriber() zeroes this the same
    // way it zeroes/invalidates every other fixed table in this file - entries are claimed lazily,
    // one per distinct (node_id, entity_id) actually heard from, via find_or_create_writer_proxy()
    // (tickle.c).
    struct tt_WriterProxy writers[tt_MAX_PEER_COUNT];
    // The default per-writer tracking windows, used unless tracking_bitmaps above points somewhere
    // wider - exactly the storage each writers[] entry used to embed directly.
    uint64_t builtin_tracking[tt_MAX_PEER_COUNT * tt_RELIABLE_BITMAP_WORDS];

    // QoS roadmap #1 (RxO matching, Milestone 31, rmw_tickle/PLAN.md) - false (tt_Node_create_
    // subscriber()'s own default): this Subscriber accepts a VOLATILE Publisher, today's only
    // behavior. true: requires TRANSIENT_LOCAL - a discovered remote Publisher on this topic whose
    // own announced tt_UpdateEntity.qos doesn't offer tt_UPDATE_QOS_DURABLE is treated as
    // incompatible (process_data()'s own subscriber_incompatible_with_publisher() check) and its
    // DATA is silently never delivered to `callback`, matching real DDS's own "an incompatible
    // pair simply never connects" semantics rather than TickLE's previous "everything matches,
    // durability is just an extra a VOLATILE reader happens to also receive if offered" behavior.
    // Mirrors tt_Publisher.durable's own "offered" half - this is the "requested" half, which
    // (unlike reliable just above) didn't exist on this struct at all before this milestone, since
    // backlog delivery itself was always purely a Publisher-side decision with no reader opt-out.
    bool durable;

    // QoS roadmap #2 (DEADLINE) RxO, Milestone 49 - see tt_Publisher.deadline_duration_ns's own
    // doc comment (tickle.h) for the full reasoning, mirrored here as the "requested" half: 0
    // (tt_Node_create_subscriber()'s own default) means no DEADLINE required.
    uint64_t deadline_duration_ns;
    // QoS roadmap #3 (LIVELINESS) RxO, Milestone 49 - see tt_Publisher.liveliness_lease_duration_ns's
    // own doc comment, mirrored here as the "requested" half: 0 means no specific lease requirement.
    uint64_t liveliness_lease_duration_ns;
    // QoS roadmap #3 (LIVELINESS) RxO, Milestone 49 - see tt_Publisher.liveliness_manual's own doc
    // comment, mirrored here as the "requested" half: false (tt_Node_create_subscriber()'s own
    // default) means this Subscriber accepts AUTOMATIC liveliness; true means it requires
    // MANUAL_BY_TOPIC specifically.
    bool liveliness_manual;
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

// Lifetime / ownership (applies to every tt_Node_create* below):
//   - The library never allocates or copies. Every struct you pass - the tt_Node, the
//     tt_Client/tt_Server/tt_Publisher/tt_Subscriber, its tt_Service or tt_Topic - and every
//     string (endpoint_name, service->name, topic->name) must stay valid and unmoved until the
//     matching tt_*_destroy() (and tt_Node_destroy() for the node). String literals are fine;
//     a stack buffer or one you free() is not.
//   - One tt_Node is single-threaded: all its calls (create/destroy/publish/call/poll) must come
//     from one thread. See DESIGN.md, "Concurrency".
//
// Returns tt_RET_OK on success. tt_Node_create() can also return tt_RET_IILEGAL_NODE_ID (address
// auto-detection found no usable id and none was set in _tt_CONFIG), tt_RET_IO_ERROR (socket
// bind), or tt_RET_OUT_OF_SCHEDULE. The create_* helpers return tt_RET_OUT_OF_BUFFER /
// tt_RET_OUT_OF_SCHEDULE when the node's fixed endpoint table or scheduler is full.
tt_ret_t tt_Node_create(struct tt_Node* node);
tt_ret_t tt_Node_create_client(struct tt_Node* node, struct tt_Client* client, struct tt_Service* service,
                               const char* endpoint_name, tt_CLIENT_CALLBACK callback);
tt_ret_t tt_Node_create_server(struct tt_Node* node, struct tt_Server* server, struct tt_Service* service,
                               const char* endpoint_name, tt_SERVER_CALLBACK callback);
tt_ret_t tt_Node_create_publisher(struct tt_Node* node, struct tt_Publisher* pub, struct tt_Topic* topic,
                                  const char* endpoint_name);
tt_ret_t tt_Node_create_subscriber(struct tt_Node* node, struct tt_Subscriber* sub, struct tt_Topic* topic,
                                   const char* endpoint_name, tt_SUBSCRIBER_CALLBACK callback);
bool tt_Node_schedule(struct tt_Node* node, uint64_t time,
                      void (*function)(struct tt_Node* node, uint64_t time, void* param), void* param);
// Cancels every pending schedule entry matching (function, param) exactly. Returns true if any were removed.
bool tt_Node_unschedule(struct tt_Node* node, void (*function)(struct tt_Node* node, uint64_t time, void* param),
                        void* param);

tt_ret_t tt_Client_call(struct tt_Client* client, struct tt_Request* request);
tt_ret_t tt_Client_destroy(struct tt_Client* client);

tt_ret_t tt_Server_destroy(struct tt_Server* server);

tt_ret_t tt_Publisher_publish(struct tt_Publisher* pub, struct tt_Data* data);
tt_ret_t tt_Publisher_destroy(struct tt_Publisher* pub);

tt_ret_t tt_Subscriber_destroy(struct tt_Subscriber* sub);

/**
 * @node node to poll
 * @timeout wait timeout in nanoseconds
 * @return tt_ret_t
 */
tt_ret_t tt_Node_poll(struct tt_Node* node, int64_t timeout);

// The one exception to every other tt_Node_*/tt_Publisher_*/... call needing to come from the
// same single thread (see this file's own "Concurrency" note, and DESIGN.md's) - this one is
// specifically meant to be called from a *different* thread than whichever one is currently
// blocked in tt_Node_poll(), to make that call return tt_RET_INTERRUPTED right away instead of
// waiting out the rest of its timeout. Meant for a caller that drives tt_Node_poll() from a
// dedicated thread with a long timeout, but sometimes needs that thread to come back and yield to
// other work (e.g. a lock the poll thread also needs) sooner than the timeout would otherwise
// allow. "At least once, at or after this call" - not "only if currently blocked": if nothing is
// blocked in tt_Node_poll() right now, the signal is queued and delivered to whichever
// tt_Node_poll() call comes *next* instead (even one that starts well after this call returns),
// not silently dropped. A caller driving tt_Node_poll() in a continuous loop (the intended usage)
// sees no difference either way; one that calls tt_Node_poll() only occasionally should account
// for an earlier tt_Node_interrupt() still being able to cut its next, unrelated wait short.
tt_ret_t tt_Node_interrupt(struct tt_Node* node);

// Opts `node` into graph introspection: every UPDATE it processes from here on also records the
// announcing entity into `*discovery` (an otherwise-inert struct the caller owns - see its own
// comment) and fires `callback` for an appearance, refresh, or (check_liveliness()/an explicit
// farewell UPDATE) departure. `discovery` must outlive `node`, and must already be zeroed
// (`memset` or `= {0}`) - this does not initialize its contents itself, only points `node` at it.
// `callback`/`param` may be NULL to record without being notified (poll tt_Discovery_find()
// yourself instead). Pass `discovery == NULL` to detach again.
tt_ret_t tt_Node_set_discovery(struct tt_Node* node, struct tt_Discovery* discovery, tt_DISCOVERY_CALLBACK callback,
                               void* param);

// Number of currently-alive occupied slots in `discovery` - for iterating/sizing a snapshot
// without walking the full tt_MAX_DISCOVERED_ENTITIES capacity by hand. Excludes tombstoned
// entries (struct tt_DiscoveredEntity.alive's own doc comment) - a presumed-dead entity is still
// findable via tt_Discovery_find() below, just not counted here, matching this function's own
// "topic list"-style use (you wouldn't want a dead node's own topic still listed).
uint32_t tt_Discovery_count(const struct tt_Discovery* discovery);

// Looks up one specific remote entity by (node_id, endpoint_id), or NULL if it's not currently
// known - never announced, or *normally* departed (an explicit farewell, or dropped from a fresh
// announce). A presumed-dead entity (struct tt_DiscoveredEntity.alive's own doc comment) is still
// returned, with .alive == false, not NULL - check that field to tell the two "not currently
// alive" shapes apart. The returned pointer is only valid until the next UPDATE this node
// processes - copy out anything needed past that point.
const struct tt_DiscoveredEntity* tt_Discovery_find(const struct tt_Discovery* discovery, uint8_t node_id,
                                                    uint32_t endpoint_id);

// QoS roadmap #3 (LIVELINESS) RxO, Milestone 62 (rmw_tickle/PLAN.md's own "DDS semantic-parity
// backlog" row 3) - computes whether `entity` is alive right now, freshly, independent of its own
// `.alive` field's own periodic background sweep (check_liveliness(), tickle.c). check_liveliness()
// watches per-*node* UPDATE traffic on one fixed window (tt_LIVELINESS_MISS_THRESHOLD *
// tt_NODE_UPDATE_INTERVAL, ~3s) - correct for a "did this whole node disappear" participant-level
// signal, but too coarse for an entity that requested its own, different `liveliness_lease_
// duration_ns` (a Publisher/Subscriber field, carried on the wire since Milestone 49 and already
// stored per discovery entry, but never actually consulted for detection before this milestone -
// only for RxO compatibility gating). Two cases:
//   - `entity->liveliness_lease_duration_ns == 0` (no specific lease requested, the common/default
//     case): unchanged from before this function existed - returns `entity->alive` verbatim,
//     deferring entirely to check_liveliness()'s own coarser, node-level sweep, since an entity
//     that never asked for a specific lease has no individual timeout of its own to compute.
//   - non-zero: computed fresh from `node->update_last_seen[entity->node_id]` against that lease
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
//     own scope. `node` must be the same tt_Node `entity`'s own discovery table is attached to.
bool tt_Node_entity_alive(const struct tt_Node* node, const struct tt_DiscoveredEntity* entity, uint64_t now);

tt_ret_t tt_Node_destroy(struct tt_Node* node);

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
#define tt_VERSION 6

struct tt_Header {
    union {
        char magic[2]; // "TK" for big endian, "KT" for little endian
        uint16_t magic_value;
    };
    uint8_t version; // protocol version
    uint8_t source;  // Sender ID
} __attribute__((packed));

#define tt_SUBMESSAGE_ID_ALL 0xff

#define tt_SUBMESSAGE_TYPE_UPDATE 1
#define tt_SUBMESSAGE_TYPE_DATA 2
#define tt_SUBMESSAGE_TYPE_ACKNACK 3
#define tt_SUBMESSAGE_TYPE_CALLREQUEST 4
#define tt_SUBMESSAGE_TYPE_CALLRESPONSE 5
#define tt_SUBMESSAGE_TYPE_HEARTBEAT 6

struct tt_SubmessageHeader {
    uint8_t type;     // 0 for Node update, 2 for Data, 3 for AckNack
    uint8_t receiver; // Receiver ID
    uint16_t length;  // Body length in 4 bytes including header
} __attribute__((packed));

struct tt_UpdateHeader {
    uint64_t last_modified;
    uint8_t entity_count;
    /* Dynamically allocated
    struct tt_UpdateEntity entities[];
    */
} __attribute__((packed));

// QoS roadmap #1 (RxO matching, Milestone 31) - the bits struct tt_UpdateEntity.qos below
// carries, one per policy this package implements a wire-visible mechanism for (services/
// clients always encode 0 here - RELIABILITY there is already unconditional via tt_Client_call()'s
// own retry, no QoS negotiation needed, and DURABILITY/LIVELINESS-kind have no service/client
// analog at all). Same bit positions regardless of direction: on a TOPIC_PUBLISHER entity this is
// what that Publisher *offers* (tt_Publisher.reliable/.durable/.liveliness_manual); on a TOPIC_
// SUBSCRIBER entity it's what that Subscriber *requests* (tt_Subscriber.reliable/.durable/
// .liveliness_manual) - decode_update_entities()'s own tt_KIND_TOPIC_SUBSCRIBER branch and
// process_data()'s own subscriber_incompatible_with_publisher() (both tickle.c) are what actually
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
    uint64_t timestamp;
    // Milestone 47 - the *sending* Publisher's own struct tt_Endpoint.entity_id (see its own doc
    // comment), distinguishing this specific Publisher instance from any other one sharing
    // endpoint_id above. header->source (struct tt_Header, message-level) already narrows this to
    // one remote node; this narrows it further to one specific entity on that node, closing the
    // real, confirmed cross-instance data-mixing gap this milestone exists for.
    uint32_t entity_id;
    // type + name
    // CDR
} __attribute__((packed));

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
    uint8_t reserved;     // pad 7 -> 8 so the CDR payload that follows starts 4-byte aligned
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
