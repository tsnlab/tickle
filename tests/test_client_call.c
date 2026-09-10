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

// Whitebox: needs node->tx_buffer internals, not just the public return code.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

static int32_t stub_request_encode_size(struct tt_Request* request) {
    (void)request;
    return 4;
}

static int32_t stub_request_encode(struct tt_Request* request, uint8_t* payload, const uint32_t len) {
    (void)request;
    if (len < 4) {
        return -1;
    }
    memset(payload, 0xab, 4);
    return 4;
}

static int last_return_code = 0;
static int callback_calls = 0;
static void test_callback(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    (void)response;
    last_return_code = (int)return_code;
    callback_calls++;
}

static void init_node_and_client(struct tt_Node* node, struct tt_Service* service, struct tt_Client* client) {
    memset(node, 0, sizeof(*node));
    node->id = 1;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;

    memset(service, 0, sizeof(*service));
    service->name = "test_service";
    service->request_encode_size = stub_request_encode_size;
    service->request_encode = stub_request_encode;

    memset(client, 0, sizeof(*client));
    client->endpoint.kind = tt_KIND_SERVICE_CLIENT;
    client->endpoint.id = 0xaabbccdd;
    client->node = node;
    client->service = service;
    client->callback = test_callback;
}

// A second call while one is still outstanding must be rejected without touching any state -
// the caller is expected to wait for the first call's response/timeout before trying again.
static void test_call_rejected_while_one_outstanding(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    init_node_and_client(&node, &service, &client);

    client.cache = (struct tt_SubmessageHeader*)client.cache_buf; // pretend a call is already in flight

    struct tt_Request request;
    tt_ret_t ret = tt_Client_call(&client, &request);

    EXPECT_EQ_INT(tt_RET_ILLEGAL_STATUS, ret);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_call_count);
}

// A fresh call must encode into the client's own cache_buf (not a new allocation), and - since
// call requests always flush immediately (see end_encode(is_flush=true) in tt_Client_call) -
// must have already gone out over tt_send() by the time tt_Client_call() returns.
// init_node_and_client() leaves client.peers[] zeroed (no known Servers yet), which is also the
// "0 known peers" case: discovery hasn't matched anyone yet, so this always broadcasts
// regardless of tt_UNICAST_PEER_THRESHOLD.
static void test_call_flushes_immediately_and_fills_cache(void) {
    test_mock_reset();
    test_mock_now = 500;

    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    init_node_and_client(&node, &service, &client);

    struct tt_Request request;
    tt_ret_t ret = tt_Client_call(&client, &request);

    EXPECT_EQ_INT(tt_RET_OK, ret);
    EXPECT_TRUE(client.cache == (struct tt_SubmessageHeader*)client.cache_buf);
    EXPECT_EQ_U32(500, (uint32_t)client.cache_time);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail); // drained back down by the flush
}

// At or under tt_UNICAST_PEER_THRESHOLD known Servers, the initial call must unicast to each of
// them instead of broadcasting - no batching concern here (RPC always flushes immediately
// regardless of destination, unlike Publisher - see tt_Client_call()'s own comment).
static void test_call_unicasts_to_known_servers_at_or_under_threshold(void) {
    test_mock_reset();
    test_mock_now = 500;

    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    init_node_and_client(&node, &service, &client);

    client.peers[0] = (struct tt_Peer) {.node_id = 2, .ip = 0xc0a80a02, .port = 8282};

    struct tt_Request request;
    tt_ret_t ret = tt_Client_call(&client, &request);

    EXPECT_EQ_INT(tt_RET_OK, ret);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(client.peers[0].ip, test_mock_send_to_last_ip);
    EXPECT_EQ_U32((uint32_t)client.peers[0].port, (uint32_t)test_mock_send_to_last_port);
}

