/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// tt_InitOptions bundle 3 (ROADMAP "Context options"): with the default reliable options a RELIABLE writer and reader
// send what they sent when the reliability timing was config.h's constants - every datagram, both ways, its bytes and
// the mock-clock instant it went.
//
// test_wire_identity_initoptions.c's scenario never retries anything, so it cannot see this group: changing any of its
// defaults leaves that capture as it was. This one is a lossy exchange on the mock link (test_idle_heartbeat.c's
// shape) that runs every reader of the group at its default: samples lost and repaired on the dynamic retry interval,
// a reader whose ACKNACKs are lost long enough to back off, a sample whose repairs are lost past the writer's resend
// count (its suppression), and a lost tail with its HEARTBEATs lost, found by the idle HEARTBEAT.
//
//   - the golden, tests/wire_identity/reliable_parent.bin, is the parent's capture (580a5ec8, before the group);
//   - this tree's capture with the defaults (tt_InitOptions_default()) and with every field set explicitly to its
//     config.h constant must both equal it.
// tests/wire_identity_initoptions_reliable.sh rebuilds the golden from the parent, checks the parent against itself,
// and that changing one default of the group is seen. Built with -DWIRE_CAPTURE_OUT=path (and -DTICKLE_C= a tree's
// tickle.c) it writes the default arm's capture instead.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox: the parent's tickle.c, or this tree's

#ifndef WIRE_GOLDEN
#define WIRE_GOLDEN "../../tests/wire_identity/reliable_parent.bin"
#endif

// This tree has the reliable group; the parent, built from this same source, has its placeholder.
#ifdef tt_RELIABLE_NO_BACKOFF
#define WIRE_HAS_RELIABLE_OPTIONS 1
#else
#define WIRE_HAS_RELIABLE_OPTIONS 0
#endif

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define RECEIVER_IP 0x0a000002
#define PORT 8282
#define TOPIC_NAME "wire_reliable"
#define ENDPOINT_NAME "wire_reliable_endpoint"
#define CACHE_DEPTH 64
#define WORDS 2

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

static int delivered_count;

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)seq_no;
    (void)data;
    delivered_count++;
}

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_TypeSupport sender_topic;
static struct tt_TypeSupport receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static uint64_t pub_storage[(size_t)64U * 1024U / sizeof(uint64_t)];
static uint64_t sub_storage[(size_t)256U * 1024U / sizeof(uint64_t)];

// ---- The capture: one record a datagram - the instant, who sent it, its length, its bytes.

#define CAPTURE_MAX ((size_t)1024U * 1024U)
struct capture_buffer {
    uint8_t bytes[CAPTURE_MAX];
    uint32_t len;
    uint32_t datagrams;
};
static struct capture_buffer* active;

// ---- The link (test_idle_heartbeat.c's): datagrams queue and are delivered in order, some dropped.

#define SIM_QUEUE 65536U
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

#define MAX_DROPS 16U
static uint32_t drop_values[MAX_DROPS]; // DATA of these sample values, once each entry
static uint32_t drop_count;
static uint32_t drop_lone_heartbeats; // HEARTBEATs on a datagram of their own, until spent
static bool drop_reader;              // every datagram from the reader

// What a datagram of the sender's carries for this Publisher.
static void shape_of(const uint8_t* bytes, uint32_t len, bool* data, bool* heartbeat, uint32_t* value) {
    static uint8_t classic[SIM_DATAGRAM_MAX + sizeof(struct tt_Header)];
    size_t classic_len = test_classic_form(bytes, len, classic);
    uint32_t endpoint = ((struct tt_Endpoint*)&pub)->id;
    size_t at = sizeof(struct tt_Header);
    *data = false;
    *heartbeat = false;
    while (at + sizeof(struct tt_SubmessageHeader) <= classic_len) {
        struct tt_SubmessageHeader submessage;
        memcpy(&submessage, classic + at, sizeof(submessage));
        const uint8_t* body = classic + at + sizeof(submessage);
        if (submessage.type == tt_SUBMESSAGE_TYPE_DATA) {
            struct tt_DataHeader header;
            memcpy(&header, body, sizeof(header));
            if (header.endpoint_id == endpoint) {
                *data = true;
                memcpy(value, body + sizeof(header), sizeof(uint32_t));
            }
        } else if (submessage.type == tt_SUBMESSAGE_TYPE_HEARTBEAT) {
            struct tt_HeartbeatHeader header;
            memcpy(&header, body, sizeof(header));
            *heartbeat = *heartbeat || header.endpoint_id == endpoint;
        }
        if (submessage.length < sizeof(submessage)) {
            break;
        }
        at += submessage.length;
    }
}

