/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A kept answer's expiry is a time in its slot, checked when the slot is looked up or reused (2026-10-21).
//
// A Server keeps each answered response for its client's retry horizon (server_cache_lifetime(); 5.1 s for
// rmw_tickle's 100 ms x 50), and a calling Client keeps up to tt_CLIENT_CALL_WINDOW (8) of them. Until this date each
// kept answer held one scheduler entry, its expiry timer, so about 16 Client-Server pairs calling at 1.6 Hz or faster
// into one context held most of its tt_MAX_SCHEDULER_LENGTH (128) entries, and a full scheduler turns requests away
// (sched_has_room()) and refuses other timers.
//
// Checked here, each reading written into its expectation:
// - scale: PAIRS Client-Server pairs, every Server in one context, each Client calling CALLS times 10 ms apart (100 Hz)
//   against the rmw-shaped 5.1 s horizon. The answers kept number more than the scheduler holds entries - the load is
//   one that filled it before - and every call is answered at once, its callback run once, with the Server context's
//   scheduler peak no more than SCHED_BOUND above its idle peak and nothing refused (sched_full 0). Built against the
//   design before this date the same arm fills the scheduler and turns calls away.
// - expiry: an answer is served until one lifetime after it went out and not from then on; once expired its slot
//   still names the call and when it went out (what learn_from_expired_response() learns from), with no live answer.
// - re-arm: a retry served from the cache keeps the answer for a lifetime from that retry, not from the first answer.
// - reuse: an expired slot is reused before a live answer is evicted - with the lifetime shortened in between, an
//   answer sent before the expired ones is still live, and must stay.
// Mutants (tests/mutants_kept_answer_lazy_expiry.py), each killed here or by the services tests it runs with.

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
#define ANSWER_OFFSET 1000U

// rmw_tickle's client retry, as test_service_retry_horizon.c restates it: 100 ms x 50, a 5.1 s horizon.
#define RMW_INTERVAL 100000000U
#define RMW_COUNT ((uint32_t)(tt_SERVER_DEFERRED_RESPONSE_TIMEOUT / RMW_INTERVAL))

// Answers one calling Client keeps (set_server_cache()): one per Client with one call each, else the call window.
#define KEPT_PER_PAIR (tt_CLIENT_MAX_OUTSTANDING == 1 ? 1 : tt_CLIENT_CALL_WINDOW)
// Enough pairs that their kept answers outnumber the scheduler's entries, and at least 16.
#define PAIRS_FOR_LOAD ((tt_MAX_SCHEDULER_LENGTH / KEPT_PER_PAIR) + 1)
#define PAIRS (PAIRS_FOR_LOAD > 16 ? PAIRS_FOR_LOAD : 16)
#define CALLS (KEPT_PER_PAIR + 4) // each Client fills its window and goes on calling
#define CALL_GAP (10ULL * tt_MILLISECOND)
#define SCHED_BOUND 2 // entries the Server context may hold above its idle peak while the pairs call

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

#define ECHO_SERVICE                                                   \
    {                                                                  \
        .name = "lazy_expiry::srv::dds_::Echo_",                       \
        .request_size = sizeof(struct message),                        \
        .response_size = sizeof(struct message),                       \
        .request_encode_size = (tt_REQUEST_ENCODE_SIZE)message_size,   \
        .request_encode = (tt_REQUEST_ENCODE)message_encode,           \
        .request_decode = (tt_REQUEST_DECODE)message_decode,           \
        .request_free = (tt_REQUEST_FREE)message_free,                 \
        .response_encode_size = (tt_RESPONSE_ENCODE_SIZE)message_size, \
        .response_encode = (tt_RESPONSE_ENCODE)message_encode,         \
        .response_decode = (tt_RESPONSE_DECODE)message_decode,         \
        .response_free = (tt_RESPONSE_FREE)message_free,               \
    }

