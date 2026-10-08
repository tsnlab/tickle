/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Two Clients of ONE service in one context, and a Server of it in another (2026-10-09, found by
// examples/perf_hil/experiments/service_window_a.c's first harness). A CallRequest names its client by (source
// context, endpoint_id, seq_no) only, and both Clients have the service's endpoint_id; each counted its own seq_no
// from 0, so their requests were identical on the wire, the server answered the second from the first's cache, and
// the response went to whichever Client find_endpoint() found first - the second Client never got an answer.
//
// Datagrams are carried between the two contexts by hand (the mock HAL captures what each sends). Checked:
// - each Client gets exactly one answer per call, its own (the server echoes the request's tag), and the server runs
//   its callback once per call;
// - the same with the answers handed back in the reverse order, and with only the second Client calling;
// - the two requests carry different seq_nos.
// Mutants, each killed here: seq_no per Client again (both requests seq 0, one answer for two calls); the response
// routed to find_endpoint()'s first match again (the second Client's answers dropped).
//
// Not covered, and still open: a context's two Clients share the server's one-live-answer-per-source cache
// (set_server_cache() clears the source's previous answer), so a retry of one Client's call that arrives after the
// other Client's call was answered runs the callback again, as one after the cache expired does.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define SERVER_ID 1
#define CLIENT_ID 2
#define SERVER_IP 0x0a000001
#define CLIENT_IP 0x0a000002
#define PORT 40000
#define CLOCK_NS (100ULL * tt_SECOND)
#define CAPTURE_MAX 16
#define ROUNDS 20
#define ANSWER_OFFSET 1000U
#define CLIENTS 2

struct message {
    uint32_t tag;
};

static int32_t message_size(struct message* msg) {
    (void)msg;
    return (int32_t)sizeof(uint32_t);
}
static int32_t message_encode(struct message* msg, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, &msg->tag, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}
static int32_t message_decode(struct message* msg, const uint8_t* payload, const uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(&msg->tag, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}
static void message_free(struct message* msg) {
    (void)msg;
}

static struct tt_Service service = {
    .name = "two_clients::srv::dds_::Echo_",
    .request_size = sizeof(struct message),
    .response_size = sizeof(struct message),
    .request_encode_size = (tt_REQUEST_ENCODE_SIZE)message_size,
    .request_encode = (tt_REQUEST_ENCODE)message_encode,
    .request_decode = (tt_REQUEST_DECODE)message_decode,
    .request_free = (tt_REQUEST_FREE)message_free,
    .response_encode_size = (tt_RESPONSE_ENCODE_SIZE)message_size,
    .response_encode = (tt_RESPONSE_ENCODE)message_encode,
    .response_decode = (tt_RESPONSE_DECODE)message_decode,
    .response_free = (tt_RESPONSE_FREE)message_free,
};

static struct tt_Context server_context;
static struct tt_Context client_context;
static struct tt_Server server;
static struct tt_Client clients[CLIENTS];

static int served; // server callback invocations
static int answers[CLIENTS];
static uint32_t last_answer[CLIENTS];
static int wrong_answers; // an answer that is not this client's own outstanding call's

static uint32_t expected_tag[CLIENTS];

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    (void)request_id;
    served++;
    response->tag = request->tag + ANSWER_OFFSET;
    return 0;
}

static int client_index(const struct tt_Client* client) {
    for (int i = 0; i < CLIENTS; i++) {
        if (client == &clients[i]) {
            return i;
        }
    }
    return -1;
}

static void answered(struct tt_Client* client, int8_t return_code, struct message* response) {
    int i = client_index(client);
    if (i < 0 || return_code != 0 || response == NULL) {
        wrong_answers++;
        return;
    }
    answers[i]++;
    last_answer[i] = response->tag;
    if (response->tag != expected_tag[i] + ANSWER_OFFSET) {
        wrong_answers++;
    }
}

// What a context sends, kept for carrying to the other one.
struct capture {
    uint8_t data[CAPTURE_MAX][tt_MAX_BUFFER_LENGTH * 2];
    size_t length[CAPTURE_MAX];
    int count;
};
static struct capture requests;
static struct capture responses;
static struct capture* capturing;

static void capture(const void* buf, size_t len) {
    if (capturing != NULL && capturing->count < CAPTURE_MAX && len <= sizeof(capturing->data[0])) {
        memcpy(capturing->data[capturing->count], buf, len);
        capturing->length[capturing->count++] = len;
    }
}

// Datagram `i` of `from`, handed to `to` as if it came from `ip`.
static void carry_one(const struct capture* from, int i, struct tt_Context* to, uint32_t ip) {
    memcpy(to->rx_buffer, from->data[i], from->length[i]);
    (void)process_packet(to, to->rx_buffer, 0, (uint32_t)from->length[i], ip, PORT, tt_TRANSPORT_UDP);
}

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_TX_BUFFER_LENGTH;
    context->last_modified = CLOCK_NS;
}

