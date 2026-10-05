/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: process_callrequest() (and the resend_cached_response()/encode_call_response() split
// it dispatches to, plus Milestone 17's own defer_call_response()/tt_Server_send_response()/
// tt_Server_send_response() deferred-response path) are static. This is the RPC server-side hot
// path exercised on real hardware by the SetBool example, but not by the HIL CI (which only runs
// pub/sub examples) or any other test file, so it needs its own direct coverage.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define ENDPOINT_ID 0xaabbccdd

static int callback_count = 0;
static int8_t stub_return_code = 0;

static int32_t stub_request_decode(struct tt_Request* request, const uint8_t* payload, const uint32_t len,
                                   bool is_native_endian) {
    (void)request;
    (void)payload;
    (void)is_native_endian;
    return (int32_t)len;
}

static void stub_request_free(struct tt_Request* request) {
    (void)request;
}

static int8_t stub_server_callback(struct tt_Server* server, struct tt_Request* request, struct tt_Response* response,
                                   tt_RequestId request_id) {
    (void)server;
    (void)request;
    (void)response;
    (void)request_id;
    callback_count++;
    return stub_return_code;
}

static int32_t stub_response_encode_size(struct tt_Response* response) {
    (void)response;
    return 1;
}

static int32_t stub_response_encode(struct tt_Response* response, uint8_t* payload, const uint32_t len) {
    (void)response;
    if (len < 1) {
        return -1;
    }
    payload[0] = 0;
    return 1;
}

static void stub_response_free(struct tt_Response* response) {
    (void)response;
}

static void init_node_service_server(struct tt_Context* node, struct tt_Service* service, struct tt_Server* server) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;

    memset(service, 0, sizeof(*service));
    service->name = "test_service";
    service->request_size = 1;
    service->response_size = 1;
    service->request_decode = stub_request_decode;
    service->request_free = stub_request_free;
    service->response_encode_size = stub_response_encode_size;
    service->response_encode = stub_response_encode;
    service->response_free = stub_response_free;

    memset(server, 0, sizeof(*server));
    server->endpoint.kind = tt_KIND_SERVICE_SERVER;
    server->endpoint.id = ENDPOINT_ID;
    server->node = node;
    server->service = service;
    server->callback = stub_server_callback;

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)server;
}

// Builds a CallRequestHeader + 1-byte body at the start of node->rx_buffer, returning the tail
// offset (matching what process_packet() would have handed process_callrequest()).
static uint32_t write_callrequest(struct tt_Context* node, uint16_t seq_no, uint8_t retry) {
    struct tt_CallRequestHeader* callrequest_header = (struct tt_CallRequestHeader*)node->rx_buffer;
    callrequest_header->endpoint_id = ENDPOINT_ID;
    callrequest_header->seq_no = seq_no;
    callrequest_header->retry = retry;

    uint32_t tail = sizeof(struct tt_CallRequestHeader);
    node->rx_buffer[tail] = 0xab; // 1-byte request body (service->request_size == 1)
    return tail + 1;
}

// A fresh request (no cached response yet) must invoke the service callback exactly once and
// send exactly one response.
static void test_fresh_request_invokes_callback_and_sends(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = 0;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_callrequest(&node, 1, 0);

    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(1, (uint32_t)callback_count);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail); // drained back down by the immediate flush
}

// A retry of the same (receiver, seq_no) - the client asking again because it hasn't seen the
// first response yet - must hit the cache: no second callback invocation, but a second send
// (the resent, retry-bumped cached response).
static void test_retry_hits_cache_without_recalling_callback(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = 0;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_callrequest(&node, 7, 0);
    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(1, (uint32_t)callback_count);

    // Same seq_no again, as a client retrying before seeing the first response would send.
    tail = write_callrequest(&node, 7, 1);
    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));

    EXPECT_EQ_U32(1, (uint32_t)callback_count);            // NOT called again - served from cache
    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_call_count); // but still resent
}

