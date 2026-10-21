/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// tt_InitOptions bundle 1 (ROADMAP "Context options"): a context created with the default options sends what
// tt_Context_create() sent before the options existed, byte for byte and to the same destinations. One fixed scenario
// through the real creation path and the mock HAL - creation, a best-effort and a RELIABLE Publisher, a Subscriber,
// samples broadcast to a peer set past the unicast threshold and unicast to one peer, then 3 s of polling (announces,
// summaries, heartbeats) - is captured, every datagram with its destination, and compared:
//   - in this tree, three arms: tt_Context_create() (the legacy _tt_CONFIG path), tt_Context_create_with(NULL) and
//     tt_Context_create_with() given tt_InitOptions_default(); all three must equal each other and the golden;
//   - the golden, tests/wire_identity/initoptions_parent.bin, is the parent's capture (83ef91b6, before the options),
//     written by tests/wire_identity_initoptions.sh from this same source, which also checks the parent against
//     itself (the control) and that a mutant changing one default is seen.
// Nothing is masked: both trees speak the same tt_VERSION, and the mock clock makes every timestamp the same.
//
// Built with -DWIRE_CAPTURE_OUT=path (and -DTICKLE_C= a tree's tickle.c) it writes the legacy arm's capture instead.

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
#define WIRE_GOLDEN "../../tests/wire_identity/initoptions_parent.bin"
#endif

// This tree has the options; the parent, built from this same source, has tt_Context_create() only.
#ifdef tt_UNICAST_ALWAYS_BROADCAST
#define WIRE_HAS_INIT_OPTIONS 1
#else
#define WIRE_HAS_INIT_OPTIONS 0
#endif

#define CAPTURE_MAX ((size_t)256U * 1024U)
#define POLL_STEPS 100
#define POLL_STEP_NS (100ULL * tt_MILLISECOND)
#define SAMPLE_LEN 100U

struct capture_buffer {
    uint8_t bytes[CAPTURE_MAX];
    uint32_t len;
    uint32_t datagrams;
};

static struct capture_buffer* active;

// One record a datagram: its length, its destination (0: the HAL's broadcast address), then the bytes as sent.
static void capture_hook(const void* buf, size_t len) {
    if (active->len + 8U + len > CAPTURE_MAX) {
        test_failures++;
        return;
    }
    uint32_t length = (uint32_t)len;
    memcpy(active->bytes + active->len, &length, 4);
    memcpy(active->bytes + active->len + 4, &test_mock_send_hook_ip, 4);
    memcpy(active->bytes + active->len + 8, buf, len);
    active->len += 8U + length;
    active->datagrams++;
}

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)SAMPLE_LEN;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = (uint8_t)((i * 7U) + 3U);
    }
    return (int32_t)len;
}

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

enum create_arm { CREATE_LEGACY, CREATE_WITH_NULL, CREATE_WITH_DEFAULTS };

static struct tt_Context node;
static struct tt_TypeSupport topic;
static struct tt_Publisher best_effort;
static struct tt_Publisher reliable;
static struct tt_Subscriber sub;
static uint64_t reliable_storage[(size_t)256U * 1024U / sizeof(uint64_t)];
static uint64_t subscriber_storage[(size_t)64U * 1024U / sizeof(uint64_t)];

static tt_ret_t create(enum create_arm arm) {
#if WIRE_HAS_INIT_OPTIONS
    if (arm == CREATE_WITH_NULL) {
        return tt_Context_create_with(&node, NULL);
    }
    if (arm == CREATE_WITH_DEFAULTS) {
        struct tt_InitOptions opts;
        tt_InitOptions_default(&opts);
        return tt_Context_create_with(&node, &opts);
    }
#else
    (void)arm;
#endif
    return tt_Context_create(&node);
}

static void run_scenario(enum create_arm arm, struct capture_buffer* out) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND; // a fixed clock: both trees stamp the same times
    test_mock_node_id = 7;
    // The link resolves to an interface, 10.0.0.7/24, as a real one would: the mock answers no interface otherwise, and
    // creation refuses a broadcast address no interface owns.
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
#if tt_CONTEXT_ID_CLAIM
    memset(test_mock_ids_held, 0, sizeof(test_mock_ids_held)); // each arm's context takes id 7, as the first one did
