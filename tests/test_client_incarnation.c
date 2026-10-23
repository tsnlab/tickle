/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A client restarted under the same context id is a new client, to the server and to itself (2026-10-23, ROADMAP 5a).
//
// A CallRequest names its call by (source context, client_tag, seq_no), and every launch's seq_no started at 0. The
// server keeps an answered response for as long as the client may retry it, and drops it when discovery shows the
// client under a new entity_id or the context said farewell. A client killed (no farewell) and started again under the
// same context id before discovery shows the new launch - or with no discovery table at all - asked its first call
// with its predecessor's seq_no and was answered with its predecessor's response, without the callback running. And a
// predecessor's answer still on its way could be taken by the new launch as the answer to its own call of that seq_no.
//
// Nothing on the wire changes. The server tells launches apart by the request's source port: a context sends
// everything from its data port, bound afresh at every launch (tt_bind(), port 0), so the new launch asks from another
// port (struct tt_Server.cache_client_port). The client starts its seq_no at a per-launch value drawn with its
// entity_id base (tt_Context_create()), so a predecessor's late answer is unlikely to name one of its calls.
//
// The network is simulated as in test_service_exactly_once.c: datagrams on a wire with a one-way delay, the contexts'
// timers and the wire run in time order on the mock clock. Client A and client B are two launches of context 2, from
// ports A_PORT and B_PORT; A is killed by no longer running it - no farewell, nothing more on the wire from it. What
// the server sends goes to every live client, as a broadcast answer does (process_callrequest() falls back to one when
// its transmit buffer is not empty), so a late answer to A reaches B.
//
// Arms, each reading enforced below:
//   R0 control  A's answer is lost; A's retry, from the same port, is served from the kept answer: the callback runs
//               once and A gets its answer. The port rule must not touch a retry of one launch.
//   R1 server   A's call is answered and kept; A dies; B asks with A's seq_no (every launch's on the parent, and the
//               worst case of a random start). Reading: B's callback runs (runs[2] == 1) and B gets 1002, its own
//               answer. On the parent B got 1001, A's answer, and its callback never ran.
//   R2 client   A's answer is slow on the wire; A dies; B, created by tt_Context_create() (its seq_no start drawn
//               there), calls before A's answer lands. Reading: B takes only its own answer (1002), none of A's. On the
//               parent B's seq_no started at 0, A's, and A's late answer was taken as B's.

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
#define SERVER_PORT 40000
#define A_PORT 40001
#define B_PORT 40002
#define CLOCK_NS (100ULL * tt_SECOND)
#define WIRE_MAX 64
#define ANSWER_OFFSET 1000U
#define TAGS 8
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

static struct tt_Service service = {
    .name = "incarnation::srv::dds_::Echo_",
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
static struct tt_Context a_context;
static struct tt_Context b_context;
static struct tt_Context drawn_context; // only for the seq_no start tt_Context_create() draws
static struct tt_Server server;
static struct tt_Client a_client;
static struct tt_Client b_client;
static bool a_alive;
static bool b_alive;

static int runs[TAGS];
static int a_answers[TAGS]; // by the answer's tag: what client A was handed
static int b_answers[TAGS];
static int wrong_answers;
static uint32_t lose_responses;   // the next this many CallResponses the server sends are lost
static uint64_t slow_response_ns; // the next CallResponse the server sends takes this much longer, then 0
static uint64_t one_way_ns = 100 * US;

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    (void)request_id;
    if (request->tag < TAGS) {
        runs[request->tag]++;
    }
    response->tag = request->tag + ANSWER_OFFSET;
    return 0;
}

static void answered(struct tt_Client* answering, int8_t return_code, struct message* response) {
    if (return_code != 0 || response == NULL || response->tag < ANSWER_OFFSET ||
        response->tag - ANSWER_OFFSET >= TAGS) {
        wrong_answers++;
        return;
    }
    if (answering == &a_client) {
        a_answers[response->tag - ANSWER_OFFSET]++;
    } else if (answering == &b_client) {
        b_answers[response->tag - ANSWER_OFFSET]++;
    } else {
        wrong_answers++;
    }
}

struct datagram {
    uint64_t at;
    struct tt_Context* to;
    uint32_t from_ip;
    uint16_t from_port;
    size_t length;
    uint8_t data[tt_MAX_BUFFER_LENGTH * 2];
};
static struct datagram wire[WIRE_MAX];
static int wire_count;
static struct tt_Context* sending;

// Whether the datagram (any wire form) carries a CallResponse.
static bool is_call_response(const uint8_t* wire_form, size_t wire_len) {
    static uint8_t buf[((size_t)tt_MAX_BUFFER_LENGTH * 2) + sizeof(struct tt_Header)];
    if (wire_len == 0 || wire_len >= (size_t)tt_MAX_BUFFER_LENGTH * 2) {
        return false;
    }
    size_t len = test_classic_form(wire_form, wire_len, buf);
    for (size_t at = sizeof(struct tt_Header); at + sizeof(struct tt_SubmessageHeader) <= len;) {
        const struct tt_SubmessageHeader* here = (const void*)(buf + at);
        if (here->type == tt_SUBMESSAGE_TYPE_CALLRESPONSE) {
            return true;
        }
        if (here->length == 0) {
            break;
        }
        at += here->length;
    }
    return false;
}

