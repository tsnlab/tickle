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

    // Per remote node (indexed by its node id), the last UPDATE announce we've acted on: its
    // last_modified, and whether we've seen it at all. Only these two facts are ever read back
    // (dedup + first-contact detection - see process_update()), so there's no need to keep a
    // malloc'd copy of the whole variable-length announce the way earlier versions did.
    uint64_t update_last_modified[tt_MAX_ENDPOINT_COUNT];
    bool update_seen[tt_MAX_ENDPOINT_COUNT];
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
    uint32_t id; // hash(topic/service name + endpoint name)
    const char* name;
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

// Opt-in per-Publisher retransmission cache for QoS roadmap #5 (RELIABILITY/RELIABLE,
// rmw_tickle/PLAN.md) - caller-owned, the same convention as struct tt_Discovery
// (tt_Node_set_discovery()): a best-effort Publisher (today's only mode) leaves
// tt_Publisher.reliable_cache NULL and pays nothing for this; one that wants RELIABLE provides a
// zeroed struct tt_ReliableCache of its own (stack/static/wherever, must stay valid and unmoved
// until tt_Publisher_destroy() - same lifetime rule as every other tt_* struct) sized by `depth`
// (1..tt_MAX_RELIABLE_HISTORY) and points reliable_cache at it - set directly any time after
// tt_Node_create_publisher() returns, same "caller-owned, plain field access" convention as
// pub->batch. tt_Publisher_publish() appends the raw encoded DATA submessage bytes here after
// every successful send (KEEP_LAST eviction once `depth` slots are full); an incoming ACKNACK
// (process_submessage()) looks samples up here by seq_no to retransmit.
struct tt_ReliableCacheEntry {
    uint32_t seq_no;
    uint16_t len; // encoded submessage length in the matching buffers[] slot; 0 = empty slot
    uint8_t retry;
    uint8_t buffer[tt_MAX_BUFFER_LENGTH]; // raw encoded submessage bytes, resent verbatim on NACK
};
struct tt_ReliableCache {
    uint16_t depth; // in-use ring capacity, 1..tt_MAX_RELIABLE_HISTORY
    uint16_t next;  // next entries[] slot tt_Publisher_publish() writes into (mod depth)
    struct tt_ReliableCacheEntry entries[tt_MAX_RELIABLE_HISTORY];
};

struct tt_Publisher { // extends endpoint
    struct tt_Endpoint endpoint;
    struct tt_Node* node;
    struct tt_Topic* topic;

    // transcation
    uint16_t seq_no;

    // Known Subscribers matching this Publisher's topic, learned via UPDATE announces - see
    // tt_UNICAST_PEER_THRESHOLD.
    struct tt_Peer peers[tt_MAX_PEER_COUNT];

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

    // NULL (tt_Node_create_publisher()'s own default): best-effort, today's only behavior. Non-
    // NULL: RELIABLE - see struct tt_ReliableCache's own doc comment above.
    struct tt_ReliableCache* reliable_cache;
};

struct tt_Subscriber;
typedef void (*tt_SUBSCRIBER_CALLBACK)(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                       struct tt_Data* data);

struct tt_Subscriber { // extends endpoint
    struct tt_Endpoint endpoint;
    struct tt_Node* node;
    struct tt_Topic* topic;
    tt_SUBSCRIBER_CALLBACK callback;

    // transcation
    uint16_t seq_no;

    // QoS roadmap #5 (RELIABILITY/RELIABLE) - false (tt_Node_create_subscriber()'s own default):
    // best-effort, today's only behavior, process_data() doesn't touch ack_seq_no/received_bitmap
    // at all. true: process_data() tracks delivery and sends ACKNACK back to the sending
    // Publisher on a gap - set directly any time after tt_Node_create_subscriber() returns, same
    // convention as tt_Publisher.batch/.reliable_cache.
    bool reliable;
    // Cumulative-ack watermark: the next wire seq_no not yet confirmed delivered to `callback` -
    // every seq_no < ack_seq_no has been. Initialized to 1 by tt_Node_create_subscriber() since a
    // Publisher's own seq_no starts posting from 1, never 0 (tt_Publisher_publish()'s
    // data_header->seq_no = pub->seq_no + 1).
    uint32_t ack_seq_no;
    // bit j set: sample (ack_seq_no + j) has already been received out of order, ahead of the
    // cumulative watermark - matches tt_AckNackHeader's own "bit j: seq_no + j" wire convention
    // exactly (bit 0 is ack_seq_no itself, always 0 here since ack_seq_no only ever advances once
    // confirmed received - see update_reliable_ack()'s own comment on why that still needs its
    // own explicit realigning shift, not just a plain compare), so building the wire "please
    // resend" bitmap is a straight ~received_bitmap, no additional offset.
    uint64_t received_bitmap;
    // Address an outstanding-gap ACKNACK retry (acknack_retry(), tickle.c) resends to - the most
    // recent reliable DATA sender, since a scheduled retry fires outside process_packet()'s own
    // call stack and so no longer has that packet's header/sender_ip/sender_port at hand. Scoped
    // to one Publisher per reliable Subscriber (documented limitation, DESIGN.md/PLAN.md): with
    // several Publishers on one topic, their independent seq_no streams would interleave and
    // this single-watermark tracking would misjudge gaps - the common case rmw_tickle needs
    // (one Publisher, one or more reliable Subscribers) is unaffected.
    uint8_t reliable_sender_node_id;
    uint32_t reliable_sender_ip;
    uint16_t reliable_sender_port;
    // How many ACKNACK retries have been sent for the *current* outstanding gap - reset to 0 when
    // a new gap first opens, capped at tt_RELIABLE_RETRY (mirrors call_retry()'s own
    // client->service->call_retry_count check) before this Subscriber gives up on that sample.
    uint8_t reliable_retry;
    // Whether acknack_retry() (tickle.c) currently has a tt_Node_schedule() entry pending for
    // this Subscriber - mirrors struct tt_Client.cache's own "is a retry timer armed right now"
    // role, needed so a burst of DATA packets while a gap is open doesn't schedule a new timer
    // per packet.
    bool reliable_acknack_scheduled;
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

// Number of occupied slots in `discovery` - for iterating/sizing a snapshot without walking the
// full tt_MAX_DISCOVERED_ENTITIES capacity by hand.
uint32_t tt_Discovery_count(const struct tt_Discovery* discovery);

// Looks up one specific remote entity by (node_id, endpoint_id), or NULL if it's not currently
// known (never announced, or already departed). The returned pointer is only valid until the
// next UPDATE this node processes - copy out anything needed past that point.
const struct tt_DiscoveredEntity* tt_Discovery_find(const struct tt_Discovery* discovery, uint8_t node_id,
                                                    uint32_t endpoint_id);

tt_ret_t tt_Node_destroy(struct tt_Node* node);

#define tt_VERSION 1

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

struct tt_UpdateEntity {
    uint32_t endpoint_id; // hash(topic/service name + endpoint name)
    uint8_t kind;
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
    // type + name
    // CDR
} __attribute__((packed));

struct tt_AckNackHeader {
    uint32_t endpoint_id; // target Publisher - same leading-field convention as tt_DataHeader/
                          // tt_CallRequestHeader/tt_CallResponseHeader (one node can host many
                          // endpoints, so the submessage receiver alone isn't enough)
    uint32_t seq_no;      // cumulative ack: every seq_no below this was received
    uint64_t bitmap;      // bit j set: (seq_no + j) is still missing, please resend - same
                          // direction as RTPS's own AckNack SequenceNumberSet
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
