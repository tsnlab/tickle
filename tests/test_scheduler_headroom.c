/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A full scheduler (tt_MAX_SCHEDULER_LENGTH) under services at full depth (2026-10-16). Each request a Server holds
// deferred and each answer it keeps for retries holds a scheduler entry, so with several calls outstanding per Client
// (tt_CLIENT_MAX_OUTSTANDING) a busy service can fill a context's scheduler. Before this test, a request that found it
// full ran its callback and was then dropped: a deferred request lost its slot after the application had been handed
// it (its answer then found nothing, tt_RET_NOT_FOUND), a synchronous answer was computed and never sent, and the
// client's retry ran the callback a second time - every refusal logged as an ERROR of its own.
//
// Checked here, with the scheduler filled by unrelated far-future entries ("everything else a context schedules")
// and CLIENTS Clients, each on a context of its own, calling at full depth (N):
// - deferred: a request that finds no room is turned away before its callback runs; every callback that ran is
//   answered; the refusals are counted (sched_full) and said once; once room returns, the retries are served and
//   every call is answered with its callback run exactly once;
// - synchronous: the same with no room at all;
// - the warning is rate-limited (one per tt_SCHED_FULL_WARN_INTERVAL) and the peak occupancy is kept;
// - an entry waiting in the scheduler inbox counts as taken.
// Mutants (tests/mutants_scheduler_headroom.py), each killed here.

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
#define SERVER_IP 0x0a000001
#define CLIENT_IP_BASE 0x0a000010
#define PORT 40000
#define CLOCK_NS (100ULL * tt_SECOND)
#define CAPTURE_MAX 64
#define ANSWER_OFFSET 1000U
#define N tt_CLIENT_MAX_OUTSTANDING
#define CLIENTS 5
#define MAX_CALLS (CLIENTS * N)
#define HEADROOM 3 // free scheduler entries left at the server in the deferred case

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
    .name = "headroom::srv::dds_::Echo_",
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
static struct tt_Context client_contexts[CLIENTS];
static struct tt_Server server;
static struct tt_Client clients[CLIENTS];

static bool defer;
static int served;              // server callback runs
static int runs[ANSWER_OFFSET]; // callback runs per request tag
static tt_RequestId held[MAX_CALLS * 4];
static uint32_t held_tag[MAX_CALLS * 4];
static int held_count;

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    served++;
    if (request->tag < ANSWER_OFFSET) {
        runs[request->tag]++;
    }
    if (defer) {
        held[held_count] = request_id;
        held_tag[held_count++] = request->tag;
        return tt_CALL_DEFERRED;
    }
    response->tag = request->tag + ANSWER_OFFSET;
    return 0;
}

static int answers[ANSWER_OFFSET]; // answers per request tag
static int wrong_answers;
static int timeouts;

static void answered(struct tt_Client* client, int8_t return_code, struct message* response) {
    (void)client;
    if (return_code == tt_CALL_TIMEOUT) {
        timeouts++;
        return;
    }
    if (return_code != 0 || response == NULL || response->tag < ANSWER_OFFSET ||
        response->tag - ANSWER_OFFSET >= ANSWER_OFFSET) {
        wrong_answers++;
        return;
    }
    answers[response->tag - ANSWER_OFFSET]++;
}

struct capture {
    uint8_t data[CAPTURE_MAX][tt_MAX_BUFFER_LENGTH * 2];
    size_t length[CAPTURE_MAX];
    uint32_t ip[CAPTURE_MAX];
    int count;
};
static struct capture requests;
static struct capture responses;
static struct capture* capturing;
static uint32_t capturing_ip;

static void capture(const void* buf, size_t len) {
    if (capturing != NULL && capturing->count < CAPTURE_MAX && len <= sizeof(capturing->data[0])) {
        memcpy(capturing->data[capturing->count], buf, len);
        capturing->ip[capturing->count] = capturing_ip;
        capturing->length[capturing->count++] = len;
    }
}

static void carry(const struct capture* from, int k, struct tt_Context* to, uint32_t ip) {
    memcpy(to->rx_buffer, from->data[k], from->length[k]);
    (void)process_packet(to, to->rx_buffer, 0, (uint32_t)from->length[k], ip, PORT, tt_TRANSPORT_UDP);
}

