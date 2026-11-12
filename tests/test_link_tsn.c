/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// TSN links and send windows (network profiles phase 2; docs/DESIGN.md section 2 "Send windows"), through the mock HAL
// and tt_Context_create_with():
//   - a TSN link gives the DATA class its priority (tt_LINK_TSN_DATA_PRIORITY) where socket.tx_class[DATA].priority is
//     0, and leaves REPAIR and CONTROL at 0 (best effort); an ETHERNET context sets no class; an explicit priority
//     wins;
//   - a link's window turns SO_TXTIME on (socket.txtime DEFAULT, or OFF as tt_InitOptions_default() resolves it; ON
//     stays ON), fills its lead (tt_LINK_WINDOW_LEAD_NS), and is the context's tx_window, at the longest lead of the
//     links that agree on it; without one no window and txtime stays OFF;
//   - every check refuses the value past its bound and accepts the bound itself.
// The HAL's use of the window (each datagram's time) is tests/test_socket_marks.c's; tests/mutants_link_tsn.py removes
// each check and each reader here and expects this test to fail.

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_link_tsn.py passes a mutant

#define LINK_BROADCAST "10.0.0.255"
#define CYCLE_NS 1000000U // 1 ms
#define OFFSET_NS 200000U
#define LENGTH_NS 300000U
#define LEAD_NS 50000U
#define EXPLICIT_PRIORITY 3U

static struct tt_Context node;

static void reset_mock_for_create(void) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    test_mock_link_mtu = -1;
}

static struct tt_InitOptions one_link(uint8_t profile) {
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = LINK_BROADCAST, .profile = profile};
    opts.net.link_count = 1;
    return opts;
}

static void set_window(struct tt_LinkOptions* link, uint32_t cycle, uint32_t offset, uint32_t length, uint32_t lead) {
    link->window_cycle_ns = cycle;
    link->window_offset_ns = offset;
    link->window_length_ns = length;
    link->window_lead_ns = lead;
}

static void create(const struct tt_InitOptions* opts) {
    reset_mock_for_create();
    memset(&node, 0, sizeof(node));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&node, opts));
}

static void destroy(void) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
}

static void expect_classes(uint32_t data, uint32_t repair, uint32_t control) {
    EXPECT_EQ_U32(data, node.opts.socket.tx_class[tt_TX_CLASS_DATA].priority);
    EXPECT_EQ_U32(repair, node.opts.socket.tx_class[tt_TX_CLASS_REPAIR].priority);
    EXPECT_EQ_U32(control, node.opts.socket.tx_class[tt_TX_CLASS_CONTROL].priority);
    for (uint32_t cls = 0; cls < tt_TX_CLASS_COUNT; cls++) {
        EXPECT_EQ_U32(0, node.opts.socket.tx_class[cls].dscp); // no profile sets a DSCP
    }
}

static void expect_no_window(void) {
    EXPECT_EQ_U32(0, node.tx_window.cycle_ns);
    EXPECT_EQ_U32(0, node.tx_window.offset_ns);
    EXPECT_EQ_U32(0, node.tx_window.length_ns);
    EXPECT_EQ_U32(0, node.tx_window.lead_ns);
}

// ---- The DATA class's priority.

static void test_tsn_gives_data_its_class(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_TSN);
    create(&opts);
    expect_classes(tt_LINK_TSN_DATA_PRIORITY, 0, 0);
    EXPECT_EQ_U32(tt_SWITCH_OFF, node.opts.socket.txtime); // no window, no send time
    expect_no_window();
    destroy();

    // ETHERNET, T1S and WIFI set no class; nor does a context without links.
    const uint8_t others[] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_T1S, tt_LINK_PROFILE_WIFI};
    for (size_t i = 0; i < sizeof(others); i++) {
        opts = one_link(others[i]);
        create(&opts);
        expect_classes(0, 0, 0);
        EXPECT_EQ_U32(tt_SWITCH_OFF, node.opts.socket.txtime);
        destroy();
    }
    create(NULL);
    expect_classes(0, 0, 0);
    expect_no_window();
    destroy();
    // A caller's context is not zeroed for it (check_context_reset.py): no window from what was there before.
    reset_mock_for_create();
    memset(&node, 0xAA, sizeof(node));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&node, NULL));
    expect_no_window();
    destroy();

    // An explicit DATA priority wins; the other classes' explicit values are the caller's too.
    opts = one_link(tt_LINK_PROFILE_TSN);
    opts.socket.tx_class[tt_TX_CLASS_DATA].priority = EXPLICIT_PRIORITY;
    opts.socket.tx_class[tt_TX_CLASS_CONTROL].priority = 1;
    create(&opts);
    expect_classes(EXPLICIT_PRIORITY, 0, 1);
    destroy();

    // One TSN link beside an ETHERNET one: the context's one socket takes the class.
    opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .profile = tt_LINK_PROFILE_TSN};
    opts.net.link_count = 2;
    create(&opts);
    expect_classes(tt_LINK_TSN_DATA_PRIORITY, 0, 0);
    destroy();

    // From tt_InitOptions_default(), whose classes are resolved 0.
    tt_InitOptions_default(&opts);
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = LINK_BROADCAST, .profile = tt_LINK_PROFILE_TSN};
    opts.net.link_count = 1;
    create(&opts);
    expect_classes(tt_LINK_TSN_DATA_PRIORITY, 0, 0);
    destroy();
}

