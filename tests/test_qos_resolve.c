/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// tt_QosProfile_resolve(), table-driven: every row is a requested profile, an endpoint kind and what discovery shows,
// with the result expected. Covers SYSTEM_DEFAULT (zero fields), BEST_AVAILABLE against matched endpoints, the
// extension defaults, and the values that must be refused rather than adjusted.
//
// Control: the zeroed profile resolves to tt_QOS_PROFILE_DEFAULT exactly, and tt_QOS_PROFILE_DEFAULT resolves to
// itself - a resolver that changed nothing, or one that changed everything, fails one of the two.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- the mock HAL links against it
#include "test_mock.h"

#define PUB tt_KIND_TOPIC_PUBLISHER
#define SUB tt_KIND_TOPIC_SUBSCRIBER
#define CLI tt_KIND_SERVICE_CLIENT
#define SRV tt_KIND_SERVICE_SERVER
#define INF tt_QOS_DURATION_INFINITE
#define BA tt_QOS_DURATION_BEST_AVAILABLE
#define MS(n) ((uint64_t)(n) * tt_MILLISECOND)

// Discovery as the BEST_AVAILABLE rows see it.
static const struct tt_QosMatched none_matched = {0};
static const struct tt_QosMatched strict_peers = {.count = 2,
                                                  .all_reliable = true,
                                                  .all_durable = true,
                                                  .all_manual = true,
                                                  .deadline_ns = MS(50),
                                                  .lease_ns = MS(900)};
static const struct tt_QosMatched tiny_lease_peers = {.count = 1, .deadline_ns = INF, .lease_ns = MS(1)};
static const struct tt_QosMatched loose_peers =
    {.count = 3, .all_reliable = false, .all_durable = false, .all_manual = false, .deadline_ns = INF, .lease_ns = INF};

#define ZEROED                                              \
    {                                                       \
        .history = {.kind = tt_QOS_HISTORY_SYSTEM_DEFAULT } \
    }

// The fields a row checks. `actual` is compared whole when ret is tt_RET_OK.
struct row {
    const char* what;
    uint8_t kind;
    struct tt_QosProfile requested;
    const struct tt_QosMatched* matched;
    tt_ret_t ret;
    struct tt_QosProfile actual;
};

// The resolved default for a topic Publisher / Subscriber / service endpoint: rmw_qos_profile_default, with the
// extension fields each kind uses.
#define RESOLVED_HISTORY {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 10, .bytes = tt_QOS_HISTORY_BYTES_DEFAULT}
#define RESOLVED_TAIL                                                                   \
    .durability = {.kind = tt_QOS_DURABILITY_VOLATILE}, .deadline = {.period_ns = INF}, \
    .lifespan = {.duration_ns = INF}, .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = INF}
#define PUB_DEFAULT                                                                                           \
    {.history = RESOLVED_HISTORY,                                                                             \
     .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE, .max_blocking_ns = tt_QOS_MAX_BLOCKING_DEFAULT_NS}, \
     RESOLVED_TAIL}
#define SUB_DEFAULT                                                  \
    {.history = RESOLVED_HISTORY,                                    \
     .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE,            \
                     .tracking_words = tt_RELIABLE_BITMAP_WORDS,     \
                     .reorder_slots = tt_QOS_REORDER_SLOTS_DEFAULT}, \
     RESOLVED_TAIL}
#define SERVICE_DEFAULT \
    {.history = RESOLVED_HISTORY, .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}, RESOLVED_TAIL}