static void put_on_wire(struct tt_Context* to, uint32_t from_ip, uint16_t from_port, const void* buf, size_t len,
                        uint64_t extra) {
    EXPECT_TRUE(wire_count < WIRE_MAX && len <= sizeof(wire[0].data));
    if (wire_count >= WIRE_MAX || len > sizeof(wire[0].data)) {
        return;
    }
    struct datagram* d = &wire[wire_count++];
    d->at = test_mock_now + one_way_ns + extra;
    d->to = to;
    d->from_ip = from_ip;
    d->from_port = from_port;
    d->length = len;
    memcpy(d->data, buf, len);
}

static void on_send(const void* buf, size_t len) {
    EXPECT_TRUE(sending != NULL);
    if (sending == &a_context) {
        put_on_wire(&server_context, CLIENT_IP, A_PORT, buf, len, 0);
    } else if (sending == &b_context) {
        put_on_wire(&server_context, CLIENT_IP, B_PORT, buf, len, 0);
    } else if (sending == &server_context) {
        uint64_t extra = 0;
        if (is_call_response(buf, len)) {
            if (lose_responses > 0) {
                lose_responses--;
                return;
            }
            extra = slow_response_ns;
            slow_response_ns = 0;
        }
        // To every live launch of context 2: a dead one's port answers nothing.
        put_on_wire(&a_context, SERVER_IP, SERVER_PORT, buf, len, extra);
        put_on_wire(&b_context, SERVER_IP, SERVER_PORT, buf, len, extra);
    }
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

static bool alive(const struct tt_Context* context) {
    return context == &server_context || (context == &a_context && a_alive) || (context == &b_context && b_alive);
}

static struct tt_Context* const contexts[3] = {&server_context, &a_context, &b_context};

// The wire's earliest datagram (*first, -1 when none) and the earliest live timer (*timer, -1 when the datagram is
// sooner or there is none); returns when the earlier of them is due.
static uint64_t next_event(int* first, int* timer) {
    *first = -1;
    for (int i = 0; i < wire_count; i++) {
        if (*first < 0 || wire[i].at < wire[*first].at) {
            *first = i;
        }
    }
    uint64_t at = *first >= 0 ? wire[*first].at : UINT64_MAX;
    *timer = -1;
    for (int c = 0; c < 3; c++) {
        uint64_t due = alive(contexts[c]) ? next_timer(contexts[c]) : UINT64_MAX;
        if (due < at) {
            at = due;
            *timer = c;
        }
    }
    return at;
}

// Delivers the wire's datagram `index`, or drops it when its launch is dead.
static void deliver(int index) {
    struct datagram d = wire[index];
    wire[index] = wire[--wire_count];
    if (!alive(d.to)) {
        return;
    }
    memcpy(d.to->rx_buffer, d.data, d.length);
    sending = d.to;
    (void)process_packet(d.to, d.to->rx_buffer, 0, (uint32_t)d.length, d.from_ip, d.from_port, tt_TRANSPORT_UDP);
    sending = NULL;
}

// Runs the wire and the live contexts' timers, in time order, up to `until`. A datagram to a dead launch is dropped.
static void run_until(uint64_t until) {
    for (;;) {
        int first = -1;
        int timer = -1;
        uint64_t at = next_event(&first, &timer);
        if (at > until) {
            break;
        }
        test_mock_now = at > test_mock_now ? at : test_mock_now;
        if (timer < 0) {
            deliver(first);
        } else {
            run_due(contexts[timer]);
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

static void make_client(struct tt_Context* context, struct tt_Client* client) {
    init_context(context, CLIENT_ID);
    memset(client, 0, sizeof(*client));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(
                                 tt_Context_default_node(context), client, &service, "incarnation/echo", NULL,
                                 &(struct tt_ClientOptions) {.callback = (tt_CLIENT_CALLBACK)answered}, NULL, 0));
}

static void setup(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    test_mock_send_hook = NULL;
    init_context(&server_context, SERVER_ID);
    memset(&server, 0, sizeof(server));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_server(
                                 tt_Context_default_node(&server_context), &server, &service, "incarnation/echo", NULL,
                                 &(struct tt_ServerOptions) {.callback = (tt_SERVER_CALLBACK)serve}, NULL, 0));
    make_client(&a_context, &a_client);
    a_alive = true;
    b_alive = false;
    memset(runs, 0, sizeof(runs));
    memset(a_answers, 0, sizeof(a_answers));
    memset(b_answers, 0, sizeof(b_answers));
    wrong_answers = 0;
    lose_responses = 0;
    slow_response_ns = 0;
    wire_count = 0;
    test_mock_send_hook = on_send;
}

// B, the next launch of context 2, with its call seq_no starting at `seq_no`.
static void start_b(uint16_t seq_no) {
    make_client(&b_context, &b_client);
    b_context.call_seq_no = seq_no;
    b_alive = true;
}

static uint16_t call(struct tt_Context* context, struct tt_Client* client, uint32_t tag) {
    struct message request = {tag};
    uint16_t seq_no = 0;
    sending = context;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call_id(client, (struct tt_Request*)&request, &seq_no));
    sending = NULL;
    return seq_no;
}

// What tt_Context_create() draws for a launch's first call seq_no, at the mock's clock.
static uint16_t drawn_seq_start(void) {
    void (*hook)(const void*, size_t) = test_mock_send_hook;
    test_mock_send_hook = NULL; // nothing the scratch context does goes on the wire
    test_mock_node_id = CLIENT_ID;
    test_mock_link_resolves = true; // a link that owns the default broadcast, or the create refuses
    test_mock_link_addr = CLIENT_IP;
    test_mock_link_netmask = 0xffffff00U;
    memset(&drawn_context, 0, sizeof(drawn_context));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&drawn_context));
    uint16_t start = drawn_context.call_seq_no;
    test_mock_send_hook = hook;
    return start;
}

