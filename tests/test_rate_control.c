/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Rate control (tt_LinkOptions.rate_control, rate_min_rate, rate_queue_ns; docs/DESIGN.md "Send pacing"), through the
// mock HAL and tt_Context_create_with(), built as rmw_tickle builds core (large samples):
//   - the zero value keeps what pacing_rate always meant: a program that sets only pacing_rate is FIXED at it, T1S's
//     profile still paces at 75 % of its link_rate, the other profiles still do not pace, tt_PACING_OFF still turns
//     pacing off - and rate_control holds the concrete mode after resolution;
//   - FIXED without a pacing_rate, a value past tt_RATE_CONTROL_ADAPTIVE, the adaptive tunables on a link that is not
//     ADAPTIVE, ADAPTIVE with pacing off, a floor above the cap or below tt_LINK_RATE_MIN and a queue target below
//     tt_RATE_QUEUE_MIN_NS are refused with tt_RET_INVALID_ARGUMENT, and each bound itself is accepted;
//   - ADAPTIVE's defaults: the cap is link_rate, the floor tt_LINK_RATE_MIN, the burst four frames.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1 // as rmw_tickle builds core

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox

#define LINK_BROADCAST "10.0.0.255"
#define T1S_RATE 10000000ULL
#define T1S_PACED (T1S_RATE / 100U * tt_LINK_T1S_PACING_PCT)

static struct tt_Context node;

static struct tt_InitOptions one_link(uint8_t profile) {
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = LINK_BROADCAST, .profile = profile};
    opts.net.link_count = 1;
    return opts;
}

static void mock_setup(void) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    test_mock_link_mtu = -1;
}

static void create(const struct tt_InitOptions* opts) {
    mock_setup();
    memset(&node, 0, sizeof(node));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&node, opts));
}

static void destroy(void) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
}

static void expect_created(const struct tt_InitOptions* opts, tt_ret_t expected, const char* what) {
    mock_setup();
    struct tt_Context created;
    memset(&created, 0, sizeof(created));
    tt_ret_t ret = tt_Context_create_with(&created, opts);
    if (ret != expected) {
        printf("test_rate_control: %s: expected %d, got %d\n", what, (int)expected, (int)ret);
    }
    EXPECT_EQ_INT(expected, ret);
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));
    }
}

// ---- The zero value is what pacing_rate says.

static void test_zero_keeps_what_pacing_rate_meant(void) {
    static const uint8_t unpaced[] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_TSN, tt_LINK_PROFILE_WIFI};
    for (size_t i = 0; i < sizeof(unpaced); i++) {
        struct tt_InitOptions opts = one_link(unpaced[i]);
        create(&opts);
        EXPECT_EQ_U32(tt_RATE_CONTROL_OFF, node.opts.net.links[0].rate_control);
        EXPECT_EQ_U64(tt_PACING_OFF, node.opts.net.links[0].pacing_rate);
        EXPECT_TRUE(!node.pacing);
        destroy();
    }
    struct tt_InitOptions t1s = one_link(tt_LINK_PROFILE_T1S);
    create(&t1s);
    EXPECT_EQ_U32(tt_RATE_CONTROL_FIXED, node.opts.net.links[0].rate_control);
    EXPECT_EQ_U64(T1S_PACED, node.opts.net.links[0].pacing_rate);
    EXPECT_TRUE(node.pacing);
    destroy();

    // Only pacing_rate set, on a profile that does not pace by default: FIXED at it, as before.
    struct tt_InitOptions eth = one_link(tt_LINK_PROFILE_WIFI);
    eth.net.links[0].pacing_rate = 5000000ULL;
    create(&eth);
    EXPECT_EQ_U32(tt_RATE_CONTROL_FIXED, node.opts.net.links[0].rate_control);
    EXPECT_EQ_U64(5000000ULL, node.opts.net.links[0].pacing_rate);
    EXPECT_TRUE(node.pacing);
    destroy();

    // tt_PACING_OFF still turns T1S's pacing off.
    t1s.net.links[0].pacing_rate = tt_PACING_OFF;
    create(&t1s);
    EXPECT_EQ_U32(tt_RATE_CONTROL_OFF, node.opts.net.links[0].rate_control);
    EXPECT_TRUE(!node.pacing);
    destroy();

    // An explicit FIXED with a rate, or with T1S's own, is the same as leaving it zero.
    struct tt_InitOptions fixed = one_link(tt_LINK_PROFILE_T1S);
    fixed.net.links[0].rate_control = tt_RATE_CONTROL_FIXED;
    create(&fixed);
    EXPECT_EQ_U32(tt_RATE_CONTROL_FIXED, node.opts.net.links[0].rate_control);
    EXPECT_EQ_U64(T1S_PACED, node.opts.net.links[0].pacing_rate);
    destroy();
}

