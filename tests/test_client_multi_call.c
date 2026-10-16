/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Several calls outstanding at once (the user's decision of 2026-10-16, DESIGN.md section 7): a Client has
// tt_CLIENT_MAX_OUTSTANDING call slots, each with its own seq_no, retry timer and cached request, and a Server answers
// several calls of several Clients at once. Before, a Client had one call: rclcpp's async_send_request() twice in a
// row, or an action client's get_result for two goals, failed.
//
// Datagrams are carried between contexts by hand (the mock HAL captures what each sends), as in
// test_two_clients_one_service.c. Checked, in the default build (4) and in the one-call build (1, the
// tests-one-call binaries), with the depth written as N:
// - N calls in a row are all sent and all answered, the answers handed back in reverse order, and each callback names
//   its own call (callback_seq_no, as tt_Client_call_id() gave it) with its own answer;
// - call N + 1 is refused with tt_RET_ILLEGAL_STATUS and nothing is sent; once one call is answered, the next goes;
// - loss per slot: one call's request lost and another's answer lost, the rest answered - each lost call is retried
//   on its own timer, the server runs its callback once per call (the lost answer comes from its cache), and every
//   call is answered exactly once;
// - a call that is never answered times out on its own, naming itself, while the others are answered;
// - N calls lost together back the estimate off once, as one lost call does;
// - a deferred server holding N calls from each of two Clients of one context and one Client of another, answered
//   in reverse order with tt_Server_send_response(): each answer reaches its own call;
// - tt_CALL_IGNORED: the request is turned away unanswered and uncached, and the client's retry is answered.
// Mutants (tests/mutants_multi_call.py), each killed here.

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
#define OTHER_ID 3
#define SERVER_IP 0x0a000001
#define CLIENT_IP 0x0a000002
#define OTHER_IP 0x0a000003
#define PORT 40000
#define CLOCK_NS (100ULL * tt_SECOND)
#define CAPTURE_MAX 64
#define ANSWER_OFFSET 1000U
#define N tt_CLIENT_MAX_OUTSTANDING
#define CLIENTS 3 // two on CLIENT_ID, one on OTHER_ID
#define MAX_CALLS (CLIENTS * N)

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
    .name = "multi_call::srv::dds_::Echo_",
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
static struct tt_Context client_context; // clients[0] and clients[1]
static struct tt_Context other_context;  // clients[2]
static struct tt_Server server;
static struct tt_Client clients[CLIENTS];

// What the server does with a request.
enum serve_mode { SERVE_ECHO, SERVE_DEFER, SERVE_IGNORE_ONCE };
static enum serve_mode mode;
static int served;                       // server callback runs
static int runs[ANSWER_OFFSET];          // callback runs per request tag
static tt_RequestId deferred[MAX_CALLS]; // SERVE_DEFER: the requests held, in arrival order
static uint32_t deferred_tag[MAX_CALLS];
static int deferred_count;

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    served++;
    if (request->tag < ANSWER_OFFSET) {
        runs[request->tag]++;
    }
    if (mode == SERVE_IGNORE_ONCE) {
        mode = SERVE_ECHO;
        return tt_CALL_IGNORED;
    }
    if (mode == SERVE_DEFER) {
        if (deferred_count < MAX_CALLS) {
            deferred[deferred_count] = request_id;
            deferred_tag[deferred_count++] = request->tag;
        }
        return tt_CALL_DEFERRED;
    }
    response->tag = request->tag + ANSWER_OFFSET;
    return 0;
}

// What each call was, and what its callback said.
struct call_record {
    int client;
    uint16_t seq_no;
    uint32_t tag;
    int answers;  // callbacks naming this call with its own answer
    int timeouts; // callbacks naming this call with tt_CALL_TIMEOUT
};
static struct call_record records[MAX_CALLS * 4];
static int record_count;
static int wrong_answers; // a callback naming no call of its client, or carrying another call's answer

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
    for (int k = record_count - 1; k >= 0; k--) {
        struct call_record* rec = &records[k];
        if (rec->client != i || rec->seq_no != client->callback_seq_no || rec->answers + rec->timeouts != 0) {
            continue;
        }
        if (return_code == tt_CALL_TIMEOUT && response == NULL) {
            rec->timeouts++;
            return;
        }
        if (return_code == 0 && response != NULL && response->tag == rec->tag + ANSWER_OFFSET) {
            rec->answers++;
            return;
        }
        break;
    }
    wrong_answers++;
}

// What a context sends, kept for carrying to another.
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

