/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// G, the timer-lateness term of both retry timers' srtt + max(G, 4 x rttvar) (2026-10-08, ROADMAP Now 5a; DESIGN.md
// 6 and 7). It was the fixed 100 us tt_RELIABLE_RETRY_GRANULARITY, fitted to no measurement; the context now measures
// how late its own waits for a retry timer return and smooths that as RFC 6298 smooths a round trip: G = mean + 4 x
// deviation, at least tt_timer_resolution_ns(), and tt_TIMER_LATENESS_INITIAL (the old 100 us) until the first
// sample. The retry timer here is call_retry() on a client with no call outstanding, which does nothing.
//
// Through the mock HAL: with test_mock_receive_advances_clock a wait that times out lets its whole timeout pass, and
// test_mock_receive_late_ns more - a timer that runs that late. So every expected value below is known exactly.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) - whitebox, as test_poll_wait.c
#include "test_mock.h"

#define US 1000ULL
#define MS 1000000ULL
// Enough samples for the deviation of a constant lateness to decay to nothing: 3/4 per sample, from X / 2.
#define SETTLE_SAMPLES 64

static void noop_entry(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
}

// A client with no call outstanding: call_retry() on it returns at once (its cache is NULL).
static struct tt_Client idle_client;

static void setup(struct tt_Context* node) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    test_mock_reset();
    test_mock_receive_advances_clock = true;
}

// One timed wait to a retry timer `ahead` from now, which the mock's timer ends `late` after its deadline.
static void sleep_once(struct tt_Context* node, uint64_t ahead, uint64_t late) {
    test_mock_receive_late_ns = late;
    EXPECT_TRUE(tt_Context_schedule(node, test_mock_now + ahead, call_retry, &idle_client));
    EXPECT_EQ_INT(tt_RET_TIMEOUT, tt_Context_poll(node, -1));
}

// Before any wait has run to its deadline G is the constant it replaced, so a context that has not slept yet retries
// exactly as it did - and a call's G is twice it, as before.
static void test_cold_start_is_the_old_constant(void) {
    struct tt_Context node;
    setup(&node);
    EXPECT_EQ_U64((uint64_t)tt_TIMER_LATENESS_INITIAL, timer_lateness_ns(&node));
    EXPECT_EQ_U64(100 * US, timer_lateness_ns(&node)); // the value tt_RELIABLE_RETRY_GRANULARITY had
    EXPECT_EQ_U64(100 * US, reliable_retry_granularity(&node));
    EXPECT_EQ_U64(200 * US, call_retry_granularity(&node));
    EXPECT_EQ_U64(100 * US, timer_lateness_ns(NULL)); // no context to ask
}

// The case the change is for: a host whose timer wakes 37 us late converges on G = 37 us, not 100 us. The first sample
// starts the estimate as RFC 6298 starts srtt (mean X, deviation X / 2: G = 3X), and the deviation then decays away.
static void test_lateness_converges_on_how_late_the_loop_wakes(void) {
    struct tt_Context node;
    setup(&node);
    const uint64_t late = 37 * US;
    sleep_once(&node, 1 * MS, late);
    EXPECT_EQ_U32((uint32_t)late, node.timer_lateness_mean_ns);
    EXPECT_EQ_U64(3 * late, timer_lateness_ns(&node));
    for (int i = 1; i < SETTLE_SAMPLES; i++) {
        sleep_once(&node, 1 * MS, late);
    }
    EXPECT_EQ_U64(late, timer_lateness_ns(&node));
    EXPECT_EQ_U64(late, reliable_retry_granularity(&node));
    EXPECT_EQ_U64(2 * late, call_retry_granularity(&node));

    // And it follows the host: the same context, now 400 us late, moves to 400 us (the jump decays as any does).
    for (int i = 0; i < 3 * SETTLE_SAMPLES; i++) {
        sleep_once(&node, 5 * MS, 400 * US);
    }
    // Within the integer gains' rounding: a mean that climbs to X in 1/8 steps stops up to 7 ns short of it, with the
    // deviation left at that shortfall, so G ends at most 3 x 7 ns above X.
    EXPECT_TRUE(timer_lateness_ns(&node) >= 400 * US - 7);
    EXPECT_TRUE(timer_lateness_ns(&node) <= (400 * US) + 21);
}

