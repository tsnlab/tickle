/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The FreeRTOS service examples' small caches (2026-10-22) still run each call once, and the limit their size sets is
// where the arithmetic puts it.
//
// Built as platform/freertos/Makefile builds ROLE=server/pong and client/ping: tt_MAX_SERVER_CACHE_COUNT 8 and the
// inline caches cut to 8 bytes an entry, the server's kept answers and the client's kept requests in caller storage
// sized for a 12-byte message (as PingPong's; SetBool's answer is 12 bytes padded, its request 1). An answer is kept
// L = 1.25 s on the auto path ((tt_CALL_RETRY_COUNT 3 + 1) x tt_CALL_DEADLINE_PER_SEND 250 ms + one more), and a server
// keeps at most tt_CLIENT_CALL_WINDOW (8) per Client. So 8 entries hold everything one Client may retry at any rate,
// and Clients that each hold fewer share 8 / 1.25 s = 6.4 calls/s; past that a request is turned away unexecuted (its
// retry asks again), never run twice.
//
// The network is test_service_exactly_once.c's: every datagram goes on a wire with a 100 us one-way delay, or is
// dropped by the arm's script (a call's n-th answer), and the contexts' timers and the wire run in time order on the
// mock clock. A request sent more than once with no request or answer lost is a request the server turned away.
//
// Arms, each reading written into its expectation:
//   E1 example   one Client calling every 500 ms (the examples' rate), every call's first answer lost: each call runs
//                once and is answered once from the kept answer, with exactly 2 sends (none turned away).
//   E2 burst     one Client calling every 5 ms (200 calls/s, 31 x the shared capacity), every third call's first answer
//                lost: still each call once, none turned away - the per-Client bound, not the rate, sizes the cache
//                for one Client.
//   C0 at cap    3 Clients every 500 ms, staggered: 6 calls/s, under 6.4 - at most 8 answers live at once. None
//                turned away, each call once.
//   C1 past cap  4 Clients every 500 ms: 8 calls/s, over 6.4 - 10 answers would be live. The control: requests ARE
//                turned away (the limit is real), and still no call runs twice (each answered call ran exactly once).
//   D  too small the server's entries sized for a 4-byte answer (16 bytes) against the 24 the answer takes: E1's
//                script then runs a call twice - the duplicate the kept answer exists to stop, which shows this
//                harness sees one.

#define tt_MAX_SERVER_CACHE_COUNT 8
#define tt_SERVER_CACHE_ENTRY_LENGTH 8
#define tt_CLIENT_CACHE_LENGTH 8

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
#define CALLS_MAX 4096
#define CLIENTS_MAX 4
#define ANSWER_OFFSET 10000U
#define US 1000ULL
#define MS 1000000ULL
#define MESSAGE_BYTES 12U // tag + 8 bytes, PingPong's size

struct message {
    uint32_t tag;
};

static int32_t message_size(struct message* msg) {
    (void)msg;
    return (int32_t)MESSAGE_BYTES;
}
static int32_t message_encode(struct message* msg, uint8_t* payload, const uint32_t len) {
    if (len < MESSAGE_BYTES) {
        return -1;
    }
    memset(payload, 0, MESSAGE_BYTES);
    memcpy(payload, &msg->tag, sizeof(uint32_t));
    return (int32_t)MESSAGE_BYTES;
}
static int32_t message_decode(struct message* msg, const uint8_t* payload, const uint32_t len, bool native) {
    (void)native;
    if (len < MESSAGE_BYTES) {
        return -1;
    }
    memcpy(&msg->tag, payload, sizeof(uint32_t));
    return (int32_t)MESSAGE_BYTES;
}
static void message_free(struct message* msg) {
    (void)msg;
}

// Retry settings 0: core's auto schedule, as PingPongService and SetBoolService.
static struct tt_Service service = {
    .name = "small_cache::srv::dds_::Echo_",
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
static struct tt_Client clients[CLIENTS_MAX];
static uint64_t server_storage[256];
static uint64_t client_storage[CLIENTS_MAX][64];

static int runs[CALLS_MAX];
static int answers[CALLS_MAX];
static uint32_t request_sends[CALLS_MAX];
static uint32_t response_sends[CALLS_MAX];
static uint8_t drop_response_mask[CALLS_MAX]; // bit n: the call's n-th CallResponse is lost
static uint16_t tag_of_seq[65536];
static int timeouts;
static int wrong_answers;
static uint32_t refused_calls; // tt_Client_call() said no (the Client's own window or depth)

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    (void)request_id;
    if (request->tag < CALLS_MAX) {
        runs[request->tag]++;
    }
    response->tag = request->tag + ANSWER_OFFSET;
    return 0;
}

