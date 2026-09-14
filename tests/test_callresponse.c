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

// This file owns the shared test-assertion and HAL-mock storage for this test binary.
#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox test: include the implementation directly so static functions like
// process_callresponse() are reachable, without widening their visibility in production.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

static int callback_count = 0;
static int callback_return_code = 0; // widened from the wire's int8_t to avoid a signed-char/int mix in EXPECT_EQ_INT
static struct tt_Response* callback_response = (struct tt_Response*)1; // sentinel != NULL

static void test_client_callback(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    callback_count++;
    callback_return_code = (int)return_code; // sign-extend explicitly; the wire value can be negative
    callback_response = response;
}

// Marks a call as outstanding: cache points at cache_buf, which holds the SubmessageHeader +
// CallRequestHeader that tt_Client_call() copies there for a retry - process_callresponse() now
// checks that request's seq_no against the response's before accepting it.
static void set_outstanding_call(struct tt_Client* client, uint16_t seq_no) {
    memset(client->cache_buf, 0, sizeof(client->cache_buf));
    client->cache = (struct tt_SubmessageHeader*)client->cache_buf;
    struct tt_CallRequestHeader* req =
        (struct tt_CallRequestHeader*)(client->cache_buf + sizeof(struct tt_SubmessageHeader));
    req->seq_no = seq_no;
}

// Regression test for the latency EWMA and cache-clearing on a successful (well, "answered")
// call response: cache_time -> now must measure forward, not backward (see calculate_latency()),
// and clearing client->cache must not try to free it (it's a fixed buffer, see cache_buf).
static void test_callresponse_updates_latency_and_clears_cache(void) {
    test_mock_reset();
    test_mock_now = 1250;

    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.id = 1;

    struct tt_Service service;
    memset(&service, 0, sizeof(service));
    service.response_size = 1; // unused here (return_code != 0 skips decode) but must be >0 for the VLA

    struct tt_Client client;
    memset(&client, 0, sizeof(client));
    client.endpoint.kind = tt_KIND_SERVICE_CLIENT;
    client.endpoint.id = 0x12345678;
    client.node = &node;
    client.service = &service;
    client.callback = test_client_callback;
    set_outstanding_call(&client, 7); // a call with seq_no 7 is outstanding
    client.cache_time = 1000;
    client.latency = 0;

    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&client;

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = 2;

    struct tt_CallResponseHeader response_header;
    memset(&response_header, 0, sizeof(response_header));
    response_header.endpoint_id = client.endpoint.id;
    response_header.seq_no = 7;
    response_header.retry = 0;
    response_header.return_code = 1; // non-zero: process_callresponse skips response_decode

    callback_count = 0;
    callback_return_code = 0;
    callback_response = (struct tt_Response*)1;

    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&response_header, 0, sizeof(response_header)));
    EXPECT_EQ_U32(250, client.latency); // now(1250) - cache_time(1000)
    EXPECT_TRUE(client.cache == NULL);
    EXPECT_EQ_U32(1, callback_count);
    EXPECT_EQ_INT(1, callback_return_code);
    EXPECT_TRUE(callback_response == NULL);
}

// Shared setup for the "must be ignored" cases below: a client registered on a node, one
// outstanding call at seq_no 7, and a response header the test then tweaks.
static void setup_client(struct tt_Node* node, struct tt_Service* service, struct tt_Client* client) {
    memset(node, 0, sizeof(*node));
    node->id = 1;
    memset(service, 0, sizeof(*service));
    service->response_size = 1;
    memset(client, 0, sizeof(*client));
    client->endpoint.kind = tt_KIND_SERVICE_CLIENT;
    client->endpoint.id = 0x12345678;
    client->node = node;
    client->service = service;
    client->callback = test_client_callback;
    client->cache_time = 1000;
    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)client;
}

static struct tt_CallResponseHeader make_response(uint32_t endpoint_id, uint16_t seq_no) {
    struct tt_CallResponseHeader h;
    memset(&h, 0, sizeof(h));
    h.endpoint_id = endpoint_id;
    h.seq_no = seq_no;
    h.return_code = 1; // skip decode
    return h;
}

// A CallResponse arriving when client->cache is NULL (no call outstanding - e.g. one already
// answered, or an unsolicited/stray response) must be dropped without invoking the callback.
static void test_callresponse_ignored_when_no_call_outstanding(void) {
    test_mock_reset();
    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    setup_client(&node, &service, &client);
    client.cache = NULL; // nothing outstanding

    struct tt_Header header = {.magic_value = NATIVE_MAGIC_VALUE, .version = tt_VERSION, .source = 2};
    struct tt_CallResponseHeader resp = make_response(client.endpoint.id, 7);

    callback_count = 0;
    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&resp, 0, sizeof(resp)));
    EXPECT_EQ_U32(0, callback_count);
}

// A response whose seq_no doesn't match the outstanding call (it answers some earlier call) must
// be dropped, leaving the current call still outstanding.
static void test_callresponse_ignored_on_seq_no_mismatch(void) {
    test_mock_reset();
    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    setup_client(&node, &service, &client);
    set_outstanding_call(&client, 8); // waiting on seq_no 8

    struct tt_Header header = {.magic_value = NATIVE_MAGIC_VALUE, .version = tt_VERSION, .source = 2};
    struct tt_CallResponseHeader resp = make_response(client.endpoint.id, 7); // stale: for seq_no 7

    callback_count = 0;
    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&resp, 0, sizeof(resp)));
    EXPECT_EQ_U32(0, callback_count);
    EXPECT_TRUE(client.cache != NULL); // still waiting
}

// The classic retry race: the server answered both the original request and a retry that crossed
// it on the wire. The first response completes the call; the duplicate must not invoke the
// callback a second time.
static void test_duplicate_callresponse_does_not_double_invoke_callback(void) {
    test_mock_reset();
    struct tt_Node node;
    struct tt_Service service;
    struct tt_Client client;
    setup_client(&node, &service, &client);
    set_outstanding_call(&client, 7);

    struct tt_Header header = {.magic_value = NATIVE_MAGIC_VALUE, .version = tt_VERSION, .source = 2};
    struct tt_CallResponseHeader resp = make_response(client.endpoint.id, 7);

    callback_count = 0;
    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&resp, 0, sizeof(resp)));
    EXPECT_EQ_U32(1, callback_count);
    EXPECT_TRUE(client.cache == NULL);

    // Duplicate arrives after the call already completed.
    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&resp, 0, sizeof(resp)));
    EXPECT_EQ_U32(1, callback_count); // still 1
}

// A response for an endpoint_id nobody registered (e.g. meant for a different node sharing the
// broadcast domain) must be ignored, not treated as an error.
static void test_callresponse_ignores_unknown_endpoint(void) {
    test_mock_reset();

    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.id = 1;
    node.endpoint_count = 0; // nothing registered

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = 2;

    struct tt_CallResponseHeader response_header;
    memset(&response_header, 0, sizeof(response_header));
    response_header.endpoint_id = 0xdeadbeef;

    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&response_header, 0, sizeof(response_header)));
}

int main(void) {
    test_callresponse_updates_latency_and_clears_cache();
    test_callresponse_ignores_unknown_endpoint();
    test_callresponse_ignored_when_no_call_outstanding();
    test_callresponse_ignored_on_seq_no_mismatch();
    test_duplicate_callresponse_does_not_double_invoke_callback();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_callresponse: all tests passed\n");
    return 0;
}