// A jittering timer: G covers the spread, as srtt + 4 x rttvar does, and is not the mean the timer reaches only half
// the time.
static void test_a_jittering_timer_gives_more_than_its_mean(void) {
    struct tt_Context node;
    setup(&node);
    for (int i = 0; i < SETTLE_SAMPLES; i++) {
        sleep_once(&node, 1 * MS, (i % 2) == 0 ? 20 * US : 80 * US); // 50 us on average
    }
    uint64_t g = timer_lateness_ns(&node);
    EXPECT_TRUE(g > 80 * US); // above even the later wakes: 4 x a deviation of ~30 us over a mean of ~50
    EXPECT_TRUE(g < 250 * US);
}

// An exact timer (lateness 0, as the mock's is by default) cannot drive G below the finest step a wait can end on -
// one tick on FreeRTOS, here a mock resolution of 1 ms.
static void test_lateness_is_floored_at_the_timer_resolution(void) {
    struct tt_Context node;
    setup(&node);
    node.timer_resolution_ns = 1 * (uint32_t)MS;
    for (int i = 0; i < SETTLE_SAMPLES; i++) {
        sleep_once(&node, 5 * MS, 0);
    }
    EXPECT_EQ_U64(1 * MS, timer_lateness_ns(&node));

    // tt_Context_create() takes the floor from the HAL.
    struct tt_Context created;
    memset(&created, 0, sizeof(created));
    test_mock_timer_resolution_ns = 250 * US;
    test_mock_link_resolves = true; // a link to create on
    test_mock_link_addr = 0xc0a80a01U;
    test_mock_link_netmask = 0xffffff00U;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&created));
    EXPECT_EQ_U32(250 * (uint32_t)US, created.timer_resolution_ns);
    EXPECT_EQ_U64(100 * US, timer_lateness_ns(&created)); // and starts cold
    tt_Context_destroy(&created);
}

// Only a wait that ran to its deadline says anything about the timer. One that ended early - a signal, a wake, data -
// is not a sample, and leaves G cold.
static void test_a_wait_cut_short_is_not_a_sample(void) {
    struct tt_Context node;
    setup(&node);
    test_mock_receive_advances_clock = false; // the wait returns at once: interrupted, nothing elapsed
    EXPECT_TRUE(tt_Context_schedule(&node, test_mock_now + (1 * MS), call_retry, &idle_client));
    (void)tt_Context_poll(&node, -1);
    EXPECT_EQ_U32(0, node.timer_lateness_mean_ns);
    EXPECT_EQ_U64(100 * US, timer_lateness_ns(&node));

    setup(&node);
    test_mock_receive_return = 0; // a datagram (a rung bell) ends the wait
    EXPECT_TRUE(tt_Context_schedule(&node, test_mock_now + (1 * MS), call_retry, &idle_client));
    (void)tt_Context_poll(&node, 2 * (int64_t)MS);
    EXPECT_EQ_U32(0, node.timer_lateness_mean_ns);

    // An indefinite wait (nothing scheduled) has no deadline to be late for.
    setup(&node);
    test_mock_receive_late_ns = 5 * US;
    (void)tt_Context_poll(&node, -1);
    EXPECT_EQ_U32(0, node.timer_lateness_mean_ns);
}

// Only a retry timer's deadline is a sample. How late a wait ends depends on how long it was (Linux's poll timer
// slack is 0.1% of the timeout), so the context's other deadlines - a 500 ms announce, a poll's budget - would measure
// a lateness the retry never sees: an entry of another kind, and a budget that ends before the retry is due, are not
// samples.
static void test_only_a_retry_timers_deadline_is_a_sample(void) {
    struct tt_Context node;
    setup(&node);
    test_mock_receive_late_ns = 500 * US;
    EXPECT_TRUE(tt_Context_schedule(&node, test_mock_now + (1 * MS), noop_entry, NULL));
    EXPECT_EQ_INT(tt_RET_TIMEOUT, tt_Context_poll(&node, -1));
    EXPECT_EQ_U32(0, node.timer_lateness_mean_ns);

    setup(&node);
    test_mock_receive_late_ns = 60 * US;
    (void)tt_Context_poll(&node, 1 * (int64_t)MS); // nothing scheduled: the wait ends at the budget
    EXPECT_EQ_U32(0, node.timer_lateness_mean_ns);
    EXPECT_TRUE(tt_Context_schedule(&node, test_mock_now + (5 * MS), call_retry, &idle_client));
    (void)tt_Context_poll(&node, 1 * (int64_t)MS); // the budget ends first
    EXPECT_EQ_U32(0, node.timer_lateness_mean_ns);
    EXPECT_EQ_U64(100 * US, timer_lateness_ns(&node));

    // The control: the same wait for the retry timer is one, under a budget as without.
    (void)tt_Context_poll(&node, 10 * (int64_t)MS);
    EXPECT_EQ_U32(60 * (uint32_t)US, node.timer_lateness_mean_ns);
}

