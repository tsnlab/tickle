/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g9 (rmw_tickle/RMW_GAPS_PLAN.md, 2026-09-28): samples between endpoints of one context, built with
// tt_LOCAL_DELIVERY (rmw_tickle's setting) and with fragmentation compiled in. Checked:
// - every sample of a local pair arrives, in order, once - also one that goes as fragments;
// - a local and a remote Subscriber together: each sample once at each, and the context's own datagrams, handed
//   back to it as a loopback would, add nothing;
// - an RxO-incompatible local pair gets nothing;
// - a late durable local Subscriber gets the durable backlog in order, a fragmented sample whole;
// - a publish with no local Subscriber sends the same bytes as one with a local Subscriber.
// Mutants, each killed here: in-process delivery removed; delivered twice; the durable backlog not delivered
// locally; the local RxO check removed.
#define tt_LOCAL_DELIVERY 1
#define tt_MAX_SAMPLE_LENGTH 16000

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define LOCAL_ID 1
#define REMOTE_ID 2
#define LOCAL_IP 0x0a000001
#define PORT 40000
#define CLOCK_NS (100ULL * tt_SECOND)
#define SAMPLES 10
#define SMALL 64
#define LARGE 5000 // several datagrams
#define CAPTURE_MAX 128
#define RECORD_MAX 64
#define CACHE_DEPTH 16

// A sample: its id, then `size` - 4 bytes of a pattern the id seeds.
struct sample {
    uint32_t id;
    uint32_t size;
};

static int32_t sample_size(struct tt_Data* data) {
    return (int32_t)((struct sample*)data)->size;
}
static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    const struct sample* sample = (const struct sample*)data;
    if (len < sample->size) {
        return -1;
    }
    memcpy(payload, &sample->id, sizeof(sample->id));
    for (uint32_t i = sizeof(sample->id); i < sample->size; i++) {
        payload[i] = (uint8_t)(sample->id + i);
    }
    return (int32_t)sample->size;
}
// Received samples are read in place, as rmw_tickle reads them.
struct view {
    const uint8_t* payload;
    uint32_t length;
};
static struct view current;
static struct tt_Data* view_payload(const uint8_t* payload, uint32_t length, bool native) {
    (void)native;
    current.payload = payload;
    current.length = length;
    return (struct tt_Data*)&current;
}
static int32_t refuse(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)len;
    (void)native;
    return -1;
}
static void free_nothing(struct tt_Data* data) {
    (void)data;
}

// What one Subscriber received: ids, lengths, and whether each payload was intact.
struct record {
    int count;
    uint32_t ids[RECORD_MAX];
    uint32_t lengths[RECORD_MAX];
    int corrupt;
};
static struct record local_record;
static struct record remote_record;

static void record_sample(struct record* record, const struct view* view) {
    uint32_t id = 0;
    memcpy(&id, view->payload, sizeof(id));
    for (uint32_t i = sizeof(id); i < view->length; i++) {
        record->corrupt += view->payload[i] != (uint8_t)(id + i) ? 1 : 0;
    }
    if (record->count < RECORD_MAX) {
        record->ids[record->count] = id;
        record->lengths[record->count] = view->length;
    }
    record->count++;
}
static void on_local(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    record_sample(&local_record, (const struct view*)data);
}
static void on_remote(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    record_sample(&remote_record, (const struct view*)data);
}

static uint8_t captured[CAPTURE_MAX][tt_MAX_BUFFER_LENGTH * 2];
static size_t captured_length[CAPTURE_MAX];
static int captured_count;
static void capture(const void* buf, size_t len) {
    if (captured_count < CAPTURE_MAX && len <= sizeof(captured[0])) {
        memcpy(captured[captured_count], buf, len);
        captured_length[captured_count++] = len;
    }
}

static struct tt_TypeSupport topic;
// Each Subscriber's caller storage: its own reassembly slot (tt_Subscriber_storage_size(); docs/DESIGN.md section 8).
static uint64_t sub_storage[(tt_FRAG_SLOT_BYTES + 64) / sizeof(uint64_t)];
static uint64_t remote_storage[(tt_FRAG_SLOT_BYTES + 64) / sizeof(uint64_t)];
static struct tt_Context local;
static struct tt_Context remote;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static struct tt_Subscriber remote_sub;
static uint64_t pub_storage[(size_t)256U * 1024U / sizeof(uint64_t)];

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_TX_BUFFER_LENGTH;
    context->last_modified = CLOCK_NS;
}

