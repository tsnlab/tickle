/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A BEST_EFFORT service Client sends each request once (decision D6, QoS redesign step 7).
//
// What must hold, decided before the code: over a lossy simulated link (10% of every datagram, both ways), a
// BEST_EFFORT Client puts exactly one CallRequest on the wire per call, every one of them a first send (retry 0); the
// calls whose request or answer the link lost stay lost and end as tt_CALL_TIMEOUT, no earlier than a RELIABLE call of
// the same settings would have given up; and every other call is answered. The control is the same calls from a
// RELIABLE Client over the same link: it must send more than one CallRequest per call (it retried) and have every call
// answered - or the link lost nothing that mattered, and the BEST_EFFORT run showed nothing. A second check: an answer
// later than the first retry interval, but within that span, still reaches a BEST_EFFORT call, with one send.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define CLIENT_ID 1
#define SERVER_ID 2
#define CLIENT_IP 0x0a000001
#define SERVER_IP 0x0a000002
#define PORT 8282
#define CALLS 200
#define LOSS_PER_MILLE 100U

static int32_t four(void) {
    return 4;
}

static int32_t request_size(struct tt_Request* request) {
    (void)request;
    return four();
}

static int32_t request_encode(struct tt_Request* request, uint8_t* payload, uint32_t len) {
    (void)request;
    memset(payload, 0, len);
    return four();
}

static int32_t request_decode(struct tt_Request* request, const uint8_t* payload, uint32_t len, bool native) {
    (void)request;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static int32_t response_size(struct tt_Response* response) {
    (void)response;
    return four();
}

static int32_t response_encode(struct tt_Response* response, uint8_t* payload, uint32_t len) {
    (void)response;
    memset(payload, 0, len);
    return four();
}

static int32_t response_decode(struct tt_Response* response, const uint8_t* payload, uint32_t len, bool native) {
    (void)response;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static void request_free(struct tt_Request* request) {
    (void)request;
}

static void response_free(struct tt_Response* response) {
    (void)response;
}

static struct tt_Context client_ctx;
static struct tt_Context server_ctx;
static struct tt_Service service;
static struct tt_Client client;
static struct tt_Server server;

// What the Client's callback saw for the call in flight.
static int outcome; // 0: none yet, 1: answered, -1: tt_CALL_TIMEOUT
static uint64_t outcome_at;
static int executions; // the server's callback runs
static bool defer_answers;
static tt_RequestId deferred_id;

static void on_response(struct tt_Client* c, int8_t code, struct tt_Response* response) {
    (void)c;
    (void)response;
    outcome = code == tt_CALL_TIMEOUT ? -1 : 1;
    outcome_at = test_mock_now;
}

static int8_t on_request(struct tt_Server* s, struct tt_Request* request, struct tt_Response* response,
                         tt_RequestId id) {
    (void)s;
    (void)request;
    (void)response;
    executions++;
    if (defer_answers) {
        deferred_id = id;
        return tt_CALL_DEFERRED;
    }
    return 0;
}

// --- the lossy link, as test_qos_create.c builds it ------------------------------------------------------------------

#define SIM_QUEUE 1024
struct sim_datagram {
    uint8_t from;
    uint32_t len;
    uint8_t bytes[tt_MAX_BUFFER_LENGTH];
};
static struct sim_datagram sim_queue[SIM_QUEUE];
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;
static uint32_t sim_rng;
static uint32_t sim_loss_per_mille;
static uint32_t sim_lost_count;
static uint32_t requests_sent;   // CallRequest submessages the Client put on the wire
static uint32_t requests_resent; // ... of which a retry (tt_CallRequestHeader.retry != 0)

static void count_request(const uint8_t* call_header) {
    const struct tt_CallRequestHeader* call = (const struct tt_CallRequestHeader*)(const void*)call_header;
    requests_sent++;
    requests_resent += call->retry != 0 ? 1U : 0U;
}

// Counts the CallRequests in a datagram the Client sends: one in the single-submessage framing (struct
// tt_SingleHeader), or each of the submessages after a tt_Header, walked by their length.
static void count_requests(const uint8_t* buf, size_t len) {
    if (len >= sizeof(struct tt_SingleHeader) && (buf[0] == tt_SINGLE_MARKER_LE || buf[0] == tt_SINGLE_MARKER_BE)) {
        const struct tt_SingleHeader* single = (const struct tt_SingleHeader*)(const void*)buf;
        if (single->type == tt_SUBMESSAGE_TYPE_CALLREQUEST) {
            count_request(buf + sizeof(struct tt_SingleHeader));
        }
        return;
    }
    size_t at = sizeof(struct tt_Header);
    while (at + sizeof(struct tt_SubmessageHeader) <= len) {
        const struct tt_SubmessageHeader* sub = (const struct tt_SubmessageHeader*)(const void*)(buf + at);
        if (sub->length < sizeof(struct tt_SubmessageHeader) || at + sub->length > len) {
            break;
        }
        if (sub->type == tt_SUBMESSAGE_TYPE_CALLREQUEST) {
            count_request(buf + at + sizeof(struct tt_SubmessageHeader));
        }
        at += sub->length;
    }
}

static void sim_capture(const void* buf, size_t len) {
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    if (sim_acting == CLIENT_ID) {
        count_requests((const uint8_t*)buf, len);
    }
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
}

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        sim_rng = (sim_rng * 1103515245U) + 12345U;
        if (((sim_rng >> 16) % 1000U) < sim_loss_per_mille) {
            sim_lost_count++;
            continue;
        }
        struct tt_Context* to = d->from == CLIENT_ID ? &server_ctx : &client_ctx;
        sim_acting = to->id;
        process_packet(to, d->bytes, 0, d->len, d->from == CLIENT_ID ? CLIENT_IP : SERVER_IP, PORT, tt_TRANSPORT_UDP);
    }
}