static void answered(struct tt_Client* answering, int8_t return_code, struct message* response) {
    (void)answering;
    if (return_code == tt_CALL_TIMEOUT) {
        timeouts++;
        return;
    }
    if (return_code != 0 || response == NULL || response->tag < ANSWER_OFFSET ||
        response->tag - ANSWER_OFFSET >= CALLS_MAX) {
        wrong_answers++;
        return;
    }
    answers[response->tag - ANSWER_OFFSET]++;
}

struct datagram {
    uint64_t at;
    struct tt_Context* to;
    uint32_t from_ip;
    size_t length;
    uint8_t data[tt_MAX_BUFFER_LENGTH * 2];
};
static struct datagram wire[WIRE_MAX];
static int wire_count;
static struct tt_Context* sending;

// Counts each CallRequest and CallResponse by its call, and drops an answer the arm's script names.
static bool lost(const uint8_t* wire_form, size_t wire_len) {
    static uint8_t buf[((size_t)tt_MAX_BUFFER_LENGTH * 2) + sizeof(struct tt_Header)];
    if (wire_len == 0 || wire_len >= (size_t)tt_MAX_BUFFER_LENGTH * 2) {
        return false;
    }
    size_t len = test_classic_form(wire_form, wire_len, buf);
    bool drop = false;
    for (size_t at = sizeof(struct tt_Header); at + sizeof(struct tt_SubmessageHeader) <= len;) {
        const struct tt_SubmessageHeader* sub = (const void*)(buf + at);
        const uint8_t* body = (const uint8_t*)sub + sizeof(struct tt_SubmessageHeader);
        if (sub->type == tt_SUBMESSAGE_TYPE_CALLREQUEST) {
            request_sends[tag_of_seq[((const struct tt_CallRequestHeader*)body)->seq_no]]++;
        } else if (sub->type == tt_SUBMESSAGE_TYPE_CALLRESPONSE) {
            uint16_t tag = tag_of_seq[((const struct tt_CallResponseHeader*)body)->seq_no];
            uint32_t nth = response_sends[tag]++;
            drop = drop || (nth < 8 && (drop_response_mask[tag] & (1U << nth)) != 0);
        }
        if (sub->length == 0) {
            break;
        }
        at += sub->length;
    }
    return drop;
}

static void on_send(const void* buf, size_t len) {
    if (sending == NULL || lost(buf, len)) {
        return;
    }
    EXPECT_TRUE(wire_count < WIRE_MAX && len <= sizeof(wire[0].data));
    if (wire_count >= WIRE_MAX || len > sizeof(wire[0].data)) {
        return;
    }
    struct datagram* d = &wire[wire_count++];
    d->at = test_mock_now + (100 * US);
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

// A server whose answers fit entries for `max_response_bytes`, and `client_count` Clients whose requests fit theirs.
static void setup(uint32_t max_response_bytes, int client_count) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&server_context, SERVER_ID);
    init_context(&client_context, CLIENT_ID);
    memset(&server, 0, sizeof(server));
    memset(clients, 0, sizeof(clients));
    memset(runs, 0, sizeof(runs));
    memset(answers, 0, sizeof(answers));
    memset(request_sends, 0, sizeof(request_sends));
    memset(response_sends, 0, sizeof(response_sends));
    memset(drop_response_mask, 0, sizeof(drop_response_mask));
    timeouts = 0;
    wrong_answers = 0;
    refused_calls = 0;
    wire_count = 0;
    struct tt_ServerOptions server_options = {.callback = (tt_SERVER_CALLBACK)serve,
                                              .max_response_bytes = max_response_bytes};
    EXPECT_TRUE(tt_Server_storage_size(&server_options) <= sizeof(server_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_server(tt_Context_default_node(&server_context), &server, &service,
                                                   "small_cache/echo", NULL, &server_options, server_storage,
                                                   tt_Server_storage_size(&server_options)));
    struct tt_ClientOptions client_options = {.callback = (tt_CLIENT_CALLBACK)answered,
                                              .max_request_bytes = MESSAGE_BYTES};
    EXPECT_TRUE(tt_Client_storage_size(&client_options) <= sizeof(client_storage[0]));
    for (int c = 0; c < client_count; c++) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(tt_Context_default_node(&client_context), &clients[c], &service,
                                                       "small_cache/echo", NULL, &client_options, client_storage[c],
                                                       tt_Client_storage_size(&client_options)));
    }
    test_mock_send_hook = on_send;
}

