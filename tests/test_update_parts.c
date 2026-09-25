/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A discovery announce too large for one datagram goes out in parts (tt_SUBMESSAGE_TYPE_UPDATE_PART,
// DESIGN.md's "Discovery announce in parts"); one that fits stays the single UPDATE every node has
// always understood. The user's choice, 2026-09-24.
//
// Before this, a node whose endpoint list outgrew the datagram could not be discovered at all -
// and, until the fix just before it, went silent altogether. rmw_tickle runs one tt_Node per
// process, so two default rclcpp nodes in one process were already past that point.
//
// Sender and receiver are both real: every datagram the sender emits is captured and fed to a
// second node's process_packet(), and what that node matched is what is checked.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: build_and_send_update()/process_packet()/check_liveliness() are static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define SENDER_PORT 8282
#define MAX_ENDPOINTS 120
#define MAX_DATAGRAMS 40
#define ENDPOINT_NAME "my_robot_node_endpoint"

// --- capturing what the sender sends ---------------------------------------------------------

static uint8_t datagrams[MAX_DATAGRAMS][tt_MAX_BUFFER_LENGTH * 2];
static uint32_t datagram_len[MAX_DATAGRAMS];
static int datagram_count;

static void capture(const void* buf, size_t len) {
    EXPECT_TRUE(datagram_count < MAX_DATAGRAMS);
    if (datagram_count < MAX_DATAGRAMS && len <= sizeof(datagrams[0])) {
        memcpy(datagrams[datagram_count], buf, len);
        datagram_len[datagram_count] = (uint32_t)len;
        datagram_count++;
    }
}

static void start_capture(void) {
    datagram_count = 0;
    test_mock_send_hook = capture;
}

static const struct tt_SubmessageHeader* first_submessage(int d) {
    return (const struct tt_SubmessageHeader*)(datagrams[d] + sizeof(struct tt_Header));
}

// --- the sender: subscribers named the way rmw_tickle names endpoints --------------------------

static struct tt_Node sender;
static struct tt_Topic sender_topics[MAX_ENDPOINTS];
static struct tt_Subscriber sender_subs[MAX_ENDPOINTS];
static char topic_names[MAX_ENDPOINTS][48];

static int32_t decode_nothing(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
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
}

static void init_bare_node(struct tt_Node* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
}

static void topic_name(int i, char* out, size_t size) {
    (void)snprintf(out, size, "/my_robot_node/some_topic_name_%04d", i);
}

// A sender announcing `count` Subscribers - ~91 bytes of UPDATE entity each.
static void init_sender(int count, uint64_t last_modified) {
    init_bare_node(&sender, SENDER_ID);
    for (int i = 0; i < count; i++) {
        topic_name(i, topic_names[i], sizeof(topic_names[i]));
        memset(&sender_topics[i], 0, sizeof(sender_topics[i]));
        sender_topics[i].name = topic_names[i];
        sender_topics[i].data_size = 8;
        sender_topics[i].data_decode = decode_nothing;
        sender_topics[i].data_free = free_nothing;
        EXPECT_EQ_INT(tt_RET_OK,
                      tt_Node_create_subscriber(&sender, &sender_subs[i], &sender_topics[i], ENDPOINT_NAME, on_data));
    }
    sender.last_modified = last_modified;
}

// --- the receiver: publishers for a chosen few of the sender's topics --------------------------

static struct tt_Node receiver;
static struct tt_Topic receiver_topics[4];
static struct tt_Publisher receiver_pubs[4];
static char receiver_names[4][48];
static int receiver_pub_count;

static int32_t encode_size_one(struct tt_Data* data) {
    (void)data;
    return 1;
}

// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's fixed signature
static int32_t encode_one(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    (void)len;
    payload[0] = 0;
    return 1;
}

static void init_receiver(const int* topic_indices, int n) {
    init_bare_node(&receiver, RECEIVER_ID);
    receiver_pub_count = n;
    for (int k = 0; k < n; k++) {
        topic_name(topic_indices[k], receiver_names[k], sizeof(receiver_names[k]));
        memset(&receiver_topics[k], 0, sizeof(receiver_topics[k]));
        receiver_topics[k].name = receiver_names[k];
        receiver_topics[k].data_size = 8;
        receiver_topics[k].data_encode_size = encode_size_one;
        receiver_topics[k].data_encode = encode_one;
        EXPECT_EQ_INT(tt_RET_OK,
                      tt_Node_create_publisher(&receiver, &receiver_pubs[k], &receiver_topics[k], ENDPOINT_NAME));
    }
}

// Whether receiver publisher k has matched the sender's Subscriber on the same topic.
static bool matched(int k) {
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (receiver_pubs[k].peers[i].node_id == SENDER_ID) {
            return true;
        }
    }
    return false;
}

