/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// discovery_upsert_cost.c - the time a context takes to take in one remote context's full announce of ENTITIES
// endpoints into its discovery table (CONTEXT_NODE_PLAN.md 4a, 2026-09-27): upsert_discovered_entity() is a linear
// scan, so a table sized 2048 instead of 16 costs more per entity. Two cases, ROUNDS rounds each, a fresh receiver per
// round: the first announce (every entity new) and a changed announce (every entity already there).
//
// Build at rmw_tickle's settings, from the repo root (tt_MAX_DISCOVERED_ENTITIES 2048, or 16 for the old table):
//   gcc -O2 -I include -I src -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_MAX_ENDPOINT_COUNT=2048 -Dtt_ENDPOINT_INDEX_SIZE=4096
//       -Dtt_MAX_DISCOVERED_ENTITIES=2048 -o /tmp/upsert examples/perf_hil/experiments/discovery_upsert_cost.c
//       src/encoding.c src/log.c -lm
// Prints one RESULT line: medians in microseconds.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#define TEST_MOCK_DEFINE_STORAGE
#include "../../../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: process_packet() is static
#include "../../../tests/test_mock.h"

#ifndef ENTITIES
#define ENTITIES 600
#endif
#define ROUNDS 21
#define CAPTURE_MAX 256
#define REMOTE_IP 0x0a000002
#define REMOTE_PORT 8282
#define CLOCK_NS 1000000000ULL
#define NS_PER_US 1000.0
#define NS_PER_S 1000000000ULL

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
    (void)data;
    return len < 4 ? -1 : (memset(payload, 0, 4), 4);
}
static int32_t data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)native;
    return (int32_t)len;
}
static void data_free(struct tt_Data* data) {
    (void)data;
}

static struct tt_Context remote;
static struct tt_Context local;
static struct tt_Discovery discovery;
static struct tt_Topic topics[ENTITIES];
static char names[ENTITIES][24];
static struct tt_Publisher pubs[ENTITIES];

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    context->last_modified = CLOCK_NS;
}

static uint64_t wall_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now); // NOLINT(misc-include-cleaner) - <time.h>, via its own internal header
    return ((uint64_t)now.tv_sec * NS_PER_S) + (uint64_t)now.tv_nsec;
}

// The remote's announce, as captured datagrams, then handed to `local` - timed.
static uint64_t take_announce(void) {
    captured_count = 0;
    remote.last_modified++;
    if (!build_and_send_update(&remote, NULL, 0)) {
        return 0;
    }
    node_flush(&remote, 0, NULL);
    uint64_t start = wall_ns();
    for (int i = 0; i < captured_count; i++) {
        memcpy(local.rx_buffer, captured[i], captured_length[i]);
        (void)process_packet(&local, local.rx_buffer, 0, (uint32_t)captured_length[i], REMOTE_IP, REMOTE_PORT,
                             tt_TRANSPORT_UDP);
    }
    return wall_ns() - start;
}

static int compare(const void* a, const void* b) {
    uint64_t lhs = *(const uint64_t*)a;
    uint64_t rhs = *(const uint64_t*)b;
    return (lhs > rhs) - (lhs < rhs);
}

int main(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    test_mock_send_hook = capture;
    init_context(&remote, 2);
    for (int i = 0; i < ENTITIES; i++) {
        (void)snprintf(names[i], sizeof(names[i]), "/topic_%04d", i);
        topics[i].name = names[i];
        topics[i].data_size = 4;
        topics[i].data_encode_size = data_size;
        topics[i].data_encode = data_encode;
        topics[i].data_decode = data_decode;
        topics[i].data_free = data_free;
        if (tt_Context_create_publisher(&remote, &pubs[i], &topics[i], "pub") != tt_RET_OK) {
            printf("RESULT: failed create at %d\n", i);
            return 1;
        }
    }
    uint64_t first[ROUNDS];
    uint64_t changed[ROUNDS];
    int parts = 0;
    for (int round = 0; round < ROUNDS; round++) {
        init_context(&local, 1);
        memset(&discovery, 0, sizeof(discovery));
        (void)tt_Context_set_discovery(&local, &discovery, NULL, NULL);
        first[round] = take_announce();
        parts = captured_count;
        changed[round] = take_announce();
    }
    qsort(first, ROUNDS, sizeof(first[0]), compare);
    qsort(changed, ROUNDS, sizeof(changed[0]), compare);
    const uint64_t first_median = first[ROUNDS / 2];
    const uint64_t changed_median = changed[ROUNDS / 2];
    int known = 0;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        known += discovery.entities[i].context_id != tt_CONTEXT_ID_INVALID ? 1 : 0;
    }
    printf("RESULT: entities=%d table=%d datagrams=%d known=%d first_us=%.1f changed_us=%.1f\n", ENTITIES,
           tt_MAX_DISCOVERED_ENTITIES, parts, known, (double)first_median / NS_PER_US,
           (double)changed_median / NS_PER_US);
    return 0;
}
