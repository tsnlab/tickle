/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// QoS plan D1, D8, D9 (tt_VERSION 14): the announce carries each topic endpoint's LIFESPAN and history depth (and a
// Subscriber's or BEST_EFFORT writer's KEEP_ALL), and the discovery table keeps them.
//
// What must hold, decided before the code:
//   1. the record is the one DESIGN.md section 3 gives: 40 bytes, lifespan_duration_ns at 28, history_depth at 36;
//   2. an announce from one context, applied by another, leaves in the discovery table exactly the lifespan, depth
//      and history kind each endpoint resolved to (get_actual_qos) - the control: a RELIABLE KEEP_LAST writer and
//      a writer with no lifespan read back 0 and no KEEP_ALL, so a match is the value, not a constant;
//   3. a v13 announce is refused by this v14 context, the whole datagram, counted as a version mismatch, nothing in
//      the table; the control, this tree's own v14 announce (the fuzz corpus seed), is applied. The other direction,
//      a v13 build refusing a v14 announce, is tests/version_mix.sh, which builds the parent.

#include <stddef.h>
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

#ifndef VERSION_FIXTURES_DIR
#define VERSION_FIXTURES_DIR "../../tests"
#endif

#define A_ID 1
#define B_ID 2
#define A_IP 0x0a000001
#define B_IP 0x0a000002
#define PORT 8282
#define TOPIC "lifespan_topic"
#define SAMPLE_BYTES 64
#define MS tt_MILLISECOND

static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return SAMPLE_BYTES;
}

static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0, len);
    return SAMPLE_BYTES;
}

static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

// Which writer each delivery came from, in order.
#define MAX_DELIVERIES 16
static uint32_t delivered_entity[MAX_DELIVERIES];
static int deliveries;

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)seq_no;
    (void)data;
    if (deliveries < MAX_DELIVERIES) {
        uint8_t source = 0;
        tt_Subscriber_delivering_writer(sub, &source, &delivered_entity[deliveries]);
    }
    deliveries++;
}

static struct tt_Context a;
static struct tt_Context b;
static struct tt_Discovery a_discovery;
static struct tt_Discovery b_discovery;
static struct tt_TypeSupport type;

static uint64_t arena[((size_t)1024 * 1024) / sizeof(uint64_t)];
static size_t arena_used;

static void* carve(size_t bytes) {
    size_t at = arena_used;
    arena_used += (bytes + 7U) & ~(size_t)7U;
    EXPECT_TRUE(arena_used <= sizeof(arena));
    return bytes == 0 ? NULL : (uint8_t*)arena + at;
}

static void init_bare_node(struct tt_Context* node, uint8_t id, struct tt_Discovery* discovery) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
    memset(discovery, 0, sizeof(*discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(node, discovery, NULL, NULL));
}

static void init(void) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    init_bare_node(&a, A_ID, &a_discovery);
    init_bare_node(&b, B_ID, &b_discovery);
    memset(&type, 0, sizeof(type));
    type.name = "lifespan_type";
    type.data_size = 8;
    type.data_encode_size = sample_encode_size;
    type.data_encode = sample_encode;
    type.data_decode = sample_decode;
    type.data_free = free_nothing;
    type.max_encoded_size = SAMPLE_BYTES;
    arena_used = 0;
    deliveries = 0;
}

// --- a lossless simulated link, as test_qos_incompatible.c's --------------------------------------------------------

#define SIM_QUEUE 64
struct sim_datagram {
    uint8_t from;
    uint32_t len;
    uint8_t bytes[tt_MAX_BUFFER_LENGTH];
};
static struct sim_datagram sim_queue[SIM_QUEUE];
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;

static void sim_capture(const void* buf, size_t len) {
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE && len <= tt_MAX_BUFFER_LENGTH);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
}

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        struct tt_Context* to = d->from == A_ID ? &b : &a;
        sim_acting = to->id;
        EXPECT_TRUE(process_packet(to, d->bytes, 0, d->len, d->from == A_ID ? A_IP : B_IP, PORT, tt_TRANSPORT_UDP));
    }
}

static struct tt_Context* to_other(const struct tt_Context* node) {
    return node == &a ? &b : &a;
}

static void flush_and_drain(struct tt_Context* from) {
    sim_acting = from->id;
    state_lock(from);
    node_flush(from, test_mock_now, NULL);
    state_unlock(from);
    sim_drain();
    sim_acting = to_other(from)->id;
    state_lock(to_other(from));
    node_flush(to_other(from), test_mock_now, NULL);
    state_unlock(to_other(from));
    sim_drain();
}