static void deliver(int d) {
    EXPECT_TRUE(process_packet(&receiver, datagrams[d], 0, datagram_len[d], SENDER_IP, SENDER_PORT));
}

static void deliver_all(void) {
    for (int d = 0; d < datagram_count; d++) {
        deliver(d);
    }
}

// Announces, and returns how many datagrams went out.
static int announce(void) {
    start_capture();
    EXPECT_TRUE(build_and_send_update(&sender, NULL, 0));
    node_flush(&sender, 0, NULL);
    return datagram_count;
}

// --- tests -------------------------------------------------------------------------------------

static void test_announce_that_fits_stays_a_single_update(void) {
    // Control, and the compatibility promise: nothing changes for a node that fits.
    test_mock_reset();
    init_sender(8, 100);
    EXPECT_EQ_INT(1, announce());
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_UPDATE, first_submessage(0)->type);
}

static void test_large_announce_goes_in_datagram_sized_parts(void) {
    // Plan's acceptance test: over 100 endpoints, and every datagram within the control limit -
    // 1472 even in a build whose tt_MAX_BUFFER_LENGTH is larger (config.h).
    test_mock_reset();
    init_sender(MAX_ENDPOINTS, 100);
    int n = announce();
    EXPECT_TRUE(n >= 2);
    uint32_t entities = 0;
    for (int d = 0; d < n; d++) {
        EXPECT_TRUE(datagram_len[d] <= tt_CONTROL_MAX_LENGTH); // whatever tt_MAX_BUFFER_LENGTH is
        const struct tt_SubmessageHeader* sub = first_submessage(d);
        EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_UPDATE_PART, sub->type);
        const struct tt_UpdatePartHeader* part = (const struct tt_UpdatePartHeader*)(sub + 1);
        EXPECT_EQ_INT(d, part->part_index);
        EXPECT_EQ_INT(n, part->part_count);
        EXPECT_EQ_U32(100, (uint32_t)part->last_modified);
        EXPECT_EQ_U32(sizeof(struct tt_Header) + sub->length, datagram_len[d]); // one submessage per datagram
        entities += part->entity_count;
    }
    EXPECT_EQ_U32(MAX_ENDPOINTS, entities);
    EXPECT_EQ_U32(0, (uint32_t)sender.tx_dropped_oversize);
}

static void test_receiver_matches_endpoints_from_every_part(void) {
    test_mock_reset();
    init_sender(MAX_ENDPOINTS, 100);
    int n = announce();
    const int topics[] = {0, MAX_ENDPOINTS / 2, MAX_ENDPOINTS - 1}; // first, middle and last part
    init_receiver(topics, 3);

    for (int d = 0; d < n - 1; d++) {
        deliver(d);
    }
    // Not complete yet: what has arrived is applied, but the announce is not the acted-on one.
    EXPECT_TRUE(matched(0));
    EXPECT_TRUE(!matched(2));
    EXPECT_TRUE(!receiver.update_seen[SENDER_ID]);

    deliver(n - 1);
    EXPECT_TRUE(matched(0));
    EXPECT_TRUE(matched(1));
    EXPECT_TRUE(matched(2));
    EXPECT_TRUE(receiver.update_seen[SENDER_ID]);
    EXPECT_EQ_U32(100, (uint32_t)receiver.update_last_modified[SENDER_ID]);
    EXPECT_EQ_U32(0, receiver.update_part_received[SENDER_ID]);
}

static void test_lost_part_is_filled_by_the_next_announce(void) {
    // The periodic announce resends every part under the same last_modified; the receiver keeps
    // what it had and completes on the part it missed.
    test_mock_reset();
    init_sender(MAX_ENDPOINTS, 100);
    int n = announce();
    const int topics[] = {MAX_ENDPOINTS - 1};
    init_receiver(topics, 1);
    for (int d = 0; d < n - 1; d++) {
        deliver(d); // the last part is "lost"
    }
    EXPECT_TRUE(!receiver.update_seen[SENDER_ID]);
    EXPECT_TRUE(!matched(0));

    EXPECT_EQ_INT(n, announce()); // next periodic announce, same content
    deliver_all();
    EXPECT_TRUE(receiver.update_seen[SENDER_ID]);
    EXPECT_TRUE(matched(0));
}