// One tt_Service per side, as rmw_tickle has; the pairs differ by endpoint name, so by endpoint_id.
static struct tt_Service server_service = ECHO_SERVICE;
static struct tt_Service client_service = ECHO_SERVICE;

static struct tt_Context server_context;
static struct tt_Context client_context;
static struct tt_Server servers[PAIRS];
static struct tt_Client clients[PAIRS];
static char names[PAIRS][32];

static int runs[PAIRS][CALLS];
static int answers[PAIRS][CALLS];
static int wrong_answers;

// The request's tag is pair * CALLS + call.
static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    (void)request_id;
    if (request->tag < (uint32_t)(PAIRS * CALLS)) {
        runs[request->tag / CALLS][request->tag % CALLS]++;
    }
    response->tag = request->tag + ANSWER_OFFSET;
    return 0;
}

static void answered(struct tt_Client* client, int8_t return_code, struct message* response) {
    uint32_t pair = (uint32_t)(client - clients);
    if (return_code != 0 || response == NULL || response->tag < ANSWER_OFFSET ||
        response->tag - ANSWER_OFFSET >= (uint32_t)(PAIRS * CALLS) || (response->tag - ANSWER_OFFSET) / CALLS != pair) {
        wrong_answers++;
        return;
    }
    answers[pair][(response->tag - ANSWER_OFFSET) % CALLS]++;
}

// What one context sends between two deliveries: a call's request, or the answer to it.
static uint8_t captured[4][tt_MAX_BUFFER_LENGTH * 2];
static size_t captured_length[4];
static int captured_count;
static bool capturing;

static void capture(const void* buf, size_t len) {
    if (capturing && captured_count < 4 && len <= sizeof(captured[0])) {
        memcpy(captured[captured_count], buf, len);
        captured_length[captured_count++] = len;
    }
}

static void carry_captured(struct tt_Context* to, uint32_t ip) {
    int count = captured_count;
    captured_count = 0;
    static uint8_t carried[4][tt_MAX_BUFFER_LENGTH * 2];
    static size_t carried_length[4];
    for (int k = 0; k < count; k++) {
        memcpy(carried[k], captured[k], captured_length[k]);
        carried_length[k] = captured_length[k];
    }
    for (int k = 0; k < count; k++) {
        memcpy(to->rx_buffer, carried[k], carried_length[k]);
        (void)process_packet(to, to->rx_buffer, 0, (uint32_t)carried_length[k], ip, PORT, tt_TRANSPORT_UDP);
    }
}

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_TX_BUFFER_LENGTH;
    context->last_modified = CLOCK_NS;
}

static void run_due(struct tt_Context* context) {
    bool has_next = false;
    uint64_t next = 0;
    while (run_due_entry(context, test_mock_now, &has_next, &next)) {
    }
}

// Answers the Server context holds. Nothing expires within this arm (CALLS x 10 ms against 5.1 s), so a set cache[]
// is a live answer here - counted that way so the arm also builds against the design before 2026-10-21.
static int kept_answers(void) {
    int kept = 0;
    for (int p = 0; p < PAIRS; p++) {
        for (int i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
            kept += servers[p].cache[i] != NULL ? 1 : 0;
        }
    }
    return kept;
}

