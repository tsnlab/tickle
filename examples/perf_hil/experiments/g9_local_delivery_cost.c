/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g9 (rmw_tickle/RMW_GAPS_PLAN.md): what in-process delivery costs a publish, on the mock HAL (no socket), -O2.
// Arms, interleaved, 5 rounds of 200000 publishes of a 64-byte sample each, median ns per publish reported:
//   none   - no local Subscriber (built with tt_LOCAL_DELIVERY 1: the one branch; build with -Dtt_LOCAL_DELIVERY=0 for
//            the parent's cost of the same arm)
//   local  - one local Subscriber whose callback does nothing: the copy into the context's scratch and the delivery
// Reported, not judged (pre-registered in RMW_GAPS_PLAN.md g9: "the in-process path's own cost is recorded").
//
// Build from the repo root: cc -O2 -DNDEBUG -Iinclude -Isrc -Dtt_LOCAL_DELIVERY=1 \
//   examples/perf_hil/experiments/g9_local_delivery_cost.c src/encoding.c src/log.c -lm -o /tmp/g9_cost
// A 60 KB sample, at rmw_tickle's datagram size: add -DSAMPLE=60000 -Dtt_MAX_BUFFER_LENGTH=65507.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tickle/config.h> // tt_LOCAL_DELIVERY, tt_TX_BUFFER_LENGTH
#include <tickle/tickle.h>

#define TEST_MOCK_DEFINE_STORAGE
#include "../../../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: a context without sockets
#include "../../../tests/test_mock.h"

#ifndef SAMPLE
#define SAMPLE 64
#endif
#define PUBLISHES 200000
#define ROUNDS 5
#define NS_PER_S 1000000000ULL

static int32_t size_of(struct tt_Data* data) {
    (void)data;
    return SAMPLE;
}
static int32_t encode_sample(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < SAMPLE) {
        return -1;
    }
    memcpy(payload, data, SAMPLE);
    return SAMPLE;
}
static struct tt_Data* view_sample(const uint8_t* payload, uint32_t length, bool native) {
    (void)length;
    (void)native;
    return (struct tt_Data*)payload;
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
static volatile uint32_t delivered;
static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
    delivered++;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - <time.h>, via its own internal header
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static struct tt_Context context;
static struct tt_Topic topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static uint8_t sample[SAMPLE];

static double run(bool with_local) {
    memset(&context, 0, sizeof(context));
    node_init_locks(&context);
    context.id = 1;
    context.tx_tail = sizeof(struct tt_Header);
    context.tx_size = tt_TX_BUFFER_LENGTH;
    memset(&pub, 0, sizeof(pub));
    memset(&sub, 0, sizeof(sub));
    (void)tt_Context_create_publisher(&context, &pub, &topic, "rt/cost");
    if (with_local) {
        (void)tt_Context_create_subscriber(&context, &sub, &topic, "rt/cost", on_sample);
    }
    uint64_t start = now_ns();
    for (int i = 0; i < PUBLISHES; i++) {
        sample[0] = (uint8_t)i;
        (void)tt_Publisher_publish(&pub, (struct tt_Data*)sample);
    }
    return (double)(now_ns() - start) / PUBLISHES;
}

static int compare(const void* a, const void* b) {
    double first = *(const double*)a;
    double second = *(const double*)b;
    return (first > second) - (first < second);
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR; // NOLINT(misc-include-cleaner) - src/log.h, reached through tickle.c
    test_mock_reset();
    topic.name = "g9::msg::dds_::Cost_";
    topic.data_size = SAMPLE;
    topic.data_encode_size = size_of;
    topic.data_encode = encode_sample;
    topic.data_decode = refuse;
    topic.data_decode_inplace = view_sample;
    topic.data_free = free_nothing;
    double none[ROUNDS];
    double local[ROUNDS];
    for (int round = 0; round < ROUNDS; round++) {
        none[round] = run(false);
        local[round] = run(true);
    }
    qsort(none, ROUNDS, sizeof(double), compare);
    qsort(local, ROUNDS, sizeof(double), compare);
    printf("tt_LOCAL_DELIVERY=%d sample=%d B: none %.1f ns/publish, local %.1f ns/publish (medians of %d x %d)\n",
           tt_LOCAL_DELIVERY, SAMPLE, none[ROUNDS / 2], local[ROUNDS / 2], ROUNDS, PUBLISHES);
    return 0;
}
