/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// QoS compatibility as an event (QoS plan D2): judged when an announce is applied and when an endpoint is created,
// counted once per incompatible pair on both of its endpoints, and reported through the context's callback.
//
// What must hold, decided before the code:
//   1. the judgement agrees with what delivery refuses: for every combination of the five RxO fields on both sides,
//      qos_incompatible_policy() names a policy exactly when writer_qos_incompatible() refuses the writer (and, in a
//      tt_LOCAL_DELIVERY build, local_pair_incompatible() the local pair), and the policy is the first failing one;
//   2. an incompatible pair counts once on each side, with its policy - the falsifier of the plan is an event firing
//      twice for one incompatibility: here a repeated announce, a replayed one (a new generation of the same source
//      listing the same endpoint) and many announce rounds must all leave the counts where the first left them;
//   3. a new incompatible endpoint of the other side counts again, and an endpoint created after the other side is
//      known counts at its creation, local pairs included;
//   4. the control: a compatible pair on another topic of the same two contexts counts nothing although discovery
//      shows it - so a zero there is a judgement, not an announce that never arrived.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define A_ID 1
#define B_ID 2
#define A_IP 0x0a000001
#define B_IP 0x0a000002
#define PORT 8282
#define TOPIC "qos_incompatible_topic"
#define CONTROL_TOPIC "qos_compatible_topic"
#define SAMPLE_BYTES 64

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

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}

static struct tt_Context a;
static struct tt_Context b;
static struct tt_Discovery a_discovery;
static struct tt_Discovery b_discovery;
static struct tt_TypeSupport type;

// Caller storage for every endpoint of a test, handed out in order.
static uint64_t arena[((size_t)1024 * 1024) / sizeof(uint64_t)];
static size_t arena_used;

static void* carve(size_t bytes) {
    size_t at = arena_used;
    arena_used += (bytes + 7U) & ~(size_t)7U;
    EXPECT_TRUE(arena_used <= sizeof(arena));
    return bytes == 0 ? NULL : (uint8_t*)arena + at;
}

// Every callback, in order.
#define MAX_EVENTS 64
static struct {
    struct tt_Context* node;
    struct tt_Endpoint* endpoint;
    uint8_t policy_kind;
} events[MAX_EVENTS];
static int event_count;

static void on_incompatible(struct tt_Context* node, struct tt_Endpoint* endpoint, uint8_t policy_kind, void* param) {
    EXPECT_TRUE(param == &event_count);
    if (event_count < MAX_EVENTS) {
        events[event_count].node = node;
        events[event_count].endpoint = endpoint;
        events[event_count].policy_kind = policy_kind;
    }
    event_count++;
}

static int events_for(const void* endpoint) {
    int n = 0;
    for (int i = 0; i < event_count && i < MAX_EVENTS; i++) {
        n += events[i].endpoint == (const struct tt_Endpoint*)endpoint ? 1 : 0;
    }
    return n;
}

static void init_bare_node(struct tt_Context* node, uint8_t id, struct tt_Discovery* discovery) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
    memset(discovery, 0, sizeof(*discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(node, discovery, NULL, NULL));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_qos_incompatible_callback(node, on_incompatible, &event_count));
}

static void init(void) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    init_bare_node(&a, A_ID, &a_discovery);
    init_bare_node(&b, B_ID, &b_discovery);
    memset(&type, 0, sizeof(type));
    type.name = "qos_incompatible_type";
    type.data_size = 8;
    type.data_encode_size = sample_encode_size;
    type.data_encode = sample_encode;
    type.data_decode = sample_decode;
    type.data_free = free_nothing;
    type.max_encoded_size = SAMPLE_BYTES;
    arena_used = 0;
    event_count = 0;
}

// --- a lossless simulated link: every datagram one context sends reaches the other ------------------------------

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

// `from` sends its whole announce now, and every reply it draws is delivered too.
static void announce(struct tt_Context* from) {
    test_mock_send_hook = sim_capture;
    sim_head = sim_tail = 0;
    sim_acting = from->id;
    state_lock(from);
    EXPECT_TRUE(build_and_send_update(from, NULL, 0));
    node_flush(from, test_mock_now, NULL); // the broadcast announce is batched until a flush
    state_unlock(from);
    sim_drain();
    sim_acting = to_other(from)->id; // a reply it drew is batched on the other side: flushed too
    state_lock(to_other(from));
    node_flush(to_other(from), test_mock_now, NULL);
    state_unlock(to_other(from));
    sim_drain();
    test_mock_send_hook = NULL;
    test_mock_now += tt_MILLISECOND;
}

static void both_announce(void) {
    announce(&a);
    announce(&b);
}