// ---- ADAPTIVE's defaults.

static void test_adaptive_defaults(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    create(&opts);
    const struct tt_LinkOptions* link = &node.opts.net.links[0];
    EXPECT_EQ_U32(tt_RATE_CONTROL_ADAPTIVE, link->rate_control);
    EXPECT_EQ_U64(link->link_rate, link->pacing_rate); // the cap
    EXPECT_EQ_U64(tt_LINK_RATE_MIN, link->rate_min_rate);
    EXPECT_EQ_U32(tt_PACING_BURST_FRAMES * (1500U + 38U), link->pacing_burst);
    EXPECT_TRUE(node.pacing);
    destroy();

    // The cap, the floor and the queue target as given.
    opts.net.links[0].link_rate = 100000000ULL;
    opts.net.links[0].pacing_rate = 50000000ULL;
    opts.net.links[0].rate_min_rate = 2000000ULL;
    opts.net.links[0].rate_queue_ns = 4000000U;
    create(&opts);
    EXPECT_EQ_U64(50000000ULL, node.opts.net.links[0].pacing_rate);
    EXPECT_EQ_U64(2000000ULL, node.opts.net.links[0].rate_min_rate);
    EXPECT_EQ_U32(4000000U, node.opts.net.links[0].rate_queue_ns);
    destroy();
}

// ---- Each check refuses the value past its bound and accepts the bound itself.

static void test_validation(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "rate_control past ADAPTIVE");
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    expect_created(&opts, tt_RET_OK, "rate_control ADAPTIVE");

    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_FIXED;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "FIXED on a link with no pacing_rate");
    opts.net.links[0].pacing_rate = 1000000ULL;
    expect_created(&opts, tt_RET_OK, "FIXED with a pacing_rate");
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_FIXED;
    expect_created(&opts, tt_RET_OK, "FIXED on T1S, which paces by its profile");
    opts.net.links[0].pacing_rate = tt_PACING_OFF;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "FIXED with pacing_rate tt_PACING_OFF");

    // The adaptive tunables only on an ADAPTIVE link.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_min_rate = 1000000ULL;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "rate_min_rate on a link that is not ADAPTIVE");
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_queue_ns = 1000000U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "rate_queue_ns on a link that is not ADAPTIVE");
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].rate_min_rate = 1000000ULL;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "rate_min_rate on a FIXED link");

    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].pacing_rate = tt_PACING_OFF;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "ADAPTIVE with pacing off");

    // The cap: at least tt_LINK_RATE_MIN, at most link_rate.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].link_rate = 100000000ULL;
    opts.net.links[0].pacing_rate = 100000000ULL;
    expect_created(&opts, tt_RET_OK, "cap at link_rate");
    opts.net.links[0].pacing_rate = 100000001ULL;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "cap above link_rate");
    opts.net.links[0].pacing_rate = tt_LINK_RATE_MIN;
    expect_created(&opts, tt_RET_OK, "cap at tt_LINK_RATE_MIN");
    opts.net.links[0].pacing_rate = tt_LINK_RATE_MIN - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "cap below tt_LINK_RATE_MIN");

    // The floor: at least tt_LINK_RATE_MIN, at most the cap.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].link_rate = 100000000ULL;
    opts.net.links[0].pacing_rate = 20000000ULL;
    opts.net.links[0].rate_min_rate = 20000000ULL;
    expect_created(&opts, tt_RET_OK, "floor at the cap");
    opts.net.links[0].rate_min_rate = 20000001ULL;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "floor above the cap");
    opts.net.links[0].rate_min_rate = tt_LINK_RATE_MIN;
    expect_created(&opts, tt_RET_OK, "floor at tt_LINK_RATE_MIN");

    // The queue target: at least tt_RATE_QUEUE_MIN_NS.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].rate_queue_ns = tt_RATE_QUEUE_MIN_NS;
    expect_created(&opts, tt_RET_OK, "queue target at tt_RATE_QUEUE_MIN_NS");
    opts.net.links[0].rate_queue_ns = tt_RATE_QUEUE_MIN_NS - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "queue target below tt_RATE_QUEUE_MIN_NS");

    // The burst keeps its bound on an ADAPTIVE link.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].pacing_burst = tt_PACING_BURST_MIN - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "pacing_burst below tt_PACING_BURST_MIN on ADAPTIVE");
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_zero_keeps_what_pacing_rate_meant();
    test_adaptive_defaults();
    test_validation();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_rate_control: all tests passed\n");
    return 0;
}
