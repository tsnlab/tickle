/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions, bundle 2 (ROADMAP "Context options"): the discovery and liveliness timings
// (struct tt_DiscoveryOptions), on the mock clock.
//   - the defaults are the config.h constants they replace;
//   - every field, set to a value other than its default, changes when the context does something (no field written
//     and never read) - each test compares the default against the changed value, so the default arm is the control;
//   - every check refuses a bad value with tt_RET_INVALID_ARGUMENT and accepts the boundary beside it
//     (tests/mutants_init_options_discovery.sh removes each check, and each field's reading, and expects a failure);
//   - a context whose update interval or miss threshold differs from the default says so, once, at INFO and not at
//     WARNING: since 2026-10-22 its summary carries both and its peers judge its silence by them
//     (tests/test_discovery_timing.c); only a peer built before then judges by its own.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_discovery.sh passes a mutant

#define AUTO_ID 7 // what the mock's address auto-detection answers
#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define REMOTE_IP 0x0a000002U
#define PUB_ENDPOINT_ID 0x11111111
#define START (5 * tt_SECOND)

static struct tt_Context ctx;

static void reset_mock(void) {
    test_mock_reset();
    test_mock_now = START;
    test_mock_node_id = AUTO_ID;
    test_mock_link_resolves = true; // every link resolves to 10.0.0.7/24, as an interface would
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    test_mock_link_last_broadcast = NULL;
#if tt_CONTEXT_ID_CLAIM
    memset(test_mock_ids_held, 0, sizeof(test_mock_ids_held));
#endif
}

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts;
}

static tt_ret_t create_with(const struct tt_InitOptions* opts) {
    memset(&ctx, 0, sizeof(ctx));
    return tt_Context_create_with(&ctx, opts);
}

static void destroy(void) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
}

// A context built by hand, as the whitebox tests build theirs, holding `discovery` as a created one would.
static void init_node(struct tt_Context* node, const struct tt_DiscoveryOptions* discovery) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    struct tt_InitOptions opts = defaults();
    if (discovery != NULL) {
        opts.discovery = *discovery;
        resolve_init_options(&opts);
    }
    node->opts = opts;
}

static struct tt_DiscoveryOptions none(void) {
    struct tt_DiscoveryOptions discovery;
    memset(&discovery, 0, sizeof(discovery));
    return discovery;
}

// When `function` is next due on the node's scheduler; 0 when it is not scheduled.
static uint64_t scheduled_at(const struct tt_Context* node, void (*function)(struct tt_Context*, uint64_t, void*)) {
    for (int i = 0; i < node->scheduler_tail; i++) {
        if (node->scheduler[i].function == function) {
            return node->scheduler[i].time;
        }
    }
    return 0;
}

static void run_scheduler_until(struct tt_Context* node, uint64_t until) {
    while (true) {
        uint64_t head = UINT64_MAX;
        if (!sched_next_time(node, &head) || head > until) {
            break;
        }
        if (head > test_mock_now) {
            test_mock_now = head;
        }
        bool has_next = false;
        uint64_t next = 0;
        (void)run_due_entry(node, test_mock_now, &has_next, &next);
    }
    test_mock_now = until;
}

// ---- A remote node heard once, by an announce (the liveliness tests' own helpers, kept local).

#define TEST_ENTITY_ID_OF(endpoint_id) ((uint32_t)(endpoint_id) ^ 0xE1D00000U)

static void hear_remote(struct tt_Context* node, uint64_t at) {
    test_mock_now = at;
    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;
    uint8_t* buf = node->rx_buffer;
    struct test_announce* update = test_announce_at(buf);
    test_announce_set_last_modified(update, 100);
    update->announce.entity_count = 1;
    uint32_t tail = sizeof(struct test_announce);
    struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)(buf + tail);
    entity->endpoint_id = PUB_ENDPOINT_ID;
    entity->entity_id = TEST_ENTITY_ID_OF(PUB_ENDPOINT_ID);
    entity->kind = tt_KIND_TOPIC_SUBSCRIBER;
    tail += sizeof(struct tt_UpdateEntity);
    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, "topic");
    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, "sub");
    EXPECT_TRUE(process_data(node, &header, buf, 0, tail, REMOTE_IP, 8282));
    EXPECT_TRUE(node->update_seen[REMOTE_NODE_ID]);
}