static void announce(struct tt_Context* from) {
    test_mock_send_hook = sim_capture;
    sim_head = sim_tail = 0;
    sim_acting = from->id;
    state_lock(from);
    EXPECT_TRUE(build_and_send_update(from, NULL, 0));
    state_unlock(from);
    flush_and_drain(from);
    test_mock_send_hook = NULL;
    test_mock_now += MS;
}

static struct tt_QosProfile profile(enum tt_QosHistoryKind history, uint32_t depth, uint64_t lifespan_ns,
                                    enum tt_QosReliabilityKind reliability) {
    struct tt_QosProfile qos;
    memset(&qos, 0, sizeof(qos));
    qos.history.kind = history;
    qos.history.depth = depth;
    qos.lifespan.duration_ns = lifespan_ns;
    qos.reliability.kind = reliability;
    return qos;
}

static void create_pub(struct tt_Context* context, struct tt_Publisher* pub, const struct tt_QosProfile* qos) {
    test_mock_now += MS; // each create a generation of its own
    size_t bytes = tt_Publisher_storage_size(&type, qos);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(context), pub, &type, TOPIC, qos, NULL,
                                                      carve(bytes), bytes));
}

static void create_sub(struct tt_Context* context, struct tt_Subscriber* sub, const struct tt_QosProfile* qos) {
    test_mock_now += MS;
    size_t bytes = tt_Subscriber_storage_size(&type, qos);
    struct tt_SubscriberOptions options = {.callback = on_data};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(context), sub, &type, TOPIC, qos,
                                                       &options, carve(bytes), bytes));
}

static const struct tt_DiscoveredEntity* entry_of(const struct tt_Discovery* discovery, uint8_t context_id,
                                                  const struct tt_Endpoint* endpoint) {
    return tt_Discovery_find_entity(discovery, context_id, endpoint->id, endpoint->entity_id);
}

// --- 1. the record
// ----------------------------------------------------------------------------------------------------

static void test_the_record_is_the_designed_one(void) {
    EXPECT_EQ_U32(14, tt_VERSION);
    EXPECT_EQ_U32(40, sizeof(struct tt_UpdateEntity));
    EXPECT_EQ_U32(28, offsetof(struct tt_UpdateEntity, lifespan_duration_ns));
    EXPECT_EQ_U32(36, offsetof(struct tt_UpdateEntity, history_depth));
}

// --- 2. the announce carries what each endpoint resolved to ---------------------------------------------------------

static void test_the_announce_carries_lifespan_depth_and_history(void) {
    init();
    static struct tt_Publisher pub_lifespan;
    static struct tt_Publisher pub_keep_all;
    static struct tt_Publisher pub_reliable; // control: RELIABLE KEEP_LAST, no lifespan
    static struct tt_Subscriber sub_lifespan;
    static struct tt_Subscriber sub_keep_all;
    struct tt_QosProfile q1 = profile(tt_QOS_HISTORY_KEEP_LAST, 7, 300 * MS, tt_QOS_RELIABILITY_BEST_EFFORT);
    struct tt_QosProfile q2 = profile(tt_QOS_HISTORY_KEEP_ALL, 0, 0, tt_QOS_RELIABILITY_BEST_EFFORT);
    struct tt_QosProfile q3 = profile(tt_QOS_HISTORY_KEEP_LAST, 3, 0, tt_QOS_RELIABILITY_RELIABLE);
    struct tt_QosProfile q4 = profile(tt_QOS_HISTORY_KEEP_LAST, 5, 50 * MS, tt_QOS_RELIABILITY_BEST_EFFORT);
    struct tt_QosProfile q5 = profile(tt_QOS_HISTORY_KEEP_ALL, 0, 0, tt_QOS_RELIABILITY_BEST_EFFORT);
    create_pub(&a, &pub_lifespan, &q1);
    create_pub(&a, &pub_keep_all, &q2);
    create_pub(&a, &pub_reliable, &q3);
    create_sub(&a, &sub_lifespan, &q4);
    create_sub(&a, &sub_keep_all, &q5);
    announce(&a);

    struct {
        const struct tt_Endpoint* endpoint;
        uint64_t lifespan_ns;
        bool keep_all;
    } expected[] = {
        {(const struct tt_Endpoint*)&pub_lifespan, 300 * MS, false},
        {(const struct tt_Endpoint*)&pub_keep_all, 0, true},
        {(const struct tt_Endpoint*)&pub_reliable, 0, false},
        {(const struct tt_Endpoint*)&sub_lifespan, 50 * MS, false},
        {(const struct tt_Endpoint*)&sub_keep_all, 0, true},
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const struct tt_Endpoint* endpoint = expected[i].endpoint;
        struct tt_QosProfile actual;
        if (endpoint->kind == tt_KIND_TOPIC_PUBLISHER) {
            EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos((const struct tt_Publisher*)endpoint, &actual));
        } else {
            EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_get_actual_qos((const struct tt_Subscriber*)endpoint, &actual));
        }
        const struct tt_DiscoveredEntity* entry = entry_of(&b_discovery, A_ID, endpoint);
        EXPECT_TRUE(entry != NULL);
        if (entry == NULL) {
            continue;
        }
        printf("test_lifespan_announce: entity %zu: lifespan %llu ns, depth %u (actual %u), KEEP_ALL %d\n", i,
               (unsigned long long)entry->lifespan_duration_ns, entry->history_depth, actual.history.depth,
               (entry->qos & tt_UPDATE_QOS_KEEP_ALL) != 0);
        EXPECT_TRUE(entry->lifespan_duration_ns == expected[i].lifespan_ns);
        EXPECT_EQ_U32(actual.history.depth, entry->history_depth);
        EXPECT_TRUE(actual.history.depth != 0); // a real depth, so the equality above decides something
        EXPECT_EQ_INT(expected[i].keep_all, (entry->qos & tt_UPDATE_QOS_KEEP_ALL) != 0);
        EXPECT_EQ_INT(expected[i].keep_all, actual.history.kind == tt_QOS_HISTORY_KEEP_ALL);
    }
    EXPECT_EQ_U32(7, entry_of(&b_discovery, A_ID, (const struct tt_Endpoint*)&pub_lifespan)->history_depth);
    EXPECT_EQ_U32(5, entry_of(&b_discovery, A_ID, (const struct tt_Endpoint*)&sub_lifespan)->history_depth);
}