static const struct row rows[] = {
    // SYSTEM_DEFAULT: a zeroed profile is rmw_qos_profile_default on every kind.
    {"zeroed publisher", PUB, ZEROED, NULL, tt_RET_OK, PUB_DEFAULT},
    {"zeroed subscriber", SUB, ZEROED, NULL, tt_RET_OK, SUB_DEFAULT},
    {"zeroed client", CLI, ZEROED, NULL, tt_RET_OK, SERVICE_DEFAULT},
    {"zeroed server", SRV, ZEROED, NULL, tt_RET_OK, SERVICE_DEFAULT},
    {"explicit kinds, zero numbers",
     PUB,
     {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST},
      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE},
      .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
      .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC}},
     NULL,
     tt_RET_OK,
     PUB_DEFAULT},
    // KEEP_ALL takes its own depth and byte defaults.
    {"keep_all defaults",
     PUB,
     {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL}},
     NULL,
     tt_RET_OK,
     {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL,
                  .depth = tt_QOS_KEEP_ALL_DEPTH_DEFAULT,
                  .bytes = tt_QOS_KEEP_ALL_BYTES_DEFAULT},
      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE, .max_blocking_ns = tt_QOS_MAX_BLOCKING_DEFAULT_NS},
      RESOLVED_TAIL}},
    // Explicit values are kept as given.
    {"explicit values kept",
     SUB,
     {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 3, .bytes = 4096},
      .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT, .tracking_words = 2},
      .durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL},
      .deadline = {.period_ns = MS(20)},
      .lifespan = {.duration_ns = MS(30)},
      .liveliness = {.kind = tt_QOS_LIVELINESS_MANUAL_BY_TOPIC, .lease_ns = MS(500)}},
     NULL,
     tt_RET_OK,
     {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 3, .bytes = 4096},
      .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT, .tracking_words = 2}, // BEST_EFFORT: no reorder slots
      .durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL},
      .deadline = {.period_ns = MS(20)},
      .lifespan = {.duration_ns = MS(30)},
      .liveliness = {.kind = tt_QOS_LIVELINESS_MANUAL_BY_TOPIC, .lease_ns = MS(500)}}},
    {"explicit infinite stays infinite",
     PUB,
     {.deadline = {.period_ns = INF}, .lifespan = {.duration_ns = INF}, .liveliness = {.lease_ns = INF}},
     NULL,
     tt_RET_OK,
     PUB_DEFAULT},
    {"reorder none",
     SUB,
     {.reliability = {.reorder_slots = tt_QOS_REORDER_NONE, .tracking_words = tt_RELIABLE_BITMAP_MAX_WORDS}},
     NULL,
     tt_RET_OK,
     {.history = RESOLVED_HISTORY,
      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE,
                      .tracking_words = tt_RELIABLE_BITMAP_MAX_WORDS,
                      .reorder_slots = tt_QOS_REORDER_NONE},
      RESOLVED_TAIL}},
    {"publisher drops subscriber-only extensions",
     PUB,
     {.reliability = {.tracking_words = 4, .reorder_slots = 7, .max_blocking_ns = MS(5)}},
     NULL,
     tt_RET_OK,
     {.history = RESOLVED_HISTORY,
      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE, .max_blocking_ns = MS(5)},
      RESOLVED_TAIL}},
    {"sensor data on a subscriber",
     SUB,
     {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 5},
      .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}},
     NULL,
     tt_RET_OK,
     {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 5, .bytes = tt_QOS_HISTORY_BYTES_DEFAULT},
      .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT, .tracking_words = tt_RELIABLE_BITMAP_WORDS},
      RESOLVED_TAIL}},

// BEST_AVAILABLE: a Publisher offers the strict choice whatever it sees; a Subscriber requests it only when every
// matched Publisher offers it, and when it knows none.
#define ALL_BA                                                                                                   \
    {                                                                                                            \
        .reliability = {.kind = tt_QOS_RELIABILITY_BEST_AVAILABLE},                                              \
        .durability = {.kind = tt_QOS_DURABILITY_BEST_AVAILABLE}, .deadline = {.period_ns = BA}, .liveliness = { \
            .kind = tt_QOS_LIVELINESS_BEST_AVAILABLE,                                                            \
            .lease_ns = BA                                                                                       \
        }                                                                                                        \
    }