// Whether the remote node heard at 0 is still alive when the liveliness check runs at `at`.
static bool alive_at(const struct tt_DiscoveryOptions* discovery, uint64_t at) {
    static struct tt_Context node;
    init_node(&node, discovery);
    hear_remote(&node, 0);
    test_mock_now = at;
    check_liveliness(&node, at, NULL);
    return node.update_seen[REMOTE_NODE_ID];
}

// ---- Defaults.

static void test_defaults_are_the_compiled_settings(void) {
    struct tt_InitOptions opts = defaults();
    const struct tt_DiscoveryOptions* d = &opts.discovery;
    EXPECT_EQ_U64(tt_CONTEXT_CYCLE, d->cycle_ns);
    EXPECT_EQ_U64(tt_CONTEXT_UPDATE_INTERVAL, d->update_interval_ns);
    EXPECT_EQ_U64(tt_CONTEXT_TX_INTERVAL, d->tx_interval_ns);
    EXPECT_EQ_U64(tt_CONTEXT_MAX_LEASE_NS, d->max_lease_ns);
    EXPECT_EQ_U64(tt_DISCOVERY_REQUEST_RETRY, d->request_retry_ns);
    EXPECT_EQ_U64(tt_LIVELINESS_DEFER_NS, d->liveliness_defer_ns);
    EXPECT_EQ_U64(tt_SCHED_FULL_WARN_INTERVAL, d->sched_full_warn_interval_ns);
    EXPECT_EQ_U32(tt_LIVELINESS_LEASE_DIVISOR, d->lease_divisor);
    EXPECT_EQ_U32(tt_DISCOVERY_REQUEST_ATTEMPTS, d->request_attempts);
    EXPECT_EQ_U32(tt_LIVELINESS_MAX_DEFERRALS, d->liveliness_max_deferrals);
    EXPECT_EQ_U32(tt_LIVELINESS_MISS_THRESHOLD, d->miss_threshold);
    // The derived silence is config.h's, for a created context and for one built by hand.
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(NULL));
    EXPECT_EQ_U64(tt_LIVELINESS_SILENCE_NS, discovery_silence_ns(&ctx));
    destroy();
    static struct tt_Context zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    EXPECT_EQ_U64(tt_LIVELINESS_SILENCE_NS, discovery_silence_ns(&zeroed));
    EXPECT_EQ_U64((uint64_t)tt_LIVELINESS_LEASE_DIVISOR * tt_CONTEXT_TX_INTERVAL, discovery_lease_floor_ns(&zeroed));
}

// ---- Each field changes behaviour.

// cycle_ns and update_interval_ns at creation: the first summary on the next cycle boundary, the first liveliness
// check an update interval after it.
static void test_cycle_and_interval_arm_the_first_tasks(void) {
    const uint64_t now = START + (300 * tt_MICROSECOND);
    reset_mock();
    test_mock_now = now;
    EXPECT_EQ_INT(tt_RET_OK, create_with(NULL));
    EXPECT_EQ_U64(START + tt_CONTEXT_CYCLE, ctx.next_summary_ns);
    EXPECT_EQ_U64(START + tt_CONTEXT_CYCLE + tt_CONTEXT_UPDATE_INTERVAL, scheduled_at(&ctx, check_liveliness));
    destroy();

    reset_mock();
    test_mock_now = now;
    struct tt_InitOptions opts = defaults();
    opts.discovery.cycle_ns = 10 * tt_MILLISECOND;
    opts.discovery.update_interval_ns = 2 * tt_SECOND;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&opts));
    EXPECT_EQ_U64(START + (10 * tt_MILLISECOND), ctx.next_summary_ns);
    EXPECT_EQ_U64(START + (10 * tt_MILLISECOND) + (2 * tt_SECOND), scheduled_at(&ctx, check_liveliness));
    destroy();
}

// update_interval_ns: the summary period, and the period of the liveliness check.
static void test_update_interval_is_the_period(void) {
    static struct tt_Context node;
    init_node(&node, NULL);
    EXPECT_EQ_U64(tt_CONTEXT_UPDATE_INTERVAL, summary_interval(&node));
    check_liveliness(&node, 1000, NULL);
    EXPECT_EQ_U64(1000 + tt_CONTEXT_UPDATE_INTERVAL, scheduled_at(&node, check_liveliness));

    struct tt_DiscoveryOptions d = none();
    d.update_interval_ns = 3 * tt_SECOND;
    init_node(&node, &d);
    EXPECT_EQ_U64(3 * tt_SECOND, summary_interval(&node));
    check_liveliness(&node, 1000, NULL);
    EXPECT_EQ_U64(1000 + (3 * tt_SECOND), scheduled_at(&node, check_liveliness));
}

