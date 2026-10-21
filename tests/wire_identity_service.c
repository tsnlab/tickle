/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// tt_InitOptions bundle 4 (tests/wire_identity_service.sh): a services scenario whose trace - every datagram either
// side sends, when, and whether it was carried, every answer the client gets and every callback the server runs - is
// written to WIRE_CAPTURE_OUT. Built against two trees, the traces are compared byte for byte: at the default options
// a context's services must behave exactly as they did before the options existed.
//
// Two whitebox contexts on the mock HAL, each holding tt_InitOptions_default() (which both trees have), a client and a
// server on the auto schedule (tt_Service.call_retry_* 0, the context's defaults), datagrams carried by hand 100 us
// each way, the clock stepped 50 us at a time and both contexts' timers run as they fall due. Calls one at a time:
//   0. 1 with the server unreachable, before any answer - the seed schedule and its timeout;
//   1. 10 answered at once - the client learns srtt;
//   2. 30 under a fixed pseudo-random 30% loss each way - retries, kept answers re-sent, the server's gap learnt;
//   3. 5 with the server unreachable - timeouts, the backoff up to the deadline;
//   4. 10 answered at once again - the estimate restored;
//   5. 3 deferred (tt_CALL_DEFERRED) and answered 300 ms later; then one answered 4.5 s later - after the client gave
//      up, inside tt_SERVER_DEFERRED_RESPONSE_TIMEOUT, so the late answer still goes out (and is ignored).
// Not a unit test (no test_ prefix): `make test` does not build it; the script builds it against both trees.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; the script passes each tree's tickle.c

#ifndef WIRE_CAPTURE_OUT
#define WIRE_CAPTURE_OUT \
    "/dev/null" // tests/wire_identity_service.sh names a file; built bare (lint), the trace goes nowhere
#endif

#define SERVER_ID 1
#define CLIENT_ID 2
#define SERVER_IP 0x0a000001U
#define CLIENT_IP 0x0a000002U
#define PORT 40000
#define CLOCK_NS (100ULL * tt_SECOND)
#define STEP_NS (50ULL * 1000ULL)
#define LATENCY_NS (100ULL * 1000ULL)
#define IN_FLIGHT_MAX 64
#define DATAGRAM_MAX 512

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
        .name = "wire_identity::srv::dds_::Echo_",                     \
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

static struct tt_Service server_service = ECHO_SERVICE;
static struct tt_Service client_service = ECHO_SERVICE;
static struct tt_Context server_context;
static struct tt_Context client_context;
static struct tt_Server server;
static struct tt_Client client;

static FILE* trace;
static uint32_t lcg = 12345U;
static uint32_t loss_percent; // each datagram is dropped with this chance
static bool server_unreachable;
static bool deferring;
static bool deferred_pending;
static tt_RequestId deferred_id;
static uint64_t deferred_due;
static bool call_done;

// One record: kind, the time since CLOCK_NS, a value, then `len` bytes.
static void record(char kind, uint64_t value, const void* bytes, uint32_t len) {
    uint64_t at = test_mock_now - CLOCK_NS;
    fputc(kind, trace);
    fwrite(&at, sizeof(at), 1, trace);
    fwrite(&value, sizeof(value), 1, trace);
    fwrite(&len, sizeof(len), 1, trace);
    if (len != 0) {
        fwrite(bytes, 1, len, trace);
    }
}

struct datagram {
    uint64_t due;
    struct tt_Context* to;
    uint32_t from_ip;
    uint32_t len;
    uint8_t bytes[DATAGRAM_MAX];
};
static struct datagram in_flight[IN_FLIGHT_MAX];
static int in_flight_count;
static struct tt_Context* sending; // the context whose code is running, so whose datagrams the hook sees

static bool lose(void) {
    lcg = (lcg * 1103515245U) + 12345U;
    return ((lcg >> 16) % 100U) < loss_percent;
}

static void hook(const void* buf, size_t len) {
    bool to_server = sending == &client_context;
    bool dropped =
        lose() || (to_server && server_unreachable) || len > DATAGRAM_MAX || in_flight_count == IN_FLIGHT_MAX;
    record(to_server ? 'Q' : 'R', dropped ? 1U : 0U, buf, (uint32_t)len);
    if (dropped) {
        return;
    }
    struct datagram* d = &in_flight[in_flight_count++];
    d->due = test_mock_now + LATENCY_NS;
    d->to = to_server ? &server_context : &client_context;
    d->from_ip = to_server ? CLIENT_IP : SERVER_IP;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
}

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    record('S', request->tag, NULL, 0);
    if (deferring) {
        deferred_pending = true;
        deferred_id = request_id;
        return tt_CALL_DEFERRED;
    }
    response->tag = request->tag + 1000U;
    return 0;
}