static bool carries_acknack(const uint8_t* bytes, uint32_t len) {
    static uint8_t classic[SIM_DATAGRAM_MAX + sizeof(struct tt_Header)];
    size_t classic_len = test_classic_form(bytes, len, classic);
    size_t at = sizeof(struct tt_Header);
    while (at + sizeof(struct tt_SubmessageHeader) <= classic_len) {
        struct tt_SubmessageHeader submessage;
        memcpy(&submessage, classic + at, sizeof(submessage));
        if (submessage.type == tt_SUBMESSAGE_TYPE_ACKNACK) {
            return true;
        }
        if (submessage.length < sizeof(submessage)) {
            break;
        }
        at += submessage.length;
    }
    return false;
}

static void sim_capture(const void* buf, size_t len) {
    if (len > SIM_DATAGRAM_MAX || len == 0) {
        return;
    }
    if (sim_tail - sim_head >= SIM_QUEUE || active->len + 13U + len > CAPTURE_MAX) {
        test_failures++;
        return;
    }
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
    uint32_t length = (uint32_t)len;
    memcpy(active->bytes + active->len, &test_mock_now, 8);
    active->bytes[active->len + 8] = sim_acting;
    memcpy(active->bytes + active->len + 9, &length, 4);
    memcpy(active->bytes + active->len + 13, buf, len);
    active->len += 13U + length;
    active->datagrams++;
}

// Only the Publisher's DATA and HEARTBEATs and the reader's ACKNACKs are delivered (test_idle_heartbeat.c says why).
static bool sim_dropped(const struct sim_datagram* d) {
    if (d->from != SENDER_ID) {
        return drop_reader || !carries_acknack(d->bytes, d->len);
    }
    bool data = false;
    bool heartbeat = false;
    uint32_t value = 0;
    shape_of(d->bytes, d->len, &data, &heartbeat, &value);
    if (!data && !heartbeat) {
        return true;
    }
    if (data) {
        for (uint32_t i = 0; i < drop_count; i++) {
            if (drop_values[i] == value) {
                drop_values[i] = UINT32_MAX;
                return true;
            }
        }
        return false;
    }
    if (drop_lone_heartbeats != 0) {
        drop_lone_heartbeats--;
        return true;
    }
    return false;
}

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        if (sim_dropped(d)) {
            continue;
        }
        struct tt_Context* to = d->from == SENDER_ID ? &receiver : &sender;
        sim_acting = to->id;
        process_packet(to, d->bytes, 0, d->len, d->from == SENDER_ID ? SENDER_IP : RECEIVER_IP, PORT, tt_TRANSPORT_UDP);
    }
}

static void sim_run_due(struct tt_Context* node) {
    bool has_next = false;
    uint64_t next = 0;
    sim_acting = node->id;
    while (run_due_entry(node, test_mock_now, &has_next, &next)) {
    }
    sim_drain();
}

static uint64_t sim_next_due(void) {
    uint64_t a = UINT64_MAX;
    uint64_t b = UINT64_MAX;
    if (!sched_next_time(&sender, &a)) {
        a = UINT64_MAX;
    }
    if (!sched_next_time(&receiver, &b)) {
        b = UINT64_MAX;
    }
    return a < b ? a : b;
}

static void sim_run_until(uint64_t until) {
    for (;;) {
        uint64_t next = sim_next_due();
        if (next > until) {
            break;
        }
        if (next > test_mock_now) {
            test_mock_now = next;
        }
        sim_run_due(&sender);
        sim_run_due(&receiver);
    }
    test_mock_now = until;
    sim_run_due(&sender);
    sim_run_due(&receiver);
}

static void sim_publish(uint32_t value) {
    sim_run_until(test_mock_now);
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    sim_drain();
}

static void drop(uint32_t value) {
    if (drop_count < MAX_DROPS) {
        drop_values[drop_count++] = value;
    }
}