static void carry_one(const struct capture* from, int i, struct tt_Context* to, uint32_t ip) {
    memcpy(to->rx_buffer, from->data[i], from->length[i]);
    (void)process_packet(to, to->rx_buffer, 0, (uint32_t)from->length[i], ip, PORT, tt_TRANSPORT_UDP);
}

// Requests go to the server; whatever the server sends meanwhile is captured as responses.
static void deliver_requests(const struct capture* from, uint32_t ip, const bool* drop) {
    capturing = &responses;
    for (int k = 0; k < from->count; k++) {
        if (drop == NULL || !drop[k]) {
            carry_one(from, k, &server_context, ip);
        }
    }
    capturing = NULL;
}

// A response goes to both client contexts: each takes only what is addressed to it.
static void deliver_response(int k) {
    carry_one(&responses, k, &client_context, SERVER_IP);
    carry_one(&responses, k, &other_context, SERVER_IP);
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
    init_context(&other_context, OTHER_ID);
    memset(&server, 0, sizeof(server));
    memset(clients, 0, sizeof(clients));
    memset(runs, 0, sizeof(runs));
    memset(records, 0, sizeof(records));
    record_count = 0;
    served = 0;
    wrong_answers = 0;
    deferred_count = 0;
    mode = SERVE_ECHO;
    service.call_retry_interval = 0;
    service.call_retry_count = 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_server(&server_context, &server, &service, "multi_call/echo",
                                                      (tt_SERVER_CALLBACK)serve));
    for (int i = 0; i < CLIENTS; i++) {
        struct tt_Context* context = i < 2 ? &client_context : &other_context;
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_client(context, &clients[i], &service, "multi_call/echo",
                                                          (tt_CLIENT_CALLBACK)answered));
    }
    test_mock_send_hook = capture;
}

static void teardown(void) {
    test_mock_send_hook = NULL;
    capturing = NULL;
}

// Client `i` calls with request tag `tag`, its CallRequest captured into `into`. Returns tt_Client_call_id()'s result.
static tt_ret_t call(int i, uint32_t tag, struct capture* into) {
    capturing = into;
    struct message request = {tag};
    uint16_t seq_no = 0;
    tt_ret_t ret = tt_Client_call_id(&clients[i], (struct tt_Request*)&request, &seq_no);
    capturing = NULL;
    if (ret == tt_RET_OK) {
        records[record_count++] = (struct call_record) {i, seq_no, tag, 0, 0};
    }
    return ret;
}

// Runs the earliest call retry of `context`, at its time, and anything due before it. False if none is pending.
static bool run_next_timer(struct tt_Context* context, struct capture* into) {
    uint64_t at = UINT64_MAX;
    for (uint32_t k = 0; k < context->scheduler_tail; k++) {
        const struct tt_TCB* entry = &context->scheduler[k];
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
    capturing = into;
    bool has_next = false;
    uint64_t next = 0;
    while (run_due_entry(context, test_mock_now, &has_next, &next)) {
    }
    capturing = NULL;
    return true;
}

static int total(int field_answers) {
    int sum = 0;
    for (int k = 0; k < record_count; k++) {
        sum += field_answers ? records[k].answers : records[k].timeouts;
    }
    return sum;
}

// N calls in a row, all outstanding at once, answered in reverse order: each callback names its own call.
static void test_n_calls_answered_out_of_order(void) {
    setup();
    requests.count = 0;
    for (int k = 0; k < N; k++) {
        EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)k, &requests));
    }
    EXPECT_EQ_INT(N, requests.count);
    EXPECT_EQ_U32(N, tt_Client_outstanding(&clients[0]));
    for (int a = 0; a < N; a++) {
        for (int b = a + 1; b < N; b++) {
            EXPECT_TRUE(records[a].seq_no != records[b].seq_no);
        }
    }
    responses.count = 0;
    deliver_requests(&requests, CLIENT_IP, NULL);
    EXPECT_EQ_INT(N, responses.count);
    for (int k = responses.count - 1; k >= 0; k--) {
        deliver_response(k);
    }
    EXPECT_EQ_INT(N, served);
    EXPECT_EQ_INT(N, total(1));
    EXPECT_EQ_INT(0, total(0));
    EXPECT_EQ_INT(0, wrong_answers);
    EXPECT_EQ_U32(0, tt_Client_outstanding(&clients[0]));
    teardown();
}

