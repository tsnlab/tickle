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
// Since 2026-10-09 also: the server caches one answer per calling Client - (source context, client_tag) - where it
// used to keep one per source, so a retry of one Client's call that arrived after the other Client was answered ran
// the callback again. Checked: the retry after the other Client moved on; a soak of interleaved calls with drops and
// duplicates, each callback run exactly once; a seq_no of the other Client's that wrapped onto a cached one.
// Mutants (tests/mutants_call_identity.py), each killed here: the tag not sent; the previous answer cleared per source
// again; the tag left out of the cache lookup.

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

#define TRACKED_CALLS 1024
static int runs[TRACKED_CALLS]; // callback runs per request tag, for the tags below TRACKED_CALLS

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    (void)request_id;
    served++;
    if (request->tag < TRACKED_CALLS) {
        runs[request->tag]++;
    }
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
    memset(runs, 0, sizeof(runs));
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

// --- One answer cached per calling Client (2026-10-09) ---
//
// The server keeps one live answer per (source context, client_tag) and drops a Client's previous answer when that
// Client calls again. Its cache expiry timers are never run here (the server context's scheduler is not driven), so
// every retry below arrives while its answer would still be kept: a callback run twice for one call can only be the
// eviction this tests, never an expiry.

// The Clients are told apart on the wire.
static void test_each_client_has_its_own_tag(void) {
    setup();
    EXPECT_TRUE(clients[0].client_tag != 0);
    EXPECT_TRUE(clients[1].client_tag != 0);
    EXPECT_TRUE(clients[0].client_tag != clients[1].client_tag);
    requests.count = 0;
    capturing = &requests;
    struct message request = {7};
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&clients[1], (struct tt_Request*)&request));
    capturing = NULL;
    EXPECT_EQ_INT(1, requests.count);
    // A CallRequest is addressed to everyone, so it goes alone with the 4-byte tt_SingleHeader (tt_VERSION 10).
    EXPECT_TRUE((requests.data[0][0] & tt_SINGLE_MARKER_FLAG) != 0);
    const struct tt_CallRequestHeader* header =
        (const struct tt_CallRequestHeader*)(requests.data[0] + sizeof(struct tt_SingleHeader));
    EXPECT_EQ_INT(clients[1].client_tag, header->client_tag);
    test_mock_send_hook = NULL;
}

// Client `i` calls with request tag `tag`; its CallRequest goes to whatever is capturing.
static void call(int i, uint32_t tag) {
    expected_tag[i] = tag;
    struct message request = {tag};
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&clients[i], (struct tt_Request*)&request));
}

// Runs the client context's earliest call retry, at its time (and anything due before it). False if no call is
// waiting on one.
static bool run_next_client_timer(void) {
    uint64_t at = UINT64_MAX;
    for (uint32_t k = 0; k < client_context.scheduler_tail; k++) {
        const struct tt_TCB* entry = &client_context.scheduler[k];
        if (entry->function == call_retry && entry->time < at) {
            at = entry->time;
        }
    }
    if (at == UINT64_MAX) {
        return false;
    }
    if (at > test_mock_now) {
        test_mock_now = at;
    }
    bool has_next = false;
    uint64_t next = 0;
    while (run_due_entry(&client_context, test_mock_now, &has_next, &next)) {
    }
    return true;
}

// Every datagram of `from`, in order.
static void carry_all(const struct capture* from, struct tt_Context* to, uint32_t ip) {
    for (int i = 0; i < from->count; i++) {
        carry_one(from, i, to, ip);
    }
}

// Whether datagram `k` of `from` is a CallResponse to the call `seq_no` (one addressed to its client's context, so
// with the full tt_Header + tt_SubmessageHeader).
static bool answers_call(const struct capture* from, int k, uint16_t seq_no) {
    const uint8_t* data = from->data[k];
    if (from->length[k] <
            sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallResponseHeader) ||
        (data[0] & tt_SINGLE_MARKER_FLAG) != 0) {
        return false;
    }
    const struct tt_SubmessageHeader* submessage = (const struct tt_SubmessageHeader*)(data + sizeof(struct tt_Header));
    const struct tt_CallResponseHeader* header =
        (const struct tt_CallResponseHeader*)(data + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    return submessage->type == tt_SUBMESSAGE_TYPE_CALLRESPONSE && header->seq_no == seq_no;
}

// How many answers the server holds live.
static int live_answers(void) {
    int live = 0;
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        live += server.cache[i] != NULL ? 1 : 0;
    }
    return live;
}