// ---- The send window.

static void test_a_window_is_the_contexts(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_TSN);
    set_window(&opts.net.links[0], CYCLE_NS, OFFSET_NS, LENGTH_NS, 0);
    create(&opts);
    EXPECT_EQ_U32(tt_SWITCH_ON, node.opts.socket.txtime);
    EXPECT_EQ_U32(CYCLE_NS, node.tx_window.cycle_ns);
    EXPECT_EQ_U32(OFFSET_NS, node.tx_window.offset_ns);
    EXPECT_EQ_U32(LENGTH_NS, node.tx_window.length_ns);
    EXPECT_EQ_U32(tt_LINK_WINDOW_LEAD_NS, node.tx_window.lead_ns);
    EXPECT_EQ_U32(tt_LINK_WINDOW_LEAD_NS, node.opts.net.links[0].window_lead_ns);
    destroy();

    // An explicit lead; txtime from tt_InitOptions_default() (OFF, its default) and explicitly ON both end ON.
    tt_InitOptions_default(&opts);
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = LINK_BROADCAST, .profile = tt_LINK_PROFILE_TSN};
    opts.net.link_count = 1;
    set_window(&opts.net.links[0], CYCLE_NS, OFFSET_NS, LENGTH_NS, LEAD_NS);
    EXPECT_EQ_U32(tt_SWITCH_OFF, opts.socket.txtime);
    create(&opts);
    EXPECT_EQ_U32(tt_SWITCH_ON, node.opts.socket.txtime);
    EXPECT_EQ_U32(LEAD_NS, node.tx_window.lead_ns);
    destroy();
    opts.socket.txtime = tt_SWITCH_ON;
    create(&opts);
    EXPECT_EQ_U32(tt_SWITCH_ON, node.opts.socket.txtime);
    destroy();

    // A window on an ETHERNET link: the window and the send time, and no class.
    opts = one_link(tt_LINK_PROFILE_ETHERNET);
    set_window(&opts.net.links[0], CYCLE_NS, 0, CYCLE_NS, 0);
    create(&opts);
    expect_classes(0, 0, 0);
    EXPECT_EQ_U32(tt_SWITCH_ON, node.opts.socket.txtime);
    EXPECT_EQ_U32(CYCLE_NS, node.tx_window.length_ns);
    destroy();

    // Two links with the same window: the longer lead. A third without one changes nothing.
    opts = one_link(tt_LINK_PROFILE_TSN);
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .profile = tt_LINK_PROFILE_TSN};
    opts.net.links[2] = (struct tt_LinkOptions) {.broadcast = "10.0.2.255", .profile = tt_LINK_PROFILE_ETHERNET};
    opts.net.link_count = 3;
    set_window(&opts.net.links[0], CYCLE_NS, OFFSET_NS, LENGTH_NS, 2 * LEAD_NS);
    set_window(&opts.net.links[1], CYCLE_NS, OFFSET_NS, LENGTH_NS, LEAD_NS);
    create(&opts);
    EXPECT_EQ_U32(2 * LEAD_NS, node.tx_window.lead_ns);
    EXPECT_EQ_U32(OFFSET_NS, node.tx_window.offset_ns);
    destroy();
    set_window(&opts.net.links[0], CYCLE_NS, OFFSET_NS, LENGTH_NS, LEAD_NS);
    set_window(&opts.net.links[1], CYCLE_NS, OFFSET_NS, LENGTH_NS, 2 * LEAD_NS);
    create(&opts);
    EXPECT_EQ_U32(2 * LEAD_NS, node.tx_window.lead_ns);
    destroy();
    // Only the second link has one.
    set_window(&opts.net.links[0], 0, 0, 0, 0);
    create(&opts);
    EXPECT_EQ_U32(CYCLE_NS, node.tx_window.cycle_ns);
    EXPECT_EQ_U32(2 * LEAD_NS, node.tx_window.lead_ns);
    destroy();
}