#endif
    out->len = 0;
    out->datagrams = 0;
    active = out;
    memset(&node, 0, sizeof(node));
    test_mock_send_hook = capture_hook;
    EXPECT_EQ_INT(tt_RET_OK, create(arm));

    memset(&topic, 0, sizeof(topic));
    topic.name = "wire_initoptions";
    topic.data_size = 8;
    topic.data_encode_size = sized_encode_size;
    topic.data_encode = sized_encode;
    topic.data_decode = decode_nothing;
    topic.data_free = free_nothing;
    struct tt_Node* default_node = tt_Context_default_node(&node);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(default_node, &best_effort, &topic, "be", &TEST_QOS_UNRELIABLE,
                                                      NULL, NULL, 0));
    size_t storage = tt_Publisher_storage_size(&topic, &tt_QOS_PROFILE_DEFAULT);
    EXPECT_TRUE(storage <= sizeof(reliable_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(default_node, &reliable, &topic, "rel", &tt_QOS_PROFILE_DEFAULT,
                                                      NULL, reliable_storage, storage));
    size_t sub_storage = tt_Subscriber_storage_size(&topic, &TEST_QOS_UNRELIABLE);
    EXPECT_TRUE(sub_storage <= sizeof(subscriber_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(default_node, &sub, &topic, "wire_sub", &TEST_QOS_UNRELIABLE,
                                                       &(struct tt_SubscriberOptions) {.callback = on_data},
                                                       sub_storage != 0 ? subscriber_storage : NULL, sub_storage));

    // Past the unicast threshold: one broadcast. Then one peer: unicast to it.
    for (uint8_t i = 0; i <= tt_UNICAST_PEER_THRESHOLD; i++) {
        (void)upsert_peer(best_effort.peers, (uint8_t)(100U + i), 0x0a000100U + i, 8282);
        (void)upsert_peer(reliable.peers, (uint8_t)(100U + i), 0x0a000100U + i, 8282);
    }
    uint32_t sample = 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&best_effort, (struct tt_Data*)&sample));
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&reliable, (struct tt_Data*)&sample));
    memset(best_effort.peers, 0, sizeof(best_effort.peers));
    (void)upsert_peer(best_effort.peers, 120, 0x0a000178U, 8282);
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&best_effort, (struct tt_Data*)&sample));

    for (int step = 0; step < POLL_STEPS; step++) {
        test_mock_now += POLL_STEP_NS;
        (void)tt_Context_poll(&node, 0);
    }
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
}

static bool same_capture(const struct capture_buffer* a, const uint8_t* b, size_t b_len, const char* what) {
    if (a->len != b_len || memcmp(a->bytes, b, b_len) != 0) {
        uint32_t first = 0;
        while (first < a->len && first < b_len && a->bytes[first] == b[first]) {
            first++;
        }
        fprintf(stderr, "%s: %u vs %zu bytes, first difference at byte %u\n", what, a->len, b_len, first);
        return false;
    }
    return true;
}

static struct capture_buffer legacy;
#if WIRE_HAS_INIT_OPTIONS && !defined(WIRE_CAPTURE_OUT)
static struct capture_buffer with_null;
static struct capture_buffer with_defaults;
static uint8_t golden[CAPTURE_MAX];
#endif

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    run_scenario(CREATE_LEGACY, &legacy);
    // Announces and summaries over 3 s, the three publishes and a heartbeat at least: a capture this small could not
    // decide anything.
    if (legacy.datagrams < 8U) {
        fprintf(stderr, "only %u datagrams captured\n", legacy.datagrams);
    }
    EXPECT_TRUE(legacy.datagrams >= 8U);
#ifdef WIRE_CAPTURE_OUT
    FILE* out = fopen(WIRE_CAPTURE_OUT, "wb");
    if (out == NULL || fwrite(legacy.bytes, 1, legacy.len, out) != legacy.len) {
        fprintf(stderr, "cannot write %s\n", WIRE_CAPTURE_OUT);
        return 1;
    }
    (void)fclose(out);
    printf("wire capture: %u datagrams, %u bytes -> %s\n", legacy.datagrams, legacy.len, WIRE_CAPTURE_OUT);
    return test_result();
#elif WIRE_HAS_INIT_OPTIONS
    run_scenario(CREATE_WITH_NULL, &with_null);
    run_scenario(CREATE_WITH_DEFAULTS, &with_defaults);
    EXPECT_TRUE(same_capture(&with_null, legacy.bytes, legacy.len, "create_with(NULL) vs create()"));
    EXPECT_TRUE(same_capture(&with_defaults, legacy.bytes, legacy.len, "create_with(defaults) vs create()"));
    FILE* in = fopen(WIRE_GOLDEN, "rb");
    EXPECT_TRUE(in != NULL);
    size_t golden_len = in != NULL ? fread(golden, 1, sizeof(golden), in) : 0;
    if (in != NULL) {
        (void)fclose(in);
    }
    EXPECT_TRUE(golden_len > 0);
    EXPECT_TRUE(same_capture(&legacy, golden, golden_len, "create() vs the parent's golden"));
    if (test_result() != 0) {
        return 1;
    }
    printf("test_wire_identity_initoptions: %u datagrams (%u bytes) from tt_Context_create(), "
           "tt_Context_create_with(NULL) and tt_Context_create_with(defaults) byte-identical to each other and to the "
           "parent's, destinations included\n",
           legacy.datagrams, legacy.len);
    return 0;
#else
    return test_result();
#endif
}