static void test_many_pairs_calling_fast_keep_the_scheduler_bounded(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&server_context, SERVER_ID);
    init_context(&client_context, CLIENT_ID);
    memset(servers, 0, sizeof(servers));
    memset(clients, 0, sizeof(clients));
    memset(runs, 0, sizeof(runs));
    memset(answers, 0, sizeof(answers));
    wrong_answers = 0;
    server_service.call_retry_interval = RMW_INTERVAL;
    server_service.call_retry_count = RMW_COUNT;
    for (int p = 0; p < PAIRS; p++) {
        (void)snprintf(names[p], sizeof(names[p]), "lazy_expiry/echo_%d", p);
        EXPECT_EQ_INT(
            tt_RET_OK,
            tt_Node_create_server(tt_Context_default_node(&server_context), &servers[p], &server_service, names[p],
                                  NULL, &(struct tt_ServerOptions) {.callback = (tt_SERVER_CALLBACK)serve}, NULL, 0));
        EXPECT_EQ_INT(tt_RET_OK,
                      tt_Node_create_client(tt_Context_default_node(&client_context), &clients[p], &client_service,
                                            names[p], NULL,
                                            &(struct tt_ClientOptions) {.callback = (tt_CLIENT_CALLBACK)answered,
                                                                        .call_retry_interval = RMW_INTERVAL,
                                                                        .call_retry_count = RMW_COUNT},
                                            NULL, 0));
    }
    test_mock_send_hook = capture;
    run_due(&server_context);
    run_due(&client_context);
    int32_t idle_peak = server_context.sched_peak;

    for (int c = 0; c < CALLS; c++) {
        test_mock_now = CLOCK_NS + ((uint64_t)c * CALL_GAP);
        run_due(&server_context);
        run_due(&client_context);
        for (int p = 0; p < PAIRS; p++) {
            captured_count = 0;
            capturing = true;
            struct message request = {(uint32_t)((p * CALLS) + c)};
            EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&clients[p], (struct tt_Request*)&request));
            carry_captured(&server_context, CLIENT_IP); // the request; the answer is captured
            carry_captured(&client_context, SERVER_IP); // the answer
            capturing = false;
        }
    }

    int ran = 0;
    int got = 0;
    int most = 0;
    for (int p = 0; p < PAIRS; p++) {
        for (int c = 0; c < CALLS; c++) {
            ran += runs[p][c];
            got += answers[p][c];
            most = runs[p][c] > most ? runs[p][c] : most;
        }
    }
    int kept = kept_answers();
    printf("%d pairs x %d calls at 100 Hz, 5.1 s horizon: callbacks %d (most of one call %d), answered %d, kept %d "
           "(scheduler holds %d), server scheduler peak %d (idle %d), sched_full %lu\n",
           PAIRS, CALLS, ran, most, got, kept, tt_MAX_SCHEDULER_LENGTH, server_context.sched_peak, idle_peak,
           (unsigned long)server_context.sched_full);
    EXPECT_EQ_INT(PAIRS * KEPT_PER_PAIR, kept);  // the cache holds what the bound allows: the load is real
    EXPECT_TRUE(kept > tt_MAX_SCHEDULER_LENGTH); // more than one entry each could have had
    EXPECT_EQ_INT(PAIRS * CALLS, ran);           // every call served at once
    EXPECT_EQ_INT(1, most);                      // none twice
    EXPECT_EQ_INT(PAIRS * CALLS, got);           // and answered
    EXPECT_EQ_INT(0, wrong_answers);
    EXPECT_EQ_INT(0, (int)server_context.sched_full);                  // nothing refused
    EXPECT_TRUE(server_context.sched_peak <= idle_peak + SCHED_BOUND); // the scheduler did not grow with the answers
    test_mock_send_hook = NULL;
}

// --- Whitebox: one Server's cache, entries written as set_server_cache() is handed them ----------------------------

static struct tt_Service default_service; // zeroed: the default client schedule, a 75 ms seed lifetime

static void init_node_and_server(struct tt_Context* node, struct tt_Server* server) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    memset(server, 0, sizeof(*server));
    server->node = node;
    server->service = &default_service;
}

static struct tt_SubmessageHeader* write_fake_response(struct tt_Context* node, uint8_t receiver, uint16_t seq_no) {
    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)node->tx_buffer;
    submessage_header->type = tt_SUBMESSAGE_TYPE_CALLRESPONSE;
    submessage_header->receiver = receiver;
    submessage_header->length = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallResponseHeader);
    struct tt_CallResponseHeader* response_header =
        (struct tt_CallResponseHeader*)(node->tx_buffer + sizeof(struct tt_SubmessageHeader));
    response_header->endpoint_id = 0x1111;
    response_header->seq_no = seq_no;
    response_header->retry = 0;
    response_header->return_code = 0;
    node->tx_tail = submessage_header->length;
    return submessage_header;
}

