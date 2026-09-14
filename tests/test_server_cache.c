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

// Whitebox: set_server_cache()/get_server_cache() are static, and this exercises the fixed
// cache_buf[][]/clean_config[]/clean_scheduled[] slots directly.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

static void init_node_and_server(struct tt_Node* node, struct tt_Server* server) {
    memset(node, 0, sizeof(*node));
    memset(server, 0, sizeof(*server));
    server->node = node;
}

// Writes a fake, already-encoded CallResponse submessage straight into node->tx_buffer, the
// way end_encode() would have left it right before a response is cached (set_server_cache()
// derives the cached length from node->tx_buffer + node->tx_tail).
static struct tt_SubmessageHeader* write_fake_response(struct tt_Node* node, uint8_t receiver, uint32_t endpoint_id,
                                                       uint16_t seq_no) {
    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)node->tx_buffer;
    submessage_header->type = tt_SUBMESSAGE_TYPE_CALLRESPONSE;
    submessage_header->receiver = receiver;
    submessage_header->length = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallResponseHeader);

    struct tt_CallResponseHeader* response_header =
        (struct tt_CallResponseHeader*)(node->tx_buffer + sizeof(struct tt_SubmessageHeader));
    response_header->endpoint_id = endpoint_id;
    response_header->seq_no = seq_no;
    response_header->retry = 0;
    response_header->return_code = 0;

    node->tx_tail = submessage_header->length;

    return submessage_header;
}

// A cached response must be retrievable by (receiver, seq_no), and must be a copy into the
// server's own cache_buf - not just the original tx_buffer pointer, which gets reused for the
// next thing encoded right after caching.
static void test_cache_round_trip(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Server server;
    init_node_and_server(&node, &server);

    struct tt_SubmessageHeader* original = write_fake_response(&node, 5, 0x1111, 42);

    EXPECT_TRUE(set_server_cache(&server, original, 5));

    struct tt_SubmessageHeader* cached = get_server_cache(&server, 5, 42);
    EXPECT_TRUE(cached != NULL);
    EXPECT_TRUE(cached != original); // copied, not aliased

    if (cached != NULL) {
        struct tt_CallResponseHeader* cached_response =
            (struct tt_CallResponseHeader*)((void*)cached + sizeof(struct tt_SubmessageHeader));
        EXPECT_EQ_U32(0x1111, cached_response->endpoint_id);
        EXPECT_EQ_U32(42, cached_response->seq_no);
    }
}

// Caching a new response for a receiver that already has one cached must evict the old entry
// (a client only ever needs a retry-dedup answer for its current outstanding call), not leak
// it into an unrelated slot or leave it retrievable by its old seq_no.
static void test_cache_evicts_old_entry_for_same_receiver(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Server server;
    init_node_and_server(&node, &server);

    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x1111, 42), 5));
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x2222, 99), 5));

    EXPECT_TRUE(get_server_cache(&server, 5, 42) == NULL); // evicted
    EXPECT_TRUE(get_server_cache(&server, 5, 99) != NULL); // the new one is there instead
}

// A different receiver's cache entry must be unaffected by another receiver's traffic.
static void test_cache_keeps_entries_for_different_receivers_independent(void) {
    test_mock_reset();

    struct tt_Node node;
    struct tt_Server server;
    init_node_and_server(&node, &server);

    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x1111, 1), 5));
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 6, 0x2222, 1), 6));

    EXPECT_TRUE(get_server_cache(&server, 5, 1) != NULL);
    EXPECT_TRUE(get_server_cache(&server, 6, 1) != NULL);
}

int main(void) {
    test_cache_round_trip();
    test_cache_evicts_old_entry_for_same_receiver();
    test_cache_keeps_entries_for_different_receivers_independent();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_server_cache: all tests passed\n");
    return 0;
}
