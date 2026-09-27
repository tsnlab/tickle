/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The discovery table's index (CONTEXT_NODE_PLAN.md 4b, 2026-09-27): (context id, endpoint id) -> slot, open
// addressing. Keys that collide are all found; a source's entries forgotten leave the index and the others stay
// found; a tombstone reclaimed for a new key takes its old key out; and forgetting and re-adding a source many times
// over never grows the index past the live entries.
//
// The index is compiled only for a large table (tt_DISCOVERY_INDEXED, config.h): built here at 128, it is; the same
// behavioural cases run on the scan at 16 in test_discovery_index_linear.c, which includes this file.
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

// Whitebox: discovery_hash() picks the colliding keys, and the forget/tombstone paths are static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#ifdef EXPECT_INDEXED
static_assert(tt_DISCOVERY_INDEXED, "a 128-entry table is built with the index");
#else
static_assert(!tt_DISCOVERY_INDEXED, "a 16-entry table keeps the scan");
#endif

#define SOURCE_A 2
#define SOURCE_B 3
#define COLLIDING (tt_MAX_DISCOVERED_ENTITIES < 20 ? tt_MAX_DISCOVERED_ENTITIES : 20)
#define CYCLES 1024 // more than four times any index this test builds

static struct tt_Context context;
static struct tt_Discovery discovery;

static void setup(void) {
    test_mock_reset();
    memset(&context, 0, sizeof(context));
    node_init_locks(&context);
    context.id = 1;
    memset(&discovery, 0, sizeof(discovery));
    context.discovery = &discovery;
}

static void add(uint8_t source, uint32_t endpoint_id) {
    upsert_discovered_entity(&context, source, endpoint_id, tt_KIND_TOPIC_PUBLISHER, 0, 0, 0, 0, "t", "e");
}

static bool known(uint8_t source, uint32_t endpoint_id) {
    const struct tt_DiscoveredEntity* entity = tt_Discovery_find(&discovery, source, endpoint_id);
    return entity != NULL && entity->context_id == source && entity->endpoint_id == endpoint_id;
}

#if tt_DISCOVERY_INDEXED
static uint32_t index_entries(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < tt_DISCOVERY_INDEX_SIZE; i++) {
        count += discovery.index[i] != 0 ? 1U : 0U;
    }
    return count;
}
#endif

// COLLIDING keys of two sources that all hash to one bucket: every one is found; forgetting source A's leaves B's
// found and A's gone.
static void test_colliding_keys_are_found_and_forgotten(void) {
    setup();
    uint32_t keys[COLLIDING];
#if tt_DISCOVERY_INDEXED
    uint32_t bucket = discovery_hash(SOURCE_A, 1);
    int found_keys = 0;
    for (uint32_t candidate = 1; found_keys < COLLIDING; candidate++) {
        if (discovery_hash(SOURCE_A, candidate) == bucket) {
            keys[found_keys++] = candidate;
        }
    }
#else
    for (int i = 0; i < COLLIDING; i++) {
        keys[i] = (uint32_t)i + 1U; // the scan has no buckets; the same cases, any keys
    }
#endif
    for (int i = 0; i < COLLIDING; i++) {
        add(i % 2 == 0 ? SOURCE_A : SOURCE_B, keys[i]);
    }
    for (int i = 0; i < COLLIDING; i++) {
        EXPECT_TRUE(known(i % 2 == 0 ? SOURCE_A : SOURCE_B, keys[i]));
    }
    forget_discovered_entities_from_source(&context, SOURCE_A);
    for (int i = 0; i < COLLIDING; i++) {
        EXPECT_TRUE(known(i % 2 == 0 ? SOURCE_A : SOURCE_B, keys[i]) == (i % 2 != 0));
    }
}

// The same endpoint id from many sources: one entry each, all found.
static void test_one_endpoint_id_from_many_sources(void) {
    setup();
    for (uint32_t source = 2; source < 2 + (tt_MAX_DISCOVERED_ENTITIES / 2); source++) {
        add((uint8_t)source, 0xfeedU);
    }
    for (uint32_t source = 2; source < 2 + (tt_MAX_DISCOVERED_ENTITIES / 2); source++) {
        EXPECT_TRUE(known((uint8_t)source, 0xfeedU));
    }
#if tt_DISCOVERY_INDEXED
    EXPECT_EQ_U32(tt_MAX_DISCOVERED_ENTITIES / 2, index_entries());
#endif
}

// A full table of tombstones: a new key reclaims one, and is found; the key it replaced is not.
static void test_a_reclaimed_tombstone_changes_its_key_in_the_index(void) {
    setup();
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        add(SOURCE_A, i + 1U);
    }
    tombstone_discovered_entities_from_source(&context, SOURCE_A);
    add(SOURCE_B, 0xbeefU);
    EXPECT_TRUE(known(SOURCE_B, 0xbeefU));
    int still_known = 0;
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        still_known += known(SOURCE_A, i + 1U) ? 1 : 0;
    }
    EXPECT_EQ_INT(tt_MAX_DISCOVERED_ENTITIES - 1, still_known);
#if tt_DISCOVERY_INDEXED
    EXPECT_EQ_U32(tt_MAX_DISCOVERED_ENTITIES, index_entries());
#endif
}

// Forget and re-add a source's entries many times more than the index has entries: it holds exactly the live ones.
static void test_forget_and_readd_never_grows_the_index(void) {
    setup();
    for (int cycle = 0; cycle < CYCLES; cycle++) {
        forget_discovered_entities_from_source(&context, SOURCE_A);
        add(SOURCE_A, (uint32_t)cycle + 1U);
    }
#if tt_DISCOVERY_INDEXED
    EXPECT_EQ_U32(1, index_entries());
#endif
    EXPECT_TRUE(known(SOURCE_A, (uint32_t)CYCLES));
}

int main(void) {
    test_colliding_keys_are_found_and_forgotten();
    test_one_endpoint_id_from_many_sources();
    test_a_reclaimed_tombstone_changes_its_key_in_the_index();
    test_forget_and_readd_never_grows_the_index();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_discovery_index: all tests passed\n");
    return 0;
}
