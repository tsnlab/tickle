/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Network profiles items D and E, core's half (DESIGN.md section 2, "Traffic classes and send time"), on the mock HAL:
//   - tt_tx_class() reads a datagram's class from what it carries: every submessage type, the discovery endpoint's
//     DATA and fragments, a call's retry count, the single and the classic form, a fence and a bell;
//   - what core actually sends is of the class it should be when the HAL looks (the mock's send hook asks
//     tt_tx_class() at that moment): a publish DATA, a resend and a durability backlog REPAIR, an eviction HEARTBEAT
//     inside a resend CONTROL, an announce and a list request CONTROL - and a publish after a resend DATA again;
//   - tt_Publisher_publish_at()'s time is the context's while its datagrams go, and 0 after.
// tests/mutants_socket_marks.py breaks each decision and expects this test, or test_socket_marks.c, to fail.

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_socket_marks.py passes a mutant

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define REMOTE_SUB_ENTITY_ID 0x22220001
#define ENDPOINT_ID 0xaabbccdd
#define USER_ENTITY_ID 0x00000010
#define TEST_SENDER_IP 0x0a000001
#define TEST_SENDER_PORT 12345
#define SEND_TIME 1761100000123456789ULL // a CLOCK_TAI time in 2025, as tt_Publisher_publish_at() is given one

// ---- What the HAL would have been told, per datagram sent.
#define MAX_SEEN 16
static uint8_t seen_class[MAX_SEEN];
static uint64_t seen_time[MAX_SEEN];
static int seen = 0;

static void record_send(const void* buf, size_t len) {
    if (seen < MAX_SEEN && test_mock_send_hook_node != NULL) {
        seen_class[seen] = tt_tx_class(test_mock_send_hook_node, buf, len);
        seen_time[seen] = test_mock_send_hook_node->tx_time_ns;
        seen++;
    }
}

static void start_recording(void) {
    seen = 0;
    test_mock_send_hook = record_send;
}

// ---- A context, a topic and a publisher, as tests/test_reliable_pubsub.c builds them.
static int32_t stub_data_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_data_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len,
                                bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void stub_data_free(struct tt_Data* data) {
    (void)data;
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
    topic->data_encode_size = stub_data_encode_size;
    topic->data_encode = stub_data_encode;
    topic->data_decode = stub_data_decode;
    topic->data_free = stub_data_free;
}

static void init_publisher(struct tt_Publisher* pub, struct tt_Context* node, struct tt_TypeSupport* topic) {
    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = ENDPOINT_ID;
    pub->endpoint.entity_id = USER_ENTITY_ID;
    pub->endpoint.name = "test_publisher";
    pub->node = node;
    pub->topic = topic;
    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)pub;
}

static void match_reader(struct tt_Publisher* pub) {
    pub->peers[0].context_id = REMOTE_NODE_ID;
    pub->peers[0].ip = TEST_SENDER_IP;
    pub->peers[0].port = TEST_SENDER_PORT;
}

static uint32_t write_acknack(struct tt_Context* node, uint32_t seq_no, uint64_t bitmap) {
    struct tt_AckNackHeader* acknack_header = (struct tt_AckNackHeader*)node->rx_buffer;
    memset(acknack_header, 0, sizeof(*acknack_header));
    acknack_header->endpoint_id = ENDPOINT_ID;
    acknack_header->sender_entity_id = REMOTE_SUB_ENTITY_ID;
    acknack_header->entity_id = USER_ENTITY_ID;
    acknack_header->seq_no = seq_no;
    acknack_header->bitmap_words = 1;
    acknack_header->bitmap[0] = bitmap;
    return sizeof(struct tt_AckNackHeader) + sizeof(uint64_t);
}

static void remote_header(struct tt_Header* header) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
}

// ---- Datagrams built by hand, for the classifier on its own.
static size_t single(uint8_t* out, uint8_t type, const void* body, size_t body_len) {
    struct tt_SingleHeader head = {tt_SINGLE_MARKER_LE, tt_VERSION, LOCAL_NODE_ID, type};
    memcpy(out, &head, sizeof(head));
    memcpy(out + sizeof(head), body, body_len);
    return sizeof(head) + body_len;
}