// Both retry timers G serves are the ones whose waits it samples: the reliable reader's ACKNACK retry and the call
// retry the tests above sleep to. Nothing else is.
static void test_the_acknack_retry_is_a_retry_timer(void) {
    EXPECT_TRUE(is_retry_timer(acknack_retry));
    EXPECT_TRUE(is_retry_timer(call_retry));
    EXPECT_TRUE(!is_retry_timer(noop_entry));
    EXPECT_TRUE(!is_retry_timer(keep_all_resolicit)); // a Publisher's timer, which does not use G
}

// The retry timers use it: a reliable proxy with a steady 400 us recovery retries at 400 us + G, its repair-in-flight
// window is transit + G, and an RPC client with a steady 100 us answer waits 100 us + 2G - with G measured at 250 us,
// where the old constant would have given 100 us.
static void test_the_retry_intervals_use_the_measured_lateness(void) {
    struct tt_Context node;
    setup(&node);
    for (int i = 0; i < SETTLE_SAMPLES; i++) {
        sleep_once(&node, 1 * MS, 250 * US);
    }
    EXPECT_EQ_U64(250 * US, timer_lateness_ns(&node));

    struct tt_WriterProxy proxy;
    memset(&proxy, 0, sizeof(proxy));
    proxy.recovery_srtt_ns = 400 * (uint32_t)US;
    proxy.recovery_rttvar_ns = 0;
    EXPECT_EQ_U64((400 + 250) * US, retry_interval_for(0, reliable_retry_granularity(&node), &proxy));
    if (tt_RELIABLE_RETRY_INTERVAL == 0 && tt_RELIABLE_RETRY_GRANULARITY == 0) {
        EXPECT_EQ_U64((400 + 250) * US, reliable_retry_interval(&node, &proxy));
    }
    proxy.transit_srtt_ns = 300 * (uint32_t)US;
    proxy.transit_rttvar_ns = 0;
    if (tt_RELIABLE_RETRY_GRANULARITY == 0) {
        EXPECT_EQ_U64((300 + 250) * US, repair_in_flight_ns(&node, &proxy));
    }

    struct tt_Service service;
    struct tt_Client client;
    memset(&service, 0, sizeof(service));
    memset(&client, 0, sizeof(client));
    client.node = &node;
    client.service = &service; // call_retry_interval 0: the auto path
    client.latency = 100 * (uint32_t)US;
    client.latency_var = 0;
    if (tt_CALL_RETRY_GRANULARITY == 0 && tt_RELIABLE_RETRY_GRANULARITY == 0) {
        EXPECT_EQ_U64((100 + 500) * US, compute_retry_interval(&client, 0));
    }
}

// A fixed G is used as given, whatever the context measured: the compile-time override.
static void test_a_fixed_granularity_overrides_the_measurement(void) {
    struct tt_Context node;
    setup(&node);
    for (int i = 0; i < SETTLE_SAMPLES; i++) {
        sleep_once(&node, 1 * MS, 250 * US);
    }
    EXPECT_EQ_U64(5 * US, granularity_for(5 * US, &node));
    EXPECT_EQ_U64(250 * US, granularity_for(0, &node));
}

int main(void) {
    test_cold_start_is_the_old_constant();
    test_lateness_converges_on_how_late_the_loop_wakes();
    test_a_jittering_timer_gives_more_than_its_mean();
    test_lateness_is_floored_at_the_timer_resolution();
    test_a_wait_cut_short_is_not_a_sample();
    test_only_a_retry_timers_deadline_is_a_sample();
    test_the_acknack_retry_is_a_retry_timer();
    test_the_retry_intervals_use_the_measured_lateness();
    test_a_fixed_granularity_overrides_the_measurement();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_timer_lateness: all tests passed\n");
    return 0;
}