// More known Servers than tt_UNICAST_PEER_THRESHOLD must fall back to broadcast, same as today.
static void test_call_broadcasts_when_server_count_exceeds_threshold(void) {
    test_mock_reset();
    test_mock_now = 500;

    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    init_node_and_client(&node, &service, &client);

    for (int i = 0; i < tt_UNICAST_PEER_THRESHOLD + 1; i++) {
        client.peers[i] =
            (struct tt_Peer) {.node_id = (uint8_t)(2 + i), .ip = 0xc0a80a00 + (uint8_t)(2 + i), .port = 8282};
    }

    struct tt_Request request;
    tt_ret_t ret = tt_Client_call(&client, &request);

    EXPECT_EQ_INT(tt_RET_OK, ret);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
}

// A retry (call_retry() -> resend_call_request()) must apply the same peer decision as the
// initial call, recomputed from the client's current peer table.
static void test_call_retry_uses_same_peer_decision_as_initial_call(void) {
    test_mock_reset();
    test_mock_now = 500;

    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    init_node_and_client(&node, &service, &client);
    service.call_retry_count = 3;

    struct tt_Request request;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&client, &request));

    // Discovery learns a Server only after the initial (broadcast) call already went out.
    client.peers[0] = (struct tt_Peer) {.node_id = 2, .ip = 0xc0a80a02, .port = 8282};

    test_mock_send_call_count = 0;
    test_mock_send_to_call_count = 0;
    call_retry(&node, tt_get_ns(), &client);

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // the retry goes out unicast
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(client.peers[0].ip, test_mock_send_to_last_ip);
}

// NULL / missing-callback arguments must be rejected with tt_RET_INVALID_ARGUMENT, not
// dereferenced.
static void test_call_rejects_invalid_arguments(void) {
    test_mock_reset();
    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    init_node_and_client(&node, &service, &client);
    struct tt_Request request;

    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Client_call(NULL, &request));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Client_call(&client, NULL));

    struct tt_Client no_node = client;
    no_node.node = NULL;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Client_call(&no_node, &request));

    struct tt_Service no_enc = service;
    no_enc.request_encode = NULL;
    struct tt_Client bad_svc = client;
    bad_svc.service = &no_enc;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Client_call(&bad_svc, &request));
}

// request_encode_size() returning a nonsense value (negative, or bigger than a datagram) must be
// caught as a protocol error, not fed to encode() where it would wrap the unsigned length check.
static int32_t stub_request_encode_size_negative(struct tt_Request* request) {
    (void)request;
    return -1;
}
static void test_call_rejects_bad_encode_size(void) {
    test_mock_reset();
    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    init_node_and_client(&node, &service, &client);
    service.request_encode_size = stub_request_encode_size_negative;

    struct tt_Request request;
    EXPECT_EQ_INT(tt_RET_PROTOCOL_ERROR, tt_Client_call(&client, &request));
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail); // rolled back
}

// When every retry goes unanswered, call_retry() must report tt_CALL_TIMEOUT (not 0, which a
// server can legitimately return) with a NULL response, exactly once.
static void test_call_retry_exhausted_reports_timeout(void) {
    test_mock_reset();
    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    init_node_and_client(&node, &service, &client);
    service.call_retry_count = 2;

    struct tt_Request request;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&client, &request));

    callback_calls = 0;
    last_return_code = 12345;
    for (int i = 0; i < 5; i++) {
        call_retry(&node, tt_get_ns(), &client); // retry 1, retry 2, then give up; then no-ops
    }

    EXPECT_EQ_INT((int)tt_CALL_TIMEOUT, last_return_code);
    EXPECT_EQ_U32(1, (uint32_t)callback_calls); // fired once, not on every subsequent no-op
    EXPECT_TRUE(client.cache == NULL);
}

int main(void) {
    test_call_rejected_while_one_outstanding();
    test_call_flushes_immediately_and_fills_cache();
    test_call_unicasts_to_known_servers_at_or_under_threshold();
    test_call_broadcasts_when_server_count_exceeds_threshold();
    test_call_retry_uses_same_peer_decision_as_initial_call();
    test_call_rejects_invalid_arguments();
    test_call_rejects_bad_encode_size();
    test_call_retry_exhausted_reports_timeout();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_client_call: all tests passed\n");
    return 0;
}