// `client_count` Clients each calling every `period`, Client c `c x period / client_count` after the first, for
// `duration`; every `lose_every`-th call (0: none) has its first answer lost. Then runs on 3 s, so every call has been
// answered or has given up. Returns the number of calls made.
static uint16_t drive(int client_count, uint64_t period, uint64_t duration, int lose_every) {
    uint64_t next_at[CLIENTS_MAX];
    uint64_t start = test_mock_now;
    for (int c = 0; c < client_count; c++) {
        next_at[c] = start + ((uint64_t)c * period / (uint64_t)client_count);
    }
    uint16_t tag = 0;
    for (;;) {
        int due = 0;
        for (int c = 1; c < client_count; c++) {
            due = next_at[c] < next_at[due] ? c : due;
        }
        if (next_at[due] >= start + duration || tag + 1 >= CALLS_MAX) {
            break;
        }
        run_until(next_at[due]);
        next_at[due] += period;
        struct message request = {tag};
        tag_of_seq[client_context.call_seq_no] = tag;
        if (lose_every > 0 && tag % lose_every == 0) {
            drop_response_mask[tag] = 1U;
        }
        sending = &client_context;
        tt_ret_t ret = tt_Client_call(&clients[due], (struct tt_Request*)&request);
        sending = NULL;
        if (ret != tt_RET_OK) {
            refused_calls++;
            continue; // the tag is not used: the next call takes it
        }
        tag++;
    }
    run_until(test_mock_now + (3 * tt_SECOND));
    return tag;
}

struct tally {
    uint32_t calls;
    uint32_t run_twice;     // a callback run more than once for one call: the failure
    uint32_t answered_once; // calls answered exactly once, their callback run exactly once
    uint32_t turned_away;   // calls sent more than their script explains (1, or 2 with the first answer lost)
};

static struct tally count(uint16_t calls) {
    struct tally t = {calls, 0, 0, 0};
    for (uint16_t k = 0; k < calls; k++) {
        t.run_twice += runs[k] > 1 ? 1U : 0U;
        t.answered_once += runs[k] == 1 && answers[k] == 1 ? 1U : 0U;
        uint32_t scripted = drop_response_mask[k] != 0 ? 2U : 1U;
        t.turned_away += request_sends[k] > scripted ? 1U : 0U;
        EXPECT_TRUE(answers[k] <= runs[k]); // never an answer to a call the server did not run
    }
    return t;
}

static void report(const char* name, struct tally t, double calls_per_s) {
    printf("  %-30s %5.1f calls/s: %4u calls, %4u answered once, %3u turned away, %u run twice, %d timeouts, "
           "%u refused\n",
           name, calls_per_s, t.calls, t.answered_once, t.turned_away, t.run_twice, timeouts, refused_calls);
}

static void test_one_client(const char* name, uint64_t period, int lose_every) {
    setup(MESSAGE_BYTES, 1);
    struct tally t = count(drive(1, period, 20 * tt_SECOND, lose_every));
    report(name, t, (double)tt_SECOND / (double)period);
    EXPECT_TRUE(t.calls > 30);
    EXPECT_EQ_U32(0, t.run_twice);
    EXPECT_EQ_U32(t.calls, t.answered_once);
    EXPECT_EQ_U32(0, t.turned_away);
    EXPECT_EQ_INT(0, timeouts);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

static void test_clients(const char* name, int client_count, bool past_capacity) {
    setup(MESSAGE_BYTES, client_count);
    uint64_t period = 500 * MS;
    struct tally t = count(drive(client_count, period, 20 * tt_SECOND, 0));
    report(name, t, (double)client_count * (double)tt_SECOND / (double)period);
    EXPECT_TRUE(t.calls > 30);
    EXPECT_EQ_U32(0, t.run_twice);
    EXPECT_EQ_INT(0, wrong_answers);
    if (past_capacity) {
        EXPECT_TRUE(t.turned_away > 0); // the limit is real
    } else {
        EXPECT_EQ_U32(0, t.turned_away);
        EXPECT_EQ_U32(t.calls, t.answered_once);
        EXPECT_EQ_INT(0, timeouts);
    }
    test_mock_send_hook = NULL;
}

static void test_entries_too_small(void) {
    setup(4, 1); // entries of ALIGN8(4 + 8 + 4) = 16 bytes; the answer takes 4 + 8 + 12 = 24
    struct tally t = count(drive(1, 500 * MS, 5 * tt_SECOND, 1));
    report("D  entries too small", t, 2.0);
    EXPECT_TRUE(t.calls > 5);
    EXPECT_EQ_U32(t.calls, t.run_twice); // every call's lost answer is computed again
    test_mock_send_hook = NULL;
}

int main(void) {
    printf("  capacity: %d entries / 1.25 s = %.1f calls/s shared; %d per Client at any rate\n",
           tt_MAX_SERVER_CACHE_COUNT, tt_MAX_SERVER_CACHE_COUNT / 1.25, tt_CLIENT_CALL_WINDOW);
    test_one_client("E1 example: 1 Client, 500 ms", 500 * MS, 1);
    test_one_client("E2 burst: 1 Client, 5 ms", 5 * MS, 3);
    test_clients("C0 at cap: 3 Clients, 500 ms", 3, false);
    test_clients("C1 past cap: 4 Clients, 500 ms", 4, true);
    test_entries_too_small();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_service_small_cache: all tests passed\n");
    return 0;
}
