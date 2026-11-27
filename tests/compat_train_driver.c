/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// tests/compat_train.sh's driver. The packet-train wire design
// (examples/perf_hil/experiments/adaptive_pacing/DESIGN.md, 27.6) rests on two claims about every build that exists,
// v2.1.0 included:
//   R  a reader ignores HEARTBEAT flag bits other than FINAL and LIVELINESS: a HEARTBEAT with bit 2 set draws exactly
//   the
//      datagrams the same HEARTBEAT without it draws (and the control, FINAL set, draws none: the driver can see a
//      reply);
//   W  a writer ignores tt_AckNackHeader.reserved: an ACKNACK with reserved 0xBEEF causes exactly the datagrams (the
//      repairs) and the same state as one with 0 (and the control, a different bitmap, differs: the driver can see
//      repairs).
// Built against one tree's core (-DTICKLE_C, that tree's headers and mock HAL), it prints one line per variant, a
// digest of every datagram the core sent while handling the datagram under test; the script compares the lines.

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox: process_heartbeat() and process_acknack() are static

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define REMOTE_SUB_ENTITY_ID 0x22220001
#define ENDPOINT_ID 0xaabbccdd
#define SENDER_IP 0x0a000001
#define SENDER_PORT 12345
#define TRAIN_BIT (1U << 2) // the new HEARTBEAT flag

static int32_t stub_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}
static int32_t stub_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}
static int32_t stub_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}
static void stub_free(struct tt_Data* data) {
    (void)data;
}
static void stub_callback(struct tt_Subscriber* s, uint64_t t, uint16_t q, struct tt_Data* d) {
    (void)s;
    (void)t;
    (void)q;
    (void)d;
}

static uint8_t captured[16384];
static size_t captured_len;
static int captured_count;
static void capture(const void* buf, size_t len) {
    if (captured_len + len <= sizeof(captured)) {
        memcpy(captured + captured_len, buf, len);
        captured_len += len;
    }
    captured_count++;
}
static void capture_reset(void) {
    captured_len = 0;
    captured_count = 0;
}
static uint64_t digest(void) { // FNV-1a over everything sent
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < captured_len; i++) {
        h = (h ^ captured[i]) * 1099511628211ULL;
    }
    return h;
}

static void init_node_and_topic(struct tt_Context* node, struct tt_TypeSupport* topic) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    memset(topic, 0, sizeof(*topic));
    topic->name = "test_topic";
    topic->data_size = sizeof(uint32_t);
    topic->data_encode_size = stub_encode_size;
    topic->data_encode = stub_encode;
    topic->data_decode = stub_decode;
    topic->data_free = stub_free;
}
static void init_header(struct tt_Header* header) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
}

#define TEST_REORDER_SLOTS 16
#define TEST_REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + 64)
static uint64_t reorder_storage[TEST_REORDER_SLOTS * TEST_REORDER_SLOT_BYTES / sizeof(uint64_t)];

static uint32_t write_heartbeat(struct tt_Context* node, uint32_t first, uint32_t last, uint8_t flags) {
    struct tt_HeartbeatHeader* hb = (struct tt_HeartbeatHeader*)node->rx_buffer;
    memset(hb, 0, sizeof(*hb));
    hb->endpoint_id = ENDPOINT_ID;
    hb->first_available_seq_no = first;
    hb->last_seq_no = last;
    hb->flags = flags;
    return sizeof(struct tt_HeartbeatHeader);
}
static uint32_t write_data(struct tt_Context* node, uint32_t seq_no, uint32_t value) {
    struct tt_DataHeader* dh = (struct tt_DataHeader*)node->rx_buffer;
    memset(dh, 0, sizeof(*dh));
    dh->endpoint_id = ENDPOINT_ID;
    dh->seq_no = seq_no;
    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node->rx_buffer + tail, &value, sizeof(value));
    return tail + sizeof(value);
}
static uint32_t write_acknack(struct tt_Context* node, uint32_t seq_no, uint64_t bitmap, uint16_t reserved) {
    struct tt_AckNackHeader* ack = (struct tt_AckNackHeader*)node->rx_buffer;
    memset(ack, 0, sizeof(*ack));
    ack->endpoint_id = ENDPOINT_ID;
    ack->sender_entity_id = REMOTE_SUB_ENTITY_ID;
    ack->seq_no = seq_no;
    ack->bitmap_words = 1;
    ack->reserved = reserved;
    ack->bitmap[0] = bitmap;
    return sizeof(struct tt_AckNackHeader) + sizeof(uint64_t);
}

