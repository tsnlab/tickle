/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A KEEP_ALL Publisher that has not matched a Subscriber yet keeps what it sends (rig, 2026-10-05): its admission
// refuses only on behalf of matched Subscribers, so before the first match nothing guarded the arena, and the
// samples it broadcast meanwhile - which a not-yet-matched Subscriber receives, and asks again for what it missed as
// soon as it acknowledges - were evicted by bytes from the small starting arena. rmw KEEP_ALL Array4k under 5% loss
// lost 5-8 samples per run that way, all in the first second.
// - unmatched, within one tt_CONTEXT_UPDATE_INTERVAL of the first publish, with a growing hook: nothing is evicted,
//   the arena grew;
// - the same with no hook: samples are evicted - the fixture can fail;
// - unmatched past that interval: the arena does not grow and evicts as before - a Publisher with nobody to keep
//   samples for does not take its whole budget.
// Mutants, each killed here: the grow loop removed; the window check removed.
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

#define PUBLISHES 6
#define SAMPLE 5000 // two datagrams per sample: fragmented, as Array4k is on the rig
#define RING 64     // index slots: datagrams, several per sample
#define ONE_SAMPLE (tt_sample_cache_bytes(SAMPLE))
#define LIMIT 65536U // past PUBLISHES samples of ONE_SAMPLE (about 5.1 KB each)
#define ARENAS 5

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
static uint8_t arenas[ARENAS][LIMIT]; // each growth moves to the next, as an owner's allocator would
static int arena_in_use;
static int grows;

// Doubles the arena toward LIMIT, as rmw_tickle's grow_reliable_cache() does.
static bool grow(struct tt_Publisher* publisher) {
    struct tt_ReliableCache* grown = publisher->reliable_cache;
    if (grown->arena_size >= grown->arena_limit || arena_in_use + 1 >= ARENAS) {
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

static void setup(bool (*hook)(struct tt_Publisher*)) {
    test_mock_reset();
    memset(&context, 0, sizeof(context));
    node_init_locks(&context);
    context.id = 1;
    context.tx_tail = sizeof(struct tt_Header);
    context.tx_size = tt_TX_BUFFER_LENGTH;
    memset(&topic, 0, sizeof(topic));
    topic.name = "keep_all::msg::dds_::Big_";
    topic.data_size = 1;
    topic.data_encode_size = sample_size;
    topic.data_encode = sample_encode;
    topic.data_decode = refuse;
    topic.data_free = free_nothing;
    memset(&pub, 0, sizeof(pub));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&context, &pub, &topic, "rt/keep_all"));
    memset(index_storage, 0, sizeof(index_storage));
    memset(&cache, 0, sizeof(cache));
    cache.index = index_storage;
    cache.capacity = RING;
    cache.depth = RING;
    cache.arena = arenas[0];
    cache.arena_size = ONE_SAMPLE + (ONE_SAMPLE / 2); // room for one sample, not two
    cache.arena_limit = LIMIT;
    arena_in_use = 0;
    grows = 0;
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.keep_all = true;
    pub.cache_grow = hook;
}

static void publish(int from, int to) {
    for (int i = from; i < to; i++) {
        uint8_t value = (uint8_t)i;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
}

static void test_unmatched_keep_all_grows_rather_than_evicts(void) {
    setup(grow);
    test_mock_now = 5 * tt_SECOND; // a clock that is not at 0, as a real one never is
    publish(0, PUBLISHES);
    EXPECT_TRUE(!any_peer_ack_matched(&pub)); // the case under test: no Subscriber matched
    EXPECT_EQ_U32(1, cache.oldest_seq_no);    // nothing evicted
    EXPECT_TRUE(grows > 0);
}

static void test_without_a_hook_it_evicts(void) {
    setup(NULL);
    test_mock_now = 5 * tt_SECOND;
    publish(0, PUBLISHES);
    EXPECT_TRUE(cache.oldest_seq_no > 1);
}

static void test_past_the_interval_it_evicts_as_before(void) {
    setup(grow);
    test_mock_now = 5 * tt_SECOND;
    publish(0, 1); // the first publish starts the interval
    test_mock_now += tt_CONTEXT_UPDATE_INTERVAL + 1;
    publish(1, PUBLISHES);
    EXPECT_EQ_INT(0, grows);
    EXPECT_TRUE(cache.oldest_seq_no > 1);
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_unmatched_keep_all_grows_rather_than_evicts();
    test_without_a_hook_it_evicts();
    test_past_the_interval_it_evicts_as_before();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_keep_all_prematch: all tests passed\n");
    return 0;
}
