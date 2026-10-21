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
// cache_buf[][]/cache_expires_at[] slots directly.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

// A server always has its service (tt_Context_create_server() refuses one without); the cache's lifetime reads the
// service's call_retry_* to know its clients' retry schedule. Zeroed: the default schedule.
static struct tt_Service default_service;

static void init_node_and_server(struct tt_Context* node, struct tt_Server* server) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    memset(server, 0, sizeof(*server));
    server->node = node;
    server->service = &default_service;
}

// Writes a fake, already-encoded CallResponse submessage straight into node->tx_buffer, the
// way end_encode() would have left it right before a response is cached (set_server_cache()
// derives the cached length from node->tx_buffer + node->tx_tail).
static struct tt_SubmessageHeader* write_fake_response(struct tt_Context* node, uint8_t receiver, uint32_t endpoint_id,
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

    struct tt_Context node;
    struct tt_Server server;
    init_node_and_server(&node, &server);

    struct tt_SubmessageHeader* original = write_fake_response(&node, 5, 0x1111, 42);

    EXPECT_TRUE(set_server_cache(&server, original, 5, 0));

    struct tt_SubmessageHeader* cached = get_server_cache(&server, 5, 0, 42);
    EXPECT_TRUE(cached != NULL);
    EXPECT_TRUE(cached != original); // copied, not aliased

    if (cached != NULL) {
        struct tt_CallResponseHeader* cached_response =
            (struct tt_CallResponseHeader*)((void*)cached + sizeof(struct tt_SubmessageHeader));
        EXPECT_EQ_U32(0x1111, cached_response->endpoint_id);
        EXPECT_EQ_U32(42, cached_response->seq_no);
    }
}

// Built with one call per Client (tt_CLIENT_MAX_OUTSTANDING 1), caching a new response for a receiver that already
// has one cached must evict the old entry (that client only ever needs a retry-dedup answer for its current
// outstanding call), not leak it into an unrelated slot or leave it retrievable by its old seq_no. Built with more,
// the old answer must stay: the client may still be retrying it while it waits on the new one (2026-10-16).
static void test_cache_for_same_receiver(void) {
    test_mock_reset();

    struct tt_Context node;
    struct tt_Server server;
    init_node_and_server(&node, &server);

    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x1111, 42), 5, 0));
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x2222, 99), 5, 0));

    if (tt_CLIENT_MAX_OUTSTANDING == 1) {
        EXPECT_TRUE(get_server_cache(&server, 5, 0, 42) == NULL); // evicted
    } else {
        EXPECT_TRUE(get_server_cache(&server, 5, 0, 42) != NULL); // kept for that call's retry
    }
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 99) != NULL); // the new one is there in any build
}

// A different receiver's cache entry must be unaffected by another receiver's traffic.
static void test_cache_keeps_entries_for_different_receivers_independent(void) {
    test_mock_reset();

    struct tt_Context node;
    struct tt_Server server;
    init_node_and_server(&node, &server);

    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x1111, 1), 5, 0));
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 6, 0x2222, 1), 6, 0));

    EXPECT_TRUE(get_server_cache(&server, 5, 0, 1) != NULL);
    EXPECT_TRUE(get_server_cache(&server, 6, 0, 1) != NULL);
}

// Built with several calls per Client (2026-10-16): one Client keeps at most tt_CLIENT_CALL_WINDOW answers, the one to
// its oldest call going first - by call, not by when it was last sent: an answer re-sent to a retry a moment ago still
// goes if its call is the oldest, since the Client cannot be waiting on a call that old (client_window_closed()). The
// answers come out of call order, as several calls are answered. A late duplicate of a call older than all of them is
// answered and not kept. Other Clients' answers are not touched.
static void test_cache_bounds_one_clients_answers(void) {
    if (tt_CLIENT_MAX_OUTSTANDING == 1) {
        return; // one answer per Client, test_cache_for_same_receiver()
    }
    test_mock_reset();
    struct tt_Context node;
    struct tt_Server server;
    init_node_and_server(&node, &server);

    test_mock_now = 1000;
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 6, 0x2222, 1), 6, 0));
    for (int k = tt_CLIENT_CALL_WINDOW - 1; k >= 0; k--) { // calls 100.. answered newest first
        test_mock_now += 10;
        EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x1111, (uint16_t)(100 + k)), 5, 0));
    }
    test_mock_now += 10;
    note_server_cache_hit(&server, find_server_cache_slot(&server, 5, 0, 100)); // call 100's answer re-sent: newest
    test_mock_now += 10;
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x1111, 100 + tt_CLIENT_CALL_WINDOW), 5, 0));
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 100) == NULL); // the oldest call's went, though sent last
    for (int k = 1; k <= tt_CLIENT_CALL_WINDOW; k++) {
        EXPECT_TRUE(get_server_cache(&server, 5, 0, (uint16_t)(100 + k)) != NULL);
    }
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x1111, 99), 5, 0)); // a late duplicate
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 99) == NULL);
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 101) != NULL);
    EXPECT_TRUE(get_server_cache(&server, 6, 0, 1) != NULL); // another Client's, older than all of them, stays
}

// A full cache, built with several calls per Client: a Client holding a full depth of answers gives up its own oldest,
// not the oldest overall - which here is a slow Client's one answer, still being retried.
static void test_full_cache_takes_a_deep_clients_own_answer(void) {
    if (tt_CLIENT_MAX_OUTSTANDING == 1) {
        return;
    }
    test_mock_reset();
    struct tt_Context node;
    struct tt_Server server;
    init_node_and_server(&node, &server);

    test_mock_now = 1000;
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 9, 0x1111, 1), 9, 0)); // the slow Client's
    for (int i = 1; i < tt_MAX_SERVER_CACHE_COUNT - tt_CLIENT_MAX_OUTSTANDING; i++) {
        test_mock_now += 10;
        EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, (uint8_t)(100 + i), 0x1111, 1),
                                     (uint8_t)(100 + i), 0));
    }
    for (int k = 0; k < tt_CLIENT_MAX_OUTSTANDING; k++) {
        test_mock_now += 10;
        EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x1111, (uint16_t)(200 + k)), 5, 0));
    }
    test_mock_now += 10;
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 5, 0x1111, 300), 5, 0)); // full: one must go
    EXPECT_TRUE(get_server_cache(&server, 9, 0, 1) != NULL);   // the oldest overall stays
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 200) == NULL); // the deep Client's own oldest went
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 300) != NULL);
    // A Client with fewer than a full depth takes the oldest overall, as before.
    test_mock_now += 10;
    EXPECT_TRUE(set_server_cache(&server, write_fake_response(&node, 7, 0x1111, 1), 7, 0));
    EXPECT_TRUE(get_server_cache(&server, 9, 0, 1) == NULL);
    EXPECT_TRUE(get_server_cache(&server, 7, 0, 1) != NULL);
}

int main(void) {
    test_cache_bounds_one_clients_answers();
    test_full_cache_takes_a_deep_clients_own_answer();
    test_cache_round_trip();
    test_cache_for_same_receiver();
    test_cache_keeps_entries_for_different_receivers_independent();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_server_cache: all tests passed\n");
    return 0;
}
