/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Two publishers of one topic in one remote context (2026-10-08). They share the endpoint_id (hash of topic and
// endpoint name); what tells them apart is the entity_id. The discovery table was keyed on (context_id, endpoint_id),
// so the second one announced overwrote the first: the graph listed one, rmw_count_publishers came out 1, and the
// RxO check read the survivor's QoS for both writers' DATA. Keyed on the entity:
//
//   - both are discovered and counted, each found by its own entity_id;
//   - each writer's DATA is judged by its own announced QoS: the pair offers different deadlines, and the local
//     subscriber's requested deadline accepts one and refuses the other. One table entry would judge both alike -
//     the control is the count of samples delivered from each writer, not a field of the table;
//   - each one's departure (an announce that no longer lists it) removes only itself.
//
// Both build sizes: tt_DISCOVERY_INDEXED at 128 here; the scan at 16 in test_discovery_siblings_linear.c.
#ifndef tt_MAX_DISCOVERED_ENTITIES
#define tt_MAX_DISCOVERED_ENTITIES 128
#define EXPECT_INDEXED 1
#endif

#include <assert.h> // static_assert
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: two contexts in one process, datagrams carried between them by hand.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#ifdef EXPECT_INDEXED
static_assert(tt_DISCOVERY_INDEXED, "a 128-entry table is built with the index");
#else
static_assert(!tt_DISCOVERY_INDEXED, "a 16-entry table keeps the scan");
#endif

#define REMOTE_ID 2
#define LOCAL_ID 1
#define REMOTE_IP 0x0a000002
#define REMOTE_PORT 8282
#define CLOCK_NS 1000000000ULL
#define CAPTURE_MAX 32
#define REQUESTED_DEADLINE_NS (100ULL * tt_MILLISECOND)
#define KEPT_DEADLINE_NS (50ULL * tt_MILLISECOND) // within what the subscriber requests
#define MISSED_DEADLINE_NS 0ULL                   // infinite: looser than requested, so incompatible

static uint8_t captured[CAPTURE_MAX][tt_MAX_BUFFER_LENGTH];
static size_t captured_length[CAPTURE_MAX];
static int captured_count;

static void capture(const void* buf, size_t len) {
    if (captured_count < CAPTURE_MAX && len <= tt_MAX_BUFFER_LENGTH) {
        memcpy(captured[captured_count], buf, len);
        captured_length[captured_count++] = len;
    }
}

static int32_t data_size(struct tt_Data* data) {
    (void)data;
    return 4;
}
static int32_t data_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < 4) {
        return -1;
    }
    memcpy(payload, data, 4);
    return 4;
}
static int32_t data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)native;
    if (len < 4) {
        return -1;
    }
    memcpy(data, payload, 4);
    return 4;
}
static void data_free(struct tt_Data* data) {
    (void)data;
}

// Delivered samples by value: each writer publishes its own value.
#define VALUE_KEPT 1U
#define VALUE_MISSED 2U
static int delivered_kept;
static int delivered_missed;
static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    delivered_kept += value == VALUE_KEPT ? 1 : 0;
    delivered_missed += value == VALUE_MISSED ? 1 : 0;
}

static struct tt_Context remote;
static struct tt_Context local;
static struct tt_Discovery discovery;
static struct tt_Topic topic;
static struct tt_Publisher kept;   // offers a deadline the subscriber accepts
static struct tt_Publisher missed; // offers one it refuses
static struct tt_Subscriber sub;

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    context->last_modified = CLOCK_NS;
    context->entity_id_base = 0x5A000000U | ((uint32_t)id << 16); // as tt_Context_create() draws one: never 0
}

// Everything the remote context has sent since the last call, handed to the local one as if received.
static void carry(void) {
    node_flush(&remote, 0, NULL);
    for (int i = 0; i < captured_count; i++) {
        memcpy(local.rx_buffer, captured[i], captured_length[i]);
        (void)process_packet(&local, local.rx_buffer, 0, (uint32_t)captured_length[i], REMOTE_IP, REMOTE_PORT,
                             tt_TRANSPORT_UDP);
    }
    captured_count = 0;
}

// A fresh announce of the remote context, as after any change to its endpoints: a new generation.
static void announce(void) {
    test_mock_now += tt_MILLISECOND;
    remote.last_modified = test_mock_now;
    captured_count = 0;
    EXPECT_TRUE(build_and_send_update(&remote, NULL, 0));
    carry();
}