#define STRICT_TAIL(period_value, lease_value)                                                            \
    .durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL}, .deadline = {.period_ns = (period_value)}, \
    .lifespan = {.duration_ns = INF},                                                                     \
    .liveliness = {.kind = tt_QOS_LIVELINESS_MANUAL_BY_TOPIC, .lease_ns = (lease_value)}
    {"best available publisher, loose peers",
     PUB,
     ALL_BA,
     &loose_peers,
     tt_RET_OK,
     {.history = RESOLVED_HISTORY,
      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE, .max_blocking_ns = tt_QOS_MAX_BLOCKING_DEFAULT_NS},
      STRICT_TAIL(INF, INF)}},
    {"best available publisher, strict peers",
     PUB,
     ALL_BA,
     &strict_peers,
     tt_RET_OK,
     {.history = RESOLVED_HISTORY,
      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE, .max_blocking_ns = tt_QOS_MAX_BLOCKING_DEFAULT_NS},
      STRICT_TAIL(MS(50), MS(900))}},
    {"best available subscriber, strict peers",
     SUB,
     ALL_BA,
     &strict_peers,
     tt_RET_OK,
     {.history = RESOLVED_HISTORY,
      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE,
                      .tracking_words = tt_RELIABLE_BITMAP_WORDS,
                      .reorder_slots = tt_QOS_REORDER_SLOTS_DEFAULT},
      STRICT_TAIL(MS(50), MS(900))}},
    {"best available subscriber, loose peers",
     SUB,
     ALL_BA,
     &loose_peers,
     tt_RET_OK,
     {.history = RESOLVED_HISTORY,
      .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT, .tracking_words = tt_RELIABLE_BITMAP_WORDS},
      RESOLVED_TAIL}},
    {"best available subscriber, nothing matched",
     SUB,
     ALL_BA,
     &none_matched,
     tt_RET_OK,
     {.history = RESOLVED_HISTORY,
      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE,
                      .tracking_words = tt_RELIABLE_BITMAP_WORDS,
                      .reorder_slots = tt_QOS_REORDER_SLOTS_DEFAULT},
      STRICT_TAIL(INF, INF)}},
    {"best available subscriber, no discovery",
     SUB,
     ALL_BA,
     NULL,
     tt_RET_OK,
     {.history = RESOLVED_HISTORY,
      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE,
                      .tracking_words = tt_RELIABLE_BITMAP_WORDS,
                      .reorder_slots = tt_QOS_REORDER_SLOTS_DEFAULT},
      STRICT_TAIL(INF, INF)}},

    // Refused, never adjusted.
    {"history kind out of range",
     PUB,
     {.history = {.kind = (enum tt_QosHistoryKind)3}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"reliability kind out of range",
     SUB,
     {.reliability = {.kind = (enum tt_QosReliabilityKind)4}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"durability kind out of range",
     PUB,
     {.durability = {.kind = (enum tt_QosDurabilityKind)4}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"liveliness kind out of range",
     PUB,
     {.liveliness = {.kind = (enum tt_QosLivelinessKind)4}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"depth above 16 bits", PUB, {.history = {.depth = UINT16_MAX + 1U}}, NULL, tt_RET_INVALID_ARGUMENT, ZEROED},
    {"tracking window too wide",
     SUB,
     {.reliability = {.tracking_words = tt_RELIABLE_BITMAP_MAX_WORDS + 1}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"lease below the floor",
     PUB,
     {.liveliness = {.lease_ns = (tt_LIVELINESS_LEASE_DIVISOR * tt_CONTEXT_TX_INTERVAL) - 1}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"best available lease below the floor",
     SUB,
     {.liveliness = {.lease_ns = BA}},
     &tiny_lease_peers,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"client best effort is allowed",
     CLI,
     {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}},
     NULL,
     tt_RET_OK,
     {.history = RESOLVED_HISTORY, .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}, RESOLVED_TAIL}},
    {"client best available reliability",
     CLI,
     {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_AVAILABLE}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"server transient local",
     SRV,
     {.durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"server best available durability",
     SRV,
     {.durability = {.kind = tt_QOS_DURABILITY_BEST_AVAILABLE}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"client manual liveliness",
     CLI,
     {.liveliness = {.kind = tt_QOS_LIVELINESS_MANUAL_BY_TOPIC}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"server best available liveliness",
     SRV,
     {.liveliness = {.kind = tt_QOS_LIVELINESS_BEST_AVAILABLE}},
     NULL,
     tt_RET_INVALID_ARGUMENT,
     ZEROED},
    {"unknown endpoint kind", tt_KIND_NODE, ZEROED, NULL, tt_RET_INVALID_ARGUMENT, ZEROED},
};

static void expect_profile(const char* what, const struct tt_QosProfile* expected, const struct tt_QosProfile* got) {
    // Field by field, so a failure names the field rather than "the bytes differ".
    int before = test_failures;
    EXPECT_EQ_INT((int)expected->history.kind, (int)got->history.kind);
    EXPECT_EQ_U32(expected->history.depth, got->history.depth);
    EXPECT_EQ_U32(expected->history.bytes, got->history.bytes);
    EXPECT_EQ_INT((int)expected->reliability.kind, (int)got->reliability.kind);
    EXPECT_EQ_U32(expected->reliability.tracking_words, got->reliability.tracking_words);
    EXPECT_EQ_U32(expected->reliability.reorder_slots, got->reliability.reorder_slots);
    EXPECT_EQ_U64(expected->reliability.max_blocking_ns, got->reliability.max_blocking_ns);
    EXPECT_EQ_INT((int)expected->durability.kind, (int)got->durability.kind);
    EXPECT_EQ_U64(expected->deadline.period_ns, got->deadline.period_ns);
    EXPECT_EQ_U64(expected->lifespan.duration_ns, got->lifespan.duration_ns);
    EXPECT_EQ_INT((int)expected->liveliness.kind, (int)got->liveliness.kind);
    EXPECT_EQ_U64(expected->liveliness.lease_ns, got->liveliness.lease_ns);
    if (test_failures != before) {
        fprintf(stderr, "  in row: %s\n", what);
    }
}

static void test_rows(void) {
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const struct row* row = &rows[i];
        struct tt_QosProfile got;
        memset(&got, 0x5A, sizeof(got)); // a refused row must leave it untouched
        struct tt_QosProfile untouched = got;
        tt_ret_t ret = tt_QosProfile_resolve(&row->requested, row->kind, row->matched, &got);
        if (ret != row->ret) {
            fprintf(stderr, "row \"%s\": expected ret %d, got %d\n", row->what, (int)row->ret, (int)ret);
            test_failures++;
            continue;
        }
        if (ret == tt_RET_OK) {
            expect_profile(row->what, &row->actual, &got);
        } else if (!tt_QosProfile_equal(&got, &untouched)) {
            fprintf(stderr, "row \"%s\": refused but wrote the output\n", row->what);
            test_failures++;
        }
    }
}

// The control: the zeroed profile is tt_QOS_PROFILE_DEFAULT, and resolution is idempotent on every resolved row.
static void test_default_profile_and_idempotence(void) {
    struct tt_QosProfile from_null;
    struct tt_QosProfile from_default;
    EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(NULL, SUB, NULL, &from_null));
    EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(&tt_QOS_PROFILE_DEFAULT, SUB, NULL, &from_default));
    expect_profile("NULL vs tt_QOS_PROFILE_DEFAULT", &from_default, &from_null);
    struct tt_QosProfile services;
    EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(&tt_QOS_PROFILE_SERVICES_DEFAULT, CLI, NULL, &services));
    const struct tt_QosProfile service_default = SERVICE_DEFAULT;
    expect_profile("services default", &service_default, &services);
    struct tt_QosProfile sensor;
    EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(&tt_QOS_PROFILE_SENSOR_DATA, PUB, NULL, &sensor));
    EXPECT_EQ_INT((int)tt_QOS_RELIABILITY_BEST_EFFORT, (int)sensor.reliability.kind);
    EXPECT_EQ_U32(5, sensor.history.depth);

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (rows[i].ret != tt_RET_OK) {
            continue;
        }
        struct tt_QosProfile again = rows[i].actual;
        // In place (actual aliases requested), and with discovery that would flip any BEST_AVAILABLE left over.
        EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(&again, rows[i].kind, &loose_peers, &again));
        expect_profile(rows[i].what, &rows[i].actual, &again);
    }
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_QosProfile_resolve(NULL, PUB, NULL, NULL));
}

int main(void) {
    test_rows();
    test_default_profile_and_idempotence();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_qos_resolve: all tests passed\n");
    return 0;
}