// update_interval_ns and miss_threshold: the silence after which a node is presumed dead.
static void test_interval_and_threshold_are_the_silence(void) {
    // Default: 3.5 s.
    EXPECT_TRUE(alive_at(NULL, tt_LIVELINESS_SILENCE_NS));
    EXPECT_TRUE(!alive_at(NULL, tt_LIVELINESS_SILENCE_NS + 1));

    struct tt_DiscoveryOptions d = none();
    d.update_interval_ns = 2 * tt_SECOND; // 3.5 x 2 s
    EXPECT_TRUE(alive_at(&d, 7 * tt_SECOND));
    EXPECT_TRUE(!alive_at(&d, (7 * tt_SECOND) + 1));

    d = none();
    d.miss_threshold = 5; // 5.5 x 1 s
    EXPECT_TRUE(alive_at(&d, (5 * tt_SECOND) + (tt_SECOND / 2)));
    EXPECT_TRUE(!alive_at(&d, (5 * tt_SECOND) + (tt_SECOND / 2) + 1));
}

// max_lease_ns: how long a silent node is kept for one of its entities' long leases.
static bool alive_with_long_lease_at(const struct tt_DiscoveryOptions* discovery, uint64_t at) {
    static struct tt_Context node;
    static struct tt_Discovery cache;
    init_node(&node, discovery);
    memset(&cache, 0, sizeof(cache));
    node.discovery = &cache;
    hear_remote(&node, 0);
    bool found = false;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        if (cache.entities[i].context_id == REMOTE_NODE_ID) {
            cache.entities[i].liveliness_lease_duration_ns = 60 * tt_SECOND;
            found = true;
        }
    }
    EXPECT_TRUE(found);
    test_mock_now = at;
    check_liveliness(&node, at, NULL);
    return node.update_seen[REMOTE_NODE_ID];
}

static void test_max_lease_caps_a_long_lease(void) {
    EXPECT_TRUE(alive_with_long_lease_at(NULL, tt_CONTEXT_MAX_LEASE_NS));
    EXPECT_TRUE(!alive_with_long_lease_at(NULL, tt_CONTEXT_MAX_LEASE_NS + 1));
    struct tt_DiscoveryOptions d = none();
    d.max_lease_ns = 20 * tt_SECOND;
    EXPECT_TRUE(alive_with_long_lease_at(&d, 20 * tt_SECOND));
    EXPECT_TRUE(!alive_with_long_lease_at(&d, (20 * tt_SECOND) + 1));
}

// tx_interval_ns: the flush tick a batched send waits for, on the clock's grid of it.
static uint64_t flush_due(const struct tt_DiscoveryOptions* discovery) {
    static struct tt_Context node;
    init_node(&node, discovery);
    node.tx_tail = sizeof(struct tt_Header) + 8; // something batched
    test_mock_now = START + (300 * tt_MICROSECOND);
    ensure_flush_scheduled(&node);
    return scheduled_at(&node, node_flush);
}

static void test_tx_interval_is_the_flush_tick(void) {
    EXPECT_EQ_U64(START + tt_CONTEXT_TX_INTERVAL, flush_due(NULL));
    struct tt_DiscoveryOptions d = none();
    d.tx_interval_ns = 10 * tt_MILLISECOND;
    EXPECT_EQ_U64(START + (10 * tt_MILLISECOND), flush_due(&d));
}

// lease_divisor and tx_interval_ns: the summary period under a short lease, and the shortest lease an endpoint of the
// context may ask for (the context's floor; tt_QosProfile_resolve(), with no context, keeps the default's).
static struct tt_Context bare;
static struct tt_TypeSupport type;
static struct tt_Publisher pub;

static int32_t sample_size(struct tt_Data* data) {
    (void)data;
    return 8;
}

static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0, len);
    return 8;
}

static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static tt_ret_t create_leased_publisher(const struct tt_DiscoveryOptions* discovery, uint64_t lease_ns) {
    init_node(&bare, discovery);
    memset(&type, 0, sizeof(type));
    type.name = "lease_type";
    type.data_size = 8;
    type.data_encode_size = sample_size;
    type.data_encode = sample_encode;
    type.data_decode = sample_decode;
    type.data_free = free_nothing;
    type.max_encoded_size = 8;
    memset(&pub, 0, sizeof(pub));
    struct tt_QosProfile qos = tt_QOS_PROFILE_SENSOR_DATA;
    qos.liveliness.lease_ns = lease_ns;
    return tt_Node_create_publisher(tt_Context_default_node(&bare), &pub, &type, "lease_topic", &qos, NULL, NULL, 0);
}