static void sim_run_due(struct tt_Context* node) {
    bool has_next = false;
    uint64_t next = 0;
    sim_acting = node->id;
    while (run_due_entry(node, test_mock_now, &has_next, &next)) {
    }
    sim_drain();
}

// Moves the clock to the next scheduled entry of either context and runs what is due. False when nothing is scheduled.
static bool sim_step(void) {
    uint64_t a = UINT64_MAX;
    uint64_t b = UINT64_MAX;
    if (!sched_next_time(&client_ctx, &a)) {
        a = UINT64_MAX;
    }
    if (!sched_next_time(&server_ctx, &b)) {
        b = UINT64_MAX;
    }
    uint64_t next = a < b ? a : b;
    if (next == UINT64_MAX) {
        return false;
    }
    if (next > test_mock_now) {
        test_mock_now = next;
    }
    sim_run_due(&client_ctx);
    sim_run_due(&server_ctx);
    return true;
}

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

static void init(const struct tt_QosProfile* client_qos) {
    test_mock_reset();
    init_bare_node(&client_ctx, CLIENT_ID);
    init_bare_node(&server_ctx, SERVER_ID);
    memset(&service, 0, sizeof(service));
    service.name = "be_service";
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
    struct tt_ClientOptions copts = {.callback = on_response}; // 0, 0: the auto retry, the default count
    struct tt_ServerOptions sopts = {.callback = on_request};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(tt_Context_default_node(&client_ctx), &client, &service, "svc",
                                                   client_qos, &copts, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_server(tt_Context_default_node(&server_ctx), &server, &service, "svc", NULL,
                                                   &sopts, NULL, 0));
    sim_head = sim_tail = 0;
    sim_rng = 11;
    sim_lost_count = 0;
    requests_sent = requests_resent = 0;
    executions = 0;
    defer_answers = false;
    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
}

struct run_result {
    int answered;
    int timed_out;
    int early_timeouts;  // timeouts reported before the span a RELIABLE call of these settings retries for
    int short_timeouts;  // timeouts of a call sent fewer times than its settings allow (count + 1)
    uint32_t most_sends; // the most CallRequests any one call sent
};

// CALLS calls, one at a time, each run until its callback reports it.
static struct run_result run_calls(const struct tt_QosProfile* client_qos, uint32_t loss_per_mille) {
    init(client_qos);
    sim_loss_per_mille = loss_per_mille;
    struct run_result r = {0, 0, 0, 0, 0};
    for (int i = 0; i < CALLS; i++) {
        outcome = 0;
        uint64_t called_at = test_mock_now;
        uint64_t span = call_give_up_span(&client); // the auto path's deadline: read before the call, as it is armed
        uint32_t sends = call_retry_count(&client) + 1U; // what a RELIABLE call may send
        uint32_t sent_before = requests_sent;
        sim_acting = CLIENT_ID;
        EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&client, (struct tt_Request*)&r));
        sim_drain();
        while (outcome == 0 && sim_step()) {
        }
        uint32_t this_call = requests_sent - sent_before;
        r.most_sends = this_call > r.most_sends ? this_call : r.most_sends;
        EXPECT_TRUE(outcome != 0); // every call ends, answered or not
        if (outcome == 1) {
            r.answered++;
        } else if (outcome == -1) {
            r.timed_out++;
            r.early_timeouts += outcome_at - called_at < span ? 1 : 0;
            r.short_timeouts += this_call < sends ? 1 : 0;
        }
        test_mock_now += 10 * tt_MILLISECOND;
    }
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_destroy(&client));
    EXPECT_EQ_INT(tt_RET_OK, tt_Server_destroy(&server));
    return r;
}

