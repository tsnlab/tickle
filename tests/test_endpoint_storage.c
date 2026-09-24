/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// tt_Server_set_storage()/tt_Client_set_storage(): a server's cached and deferred responses, and a
// client's outstanding request, kept in storage sized for the endpoint's own service instead of
// the inline default sized for any message - stage (iv) of the storage design the user approved on
// 2026-09-24. At tt_MAX_BUFFER_LENGTH 65507 the inline default makes every tt_Server 12.6 MB, so
// rmw_tickle defines it small and attaches what each service needs.
//
// What must hold:
//   - attached storage is where entries actually land, not the inline arrays;
//   - a response larger than a cache entry is still SENT, just not cached (it used to be that a
//     response which could not be cached was rolled back and never sent at all);
//   - a request larger than the client's storage is refused rather than sent unretryable;
//   - the attach calls refuse storage that cannot hold what they promise, or that is swapped out
//     from under an entry in use;
//   - NULL, and a zeroed struct, mean the inline default.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: set_server_cache()/get_server_cache() are static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox

#define ENTRY 64 // bytes per attached cache entry: room for a small response, not a large one

struct fake_response {
    uint32_t value;
    uint32_t pad;
};

static tt_ALIGNAS(8) uint8_t cache_area[tt_MAX_SERVER_CACHE_COUNT * ENTRY];
static tt_ALIGNAS(8) uint8_t pending_area[tt_MAX_SERVER_CACHE_COUNT * sizeof(struct fake_response)];
static tt_ALIGNAS(8) uint8_t client_area[ENTRY];

static struct tt_Node node;
static struct tt_Service service;
static struct tt_Server server;

static void init_server(void) {
    test_mock_reset();
    memset(&node, 0, sizeof(node));
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    memset(&service, 0, sizeof(service));
    service.name = "storage_service";
    service.response_size = sizeof(struct fake_response);
    memset(&server, 0, sizeof(server));
    server.node = &node;
    server.service = &service;
}

// A CallResponse of `body` payload bytes at the start of tx_buffer, as encode_call_response() leaves
// one for set_server_cache().
static struct tt_SubmessageHeader* write_response(uint8_t receiver, uint16_t seq_no, uint32_t body) {
    node.tx_tail = sizeof(struct tt_Header);
    struct tt_SubmessageHeader* sub = (struct tt_SubmessageHeader*)(node.tx_buffer + node.tx_tail);
    sub->type = tt_SUBMESSAGE_TYPE_CALLRESPONSE;
    sub->receiver = receiver;
    struct tt_CallResponseHeader* header = (struct tt_CallResponseHeader*)(sub + 1);
    header->endpoint_id = 0x1234;
    header->seq_no = seq_no;
    header->retry = 0;
    header->return_code = 0;
    node.tx_tail += sizeof(*sub) + sizeof(*header) + body;
    sub->length = (uint16_t)(node.tx_tail - sizeof(struct tt_Header));
    return sub;
}

static bool inside(const void* ptr, const uint8_t* area, size_t size) {
    return (const uint8_t*)ptr >= area && (const uint8_t*)ptr < area + size;
}

static void test_zeroed_server_uses_inline_storage(void) {
    init_server();
    EXPECT_TRUE(set_server_cache(&server, write_response(5, 1, 16), 5));
    struct tt_SubmessageHeader* cached = get_server_cache(&server, 5, 1);
    EXPECT_TRUE(cached != NULL && inside(cached, &server.cache_buf[0][0], sizeof(server.cache_buf)));
}

static void test_attached_cache_holds_what_fits_and_sends_what_does_not(void) {
    init_server();
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Server_set_storage(&server, cache_area, ENTRY, pending_area, sizeof(struct fake_response)));

    // Fits: cached, in the caller's storage.
    EXPECT_TRUE(set_server_cache(&server, write_response(5, 1, 16), 5));
    struct tt_SubmessageHeader* cached = get_server_cache(&server, 5, 1);
    EXPECT_TRUE(cached != NULL && inside(cached, cache_area, sizeof(cache_area)));

    // Larger than an entry: set_server_cache() still reports success, so the response is sent - and
    // nothing is cached for it. A retry will re-run the callback, as after a cache timeout.
    EXPECT_TRUE(set_server_cache(&server, write_response(6, 1, 200), 6));
    EXPECT_TRUE(get_server_cache(&server, 6, 1) == NULL);
}