// A classic datagram of two submessages, `first` then `second`, each with a body of `body_len` zero bytes but for the
// headers given.
static size_t classic_two(uint8_t* out, uint8_t first, const void* first_body, size_t first_len, uint8_t second,
                          const void* second_body, size_t second_len) {
    struct tt_Header header = {.magic_value = NATIVE_MAGIC_VALUE, .version = tt_VERSION, .source = LOCAL_NODE_ID};
    size_t at = 0;
    memcpy(out, &header, sizeof(header));
    at += sizeof(header);
    struct tt_SubmessageHeader sub = {first, tt_SUBMESSAGE_ID_ALL, (uint16_t)(sizeof(sub) + first_len)};
    memcpy(out + at, &sub, sizeof(sub));
    memcpy(out + at + sizeof(sub), first_body, first_len);
    at += sizeof(sub) + first_len;
    sub = (struct tt_SubmessageHeader) {second, tt_SUBMESSAGE_ID_ALL, (uint16_t)(sizeof(sub) + second_len)};
    memcpy(out + at, &sub, sizeof(sub));
    memcpy(out + at + sizeof(sub), second_body, second_len);
    return at + sizeof(sub) + second_len;
}

static void test_the_classifier_reads_what_a_datagram_carries(void) {
    test_mock_reset();
    struct tt_Context node;
    memset(&node, 0, sizeof(node));
    node.tx_class = tt_TX_CLASS_DATA;
    uint8_t datagram[256];

    struct tt_DataHeader user = {.endpoint_id = ENDPOINT_ID, .seq_no = 1, .timestamp = 0, .entity_id = USER_ENTITY_ID};
    struct tt_DataHeader announce = {.endpoint_id = tt_DISCOVERY_ENDPOINT_ID,
                                     .seq_no = 7,
                                     .timestamp = 0,
                                     .entity_id = tt_DISCOVERY_ENTITY_ID};
    struct tt_HeartbeatHeader heartbeat;
    memset(&heartbeat, 0, sizeof(heartbeat));
    struct tt_AckNackHeader acknack;
    memset(&acknack, 0, sizeof(acknack));

    EXPECT_EQ_INT(tt_TX_CLASS_DATA,
                  tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_DATA, &user, sizeof(user))));
    EXPECT_EQ_INT(tt_TX_CLASS_CONTROL,
                  tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_DATA, &announce, sizeof(announce))));
    EXPECT_EQ_INT(
        tt_TX_CLASS_CONTROL,
        tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_HEARTBEAT, &heartbeat, sizeof(heartbeat))));
    EXPECT_EQ_INT(
        tt_TX_CLASS_CONTROL,
        tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_ACKNACK, &acknack, sizeof(acknack))));

    struct tt_FragFirstHeader first_user = {.data = user, .frag_count = 2};
    struct tt_FragFirstHeader first_announce = {.data = announce, .frag_count = 2};
    EXPECT_EQ_INT(
        tt_TX_CLASS_DATA,
        tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_FRAG_FIRST, &first_user, sizeof(first_user))));
    EXPECT_EQ_INT(tt_TX_CLASS_CONTROL, tt_tx_class(&node, datagram,
                                                   single(datagram, tt_SUBMESSAGE_TYPE_FRAG_FIRST, &first_announce,
                                                          sizeof(first_announce))));
    struct tt_FragContHeader cont_user = {.entity_id = USER_ENTITY_ID, .seq_no = 2, .frag_index = 1, .frag_count = 2};
    struct tt_FragContHeader cont_announce = {.entity_id = tt_DISCOVERY_ENTITY_ID,
                                              .seq_no = 7,
                                              .frag_index = 1,
                                              .frag_count = 2};
    EXPECT_EQ_INT(
        tt_TX_CLASS_DATA,
        tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_FRAG_CONT, &cont_user, sizeof(cont_user))));
    EXPECT_EQ_INT(tt_TX_CLASS_CONTROL,
                  tt_tx_class(&node, datagram,
                              single(datagram, tt_SUBMESSAGE_TYPE_FRAG_CONT, &cont_announce, sizeof(cont_announce))));
    uint8_t large[16] = {0};
    EXPECT_EQ_INT(tt_TX_CLASS_DATA,
                  tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_FRAG_CONT_L, large, sizeof(large))));

    // A call: first time DATA, a retry REPAIR - the request's and the response's own retry count.
    struct tt_CallRequestHeader request = {.endpoint_id = ENDPOINT_ID, .seq_no = 3, .retry = 0, .client_tag = 1};
    EXPECT_EQ_INT(
        tt_TX_CLASS_DATA,
        tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_CALLREQUEST, &request, sizeof(request))));
    request.retry = 1;
    EXPECT_EQ_INT(
        tt_TX_CLASS_REPAIR,
        tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_CALLREQUEST, &request, sizeof(request))));
    struct tt_CallResponseHeader response = {.endpoint_id = ENDPOINT_ID, .seq_no = 3, .retry = 0, .return_code = 0};
    EXPECT_EQ_INT(
        tt_TX_CLASS_DATA,
        tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_CALLRESPONSE, &response, sizeof(response))));
    response.retry = 2;
    EXPECT_EQ_INT(
        tt_TX_CLASS_REPAIR,
        tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_CALLRESPONSE, &response, sizeof(response))));

    // The classic form: the most important submessage decides, wherever it is.
    EXPECT_EQ_INT(tt_TX_CLASS_DATA,
                  tt_tx_class(&node, datagram,
                              classic_two(datagram, tt_SUBMESSAGE_TYPE_HEARTBEAT, &heartbeat, sizeof(heartbeat),
                                          tt_SUBMESSAGE_TYPE_DATA, &user, sizeof(user))));
    EXPECT_EQ_INT(tt_TX_CLASS_CONTROL,
                  tt_tx_class(&node, datagram,
                              classic_two(datagram, tt_SUBMESSAGE_TYPE_ACKNACK, &acknack, sizeof(acknack),
                                          tt_SUBMESSAGE_TYPE_HEARTBEAT, &heartbeat, sizeof(heartbeat))));
    request.retry = 1;
    EXPECT_EQ_INT(tt_TX_CLASS_REPAIR,
                  tt_tx_class(&node, datagram,
                              classic_two(datagram, tt_SUBMESSAGE_TYPE_DATA, &announce, sizeof(announce),
                                          tt_SUBMESSAGE_TYPE_CALLREQUEST, &request, sizeof(request))));

    // A segment's bell (empty) and fence: CONTROL. Bytes that are no datagram of core's: the socket's own, DATA.
    EXPECT_EQ_INT(tt_TX_CLASS_CONTROL, tt_tx_class(&node, "", 0));
    const uint8_t fence[tt_SEGMENT_FENCE_LENGTH] = {tt_SEGMENT_FENCE_MARKER, LOCAL_NODE_ID, 1};
    EXPECT_EQ_INT(tt_TX_CLASS_CONTROL, tt_tx_class(&node, fence, sizeof(fence)));
    EXPECT_EQ_INT(tt_TX_CLASS_DATA, tt_tx_class(&node, "xyzzy", 5));

    // While core resends: a user's is REPAIR, control stays CONTROL.
    node.tx_class = tt_TX_CLASS_REPAIR;
    EXPECT_EQ_INT(tt_TX_CLASS_REPAIR,
                  tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_DATA, &user, sizeof(user))));
    EXPECT_EQ_INT(
        tt_TX_CLASS_CONTROL,
        tt_tx_class(&node, datagram, single(datagram, tt_SUBMESSAGE_TYPE_HEARTBEAT, &heartbeat, sizeof(heartbeat))));
}

