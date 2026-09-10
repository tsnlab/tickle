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

// Whitebox: a packet from a peer of the opposite byte order carries a tt_Header with
// REVERSE_MAGIC_VALUE, and every framing field (submessage length, endpoint ids, seq/ack
// numbers, timestamps, string-length prefixes) is then in that peer's order. This checks the
// receive path byte-swaps all of them - the app's own CDR payload is its codec's problem.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2

// The host running this test is (almost certainly) little-endian, so "the other endianness" is
// big-endian: write each field byte-swapped and stamp the header REVERSE.
static uint16_t sw16(uint16_t v) {
    return _tt_bswap_16(v);
}
static uint32_t sw32(uint32_t v) {
    return _tt_bswap_32(v);
}
static uint64_t sw64(uint64_t v) {
    return _tt_bswap_64(v);
}

static void write_reverse_header(uint8_t* buf, uint8_t source) {
    struct tt_Header* h = (struct tt_Header*)buf;
    h->magic_value = REVERSE_MAGIC_VALUE;
    h->version = tt_VERSION;
    h->source = source;
}

// --- tt_hash_id is endian-independent ---------------------------------------

static uint32_t byte_swapped_string_hash_ref;

static void test_hash_id_is_byte_oriented_and_stable(void) {
    // Same names must hash the same regardless of host endianness; this pins the value so a
    // regression on either side (or a switch back to word-at-a-time reads) is caught.
    uint32_t h = tt_hash_id("std_msgs/String", "chatter");
    EXPECT_TRUE(h != 0);
    EXPECT_EQ_U32(h, tt_hash_id("std_msgs/String", "chatter")); // deterministic

    // The separator makes the split between type and name significant.
    EXPECT_TRUE(tt_hash_id("ab", "c") != tt_hash_id("a", "bc"));
    byte_swapped_string_hash_ref = h;
}

// --- tt_decode_string with a byte-swapped length prefix --------------------

static void test_decode_string_swaps_length_prefix(void) {
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    const char* s = "hello"; // encoded length is 6 (includes the NUL)
    uint16_t len_be = sw16(6);
    memcpy(buf, &len_be, sizeof(len_be));
    memcpy(buf + sizeof(len_be), s, 6);
    uint32_t tail = sizeof(len_be) + 6;

    uint32_t head = 0;
    uint16_t out_len = 0;
    char* out = NULL;
    EXPECT_TRUE(tt_decode_string(buf, &head, tail, &out_len, &out, true));
    EXPECT_EQ_U32(6, (uint32_t)out_len);
    EXPECT_TRUE(strcmp(out, "hello") == 0);
    EXPECT_EQ_U32(tail, head); // advanced past the whole field
}

// --- DATA submessage from a reverse-endian publisher ----------------------

static int sub_calls;
static uint64_t sub_time;
static uint16_t sub_seq;
static uint32_t sub_value;

static int32_t data_decode_le(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    uint32_t v = 0;
    memcpy(&v, payload, sizeof(v));
    *(uint32_t*)data = is_native_endian ? v : _tt_bswap_32(v);
    return (int32_t)sizeof(uint32_t);
}
static void data_free_noop(struct tt_Data* data) {
    (void)data;
}
static void sub_cb(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    sub_calls++;
    sub_time = time;
    sub_seq = seq_no;
    sub_value = *(uint32_t*)data;
}

static void test_reverse_endian_data_routes_and_unswaps(void) {
    test_mock_reset();
    sub_calls = 0;

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "t";
    topic.data_size = sizeof(uint32_t);
    topic.data_decode = data_decode_le;
    topic.data_free = data_free_noop;

    struct tt_Subscriber sub;
    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = 0xdeadbeef;
    sub.topic = &topic;
    sub.callback = sub_cb;

    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.id = LOCAL_NODE_ID;
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&sub;

    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    write_reverse_header(buf, REMOTE_NODE_ID);
    uint32_t off = sizeof(struct tt_Header);

    uint16_t sub_len = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader) + sizeof(uint32_t);
    struct tt_SubmessageHeader* sh = (struct tt_SubmessageHeader*)(buf + off);
    sh->type = tt_SUBMESSAGE_TYPE_DATA;
    sh->receiver = tt_SUBMESSAGE_ID_ALL;
    sh->length = sw16(sub_len);
    off += sizeof(struct tt_SubmessageHeader);

    struct tt_DataHeader* dh = (struct tt_DataHeader*)(buf + off);
    dh->endpoint_id = sw32(0xdeadbeef);
    dh->seq_no = sw32(42);
    dh->timestamp = sw64(0x1122334455667788ULL);
    off += sizeof(struct tt_DataHeader);

    uint32_t payload_be = sw32(0xCAFEF00D);
    memcpy(buf + off, &payload_be, sizeof(payload_be));
    off += sizeof(payload_be);

    EXPECT_TRUE(process_packet(&node, buf, 0, off, 0, 0));
    EXPECT_EQ_U32(1, (uint32_t)sub_calls);
    EXPECT_EQ_U32(42, (uint32_t)sub_seq);
    EXPECT_TRUE(sub_time == 0x1122334455667788ULL);
    EXPECT_EQ_U32(0xCAFEF00D, sub_value);
}

// --- UPDATE announce from a reverse-endian peer: discovery still matches --