// ---- The two contexts, each with `opts` as creation would resolve them.

static void init_bare_node(struct tt_Context* node, uint8_t id, const struct tt_InitOptions* opts) {
    memset(node, 0, sizeof(*node));
    node->opts = *opts;
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

static void init_pair(const struct tt_InitOptions* opts) {
    test_mock_reset();
    delivered_count = 0;
    init_bare_node(&sender, SENDER_ID, opts);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = sizeof(uint32_t);
    sender_topic.data_encode_size = u32_encode_size;
    sender_topic.data_encode = u32_encode;
    sender_topic.max_encoded_size = sizeof(uint32_t);
    const struct tt_QosProfile pub_qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = CACHE_DEPTH},
                                          .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    size_t pub_storage_size = tt_Publisher_storage_size(&sender_topic, &pub_qos);
    EXPECT_TRUE(pub_storage_size <= sizeof(pub_storage));
    // No Publisher options: the idle HEARTBEAT at the context's default period.
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&sender), &pub, &sender_topic,
                                                      ENDPOINT_NAME, &pub_qos, NULL, pub_storage, pub_storage_size));

    init_bare_node(&receiver, RECEIVER_ID, opts);
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = sizeof(uint32_t);
    receiver_topic.data_decode = u32_decode;
    receiver_topic.data_free = free_nothing;
    receiver_topic.max_encoded_size = sizeof(uint32_t);
    // RELIABLE with a reorder buffer, so a sample past a gap is held and the gap stays open until it is repaired.
    const struct tt_QosProfile sub_qos = {
        .history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = CACHE_DEPTH},
        .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE, .tracking_words = WORDS}};
    size_t sub_storage_size = tt_Subscriber_storage_size(&receiver_topic, &sub_qos);
    EXPECT_TRUE(sub_storage_size <= sizeof(sub_storage));
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub, &receiver_topic, ENDPOINT_NAME,
                                            &sub_qos, &(struct tt_SubscriberOptions) {.callback = on_data},
                                            sub_storage_size != 0 ? sub_storage : NULL, sub_storage_size));
    struct tt_PeerAck* ack = claim_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id);
    EXPECT_TRUE(ack != NULL);
    if (ack != NULL) {
        ack->tracking_words = WORDS;
        ack->reliable_reader = true;
    }
    (void)upsert_peer(pub.peers, RECEIVER_ID, RECEIVER_IP, PORT);
}

static void teardown_pair(void) {
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&sender));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&receiver));
}

static void run_scenario(const struct tt_InitOptions* opts, struct capture_buffer* capture) {
    memset(capture, 0, sizeof(*capture));
    active = capture;
    if (sim_queue == NULL) {
        sim_queue = (struct sim_datagram*)calloc(SIM_QUEUE, sizeof(struct sim_datagram));
    }
    sim_head = sim_tail = 0;
    drop_count = 0;
    drop_lone_heartbeats = 0;
    drop_reader = false;
    init_pair(opts);
    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
    sim_run_until(test_mock_now + tt_MILLISECOND);

    // A first gap whose ACKNACKs are lost: only the retry timer asks again, at the starting interval (no recovery
    // sample yet), and its answer is the first recovery sample.
    drop(3);
    drop_reader = true;
    for (uint32_t v = 1; v <= 5; v++) {
        sim_publish(v);
        sim_run_until(test_mock_now + (100 * tt_MICROSECOND));
    }
    drop_reader = false;
    sim_run_until(test_mock_now + (50 * tt_MILLISECOND));

    // Lost samples repaired on the dynamic interval, one of them lost twice more (its first repairs).
    drop(8);
    drop(12);
    drop(12);
    drop(12);
    for (uint32_t v = 6; v <= 20; v++) {
        sim_publish(v);
        sim_run_until(test_mock_now + (500 * tt_MICROSECOND));
    }
    sim_run_until(test_mock_now + (50 * tt_MILLISECOND));

    // The reader's ACKNACKs lost for 150 ms with a gap open and the writer quiet: its retries back off to the ceiling;
    // then they get through.
    drop(23);
    drop_reader = true;
    for (uint32_t v = 21; v <= 26; v++) {
        sim_publish(v);
    }
    sim_run_until(test_mock_now + (150 * tt_MILLISECOND));
    drop_reader = false;
    sim_run_until(test_mock_now + (100 * tt_MILLISECOND));

    // A sample whose repairs keep being lost: the writer resends it past its resend count, each request at once at
    // first, then at most once per its interval.
    for (uint32_t i = 0; i < 6; i++) {
        drop(30);
    }
    for (uint32_t v = 27; v <= 32; v++) {
        sim_publish(v);
    }
    sim_run_until(test_mock_now + (200 * tt_MILLISECOND));

    // A lost tail, and the HEARTBEATs that would reveal it: found by the idle HEARTBEAT.
    drop(40);
    drop_lone_heartbeats = 2;
    for (uint32_t v = 33; v <= 40; v++) {
        sim_publish(v);
    }
    sim_run_until(test_mock_now + (2 * tt_SECOND));
    EXPECT_EQ_INT(40, delivered_count);
    teardown_pair();
}