// ---- The checks.

static void expect_created(const struct tt_InitOptions* opts, tt_ret_t expected, const char* what) {
    reset_mock_for_create();
    struct tt_Context created;
    memset(&created, 0, sizeof(created));
    tt_ret_t ret = tt_Context_create_with(&created, opts);
    if (ret != expected) {
        fprintf(stderr, "%s: returned %d, expected %d\n", what, (int)ret, (int)expected);
        test_failures++;
    }
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));
    }
}

static void test_validation(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_TSN);
    struct tt_LinkOptions* link = &opts.net.links[0];
    // Without a cycle, nothing else.
    set_window(link, 0, 1, 0, 0);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "offset without a cycle");
    set_window(link, 0, 0, 1, 0);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "length without a cycle");
    set_window(link, 0, 0, 0, 1);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "lead without a cycle");
    set_window(link, 0, 0, 0, 0);
    expect_created(&opts, tt_RET_OK, "no window");
    // The offset below the cycle.
    set_window(link, CYCLE_NS, CYCLE_NS, 1, 0);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "offset at the cycle");
    set_window(link, CYCLE_NS, CYCLE_NS + 1U, 1, 0); // past it: the length's check alone would wrap below 0
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "offset past the cycle");
    set_window(link, CYCLE_NS, CYCLE_NS - 1U, 1, 0);
    expect_created(&opts, tt_RET_OK, "offset just below the cycle, length 1");
    // The length at least 1, and within the cycle with the offset.
    set_window(link, CYCLE_NS, OFFSET_NS, 0, 0);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "length 0");
    set_window(link, CYCLE_NS, OFFSET_NS, CYCLE_NS - OFFSET_NS, 0);
    expect_created(&opts, tt_RET_OK, "offset + length at the cycle");
    set_window(link, CYCLE_NS, OFFSET_NS, CYCLE_NS - OFFSET_NS + 1U, 0);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "offset + length past the cycle");
    set_window(link, CYCLE_NS, 0, CYCLE_NS, 0);
    expect_created(&opts, tt_RET_OK, "the whole cycle");
    set_window(link, UINT32_MAX, UINT32_MAX - 1U, 1, UINT32_MAX);
    expect_created(&opts, tt_RET_OK, "the largest cycle, offset and lead");

    // Two links: the same cycle, offset and length, or refused. The lead may differ.
    opts = one_link(tt_LINK_PROFILE_TSN);
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .profile = tt_LINK_PROFILE_TSN};
    opts.net.link_count = 2;
    set_window(&opts.net.links[0], CYCLE_NS, OFFSET_NS, LENGTH_NS, LEAD_NS);
    set_window(&opts.net.links[1], CYCLE_NS, OFFSET_NS, LENGTH_NS, 2 * LEAD_NS);
    expect_created(&opts, tt_RET_OK, "two links, one window, two leads");
    set_window(&opts.net.links[1], 2 * CYCLE_NS, OFFSET_NS, LENGTH_NS, 0);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "two links, two cycles");
    set_window(&opts.net.links[1], CYCLE_NS, OFFSET_NS + 1U, LENGTH_NS, 0);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "two links, two offsets");
    set_window(&opts.net.links[1], CYCLE_NS, OFFSET_NS, LENGTH_NS + 1U, 0);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "two links, two lengths");
    set_window(&opts.net.links[1], 0, 0, 0, 0);
    expect_created(&opts, tt_RET_OK, "two links, one with a window");
    // The disagreement is found whichever link comes first, and past a link without one.
    opts.net.links[2] = (struct tt_LinkOptions) {.broadcast = "10.0.2.255", .profile = tt_LINK_PROFILE_TSN};
    opts.net.link_count = 3;
    set_window(&opts.net.links[2], CYCLE_NS, 0, LENGTH_NS, 0);
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "three links, the third's window differs");
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_tsn_gives_data_its_class();
    test_a_window_is_the_contexts();
    test_validation();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_link_tsn: all tests passed\n");
    return 0;
}