static void test_lease_divisor_and_tx_interval_are_the_lease_floor(void) {
    const uint64_t floor = (uint64_t)tt_LIVELINESS_LEASE_DIVISOR * tt_CONTEXT_TX_INTERVAL; // 6 ms
    EXPECT_EQ_INT(tt_RET_OK, create_leased_publisher(NULL, floor));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create_leased_publisher(NULL, floor - 1));

    struct tt_DiscoveryOptions d = none();
    d.lease_divisor = tt_LIVELINESS_LEASE_DIVISOR + 1;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create_leased_publisher(&d, floor));
    EXPECT_EQ_INT(tt_RET_OK, create_leased_publisher(&d, floor + tt_CONTEXT_TX_INTERVAL));

    d = none();
    d.tx_interval_ns = 2 * tt_CONTEXT_TX_INTERVAL;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create_leased_publisher(&d, floor));
    EXPECT_EQ_INT(tt_RET_OK, create_leased_publisher(&d, 2 * floor));

    d = none();
    d.lease_divisor = 2;
    EXPECT_EQ_INT(tt_RET_OK, create_leased_publisher(&d, 2 * tt_CONTEXT_TX_INTERVAL)); // below the default's floor
    // The context-free resolution keeps the default floor.
    struct tt_QosProfile qos = tt_QOS_PROFILE_SENSOR_DATA;
    struct tt_QosProfile actual;
    qos.liveliness.lease_ns = 2 * tt_CONTEXT_TX_INTERVAL;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_QosProfile_resolve(&qos, tt_KIND_TOPIC_PUBLISHER, NULL, &actual));
}

static void test_lease_divisor_is_the_summaries_per_lease(void) {
    EXPECT_EQ_INT(tt_RET_OK, create_leased_publisher(NULL, 60 * tt_MILLISECOND));
    EXPECT_EQ_U64(10 * tt_MILLISECOND, summary_interval(&bare));
    struct tt_DiscoveryOptions d = none();
    d.lease_divisor = 3;
    EXPECT_EQ_INT(tt_RET_OK, create_leased_publisher(&d, 60 * tt_MILLISECOND));
    EXPECT_EQ_U64(20 * tt_MILLISECOND, summary_interval(&bare));
}

// request_retry_ns and request_attempts: when an unanswered list request is sent again, and how often in all.
static int datagrams_sent;

static void count_datagram(const void* buf, size_t len) {
    (void)buf;
    (void)len;
    datagrams_sent++;
}

// Requests sent by `after` for a summary of a peer that never answers.
static int requests_by(const struct tt_DiscoveryOptions* discovery, uint64_t after) {
    static struct tt_Context node;
    reset_mock();
    test_mock_send_hook = count_datagram;
    datagrams_sent = 0;
    init_node(&node, discovery);
    uint64_t asked = test_mock_now;
    EXPECT_TRUE(process_discovery_summary(&node, REMOTE_NODE_ID, 7, REMOTE_IP, 8282));
    run_scheduler_until(&node, asked + after);
    test_mock_send_hook = NULL;
    return datagrams_sent;
}

static void test_request_retry_and_attempts(void) {
    EXPECT_EQ_INT(1, requests_by(NULL, tt_DISCOVERY_REQUEST_RETRY - 1));
    EXPECT_EQ_INT(2, requests_by(NULL, tt_DISCOVERY_REQUEST_RETRY));
    EXPECT_EQ_INT(tt_DISCOVERY_REQUEST_ATTEMPTS, requests_by(NULL, tt_SECOND));

    struct tt_DiscoveryOptions d = none();
    d.request_retry_ns = 3 * tt_DISCOVERY_REQUEST_RETRY;
    EXPECT_EQ_INT(1, requests_by(&d, (3 * tt_DISCOVERY_REQUEST_RETRY) - 1));
    EXPECT_EQ_INT(2, requests_by(&d, 3 * tt_DISCOVERY_REQUEST_RETRY));

    d = none();
    d.request_attempts = tt_DISCOVERY_REQUEST_ATTEMPTS + 2;
    EXPECT_EQ_INT(tt_DISCOVERY_REQUEST_ATTEMPTS + 2, requests_by(&d, tt_SECOND));
}

// liveliness_defer_ns and liveliness_max_deferrals: a check put off while datagrams wait unread.
static struct tt_Context deferring;

