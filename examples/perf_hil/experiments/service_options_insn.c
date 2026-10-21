/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// service_options_insn.c - user-space instructions per service call, client side, server side and the answer
// (2026-10-21, tt_InitOptions bundle 4, feat/initoptions-4-service). The change under test reads the call retry
// schedule, the kept-answer lifetime and the deferred timeout from the context's options (node->opts.service) where
// it read config.h constants. Derived from kept_answer_insn.c: instructions by perf_event_open() around the measured
// calls alone, on the mock HAL, so the count is the core's own.
//
// Both contexts hold tt_InitOptions_default() (both trees have it), so the change reads resolved options, as a created
// context does. Counted per call:
//   client    - the Client context's due timers and tt_Client_call(): its first wait and deadline (call_next_wake())
//               read the options - where the change is, on the client side.
//   server    - the Server context's due timers and process_packet() of the request: the kept answer's lifetime
//               (server_cache_lifetime()) reads the options.
//   control   - the Client context's process_packet() of the answer (match, callback, srtt update), which reads no
//               option. It must be the same in both builds; if it moves, they differ in something besides the change.
//   total     - the whole call.
// Cases:
//   auto_1    - one pair on the auto schedule (tt_Service and tt_ClientOptions call_retry_* 0: the context's), a call
//               every 1 ms.
//   auto_slow - the same, a call every 1 s: every kept answer expires before the next call.
//   rmw_1     - one pair, the rmw_tickle shape (100 ms x 50, explicit): no option is read for interval and count.
// Every case checks its own treatment: each call's callback ran once and each call was answered, else the line says
// VOID. Prints "<case> client=<w> server=<x> control=<y> total=<z> calls=<n>" per case.
//
// Build: service_options_insn.sh (A: the base tickle.c, B: the change), each from a copy of its own tree.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h> // tt_log_set_level, TT_LOG_NONE
#include <tickle/tickle.h>

#define TEST_MOCK_DEFINE_STORAGE
#include "../../../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: process_packet() is static
#include "../../../tests/test_mock.h"

#define SERVER_ID 1
#define CLIENT_ID 2
#define SERVER_IP 0x0a000001U
#define CLIENT_IP 0x0a000002U
#define PORT 40000
#define CLOCK_NS (100ULL * tt_SECOND)
#define MAX_PAIRS 12
#define CALLS 20000U
#define EXPIRE_CALLS 5000U
#define WARMUP_PER_PAIR (tt_CLIENT_CALL_WINDOW + 2)
#define RMW_INTERVAL 100000000U // rmw_tickle's 100 ms
#define RMW_COUNT 50U           // x 50

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
        .name = "service_options_insn::srv::dds_::Echo_",              \
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
static struct tt_Server servers[MAX_PAIRS];
static struct tt_Client clients[MAX_PAIRS];
static char names[MAX_PAIRS][32];
static uint64_t runs;
static uint64_t answers;

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    (void)request_id;
    runs++;
    response->tag = request->tag + 1;
    return 0;
}
static void answered(struct tt_Client* client, int8_t return_code, struct message* response) {
    (void)client;
    answers += (return_code == 0 && response != NULL) ? 1 : 0;
}

static uint8_t captured[tt_MAX_BUFFER_LENGTH * 2];
static size_t captured_length;
static int captured_count;
static void capture(const void* buf, size_t len) {
    if (len <= sizeof(captured)) {
        memcpy(captured, buf, len);
        captured_length = len;
        captured_count++;
    }
}

static int counter = -1;
static void counter_open(void) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    counter = (int)syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
    if (counter < 0) {
        perror("perf_event_open");
        exit(1);
    }
}
static uint64_t counter_read(void) {
    uint64_t value = 0;
    if (read(counter, &value, sizeof(value)) != (ssize_t)sizeof(value)) {
        return 0;
    }
    return value;
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
    while (run_due_entry(context, test_mock_now, &has_next, &next)) {
    }
}