// Call N + 1 is refused, with nothing sent and nothing changed; once one call is answered the next is accepted.
static void test_one_more_than_n_is_refused(void) {
    setup();
    requests.count = 0;
    for (int k = 0; k < N; k++) {
        EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)k, &requests));
    }
    int sends = test_mock_send_call_count + test_mock_send_to_call_count;
    uint16_t counter = client_context.call_seq_no;
    EXPECT_EQ_INT(tt_RET_ILLEGAL_STATUS, call(0, (uint32_t)N, &requests));
    EXPECT_EQ_INT(sends, test_mock_send_call_count + test_mock_send_to_call_count);
    EXPECT_EQ_INT(N, requests.count);
    EXPECT_EQ_U32(counter, client_context.call_seq_no);
    EXPECT_EQ_U32(N, tt_Client_outstanding(&clients[0]));
    // Another Client of the same service in the same context is not limited by this one's calls.
    EXPECT_EQ_INT(tt_RET_OK, call(1, (uint32_t)(N + 1), &requests));

    responses.count = 0;
    bool only_first[CAPTURE_MAX] = {false};
    for (int k = 1; k < requests.count; k++) {
        only_first[k] = true;
    }
    deliver_requests(&requests, CLIENT_IP, only_first); // the first call alone reaches the server
    EXPECT_EQ_INT(1, responses.count);
    deliver_response(0);
    EXPECT_EQ_INT(1, records[0].answers);
    EXPECT_EQ_U32(N - 1, tt_Client_outstanding(&clients[0]));
    requests.count = 0;
    EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)(N + 2), &requests)); // a slot is free again
    EXPECT_EQ_INT(1, requests.count);
    EXPECT_EQ_INT(0, wrong_answers);
    teardown();
}

// Two of N calls hit loss: call 0's request, and call 1's answer while the later calls are answered - so its
// answer must outlive theirs at the server. Each lost call is retried on its own timer and answered once; the server
// runs its callback once per call.
static void test_loss_and_retry_per_slot(void) {
    setup();
    service.call_retry_interval = tt_MILLISECOND; // fixed, so the timers are predictable
    service.call_retry_count = 5;
    requests.count = 0;
    for (int k = 0; k < N; k++) {
        EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)k, &requests));
    }
    bool drop_first[CAPTURE_MAX] = {true};
    responses.count = 0;
    deliver_requests(&requests, CLIENT_IP, drop_first);
    EXPECT_EQ_INT(N - 1, responses.count);
    for (int k = 1; k < responses.count; k++) { // the first answer (call 1's) is lost, the later ones arrive
        deliver_response(k);
    }
    int answered_now = N == 1 ? 0 : N - 2;
    EXPECT_EQ_INT(answered_now, total(1));
    EXPECT_EQ_U32((uint32_t)(N - answered_now), tt_Client_outstanding(&clients[0]));

    for (int round = 0; round < 3 && tt_Client_outstanding(&clients[0]) != 0; round++) {
        requests.count = 0;
        EXPECT_TRUE(run_next_timer(&client_context, &requests));
        responses.count = 0;
        deliver_requests(&requests, CLIENT_IP, NULL);
        for (int k = 0; k < responses.count; k++) {
            deliver_response(k);
        }
    }
    EXPECT_EQ_U32(0, tt_Client_outstanding(&clients[0]));
    EXPECT_EQ_INT(N, total(1));
    EXPECT_EQ_INT(0, total(0));
    EXPECT_EQ_INT(N, served); // the lost answer came from the cache
    for (int k = 0; k < N; k++) {
        EXPECT_EQ_INT(1, runs[k]);
        EXPECT_EQ_INT(1, records[k].answers);
    }
    EXPECT_EQ_INT(0, wrong_answers);
    teardown();
}

// A call that is never answered times out on its own, naming itself; the others are answered meanwhile, and its slot
// takes a new call afterwards.
static void test_one_call_times_out_alone(void) {
    setup();
    service.call_retry_interval = tt_MILLISECOND;
    service.call_retry_count = 2;
    requests.count = 0;
    for (int k = 0; k < N; k++) {
        EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)k, &requests));
    }
    bool drop_first[CAPTURE_MAX] = {true};
    responses.count = 0;
    deliver_requests(&requests, CLIENT_IP, drop_first);
    for (int k = 0; k < responses.count; k++) {
        deliver_response(k);
    }
    while (run_next_timer(&client_context, NULL)) { // every retry of call 0 is lost too
    }
    EXPECT_EQ_INT(1, records[0].timeouts);
    EXPECT_EQ_INT(0, records[0].answers);
    for (int k = 1; k < N; k++) {
        EXPECT_EQ_INT(1, records[k].answers);
    }
    EXPECT_EQ_INT(0, wrong_answers);
    EXPECT_EQ_U32(0, tt_Client_outstanding(&clients[0]));
    EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)N, NULL));
    teardown();
}

