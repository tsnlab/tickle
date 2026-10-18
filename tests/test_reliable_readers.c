/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// RELIABLE readers per publisher (config.h's tt_MAX_ACK_ENTRIES; the user's decision 2026-10-16): a compile-time
// limit, which rmw_tickle's build sets from RMW_TICKLE_MAX_RELIABLE_READERS. Built here with the limit raised to 24:
// 20 RELIABLE Subscribers are matched by the announce path (register_subscriber_peer_on_publisher()), all receive every
// sample, and every one's acknowledgement is counted. test_reliable_readers_default.c builds this same file at the
// default 16 and checks that the 17th is refused - the behaviour kept - and the 16 before it still receive.
//
// Two contexts in one process over the mock HAL, as test_idle_heartbeat.c: the 20 Subscribers share the reader
// context, so the writer has one peer and tracks 20 ack entries.

#ifndef TEST_DEFAULT_LIMIT
#define tt_MAX_ACK_ENTRIES 24 // as a build with RMW_TICKLE_MAX_RELIABLE_READERS=24 sets it
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define RECEIVER_IP 0x0a000002
#define PORT 8282
#define TOPIC_NAME "readers_topic"
#define ENDPOINT_NAME "readers_endpoint"
#define READERS 20
#define SAMPLES 10U
#define WORDS 1 // a 64-sample window, within the writer's depth

static int32_t u32_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}

static int32_t u32_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static int32_t u32_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_TypeSupport sender_topic;
static struct tt_TypeSupport receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber subs[READERS];
static uint64_t tracking[READERS][tt_MAX_PEER_COUNT * WORDS];
static int delivered[READERS];

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)seq_no;
    (void)data;
    delivered[s - subs]++;
}

#define CACHE_DEPTH 64 // samples retained: one datagram each
static uint64_t pub_storage[(size_t)64U * 1024U / sizeof(uint64_t)];

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

// --- the link: a queue, the writer's DATA/HEARTBEAT one way and the readers' ACKNACKs the other
// ------------------------

#define SIM_QUEUE 4096U
#define SIM_DATAGRAM_MAX 1600U
struct sim_datagram {
    uint8_t from;
    uint32_t len;
    uint8_t bytes[SIM_DATAGRAM_MAX];
};
static struct sim_datagram* sim_queue;
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;

static void sim_capture(const void* buf, size_t len) {
    if (len == 0 || len > SIM_DATAGRAM_MAX) {
        return;
    }
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
}

// Only DATA, HEARTBEAT and ACKNACK go through: the contexts' own announces would run discovery between two bare
// contexts that share a mock address (test_idle_heartbeat.c, carries_acknack()); the match is made below instead.
static bool sim_passes(const struct sim_datagram* d) {
    static uint8_t classic[SIM_DATAGRAM_MAX + sizeof(struct tt_Header)];
    size_t classic_len = test_classic_form(d->bytes, d->len, classic);
    size_t at = sizeof(struct tt_Header);
    while (at + sizeof(struct tt_SubmessageHeader) <= classic_len) {
        struct tt_SubmessageHeader submessage;
        memcpy(&submessage, classic + at, sizeof(submessage));
        if (submessage.type == tt_SUBMESSAGE_TYPE_ACKNACK || submessage.type == tt_SUBMESSAGE_TYPE_HEARTBEAT) {
            return true;
        }
        if (submessage.type == tt_SUBMESSAGE_TYPE_DATA) {
            struct tt_DataHeader data;
            memcpy(&data, classic + at + sizeof(submessage), sizeof(data));
            if (data.endpoint_id == ((struct tt_Endpoint*)&pub)->id) {
                return true;
            }
        }
        if (submessage.length < sizeof(submessage)) {
            break;
        }
        at += submessage.length;
    }
    return false;
}

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        if (!sim_passes(d)) {
            continue;
        }
        struct tt_Context* to = d->from == SENDER_ID ? &receiver : &sender;
        sim_acting = to->id;
        process_packet(to, d->bytes, 0, d->len, d->from == SENDER_ID ? SENDER_IP : RECEIVER_IP, PORT, tt_TRANSPORT_UDP);
    }
}

static void sim_run_until(uint64_t until) {
    for (;;) {
        uint64_t a = UINT64_MAX;
        uint64_t b = UINT64_MAX;
        if (!sched_next_time(&sender, &a)) {
            a = UINT64_MAX;
        }
        if (!sched_next_time(&receiver, &b)) {
            b = UINT64_MAX;
        }
        uint64_t next = a < b ? a : b;
        if (next > until) {
            break;
        }
        if (next > test_mock_now) {
            test_mock_now = next;
        }
        bool has_next = false;
        uint64_t later = 0;
        sim_acting = SENDER_ID;
        while (run_due_entry(&sender, test_mock_now, &has_next, &later)) {
        }
        sim_drain();
        sim_acting = RECEIVER_ID;
        while (run_due_entry(&receiver, test_mock_now, &has_next, &later)) {
        }
        sim_drain();
    }
    test_mock_now = until;
}