// The remote publishers of the topic the local table holds - what rmw_count_publishers counts.
static int remote_publishers(void) {
    int count = 0;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        const struct tt_DiscoveredEntity* entity = &discovery.entities[i];
        count += entity->context_id == REMOTE_ID && entity->kind == tt_KIND_TOPIC_PUBLISHER &&
                         entity->endpoint_id == kept.endpoint.id && entity->alive
                     ? 1
                     : 0;
    }
    return count;
}

static bool known(const struct tt_Publisher* pub) {
    const struct tt_DiscoveredEntity* entity =
        tt_Discovery_find_entity(&discovery, REMOTE_ID, pub->endpoint.id, pub->endpoint.entity_id);
    return entity != NULL && entity->alive && entity->kind == tt_KIND_TOPIC_PUBLISHER;
}

static void publish(struct tt_Publisher* pub, uint32_t value) {
    captured_count = 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(pub, (struct tt_Data*)&value));
    carry();
}

static void setup(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    test_mock_send_hook = capture;
    init_context(&remote, REMOTE_ID);
    init_context(&local, LOCAL_ID);
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&local, &discovery, NULL, NULL));

    memset(&topic, 0, sizeof(topic));
    topic.name = "chatter";
    topic.data_size = 4;
    topic.data_encode_size = data_size;
    topic.data_encode = data_encode;
    topic.data_decode = data_decode;
    topic.data_free = data_free;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&remote, &kept, &topic, "pub"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&remote, &missed, &topic, "pub"));
    kept.deadline_duration_ns = KEPT_DEADLINE_NS;
    missed.deadline_duration_ns = MISSED_DEADLINE_NS;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&local, &sub, &topic, "pub", on_sample));
    sub.deadline_duration_ns = REQUESTED_DEADLINE_NS;
    // The premise: one endpoint_id, two entities.
    EXPECT_EQ_U32(kept.endpoint.id, missed.endpoint.id);
    EXPECT_TRUE(kept.endpoint.entity_id != missed.endpoint.entity_id);
}

static void teardown(void) {
    tt_Context_set_discovery(&local, NULL, NULL, NULL);
    test_mock_send_hook = NULL;
}

static void test_both_publishers_are_discovered_and_counted(void) {
    setup();
    announce();
    EXPECT_TRUE(known(&kept));
    EXPECT_TRUE(known(&missed));
    EXPECT_EQ_INT(2, remote_publishers());
    teardown();
}

// Each writer's DATA is judged by its own QoS: the one offering the requested deadline is delivered, the other
// refused. With one entry for both, both would be delivered or both refused.
static void test_each_writer_is_matched_by_its_own_qos(void) {
    setup();
    announce();
    delivered_kept = 0;
    delivered_missed = 0;
    publish(&kept, VALUE_KEPT);
    publish(&missed, VALUE_MISSED);
    EXPECT_EQ_INT(1, delivered_kept);
    EXPECT_EQ_INT(0, delivered_missed);
    EXPECT_EQ_U32(1, sub.rxo_drops);
    teardown();
}

// One leaves (its node re-announces without it): only it goes; then the other.
static void test_each_departure_removes_only_itself(void) {
    setup();
    announce();
    EXPECT_EQ_INT(2, remote_publishers());

    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_destroy(&kept));
    announce();
    EXPECT_TRUE(!known(&kept));
    EXPECT_TRUE(known(&missed));
    EXPECT_EQ_INT(1, remote_publishers());

    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_destroy(&missed));
    announce();
    EXPECT_TRUE(!known(&missed));
    EXPECT_EQ_INT(0, remote_publishers());
    teardown();
}

// The mirror order, so neither position on the probe chain is the one that happens to survive.
static void test_the_other_departure_removes_only_itself(void) {
    setup();
    announce();
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_destroy(&missed));
    announce();
    EXPECT_TRUE(known(&kept));
    EXPECT_TRUE(!known(&missed));
    EXPECT_EQ_INT(1, remote_publishers());
    teardown();
}

int main(void) {
    test_both_publishers_are_discovered_and_counted();
    test_each_writer_is_matched_by_its_own_qos();
    test_each_departure_removes_only_itself();
    test_the_other_departure_removes_only_itself();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_discovery_siblings: all tests passed\n");
    return 0;
}