// A fresh request's response must be unicast straight back to the request's own source, not
// broadcast to the rest of the segment that never asked - see process_callrequest()'s own
// comment on why that's safe whenever tx_buffer was empty before this response (init_node_
// service_server() sets tx_tail to exactly that baseline, same as a real freshly-flushed node).
static void test_fresh_request_response_is_unicast_to_sender(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = 0;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_callrequest(&node, 1, 0);
    uint32_t sender_ip = 0xc0a80a02; // 192.168.10.2
    uint16_t sender_port = 8282;

    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, sender_ip, sender_port));
    EXPECT_EQ_U32(1, (uint32_t)callback_count);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // unicast, not tt_send()'s broadcast
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);    // and only once, not also broadcast
    EXPECT_EQ_U32(sender_ip, test_mock_send_to_last_ip);
    EXPECT_EQ_U32((uint32_t)sender_port, (uint32_t)test_mock_send_to_last_port);
}

// An endpoint_id nobody registered (e.g. meant for a different node sharing the broadcast
// domain) must be ignored, not treated as an error, and must not invoke any callback.
static void test_unknown_endpoint_is_ignored(void) {
    test_mock_reset();
    callback_count = 0;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    struct tt_CallRequestHeader* callrequest_header = (struct tt_CallRequestHeader*)node.rx_buffer;
    callrequest_header->endpoint_id = 0xdeadbeef; // not ENDPOINT_ID
    callrequest_header->seq_no = 1;
    callrequest_header->retry = 0;
    uint32_t tail = sizeof(struct tt_CallRequestHeader);

    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(0, (uint32_t)callback_count);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_call_count);
}

// Milestone 17 (rmw_tickle/PLAN.md): a callback returning tt_CALL_DEFERRED must not send anything
// immediately. Since 2026-09-27 the later tt_Server_send_response() sends the answer itself, before it returns
// (it used to leave it READY for the poll thread's own flush_pending_
// responses() (called directly here, whitebox, standing in for tt_Context_poll()) must still get the
// real answer out - proving the deferred-then-answered path end to end.
static void test_deferred_request_answered_later_is_sent(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = tt_CALL_DEFERRED;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_callrequest(&node, 42, 0);

    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(1, (uint32_t)callback_count);            // callback ran once, to learn it should defer
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_call_count); // but nothing sent yet
    EXPECT_EQ_U32((uint32_t)tt_SERVER_SLOT_PENDING, (uint32_t)server.slot_state[0]);

    uint8_t response_byte = 0;
    tt_RequestId request_id = {REMOTE_NODE_ID, 42};
    EXPECT_TRUE(tt_RET_OK == tt_Server_send_response(&server, request_id, 0, (struct tt_Response*)&response_byte));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);                         // sent by the call itself
    EXPECT_EQ_U32((uint32_t)tt_SERVER_SLOT_EMPTY, (uint32_t)server.slot_state[0]); // and the slot reclaimed
    EXPECT_TRUE(!server.pending_timeout_scheduled[0]);                             // with its timeout

    (void)tt_Context_poll(&node, 0);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count); // nothing left for a poll to send
}

// The same path with tt_Context_poll() around it: the response goes out from tt_Server_send_response() itself, so no
// poll is needed for it and none sends it twice; nothing is left READY for a poll, which before the 2026-09-27
// change was the poll's job (flush_pending_responses(), gated by D3's counter, both gone with it).
static void test_deferred_response_is_sent_by_send_response_itself(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = tt_CALL_DEFERRED;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;
    uint32_t tail = write_callrequest(&node, 42, 0);
    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));

    (void)tt_Context_poll(&node, 0);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_call_count); // nothing ready, nothing sent

    uint8_t response_byte = 0;
    tt_RequestId request_id = {REMOTE_NODE_ID, 42};
    EXPECT_TRUE(tt_RET_OK == tt_Server_send_response(&server, request_id, 0, (struct tt_Response*)&response_byte));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);

    (void)tt_Context_poll(&node, 0);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32((uint32_t)tt_SERVER_SLOT_EMPTY, (uint32_t)server.slot_state[0]);
}