// A publish is DATA; the resend an ACKNACK draws is REPAIR; the publish after it DATA again (the mark is left).
static void test_a_resend_is_repair_and_a_publish_after_it_data(void) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_TypeSupport topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    match_reader(&pub);
    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;

    start_recording();
    uint32_t value = 42;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1
    EXPECT_TRUE(seen >= 1);
    for (int i = 0; i < seen; i++) {
        EXPECT_EQ_INT(tt_TX_CLASS_DATA, seen_class[i]);
        EXPECT_EQ_U64(0, seen_time[i]);
    }

    start_recording();
    struct tt_Header header;
    remote_header(&header);
    uint32_t tail = write_acknack(&node, 1, 1ULL); // seq_no 1 again
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_U32(1, pub.retransmitted);
    EXPECT_EQ_INT(1, seen);
    EXPECT_EQ_INT(tt_TX_CLASS_REPAIR, seen_class[0]);
    EXPECT_EQ_INT(tt_TX_CLASS_DATA, node.tx_class);

    start_recording();
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 2
    EXPECT_TRUE(seen >= 1);
    for (int i = 0; i < seen; i++) {
        EXPECT_EQ_INT(tt_TX_CLASS_DATA, seen_class[i]);
    }
}

// The eviction HEARTBEAT a resend sends when the sample is gone: CONTROL, though core is resending.
static void test_an_eviction_heartbeat_inside_a_resend_is_control(void) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_TypeSupport topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    pub.reliable = true; // and no history
    uint32_t value = 42;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1, unmatched

    start_recording();
    struct tt_Header header;
    remote_header(&header);
    uint32_t tail = write_acknack(&node, 1, 1ULL);
    EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, TEST_SENDER_IP, TEST_SENDER_PORT));
    EXPECT_EQ_INT(1, seen);
    EXPECT_EQ_INT(tt_TX_CLASS_CONTROL, seen_class[0]);
    EXPECT_EQ_INT(tt_TX_CLASS_DATA, node.tx_class);
}

