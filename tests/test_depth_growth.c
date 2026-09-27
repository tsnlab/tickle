/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g10 (rmw_tickle/RMW_GAPS_PLAN.md, 2026-09-28): a KEEP_LAST cache keeps `depth` samples when its owner can grow it.
// A Publisher of depth 4 (sample_depth), fragmented 5000-byte samples, and an arena that starts with room for one:
// - with a growing hook (tt_Publisher.cache_grow), 6 publishes leave the newest 4 retained and no shortfall - durable
//   and RELIABLE alike, since both share the cache;
// - with no hook, or one that cannot grow, fewer are retained and every short publish is counted
//   (tt_ReliableCache.depth_shortfalls).
// Mutants, each killed here: the hook not called; a shortfall not counted.
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

#define DEPTH 4
#define PUBLISHES 6
#define SAMPLE 5000
#define RING 64 // index slots: datagrams, several per sample
#define ONE_SAMPLE (tt_sample_cache_bytes(SAMPLE))
#define LIMIT 65536U // past 4 samples of ONE_SAMPLE (about 5.1 KB each)

static int32_t sample_size(struct tt_Data* data) {
    (void)data;
    return SAMPLE;
}
static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < SAMPLE) {
        return -1;
    }
    memset(payload, *(const uint8_t*)data, SAMPLE);
    return SAMPLE;
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

static struct tt_Context context;
static struct tt_Topic topic;
static struct tt_Publisher pub;
static struct tt_ReliableCacheIndex index_storage[RING];
static struct tt_ReliableCache cache;
static uint8_t arenas[4][LIMIT]; // each growth moves to the next, as an owner's allocator would
static int arena_in_use;
static int grows;

// Doubles the arena toward LIMIT, as rmw_tickle's grow_reliable_cache() does.
static bool grow(struct tt_Publisher* publisher) {
    struct tt_ReliableCache* grown = publisher->reliable_cache;
    if (grown->arena_size >= grown->arena_limit || arena_in_use + 1 >= 4) {
        return false;
    }
    uint32_t next = grown->arena_size * 2 < grown->arena_limit ? grown->arena_size * 2 : grown->arena_limit;
    if (tt_ReliableCache_grow(grown, arenas[arena_in_use + 1], next) != tt_RET_OK) {
        return false;
    }
    arena_in_use++;
    grows++;
    return true;
}
static bool cannot_grow(struct tt_Publisher* publisher) {
    (void)publisher;
    return false;
}

static void setup(bool (*hook)(struct tt_Publisher*), bool durable) {
    test_mock_reset();
    memset(&context, 0, sizeof(context));
    node_init_locks(&context);
    context.id = 1;
    context.tx_tail = sizeof(struct tt_Header);
    context.tx_size = tt_TX_BUFFER_LENGTH;
    memset(&topic, 0, sizeof(topic));
    topic.name = "g10::msg::dds_::Big_";
    topic.data_size = 1;
    topic.data_encode_size = sample_size;
    topic.data_encode = sample_encode;
    topic.data_decode = refuse;
    topic.data_free = free_nothing;
    memset(&pub, 0, sizeof(pub));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&context, &pub, &topic, "rt/g10"));
    memset(index_storage, 0, sizeof(index_storage));
    memset(&cache, 0, sizeof(cache));
    cache.index = index_storage;
    cache.capacity = RING;
    cache.depth = RING;
    cache.arena = arenas[0];
    cache.arena_size = ONE_SAMPLE + (ONE_SAMPLE / 2); // room for one sample, not two
    cache.arena_limit = LIMIT;
    cache.sample_depth = DEPTH;
    arena_in_use = 0;
    grows = 0;
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.durable = durable;
    pub.cache_grow = hook;
}

static void publish_all(void) {
    for (int i = 0; i < PUBLISHES; i++) {
        uint8_t value = (uint8_t)i;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
}

static void test_a_growing_hook_keeps_depth(bool durable) {
    setup(grow, durable);
    publish_all();
    EXPECT_EQ_U32(DEPTH, cache.retained_samples);
    EXPECT_EQ_U32(0, cache.depth_shortfalls);
    EXPECT_TRUE(grows > 0);
}

static void test_no_room_is_counted(bool (*hook)(struct tt_Publisher*)) {
    setup(hook, true);
    publish_all();
    EXPECT_TRUE(cache.retained_samples < DEPTH);
    EXPECT_TRUE(cache.depth_shortfalls > 0);
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_a_growing_hook_keeps_depth(true);  // TRANSIENT_LOCAL
    test_a_growing_hook_keeps_depth(false); // RELIABLE, volatile: the retransmit window
    test_no_room_is_counted(NULL);
    test_no_room_is_counted(cannot_grow);

    if (test_result() != 0) {
        return 1;
    }
    printf("test_depth_growth: all tests passed\n");
    return 0;
}