// Client `lost`'s answer is dropped, the other Client's is delivered and that Client calls twice more; then `lost`
// retries. Its retry must be answered from the cache - the callback does not run again - with its own answer.
static void test_a_retry_after_the_other_client_moved_on(int lost) {
    setup();
    int other = 1 - lost;
    requests.count = 0;
    capturing = &requests;
    call(lost, 0);
    call(other, 1);
    capturing = &responses;
    responses.count = 0;
    for (int k = 0; k < requests.count; k++) {
        carry_one(&requests, k, &server_context, CLIENT_IP);
    }
    capturing = NULL;
    int dropped = 0;
    for (int k = 0; k < responses.count; k++) { // only the other Client's answer arrives
        if (answers_call(&responses, k, outstanding_seq_no(&clients[lost]))) {
            dropped++;
        } else {
            carry_one(&responses, k, &client_context, SERVER_IP);
        }
    }
    EXPECT_EQ_INT(1, dropped);
    EXPECT_EQ_INT(0, answers[lost]);
    EXPECT_EQ_INT(1, answers[other]);

    for (uint32_t tag = 2; tag <= 3; tag++) { // the other Client moves on, twice
        requests.count = 0;
        capturing = &requests;
        call(other, tag);
        capturing = &responses;
        responses.count = 0;
        carry_all(&requests, &server_context, CLIENT_IP);
        capturing = NULL;
        carry_all(&responses, &client_context, SERVER_IP);
    }
    EXPECT_EQ_INT(3, answers[other]);
    EXPECT_EQ_INT(4, served);
    EXPECT_EQ_INT(CLIENTS, live_answers()); // one per Client: the other's two earlier answers went

    requests.count = 0;
    capturing = &requests;
    EXPECT_TRUE(run_next_client_timer()); // the lost Client's retry
    capturing = &responses;
    responses.count = 0;
    EXPECT_TRUE(requests.count >= 1);
    carry_all(&requests, &server_context, CLIENT_IP);
    capturing = NULL;
    EXPECT_EQ_INT(4, served); // answered from the cache: the callback did not run again
    EXPECT_EQ_INT(1, runs[0]);
    carry_all(&responses, &client_context, SERVER_IP);
    EXPECT_EQ_INT(1, answers[lost]);
    EXPECT_EQ_U32(0 + ANSWER_OFFSET, last_answer[lost]);
    EXPECT_TRUE(clients[lost].cache == NULL);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

// A small deterministic generator: the same run every time.
static uint32_t rng_state;
static uint32_t rng(void) {
    rng_state = (rng_state * 1103515245U) + 12345U;
    return (rng_state >> 16) & 0x7fffU;
}

#define SOAK_CALLS_PER_CLIENT 200
#define SOAK_DROP_PERCENT 25
#define SOAK_DUPLICATE_PERCENT 10
#define SOAK_RETRY_COUNT 20
#define SOAK_RETRY_INTERVAL tt_MILLISECOND

// Each idle Client with calls left starts one, at random.
static void soak_start_calls(uint32_t issued[CLIENTS]) {
    for (int i = 0; i < CLIENTS; i++) {
        if (clients[i].cache == NULL && issued[i] < SOAK_CALLS_PER_CLIENT && (rng() % 2) == 0) {
            call(i, (issued[i]++ * CLIENTS) + (uint32_t)i);
        }
    }
}

// The captured requests to the server, each dropped with SOAK_DROP_PERCENT or else doubled with
// SOAK_DUPLICATE_PERCENT.
static void soak_deliver_requests(void) {
    for (int k = 0; k < requests.count; k++) {
        if (rng() % 100 < SOAK_DROP_PERCENT) {
            continue;
        }
        carry_one(&requests, k, &server_context, CLIENT_IP);
        if (rng() % 100 < SOAK_DUPLICATE_PERCENT) {
            carry_one(&requests, k, &server_context, CLIENT_IP);
        }
    }
}

// The captured responses to the clients, each dropped with SOAK_DROP_PERCENT.
static void soak_deliver_responses(void) {
    for (int k = 0; k < responses.count; k++) {
        if (rng() % 100 >= SOAK_DROP_PERCENT) {
            carry_one(&responses, k, &client_context, SERVER_IP);
        }
    }
}

// Every call made and none outstanding.
static bool soak_done(const uint32_t issued[CLIENTS]) {
    for (int i = 0; i < CLIENTS; i++) {
        if (clients[i].cache != NULL || issued[i] < SOAK_CALLS_PER_CLIENT) {
            return false;
        }
    }
    return true;
}

// Both Clients call, interleaved; every datagram is dropped with SOAK_DROP_PERCENT and a request delivered twice with
// SOAK_DUPLICATE_PERCENT; lost calls are retried by the Clients' own timers. Every call's callback runs exactly once,
// each Client gets each of its own answers once, and the server never holds more than one live answer per Client.
static void test_interleaved_calls_with_drops_run_each_callback_once(void) {
    setup();
    // A fixed retry interval, and enough sends that no call gives up at this loss (4 sends all fail 3.7% of the time
    // at 25% each way; 21, never in practice): a timeout is the retry budget, not the cache, and counts as a wrong
    // answer below. Fixed, because the mock clock jumps to whichever Client retries next, so a round trip measured
    // here includes the other Client's waits, and the auto path backs off to its deadline within a few sends.
    service.call_retry_count = SOAK_RETRY_COUNT;
    service.call_retry_interval = SOAK_RETRY_INTERVAL;
    rng_state = 20261009U;
    uint32_t issued[CLIENTS] = {0};
    int max_live = 0;
    int retries = 0;
    for (int step = 0; step < 100000 && !soak_done(issued); step++) {
        requests.count = 0;
        capturing = &requests;
        soak_start_calls(issued);
        bool idle = clients[0].cache == NULL && clients[1].cache == NULL;
        if (requests.count == 0 && !idle) {
            EXPECT_TRUE(run_next_client_timer()); // nothing new: let an outstanding call retry
            retries++;
        }
        capturing = &responses;
        responses.count = 0;
        soak_deliver_requests();
        capturing = NULL;
        soak_deliver_responses();
        int live = live_answers();
        max_live = live > max_live ? live : max_live;
    }
    for (int i = 0; i < CLIENTS; i++) {
        EXPECT_EQ_U32(SOAK_CALLS_PER_CLIENT, issued[i]);
        EXPECT_EQ_INT(SOAK_CALLS_PER_CLIENT, answers[i]);
        EXPECT_TRUE(clients[i].cache == NULL);
    }
    int twice = 0;
    for (uint32_t tag = 0; tag < SOAK_CALLS_PER_CLIENT * CLIENTS; tag++) {
        twice += runs[tag] > 1 ? 1 : 0;
        EXPECT_TRUE(runs[tag] >= 1);
    }
    EXPECT_EQ_INT(0, twice);
    EXPECT_EQ_INT(SOAK_CALLS_PER_CLIENT * CLIENTS, served);
    EXPECT_EQ_INT(0, wrong_answers); // a timeout counts here too
    EXPECT_TRUE(max_live <= CLIENTS);
    EXPECT_TRUE(retries > 100); // the shape under test: many retries, interleaved with the other Client's calls
    printf("soak: %d calls, %d retries, at most %d live answers\n", SOAK_CALLS_PER_CLIENT * CLIENTS, retries, max_live);
    service.call_retry_count = 0;
    service.call_retry_interval = 0;
    test_mock_send_hook = NULL;
}

// The context's seq_no counter is 16 bits; client 1 makes 65,536 calls after client 0's one call (seq_no s), so the
// counter comes round to s.
// - Client 0 got its answer and is idle (`outstanding` false): the server still holds that answer, and client 1's call
//   with seq_no s must run the callback and get its own answer, not client 0's - the server's lookup names the
//   client_tag as well as the seq_no.
// - Client 0's answer was lost (`outstanding` true): client 1 never takes s while client 0 is still waiting on it -
//   an answer reaches its Client by seq_no alone - and client 0's retry is answered from the cache with its own.
static void test_a_wrapped_seq_no(bool outstanding) {
    setup();
    requests.count = 0;
    capturing = &requests;
    call(0, 0);
    uint16_t first_seq_no = outstanding_seq_no(&clients[0]);
    capturing = &responses;
    responses.count = 0;
    carry_all(&requests, &server_context, CLIENT_IP);
    capturing = NULL;
    if (!outstanding) {
        carry_all(&responses, &client_context, SERVER_IP);
        EXPECT_EQ_INT(1, answers[0]);
    }
    int took_it = 0;
    for (uint32_t k = 0; k <= UINT16_MAX; k++) {
        requests.count = 0;
        capturing = &requests;
        call(1, TRACKED_CALLS + k);
        took_it += outstanding_seq_no(&clients[1]) == first_seq_no ? 1 : 0;
        capturing = &responses;
        responses.count = 0;
        carry_all(&requests, &server_context, CLIENT_IP);
        capturing = NULL;
        carry_all(&responses, &client_context, SERVER_IP);
    }
    // The shape under test: client 1's calls came round to client 0's seq_no, and took it only when it was free.
    EXPECT_EQ_INT(outstanding ? 0 : 1, took_it);
    EXPECT_EQ_INT(UINT16_MAX + 1, answers[1]);
    EXPECT_EQ_INT(UINT16_MAX + 2, served); // client 0's call and each of client 1's, once
    EXPECT_EQ_INT(0, wrong_answers);

    if (outstanding) {
        requests.count = 0;
        capturing = &requests;
        EXPECT_TRUE(run_next_client_timer()); // client 0's retry
        capturing = &responses;
        responses.count = 0;
        carry_all(&requests, &server_context, CLIENT_IP);
        capturing = NULL;
        EXPECT_EQ_INT(UINT16_MAX + 2, served); // from the cache
        carry_all(&responses, &client_context, SERVER_IP);
    }
    EXPECT_EQ_INT(1, answers[0]);
    EXPECT_EQ_U32(ANSWER_OFFSET, last_answer[0]);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

int main(void) {
    test_each_client_gets_its_own_answers(false);
    test_each_client_gets_its_own_answers(true);
    test_the_second_client_alone_is_answered();
    test_each_client_has_its_own_tag();
    test_a_retry_after_the_other_client_moved_on(0);
    test_a_retry_after_the_other_client_moved_on(1);
    test_interleaved_calls_with_drops_run_each_callback_once();
    test_a_wrapped_seq_no(false);
    test_a_wrapped_seq_no(true);

    if (test_result() != 0) {
        return 1;
    }
    printf("test_two_clients_one_service: all tests passed\n");
    return 0;
}
