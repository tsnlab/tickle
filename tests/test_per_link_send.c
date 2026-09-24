/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: flush_tx() and the link helpers are static, and the whole point of these tests is the
// decision those make, not a return code any public call would report.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

// The unicast-or-broadcast decision is made per link, not across the combined peer set (A3). What
// that buys is the case one global threshold cannot express: five subscribers on a slow shared
// segment want one broadcast while one subscriber on a fast link wants a unicast, and a count
// across both picks wrong for one of them. These tests pin that, and pin that a single-link node -
// which is every deployment that has not configured links[] - is unaffected.

#define LINK_A_ADDR 0xc0a80a02    // 192.168.10.2
#define LINK_A_NETMASK 0xffffff00 // /24
#define LINK_A_BCAST 0xc0a80aff   // 192.168.10.255
#define LINK_B_ADDR 0x0a010102    // 10.1.1.2
#define LINK_B_NETMASK 0xffff0000 // /16
#define LINK_B_BCAST 0x0a01ffff   // 10.1.255.255

// flush_tx() sends the first `len` bytes and then shifts whatever is left in the buffer down, so
// tx_tail has to be at least len or that shift runs backwards - node->tx_tail - len underflows and
// memmove copies gigabytes. Found by segfault rather than by reading, which is fair: a caller that
// asks to flush more than it has written is not a state the real code can reach.
#define FLUSH_LEN (sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader))

static void init_node(struct tt_Node* node) {
    memset(node, 0, sizeof(*node));
    node->id = 1;
    node->tx_tail = FLUSH_LEN;
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
}

// Two links, resolved as the OS would have resolved them. Set directly rather than through
// resolve_links(), because the mock HAL cannot describe two different interfaces - and what is
// under test is the decision made from a resolved table, not the resolving.
static void configure_two_links(uint8_t threshold_a, uint8_t threshold_b) {
    memset(_tt_CONFIG.links, 0, sizeof(_tt_CONFIG.links));
    _tt_CONFIG.links[0] = (struct _tt_Link) {.broadcast = "192.168.10.255",
                                             .unicast_threshold = threshold_a,
                                             .resolved_addr = LINK_A_ADDR,
                                             .resolved_netmask = LINK_A_NETMASK,
                                             .resolved_broadcast = LINK_A_BCAST,
                                             .resolved = true};
    _tt_CONFIG.links[1] = (struct _tt_Link) {.broadcast = "10.1.255.255",
                                             .unicast_threshold = threshold_b,
                                             .resolved_addr = LINK_B_ADDR,
                                             .resolved_netmask = LINK_B_NETMASK,
                                             .resolved_broadcast = LINK_B_BCAST,
                                             .resolved = true};
    _tt_CONFIG.link_count = 2;
}

static void configure_no_links(void) {
    memset(_tt_CONFIG.links, 0, sizeof(_tt_CONFIG.links));
    _tt_CONFIG.link_count = 0;
}

static bool sent_to(uint32_t ip) {
    for (int i = 0; i < test_mock_send_to_ip_count; i++) {
        if (test_mock_send_to_ips[i] == ip) {
            return true;
        }
    }
    return false;
}

// A peer is matched to a link by subnet, and a limited-broadcast link (one the OS does not own)
// is the catch-all. This is what makes the compiled-in 255.255.255.255 default keep working.
static void test_peer_is_matched_to_its_own_link(void) {
    test_mock_reset();
    configure_two_links(2, 2);

    EXPECT_EQ_U32(0, (uint32_t)link_of_ip(0xc0a80a07)); // 192.168.10.7 -> link A
    EXPECT_EQ_U32(1, (uint32_t)link_of_ip(0x0a010207)); // 10.1.2.7     -> link B
    // On neither subnet, and no catch-all configured: falls back to link 0 rather than to nothing,
    // so a peer always names a link and no send can silently address zero destinations.
    EXPECT_EQ_U32(0, (uint32_t)link_of_ip(0x08080808));

    // An unresolved link is the catch-all for anything the resolved ones do not claim.
    _tt_CONFIG.links[1].resolved = false;
    EXPECT_EQ_U32(1, (uint32_t)link_of_ip(0x08080808));
    EXPECT_EQ_U32(0, (uint32_t)link_of_ip(0xc0a80a07)); // still claimed by the resolved link
}

