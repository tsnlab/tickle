/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A call runs its service callback once while the client may still retry it, on core's auto schedule (2026-10-21).
//
// The rig's service cell c21 (1 kHz calls, 5% loss each way; examples/perf_hil/tickle/service_call/) ran a callback
// twice for one call in most runs, with both sides on the auto schedule (tt_Service call_retry_* = 0). Instrumented on
// the PC (two netns over a veth), the duplicates were retries that reached the server more than 75 ms after it sent the
// answer: the server kept an answer for the client's SEED schedule (5 + 10 + 20 + 40 ms), while the client's first
// wait had grown to 290 ms within ~120 calls. It grew because every answer fed the client's srtt/rttvar, an answer
// that came only after a resend included: such a sample is the lost send's wait plus the round trip, so each loss
// raised the next first wait (srtt + 4 rttvar), which made the next loss's sample larger still - Karn's ambiguity,
// unchecked.
//
// Two defects, each with its own arms here:
//   horizon  the server keeps an answer for less than the client's auto horizon. The client sends nothing after its
//            deadline, (count + 1) x tt_CALL_DEADLINE_PER_SEND (call_deadline()), but its retries before that can be
//            later than any seed schedule: its waits are srtt-relative, and srtt is measured.
//   karn     the client's estimate takes samples from calls it resent, so loss inflates its waits (and with them how
//            late a retry comes, and how few calls a second it makes: the cell's 130 calls/s collapse).
//
// The network is simulated: every datagram a context sends goes on a wire with a one-way delay, or is dropped by the
// arm's script (by call and send), and the two contexts' timers and the wire are run in time order on the mock clock.
// The server's callback takes the arm's service time on that clock. Counted per call: callback runs, answers.
//
// Arms, each reading written into its expectation:
//   H0 control   service 10 ms, a lost answer: the retry comes 20 ms after the answer, inside the seed schedule - one
//                run before and after the fix. Each H arm first calls until a call is answered without a resend: the
//                client's first sample (K arms), srtt = the service time and rttvar half of it. Shows a retry the
//                server still holds the answer for counts as no re-run.
//   H1 defect    service 100 ms (srtt 100 ms, rttvar 50 ms: first wait 300 ms), a lost answer: the retry comes ~200
//                ms after the answer. Before the fix two runs; the reading after: one.
//   H2 edge      as H1, with the first retry lost too: the second, ~900 ms after the call - the last send before the
//                1 s deadline - is still served from the kept answer. One run.
//   H3 margin    as H2, the second retry 250 ms longer on the wire: it reaches the server ~1.05 s after the answer,
//                past the deadline itself (the lifetime keeps one tt_CALL_DEADLINE_PER_SEND beyond it). One run; the
//                client has given up by then, so no answer and one timeout.
//   K1 wait      a 200 us round trip, alternately a clean call and a call whose first send is lost. Reading: the
//                first wait after it is at most twice the clean first wait (762 ms against 0.4 ms before the fix).
//   K2 c21       K1's history, then a call whose answer is lost: one run (two before either fix).
//   K3 jump      the round trip jumps from 200 us to 5 ms. Samples from resent calls are no longer taken, so the
//                estimate could only learn the new round trip from a clean call, and with every first wait too short
//                there would be none: the backed-off wait is kept for the next call (RFC 6298 5.5-5.7), so by the
//                fourth call after the jump calls go clean. Reading: no resend in calls 4..8 after the jump.

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
#define WIRE_MAX 64
#define CALLS_MAX 256
#define ANSWER_OFFSET 1000U
#define US 1000ULL
#define MS 1000000ULL

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

// One definition for both sides, retry settings 0: core's auto schedule, as the rig's bench (BenchService.h).
static struct tt_Service service = {
    .name = "exactly_once::srv::dds_::Echo_",
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
    .call_retry_interval = 0,
    .call_retry_count = 0,
};

static struct tt_Context server_context;
static struct tt_Context client_context;
static struct tt_Server server;
static struct tt_Client client;

// Per call, indexed by the call's tag (the request's own id, as the bench's k).
static int runs[CALLS_MAX];
static int answers[CALLS_MAX];
static int timeouts;
static int wrong_answers;
static uint32_t request_sends[CALLS_MAX];     // CallRequests the client put on the wire for the call
static uint8_t drop_request_mask[CALLS_MAX];  // bit r: the send with retry r is lost
static uint8_t drop_response_mask[CALLS_MAX]; // bit n: the call's n-th CallResponse is lost
static uint32_t response_sends[CALLS_MAX];
static uint16_t tag_of_seq[65536];
static uint8_t slow_request_retry[CALLS_MAX]; // the call's send with this retry (0: none) is slow_request_ns slower
static uint64_t slow_request_ns;
static uint64_t extra_delay; // what lost() says the datagram it judged takes beyond one_way_ns
static uint64_t service_ns;  // the callback's run time on the mock clock
static uint64_t one_way_ns;  // the wire's delay

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    (void)request_id;
    if (request->tag < CALLS_MAX) {
        runs[request->tag]++;
    }
    test_mock_now += service_ns;
    response->tag = request->tag + ANSWER_OFFSET;
    return 0;
}