static void answered(struct tt_Client* answering, int8_t return_code, struct message* response) {
    (void)answering;
    record('A', ((uint64_t)(uint8_t)return_code << 32) | (response != NULL ? response->tag : 0xffffffffU), NULL, 0);
    call_done = true;
}

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    tt_InitOptions_default(&context->opts);
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_TX_BUFFER_LENGTH;
    context->last_modified = CLOCK_NS;
}

static void run_due(struct tt_Context* context) {
    bool has_next = false;
    uint64_t next = 0;
    sending = context;
    while (run_due_entry(context, test_mock_now, &has_next, &next)) {
    }
    sending = NULL;
}

// One clock step: datagrams due are delivered, then each context's timers due run, then a deferred answer due goes out.
static void step(void) {
    test_mock_now += STEP_NS;
    for (int i = 0; i < in_flight_count;) {
        if (in_flight[i].due <= test_mock_now) {
            struct datagram d = in_flight[i];
            in_flight[i] = in_flight[--in_flight_count];
            sending = d.to;
            memcpy(d.to->rx_buffer, d.bytes, d.len);
            (void)process_packet(d.to, d.to->rx_buffer, 0, d.len, d.from_ip, PORT, tt_TRANSPORT_UDP);
            sending = NULL;
        } else {
            i++;
        }
    }
    run_due(&server_context);
    run_due(&client_context);
    if (deferred_pending && test_mock_now >= deferred_due) {
        deferred_pending = false;
        struct message response = {7777U};
        sending = &server_context;
        tt_ret_t ret = tt_Server_send_response(&server, deferred_id, 0, (struct tt_Response*)&response);
        sending = NULL;
        record('D', (uint64_t)(int64_t)ret, NULL, 0);
    }
}

static void run_for(uint64_t ns) {
    uint64_t until = test_mock_now + ns;
    while (test_mock_now < until) {
        step();
    }
}

// One call, to its end (an answer or a timeout), then 1 ms of quiet; `defer_ns` > 0 defers its answer that long.
static void call(uint32_t tag, uint64_t defer_ns) {
    deferring = defer_ns != 0;
    deferred_due = test_mock_now + defer_ns;
    call_done = false;
    struct message request = {tag};
    sending = &client_context;
    tt_ret_t ret = tt_Client_call(&client, (struct tt_Request*)&request);
    sending = NULL;
    record('C', (uint64_t)(int64_t)ret, NULL, 0);
    for (uint64_t steps = 0; !call_done && steps < 200000U; steps++) {
        step();
    }
    run_for(tt_MILLISECOND);
}

int main(void) {
    trace = fopen(WIRE_CAPTURE_OUT, "wb");
    if (trace == NULL) {
        fprintf(stderr, "cannot write %s\n", WIRE_CAPTURE_OUT);
        return 2;
    }
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&server_context, SERVER_ID);
    init_context(&client_context, CLIENT_ID);
    memset(&server, 0, sizeof(server));
    memset(&client, 0, sizeof(client));
    if (tt_Node_create_server(tt_Context_default_node(&server_context), &server, &server_service, "wire/echo", NULL,
                              &(struct tt_ServerOptions) {.callback = (tt_SERVER_CALLBACK)serve}, NULL,
                              0) != tt_RET_OK ||
        tt_Node_create_client(tt_Context_default_node(&client_context), &client, &client_service, "wire/echo", NULL,
                              &(struct tt_ClientOptions) {.callback = (tt_CLIENT_CALLBACK)answered}, NULL,
                              0) != tt_RET_OK) {
        fprintf(stderr, "cannot create the client and the server\n");
        return 2;
    }
    test_mock_send_hook = hook;

    uint32_t tag = 0;
    server_unreachable = true; // before any answer: the seed schedule
    call(tag++, 0);
    server_unreachable = false;
    for (int i = 0; i < 10; i++) {
        call(tag++, 0);
    }
    loss_percent = 30;
    for (int i = 0; i < 30; i++) {
        call(tag++, 0);
    }
    loss_percent = 0;
    server_unreachable = true;
    for (int i = 0; i < 5; i++) {
        call(tag++, 0);
    }
    server_unreachable = false;
    for (int i = 0; i < 10; i++) {
        call(tag++, 0);
    }
    for (int i = 0; i < 3; i++) {
        call(tag++, 300 * tt_MILLISECOND);
    }
    call(tag++, 4500 * tt_MILLISECOND);
    run_for(5 * tt_SECOND);

    long size = ftell(trace);
    fclose(trace);
    printf("service trace: %u calls, %ld bytes -> %s\n", tag, size, WIRE_CAPTURE_OUT);
    return 0;
}
