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

// Task Control Block
struct tt_TCB {
    uint64_t time;
    void (*function)(struct tt_Node* node, uint64_t time, void* param);
    void* param;
};

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

    struct tt_UpdateHeader* updates[tt_MAX_ENDPOINT_COUNT];

    uint8_t tx_buffer[tt_MAX_BUFFER_LENGTH * 2];
    uint32_t tx_tail;
    uint32_t tx_size;
    // Set whenever node_update()'s always-broadcast UPDATE announce is sitting batched,
    // unflushed, in tx_buffer (cleared once a flush actually sends it) - node_flush() must not
    // unicast while this is true, since tx_buffer is one shared buffer flushed as a unit and an
    // UPDATE has to reach the whole segment, not just a couple of known peers. See node_flush().
    bool tx_has_pending_update;

    uint8_t rx_buffer[tt_MAX_BUFFER_LENGTH * 2];
    uint32_t rx_tail;
    uint32_t rx_size;

    struct tt_TCB scheduler[tt_MAX_SCHEDULER_LENGTH];
    int32_t scheduler_tail;

    // tt_hal is defined indirectly via <tickle/hal.h>, which includes the
    // platform-specific HAL header (<tickle/hal_linux.h> or <tickle/hal_freertos.h>).
    struct tt_hal hal; // NOLINT(misc-include-cleaner)
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

struct tt_Service;
struct tt_Client;
struct tt_Response;
struct tt_SubmessageHeader;

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

typedef int8_t (*tt_SERVER_CALLBACK)(struct tt_Server* server, struct tt_Request* request,
                                     struct tt_Response* response);

// Identifies which server/slot a scheduled server_cache_clean timer belongs to. Embedded (one
// per slot) in struct tt_Server so scheduling that timer never needs a malloc either.
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
};

// tt_Request / tt_Response / tt_Data are opaque bases: the application defines the real,
// generated struct for its own message type and hands the library a pointer to it, which the
// per-type encode/decode callbacks cast back. The single reserved byte is only there so these
// are valid ISO C (an empty struct is a GNU extension - a consumer building the public headers
// with -std=c99 -pedantic-errors would otherwise fail to compile them).
struct tt_Request {
    char _reserved;
};

struct tt_Response {
    char _reserved;
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
    char _reserved; // see tt_Request's own note - opaque base, one byte only to stay valid ISO C
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
    uint32_t seq_no;
    uint64_t bitmap;
    // type + name
} __attribute__((packed));

struct tt_CallRequestHeader {
    uint32_t endpoint_id; // endpoint id for service server lookup
    uint16_t seq_no;      // sequence number
    uint8_t retry;        // retry count from client side
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