static void answered(struct tt_Client* answering, int8_t return_code, struct message* response) {
    if (return_code == tt_CALL_TIMEOUT) {
        timeouts++;
        return;
    }
    if (answering != &client || return_code != 0 || response == NULL || response->tag < ANSWER_OFFSET ||
        response->tag - ANSWER_OFFSET >= CALLS_MAX) {
        wrong_answers++;
        return;
    }
    answers[response->tag - ANSWER_OFFSET]++;
}

// The wire: what is in flight, to whom, arriving when.
struct datagram {
    uint64_t at;
    struct tt_Context* to;
    uint32_t from_ip;
    size_t length;
    uint8_t data[tt_MAX_BUFFER_LENGTH * 2];
};
static struct datagram wire[WIRE_MAX];
static int wire_count;
static struct tt_Context* sending; // the context whose code is running, so whose send the hook sees

// Whether the datagram is lost, by the arm's script: a CallRequest by its call and retry, a CallResponse by its call
// and how many went before it. Anything else goes through.
static bool lost(const uint8_t* wire_form, size_t wire_len) {
    // In the classic form (the mock rewrites a single-submessage datagram into it), the call's submessage: the first
    // CallRequest or CallResponse of the datagram (one per datagram here).
    static uint8_t buf[((size_t)tt_MAX_BUFFER_LENGTH * 2) + sizeof(struct tt_Header)];
    if (wire_len == 0 || wire_len >= (size_t)tt_MAX_BUFFER_LENGTH * 2) {
        return false;
    }
    size_t len = test_classic_form(wire_form, wire_len, buf);
    const struct tt_SubmessageHeader* sub = NULL;
    for (size_t at = sizeof(struct tt_Header); at + sizeof(struct tt_SubmessageHeader) <= len;) {
        const struct tt_SubmessageHeader* here = (const void*)(buf + at);
        if (here->type == tt_SUBMESSAGE_TYPE_CALLREQUEST || here->type == tt_SUBMESSAGE_TYPE_CALLRESPONSE) {
            sub = here;
            break;
        }
        if (here->length == 0) {
            break;
        }
        at += here->length;
    }
    if (sub == NULL) {
        return false;
    }
    const uint8_t* body = (const uint8_t*)sub + sizeof(struct tt_SubmessageHeader);
    if (sub->type == tt_SUBMESSAGE_TYPE_CALLREQUEST) {
        const struct tt_CallRequestHeader* req = (const void*)body;
        uint16_t tag = tag_of_seq[req->seq_no];
        request_sends[tag]++;
        extra_delay = req->retry != 0 && req->retry == slow_request_retry[tag] ? slow_request_ns : 0;
        return req->retry < 8 && (drop_request_mask[tag] & (1U << req->retry)) != 0;
    }
    if (sub->type == tt_SUBMESSAGE_TYPE_CALLRESPONSE) {
        const struct tt_CallResponseHeader* res = (const void*)body;
        uint16_t tag = tag_of_seq[res->seq_no];
        uint32_t nth = response_sends[tag]++;
        return nth < 8 && (drop_response_mask[tag] & (1U << nth)) != 0;
    }
    return false;
}

static void on_send(const void* buf, size_t len) {
    EXPECT_TRUE(sending != NULL);
    extra_delay = 0;
    if (sending == NULL || lost(buf, len)) {
        return;
    }
    EXPECT_TRUE(wire_count < WIRE_MAX && len <= sizeof(wire[0].data));
    if (wire_count >= WIRE_MAX || len > sizeof(wire[0].data)) {
        return;
    }
    struct datagram* d = &wire[wire_count++];
    d->at = test_mock_now + one_way_ns + extra_delay;
    d->to = sending == &client_context ? &server_context : &client_context;
    d->from_ip = sending == &client_context ? CLIENT_IP : SERVER_IP;
    d->length = len;
    memcpy(d->data, buf, len);
}