// The flip side: nobody ever calls tt_Server_send_response() for a deferred request -
// pending_response_timeout() (the tt_Context_schedule() callback, invoked directly here rather than
// via a real elapsed wait - test_liveliness.c's own check_liveliness() tests already establish
// this same "call the timer callback directly" whitebox pattern) must reclaim the slot instead of
// leaking it forever, and a tt_Server_send_response() that arrives after that must cleanly report
// tt_RET_NOT_FOUND rather than resurrecting a slot nothing is listening for the response of.
static void test_deferred_request_timeout_reclaims_slot(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = tt_CALL_DEFERRED;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_callrequest(&node, 7, 0);
    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32((uint32_t)tt_SERVER_SLOT_PENDING, (uint32_t)server.slot_state[0]);

    pending_response_timeout(&node, 0, &server.pending_timeout_config[0]);
    EXPECT_EQ_U32((uint32_t)tt_SERVER_SLOT_EMPTY, (uint32_t)server.slot_state[0]);

    uint8_t response_byte = 0;
    tt_RequestId request_id = {REMOTE_NODE_ID, 7};
    EXPECT_TRUE(tt_RET_NOT_FOUND ==
                tt_Server_send_response(&server, request_id, 0, (struct tt_Response*)&response_byte));
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_call_count); // never sent - it was already given up on
}

// A retry (client hasn't seen an answer yet, so it asks again) for a request that's already
// deferred must not invoke the callback a second time - the real answer is already on its way
// whenever tt_Server_send_response() gets called, same as a retry against an *already-cached*
// response already doesn't (test_retry_hits_cache_without_recalling_callback() above).
static void test_retry_while_deferred_does_not_recall_callback(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = tt_CALL_DEFERRED;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t tail = write_callrequest(&node, 9, 0);
    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));
    EXPECT_EQ_U32(1, (uint32_t)callback_count);

    // Same seq_no again, as a client retrying before seeing any response would send.
    tail = write_callrequest(&node, 9, 1);
    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));

    EXPECT_EQ_U32(1, (uint32_t)callback_count);            // NOT called again
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_call_count); // still nothing to send
}

// A response that points at data it does not own (a string, as every generated TickLE struct holds one) must go
// out as it was when tt_Server_send_response() was called, whatever becomes of that data afterwards. rmw_tickle's
// service responses alias the ROS response object's strings, which rclcpp destroys as soon as rmw_send_response()
// returns; the deferred path encoded them at the next poll, from freed memory, and every string past the 15 bytes
// std::string keeps inline arrived as garbage (2026-09-27, `ros2 service call .../list_parameters`).
struct aliasing_response {
    const char* text;
};

static int32_t aliasing_response_encode_size(struct tt_Response* response) {
    return (int32_t)strlen(((struct aliasing_response*)response)->text) + 1;
}

static int32_t aliasing_response_encode(struct tt_Response* response, uint8_t* payload, const uint32_t len) {
    const char* text = ((struct aliasing_response*)response)->text;
    uint32_t bytes = (uint32_t)strlen(text) + 1;
    if (len < bytes) {
        return -1;
    }
    memcpy(payload, text, bytes);
    return (int32_t)bytes;
}

static uint8_t captured[tt_MAX_BUFFER_LENGTH];
static size_t captured_len;

static void capture_sent(const void* buf, size_t len) {
    captured_len = len < sizeof(captured) ? len : sizeof(captured);
    memcpy(captured, buf, captured_len);
}

static bool captured_contains(const char* text) {
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= captured_len; i++) {
        if (memcmp(captured + i, text, n) == 0) {
            return true;
        }
    }
    return false;
}