// Every captured request to the server; whatever the server sends meanwhile is captured as responses.
static void deliver_requests(void) {
    capturing = &responses;
    capturing_ip = SERVER_IP;
    for (int k = 0; k < requests.count; k++) {
        carry(&requests, k, &server_context, requests.ip[k]);
    }
    capturing = NULL;
    requests.count = 0;
}

static void deliver_responses(void) {
    for (int k = 0; k < responses.count; k++) {
        for (int i = 0; i < CLIENTS; i++) {
            carry(&responses, k, &client_contexts[i], SERVER_IP);
        }
    }
    responses.count = 0;
}

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_TX_BUFFER_LENGTH;
    context->last_modified = CLOCK_NS;
}

// "Everything else a context schedules": entries an hour away, never due in these tests.
static void other_work(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
}
static int other_tokens[tt_MAX_SCHEDULER_LENGTH];

// Fills `node`'s scheduler with other_work() until `free` entries are left.
static void fill_scheduler(struct tt_Context* node, int free) {
    int k = 0;
    while (tt_MAX_SCHEDULER_LENGTH - 1 - node->scheduler_tail > free) {
        EXPECT_TRUE(tt_Context_schedule(node, CLOCK_NS + (3600 * tt_SECOND), other_work, &other_tokens[k++]));
    }
}

static void unfill_scheduler(struct tt_Context* node) {
    for (int k = 0; k < tt_MAX_SCHEDULER_LENGTH; k++) {
        (void)tt_Context_unschedule(node, other_work, &other_tokens[k]);
    }
}

static FILE* log_file;

static int log_lines_with(const char* text) {
    fflush(log_file);
    (void)fseek(log_file, 0, SEEK_SET);
    char line[1024];
    int count = 0;
    while (fgets(line, sizeof(line), log_file) != NULL) {
        if (strstr(line, text) != NULL) {
            count++;
        }
    }
    fseek(log_file, 0, SEEK_END);
    return count;
}

static void setup(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&server_context, SERVER_ID);
    memset(&server, 0, sizeof(server));
    memset(clients, 0, sizeof(clients));
    memset(runs, 0, sizeof(runs));
    memset(answers, 0, sizeof(answers));
    served = 0;
    held_count = 0;
    wrong_answers = 0;
    timeouts = 0;
    defer = false;
    requests.count = 0;
    responses.count = 0;
    service.call_retry_interval = tt_MILLISECOND; // fixed, so the retries are predictable
    service.call_retry_count = 20;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_server(&server_context, &server, &service, "headroom/echo",
                                                      (tt_SERVER_CALLBACK)serve));
    for (int i = 0; i < CLIENTS; i++) {
        init_context(&client_contexts[i], (uint8_t)(2 + i));
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_client(&client_contexts[i], &clients[i], &service, "headroom/echo",
                                                          (tt_CLIENT_CALLBACK)answered));
    }
    test_mock_send_hook = capture;
    log_file = tmpfile();
    tt_log_set_output(log_file);
}

static void teardown(void) {
    test_mock_send_hook = NULL;
    capturing = NULL;
    tt_log_set_output(stderr);
    fclose(log_file);
    log_file = NULL;
}

// Every Client calls N times, tags 0 .. MAX_CALLS - 1, its requests captured.
static void call_all(void) {
    for (int k = 0; k < N; k++) {
        for (int i = 0; i < CLIENTS; i++) {
            struct message request = {(uint32_t)((k * CLIENTS) + i)};
            capturing = &requests;
            capturing_ip = CLIENT_IP_BASE + (uint32_t)i;
            EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&clients[i], (struct tt_Request*)&request));
            capturing = NULL;
        }
    }
}

// Each client's retries due by now + 1 ms, their requests captured.
static void run_client_retries(void) {
    test_mock_now += tt_MILLISECOND;
    for (int i = 0; i < CLIENTS; i++) {
        capturing = &requests;
        capturing_ip = CLIENT_IP_BASE + (uint32_t)i;
        bool has_next = false;
        uint64_t next = 0;
        while (run_due_entry(&client_contexts[i], test_mock_now, &has_next, &next)) {
        }
        capturing = NULL;
    }
}