static void start_busy(const struct tt_DiscoveryOptions* discovery) {
    reset_mock();
    init_node(&deferring, discovery);
    hear_remote(&deferring, 0);
    test_mock_now = tt_LIVELINESS_SILENCE_NS * 3;
    test_mock_try_receive_remaining = 1; // buffered and never drained: nothing here reads the socket
}

// Checks run back to back until the remote node is judged dead; how many were deferred first.
static uint32_t deferrals_before_judging(const struct tt_DiscoveryOptions* discovery) {
    start_busy(discovery);
    uint32_t deferred = 0;
    for (int i = 0; i < 300 && deferring.update_seen[REMOTE_NODE_ID]; i++) {
        uint64_t before = deferring.liveliness_deferrals_total;
        check_liveliness(&deferring, test_mock_now, NULL);
        deferred += (uint32_t)(deferring.liveliness_deferrals_total - before);
    }
    test_mock_try_receive_remaining = 0;
    EXPECT_TRUE(!deferring.update_seen[REMOTE_NODE_ID]);
    return deferred;
}

static void test_liveliness_deferral(void) {
    start_busy(NULL);
    check_liveliness(&deferring, test_mock_now, NULL);
    EXPECT_EQ_U64(test_mock_now + tt_LIVELINESS_DEFER_NS, scheduled_at(&deferring, check_liveliness));
    struct tt_DiscoveryOptions d = none();
    d.liveliness_defer_ns = 7 * tt_MILLISECOND;
    start_busy(&d);
    check_liveliness(&deferring, test_mock_now, NULL);
    EXPECT_EQ_U64(test_mock_now + (7 * tt_MILLISECOND), scheduled_at(&deferring, check_liveliness));
    test_mock_try_receive_remaining = 0;

    EXPECT_EQ_U32(tt_LIVELINESS_MAX_DEFERRALS, deferrals_before_judging(NULL));
    d = none();
    d.liveliness_max_deferrals = 9;
    EXPECT_EQ_U32(9, deferrals_before_judging(&d));
    d.liveliness_max_deferrals = tt_LIVELINESS_NO_DEFERRALS;
    EXPECT_EQ_U32(0, deferrals_before_judging(&d));
}

// sched_full_warn_interval_ns: a full scheduler is warned about at most this often.
static uint64_t warned_after(const struct tt_DiscoveryOptions* discovery, uint64_t gap) {
    static struct tt_Context node;
    init_node(&node, discovery);
    test_mock_now = START;
    sched_note_full(&node);
    test_mock_now = START + gap;
    sched_note_full(&node);
    EXPECT_EQ_U64(2, node.sched_full);
    return node.sched_full_warned_ns;
}

static void test_sched_full_warn_interval(void) {
    EXPECT_EQ_U64(START, warned_after(NULL, tt_SCHED_FULL_WARN_INTERVAL - 1));
    EXPECT_EQ_U64(START + tt_SCHED_FULL_WARN_INTERVAL, warned_after(NULL, tt_SCHED_FULL_WARN_INTERVAL));
    struct tt_DiscoveryOptions d = none();
    d.sched_full_warn_interval_ns = tt_SECOND;
    EXPECT_EQ_U64(START + tt_SECOND, warned_after(&d, tt_SECOND));
}

// ---- Checks.

static void expect_refused(const struct tt_InitOptions* opts, const char* what) {
    reset_mock();
    tt_ret_t ret = create_with(opts);
    if (ret != tt_RET_INVALID_ARGUMENT) {
        fprintf(stderr, "  expected %s to be refused, got %d\n", what, ret);
    }
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, ret);
}

static void expect_accepted(const struct tt_InitOptions* opts, const char* what) {
    reset_mock();
    tt_ret_t ret = create_with(opts);
    if (ret != tt_RET_OK) {
        fprintf(stderr, "  expected %s to be accepted, got %d\n", what, ret);
    }
    EXPECT_EQ_INT(tt_RET_OK, ret);
    if (ret == tt_RET_OK) {
        destroy();
    }
}