static void test_deferred_response_lands_in_attached_pending_storage(void) {
    init_server();
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Server_set_storage(&server, cache_area, ENTRY, pending_area, sizeof(struct fake_response)));
    // A request deferred by the callback occupies slot 0 (what defer_call_response() records).
    server.pending_request_id[0].receiver = 7;
    server.pending_request_id[0].seq_no = 3;
    server.slot_state[0] = tt_SERVER_SLOT_PENDING;
    struct fake_response response = {.value = 0xC0FFEE, .pad = 0};
    tt_RequestId id = {.receiver = 7, .seq_no = 3};
    EXPECT_EQ_INT(tt_RET_OK, tt_Server_send_response(&server, id, 0, (struct tt_Response*)&response));
    EXPECT_EQ_U32(0xC0FFEE, ((struct fake_response*)server_pending_entry(&server, 0))->value);
    EXPECT_TRUE(inside(server_pending_entry(&server, 0), pending_area, sizeof(pending_area)));
}

static void test_server_attach_refuses_what_cannot_work(void) {
    init_server();
    // A pending entry smaller than the response struct could not hold a deferred response.
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Server_set_storage(&server, cache_area, ENTRY, pending_area, sizeof(struct fake_response) - 8));
    // A cache entry that could not hold even an empty response.
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Server_set_storage(&server, cache_area, 8, NULL, 0));
    // Misaligned storage, or a length that is not a multiple of 8.
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Server_set_storage(&server, cache_area + 4, ENTRY, NULL, 0));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Server_set_storage(&server, cache_area, ENTRY + 4, NULL, 0));
    // None of those changed anything.
    EXPECT_TRUE(server.cache_storage == NULL && server.pending_storage == NULL);

    // Swapping storage out from under a cached entry would leave it pointing at the old storage.
    EXPECT_TRUE(set_server_cache(&server, write_response(5, 1, 16), 5));
    EXPECT_EQ_INT(tt_RET_ILLEGAL_STATUS, tt_Server_set_storage(&server, cache_area, ENTRY, NULL, 0));
    clear_server_cache_slot(&server, 0);
    EXPECT_EQ_INT(tt_RET_OK, tt_Server_set_storage(&server, cache_area, ENTRY, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK, tt_Server_set_storage(&server, NULL, 0, NULL, 0)); // back to inline
    EXPECT_TRUE(server.cache_storage == NULL);
}

static int32_t sized_request_bytes;

static int32_t request_encode_size(struct tt_Request* request) {
    (void)request;
    return sized_request_bytes;
}

// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_REQUEST_ENCODE's fixed signature
static int32_t request_encode(struct tt_Request* request, uint8_t* payload, const uint32_t len) {
    (void)request;
    memset(payload, 0xAB, len);
    return (int32_t)len;
}

static void on_response(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    (void)return_code;
    (void)response;
}

static void test_client_storage(void) {
    init_server(); // for node and service
    service.request_encode_size = request_encode_size;
    service.request_encode = request_encode;
    static struct tt_Client client;
    memset(&client, 0, sizeof(client));
    client.endpoint.kind = tt_KIND_SERVICE_CLIENT;
    client.endpoint.id = 0xaabbccdd;
    client.node = &node;
    client.service = &service;
    client.callback = on_response;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_set_storage(&client, client_area, ENTRY));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Client_set_storage(&client, client_area + 4, ENTRY));

    // Too large for the attached storage: refused, nothing sent - a call that could not be retried
    // would fail silently on the first lost packet.
    struct tt_Request* request = (struct tt_Request*)client_area; // content is irrelevant to the stubs
    sized_request_bytes = 200;
    EXPECT_EQ_INT(tt_RET_OUT_OF_BUFFER, tt_Client_call(&client, request));
    EXPECT_EQ_INT(0, test_mock_send_call_count);
    EXPECT_TRUE(client.cache == NULL);

    // Fits: sent, and kept in the caller's storage for a retry.
    sized_request_bytes = 8;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&client, request));
    EXPECT_EQ_INT(1, test_mock_send_call_count);
    EXPECT_TRUE(client.cache != NULL && inside(client.cache, client_area, sizeof(client_area)));
    // And the storage cannot be swapped while that call is outstanding.
    EXPECT_EQ_INT(tt_RET_ILLEGAL_STATUS, tt_Client_set_storage(&client, NULL, 0));
}

int main(void) {
    test_zeroed_server_uses_inline_storage();
    test_attached_cache_holds_what_fits_and_sends_what_does_not();
    test_deferred_response_lands_in_attached_pending_storage();
    test_server_attach_refuses_what_cannot_work();
    test_client_storage();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_endpoint_storage: all tests passed\n");
    return 0;
}