static void test_deferred_response_is_sent_as_it_was_at_send_response(void) {
    test_mock_reset();
    test_mock_send_hook = capture_sent;
    captured_len = 0;
    callback_count = 0;
    stub_return_code = tt_CALL_DEFERRED;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);
    service.response_size = sizeof(struct aliasing_response);
    service.response_encode_size = aliasing_response_encode_size;
    service.response_encode = aliasing_response_encode;

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;
    uint32_t tail = write_callrequest(&node, 42, 0);
    EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));

    // The caller's own string, gone - overwritten, as a freed heap block is - as soon as the call returns.
    char text[] = "qos_overrides./parameter_events.publisher.depth";
    struct aliasing_response response = {text};
    tt_RequestId request_id = {REMOTE_NODE_ID, 42};
    EXPECT_TRUE(tt_RET_OK == tt_Server_send_response(&server, request_id, 0, (struct tt_Response*)&response));
    memset(text, 'X', sizeof(text) - 1);

    (void)tt_Context_poll(&node, 0);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_TRUE(captured_contains("qos_overrides./parameter_events.publisher.depth"));
    EXPECT_TRUE(!captured_contains("XXXXXXXX"));
    test_mock_send_hook = NULL;
}

// Every deferred request arms a timeout on its slot; answering it must disarm that timeout. Until 2026-09-27 the
// answer left it armed for tt_SERVER_DEFERRED_RESPONSE_TIMEOUT (5 s), one per call, and the 122nd call inside those
// 5 s found the scheduler full: "Cannot schedule pending_response_timeout", the request could not be deferred,
// rmw_send_response() failed, and rclcpp terminated the server process. Far more calls than the scheduler holds,
// each answered at once, must all succeed.
#define MANY_CALLS (3 * tt_MAX_SCHEDULER_LENGTH)
static void test_many_answered_calls_do_not_fill_the_scheduler(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = tt_CALL_DEFERRED;

    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    uint32_t answered = 0;
    for (uint16_t seq = 1; seq <= MANY_CALLS; seq++) {
        uint32_t tail = write_callrequest(&node, seq, 0);
        EXPECT_TRUE(process_callrequest(&node, &header, node.rx_buffer, 0, tail, 0, 0));
        uint8_t response_byte = 0;
        tt_RequestId request_id = {REMOTE_NODE_ID, seq};
        answered += tt_Server_send_response(&server, request_id, 0, (struct tt_Response*)&response_byte) == tt_RET_OK;
    }
    EXPECT_EQ_U32(MANY_CALLS, answered);
    EXPECT_EQ_U32(MANY_CALLS, (uint32_t)test_mock_send_call_count);
}

// --- How long an answered response is kept (ROADMAP "Now" 5a, 2026-10-05) ---------------------------------------
// It used to be a fixed tt_SERVER_CACHE_TIMEOUT (100 ms), fitted to the rig and already shorter than the 250 ms the
// client's own retry interval could reach. The server cannot see the client's srtt, so it keeps a response for the
// longest of what it can know: the client's retry schedule before any answer (the seed, from the shared defaults or
// this service's explicit call_retry_*), and a multiple of the gaps it has actually seen between a response going
// out and the same client asking again. Each pin names the mutant it kills.

#define US 1000ULL
#define MS 1000000ULL

// Runs every scheduler entry due by `to`, on the mock clock.
static void advance_to(struct tt_Context* node, uint64_t to) {
    for (;;) {
        struct tt_TCB* due = peek_scheduler(node);
        if (due == NULL || due->time > to) {
            break;
        }
        struct tt_TCB fire = *due;
        pop_scheduler(node);
        test_mock_now = fire.time;
        fire.function(node, fire.time, fire.param);
    }
    test_mock_now = to;
}

static void ask(struct tt_Context* node, uint8_t source, uint16_t seq_no, uint8_t retry, uint64_t at) {
    advance_to(node, at);
    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = source;
    uint32_t tail = write_callrequest(node, seq_no, retry);
    EXPECT_TRUE(process_callrequest(node, &header, node->rx_buffer, 0, tail, 0, 0));
}

static void setup_lifetime(struct tt_Context* node, struct tt_Service* service, struct tt_Server* server) {
    test_mock_reset();
    test_mock_now = 1000 * MS;
    callback_count = 0;
    stub_return_code = 0;
    init_node_service_server(node, service, server);
}