static void setup(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&server_context, SERVER_ID);
    init_context(&client_context, CLIENT_ID);
    memset(&server, 0, sizeof(server));
    memset(clients, 0, sizeof(clients));
    served = 0;
    wrong_answers = 0;
    for (int i = 0; i < CLIENTS; i++) {
        answers[i] = 0;
        last_answer[i] = 0;
        expected_tag[i] = 0;
    }
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_server(&server_context, &server, &service, "two_clients/echo",
                                                      (tt_SERVER_CALLBACK)serve));
    for (int i = 0; i < CLIENTS; i++) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_client(&client_context, &clients[i], &service, "two_clients/echo",
                                                          (tt_CLIENT_CALLBACK)answered));
    }
    EXPECT_EQ_U32(clients[0].endpoint.id, clients[1].endpoint.id); // the shape under test: one endpoint_id
    EXPECT_EQ_U32(server.endpoint.id, clients[0].endpoint.id);
    test_mock_send_hook = capture;
}

static uint16_t outstanding_seq_no(const struct tt_Client* client) {
    return ((const struct tt_CallRequestHeader*)((const uint8_t*)client->cache + sizeof(struct tt_SubmessageHeader)))
        ->seq_no;
}

// One round: the clients in `callers` call, the server takes every request, and the answers go back, in reverse
// order when `reverse`.
static void round_trip(const bool callers[CLIENTS], uint32_t round, bool reverse) {
    requests.count = 0;
    responses.count = 0;
    capturing = &requests;
    for (int i = 0; i < CLIENTS; i++) {
        if (callers[i]) {
            expected_tag[i] = (round * CLIENTS) + (uint32_t)i;
            struct message request = {expected_tag[i]};
            EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&clients[i], (struct tt_Request*)&request));
        }
    }
    if (callers[0] && callers[1]) {
        EXPECT_TRUE(outstanding_seq_no(&clients[0]) != outstanding_seq_no(&clients[1]));
    }
    capturing = &responses;
    for (int i = 0; i < requests.count; i++) {
        carry_one(&requests, i, &server_context, CLIENT_IP);
    }
    capturing = NULL;
    for (int k = 0; k < responses.count; k++) {
        carry_one(&responses, reverse ? responses.count - 1 - k : k, &client_context, SERVER_IP);
    }
}

static void test_each_client_gets_its_own_answers(bool reverse) {
    setup();
    const bool both[CLIENTS] = {true, true};
    for (uint32_t round = 0; round < ROUNDS; round++) {
        round_trip(both, round, reverse);
        for (int i = 0; i < CLIENTS; i++) {
            EXPECT_EQ_INT((int)round + 1, answers[i]);
            EXPECT_EQ_U32(expected_tag[i] + ANSWER_OFFSET, last_answer[i]);
            EXPECT_TRUE(clients[i].cache == NULL); // nothing left outstanding
        }
    }
    EXPECT_EQ_INT(ROUNDS * CLIENTS, served);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

// Only the second Client - the one find_endpoint() does not return - calls.
static void test_the_second_client_alone_is_answered(void) {
    setup();
    const bool second[CLIENTS] = {false, true};
    for (uint32_t round = 0; round < ROUNDS; round++) {
        round_trip(second, round, false);
    }
    EXPECT_EQ_INT(0, answers[0]);
    EXPECT_EQ_INT(ROUNDS, answers[1]);
    EXPECT_EQ_INT(ROUNDS, served);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

int main(void) {
    test_each_client_gets_its_own_answers(false);
    test_each_client_gets_its_own_answers(true);
    test_the_second_client_alone_is_answered();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_two_clients_one_service: all tests passed\n");
    return 0;
}
