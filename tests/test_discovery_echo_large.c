/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The large-sample half of the announce round trip (tests/test_peer_discovery.c has the rest): an echo that spans the
// peer sending us a large sample is not timed (note_discovery_echo(), tt_Context.peer_large_ns). That field exists only
// in a large-sample build, which test_peer_discovery.c is not (its other tests were written for the default one and do
// not all run under this), so this file defines the build as test_large_sample.c does and carries what the check needs.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1 // as rmw_tickle builds core

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: process_data(), build_and_send_update() and the announce state are static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1

static void init_node(struct tt_Context* node) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
}

static void init_header(struct tt_Header* header, uint8_t source) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = source;
}

// An announce of `last_modified` with these marks (the tag in the low byte, the echo in the high one), no endpoints.
static uint32_t write_update_no_entities_marked(uint8_t* buf, uint64_t last_modified, uint16_t marks) {
    struct test_announce* update_header = test_announce_at(buf);
    test_announce_set_last_modified(update_header, last_modified);
    update_header->announce.entity_count = 0;
    update_header->announce.marks = marks;
    return sizeof(struct test_announce);
}

#define MARKS(tag, echo) ((uint16_t)((uint16_t)(tag) | ((uint16_t)(echo) << 8)))

// `source`'s announce of `generation` arrives on the data socket with these marks.
static void deliver_marked(struct tt_Context* node, uint8_t source, uint32_t generation, uint16_t marks) {
    struct tt_Header header;
    init_header(&header, source);
    node->rx_via_data_port = true;
    uint32_t tail = write_update_no_entities_marked(node->rx_buffer, generation, marks);
    EXPECT_TRUE(process_data(node, &header, node->rx_buffer, 0, tail, 0x0a000000U + source, 8282));
}

// This node's announce, broadcast, built and sent now as if the flush tick came at once. Returns its tag.
static uint8_t announce_now(struct tt_Context* node) {
    EXPECT_TRUE(build_and_send_update(node, NULL, 0));
    flush_pending_broadcast(node);
    return node->announce_tag_out;
}

// A peer that is sending us a large sample answers when its send ends: a reply that came while its original fragments
// were arriving says how long it was busy, not how long the path takes, and is not timed. One whose fragments came
// before the announce was built is (the queue was gone), and so is any other peer's.
static void test_an_echo_spanning_a_peers_large_send_times_nothing(void) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    static struct tt_Context node;
    init_node(&node);
    const uint64_t sent = test_mock_now;
    const uint8_t tag = announce_now(&node);
    node.peer_large_ns[3] = sent + (50 * tt_MILLISECOND); // arriving while the announce was out
    node.peer_large_ns[4] = sent;                         // from the announce's own instant
    node.peer_large_ns[5] = sent - tt_MILLISECOND;        // over before it was built
    test_mock_now = sent + (200 * tt_MILLISECOND);
    deliver_marked(&node, 3, 7, MARKS(5, tag));
    deliver_marked(&node, 4, 7, MARKS(6, tag));
    deliver_marked(&node, 5, 7, MARKS(7, tag));
    deliver_marked(&node, 6, 7, MARKS(8, tag)); // never sent a large sample
    EXPECT_EQ_U32(0, node.announce_rtt_ns[3]);
    EXPECT_EQ_U32(0, node.announce_rtt_ns[4]);
    EXPECT_EQ_U32(200 * tt_MILLISECOND, node.announce_rtt_ns[5]);
    EXPECT_EQ_U32(200 * tt_MILLISECOND, node.announce_rtt_ns[6]);
}

int main(void) {
    test_an_echo_spanning_a_peers_large_send_times_nothing();
    return test_result();
}