// Before it has seen any client retry, the server keeps a response for the default client's whole seed schedule:
// 5 + 10 + 20 + 40 ms, the waits of a client with no answer yet. A retry at the end of it is served from the cache.
static void test_the_seed_lifetime_is_the_default_clients_schedule(void) {
    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    setup_lifetime(&node, &service, &server);
    uint64_t window = ((2ULL << tt_CALL_RETRY_COUNT) - 1) * tt_CALL_RETRY_INTERVAL;
    EXPECT_EQ_U64(window, server_cache_lifetime(&server));
    uint64_t t0 = test_mock_now;
    ask(&node, REMOTE_NODE_ID, 1, 0, t0);
    ask(&node, REMOTE_NODE_ID, 1, tt_CALL_RETRY_COUNT, t0 + window - US); // retries 1..count-1 were lost
    EXPECT_EQ_U32(1, (uint32_t)callback_count);
}

// A service with an explicit call_retry_interval tells the server the client's schedule exactly: (count + 1) waits
// of that interval - the formula tt_SERVER_CACHE_TIMEOUT's own comment gave and nobody computed. Killed by: the
// fixed 100 ms (a retry at 150 ms re-runs the callback), and by the seed ignoring the explicit interval (75 ms).
static void test_an_explicit_interval_sets_the_lifetime(void) {
    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    setup_lifetime(&node, &service, &server);
    service.call_retry_interval = 50 * MS;
    EXPECT_EQ_U64(4ULL * 50 * MS, server_cache_lifetime(&server));
    uint64_t t0 = test_mock_now;
    ask(&node, REMOTE_NODE_ID, 1, 0, t0);
    ask(&node, REMOTE_NODE_ID, 1, 3, t0 + (150 * MS)); // only the third retry gets through
    EXPECT_EQ_U32(1, (uint32_t)callback_count);
}

// A client whose retries are 60, 120 and 240 ms apart (60 ms first interval, doubled per retry) finds its response
// every time: each hit re-arms the entry, for a multiple of the gap it just measured. Killed by: the fixed 100 ms
// (the 180 ms retry misses), the re-arm removed (the entry dies at 75 ms), and the re-arm without the measured gap
// (it dies 75 ms after the first hit, before the second).
static void test_a_retrying_client_keeps_its_response(void) {
    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    setup_lifetime(&node, &service, &server);
    uint64_t t0 = test_mock_now;
    ask(&node, REMOTE_NODE_ID, 1, 0, t0);
    ask(&node, REMOTE_NODE_ID, 1, 1, t0 + (60 * MS));
    ask(&node, REMOTE_NODE_ID, 1, 2, t0 + (180 * MS));
    ask(&node, REMOTE_NODE_ID, 1, 3, t0 + (420 * MS));
    EXPECT_EQ_U32(1, (uint32_t)callback_count);
    EXPECT_EQ_U32(4, (uint32_t)test_mock_send_call_count); // the answer and three resends
}

// A client slower than anything the server has seen misses once - its retry arrives after the entry expired - and
// the server learns from that miss: the expired entry still names (client, seq_no) and when it went out. The next
// call's response is then kept long enough. Killed by: the learning from an expired entry removed (the second call
// re-runs its callback too), and by the fixed 100 ms.
static void test_a_slow_client_is_learnt_from_one_miss(void) {
    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    setup_lifetime(&node, &service, &server);
    uint64_t t0 = test_mock_now;
    ask(&node, REMOTE_NODE_ID, 1, 0, t0);
    ask(&node, REMOTE_NODE_ID, 1, 1, t0 + (200 * MS)); // after the 75 ms seed: a miss, the callback runs again
    EXPECT_EQ_U32(2, (uint32_t)callback_count);
    ask(&node, REMOTE_NODE_ID, 2, 0, t0 + (300 * MS));
    ask(&node, REMOTE_NODE_ID, 2, 1, t0 + (500 * MS)); // the same 200 ms gap: now a hit
    EXPECT_EQ_U32(3, (uint32_t)callback_count);
}

