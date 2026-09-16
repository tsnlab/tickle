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
// flush_pending_responses() deferred-response path) are static. This is the RPC server-side hot
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

static void init_node_service_server(struct tt_Node* node, struct tt_Service* service, struct tt_Server* server) {
    memset(node, 0, sizeof(*node));
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
static uint32_t write_callrequest(struct tt_Node* node, uint16_t seq_no, uint8_t retry) {
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

    struct tt_Node node;
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

    struct tt_Node node;
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

    struct tt_Node node;
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

    struct tt_Node node;
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
// immediately, but a later tt_Server_send_response() + the poll thread's own flush_pending_
// responses() (called directly here, whitebox, standing in for tt_Node_poll()) must still get the
// real answer out - proving the deferred-then-answered path end to end.
static void test_deferred_request_answered_later_is_sent(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = tt_CALL_DEFERRED;

    struct tt_Node node;
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
    EXPECT_EQ_U32((uint32_t)tt_SERVER_SLOT_READY, (uint32_t)server.slot_state[0]);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_call_count); // still not sent - that's flush's job

    flush_pending_responses(&node);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32((uint32_t)tt_SERVER_SLOT_EMPTY, (uint32_t)server.slot_state[0]); // slot reclaimed after flush
}

// The flip side: nobody ever calls tt_Server_send_response() for a deferred request -
// pending_response_timeout() (the tt_Node_schedule() callback, invoked directly here rather than
// via a real elapsed wait - test_liveliness.c's own check_liveliness() tests already establish
// this same "call the timer callback directly" whitebox pattern) must reclaim the slot instead of
// leaking it forever, and a tt_Server_send_response() that arrives after that must cleanly report
// tt_RET_NOT_FOUND rather than resurrecting a slot nothing is listening for the response of.
static void test_deferred_request_timeout_reclaims_slot(void) {
    test_mock_reset();
    callback_count = 0;
    stub_return_code = tt_CALL_DEFERRED;

    struct tt_Node node;
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

    struct tt_Node node;
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

int main(void) {
    test_fresh_request_invokes_callback_and_sends();
    test_retry_hits_cache_without_recalling_callback();
    test_fresh_request_response_is_unicast_to_sender();
    test_unknown_endpoint_is_ignored();
    test_deferred_request_answered_later_is_sent();
    test_deferred_request_timeout_reclaims_slot();
    test_retry_while_deferred_does_not_recall_callback();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_process_callrequest: all tests passed\n");
    return 0;
}