// --- 3. mixed versions refuse each other
// ------------------------------------------------------------------------------

static size_t read_file(const char* path, uint8_t* buf, size_t size) {
    FILE* f = fopen(path, "rb");
    EXPECT_TRUE(f != NULL);
    if (f == NULL) {
        return 0;
    }
    size_t len = fread(buf, 1, size, f);
    (void)fclose(f);
    return len;
}

static uint32_t table_entries(const struct tt_Discovery* discovery) {
    uint32_t n = 0;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        n += discovery->entities[i].context_id != tt_CONTEXT_ID_INVALID ? 1U : 0U;
    }
    return n;
}

static void test_a_v13_announce_is_refused(void) {
    static uint8_t buf[tt_MAX_BUFFER_LENGTH];
    // The seeds come from node 2 (gen_fuzz_corpus.c), so context `a` (node 1) applies them.
    // Control: this tree's own announce (the fuzz corpus seed `make fuzz-corpus` writes from the real encoder).
    init();
    size_t len = read_file(VERSION_FIXTURES_DIR "/fuzz_corpus/announce_single.bin", buf, sizeof(buf));
    EXPECT_TRUE(len > sizeof(struct tt_SingleHeader) && buf[1] == tt_VERSION);
    uint8_t source = buf[2];
    (void)process_packet(&a, buf, 0, (uint32_t)len, B_IP, PORT, tt_TRANSPORT_UDP);
    uint32_t own_entries = table_entries(&a_discovery);
    EXPECT_TRUE(own_entries > 0);
    EXPECT_TRUE(a.version_mismatch_drops == 0);

    // Treatment: the same seed as tt_VERSION 13 wrote it (tests/wire_identity/announce_v13.bin, from the parent).
    init();
    len = read_file(VERSION_FIXTURES_DIR "/wire_identity/announce_v13.bin", buf, sizeof(buf));
    EXPECT_TRUE(len > sizeof(struct tt_SingleHeader) && buf[1] == 13);
    EXPECT_TRUE(!process_packet(&a, buf, 0, (uint32_t)len, B_IP, PORT, tt_TRANSPORT_UDP));
    EXPECT_EQ_U32(0, table_entries(&a_discovery));
    EXPECT_TRUE(a.version_mismatch_drops == 1);
    EXPECT_EQ_U32(13, a.version_mismatch_logged[buf[2]]);
    EXPECT_TRUE(!a.update_seen[source]);
    printf("test_lifespan_announce: v14 announce applied (%u entities), v13 announce refused (%llu version drop)\n",
           own_entries, (unsigned long long)a.version_mismatch_drops);
}

int main(void) {
    tt_current_log_level = getenv("LIFESPAN_DEBUG") != NULL ? TT_LOG_DEBUG : TT_LOG_NONE;
    test_the_record_is_the_designed_one();
    test_the_announce_carries_lifespan_depth_and_history();
    test_a_v13_announce_is_refused();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_lifespan_announce: all tests passed\n");
    return 0;
}