// One Subscriber's announce, as decode_update_entities() hands it to the writer: RELIABLE, VOLATILE, its window.
static void announce_reader(struct tt_Subscriber* s) {
    struct tt_Header header = {0};
    header.source = RECEIVER_ID;
    struct update_peer_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.header = &header;
    ctx.sender_ip = RECEIVER_IP;
    ctx.sender_port = PORT;
    ctx.qos = tt_UPDATE_QOS_RELIABLE;
    ctx.entity_id = s->endpoint.entity_id;
    ctx.tracking_words = WORDS;
    ctx.announce_generation = 1;
    register_subscriber_peer_on_publisher(&sender, (struct tt_Endpoint*)&pub, &ctx);
}

int main(void) {
    test_mock_reset();
    sim_queue = (struct sim_datagram*)calloc(SIM_QUEUE, sizeof(struct sim_datagram));
    test_mock_now = tt_SECOND;

    init_bare_node(&sender, SENDER_ID);
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = sizeof(uint32_t);
    sender_topic.data_encode_size = u32_encode_size;
    sender_topic.data_encode = u32_encode;
    sender_topic.max_encoded_size = sizeof(uint32_t);
    const struct tt_QosProfile pub_qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = CACHE_DEPTH},
                                          .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    size_t pub_storage_size = tt_Publisher_storage_size(&sender_topic, &pub_qos);
    EXPECT_TRUE(pub_storage_size <= sizeof(pub_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&sender), &pub, &sender_topic,
                                                      ENDPOINT_NAME, &pub_qos, NULL, pub_storage, pub_storage_size));

    init_bare_node(&receiver, RECEIVER_ID);
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = sizeof(uint32_t);
    receiver_topic.data_decode = u32_decode;
    receiver_topic.data_free = free_nothing;
    receiver_topic.max_encoded_size = sizeof(uint32_t);
    struct tt_QosProfile sub_qos = TEST_QOS_RELIABLE_READER;
    sub_qos.reliability.tracking_words = WORDS;
    size_t sub_storage_size = tt_Subscriber_storage_size(&receiver_topic, &sub_qos);
    EXPECT_TRUE(sub_storage_size <= sizeof(tracking[0]));
    for (int i = 0; i < READERS; i++) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(
                                     tt_Context_default_node(&receiver), &subs[i], &receiver_topic, ENDPOINT_NAME,
                                     &sub_qos, &(struct tt_SubscriberOptions) {.callback = on_data},
                                     sub_storage_size != 0 ? tracking[i] : NULL, sub_storage_size));
    }
    test_mock_send_hook = sim_capture;
    sim_run_until(test_mock_now + tt_MILLISECOND); // whatever creation scheduled

    for (int i = 0; i < READERS; i++) {
        announce_reader(&subs[i]);
    }
    // Matched: every Subscriber up to the limit, none past it.
    int matched = 0;
    for (int i = 0; i < READERS; i++) {
        bool has_entry = find_peer_ack(&pub, RECEIVER_ID, subs[i].endpoint.entity_id) != NULL;
        EXPECT_TRUE(has_entry == (i < tt_MAX_ACK_ENTRIES));
        matched += has_entry ? 1 : 0;
    }
    int expected = READERS < tt_MAX_ACK_ENTRIES ? READERS : tt_MAX_ACK_ENTRIES;
    EXPECT_EQ_INT(expected, matched);

    for (uint32_t v = 1; v <= SAMPLES; v++) {
        sim_acting = SENDER_ID;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&v));
        sim_drain();
        sim_run_until(test_mock_now + tt_MILLISECOND);
    }
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_request_ack(&pub));
    sim_drain();
    sim_run_until(test_mock_now + tt_SECOND);

    // Every Subscriber on the reader context receives every sample - the ones refused at the writer too, because the
    // writer's datagrams go to the reader context and are delivered to each matching Subscriber there. What the
    // refusal takes away is the writer counting that Subscriber's acknowledgement: a KEEP_ALL writer would not wait
    // for it, and a repair it alone needs is answered (process_acknack() answers any ACKNACK) but never awaited.
    for (int i = 0; i < READERS; i++) {
        EXPECT_EQ_INT((int)SAMPLES, delivered[i]);
    }
    // Every matched Subscriber's acknowledgement is counted: each entry reached the last sample.
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        if (pub.peer_acks[i].context_id != tt_CONTEXT_ID_INVALID) {
            EXPECT_TRUE(pub.peer_acks[i].acked);
            EXPECT_EQ_U32(SAMPLES + 1U, pub.peer_acks[i].ack_seq_no);
        }
    }
    printf("test_reliable_readers: limit %d, %d of %d RELIABLE Subscribers matched, each acknowledged %u samples\n",
           tt_MAX_ACK_ENTRIES, matched, READERS, SAMPLES);

    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&sender));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&receiver));
    free(sim_queue);
    if (test_result() != 0) {
        return 1;
    }
    printf("test_reliable_readers: all tests passed\n");
    return 0;
}
