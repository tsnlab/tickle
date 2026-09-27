/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// announce_bytes.c - prints, in hex, the full announce and the discovery summary one context sends for a fixed set
// of endpoints (a publisher, a subscriber, a client and a server), with the clock and entity ids pinned, so two trees
// can be compared byte for byte (CONTEXT_NODE_PLAN.md stage 2: "the announce bytes are byte-identical to stage 1's
// for the same endpoints"; stage 3 will change them on purpose).
//
// Build against a tree, from the tree's root:
//   gcc -I include -I src [-DON_NODE] -o /tmp/announce_bytes examples/perf_hil/experiments/announce_bytes.c
//       src/encoding.c src/log.c -lm
// -DON_NODE creates the endpoints on an explicit node (tt_Node_create_*, stage 2 onwards) instead of through the
// tt_Context_create_*() shorthands.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#define TEST_MOCK_DEFINE_STORAGE
#include "../../../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: build_and_send_update() is static
#include "../../../tests/test_mock.h"

#define CLOCK_NS 1000000000ULL // pinned, so the announce's timestamps are the same on every run
#define ENTITY_ID_BASE 0x1000  // pinned, instead of the launch-drawn base
#define LAST_MODIFIED 1000     // the discovery generation the announce carries

static int32_t data_size(struct tt_Data* data) {
    (void)data;
    return 4;
}
static int32_t data_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)data;
    return len < 4 ? -1 : (memset(payload, 0, 4), 4);
}
static int32_t data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)native;
    return len >= 4 ? 4 : -1;
}
static void data_free(struct tt_Data* data) {
    (void)data;
}
static int32_t request_size(struct tt_Request* request) {
    (void)request;
    return 4;
}
static int32_t request_encode(struct tt_Request* request, uint8_t* payload, const uint32_t len) {
    (void)request;
    return len < 4 ? -1 : (memset(payload, 0, 4), 4);
}
static int32_t request_decode(struct tt_Request* request, const uint8_t* payload, const uint32_t len, bool native) {
    (void)request;
    (void)payload;
    (void)native;
    return len >= 4 ? 4 : -1;
}
static void request_free(struct tt_Request* request) {
    (void)request;
}
static int32_t response_size(struct tt_Response* response) {
    (void)response;
    return 4;
}
static int32_t response_encode(struct tt_Response* response, uint8_t* payload, const uint32_t len) {
    (void)response;
    return len < 4 ? -1 : (memset(payload, 0, 4), 4);
}
static int32_t response_decode(struct tt_Response* response, const uint8_t* payload, const uint32_t len, bool native) {
    (void)response;
    (void)payload;
    (void)native;
    return len >= 4 ? 4 : -1;
}
static void response_free(struct tt_Response* response) {
    (void)response;
}
static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}
static void on_answer(struct tt_Client* client, int8_t code, struct tt_Response* response) {
    (void)client;
    (void)code;
    (void)response;
}
static int8_t on_call(struct tt_Server* server, struct tt_Request* request, struct tt_Response* response,
                      tt_RequestId id) {
    (void)server;
    (void)request;
    (void)response;
    (void)id;
    return 0;
}

static void print_datagram(const void* buf, size_t len) {
    printf("datagram %zu:", len);
    for (size_t i = 0; i < len; i++) {
        printf(" %02x", ((const uint8_t*)buf)[i]);
    }
    printf("\n");
}

int main(void) {
    static struct tt_Context context;
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    memset(&context, 0, sizeof(context));
    node_init_locks(&context);
    context.id = 1;
    context.tx_tail = sizeof(struct tt_Header);
    context.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    context.entity_id_base = ENTITY_ID_BASE;

    static struct tt_Topic topic;
    topic.name = "bytes_topic";
    topic.data_size = 4;
    topic.data_encode_size = data_size;
    topic.data_encode = data_encode;
    topic.data_decode = data_decode;
    topic.data_free = data_free;
    static struct tt_Service service;
    service.name = "bytes_service";
    service.request_size = 4;
    service.response_size = 4;
    service.request_encode_size = request_size;
    service.request_encode = request_encode;
    service.request_decode = request_decode;
    service.request_free = request_free;
    service.response_encode_size = response_size;
    service.response_encode = response_encode;
    service.response_decode = response_decode;
    service.response_free = response_free;

    static struct tt_Publisher pub;
    static struct tt_Subscriber sub;
    static struct tt_Client client;
    static struct tt_Server server;
#ifdef ON_NODE
    static struct tt_Node node;
    if (tt_Node_create(&context, &node, "talker", "/") != tt_RET_OK ||
        tt_Node_create_publisher(&node, &pub, &topic, "chatter") != tt_RET_OK ||
        tt_Node_create_subscriber(&node, &sub, &topic, "chatter", on_sample) != tt_RET_OK ||
        tt_Node_create_client(&node, &client, &service, "add", on_answer) != tt_RET_OK ||
        tt_Node_create_server(&node, &server, &service, "add", on_call) != tt_RET_OK) {
        printf("create failed\n");
        return 1;
    }
#else
    if (tt_Context_create_publisher(&context, &pub, &topic, "chatter") != tt_RET_OK ||
        tt_Context_create_subscriber(&context, &sub, &topic, "chatter", on_sample) != tt_RET_OK ||
        tt_Context_create_client(&context, &client, &service, "add", on_answer) != tt_RET_OK ||
        tt_Context_create_server(&context, &server, &service, "add", on_call) != tt_RET_OK) {
        printf("create failed\n");
        return 1;
    }
#endif
    test_mock_send_hook = print_datagram;
    printf("-- announce\n");
    context.last_modified = LAST_MODIFIED;
    if (!build_and_send_update(&context, NULL, 0)) {
        printf("announce not sent\n");
    }
    node_flush(&context, 0, NULL);
    printf("-- summary\n");
    send_discovery_summary(&context);
    node_flush(&context, 0, NULL);
    return 0;
}