// N calls lost together on the auto path: the estimate doubles once, as for one lost call, not N times.
static void test_calls_lost_together_back_off_once(void) {
    setup();
    clients[0].latency = 100 * tt_MICROSECOND;
    clients[0].latency_var = 10 * tt_MICROSECOND;
    for (int k = 0; k < N; k++) {
        EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)k, NULL));
    }
    while (run_next_timer(&client_context, NULL)) { // nothing is ever answered
    }
    EXPECT_EQ_INT(N, total(0));
    EXPECT_EQ_U32(200 * tt_MICROSECOND, clients[0].latency);
    EXPECT_TRUE(clients[0].latency_backed_off);
    // A call made after that backoff, lost too, doubles it again: it ran on the new estimate.
    EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)N, NULL));
    while (run_next_timer(&client_context, NULL)) {
    }
    EXPECT_EQ_U32(400 * tt_MICROSECOND, clients[0].latency);
    teardown();
}

// A deferred server holds N calls from each of three Clients - two of one context, one of another - and answers them
// in reverse order of arrival; each answer reaches its own call.
static void test_deferred_server_many_clients_many_calls(void) {
    setup();
    mode = SERVE_DEFER;
    struct capture* from_other = &responses; // reused below: requests of the other context
    requests.count = 0;
    from_other->count = 0;
    for (int k = 0; k < N; k++) {
        EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)(3 * k), &requests));
        EXPECT_EQ_INT(tt_RET_OK, call(1, (uint32_t)((3 * k) + 1), &requests));
        EXPECT_EQ_INT(tt_RET_OK, call(2, (uint32_t)((3 * k) + 2), from_other));
    }
    struct capture other_requests = *from_other;
    responses.count = 0;
    deliver_requests(&requests, CLIENT_IP, NULL);
    deliver_requests(&other_requests, OTHER_IP, NULL);
    EXPECT_EQ_INT(0, responses.count); // all deferred
    EXPECT_EQ_INT(CLIENTS * N, deferred_count);
    EXPECT_EQ_INT(CLIENTS * N, served);

    for (int d = deferred_count - 1; d >= 0; d--) {
        struct message answer = {deferred_tag[d] + ANSWER_OFFSET};
        responses.count = 0;
        capturing = &responses;
        EXPECT_EQ_INT(tt_RET_OK, tt_Server_send_response(&server, deferred[d], 0, (struct tt_Response*)&answer));
        capturing = NULL;
        EXPECT_EQ_INT(1, responses.count);
        deliver_response(0);
    }
    EXPECT_EQ_INT(CLIENTS * N, total(1));
    EXPECT_EQ_INT(0, wrong_answers);
    for (int i = 0; i < CLIENTS; i++) {
        EXPECT_EQ_U32(0, tt_Client_outstanding(&clients[i]));
    }
    // A request id answered already is not found again.
    struct message again = {0};
    EXPECT_EQ_INT(tt_RET_NOT_FOUND, tt_Server_send_response(&server, deferred[0], 0, (struct tt_Response*)&again));
    teardown();
}

// tt_CALL_IGNORED: the first request is turned away with nothing sent or cached; the retry runs the callback again
// and is answered.
static void test_ignored_request_is_answered_on_retry(void) {
    setup();
    service.call_retry_interval = tt_MILLISECOND;
    service.call_retry_count = 3;
    mode = SERVE_IGNORE_ONCE;
    requests.count = 0;
    EXPECT_EQ_INT(tt_RET_OK, call(0, 7, &requests));
    responses.count = 0;
    deliver_requests(&requests, CLIENT_IP, NULL);
    EXPECT_EQ_INT(0, responses.count);
    EXPECT_EQ_INT(1, served);
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        EXPECT_TRUE(server.cache[i] == NULL);
        EXPECT_EQ_INT(tt_SERVER_SLOT_EMPTY, server.slot_state[i]);
    }
    requests.count = 0;
    EXPECT_TRUE(run_next_timer(&client_context, &requests));
    responses.count = 0;
    deliver_requests(&requests, CLIENT_IP, NULL);
    EXPECT_TRUE(responses.count >= 1); // the answer (and whatever the server context had due, an announce)
    for (int k = 0; k < responses.count; k++) {
        deliver_response(k);
    }
    EXPECT_EQ_INT(2, served);
    EXPECT_EQ_INT(1, records[0].answers);
    EXPECT_EQ_INT(0, wrong_answers);
    teardown();
}