static void test_r0_a_retry_of_one_launch_is_served_from_the_kept_answer(void) {
    setup();
    lose_responses = 1;
    (void)call(&a_context, &a_client, 1);
    run_until(test_mock_now + (2 * tt_SECOND));
    printf("  R0 control: runs %d (expected 1), A answered %d (expected 1)\n", runs[1], a_answers[1]);
    EXPECT_EQ_INT(1, runs[1]);
    EXPECT_EQ_INT(1, a_answers[1]);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

static void test_r1_a_new_launch_is_not_answered_from_its_predecessors_cache(void) {
    setup();
    uint16_t a_seq = call(&a_context, &a_client, 1);
    run_until(test_mock_now + (10 * MS)); // answered, and kept for A's retries
    EXPECT_EQ_INT(1, a_answers[1]);
    EXPECT_TRUE(get_server_cache(&server, CLIENT_ID, a_client.client_tag, a_seq) != NULL);
    a_alive = false; // killed: no farewell
    start_b(a_seq);
    uint16_t b_seq = call(&b_context, &b_client, 2);
    EXPECT_EQ_INT(a_seq, b_seq);
    run_until(test_mock_now + (2 * tt_SECOND));
    printf(
        "  R1 server: B's callback runs %d (expected 1), B got its own answer %d (expected 1), A's %d (expected 0)\n",
        runs[2], b_answers[2], b_answers[1]);
    EXPECT_EQ_INT(1, runs[2]);
    EXPECT_EQ_INT(1, b_answers[2]);
    EXPECT_EQ_INT(0, b_answers[1]);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

static void test_r2_a_predecessors_late_answer_is_not_taken_as_the_new_launchs(void) {
    setup();
    slow_response_ns = 50 * MS;
    uint16_t a_seq = call(&a_context, &a_client, 1);
    run_until(test_mock_now + (2 * MS)); // the server has answered; the answer is still on the wire
    EXPECT_EQ_INT(1, runs[1]);
    a_alive = false;
    uint16_t start = drawn_seq_start();
    start_b(start);
    uint16_t b_seq = call(&b_context, &b_client, 2);
    run_until(test_mock_now + (2 * tt_SECOND));
    printf("  R2 client: A's seq_no %u, B's %u; B took its own answer %d (expected 1), A's %d (expected 0)\n",
           (unsigned)a_seq, (unsigned)b_seq, b_answers[2], b_answers[1]);
    EXPECT_TRUE(a_seq != b_seq);
    EXPECT_EQ_INT(1, b_answers[2]);
    EXPECT_EQ_INT(0, b_answers[1]);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

// The start is drawn per launch: two launches at different instants start at different seq_nos (the parent's were
// both 0).
static void test_the_seq_start_is_drawn_per_launch(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    uint16_t first = drawn_seq_start();
    test_mock_now = CLOCK_NS + (1234567 * US);
    uint16_t second = drawn_seq_start();
    printf("  seq_no start: %u and %u\n", (unsigned)first, (unsigned)second);
    EXPECT_TRUE(first != second);
}

int main(void) {
    test_r0_a_retry_of_one_launch_is_served_from_the_kept_answer();
    test_r1_a_new_launch_is_not_answered_from_its_predecessors_cache();
    test_r2_a_predecessors_late_answer_is_not_taken_as_the_new_launchs();
    test_the_seq_start_is_drawn_per_launch();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_client_incarnation: all tests passed\n");
    return 0;
}