static void test_best_effort_sends_once_and_loses_what_the_link_lost(void) {
    const struct tt_QosProfile best_effort = {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}};
    struct run_result be = run_calls(&best_effort, LOSS_PER_MILLE);
    uint32_t be_sent = requests_sent;
    uint32_t be_resent = requests_resent;
    printf("test_service_best_effort: BEST_EFFORT: %d answered, %d timed out (%d early), %u requests sent "
           "(%u retries), %d executions, %u datagrams lost\n",
           be.answered, be.timed_out, be.early_timeouts, be_sent, be_resent, executions, sim_lost_count);
    EXPECT_EQ_U32(CALLS, be_sent); // exactly one send per call
    EXPECT_EQ_U32(0, be_resent);   // none of them a retry
    EXPECT_TRUE(be.timed_out > 0); // the link lost calls, and they stay lost
    EXPECT_EQ_INT(CALLS, be.answered + be.timed_out);
    EXPECT_EQ_INT(0, be.early_timeouts); // a lost call ends when a RELIABLE one would have given up, not before
    // Every request that arrived ran once: a call is answered only if its request arrived and its answer did too.
    EXPECT_TRUE(executions >= be.answered && executions <= CALLS);

    EXPECT_EQ_U32(1, be.most_sends);

    // The control: RELIABLE over the same link retries, and loses only a call every one of whose sends it retried to
    // the limit went unanswered - (1 - 0.9 x 0.9)^4, ~0.1% a call, and the draw is fixed: one in this run.
    struct run_result rel = run_calls(NULL, LOSS_PER_MILLE); // NULL: the services default, RELIABLE
    printf("test_service_best_effort: control, RELIABLE: %d answered, %d timed out (%d before every retry), "
           "%u requests sent (%u retries, at most %u for one call), %u datagrams lost\n",
           rel.answered, rel.timed_out, rel.short_timeouts, requests_sent, requests_resent, rel.most_sends,
           sim_lost_count);
    EXPECT_TRUE(requests_resent > 0 && requests_sent > CALLS);
    EXPECT_EQ_INT(0, rel.short_timeouts);
    EXPECT_TRUE(rel.timed_out * 10 < be.timed_out); // retrying recovers nearly every call BEST_EFFORT lost
}

// The link loses nothing: a BEST_EFFORT client is answered every time, with one send each - BEST_EFFORT loses only
// what the link loses.
static void test_best_effort_on_a_clean_link_loses_nothing(void) {
    const struct tt_QosProfile best_effort = {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}};
    struct run_result be = run_calls(&best_effort, 0);
    EXPECT_EQ_INT(CALLS, be.answered);
    EXPECT_EQ_U32(CALLS, requests_sent);
    EXPECT_EQ_U32(0, requests_resent);
}

// An answer later than the call's first retry interval, but within the span, still reaches a BEST_EFFORT call, and
// the request was sent once. The server defers it (tt_CALL_DEFERRED) and answers 300 ms later.
static void test_a_late_answer_still_reaches_a_best_effort_call(void) {
    const struct tt_QosProfile best_effort = {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}};
    init(&best_effort);
    sim_loss_per_mille = 0;
    defer_answers = true;
    outcome = 0;
    int dummy = 0;
    uint64_t first_wait = compute_retry_interval(&client, 0);
    sim_acting = CLIENT_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&client, (struct tt_Request*)&dummy));
    sim_drain();
    EXPECT_EQ_INT(1, executions);
    uint64_t answer_at = test_mock_now + (300 * tt_MILLISECOND);
    EXPECT_TRUE(answer_at - test_mock_now > first_wait); // later than the first retry interval
    int wakes = 0; // the call's timer runs before the answer: its first wake, at the first retry interval
    uint64_t next = 0;
    while (outcome == 0 && sched_next_time(&client_ctx, &next) && next < answer_at && sim_step()) {
        wakes++;
    }
    EXPECT_TRUE(wakes >= 1);
    EXPECT_EQ_INT(0, outcome);       // still waiting, not timed out
    EXPECT_EQ_U32(1, requests_sent); // ... and not resent
    test_mock_now = answer_at;
    sim_acting = SERVER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Server_send_response(&server, deferred_id, 0, (struct tt_Response*)&dummy));
    sim_drain();
    EXPECT_EQ_INT(1, outcome);
    EXPECT_EQ_U32(1, requests_sent);
    EXPECT_EQ_INT(1, executions);
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_destroy(&client));
    EXPECT_EQ_INT(tt_RET_OK, tt_Server_destroy(&server));
}

int main(void) {
    test_best_effort_sends_once_and_loses_what_the_link_lost();
    test_best_effort_on_a_clean_link_loses_nothing();
    test_a_late_answer_still_reaches_a_best_effort_call();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_service_best_effort: all tests passed\n");
    return 0;
}
