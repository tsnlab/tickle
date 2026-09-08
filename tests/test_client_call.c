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

static void test_callback(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    (void)return_code;
    (void)response;
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

int main(void) {
    test_call_rejected_while_one_outstanding();
    test_call_flushes_immediately_and_fills_cache();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_client_call: all tests passed\n");
    return 0;
}