// What one miss can teach is bounded: a gap measured against a long-expired entry - a client that restarted and
// reused the id, say - counts as at most twice the current lifetime, not as itself. Killed by: the clamp removed
// (the lifetime would become GAP_MULTIPLE x 10 s).
static void test_one_miss_cannot_inflate_the_lifetime_without_bound(void) {
    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    setup_lifetime(&node, &service, &server);
    uint64_t seed = server_cache_lifetime(&server);
    uint64_t t0 = test_mock_now;
    ask(&node, REMOTE_NODE_ID, 1, 0, t0);
    ask(&node, REMOTE_NODE_ID, 1, 1, t0 + (10000 * MS));
    EXPECT_TRUE(server_cache_lifetime(&server) > seed); // it did learn
    EXPECT_TRUE(server_cache_lifetime(&server) <= (uint64_t)tt_SERVER_CACHE_GAP_MULTIPLE * 2 * seed);
}

// A first transmission (retry 0) is never answered from the cache: only a retry can be the same call again. A client
// node that restarts under the same id starts its seq_no at 0 again, and with a cache that lives as long as the
// client may retry, its first call would otherwise get the previous incarnation's answer. Killed by: the retry check
// removed (the second call is answered from the cache, the callback runs once).
static void test_a_first_transmission_is_never_served_from_the_cache(void) {
    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    setup_lifetime(&node, &service, &server);
    uint64_t t0 = test_mock_now;
    ask(&node, REMOTE_NODE_ID, 0, 0, t0);
    ask(&node, REMOTE_NODE_ID, 0, 0, t0 + MS); // the restarted client's own seq_no 0
    EXPECT_EQ_U32(2, (uint32_t)callback_count);
}

// With every slot holding a live response, a new client's answer evicts the one sent longest ago instead of not
// being sent at all ("Out of server cache slots" used to roll the response back). Killed by: the eviction removed.
static void test_full_slots_evict_the_oldest_response(void) {
    struct tt_Context node;
    struct tt_Service service;
    struct tt_Server server;
    setup_lifetime(&node, &service, &server);
    uint64_t t0 = test_mock_now;
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        ask(&node, (uint8_t)(10 + i), 1, 0, t0 + ((uint64_t)i * US));
    }
    uint8_t newcomer = (uint8_t)(10 + tt_MAX_SERVER_CACHE_COUNT);
    ask(&node, newcomer, 1, 0, t0 + MS);
    EXPECT_EQ_U32(tt_MAX_SERVER_CACHE_COUNT + 1, (uint32_t)test_mock_send_call_count);
    EXPECT_TRUE(get_server_cache(&server, newcomer, 1) != NULL);
    EXPECT_TRUE(get_server_cache(&server, 10, 1) == NULL); // the oldest went
    EXPECT_TRUE(get_server_cache(&server, 11, 1) != NULL); // and only it
}

int main(void) {
    test_fresh_request_invokes_callback_and_sends();
    test_retry_hits_cache_without_recalling_callback();
    test_fresh_request_response_is_unicast_to_sender();
    test_unknown_endpoint_is_ignored();
    test_deferred_request_answered_later_is_sent();
    test_deferred_response_is_sent_by_send_response_itself();
    test_deferred_request_timeout_reclaims_slot();
    test_retry_while_deferred_does_not_recall_callback();
    test_deferred_response_is_sent_as_it_was_at_send_response();
    test_many_answered_calls_do_not_fill_the_scheduler();
    test_the_seed_lifetime_is_the_default_clients_schedule();
    test_an_explicit_interval_sets_the_lifetime();
    test_a_retrying_client_keeps_its_response();
    test_a_slow_client_is_learnt_from_one_miss();
    test_one_miss_cannot_inflate_the_lifetime_without_bound();
    test_a_first_transmission_is_never_served_from_the_cache();
    test_full_slots_evict_the_oldest_response();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_process_callrequest: all tests passed\n");
    return 0;
}