// A durability backlog to a late joiner: REPAIR, every sample of it.
static void test_a_durability_backlog_is_repair(void) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_TypeSupport topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    TEST_RELIABLE_CACHE(cache, 4);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.durable = true;
    uint32_t value = 7;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));

    start_recording();
    struct tt_Peer late = {REMOTE_NODE_ID, TEST_SENDER_IP, TEST_SENDER_PORT};
    deliver_durability_backlog(&node, &pub, &late);
    EXPECT_EQ_INT(2, seen);
    for (int i = 0; i < seen; i++) {
        EXPECT_EQ_INT(tt_TX_CLASS_REPAIR, seen_class[i]);
    }
    EXPECT_EQ_INT(tt_TX_CLASS_DATA, node.tx_class);
}

// Discovery: the announce to a peer and a list request are CONTROL.
static void test_discovery_is_control(void) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_TypeSupport topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);

    start_recording();
    struct tt_Peer peer = {REMOTE_NODE_ID, TEST_SENDER_IP, TEST_SENDER_PORT};
    EXPECT_TRUE(build_and_send_update(&node, &peer, 1));
    send_discovery_request(&node, REMOTE_NODE_ID, 5, TEST_SENDER_IP, TEST_SENDER_PORT);
    EXPECT_EQ_INT(2, seen);
    for (int i = 0; i < seen; i++) {
        EXPECT_EQ_INT(tt_TX_CLASS_CONTROL, seen_class[i]);
    }
}

// tt_Publisher_publish_at(): the context holds the time while the publish sends, and 0 after; a plain publish, none.
static void test_publish_at_holds_its_time_for_its_datagrams(void) {
    test_mock_reset();
    struct tt_Context node;
    struct tt_TypeSupport topic;
    struct tt_Publisher pub;
    init_node_and_topic(&node, &topic);
    init_publisher(&pub, &node, &topic);
    match_reader(&pub);

    start_recording();
    uint32_t value = 1;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish_at(&pub, (struct tt_Data*)&value, SEND_TIME));
    EXPECT_TRUE(seen >= 1);
    for (int i = 0; i < seen; i++) {
        EXPECT_EQ_U64(SEND_TIME, seen_time[i]);
        EXPECT_EQ_INT(tt_TX_CLASS_DATA, seen_class[i]);
    }
    EXPECT_EQ_U64(0, node.tx_time_ns);

    start_recording();
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    EXPECT_TRUE(seen >= 1);
    for (int i = 0; i < seen; i++) {
        EXPECT_EQ_U64(0, seen_time[i]);
    }
}

int main(void) {
    test_the_classifier_reads_what_a_datagram_carries();
    test_a_resend_is_repair_and_a_publish_after_it_data();
    test_an_eviction_heartbeat_inside_a_resend_is_control();
    test_a_durability_backlog_is_repair();
    test_discovery_is_control();
    test_publish_at_holds_its_time_for_its_datagrams();
    return test_result();
}
