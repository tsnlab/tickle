/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Large-message stage 2, pre-registration L1.2 (DESIGN.md section 8): small samples are untouched on the wire. The
// datagrams a 64 B, a 1,472 B and a 64,000 B sample go out as - best effort and RELIABLE, as rmw_tickle builds core -
// must be byte-identical to the ones the parent of the change sent, captured into tests/wire_identity/ by
// tests/wire_identity_largemsg.sh (which also checks the parent against itself, the control, and that a mutant
// sending every sample as a large one fails here). Only the protocol version byte and the timestamp are masked: the
// first is the approved version bump itself, the second the clock.
//
// The same source is the capture tool: built with -DWIRE_CAPTURE_OUT=path (and -DTICKLE_C= the tree's tickle.c) it
// writes what it captured instead of comparing it, so the parent's capture comes from this exact procedure.

#define tt_MAX_BUFFER_LENGTH 65507

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
#define WIRE_GOLDEN "../../tests/wire_identity/largemsg_parent.bin"
#endif

#define CAPTURE_MAX ((size_t)512U * 1024U)
static uint8_t capture[CAPTURE_MAX];
static uint32_t capture_len;
static uint32_t captured_datagrams;

// The version byte and a DATA's or fragment 0's timestamp, zeroed: everything else is compared as sent.
static void mask(uint8_t* bytes, uint32_t len) {
    bool single = bytes[0] == tt_SINGLE_MARKER_LE || bytes[0] == tt_SINGLE_MARKER_BE;
    uint32_t body = single ? (uint32_t)sizeof(struct tt_SingleHeader)
                           : (uint32_t)(sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    uint8_t type = single ? bytes[3] : bytes[sizeof(struct tt_Header)];
    bytes[single ? 1 : 2] = 0; // tt_Header.version / tt_SingleHeader.version
    if ((type == tt_SUBMESSAGE_TYPE_DATA || type == tt_SUBMESSAGE_TYPE_FRAG_FIRST) && body + 12U <= len) {
        memset(bytes + body + 8, 0, 4); // tt_DataHeader.timestamp
    }
}

static void capture_hook(const void* buf, size_t len) {
    if (capture_len + 4U + len > CAPTURE_MAX) {
        test_failures++;
        return;
    }
    uint32_t length = (uint32_t)len;
    memcpy(capture + capture_len, &length, 4);
    memcpy(capture + capture_len + 4, buf, len);
    mask(capture + capture_len + 4, length);
    capture_len += 4U + length;
    captured_datagrams++;
}

static uint32_t sample_len;

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = (uint8_t)((i * 13U) + 5U);
    }
    return (int32_t)len;
}

#if defined(tt_LARGE_SAMPLES) && tt_LARGE_SAMPLES
// Large buffers are set on the tree that has them, as rmw_tickle sets them: L1.2 is about a build that can send large
// samples sending small ones exactly as before.
static void* test_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void test_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}
#endif

static struct tt_Context node;
static struct tt_Topic topic;
static struct tt_Publisher pub;
#define CACHE_DEPTH 128
static struct tt_ReliableCacheIndex cache_index[CACHE_DEPTH];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache cache;

static void publish_three(bool reliable) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND; // a fixed clock: both trees stamp the same time
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = sizeof(node.tx_buffer);
    memset(&topic, 0, sizeof(topic));
    topic.name = "wire_identity";
    topic.data_size = 8;
    topic.data_encode_size = sized_encode_size;
    topic.data_encode = sized_encode;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&node, &pub, &topic, "wire"));
#if defined(tt_LARGE_SAMPLES) && tt_LARGE_SAMPLES
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&node, test_acquire, test_release, NULL));
#endif
    if (reliable) {
        memset(cache_index, 0, sizeof(cache_index));
        memset(&cache, 0, sizeof(cache));
        cache.index = cache_index;
        cache.capacity = CACHE_DEPTH;
        cache.depth = CACHE_DEPTH;
        cache.arena = cache_arena;
        cache.arena_size = (uint32_t)sizeof(cache_arena);
        pub.reliable_cache = &cache;
        pub.reliable = true;
    }
    test_mock_send_hook = capture_hook;
    static const uint32_t sizes[] = {64, 1472, 64000};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        sample_len = sizes[i];
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    }
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_destroy(&pub));
}

int main(void) {
    capture_len = 0;
    captured_datagrams = 0;
    publish_three(false);
    publish_three(true);
    // 1 + 2 + 44 datagrams a pass, and the goodbye announce at each destroy.
    EXPECT_TRUE(captured_datagrams >= 2U * (1U + 2U + 44U));
#ifdef WIRE_CAPTURE_OUT
    FILE* out = fopen(WIRE_CAPTURE_OUT, "wb");
    if (out == NULL || fwrite(capture, 1, capture_len, out) != capture_len) {
        fprintf(stderr, "cannot write %s\n", WIRE_CAPTURE_OUT);
        return 1;
    }
    (void)fclose(out);
    printf("wire capture: %u datagrams, %u bytes -> %s\n", captured_datagrams, capture_len, WIRE_CAPTURE_OUT);
    return test_result();
#else
    static uint8_t golden[CAPTURE_MAX];
    FILE* in = fopen(WIRE_GOLDEN, "rb");
    EXPECT_TRUE(in != NULL);
    size_t golden_len = in != NULL ? fread(golden, 1, sizeof(golden), in) : 0;
    if (in != NULL) {
        (void)fclose(in);
    }
    EXPECT_EQ_U32((uint32_t)golden_len, capture_len);
    uint32_t first_difference = UINT32_MAX;
    for (uint32_t i = 0; i < capture_len && i < golden_len; i++) {
        if (capture[i] != golden[i]) {
            first_difference = i;
            break;
        }
    }
    EXPECT_EQ_U32(UINT32_MAX, first_difference);
    if (test_result() != 0) {
        return 1;
    }
    printf("test_wire_identity_largemsg: %u datagrams (64 B, 1,472 B and 64,000 B samples, best effort and "
           "RELIABLE) byte-identical to the parent's, version and timestamp masked\n",
           captured_datagrams);
    return 0;
#endif
}