static uint64_t next_timer(const struct tt_Context* context) {
    uint64_t at = UINT64_MAX;
    for (int32_t k = 0; k < context->scheduler_tail; k++) {
        at = context->scheduler[k].time < at ? context->scheduler[k].time : at;
    }
    return at;
}

static void run_due(struct tt_Context* context) {
    bool has_next = false;
    uint64_t next = 0;
    sending = context;
    while (run_due_entry(context, test_mock_now, &has_next, &next)) {
    }
    sending = NULL;
}

// Runs the wire and both contexts' timers, in time order, up to `until`.
static void run_until(uint64_t until) {
    for (;;) {
        int first = -1;
        for (int i = 0; i < wire_count; i++) {
            if (first < 0 || wire[i].at < wire[first].at) {
                first = i;
            }
        }
        uint64_t wire_at = first >= 0 ? wire[first].at : UINT64_MAX;
        uint64_t client_at = next_timer(&client_context);
        uint64_t server_at = next_timer(&server_context);
        uint64_t at = wire_at < client_at ? wire_at : client_at;
        at = server_at < at ? server_at : at;
        if (at > until) {
            break;
        }
        test_mock_now = at > test_mock_now ? at : test_mock_now;
        if (at == wire_at) {
            struct datagram d = wire[first];
            wire[first] = wire[--wire_count];
            memcpy(d.to->rx_buffer, d.data, d.length);
            sending = d.to;
            (void)process_packet(d.to, d.to->rx_buffer, 0, (uint32_t)d.length, d.from_ip, PORT, tt_TRANSPORT_UDP);
            sending = NULL;
        } else {
            run_due(at == client_at ? &client_context : &server_context);
        }
    }
    test_mock_now = until > test_mock_now ? until : test_mock_now;
}

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_TX_BUFFER_LENGTH;
    context->last_modified = CLOCK_NS;
}

static void setup(uint64_t service_time, uint64_t one_way) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&server_context, SERVER_ID);
    init_context(&client_context, CLIENT_ID);
    memset(&server, 0, sizeof(server));
    memset(&client, 0, sizeof(client));
    memset(runs, 0, sizeof(runs));
    memset(answers, 0, sizeof(answers));
    memset(request_sends, 0, sizeof(request_sends));
    memset(response_sends, 0, sizeof(response_sends));
    memset(drop_request_mask, 0, sizeof(drop_request_mask));
    memset(drop_response_mask, 0, sizeof(drop_response_mask));
    memset(slow_request_retry, 0, sizeof(slow_request_retry));
    slow_request_ns = 0;
    timeouts = 0;
    wrong_answers = 0;
    wire_count = 0;
    service_ns = service_time;
    one_way_ns = one_way;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_server(
                                 tt_Context_default_node(&server_context), &server, &service, "exactly_once/echo", NULL,
                                 &(struct tt_ServerOptions) {.callback = (tt_SERVER_CALLBACK)serve}, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(
                                 tt_Context_default_node(&client_context), &client, &service, "exactly_once/echo", NULL,
                                 &(struct tt_ClientOptions) {.callback = (tt_CLIENT_CALLBACK)answered}, NULL, 0));
    test_mock_send_hook = on_send;
}

// Makes call `tag` and runs until it is answered or has given up (a call ends within its 1 s deadline), then 2 s
// more, so the server has seen every retry of it that was sent.
static void call(uint16_t tag) {
    struct message request = {tag};
    uint16_t seq_no = 0;
    tag_of_seq[client_context.call_seq_no] = tag; // the seq_no the call takes, so its first send is judged as its own
    sending = &client_context;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call_id(&client, (struct tt_Request*)&request, &seq_no));
    sending = NULL;
    EXPECT_EQ_INT(tag, tag_of_seq[seq_no]);
    run_until(test_mock_now + (3 * tt_SECOND));
}

static uint64_t first_wait(void) {
    return compute_retry_interval(&client, 0);
}