// Answers every request the server callback holds deferred; returns how many tt_Server_send_response() found.
static int answer_held(int from) {
    int found = 0;
    capturing = &responses;
    capturing_ip = SERVER_IP;
    for (int d = from; d < held_count; d++) {
        struct message answer = {held_tag[d] + ANSWER_OFFSET};
        if (tt_Server_send_response(&server, held[d], 0, (struct tt_Response*)&answer) == tt_RET_OK) {
            found++;
        }
    }
    capturing = NULL;
    return found;
}

static int pending_slots(void) {
    int count = 0;
    for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        count += server.slot_state[i] == tt_SERVER_SLOT_PENDING;
    }
    return count;
}

static int total(const int* per_tag) {
    int sum = 0;
    for (int k = 0; k < MAX_CALLS; k++) {
        sum += per_tag[k];
    }
    return sum;
}

static int max_runs(void) {
    int most = 0;
    for (int k = 0; k < MAX_CALLS; k++) {
        most = runs[k] > most ? runs[k] : most;
    }
    return most;
}

// A deferred service, HEADROOM entries free, MAX_CALLS requests at once.
static void test_deferred_requests_past_a_full_scheduler(void) {
    setup();
    defer = true;
    fill_scheduler(&server_context, HEADROOM);
    call_all();
    deliver_requests();
    int first_served = served;
    int first_pending = pending_slots();
    int first_found = answer_held(0);
    deliver_responses();
    int first_answers = total(answers);
    int refused = (int)server_context.sched_full;
    int warned = log_lines_with("scheduler full");
    int errors = log_lines_with("Cannot schedule");
    printf("deferred, %d calls, %d entries free: callbacks %d, held %d, send_response found %d, answered %d, "
           "sched_full %d, warnings %d, per-event errors %d\n",
           MAX_CALLS, HEADROOM, first_served, first_pending, first_found, first_answers, refused, warned, errors);

    // A callback runs only when its request can be held: each one that ran is answered.
    EXPECT_EQ_INT(first_pending, first_served);
    EXPECT_EQ_INT(first_served, first_found);
    EXPECT_EQ_INT(first_served, first_answers);
    EXPECT_TRUE(first_served >= 1 && first_served <= HEADROOM);
    EXPECT_EQ_INT(MAX_CALLS - first_served, refused); // the rest turned away, counted
    EXPECT_EQ_INT(1, warned);                         // and said once
    EXPECT_EQ_INT(0, errors);

    // Room again: the retries are served, every call answered, no callback run twice.
    unfill_scheduler(&server_context);
    int from = held_count;
    for (int round = 0; round < 5 && total(answers) < MAX_CALLS; round++) {
        run_client_retries();
        deliver_requests();
        (void)answer_held(from);
        from = held_count;
        deliver_responses();
    }
    printf("  after room returns: answered %d of %d, callbacks %d, most runs of one request %d\n", total(answers),
           MAX_CALLS, served, max_runs());
    EXPECT_EQ_INT(MAX_CALLS, total(answers));
    EXPECT_EQ_INT(MAX_CALLS, served);
    EXPECT_EQ_INT(1, max_runs());
    EXPECT_EQ_INT(0, wrong_answers);
    EXPECT_EQ_INT(0, timeouts);
    teardown();
}