static void setup(int pairs, uint32_t interval, uint32_t count) { // the explicit schedule of both sides; 0, 0: auto
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&server_context, SERVER_ID);
    init_context(&client_context, CLIENT_ID);
    memset(servers, 0, sizeof(servers));
    memset(clients, 0, sizeof(clients));
    server_service.call_retry_interval = interval;
    server_service.call_retry_count = count;
    for (int pair = 0; pair < pairs; pair++) {
        (void)snprintf(names[pair], sizeof(names[pair]), "service_options_insn/echo_%d", pair);
        if (tt_Node_create_server(tt_Context_default_node(&server_context), &servers[pair], &server_service,
                                  names[pair], NULL, &(struct tt_ServerOptions) {.callback = (tt_SERVER_CALLBACK)serve},
                                  NULL, 0) != tt_RET_OK ||
            tt_Node_create_client(tt_Context_default_node(&client_context), &clients[pair], &client_service,
                                  names[pair], NULL,
                                  &(struct tt_ClientOptions) {.callback = (tt_CLIENT_CALLBACK)answered,
                                                              .call_retry_interval = interval,
                                                              .call_retry_count = count},
                                  NULL, 0) != tt_RET_OK) {
            printf("setup failed\n");
            exit(1);
        }
    }
    test_mock_send_hook = capture;
    run_due(&server_context);
    run_due(&client_context);
}

struct counts {
    uint64_t client;
    uint64_t server;
    uint64_t control;
    uint64_t total;
};

// One call of pair `pair` at the clock's next step; counts added to `into` when it is not NULL.
static void one_call(int pair, uint64_t step, uint32_t tag, struct counts* into) {
    uint64_t at_start = counter_read();
    test_mock_now += step;
    run_due(&client_context);
    captured_count = 0;
    struct message request = {tag};
    (void)tt_Client_call(&clients[pair], (struct tt_Request*)&request);
    int requests = captured_count;
    uint64_t at_server = counter_read();
    run_due(&server_context);
    captured_count = 0;
    if (requests == 1) {
        memcpy(server_context.rx_buffer, captured, captured_length);
        (void)process_packet(&server_context, server_context.rx_buffer, 0, (uint32_t)captured_length, CLIENT_IP, PORT,
                             tt_TRANSPORT_UDP);
    }
    int responses = captured_count;
    uint64_t at_client = counter_read();
    if (responses == 1) {
        memcpy(client_context.rx_buffer, captured, captured_length);
        (void)process_packet(&client_context, client_context.rx_buffer, 0, (uint32_t)captured_length, SERVER_IP, PORT,
                             tt_TRANSPORT_UDP);
    }
    uint64_t at_end = counter_read();
    if (into != NULL) {
        into->client += at_server - at_start;
        into->server += at_client - at_server;
        into->control += at_end - at_client;
        into->total += at_end - at_start;
    }
}

static void run_case(const char* name, int pairs, uint32_t interval, uint32_t count, uint64_t step, uint32_t calls) {
    setup(pairs, interval, count);
    uint32_t tag = 0;
    for (int k = 0; k < WARMUP_PER_PAIR; k++) {
        for (int pair = 0; pair < pairs; pair++) {
            one_call(pair, step, tag++, NULL);
        }
    }
    runs = 0;
    answers = 0;
    struct counts counts = {0, 0, 0, 0};
    (void)ioctl(counter, PERF_EVENT_IOC_RESET, 0);
    (void)ioctl(counter, PERF_EVENT_IOC_ENABLE, 0);
    for (uint32_t k = 0; k < calls; k++) {
        one_call((int)(k % (uint32_t)pairs), step, tag++, &counts);
    }
    (void)ioctl(counter, PERF_EVENT_IOC_DISABLE, 0);
    if (runs != calls || answers != calls) {
        printf("%s VOID runs=%llu answers=%llu calls=%u\n", name, (unsigned long long)runs, (unsigned long long)answers,
               calls);
        return;
    }
    printf("%s client=%.2f server=%.2f control=%.2f total=%.2f calls=%u\n", name, (double)counts.client / calls,
           (double)counts.server / calls, (double)counts.control / calls, (double)counts.total / calls, calls);
}

int main(void) {
    tt_log_set_level(TT_LOG_NONE);
    counter_open();
    run_case("auto_1", 1, 0, 0, tt_MILLISECOND, CALLS);
    run_case("auto_slow", 1, 0, 0, tt_SECOND, EXPIRE_CALLS);
    run_case("rmw_1", 1, RMW_INTERVAL, RMW_COUNT, tt_MILLISECOND, CALLS);
    return 0;
}