// With storage attached (tt_Client_set_storage(), as rmw_tickle does), each call keeps its request in its own share
// of the area: all N requests lost, the retries carry each call's own request, and each is answered with its own.
#define SLOT_BYTES 64
static tt_ALIGNAS(8) uint8_t attached[N * SLOT_BYTES];

static void test_attached_storage_keeps_each_request(void) {
    setup();
    service.call_retry_interval = tt_MILLISECOND;
    service.call_retry_count = 3;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_set_storage(&clients[0], attached, sizeof(attached)));
    for (int k = 0; k < N; k++) {
        EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)(10 + k), NULL)); // every first send lost
    }
    for (int k = 0; k < N; k++) {
        const uint8_t* cache = (const uint8_t*)clients[0].calls[k].cache;
        EXPECT_TRUE(cache >= attached + ((size_t)k * SLOT_BYTES) && cache < attached + ((size_t)(k + 1) * SLOT_BYTES));
    }
    requests.count = 0;
    EXPECT_TRUE(run_next_timer(&client_context, &requests)); // the retries, all due at once
    responses.count = 0;
    deliver_requests(&requests, CLIENT_IP, NULL);
    for (int k = 0; k < responses.count; k++) {
        deliver_response(k);
    }
    EXPECT_EQ_INT(N, total(1));
    EXPECT_EQ_INT(0, wrong_answers);
    EXPECT_EQ_U32(0, tt_Client_outstanding(&clients[0]));
    // Too small a share for the request: refused, as one call's storage always was.
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_set_storage(&clients[0], attached, N * 8));
    EXPECT_EQ_INT(tt_RET_OUT_OF_BUFFER, call(0, 99, NULL));
    teardown();
}

// The window (tt_CLIENT_CALL_WINDOW): call 0's answer is lost, and the Client goes on calling through its other slots,
// each call answered at once. It makes tt_CLIENT_CALL_WINDOW - 1 calls after call 0, then is refused although slots are
// free: one more answer could push call 0's out of the server's cache. Call 0's retry is answered from that cache - the
// callback does not run again - and the Client may call again.
static void test_window_keeps_a_lost_answer(void) {
    if (N == 1) {
        return; // a second call is refused by the slots already
    }
    setup();
    service.call_retry_interval = tt_MILLISECOND;
    service.call_retry_count = 5;
    requests.count = 0;
    EXPECT_EQ_INT(tt_RET_OK, call(0, 0, &requests));
    responses.count = 0;
    deliver_requests(&requests, CLIENT_IP, NULL); // answered, and the answer lost
    int made = 1;
    while (made < 4 * tt_CLIENT_CALL_WINDOW) {
        requests.count = 0;
        if (call(0, (uint32_t)made, &requests) != tt_RET_OK) {
            break;
        }
        made++;
        responses.count = 0;
        deliver_requests(&requests, CLIENT_IP, NULL);
        for (int k = 0; k < responses.count; k++) {
            deliver_response(k);
        }
    }
    EXPECT_EQ_INT(tt_CLIENT_CALL_WINDOW, made);
    EXPECT_EQ_U32(1, tt_Client_outstanding(&clients[0]));
    EXPECT_EQ_INT(tt_CLIENT_CALL_WINDOW, served);

    requests.count = 0;
    EXPECT_TRUE(run_next_timer(&client_context, &requests)); // call 0's retry
    responses.count = 0;
    deliver_requests(&requests, CLIENT_IP, NULL);
    for (int k = 0; k < responses.count; k++) {
        deliver_response(k);
    }
    EXPECT_EQ_INT(tt_CLIENT_CALL_WINDOW, served); // from the cache
    EXPECT_EQ_INT(1, records[0].answers);
    EXPECT_EQ_INT(tt_CLIENT_CALL_WINDOW, total(1));
    EXPECT_EQ_INT(tt_RET_OK, call(0, (uint32_t)made, NULL)); // open again
    EXPECT_EQ_INT(0, wrong_answers);
    teardown();
}

int main(void) {
    test_window_keeps_a_lost_answer();
    test_attached_storage_keeps_each_request();
    test_n_calls_answered_out_of_order();
    test_one_more_than_n_is_refused();
    test_loss_and_retry_per_slot();
    test_one_call_times_out_alone();
    test_calls_lost_together_back_off_once();
    test_deferred_server_many_clients_many_calls();
    test_ignored_request_is_answered_on_retry();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_client_multi_call: all tests passed (tt_CLIENT_MAX_OUTSTANDING %d)\n", N);
    return 0;
}