// A synchronous service with no room at all: nothing is computed and lost; once room returns every call is answered.
static void test_synchronous_answers_past_a_full_scheduler(void) {
    setup();
    fill_scheduler(&server_context, 0);
    call_all();
    deliver_requests();
    int sent = responses.count;
    int first_served = served;
    deliver_responses();
    int first_answers = total(answers);
    printf("synchronous, %d calls, no entry free: callbacks %d, responses sent %d, answered %d, sched_full %lu, "
           "warnings %d, per-event errors %d\n",
           MAX_CALLS, first_served, sent, first_answers, (unsigned long)server_context.sched_full,
           log_lines_with("scheduler full"), log_lines_with("Cannot schedule"));
    EXPECT_EQ_INT(0, first_served);
    EXPECT_EQ_INT(0, first_answers);
    EXPECT_EQ_INT(MAX_CALLS, (int)server_context.sched_full);
    EXPECT_EQ_INT(1, log_lines_with("scheduler full"));

    unfill_scheduler(&server_context);
    for (int round = 0; round < 5 && total(answers) < MAX_CALLS; round++) {
        run_client_retries();
        deliver_requests();
        deliver_responses();
    }
    printf("  after room returns: answered %d of %d, callbacks %d, most runs of one request %d\n", total(answers),
           MAX_CALLS, served, max_runs());
    EXPECT_EQ_INT(MAX_CALLS, total(answers));
    EXPECT_EQ_INT(MAX_CALLS, served);
    EXPECT_EQ_INT(1, max_runs());
    EXPECT_EQ_INT(0, wrong_answers);
    teardown();
}

// tt_Context_schedule() into a full scheduler: refused, counted every time, said once per interval; the peak kept.
static void test_full_scheduler_is_counted_and_said_once_per_interval(void) {
    setup();
    int before = server_context.scheduler_tail;
    fill_scheduler(&server_context, 0);
    EXPECT_EQ_INT(tt_MAX_SCHEDULER_LENGTH - 1, server_context.sched_peak);
    for (int k = 0; k < 10; k++) {
        EXPECT_TRUE(!tt_Context_schedule(&server_context, CLOCK_NS, other_work, NULL));
    }
    EXPECT_EQ_INT(10, (int)server_context.sched_full);
    EXPECT_EQ_INT(1, log_lines_with("scheduler full"));
    test_mock_now += tt_SCHED_FULL_WARN_INTERVAL - 1;
    EXPECT_TRUE(!tt_Context_schedule(&server_context, CLOCK_NS, other_work, NULL));
    EXPECT_EQ_INT(1, log_lines_with("scheduler full"));
    test_mock_now += 1;
    EXPECT_TRUE(!tt_Context_schedule(&server_context, CLOCK_NS, other_work, NULL));
    EXPECT_EQ_INT(2, log_lines_with("scheduler full"));
    EXPECT_EQ_INT(12, (int)server_context.sched_full);
    // The peak stays where it was when the entries go.
    unfill_scheduler(&server_context);
    EXPECT_EQ_INT(before, server_context.scheduler_tail);
    EXPECT_EQ_INT(tt_MAX_SCHEDULER_LENGTH - 1, server_context.sched_peak);
    teardown();
}

// An entry another thread put in the scheduler inbox takes room too: the next drain moves it into the heap. With one
// entry free and one waiting there, a request is turned away.
static void test_inbox_entries_count_as_taken(void) {
    setup();
    defer = true;
    fill_scheduler(&server_context, 1);
    EXPECT_TRUE(sched_inbox_push(&server_context, CLOCK_NS + (3600 * tt_SECOND), other_work, NULL));
    call_all();
    deliver_requests();
    EXPECT_EQ_INT(0, served);
    EXPECT_EQ_INT(MAX_CALLS, (int)server_context.sched_full);
    // Drained, the inbox entry fills the heap: nothing it held was lost to a deferral.
    state_lock(&server_context);
    sched_drain_inbox(&server_context);
    state_unlock(&server_context);
    EXPECT_EQ_INT(tt_MAX_SCHEDULER_LENGTH - 1, server_context.scheduler_tail);
    EXPECT_EQ_INT(0, (int)server_context.sched_inbox_pending);
    teardown();
}

int main(void) {
    test_deferred_requests_past_a_full_scheduler();
    test_synchronous_answers_past_a_full_scheduler();
    test_full_scheduler_is_counted_and_said_once_per_interval();
    test_inbox_entries_count_as_taken();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_scheduler_headroom: all tests passed (tt_MAX_SCHEDULER_LENGTH %d, tt_CLIENT_MAX_OUTSTANDING %d)\n",
           tt_MAX_SCHEDULER_LENGTH, N);
    return 0;
}
