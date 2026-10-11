/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// seq_no past 2^32. A writer's seq_no is a 32-bit counter that wraps to 0 and goes on; every comparison of two seq_no
// is serial (tickle.c's seq_delta()), nothing takes seq_no 0 for "none", and the rings indexed by seq_no stay
// contiguous across the wrap. These tests start writers just short of 2^32 so the wrap falls inside the run.
//
// Each wrap test has a control that cannot see the wrap: the same run started at seq_no 1 (or a ring whose depth
// divides 2^32), and a wrap run is judged against its control, not against expectations of its own.

// Room for 10 fragments, so the end-to-end runs cover fragmented samples too (as test_data_frag.c builds).
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

// --- the writer's cache ring ----------------------------------------------------------------------

static struct tt_Context cache_node;

// Caches a `len`-byte DATA record for seq_no whose bytes after the submessage header are all (uint8_t)seq_no
// (test_reliable_pubsub.c's own helper, with a real type: a fragmenting build evicts a FRAG_CONT with its sample).
static void cache_write(struct tt_ReliableCache* cache, uint32_t seq_no, uint32_t len) {
    struct tt_SubmessageHeader* header = (struct tt_SubmessageHeader*)cache_node.tx_buffer;
    memset(cache_node.tx_buffer, (int)(seq_no & 0xFFU), ROUNDUP(len));
    header->type = tt_SUBMESSAGE_TYPE_DATA;
    cache_node.tx_tail = len;
    cache_reliable_sample(&cache_node, header, cache, seq_no, len);
}

// Whether seq_no is retained with the bytes it was written with, found where every lookup finds it.
static bool cache_holds(const struct tt_ReliableCache* cache, uint32_t seq_no) {
    uint16_t depth = reliable_cache_depth(cache);
    const struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, seq_no);
    if (!cache->any_retained || entry->seq_no != seq_no || entry->len == 0) {
        return false;
    }
    for (uint32_t i = sizeof(struct tt_SubmessageHeader); i < entry->len; i++) {
        if (cache->arena[entry->offset + i] != (uint8_t)(seq_no & 0xFFU)) {
            return false;
        }
    }
    return true;
}

// Writes seq_no first .. first + count - 1 into a cache of `depth` slots, with room for every record, and after each
// write checks the last `depth` written are all held, intact - KEEP_LAST by count - and that oldest/newest name them.
// Returns how many checks failed.
static uint32_t cache_run(uint16_t depth, uint32_t first, uint32_t count) {
    static struct tt_ReliableCacheIndex index[64];
    static uint8_t arena[tt_RELIABLE_CACHE_ARENA_BYTES(64, tt_RELIABLE_RECORD_BYTES(512))];
    struct tt_ReliableCache cache;
    memset(index, 0, sizeof(index));
    memset(&cache, 0, sizeof(cache));
    cache.index = index;
    cache.capacity = depth;
    cache.depth = depth;
    cache.arena = arena;
    cache.arena_size = (uint32_t)sizeof(arena);
    uint32_t failures = 0;
    for (uint32_t n = 0; n < count; n++) {
        uint32_t seq_no = first + n;
        cache_write(&cache, seq_no, 16 + ((seq_no * 7U) % 40U));
        uint32_t held = n + 1 < depth ? n + 1 : depth;
        for (uint32_t back = 0; back < held; back++) {
            failures += cache_holds(&cache, seq_no - back) ? 0U : 1U;
        }
        failures += cache.oldest_seq_no == seq_no - (held - 1U) && cache.newest_seq_no == seq_no ? 0U : 1U;
    }
    return failures;
}

static void test_cache_ring_stays_contiguous_across_the_wrap(void) {
    // 13 slots: 2^32 % 13 = 9, so (seq_no - 1) % 13 jumps by 9 slots where seq_no passes 2^32, and seq_no 1 would
    // land on a slot still holding a sample from before the wrap. 16 divides 2^32: the control, which no rebase is
    // needed for. Both run from 40 short of 2^32 to 40 past it, and from 1 as the stream that never wraps.
    EXPECT_EQ_U32(0, cache_run(13, 0xFFFFFFFFU - 39U, 80));
    EXPECT_EQ_U32(0, cache_run(16, 0xFFFFFFFFU - 39U, 80));
    EXPECT_EQ_U32(0, cache_run(13, 1, 80));
    // seq_no 0 is a sample like any other: retained, found, and the cache not read as empty while it is the oldest.
    EXPECT_EQ_U32(0, cache_run(3, 0xFFFFFFFEU, 6));
}

static void test_cache_ring_rebases_without_moving_a_sample(void) {
    // From seq_no 1 the slots are (seq_no - 1) % depth (slot_origin 1). When seq_no - 1 reaches 2^31 the base moves on
    // by a multiple of depth while samples are retained, and every one of them must still be found where it was:
    // a run across 2^31, at a depth that divides 2^32 and one that does not.
    EXPECT_EQ_U32(0, cache_run(13, 0x80000000U - 20U, 40));
    EXPECT_EQ_U32(0, cache_run(16, 0x80000000U - 20U, 40));
    static struct tt_ReliableCacheIndex index[13];
    static uint8_t arena[tt_RELIABLE_CACHE_ARENA_BYTES(13, tt_RELIABLE_RECORD_BYTES(512))];
    struct tt_ReliableCache cache;
    memset(index, 0, sizeof(index));
    memset(&cache, 0, sizeof(cache));
    cache.index = index;
    cache.capacity = 13;
    cache.depth = 13;
    cache.arena = arena;
    cache.arena_size = (uint32_t)sizeof(arena);
    for (uint32_t seq_no = 1; seq_no < 30; seq_no++) {
        cache_write(&cache, seq_no, 24);
    }
    EXPECT_EQ_U32(1, cache.slot_origin); // control: never wrapped, slots as they always were
    EXPECT_TRUE(&index[(29U - 1U) % 13U] == reliable_cache_slot(&cache, 13, 29));
    for (uint32_t seq_no = 0x80000000U - 5U; seq_no != 0x80000000U + 5U; seq_no++) {
        cache_write(&cache, seq_no, 24);
    }
    EXPECT_TRUE(cache.slot_origin != 1 && (cache.slot_origin - 1U) % 13U == 0); // treatment: moved, by whole rings
}

int main(void) {
    memset(&cache_node, 0, sizeof(cache_node));
    node_init_locks(&cache_node);
    cache_node.tx_size = sizeof(cache_node.tx_buffer);

    test_cache_ring_stays_contiguous_across_the_wrap();
    test_cache_ring_rebases_without_moving_a_sample();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_seq_wrap: all tests passed\n");
    return 0;
}
