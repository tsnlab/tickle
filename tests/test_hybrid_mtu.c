/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Hybrid transport (DESIGN.md section 8) in a core-default build - a 1472-byte datagram, no fragmentation, no stage 2,
// as an MCU or FreeRTOS builds core: it advertises no more than one datagram (so no writer ever sends it a run), it
// still reads a peer's announce of the new version, and it passes runs over and carries on.
// test_hybrid_capability.c is the rmw_tickle-shaped build.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: process_packet() is static
#include "test_mock.h"

_Static_assert(tt_MAX_BUFFER_LENGTH == 1472, "this test is about core defaults");
_Static_assert(!tt_FRAG_ENABLED, "this test is about a build without fragmentation");

static struct tt_Context node;
static int delivered;

static int32_t decode_any(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
    delivered++;
}

// One single-submessage datagram from node 1 of `type`, its body `body_len` bytes of `body`.
static uint32_t datagram(uint8_t* out, uint8_t type, const void* body, uint32_t body_len) {
    uint16_t magic = NATIVE_MAGIC_VALUE;
    uint8_t first = 0;
    memcpy(&first, &magic, 1);
    struct tt_SingleHeader single = {(uint8_t)(first | tt_SINGLE_MARKER_FLAG), tt_VERSION, 1, type};
    memcpy(out, &single, sizeof(single));
    memcpy(out + sizeof(single), body, body_len);
    return (uint32_t)sizeof(single) + body_len;
}

static struct tt_Subscriber sub;
static struct tt_Topic topic;

static void init_node(void) {
    test_mock_reset();
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    reset_node_state(&node);
    node.id = 2;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = sizeof(node.tx_buffer);
    memset(&topic, 0, sizeof(topic));
    topic.name = "t";
    topic.data_size = 8;
    topic.data_decode = decode_any;
    topic.data_free = free_nothing;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&node, &sub, &topic, "e", on_data));
    delivered = 0;
}

// It advertises its receive buffer, one unit: the Linux setting (65507) capped at 1472.
static void test_advertises_one_datagram(void) {
    init_node();
    EXPECT_EQ_U32(65507, _tt_CONFIG.ip_reassembly_max);
    EXPECT_EQ_U32(1472, node.ip_reassembly_max);
}

// A peer's announce of this version, with a reassembly ceiling, is read and applied as any announce.
static void test_reads_a_peer_announce(void) {
    init_node();
    struct {
        struct tt_DataHeader data;
        struct tt_AnnounceHeader announce;
    } __attribute__((packed)) body;
    memset(&body, 0, sizeof(body));
    body.data.endpoint_id = tt_DISCOVERY_ENDPOINT_ID;
    body.data.entity_id = tt_DISCOVERY_ENTITY_ID;
    body.data.seq_no = 42;
    body.announce.ip_reassembly_max = 65507;
    uint8_t bytes[64];
    uint32_t len = datagram(bytes, tt_SUBMESSAGE_TYPE_DATA, &body, sizeof(body));
    EXPECT_TRUE(process_packet(&node, bytes, 0, len, 0x0a000001, 8282, tt_TRANSPORT_UDP));
    EXPECT_TRUE(node.update_seen[1]);
    EXPECT_EQ_U32(42, node.update_generation[1]);
}

// Runs of either type are passed over and counted, and the stream carries on.
static void test_runs_are_skipped(void) {
    init_node();
    uint8_t bytes[256];
    struct {
        struct tt_FragRunHeader header;
        uint8_t payload[64];
    } __attribute__((packed)) run;
    memset(&run, 0x11, sizeof(run));
    run.header.data.endpoint_id = sub.endpoint.id;
    run.header.data.seq_no = 1;
    run.header.data.entity_id = 7;
    run.header.frag_index = 0;
    run.header.frag_count = 3;
    run.header.run_count = 2;
    run.header.zero = 0;
    uint32_t len = datagram(bytes, tt_SUBMESSAGE_TYPE_FRAG_RUN, &run, sizeof(run));
    EXPECT_TRUE(process_packet(&node, bytes, 0, len, 0x0a000001, 8282, tt_TRANSPORT_UDP));
    struct {
        struct tt_FragRunLHeader header;
        uint8_t payload[64];
    } __attribute__((packed)) run_l;
    memset(&run_l, 0x22, sizeof(run_l));
    run_l.header.data.endpoint_id = sub.endpoint.id;
    run_l.header.data.seq_no = 1;
    run_l.header.data.entity_id = 7;
    run_l.header.frag_index = 0;
    run_l.header.frag_count = 723;
    run_l.header.run_count = 45;
    run_l.header.zero = 0;
    len = datagram(bytes, tt_SUBMESSAGE_TYPE_FRAG_RUN_L, &run_l, sizeof(run_l));
    EXPECT_TRUE(process_packet(&node, bytes, 0, len, 0x0a000001, 8282, tt_TRANSPORT_UDP));
    EXPECT_EQ_U64(2, node.frag_large_skipped);
    EXPECT_EQ_U64(0, node.rx_malformed_drops); // passed over, not refused as malformed
    EXPECT_EQ_INT(0, delivered);

    // Control: a small DATA from the same node right after is delivered - nothing stopped.
    struct {
        struct tt_DataHeader header;
        uint8_t payload[8];
    } __attribute__((packed)) data;
    memset(&data, 0, sizeof(data));
    data.header.endpoint_id = sub.endpoint.id;
    data.header.seq_no = 1;
    data.header.entity_id = 9;
    len = datagram(bytes, tt_SUBMESSAGE_TYPE_DATA, &data, sizeof(data));
    EXPECT_TRUE(process_packet(&node, bytes, 0, len, 0x0a000001, 8282, tt_TRANSPORT_UDP));
    EXPECT_EQ_INT(1, delivered);
}

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    test_advertises_one_datagram();
    test_reads_a_peer_announce();
    test_runs_are_skipped();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_hybrid_mtu: all tests passed\n");
    return 0;
}
