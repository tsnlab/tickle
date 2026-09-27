/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The auto retry interval (call_retry_interval 0) against a simulated server, on the mock clock
// (CONTEXT_NODE_PLAN.md "Client retry fix", 2026-09-27). Each call is run to its end: the client's call_retry
// timers fire at their scheduled times, and the server's answer arrives a given time after the request was first
// sent, or never. The interval used to be 1.5 x an EMA of accepted answers only, with no floor: one fast answer
// shrank every later call's budget to match, and an answer slower than that timed out every call from then on.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: runs the client's scheduler entries and delivers responses through tickle.c's static functions.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define NEVER UINT64_MAX
#define US 1000ULL
#define MS 1000000ULL
#define FAST (100 * US)
#define SLOW (40 * MS)

static struct tt_Context node;
static struct tt_Service service;
static struct tt_Client client;
static int last_return_code;
static int callbacks;

static int32_t stub_request_encode_size(struct tt_Request* request) {
    (void)request;
    return 4;
}

static int32_t stub_request_encode(struct tt_Request* request, uint8_t* payload, const uint32_t len) {
    (void)request;
    if (len < 4) {
        return -1;
    }
    memset(payload, 0xab, 4);
    return 4;
}

static void on_answer(struct tt_Client* answered, int8_t return_code, struct tt_Response* response) {
    (void)answered;
    (void)response;
    last_return_code = return_code;
    callbacks++;
}

static void setup(uint32_t call_retry_interval) {
    test_mock_reset();
    test_mock_now = 1000 * MS;
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    memset(&service, 0, sizeof(service));
    service.name = "retry_service";
    service.request_encode_size = stub_request_encode_size;
    service.request_encode = stub_request_encode;
    service.response_size = 1; // return_code 1 below skips decode
    service.call_retry_interval = call_retry_interval;
    memset(&client, 0, sizeof(client));
    client.endpoint.kind = tt_KIND_SERVICE_CLIENT;
    client.endpoint.id = 0x12345678;
    client.node = &node;
    client.service = &service;
    client.callback = on_answer;
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&client;
}

// One call to its end. The answer arrives answer_after ns after the request was first sent (NEVER: no answer).
// Returns true if it was answered, false if it timed out; *took is the time from the call to its callback.
static bool run_call(uint64_t answer_after, uint64_t* took) {
    struct tt_Request request;
    uint64_t start = test_mock_now;
    uint64_t answer_at = answer_after == NEVER ? NEVER : start + answer_after;
    int before = callbacks;
    if (tt_Client_call(&client, &request) != tt_RET_OK) {
        return false;
    }
    uint16_t seq_no =
        ((struct tt_CallRequestHeader*)((uint8_t*)client.cache + sizeof(struct tt_SubmessageHeader)))->seq_no;
    while (callbacks == before) {
        struct tt_TCB* due = peek_scheduler(&node);
        uint64_t next = due != NULL ? due->time : NEVER;
        if (answer_at != NEVER && answer_at <= next) {
            test_mock_now = answer_at;
            struct tt_Header header;
            memset(&header, 0, sizeof(header));
            header.magic_value = NATIVE_MAGIC_VALUE;
            header.version = tt_VERSION;
            header.source = 2;
            struct tt_CallResponseHeader response;
            memset(&response, 0, sizeof(response));
            response.endpoint_id = client.endpoint.id;
            response.seq_no = seq_no;
            response.return_code = 1;
            process_callresponse(&node, &header, (uint8_t*)&response, 0, sizeof(response));
            break;
        }
        if (due == NULL) {
            break; // nothing scheduled and no answer coming: the call is stuck, which the caller reports
        }
        struct tt_TCB fire = *due;
        pop_scheduler(&node);
        test_mock_now = fire.time;
        fire.function(&node, fire.time, fire.param);
    }
    *took = test_mock_now - start;
    while (peek_scheduler(&node) != NULL) { // an answered call's timer is unscheduled; nothing else may be left
        pop_scheduler(&node);
    }
    test_mock_now += MS; // the caller's pause between calls
    return callbacks > before && last_return_code != (int)tt_CALL_TIMEOUT;
}