static struct tt_Publisher* create_pub(struct tt_Context* context, struct tt_Publisher* pub, const char* topic,
                                       const struct tt_QosProfile* qos) {
    test_mock_now += tt_MILLISECOND; // each create a generation of its own
    size_t bytes = tt_Publisher_storage_size(&type, qos);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(context), pub, &type, topic, qos, NULL,
                                                      carve(bytes), bytes));
    return pub;
}

static struct tt_Subscriber* create_sub(struct tt_Context* context, struct tt_Subscriber* sub, const char* topic,
                                        const struct tt_QosProfile* qos) {
    test_mock_now += tt_MILLISECOND;
    size_t bytes = tt_Subscriber_storage_size(&type, qos);
    struct tt_SubscriberOptions options = {.callback = on_data};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(context), sub, &type, topic, qos,
                                                       &options, carve(bytes), bytes));
    return sub;
}

static const struct tt_QosProfile reliable = {.reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
static const struct tt_QosProfile best_effort = {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}};

// --- 1. the judgement is delivery's -------------------------------------------------------------------------------

static const uint64_t durations[] = {0, tt_MILLISECOND, 2 * tt_MILLISECOND};
#define DURATIONS ((int)(sizeof(durations) / sizeof(durations[0])))
#define RXO_COMBINATIONS (8 * DURATIONS * DURATIONS)

static struct qos_rxo rxo_number(int n) {
    struct qos_rxo rxo = {.reliable = (n & 1) != 0,
                          .durable = (n & 2) != 0,
                          .manual = (n & 4) != 0,
                          .deadline_ns = durations[(n / 8) % DURATIONS],
                          .lease_ns = durations[(n / (8 * DURATIONS)) % DURATIONS]};
    return rxo;
}

// The first failing policy, written out per policy rather than as one expression, so that a wrong order shows.
static uint8_t expected_policy(const struct qos_rxo* requested, const struct qos_rxo* offered) {
    bool reliability = requested->reliable && !offered->reliable;
    bool durability = requested->durable && !offered->durable;
    bool deadline =
        requested->deadline_ns != 0 && (offered->deadline_ns == 0 || offered->deadline_ns > requested->deadline_ns);
    bool kind = requested->manual && !offered->manual;
    bool lease = requested->lease_ns != 0 && (offered->lease_ns == 0 || offered->lease_ns > requested->lease_ns);
    if (reliability) {
        return tt_QOS_POLICY_RELIABILITY;
    }
    if (durability) {
        return tt_QOS_POLICY_DURABILITY;
    }
    if (deadline) {
        return tt_QOS_POLICY_DEADLINE;
    }
    return kind || lease ? tt_QOS_POLICY_LIVELINESS : tt_QOS_POLICY_INVALID;
}

static void test_judgement_is_what_delivery_refuses(void) {
    init();
    int incompatible = 0;
    int per_policy[5] = {0};
    for (int r = 0; r < RXO_COMBINATIONS; r++) {
        for (int o = 0; o < RXO_COMBINATIONS; o++) {
            struct qos_rxo requested = rxo_number(r);
            struct qos_rxo offered = rxo_number(o);
            uint8_t policy = qos_incompatible_policy(&requested, &offered);
            EXPECT_EQ_INT(expected_policy(&requested, &offered), policy);

            struct tt_Subscriber sub;
            memset(&sub, 0, sizeof(sub));
            sub.reliable = requested.reliable;
            sub.durable = requested.durable;
            sub.liveliness_manual = requested.manual;
            sub.deadline_duration_ns = requested.deadline_ns;
            sub.liveliness_lease_duration_ns = requested.lease_ns;
            struct tt_DiscoveredEntity writer;
            memset(&writer, 0, sizeof(writer));
            writer.qos = (uint8_t)((offered.reliable ? tt_UPDATE_QOS_RELIABLE : 0) |
                                   (offered.durable ? tt_UPDATE_QOS_DURABLE : 0) |
                                   (offered.manual ? tt_UPDATE_QOS_LIVELINESS_MANUAL : 0));
            writer.deadline_duration_ns = offered.deadline_ns;
            writer.liveliness_lease_duration_ns = offered.lease_ns;
            EXPECT_TRUE((policy != tt_QOS_POLICY_INVALID) == writer_qos_incompatible(&sub, &writer));
#if tt_LOCAL_DELIVERY
            struct tt_Publisher pub;
            memset(&pub, 0, sizeof(pub));
            pub.reliable = offered.reliable;
            pub.durable = offered.durable;
            pub.liveliness_manual = offered.manual;
            pub.deadline_duration_ns = offered.deadline_ns;
            pub.liveliness_lease_duration_ns = offered.lease_ns;
            EXPECT_TRUE((policy != tt_QOS_POLICY_INVALID) == local_pair_incompatible(&sub, &pub));
#endif
            incompatible += policy != tt_QOS_POLICY_INVALID ? 1 : 0;
            per_policy[policy]++;
        }
    }
    printf("test_qos_incompatible: %d pairs, %d incompatible: reliability %d durability %d deadline %d liveliness %d\n",
           RXO_COMBINATIONS * RXO_COMBINATIONS, incompatible, per_policy[1], per_policy[2], per_policy[3],
           per_policy[4]);
    // Every policy is reached, so the table above is not passing on compatible pairs alone.
    EXPECT_TRUE(per_policy[1] > 0 && per_policy[2] > 0 && per_policy[3] > 0 && per_policy[4] > 0);
    EXPECT_TRUE(per_policy[0] > 0);
}