// R: a Subscriber that holds sequence numbers 1 and 2 (gap = false) or 1 and 3 (a gap at 2) is handed one HEARTBEAT.
static void scenario_r(const char* name, bool gap, uint8_t flags) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_TypeSupport topic;
    struct tt_Subscriber sub;
    init_node_and_topic(&node, &topic);
    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = ENDPOINT_ID;
    sub.node = &node;
    sub.topic = &topic;
    sub.callback = stub_callback;
    sub.reliable = true;
    sub.reorder_storage = reorder_storage;
    sub.reorder_slots = TEST_REORDER_SLOTS;
    sub.reorder_slot_bytes = TEST_REORDER_SLOT_BYTES;
    memset(reorder_storage, 0, sizeof(reorder_storage));
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub.writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&sub;
    struct tt_Header header;
    init_header(&header);
    uint32_t tail = write_data(&node, 1, 11);
    (void)process_data(&node, &header, node.rx_buffer, 0, tail, SENDER_IP, SENDER_PORT);
    tail = write_data(&node, gap ? 3U : 2U, 12);
    (void)process_data(&node, &header, node.rx_buffer, 0, tail, SENDER_IP, SENDER_PORT);
    test_mock_send_hook = capture;
    capture_reset();
    tail = write_heartbeat(&node, 1, gap ? 3U : 2U, flags);
    bool ok = process_heartbeat(&node, &header, node.rx_buffer, 0, tail, SENDER_IP, SENDER_PORT);
    test_mock_send_hook = NULL;
    printf("%-14s ok=%d sends=%d bytes=%zu digest=%016llx\n", name, ok ? 1 : 0, captured_count, captured_len,
           (unsigned long long)digest());
}

// W: a reliable Publisher that has sent sequence numbers 1-3 to one matched peer is handed one ACKNACK.
static void scenario_w(const char* name, uint32_t seq_no, uint64_t bitmap, uint16_t reserved) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_TypeSupport topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    memset(&pub, 0, sizeof(pub));
    pub.endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub.endpoint.id = ENDPOINT_ID;
    pub.endpoint.name = "test_publisher";
    pub.node = &node;
    pub.topic = &topic;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub.peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;
    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.peers[0].context_id = REMOTE_NODE_ID;
    pub.peers[0].ip = SENDER_IP;
    pub.peers[0].port = SENDER_PORT;
    for (uint32_t v = 1; v <= 3; v++) {
        (void)tt_Publisher_publish(&pub, (struct tt_Data*)&v);
    }
    struct tt_Header header;
    init_header(&header);
    test_mock_send_hook = capture;
    capture_reset();
    uint32_t tail = write_acknack(&node, seq_no, bitmap, reserved);
    bool ok = process_acknack(&node, &header, node.rx_buffer, 0, tail, SENDER_IP, SENDER_PORT);
    test_mock_send_hook = NULL;
    const struct tt_PeerAck* ack = find_peer_ack(&pub, REMOTE_NODE_ID, REMOTE_SUB_ENTITY_ID);
    printf("%-14s ok=%d sends=%d bytes=%zu digest=%016llx peer_ack=%u\n", name, ok ? 1 : 0, captured_count,
           captured_len, (unsigned long long)digest(), ack != NULL ? (unsigned)ack->ack_seq_no : 0U);
}

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    // R: reader. ctl = the HEARTBEAT as today (FINAL clear asks for an answer); bit2 = the same with the train bit;
    // final = FINAL set (asks for none: proves the driver sees a reply); the gap variants repeat it with a gap open.
    scenario_r("R-ctl", false, 0);
    scenario_r("R-bit2", false, TRAIN_BIT);
    scenario_r("R-bit2-hi", false, (uint8_t)0xFC); // every bit above LIVELINESS and FINAL set (bit 1 clear)
    scenario_r("R-final", false, tt_HEARTBEAT_FLAG_FINAL);
    scenario_r("R-gap-ctl", true, 0);
    scenario_r("R-gap-bit2", true, TRAIN_BIT);
    // W: writer. ctl = ACKNACK naming sequence number 2 missing, reserved 0; res = the same with reserved 0xBEEF;
    // other = a different bitmap (proves the driver sees repairs); the last pair has no gap.
    scenario_w("W-ctl", 2, 0x1, 0);
    scenario_w("W-reserved", 2, 0x1, 0xBEEF);
    scenario_w("W-reserved-ff", 2, 0x1, 0xFFFF);
    scenario_w("W-other", 2, 0x3, 0);
    scenario_w("W-ack-ctl", 4, 0x0, 0);
    scenario_w("W-ack-reserved", 4, 0x0, 0x1234);
    return 0;
}