// One fast answer used to set the budget of every later call: 100 us gave 0.6 ms, and a 3 ms answer timed out.
static void test_fast_then_slow_answers_are_all_answered(void) {
    setup(0);
    uint64_t took = 0;
    for (int i = 0; i < 5; i++) {
        EXPECT_TRUE(run_call(FAST, &took));
    }
    int answered = 0;
    for (int i = 0; i < 10; i++) {
        answered += run_call(3 * MS, &took) ? 1 : 0;
    }
    EXPECT_EQ_INT(10, answered);
}

// An answer slower than the floor's budget: the first calls may time out, the backoff grows the budget, and from
// then on every call is answered. Without the backoff no answer is ever accepted, so the estimate never moves.
static void test_answers_slower_than_the_floor_are_reached_by_backoff(void) {
    setup(0);
    uint64_t took = 0;
    int timed_out_early = 0;
    for (int i = 0; i < 5; i++) {
        timed_out_early += run_call(SLOW, &took) ? 0 : 1;
    }
    EXPECT_TRUE(timed_out_early <= 2);
    int answered = 0;
    for (int i = 0; i < 15; i++) {
        answered += run_call(SLOW, &took) ? 1 : 0;
    }
    EXPECT_EQ_INT(15, answered);
}

// After timeouts have backed the estimate off, the first fast answer brings the interval straight back to the floor,
// not 1/8 of the way per call.
static void test_a_fast_answer_after_a_backoff_restores_the_floor(void) {
    setup(0);
    uint64_t took = 0;
    for (int i = 0; i < 3; i++) {
        EXPECT_TRUE(run_call(FAST, &took));
    }
    for (int i = 0; i < 5; i++) {
        EXPECT_TRUE(!run_call(NEVER, &took)); // the server stops answering
    }
    EXPECT_TRUE(compute_retry_interval(&client) > 10 * tt_CALL_RETRY_INTERVAL);
    EXPECT_TRUE(run_call(FAST, &took)); // it answers again, fast
    EXPECT_EQ_U32((uint32_t)tt_CALL_RETRY_INTERVAL, compute_retry_interval(&client));
}

// A dead server: every call ends in tt_CALL_TIMEOUT, none takes longer than (count + 1) x the cap, and the interval
// stops at the cap however many calls time out.
static void test_a_dead_server_bounds_every_call(void) {
    setup(0);
    uint64_t took = 0;
    uint64_t longest = 0;
    for (int i = 0; i < 20; i++) {
        callbacks = 0;
        EXPECT_TRUE(!run_call(NEVER, &took));
        EXPECT_EQ_INT(1, callbacks);
        EXPECT_EQ_INT((int)tt_CALL_TIMEOUT, last_return_code);
        longest = took > longest ? took : longest;
    }
    EXPECT_TRUE(longest <= (uint64_t)(tt_CALL_RETRY_COUNT + 1) * tt_CALL_RETRY_INTERVAL_MAX);
    EXPECT_EQ_U32((uint32_t)tt_CALL_RETRY_INTERVAL_MAX, compute_retry_interval(&client));
}

// An explicit call_retry_interval is used as given: no floor, no backoff, no cap - a timeout changes nothing.
static void test_an_explicit_interval_is_used_as_given(void) {
    const uint32_t explicit_interval = 2 * MS;
    setup(explicit_interval);
    uint64_t took = 0;
    for (int i = 0; i < 3; i++) {
        EXPECT_TRUE(!run_call(NEVER, &took));
        EXPECT_EQ_U64((uint64_t)(tt_CALL_RETRY_COUNT + 1) * explicit_interval, took);
    }
    EXPECT_EQ_U32(explicit_interval, compute_retry_interval(&client));
    EXPECT_EQ_U32(0, client.latency);
}

int main(void) {
    test_fast_then_slow_answers_are_all_answered();
    test_answers_slower_than_the_floor_are_reached_by_backoff();
    test_a_fast_answer_after_a_backoff_restores_the_floor();
    test_a_dead_server_bounds_every_call();
    test_an_explicit_interval_is_used_as_given();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_call_retry_adaptive: all tests passed\n");
    return 0;
}