static void setup(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&local, LOCAL_ID);
    init_context(&remote, REMOTE_ID);
    memset(&topic, 0, sizeof(topic));
    topic.name = "g9::msg::dds_::Sample_";
    topic.data_size = sizeof(struct view);
    topic.data_encode_size = sample_size;
    topic.data_encode = sample_encode;
    topic.data_decode = refuse;
    topic.data_decode_inplace = view_payload;
    topic.data_free = free_nothing;
    topic.max_encoded_size = LARGE;
    memset(&local_record, 0, sizeof(local_record));
    memset(&remote_record, 0, sizeof(remote_record));
    memset(&pub, 0, sizeof(pub));
    memset(&sub, 0, sizeof(sub));
    memset(&remote_sub, 0, sizeof(remote_sub));
}

// The Publisher, RELIABLE TRANSIENT_LOCAL KEEP_LAST CACHE_DEPTH.
static void create_durable_publisher(void) {
    const struct tt_QosProfile qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = CACHE_DEPTH},
                                      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE},
                                      .durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL}};
    size_t storage_size = tt_Publisher_storage_size(&topic, &qos);
    EXPECT_TRUE(storage_size <= sizeof(pub_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&local), &pub, &topic, "rt/g9", &qos,
                                                      NULL, pub_storage, storage_size));
}

static void publish(uint32_t id, uint32_t size) {
    struct sample sample = {id, size};
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample));
}

// Everything captured, handed to `to` as if it came from `ip`.
static void carry_captured(struct tt_Context* to, uint32_t ip) {
    for (int i = 0; i < captured_count; i++) {
        memcpy(to->rx_buffer, captured[i], captured_length[i]);
        (void)process_packet(to, to->rx_buffer, 0, (uint32_t)captured_length[i], ip, PORT, tt_TRANSPORT_UDP);
    }
}

static void test_a_local_pair(void) {
    setup();
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&local), &pub, &topic, "rt/g9",
                                                      &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&local), &sub, &topic, "rt/g9",
                                            &TEST_QOS_UNRELIABLE, &(struct tt_SubscriberOptions) {.callback = on_local},
                                            sub_storage, sizeof(sub_storage)));
    for (uint32_t i = 0; i < SAMPLES; i++) {
        publish(i, i == SAMPLES / 2 ? LARGE : SMALL); // one of them fragmented
    }
    EXPECT_EQ_INT(SAMPLES, local_record.count);
    for (uint32_t i = 0; i < SAMPLES; i++) {
        EXPECT_EQ_U32(i, local_record.ids[i]);
    }
    EXPECT_EQ_U32(LARGE, local_record.lengths[SAMPLES / 2]);
    EXPECT_EQ_INT(0, local_record.corrupt);
    // No remote reader is matched: the local Subscriber has every sample and nothing went on the wire (2026-10-15).
    EXPECT_EQ_INT(0, test_mock_send_call_count);
    EXPECT_EQ_U32(SAMPLES, (uint32_t)local.tx_unmatched_dropped);
}

static void test_local_and_remote_each_once(void) {
    setup();
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&local), &pub, &topic, "rt/g9",
                                                      &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&local), &sub, &topic, "rt/g9",
                                            &TEST_QOS_UNRELIABLE, &(struct tt_SubscriberOptions) {.callback = on_local},
                                            sub_storage, sizeof(sub_storage)));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&remote), &remote_sub, &topic, "rt/g9",
                                                       &TEST_QOS_UNRELIABLE,
                                                       &(struct tt_SubscriberOptions) {.callback = on_remote},
                                                       remote_storage, sizeof(remote_storage)));
    captured_count = 0;
    test_mock_send_hook = capture;
    // The remote Subscriber's announce reaches the Publisher first: a Publisher with no matched reader sends nothing
    // (2026-10-15), so the remote one must be matched for the link to carry anything.
    build_and_send_update(&remote, NULL, 0);
    EXPECT_TRUE(flush_tx(&remote, remote.tx_tail, NULL, 0));
    carry_captured(&local, LOCAL_IP + 1);
    EXPECT_EQ_INT(1, (int)count_peers(pub.peers));
    captured_count = 0;
    for (uint32_t i = 0; i < SAMPLES; i++) {
        publish(i, i == 2 ? LARGE : SMALL);
    }
    node_flush(&local, 0, NULL);
    test_mock_send_hook = NULL;
    carry_captured(&remote, LOCAL_IP + 1); // over the link
    carry_captured(&local, LOCAL_IP);      // the context's own datagrams, looped back
    EXPECT_EQ_INT(SAMPLES, local_record.count);
    EXPECT_EQ_INT(SAMPLES, remote_record.count);
    for (uint32_t i = 0; i < SAMPLES; i++) {
        EXPECT_EQ_U32(i, local_record.ids[i]);
        EXPECT_EQ_U32(i, remote_record.ids[i]);
    }
    EXPECT_EQ_INT(0, local_record.corrupt + remote_record.corrupt);
}

