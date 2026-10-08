/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// What the discovery table's capacity decides beyond introspection (CONTEXT_NODE_PLAN.md 4a, 2026-09-27). A remote
// context announces PUBLISHERS best-effort publishers; a reliable subscriber listens to the last of them, which a
// table smaller than the announce drops. subscriber_incompatible_with_writer() reads the table, and finding no
// entry it lets the DATA through: RxO (reliable requested, best effort offered) is not enforced for that publisher.
// Likewise the per-entity liveliness lease and the KEEP_ALL answer a reliable subscriber reads have nothing to read.
//
// Built here with a table large enough (the rmw_tickle setting's scale); built with -Dtt_MAX_DISCOVERED_ENTITIES=16,
// core's default and what rmw_tickle had until 4a, it fails - the control.
#ifndef tt_MAX_DISCOVERED_ENTITIES
#define tt_MAX_DISCOVERED_ENTITIES 64
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/log.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: two contexts in one process, datagrams carried between them by hand.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

// The entity_id a test gives a discovered entity: derived from endpoint_id so it is predictable, and
// deliberately DIFFERENT from it so anything that confuses the two fails rather than agreeing by
// accident. endpoint_id is shared by every endpoint of one topic and name; entity_id is the instance.
#define ENTITY_ID_OF(endpoint_id) ((uint32_t)(endpoint_id) ^ 0xE1D00000U)

#define PUBLISHERS 20
#define REMOTE_ID 2
#define LOCAL_ID 1
#define REMOTE_IP 0x0a000002
#define REMOTE_PORT 8282
#define CLOCK_NS 1000000000ULL
#define CAPTURE_MAX 64

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

static int delivered;
static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
    delivered++;
}

static struct tt_Context remote;
static struct tt_Context local;
static struct tt_Discovery discovery;
static struct tt_Topic topics[PUBLISHERS];
static char names[PUBLISHERS][24];
static struct tt_Publisher pubs[PUBLISHERS];
static struct tt_Subscriber sub;

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    context->last_modified = CLOCK_NS;
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

static void test_the_last_of_many_publishers_is_known_and_refused(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    test_mock_send_hook = capture;
    init_context(&remote, REMOTE_ID);
    init_context(&local, LOCAL_ID);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&local, &discovery, NULL, NULL));

    for (int i = 0; i < PUBLISHERS; i++) {
        (void)snprintf(names[i], sizeof(names[i]), "topic_%02d", i);
        memset(&topics[i], 0, sizeof(topics[i]));
        topics[i].name = names[i];
        topics[i].data_size = 4;
        topics[i].data_encode_size = data_size;
        topics[i].data_encode = data_encode;
        topics[i].data_decode = data_decode;
        topics[i].data_free = data_free;
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&remote, &pubs[i], &topics[i], "pub"));
    }
    const int last = PUBLISHERS - 1;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&local, &sub, &topics[last], "pub", on_sample));
    sub.reliable = true; // requested RELIABLE; every remote publisher offers BEST_EFFORT

    captured_count = 0;
    EXPECT_TRUE(build_and_send_update(&remote, NULL, 0));
    carry();

    // The table knows the last publisher, so RxO, liveliness and KEEP_ALL have something to read.
    const struct tt_DiscoveredEntity* entity = tt_Discovery_find(&discovery, REMOTE_ID, pubs[last].endpoint.id);
    EXPECT_TRUE(entity != NULL);
    EXPECT_TRUE(writer_announced_keep_all(&local, REMOTE_ID, pubs[last].endpoint.id, pubs[last].endpoint.entity_id) !=
                tt_WRITER_KEEP_ALL_UNKNOWN);

    // Its DATA is refused as RxO-incompatible, not delivered.
    uint32_t value = 42;
    delivered = 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pubs[last], (struct tt_Data*)&value));
    carry();
    EXPECT_EQ_INT(0, delivered);
    EXPECT_EQ_U32(1, sub.rxo_drops);
    test_mock_send_hook = NULL;
}

// A table too small for the announce counts every entity it drops and warns once, however many announces fill it.
static int count_occurrences(const char* text, const char* needle) {
    int count = 0;
    for (const char* at = strstr(text, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

static void test_a_full_table_counts_what_it_drops_and_warns_once(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    test_mock_send_hook = capture;
    init_context(&remote, REMOTE_ID);
    init_context(&local, LOCAL_ID);
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&local, &discovery, NULL, NULL));
    // Fill every slot with entities of a third context, so the remote's publishers find none free.
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        upsert_discovered_entity(&local, REMOTE_ID + 1, (uint32_t)i + 1U, ENTITY_ID_OF((uint32_t)i + 1U),
                                 tt_KIND_TOPIC_PUBLISHER, 0, 0, 0, 0, "t", "e");
    }
    EXPECT_EQ_U32(0, discovery.entities_dropped);
    for (int i = 0; i < PUBLISHERS; i++) {
        (void)snprintf(names[i], sizeof(names[i]), "topic_%02d", i);
        memset(&topics[i], 0, sizeof(topics[i]));
        topics[i].name = names[i];
        topics[i].data_size = 4;
        topics[i].data_encode_size = data_size;
        topics[i].data_encode = data_encode;
        topics[i].data_decode = data_decode;
        topics[i].data_free = data_free;
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&remote, &pubs[i], &topics[i], "pub"));
    }
    char* log_text = NULL;
    size_t log_size = 0;
    FILE* log = open_memstream(&log_text, &log_size);
    tt_log_set_output(log);
    captured_count = 0;
    EXPECT_TRUE(build_and_send_update(&remote, NULL, 0));
    carry();
    remote.last_modified++; // a second, changed announce: dropped again, not warned again
    EXPECT_TRUE(build_and_send_update(&remote, NULL, 0));
    carry();
    (void)fclose(log);
    tt_log_set_output(stderr);
    // Each announce lists the publishers and, since stage 3, the node they are on (the remote's default node).
    EXPECT_EQ_U32(2 * (PUBLISHERS + 1), discovery.entities_dropped);
    EXPECT_EQ_INT(1, count_occurrences(log_text != NULL ? log_text : "", "Discovery table full"));
    free(log_text);
    test_mock_send_hook = NULL;
}

int main(void) {
    test_the_last_of_many_publishers_is_known_and_refused();
    test_a_full_table_counts_what_it_drops_and_warns_once();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_discovery_capacity: all tests passed\n");
    return 0;
}