static void test_completed_announce_replaces_the_previous_one(void) {
    // Same replacement semantics as a single UPDATE: an endpoint the new announce no longer lists
    // is forgotten once it completes.
    test_mock_reset();
    init_sender(MAX_ENDPOINTS, 100);
    announce();
    const int topics[] = {MAX_ENDPOINTS - 1};
    init_receiver(topics, 1);
    deliver_all();
    EXPECT_TRUE(matched(0));

    // The sender drops its last endpoint (still more than a datagram's worth) and re-announces.
    sender.endpoints[MAX_ENDPOINTS - 1] = NULL;
    sender.last_modified = 101;
    announce();
    deliver_all();
    EXPECT_TRUE(receiver.update_seen[SENDER_ID]);
    EXPECT_EQ_U32(101, (uint32_t)receiver.update_last_modified[SENDER_ID]);
    EXPECT_TRUE(!matched(0));
}

static void test_repeated_complete_announce_is_a_no_op(void) {
    test_mock_reset();
    init_sender(MAX_ENDPOINTS, 100);
    announce();
    const int topics[] = {0};
    init_receiver(topics, 1);
    deliver_all();
    EXPECT_TRUE(matched(0));
    announce();
    deliver(0); // one part of the unchanged resend must not start a new, incomplete announce
    EXPECT_TRUE(receiver.update_seen[SENDER_ID]);
    EXPECT_EQ_U32(0, receiver.update_part_received[SENDER_ID]);
    EXPECT_TRUE(matched(0));
}

static void test_single_update_supersedes_parts_in_progress(void) {
    // The sender shrinks back under one datagram while the receiver is mid-assembly.
    test_mock_reset();
    init_sender(MAX_ENDPOINTS, 100);
    announce();
    const int topics[] = {0};
    init_receiver(topics, 1);
    deliver(0);
    EXPECT_TRUE(receiver.update_part_received[SENDER_ID] != 0);

    init_sender(4, 200);
    EXPECT_EQ_INT(1, announce());
    deliver(0);
    EXPECT_EQ_U32(0, receiver.update_part_received[SENDER_ID]);
    EXPECT_TRUE(receiver.update_seen[SENDER_ID]);
    EXPECT_EQ_U32(200, (uint32_t)receiver.update_last_modified[SENDER_ID]);
    EXPECT_TRUE(matched(0));
}

static void test_node_heard_only_through_parts_still_expires(void) {
    // Liveliness must cover a source whose announce never completed: its entities are recorded
    // part by part, so leaving it out of the sweep would keep them forever.
    test_mock_reset();
    init_sender(MAX_ENDPOINTS, 100);
    announce();
    const int topics[] = {0};
    init_receiver(topics, 1);
    deliver(0);
    EXPECT_TRUE(matched(0));
    uint64_t later = (uint64_t)(tt_LIVELINESS_MISS_THRESHOLD + 2) * tt_NODE_UPDATE_INTERVAL;
    test_mock_now = later;
    check_liveliness(&receiver, later, NULL);
    EXPECT_EQ_U32(0, receiver.update_part_received[SENDER_ID]);
    EXPECT_TRUE(!matched(0));
}

static void test_malformed_part_headers_are_rejected(void) {
    test_mock_reset();
    init_sender(MAX_ENDPOINTS, 100);
    announce();
    const int topics[] = {0};
    init_receiver(topics, 1);
    struct tt_UpdatePartHeader* part =
        (struct tt_UpdatePartHeader*)((struct tt_SubmessageHeader*)(datagrams[0] + sizeof(struct tt_Header)) + 1);
    part->part_count = 1; // a single part is a single UPDATE, never a part
    deliver(0);
    part->part_count = tt_UPDATE_MAX_PARTS + 1;
    deliver(0);
    part->part_count = 2;
    part->part_index = 2; // index past the count
    deliver(0);
    EXPECT_EQ_U32(0, receiver.update_part_received[SENDER_ID]);
    EXPECT_TRUE(!matched(0));
}

int main(void) {
    test_announce_that_fits_stays_a_single_update();
    test_large_announce_goes_in_datagram_sized_parts();
    test_receiver_matches_endpoints_from_every_part();
    test_lost_part_is_filled_by_the_next_announce();
    test_completed_announce_replaces_the_previous_one();
    test_repeated_complete_announce_is_a_no_op();
    test_single_update_supersedes_parts_in_progress();
    test_node_heard_only_through_parts_still_expires();
    test_malformed_part_headers_are_rejected();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_update_parts: all tests passed\n");
    return 0;
}