// --- 2. once per pair, on both sides; 3. new endpoints count; 4. the control ---------------------------------------

static void test_once_per_pair_on_both_sides(void) {
    init();
    static struct tt_Subscriber a_sub;      // requests RELIABLE
    static struct tt_Publisher b_pub;       // offers BEST_EFFORT: incompatible by RELIABILITY
    static struct tt_Subscriber a_control;  // BEST_EFFORT ...
    static struct tt_Publisher b_control;   // ... and RELIABLE: compatible
    static struct tt_Publisher b_pub2;      // a second BEST_EFFORT writer, later
    static struct tt_Subscriber a_late_sub; // a RELIABLE reader created once b is known
    static struct tt_Publisher a_local_pub; // a BEST_EFFORT writer in a itself: a local pair with a's readers
    create_sub(&a, &a_sub, TOPIC, &reliable);
    create_sub(&a, &a_control, CONTROL_TOPIC, &best_effort);
    create_pub(&b, &b_pub, TOPIC, &best_effort);
    create_pub(&b, &b_control, CONTROL_TOPIC, &reliable);
    EXPECT_EQ_INT(0, event_count); // no other side known yet

    both_announce();
    // The control's other side is in the table: its zero below is judged, not missed.
    EXPECT_TRUE(tt_Discovery_find_entity(&a_discovery, B_ID, b_control.endpoint.id, b_control.endpoint.entity_id) !=
                NULL);
    EXPECT_TRUE(tt_Discovery_find_entity(&b_discovery, A_ID, a_control.endpoint.id, a_control.endpoint.entity_id) !=
                NULL);
    printf("test_qos_incompatible: after the first announces: a_sub %u, b_pub %u, controls %u/%u, callbacks %d\n",
           a_sub.qos_incompatible.total_count, b_pub.qos_incompatible.total_count,
           a_control.qos_incompatible.total_count, b_control.qos_incompatible.total_count, event_count);
    EXPECT_EQ_U32(1, a_sub.qos_incompatible.total_count); // requested
    EXPECT_EQ_U32(1, b_pub.qos_incompatible.total_count); // offered
    EXPECT_EQ_INT(tt_QOS_POLICY_RELIABILITY, a_sub.qos_incompatible.last_policy_kind);
    EXPECT_EQ_INT(tt_QOS_POLICY_RELIABILITY, b_pub.qos_incompatible.last_policy_kind);
    EXPECT_EQ_U32(0, a_control.qos_incompatible.total_count);
    EXPECT_EQ_U32(0, b_control.qos_incompatible.total_count);
    EXPECT_EQ_INT(1, events_for(&a_sub));
    EXPECT_EQ_INT(1, events_for(&b_pub));
    EXPECT_EQ_INT(2, event_count);
    EXPECT_TRUE(events[0].policy_kind == tt_QOS_POLICY_RELIABILITY &&
                events[1].policy_kind == tt_QOS_POLICY_RELIABILITY);

    // The falsifier: the same announce again, many times; then replayed as a new generation of the same source
    // (reprocess_known_announces(), as a new Publisher triggers it), which forgets the source's entities and applies
    // them again.
    for (int round = 0; round < 10; round++) {
        both_announce();
    }
    reprocess_known_announces(&a);
    reprocess_known_announces(&b);
    both_announce();
    both_announce();
    EXPECT_EQ_U32(1, a_sub.qos_incompatible.total_count);
    EXPECT_EQ_U32(1, b_pub.qos_incompatible.total_count);
    EXPECT_EQ_INT(2, event_count);

    // A new incompatible writer: a new pair for a_sub, and b's new announce generation lists b_pub again - which must
    // not count b_pub's pair a second time.
    create_pub(&b, &b_pub2, TOPIC, &best_effort);
    both_announce();
    both_announce();
    printf("test_qos_incompatible: after a second writer: a_sub %u, b_pub %u, b_pub2 %u, callbacks %d\n",
           a_sub.qos_incompatible.total_count, b_pub.qos_incompatible.total_count, b_pub2.qos_incompatible.total_count,
           event_count);
    EXPECT_EQ_U32(2, a_sub.qos_incompatible.total_count);
    EXPECT_EQ_U32(1, b_pub.qos_incompatible.total_count);
    EXPECT_EQ_U32(1, b_pub2.qos_incompatible.total_count); // b knew a_sub already: counted at b_pub2's creation
    EXPECT_EQ_INT(4, event_count);

    // Created after the other side is known: counted at creation, against both writers, before any announce.
    int before = event_count;
    create_sub(&a, &a_late_sub, TOPIC, &reliable);
    EXPECT_EQ_U32(2, a_late_sub.qos_incompatible.total_count);
    EXPECT_EQ_INT(before + 2, event_count);
    both_announce(); // and b counts it from a's announce, once per writer
    both_announce();
    EXPECT_EQ_U32(2, a_late_sub.qos_incompatible.total_count);
    EXPECT_EQ_U32(2, b_pub.qos_incompatible.total_count);
    EXPECT_EQ_U32(2, b_pub2.qos_incompatible.total_count);
    EXPECT_EQ_INT(before + 4, event_count);

    // A local pair: a's own BEST_EFFORT writer against a's two RELIABLE readers, both sides, at its creation.
    before = event_count;
    create_pub(&a, &a_local_pub, TOPIC, &best_effort);
    EXPECT_EQ_U32(2, a_local_pub.qos_incompatible.total_count);
    EXPECT_EQ_U32(3, a_sub.qos_incompatible.total_count);
    EXPECT_EQ_U32(3, a_late_sub.qos_incompatible.total_count);
    EXPECT_EQ_INT(before + 4, event_count);
    both_announce();
    both_announce();
    EXPECT_EQ_U32(2, a_local_pub.qos_incompatible.total_count); // b's BEST_EFFORT writers request nothing of it
    EXPECT_EQ_INT(before + 4, event_count);
    EXPECT_EQ_U32(0, a_control.qos_incompatible.total_count);
    EXPECT_EQ_U32(0, b_control.qos_incompatible.total_count);
}