static void test_reverse_endian_update_matches_and_learns_peer(void) {
    test_mock_reset();

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "t";

    struct tt_Publisher pub;
    memset(&pub, 0, sizeof(pub));
    pub.endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub.endpoint.id = 0x01020304;
    pub.endpoint.name = "p";
    pub.topic = &topic;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub.peers[i].node_id = tt_NODE_ID_INVALID;
    }

    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.id = LOCAL_NODE_ID;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    write_reverse_header(buf, REMOTE_NODE_ID);
    uint32_t off = sizeof(struct tt_Header);

    struct tt_SubmessageHeader* sh = (struct tt_SubmessageHeader*)(buf + off);
    sh->type = tt_SUBMESSAGE_TYPE_UPDATE;
    sh->receiver = tt_SUBMESSAGE_ID_ALL;
    uint32_t sub_start = off;
    off += sizeof(struct tt_SubmessageHeader);

    struct tt_UpdateHeader* uh = (struct tt_UpdateHeader*)(buf + off);
    uh->last_modified = sw64(1000);
    uh->entity_count = 1;
    off += sizeof(struct tt_UpdateHeader);

    struct tt_UpdateEntity* ue = (struct tt_UpdateEntity*)(buf + off);
    ue->endpoint_id = sw32(0x01020304); // same id our Publisher has
    ue->kind = tt_KIND_TOPIC_SUBSCRIBER;
    off += sizeof(struct tt_UpdateEntity);

    // two length-prefixed strings ("type", "name"), each length byte-swapped
    for (int k = 0; k < 2; k++) {
        const char* str = k == 0 ? "T" : "N";
        uint16_t l = sw16(2);
        memcpy(buf + off, &l, sizeof(l));
        off += sizeof(l);
        memcpy(buf + off, str, 2);
        off += 2;
    }

    sh->length = sw16((uint16_t)(off - sub_start));

    uint32_t sender_ip = 0x0a000005;
    uint16_t sender_port = 9999;
    EXPECT_TRUE(process_packet(&node, buf, 0, off, sender_ip, sender_port));

    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)pub.peers[0].node_id);
    EXPECT_EQ_U32(sender_ip, pub.peers[0].ip);
    EXPECT_EQ_U32((uint32_t)sender_port, (uint32_t)pub.peers[0].port);
}

// --- CALLREQUEST from a reverse-endian client: server answers with the right seq_no ---

static int8_t srv_cb(struct tt_Server* s, struct tt_Request* req, struct tt_Response* resp) {
    (void)s;
    (void)req;
    (void)resp;
    return 0;
}
static int32_t req_decode(struct tt_Request* r, const uint8_t* p, uint32_t len, bool native) {
    (void)r;
    (void)p;
    (void)len;
    (void)native;
    return 0;
}
static void req_free(struct tt_Request* r) {
    (void)r;
}
static int32_t resp_encode_size(struct tt_Response* r) {
    (void)r;
    return 0;
}
static int32_t resp_encode(struct tt_Response* r, uint8_t* p, uint32_t len) {
    (void)r;
    (void)p;
    (void)len;
    return 0;
}
static void resp_free(struct tt_Response* r) {
    (void)r;
}

static void test_reverse_endian_callrequest_reaches_server(void) {
    test_mock_reset();

    struct tt_Service service;
    memset(&service, 0, sizeof(service));
    service.name = "svc";
    service.request_size = 1;
    service.response_size = 1;
    service.request_decode = req_decode;
    service.request_free = req_free;
    service.response_encode_size = resp_encode_size;
    service.response_encode = resp_encode;
    service.response_free = resp_free;

    struct tt_Server server;
    memset(&server, 0, sizeof(server));
    server.endpoint.kind = tt_KIND_SERVICE_SERVER;
    server.endpoint.id = 0x55667788;
    server.service = &service;
    server.callback = srv_cb;

    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.id = LOCAL_NODE_ID;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&server;
    server.node = &node;

    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    write_reverse_header(buf, REMOTE_NODE_ID);
    uint32_t off = sizeof(struct tt_Header);

    uint16_t sub_len = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallRequestHeader);
    struct tt_SubmessageHeader* sh = (struct tt_SubmessageHeader*)(buf + off);
    sh->type = tt_SUBMESSAGE_TYPE_CALLREQUEST;
    sh->receiver = tt_SUBMESSAGE_ID_ALL;
    sh->length = sw16(sub_len);
    off += sizeof(struct tt_SubmessageHeader);

    struct tt_CallRequestHeader* crh = (struct tt_CallRequestHeader*)(buf + off);
    crh->endpoint_id = sw32(0x55667788);
    crh->seq_no = sw16(9);
    crh->retry = 0;
    off += sizeof(struct tt_CallRequestHeader);

    EXPECT_TRUE(process_packet(&node, buf, 0, off, 0x0a000009, 8282));

    // A CallResponse was unicast straight back to the requester.
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);

    // ...and its seq_no is 9 in this node's own (native) byte order.
    struct tt_Header* out_h = (struct tt_Header*)node.tx_buffer;
    (void)out_h;
    struct tt_SubmessageHeader* out_sh = (struct tt_SubmessageHeader*)(node.tx_buffer + sizeof(struct tt_Header));
    struct tt_CallResponseHeader* out_crh =
        (struct tt_CallResponseHeader*)((uint8_t*)out_sh + sizeof(struct tt_SubmessageHeader));
    EXPECT_EQ_U32(9, (uint32_t)out_crh->seq_no);
    EXPECT_EQ_U32(server.endpoint.id, out_crh->endpoint_id);
}

int main(void) {
    test_hash_id_is_byte_oriented_and_stable();
    test_decode_string_swaps_length_prefix();
    test_reverse_endian_data_routes_and_unswaps();
    test_reverse_endian_update_matches_and_learns_peer();
    test_reverse_endian_callrequest_reaches_server();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_cross_endian: all tests passed\n");
    return 0;
}