// The case a single global threshold cannot express: the same buffer, unicast on the link whose
// peer count is under its threshold and broadcast on the link whose count is over its own.
static void test_each_link_decides_on_its_own_count_and_threshold(void) {
    test_mock_reset();
    configure_two_links(/*threshold_a=*/1, /*threshold_b=*/4);

    struct tt_Node node;
    init_node(&node);

    // Link A: two peers against a threshold of one -> one broadcast, not two unicasts.
    // Link B: three peers against a threshold of four -> three unicasts, not a broadcast.
    struct tt_Peer peers[] = {
        {.node_id = 2, .ip = 0xc0a80a07, .port = 8282}, {.node_id = 3, .ip = 0xc0a80a08, .port = 8282},
        {.node_id = 4, .ip = 0x0a010207, .port = 8282}, {.node_id = 5, .ip = 0x0a010208, .port = 8282},
        {.node_id = 6, .ip = 0x0a010209, .port = 8282},
    };

    EXPECT_TRUE(flush_tx(&node, FLUSH_LEN, peers, 5));

    EXPECT_EQ_U32(4, (uint32_t)test_mock_send_to_ip_count); // 1 broadcast + 3 unicasts
    EXPECT_TRUE(sent_to(LINK_A_BCAST));
    EXPECT_TRUE(!sent_to(0xc0a80a07)); // A's peers were not unicast to
    EXPECT_TRUE(!sent_to(0xc0a80a08));
    EXPECT_TRUE(sent_to(0x0a010207)); // B's were
    EXPECT_TRUE(sent_to(0x0a010208));
    EXPECT_TRUE(sent_to(0x0a010209));
    EXPECT_TRUE(!sent_to(LINK_B_BCAST)); // and B did not also broadcast
}

// A link nobody is known on gets nothing from an addressed buffer. The buffer is for known peers,
// and a link with none has no one to receive it - broadcasting there would put this node's data on
// a segment that has not asked for it.
static void test_link_with_no_peers_is_not_broadcast_to(void) {
    test_mock_reset();
    configure_two_links(2, 2);

    struct tt_Node node;
    init_node(&node);

    struct tt_Peer peers[] = {{.node_id = 2, .ip = 0xc0a80a07, .port = 8282}};

    EXPECT_TRUE(flush_tx(&node, FLUSH_LEN, peers, 1));

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_ip_count);
    EXPECT_TRUE(sent_to(0xc0a80a07));
    EXPECT_TRUE(!sent_to(LINK_B_BCAST));
}

// With nothing addressable, a multi-link node announces on every link - that is how it is
// discovered at all, and its peers may be on any of them.
static void test_unaddressed_buffer_broadcasts_on_every_link(void) {
    test_mock_reset();
    configure_two_links(2, 2);

    struct tt_Node node;
    init_node(&node);

    EXPECT_TRUE(flush_tx(&node, FLUSH_LEN, NULL, 0));

    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_to_ip_count);
    EXPECT_TRUE(sent_to(LINK_A_BCAST));
    EXPECT_TRUE(sent_to(LINK_B_BCAST));
}

// A node that has configured nothing - which is every deployment today - must send exactly as it
// did before per-link existed: one tt_send() through the HAL's precomputed broadcast address, not
// a tt_send_to() that happens to be equivalent. test_mock_send_call_count counts both; only
// test_mock_send_to_call_count distinguishes them.
static void test_single_link_node_still_uses_plain_broadcast(void) {
    test_mock_reset();
    configure_no_links();

    struct tt_Node node;
    init_node(&node);

    EXPECT_TRUE(flush_tx(&node, FLUSH_LEN, NULL, 0));

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);
    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
    // And the table was populated on first use rather than left empty: an empty table read a
    // threshold of 0, which made a single known peer fail `on_link <= threshold` and fall through
    // to broadcast. A default that is never written is indistinguishable from a configured zero.
    EXPECT_EQ_U32(1, (uint32_t)_tt_CONFIG.link_count);
    EXPECT_EQ_U32((uint32_t)tt_UNICAST_PEER_THRESHOLD, (uint32_t)_tt_CONFIG.links[0].unicast_threshold);
}

int main(void) {
    test_peer_is_matched_to_its_own_link();
    test_each_link_decides_on_its_own_count_and_threshold();
    test_link_with_no_peers_is_not_broadcast_to();
    test_unaddressed_buffer_broadcasts_on_every_link();
    test_single_link_node_still_uses_plain_broadcast();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_per_link_send: all tests passed\n");
    return 0;
}