// Each policy, end to end through a real announce, names itself on both sides.
static void test_each_policy_names_itself(void) {
    struct {
        struct tt_QosProfile requested;
        struct tt_QosProfile offered;
        uint8_t policy;
    } cases[] = {
        {reliable, best_effort, tt_QOS_POLICY_RELIABILITY},
        {{.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT},
          .durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL}},
         best_effort,
         tt_QOS_POLICY_DURABILITY},
        {{.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}, .deadline = {.period_ns = 10 * tt_MILLISECOND}},
         {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}, .deadline = {.period_ns = 20 * tt_MILLISECOND}},
         tt_QOS_POLICY_DEADLINE},
        {{.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT},
          .liveliness = {.kind = tt_QOS_LIVELINESS_MANUAL_BY_TOPIC, .lease_ns = tt_SECOND}},
         {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}, .liveliness = {.lease_ns = tt_SECOND}},
         tt_QOS_POLICY_LIVELINESS},
        {{.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}, .liveliness = {.lease_ns = tt_SECOND}},
         {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}, .liveliness = {.lease_ns = 2 * tt_SECOND}},
         tt_QOS_POLICY_LIVELINESS},
        // the control: the same deadline both sides
        {{.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}, .deadline = {.period_ns = 10 * tt_MILLISECOND}},
         {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}, .deadline = {.period_ns = 10 * tt_MILLISECOND}},
         tt_QOS_POLICY_INVALID},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        init();
        static struct tt_Subscriber sub;
        static struct tt_Publisher pub;
        create_sub(&a, &sub, TOPIC, &cases[i].requested);
        create_pub(&b, &pub, TOPIC, &cases[i].offered);
        both_announce();
        both_announce();
        uint32_t expected = cases[i].policy != tt_QOS_POLICY_INVALID ? 1U : 0U;
        EXPECT_TRUE(tt_Discovery_find_entity(&a_discovery, B_ID, pub.endpoint.id, pub.endpoint.entity_id) != NULL);
        EXPECT_EQ_U32(expected, sub.qos_incompatible.total_count);
        EXPECT_EQ_U32(expected, pub.qos_incompatible.total_count);
        EXPECT_EQ_INT(cases[i].policy, sub.qos_incompatible.last_policy_kind);
        EXPECT_EQ_INT(cases[i].policy, pub.qos_incompatible.last_policy_kind);
        EXPECT_EQ_INT((int)(2 * expected), event_count);
    }
}

int main(void) {
    test_judgement_is_what_delivery_refuses();
    test_once_per_pair_on_both_sides();
    test_each_policy_names_itself();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_qos_incompatible: all tests passed\n");
    return 0;
}