static bool same_capture(const struct capture_buffer* a, const uint8_t* b, size_t b_len, const char* what) {
    if (a->len == b_len && memcmp(a->bytes, b, b_len) == 0) {
        return true;
    }
    size_t first = 0;
    while (first < a->len && first < b_len && a->bytes[first] == b[first]) {
        first++;
    }
    printf("  %s: %u vs %zu bytes, first difference at byte %zu\n", what, a->len, b_len, first);
    return false;
}

static struct capture_buffer by_default;
static struct capture_buffer explicit_values;
static uint8_t golden[CAPTURE_MAX];

int main(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    run_scenario(&opts, &by_default);
#ifdef WIRE_CAPTURE_OUT
    FILE* out = fopen(WIRE_CAPTURE_OUT, "wb");
    if (out == NULL || fwrite(by_default.bytes, 1, by_default.len, out) != by_default.len) {
        fprintf(stderr, "cannot write %s\n", WIRE_CAPTURE_OUT);
        return 1;
    }
    (void)fclose(out);
    printf("wire capture: %u datagrams, %u bytes -> %s\n", by_default.datagrams, by_default.len, WIRE_CAPTURE_OUT);
    return test_result();
#elif WIRE_HAS_RELIABLE_OPTIONS
    // Every field set to the constant its zero stands for.
    struct tt_InitOptions spelled;
    tt_InitOptions_default(&spelled);
    spelled.reliable = (struct tt_ReliableOptions) {
        .deadline = tt_RELIABLE_DEADLINE,
        .retry_interval = tt_RELIABLE_RETRY_INTERVAL,
        .retry_initial = tt_RELIABLE_RETRY_INITIAL,
        .retry_granularity = tt_RELIABLE_RETRY_GRANULARITY,
        .timer_lateness_initial = tt_TIMER_LATENESS_INITIAL,
        .stuck_warn_interval = tt_RELIABLE_STUCK_WARN_INTERVAL,
        .idle_heartbeat_period = tt_IDLE_HEARTBEAT_PERIOD,
        .max_srtt_multiple = tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE,
        .idle_heartbeat_retry_multiple = tt_IDLE_HEARTBEAT_RETRY_MULTIPLE,
        .retry = tt_RELIABLE_RETRY,
        .backoff_shift = tt_RELIABLE_RETRY_BACKOFF_SHIFT,
    };
    run_scenario(&spelled, &explicit_values);
    EXPECT_TRUE(same_capture(&explicit_values, by_default.bytes, by_default.len, "spelled out vs defaults"));
    FILE* in = fopen(WIRE_GOLDEN, "rb");
    EXPECT_TRUE(in != NULL);
    size_t golden_len = in != NULL ? fread(golden, 1, sizeof(golden), in) : 0;
    if (in != NULL) {
        (void)fclose(in);
    }
    EXPECT_TRUE(golden_len > 0);
    EXPECT_TRUE(same_capture(&by_default, golden, golden_len, "defaults vs the parent's golden"));
    if (test_result() != 0) {
        return 1;
    }
    printf("test_wire_identity_reliable: %u datagrams (%u bytes, both ways, with their instants) with the default and "
           "the spelled-out reliable options byte-identical to the parent's\n",
           by_default.datagrams, by_default.len);
    return 0;
#else
    return test_result();
#endif
}
