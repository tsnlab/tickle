/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A node built without large-message stage 2 (core defaults, as FreeRTOS builds it) passes a large sample's fragments
// over and counts them (DESIGN.md section 8: frag_large_skipped), and the rest of the datagram stream carries on.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: process_packet() is static
#include "test_mock.h"

_Static_assert(!tt_LARGE_SAMPLES, "this test is about a build without stage 2");

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

static void test_large_fragments_are_skipped_and_counted(void) {
    test_mock_reset();
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = 2;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = sizeof(node.tx_buffer);
    struct tt_Topic topic = {0};
    topic.name = "t";
    topic.data_size = 8;
    topic.data_decode = decode_any;
    topic.data_free = free_nothing;
    struct tt_Subscriber sub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&node, &sub, &topic, "e", on_data));

    uint8_t bytes[256];
    struct {
        struct tt_FragFirstLHeader header;
        uint8_t payload[64];
    } __attribute__((packed)) first;
    memset(&first, 0x11, sizeof(first));
    first.header.data.endpoint_id = sub.endpoint.id;
    first.header.data.seq_no = 1;
    first.header.data.entity_id = 7;
    first.header.frag_count = 723;
    uint32_t len = datagram(bytes, tt_SUBMESSAGE_TYPE_FRAG_FIRST_L, &first, sizeof(first));
    EXPECT_TRUE(process_packet(&node, bytes, 0, len, 0x0a000001, 8282, tt_TRANSPORT_UDP));
    EXPECT_EQ_U64(1, node.frag_large_skipped);

    struct {
        struct tt_FragContLHeader header;
        uint8_t payload[64];
    } __attribute__((packed)) cont;
    memset(&cont, 0x22, sizeof(cont));
    cont.header.entity_id = 7;
    cont.header.seq_no = 300;
    cont.header.frag_index = 299;
    cont.header.frag_count = 723;
    len = datagram(bytes, tt_SUBMESSAGE_TYPE_FRAG_CONT_L, &cont, sizeof(cont));
    EXPECT_TRUE(process_packet(&node, bytes, 0, len, 0x0a000001, 8282, tt_TRANSPORT_UDP));
    EXPECT_EQ_U64(2, node.frag_large_skipped);
    EXPECT_EQ_INT(0, delivered);
    EXPECT_EQ_U64(0, node.rx_malformed_drops); // passed over, not refused as malformed

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
    test_large_fragments_are_skipped_and_counted();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_large_skip: all tests passed\n");
    return 0;
}