// H arms: calls until one is answered without a resend - the client's first sample, srtt = the service time, rttvar
// half of it - then the subject call, with its first answer lost, the request sends in `lost_requests` lost, and the
// send with retry `slow_retry` (0: none) `slow_ns` longer on the wire. One answer expected unless the call times out.
#define SUBJECT 20
static void test_horizon(const char* name, uint64_t service_time, uint8_t lost_requests, uint8_t slow_retry,
                         uint64_t slow_ns, int expected_runs, int expected_timeouts) {
    setup(service_time, 100 * US);
    uint16_t warm = 1;
    for (; warm < SUBJECT; warm++) {
        call(warm);
        EXPECT_EQ_INT(1, runs[warm]);
        EXPECT_EQ_INT(1, answers[warm]);
        if (request_sends[warm] == 1) {
            break; // never resent: a sample (Karn)
        }
    }
    EXPECT_TRUE(warm < SUBJECT);
    drop_response_mask[SUBJECT] = 1U;
    drop_request_mask[SUBJECT] = lost_requests;
    slow_request_retry[SUBJECT] = slow_retry;
    slow_request_ns = slow_ns;
    uint64_t subject_wait = first_wait();
    call(SUBJECT);
    printf("  %-44s %u warm-up, first wait %5.1f ms, runs %d (expected %d), answers %d, sends %u\n", name,
           (unsigned)warm, (double)subject_wait / MS, runs[SUBJECT], expected_runs, answers[SUBJECT],
           request_sends[SUBJECT]);
    EXPECT_EQ_INT(expected_runs, runs[SUBJECT]);
    EXPECT_EQ_INT(1 - expected_timeouts, answers[SUBJECT]);
    EXPECT_EQ_INT(expected_timeouts, timeouts);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

// K1/K2's history: CLEAN clean calls, then PAIRS pairs of (first send lost, clean). Returns the clean first wait.
#define CLEAN 16
#define PAIRS 16
static uint16_t lossy_history(uint64_t* clean_wait) {
    setup(0, 100 * US);
    uint16_t tag = 1;
    for (int i = 0; i < CLEAN; i++) {
        call(tag++);
    }
    *clean_wait = first_wait();
    for (int i = 0; i < PAIRS; i++) {
        drop_request_mask[tag] = 1U;
        call(tag++);
        call(tag++);
    }
    for (uint16_t k = 1; k < tag; k++) {
        EXPECT_EQ_INT(1, runs[k]);
        EXPECT_EQ_INT(1, answers[k]);
    }
    return tag;
}

static void test_karn_wait(void) {
    uint64_t clean_wait = 0;
    (void)lossy_history(&clean_wait);
    uint64_t wait = first_wait();
    printf("  %-44s clean first wait %.3f ms, after the losses %.3f ms (expected <= %.3f)\n", "K1 wait",
           (double)clean_wait / MS, (double)wait / MS, (double)(2 * clean_wait) / MS);
    EXPECT_TRUE(wait <= 2 * clean_wait);
    EXPECT_EQ_INT(0, timeouts);
    test_mock_send_hook = NULL;
}

static void test_karn_c21(void) {
    uint64_t clean_wait = 0;
    uint16_t tag = lossy_history(&clean_wait);
    drop_response_mask[tag] = 1U;
    call(tag);
    printf("  %-44s first wait %.3f ms, runs %d (expected 1), answers %d\n", "K2 c21: lossy history, answer lost",
           (double)first_wait() / MS, runs[tag], answers[tag]);
    EXPECT_EQ_INT(1, runs[tag]);
    EXPECT_EQ_INT(1, answers[tag]);
    EXPECT_EQ_INT(0, timeouts);
    test_mock_send_hook = NULL;
}

static void test_karn_jump(void) {
    setup(0, 100 * US);
    uint16_t tag = 1;
    for (int i = 0; i < CLEAN; i++) {
        call(tag++);
    }
    one_way_ns = 2500 * US;
    uint16_t jump = tag;
    for (int i = 0; i < 8; i++) {
        call(tag++);
    }
    uint32_t late_resends = 0;
    for (uint16_t k = jump; k < tag; k++) {
        EXPECT_EQ_INT(1, runs[k]);
        EXPECT_EQ_INT(1, answers[k]);
        late_resends += k >= jump + 3 ? request_sends[k] - 1 : 0;
    }
    printf("  %-44s sends per call after the jump:", "K3 jump 200 us -> 5 ms");
    for (uint16_t k = jump; k < tag; k++) {
        printf(" %u", request_sends[k]);
    }
    printf(" (expected no resend from the 4th)\n");
    EXPECT_EQ_U32(0, late_resends);
    EXPECT_EQ_INT(0, timeouts);
    test_mock_send_hook = NULL;
}

int main(void) {
    test_horizon("H0 control: service 10 ms, answer lost", 10 * MS, 0, 0, 0, 1, 0);
    test_horizon("H1 defect: service 100 ms, answer lost", 100 * MS, 0, 0, 0, 1, 0);
    test_horizon("H2 edge: + first retry lost", 100 * MS, 1U << 1U, 0, 0, 1, 0);
    test_horizon("H3 margin: + last retry 250 ms on the wire", 100 * MS, 1U << 1U, 2, 250 * MS, 1, 1);
    test_karn_wait();
    test_karn_c21();
    test_karn_jump();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_service_exactly_once: all tests passed\n");
    return 0;
}