static void test_validation(void) {
    // Every duration: at most a day. tx_interval_ns must also stay at most update_interval_ns: its own arm follows.
    struct tt_InitOptions opts = defaults();
    uint64_t* durations[] = {&opts.discovery.cycle_ns,
                             &opts.discovery.update_interval_ns,
                             &opts.discovery.max_lease_ns,
                             &opts.discovery.request_retry_ns,
                             &opts.discovery.liveliness_defer_ns,
                             &opts.discovery.sched_full_warn_interval_ns};
    for (size_t i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
        opts = defaults();
        *durations[i] = tt_DISCOVERY_MAX_DURATION_NS;
        expect_accepted(&opts, "a duration of one day");
        *durations[i] = tt_DISCOVERY_MAX_DURATION_NS + 1;
        expect_refused(&opts, "a duration over a day");
    }
    opts = defaults();
    opts.discovery.update_interval_ns = tt_DISCOVERY_MAX_DURATION_NS;
    opts.discovery.tx_interval_ns = tt_DISCOVERY_MAX_DURATION_NS;
    expect_accepted(&opts, "discovery.tx_interval_ns of one day");
    opts.discovery.tx_interval_ns = tt_DISCOVERY_MAX_DURATION_NS + 1;
    expect_refused(&opts, "discovery.tx_interval_ns over a day");

    // tx_interval_ns at most update_interval_ns.
    opts = defaults();
    opts.discovery.tx_interval_ns = opts.discovery.update_interval_ns;
    expect_accepted(&opts, "discovery.tx_interval_ns = update_interval_ns");
    opts.discovery.tx_interval_ns = opts.discovery.update_interval_ns + 1;
    expect_refused(&opts, "discovery.tx_interval_ns above update_interval_ns");

#if tt_SEGMENT_ENABLED
    // A silence not above the build's tt_SEGMENT_DEAD_READER_NS (3.5 x 285714285 ns) was refused here until bundle 8:
    // the context's dead-reader time is now derived from its own interval, so it stays below the silence, and the
    // order is checked with the segment (tests/test_init_options_segment.c).
    opts = defaults();
    opts.discovery.update_interval_ns = (tt_SEGMENT_DEAD_READER_NS * 2U) / 7U;
    expect_accepted(&opts,
                    "a silence not above the build's tt_SEGMENT_DEAD_READER_NS: the dead-reader time follows it");
#endif
}

// The interop line: logged when update_interval_ns or miss_threshold differs from the default, and not otherwise; at
// INFO, which a WARNING level hides (it was a WARNING until the summary carried the two, 2026-10-22). The count of
// lines that `level` lets through.
static int lines_creating(const struct tt_InitOptions* opts, tt_LogLevel at) {
    FILE* log = tmpfile();
    EXPECT_TRUE(log != NULL);
    if (log == NULL) {
        return -1;
    }
    tt_LogLevel level = tt_current_log_level;
    tt_current_log_level = at;
    tt_log_set_output(log);
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(opts));
    destroy();
    tt_log_set_output(stderr);
    tt_current_log_level = level;
    EXPECT_EQ_INT(0, fseek(log, 0, SEEK_SET));
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), log) != NULL) {
        if (strstr(line, "tt_InitOptions.discovery: update_interval_ns") != NULL) {
            found++;
        }
    }
    fclose(log);
    return found;
}

static void test_interop_line(void) {
    EXPECT_EQ_INT(0, lines_creating(NULL, TT_LOG_INFO));
    struct tt_InitOptions opts = defaults();
    opts.discovery.tx_interval_ns = 2 * tt_MILLISECOND; // not judged by the peers: no line
    opts.discovery.max_lease_ns = 20 * tt_SECOND;
    EXPECT_EQ_INT(0, lines_creating(&opts, TT_LOG_INFO));
    opts = defaults();
    opts.discovery.update_interval_ns = 2 * tt_SECOND;
    EXPECT_EQ_INT(1, lines_creating(&opts, TT_LOG_INFO));
    EXPECT_EQ_INT(0, lines_creating(&opts, TT_LOG_WARNING));
    opts = defaults();
    opts.discovery.miss_threshold = 5;
    EXPECT_EQ_INT(1, lines_creating(&opts, TT_LOG_INFO));
    EXPECT_EQ_INT(0, lines_creating(&opts, TT_LOG_WARNING));
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_defaults_are_the_compiled_settings();
    test_cycle_and_interval_arm_the_first_tasks();
    test_update_interval_is_the_period();
    test_interval_and_threshold_are_the_silence();
    test_max_lease_caps_a_long_lease();
    test_tx_interval_is_the_flush_tick();
    test_lease_divisor_and_tx_interval_are_the_lease_floor();
    test_lease_divisor_is_the_summaries_per_lease();
    test_request_retry_and_attempts();
    test_liveliness_deferral();
    test_sched_full_warn_interval();
    test_validation();
    test_interop_line();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_discovery: all tests passed\n");
    return 0;
}