static void test_an_incompatible_local_pair(void) {
    setup();
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&local), &pub, &topic, "rt/g9",
                                                      &TEST_QOS_UNRELIABLE, NULL, NULL, 0)); // BEST_EFFORT
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&local), &sub, &topic, "rt/g9",
                                                       &TEST_QOS_RELIABLE_READER, // RELIABLE
                                                       &(struct tt_SubscriberOptions) {.callback = on_local},
                                                       sub_storage, sizeof(sub_storage)));
    publish(1, SMALL);
    EXPECT_EQ_INT(0, local_record.count);
}

static void test_a_late_durable_subscriber(void) {
    setup();
    create_durable_publisher();
    publish(0, SMALL);
    publish(1, LARGE);
    publish(2, SMALL);
    struct tt_QosProfile durable = TEST_QOS_RELIABLE_READER;
    durable.durability.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&local), &sub, &topic, "rt/g9", &durable,
                                                       &(struct tt_SubscriberOptions) {.callback = on_local},
                                                       sub_storage, sizeof(sub_storage)));
    EXPECT_EQ_INT(0, local_record.count); // nothing before the backlog is asked for
    tt_Subscriber_deliver_local_backlog(&sub);
    EXPECT_EQ_INT(3, local_record.count);
    for (uint32_t i = 0; i < 3; i++) {
        EXPECT_EQ_U32(i, local_record.ids[i]);
    }
    EXPECT_EQ_U32(LARGE, local_record.lengths[1]);
    EXPECT_EQ_INT(0, local_record.corrupt);
    publish(3, SMALL); // then live, as any other
    EXPECT_EQ_INT(4, local_record.count);
}

static void test_a_late_durable_subscriber_across_2_32(void) {
    // test_a_late_durable_subscriber with the writer short of 2^32: the backlog's seq_no 2^32 - 2 .. 3 (the fragmented
    // sample takes four of them, two either side) is walked oldest to newest as if it had not wrapped. Compared as
    // plain unsigned numbers, the newest (3) is behind the oldest and the walk delivers nothing
    // (tests/mutants_seq_wrap.py: deliver_backlog_from() is compiled only where tt_LOCAL_DELIVERY is).
    setup();
    create_durable_publisher();
    pub.seq_no = UINT32_MAX - 2U; // the next sample takes seq_no 2^32 - 2
    publish(0, SMALL);
    publish(1, LARGE);
    publish(2, SMALL);
    EXPECT_EQ_U32(3, pub.seq_no); // the run did cross 2^32
    struct tt_QosProfile durable = TEST_QOS_RELIABLE_READER;
    durable.durability.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&local), &sub, &topic, "rt/g9", &durable,
                                                       &(struct tt_SubscriberOptions) {.callback = on_local},
                                                       sub_storage, sizeof(sub_storage)));
    tt_Subscriber_deliver_local_backlog(&sub);
    EXPECT_EQ_INT(3, local_record.count);
    for (uint32_t i = 0; i < 3; i++) {
        EXPECT_EQ_U32(i, local_record.ids[i]);
    }
    EXPECT_EQ_U32(LARGE, local_record.lengths[1]);
    EXPECT_EQ_INT(0, local_record.corrupt);
}

// The same publish from two contexts alike but for a local Subscriber: the same datagrams.
static void test_a_local_subscriber_changes_nothing_sent(void) {
    setup();
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&local), &pub, &topic, "rt/g9",
                                                      &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    captured_count = 0;
    test_mock_send_hook = capture;
    publish(7, SMALL);
    publish(8, LARGE);
    test_mock_send_hook = NULL;
    int without_count = captured_count;
    static uint8_t without[CAPTURE_MAX][tt_MAX_BUFFER_LENGTH * 2];
    static size_t without_length[CAPTURE_MAX];
    memcpy(without, captured, sizeof(without));
    memcpy(without_length, captured_length, sizeof(without_length));

    setup();
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&local), &pub, &topic, "rt/g9",
                                                      &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&local), &sub, &topic, "rt/g9",
                                            &TEST_QOS_UNRELIABLE, &(struct tt_SubscriberOptions) {.callback = on_local},
                                            sub_storage, sizeof(sub_storage)));
    captured_count = 0;
    test_mock_send_hook = capture;
    publish(7, SMALL);
    publish(8, LARGE);
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(without_count, captured_count);
    int differing = 0;
    for (int i = 0; i < captured_count && i < without_count; i++) {
        differing +=
            captured_length[i] != without_length[i] || memcmp(captured[i], without[i], captured_length[i]) != 0 ? 1 : 0;
    }
    EXPECT_EQ_INT(0, differing);
    EXPECT_EQ_INT(2, local_record.count);
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_a_local_pair();
    test_local_and_remote_each_once();
    test_an_incompatible_local_pair();
    test_a_late_durable_subscriber();
    test_a_late_durable_subscriber_across_2_32();
    test_a_local_subscriber_changes_nothing_sent();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_local_delivery: all tests passed\n");
    return 0;
}