static void keep(struct tt_Context* node, struct tt_Server* server, uint8_t receiver, uint16_t seq_no) {
    EXPECT_TRUE(set_server_cache(server, write_fake_response(node, receiver, seq_no), receiver, 0));
}

// Served until one lifetime after it went out, and not from then on; expired, the slot still names the call and when
// it went out, and holds nothing live.
static void test_an_answer_expires_one_lifetime_after_it_went_out(void) {
    struct tt_Context node;
    struct tt_Server server;
    init_node_and_server(&node, &server);
    uint64_t lifetime = server_cache_lifetime(&server);
    uint64_t t0 = test_mock_now;
    keep(&node, &server, 5, 1);
    int slot = find_server_cache_slot(&server, 5, 0, 1);
    EXPECT_TRUE(slot >= 0);
    test_mock_now = t0 + lifetime - 1;
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 1) != NULL);
    test_mock_now = t0 + lifetime;
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 1) == NULL);
    if (slot >= 0) {
        EXPECT_TRUE(server.cache[slot] == NULL); // released, as the expiry timer did
        EXPECT_EQ_U64(t0, server.cache_sent_at[slot]);
        EXPECT_TRUE(server_cache_slot_names(&server, slot, 5, 0, 1));
    }
}

// A retry served from the cache keeps the answer one lifetime from that retry.
static void test_a_hit_keeps_the_answer_from_the_hit(void) {
    struct tt_Context node;
    struct tt_Server server;
    init_node_and_server(&node, &server);
    uint64_t lifetime = server_cache_lifetime(&server);
    uint64_t t0 = test_mock_now;
    keep(&node, &server, 5, 1);
    test_mock_now = t0 + lifetime - 1;
    int slot = find_server_cache_slot(&server, 5, 0, 1);
    EXPECT_TRUE(slot >= 0);
    if (slot >= 0) {
        note_server_cache_hit(&server, slot);
    }
    test_mock_now = t0 + lifetime + 1; // past the first expiry, inside the one from the hit
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 1) != NULL);
}

// An expired slot is reused before a live answer is evicted. The lifetime is long for X's answer and short for the
// rest (the learnt retry gap dropped in between), so X's, sent first, is the one still live when the others expire.
// With every slot then holding something, the new answer must take an expired slot and not evict X.
static void test_an_expired_slot_is_reused_before_a_live_answer_goes(void) {
    struct tt_Context node;
    struct tt_Server server;
    init_node_and_server(&node, &server);
    uint64_t t0 = test_mock_now;
    server.client_retry_gap = tt_SECOND; // X kept tt_SERVER_CACHE_GAP_MULTIPLE s
    keep(&node, &server, 5, 1);
    server.client_retry_gap = 0; // the rest kept the 75 ms seed
    uint64_t seed = server_cache_lifetime(&server);
    test_mock_now = t0 + tt_MILLISECOND;
    for (int i = 1; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        keep(&node, &server, (uint8_t)(10 + i), 1);
    }
    test_mock_now = t0 + tt_MILLISECOND + seed; // the rest expired, X live
    keep(&node, &server, 200, 1);
    EXPECT_TRUE(get_server_cache(&server, 200, 0, 1) != NULL);
    EXPECT_TRUE(get_server_cache(&server, 5, 0, 1) != NULL); // X stays
}

int main(void) {
    test_many_pairs_calling_fast_keep_the_scheduler_bounded();
    test_an_answer_expires_one_lifetime_after_it_went_out();
    test_a_hit_keeps_the_answer_from_the_hit();
    test_an_expired_slot_is_reused_before_a_live_answer_goes();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_kept_answer_lazy_expiry: all tests passed (%d pairs, tt_CLIENT_MAX_OUTSTANDING %d)\n", PAIRS,
           tt_CLIENT_MAX_OUTSTANDING);
    return 0;
}
