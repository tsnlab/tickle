/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions, bundle 3 (ROADMAP "Context options"): the reliable group, struct tt_ReliableOptions.
//   - the defaults are config.h's constants, and creation with a zeroed group, with NULL and with the defaults all
//     give a context those values;
//   - every field, set to a value other than its default, changes what the context does, on the mock clock: the same
//     scenario is run with the default and with the other value, and the two must differ in the way the field says
//     (no field written and never read). The scenarios are the code paths that read the field: the reader's ACKNACK
//     retry timer and its back-off, the writer's resend suppression, the stuck-gap warning, the idle HEARTBEAT;
//   - every check refuses a bad value with tt_RET_INVALID_ARGUMENT and accepts the boundary beside it
//     (tests/mutants_init_options_reliable.sh removes each check and expects this test to fail);
//   - two contexts in one process keep their own retry interval (tt_reliable_retry_interval_configured() is per
//     context).
// Whitebox contexts here are built by hand, as test_reliable_pubsub.c builds them, with their options resolved as
// creation resolves them (resolve_init_options()); creation itself is exercised by the defaults and checks below.

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_reliable.sh passes a mutant

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define ENDPOINT_ID 0xaabbccdd
#define SENDER_IP 0x0a000001
#define SENDER_PORT 12345
#define US 1000ULL
#define MS 1000000ULL

// ---- A hand-built context, Publisher and Subscriber (test_reliable_pubsub.c's shapes), with the given options.

static int32_t u32_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}

static int32_t u32_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static int32_t u32_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void u32_free(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
}

static struct tt_Context node;
static struct tt_TypeSupport topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;

// `reliable` as tt_Context_create_with() would take it: resolved into the context's own copy, then checked.
static void init_node(const struct tt_ReliableOptions* reliable) {
    test_mock_reset();
    test_mock_now = 10 * tt_SECOND;
    memset(&node, 0, sizeof(node));
    node.opts.reliable = *reliable;
    resolve_init_options(&node.opts);
    EXPECT_EQ_INT(tt_RET_OK, validate_init_options(&node.opts));
    node_init_locks(&node);
    node.id = LOCAL_NODE_ID;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;

    memset(&topic, 0, sizeof(topic));
    topic.name = "reliable_options";
    topic.data_size = sizeof(uint32_t);
    topic.data_encode_size = u32_encode_size;
    topic.data_encode = u32_encode;
    topic.data_decode = u32_decode;
    topic.data_free = u32_free;
    topic.max_encoded_size = sizeof(uint32_t);
}

#define REORDER_SLOTS 16
#define REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + 64)
static uint64_t reorder_storage[REORDER_SLOTS * REORDER_SLOT_BYTES / sizeof(uint64_t)];

static void init_subscriber(void) {
    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = ENDPOINT_ID;
    sub.node = &node;
    sub.topic = &topic;
    sub.callback = on_data;
    sub.reliable = true;
    sub.reorder_storage = reorder_storage;
    sub.reorder_slots = REORDER_SLOTS;
    sub.reorder_slot_bytes = REORDER_SLOT_BYTES;
    memset(reorder_storage, 0, sizeof(reorder_storage));
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub.writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&sub;
}

static void init_header(struct tt_Header* header) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
}

static void receive_data(uint32_t seq_no) {
    struct tt_Header header;
    init_header(&header);
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node.rx_buffer;
    memset(data_header, 0, sizeof(*data_header));
    data_header->endpoint_id = ENDPOINT_ID;
    data_header->seq_no = seq_no;
    data_header->timestamp = seq_no * 100ULL;
    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node.rx_buffer + tail, &seq_no, sizeof(seq_no));
    tail += sizeof(seq_no);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, SENDER_IP, SENDER_PORT));
}

static uint64_t scheduled_retry_at(const struct tt_WriterProxy* proxy) {
    for (int32_t i = 0; i < node.scheduler_tail; i++) {
        if (node.scheduler[i].function == acknack_retry && node.scheduler[i].param == proxy) {
            return node.scheduler[i].time;
        }
    }
    return 0;
}

// A reader with 1 received and 2 lost (3 arrived): its gap's retry timer armed. Answers the writer's proxy.
static struct tt_WriterProxy* reader_with_a_gap(const struct tt_ReliableOptions* reliable) {
    init_node(reliable);
    init_subscriber();
    receive_data(1);
    receive_data(3);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, REMOTE_NODE_ID, 0);
    EXPECT_TRUE(proxy != NULL);
    if (proxy != NULL) {
        proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
        EXPECT_TRUE(proxy->acknack_scheduled);
    }
    return proxy;
}

// Runs the reader's retry timer `count` times with the writer silent, each time at the moment it was scheduled for,
// and writes each wait (scheduled - now) into waits[].
static void silent_retries(struct tt_WriterProxy* proxy, uint64_t* waits, int count) {
    for (int i = 0; i < count; i++) {
        uint64_t due = scheduled_retry_at(proxy);
        EXPECT_TRUE(due >= test_mock_now);
        test_mock_now = due;
        tt_Context_unschedule(&node, acknack_retry, proxy);
        acknack_retry(&node, test_mock_now, proxy);
        waits[i] = scheduled_retry_at(proxy) - test_mock_now;
    }
}

// The mock as tt_Context_create_with() needs it: every link resolves, as an interface would.
static void reset_for_creation(void) {
    test_mock_reset();
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
}

static struct tt_ReliableOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts.reliable;
}

// ---- The defaults.

static bool same_reliable(const struct tt_ReliableOptions* a, const struct tt_ReliableOptions* b) {
    return a->deadline == b->deadline && a->retry_interval == b->retry_interval &&
           a->retry_initial == b->retry_initial && a->retry_granularity == b->retry_granularity &&
           a->timer_lateness_initial == b->timer_lateness_initial && a->stuck_warn_interval == b->stuck_warn_interval &&
           a->idle_heartbeat_period == b->idle_heartbeat_period && a->max_srtt_multiple == b->max_srtt_multiple &&
           a->idle_heartbeat_retry_multiple == b->idle_heartbeat_retry_multiple && a->retry == b->retry &&
           a->backoff_shift == b->backoff_shift;
}

static void test_defaults_are_the_compiled_settings(void) {
    struct tt_ReliableOptions def = defaults();
    EXPECT_EQ_U64(tt_RELIABLE_DEADLINE, def.deadline);
    EXPECT_EQ_U64(tt_RELIABLE_RETRY_INTERVAL, def.retry_interval);
    EXPECT_EQ_U64(tt_RELIABLE_RETRY_INITIAL, def.retry_initial);
    EXPECT_EQ_U64(tt_RELIABLE_RETRY_GRANULARITY, def.retry_granularity);
    EXPECT_EQ_U64(tt_TIMER_LATENESS_INITIAL, def.timer_lateness_initial);
    EXPECT_EQ_U64(tt_RELIABLE_STUCK_WARN_INTERVAL, def.stuck_warn_interval);
    EXPECT_EQ_U64(tt_IDLE_HEARTBEAT_PERIOD, def.idle_heartbeat_period);
    EXPECT_EQ_U32(tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE, def.max_srtt_multiple);
    EXPECT_EQ_U32(tt_IDLE_HEARTBEAT_RETRY_MULTIPLE, def.idle_heartbeat_retry_multiple);
    EXPECT_EQ_U32(tt_RELIABLE_RETRY, def.retry);
    EXPECT_EQ_U32(tt_RELIABLE_RETRY_BACKOFF_SHIFT, def.backoff_shift);
    // The group as the code with no context reads it.
    EXPECT_TRUE(same_reliable(&def, &reliable_defaults));

    // tt_Context_create(), tt_Context_create_with(NULL) and with the defaults give the context these values.
    for (int arm = 0; arm < 3; arm++) {
        reset_for_creation();
        struct tt_Context ctx;
        memset(&ctx, 0, sizeof(ctx));
        struct tt_InitOptions opts;
        tt_InitOptions_default(&opts);
        tt_ret_t ret = arm == 0 ? tt_Context_create(&ctx) : tt_Context_create_with(&ctx, arm == 1 ? NULL : &opts);
        EXPECT_EQ_INT(tt_RET_OK, ret);
        if (ret == tt_RET_OK) {
            EXPECT_TRUE(same_reliable(&def, &ctx.opts.reliable));
            EXPECT_EQ_U64(tt_RELIABLE_RETRY_INTERVAL, tt_reliable_retry_interval_configured(&ctx));
            EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
        }
    }
    EXPECT_EQ_U64(0, tt_reliable_retry_interval_configured(NULL));
}

// ---- Each field changes behaviour.

// retry_interval: a fixed interval replaces the dynamic one - the first retry is armed that far away, and every
// later one too. deadline: the same, and over retry_interval when both are set.
static void test_a_fixed_interval_and_the_deadline_set_the_retry_timer(void) {
    struct tt_ReliableOptions reliable = {0};
    struct tt_WriterProxy* proxy = reader_with_a_gap(&reliable);
    uint64_t first_default = scheduled_retry_at(proxy) - test_mock_now;
    EXPECT_EQ_U64(tt_RELIABLE_RETRY_INITIAL, first_default); // dynamic, no sample yet: the starting value

    reliable.retry_interval = 3 * MS;
    proxy = reader_with_a_gap(&reliable);
    EXPECT_EQ_U64(3 * MS, scheduled_retry_at(proxy) - test_mock_now);
    uint64_t waits[2];
    silent_retries(proxy, waits, 2);
    EXPECT_EQ_U64(3 * MS, waits[0]);
    EXPECT_EQ_U64(3 * MS, tt_reliable_retry_interval_configured(&node));

    reliable.deadline = 7 * MS; // over retry_interval
    proxy = reader_with_a_gap(&reliable);
    EXPECT_EQ_U64(7 * MS, scheduled_retry_at(proxy) - test_mock_now);
    EXPECT_EQ_U64(7 * MS, tt_reliable_retry_interval_configured(&node));
    reliable.retry_interval = 0;
    proxy = reader_with_a_gap(&reliable);
    EXPECT_EQ_U64(7 * MS, scheduled_retry_at(proxy) - test_mock_now);
}

// retry_initial: the dynamic interval before the proxy has a sample, the repair window before a transit sample, and
// the Publisher's interval in dynamic mode.
static void test_the_initial_interval(void) {
    struct tt_ReliableOptions reliable = {.retry_initial = 2500 * US};
    struct tt_WriterProxy* proxy = reader_with_a_gap(&reliable);
    EXPECT_EQ_U64(2500 * US, scheduled_retry_at(proxy) - test_mock_now);
    EXPECT_EQ_U64(2500 * US, repair_in_flight_ns(&node, proxy));
    EXPECT_EQ_U64(2500 * US, reliable_retry_interval_publisher(&node));
    struct tt_ReliableOptions none = {0};
    proxy = reader_with_a_gap(&none);
    EXPECT_EQ_U64(tt_RELIABLE_RETRY_INITIAL, repair_in_flight_ns(&node, proxy));
    EXPECT_EQ_U64(tt_RELIABLE_RETRY_INITIAL, reliable_retry_interval_publisher(&node));
}

// The dynamic interval with a recovery sample: srtt + max(G, 4 rttvar), at most max_srtt_multiple * max(srtt, G).
// G is the measured lateness, timer_lateness_initial before its first sample, or a fixed retry_granularity.
static uint64_t interval_with(const struct tt_ReliableOptions* reliable, uint32_t srtt, uint32_t rttvar) {
    struct tt_WriterProxy* proxy = reader_with_a_gap(reliable);
    if (proxy == NULL) {
        return 0;
    }
    proxy->recovery_srtt_ns = srtt;
    proxy->recovery_rttvar_ns = rttvar;
    return reliable_retry_interval(&node, proxy);
}

static void test_granularity_lateness_and_ceiling(void) {
    struct tt_ReliableOptions reliable = {0};
    const uint64_t cold_g = tt_TIMER_LATENESS_INITIAL;
    // G binds: rttvar 0.
    EXPECT_EQ_U64((1 * MS) + cold_g, interval_with(&reliable, 1 * MS, 0));
    reliable.timer_lateness_initial = 250 * US;
    EXPECT_EQ_U64((1 * MS) + (250 * US), interval_with(&reliable, 1 * MS, 0));
    EXPECT_EQ_U64(250 * US, timer_lateness_ns(&node));
    // A fixed G wins over the lateness, measured or not.
    reliable.retry_granularity = 300 * US;
    EXPECT_EQ_U64((1 * MS) + (300 * US), interval_with(&reliable, 1 * MS, 0));
    __atomic_store_n(&node.timer_lateness_ns, (uint32_t)(40 * US), __ATOMIC_RELAXED);
    EXPECT_EQ_U64(300 * US, reliable_retry_granularity(&node));

    // The ceiling binds: rttvar far above srtt.
    struct tt_ReliableOptions ceiling = {0};
    const uint64_t srtt = 200 * US;
    EXPECT_EQ_U64((uint64_t)tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE * srtt, interval_with(&ceiling, srtt, 50 * MS));
    ceiling.max_srtt_multiple = 8;
    EXPECT_EQ_U64(8 * srtt, interval_with(&ceiling, srtt, 50 * MS));
}

// retry and backoff_shift on the reader: the first `retry` retries of a silent writer's gap wait the plain interval,
// then each doubles up to 2^backoff_shift times it. Default: 3 plain, then 2, 4, 8, 16, 16.
static void test_the_reader_backs_off_after_retry_up_to_backoff_shift(void) {
    const uint64_t interval = 2 * MS; // fixed, so the waits are its multiples
    uint64_t waits[10];

    struct tt_ReliableOptions reliable = {.retry_interval = interval};
    silent_retries(reader_with_a_gap(&reliable), waits, 10);
    static const uint64_t by_default[10] = {1, 1, 1, 2, 4, 8, 16, 16, 16, 16};
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ_U64(by_default[i] * interval, waits[i]);
    }

    reliable.retry = 5;
    silent_retries(reader_with_a_gap(&reliable), waits, 10);
    static const uint64_t five_plain[10] = {1, 1, 1, 1, 1, 2, 4, 8, 16, 16};
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ_U64(five_plain[i] * interval, waits[i]);
    }

    reliable.retry = 0;
    reliable.backoff_shift = 2;
    silent_retries(reader_with_a_gap(&reliable), waits, 10);
    static const uint64_t capped_at_4[10] = {1, 1, 1, 2, 4, 4, 4, 4, 4, 4};
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ_U64(capped_at_4[i] * interval, waits[i]);
    }

    reliable.backoff_shift = tt_RELIABLE_NO_BACKOFF;
    silent_retries(reader_with_a_gap(&reliable), waits, 10);
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ_U64(interval, waits[i]);
    }
}

// The writer's cache, in static storage: the test reads its entry after resends_at_one_instant() returns.
#define CACHE_SAMPLES 4
static struct tt_ReliableCacheIndex cache_index[CACHE_SAMPLES];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_SAMPLES, tt_RELIABLE_RECORD_BYTES(64))];
static struct tt_ReliableCache cache;

static void init_cache(void) {
    memset(cache_index, 0, sizeof(cache_index));
    memset(cache_arena, 0, sizeof(cache_arena));
    memset(&cache, 0, sizeof(cache));
    cache.index = cache_index;
    cache.capacity = CACHE_SAMPLES;
    cache.depth = CACHE_SAMPLES;
    cache.arena = cache_arena;
    cache.arena_size = (uint32_t)sizeof(cache_arena);
}

// retry on the writer: the first `retry` resends of a datagram answer every request at once; past them one goes at
// most once per retry interval. Requests here come all at one instant, so only the unsuppressed ones are answered.
static uint32_t resends_at_one_instant(const struct tt_ReliableOptions* reliable, int requests) {
    init_node(reliable);
    memset(&pub, 0, sizeof(pub));
    pub.endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub.endpoint.id = ENDPOINT_ID;
    pub.endpoint.name = "reliable_options";
    pub.node = &node;
    pub.topic = &topic;
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;
    init_cache();
    pub.reliable_cache = &cache;
    pub.reliable = true;
    uint32_t value = 42;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value)); // seq_no 1

    struct tt_Header header;
    init_header(&header);
    for (int i = 0; i < requests; i++) {
        struct tt_AckNackHeader* acknack = (struct tt_AckNackHeader*)node.rx_buffer;
        memset(acknack, 0, sizeof(*acknack) + sizeof(uint64_t));
        acknack->endpoint_id = ENDPOINT_ID;
        acknack->sender_entity_id = 0x22220001;
        acknack->seq_no = 1;
        acknack->bitmap_words = 1;
        acknack->bitmap[0] = 1ULL; // seq_no 1 again
        uint32_t tail = sizeof(struct tt_AckNackHeader) + sizeof(uint64_t);
        EXPECT_TRUE(process_acknack(&node, &header, node.rx_buffer, 0, tail, SENDER_IP, SENDER_PORT));
    }
    return pub.retransmitted;
}

static void test_the_writer_suppresses_resends_past_retry(void) {
    struct tt_ReliableOptions reliable = {0};
    EXPECT_EQ_U32(tt_RELIABLE_RETRY, resends_at_one_instant(&reliable, 10));
    reliable.retry = 7;
    EXPECT_EQ_U32(7, resends_at_one_instant(&reliable, 10));
    // Past them the suppression lasts the Publisher's interval: retry_initial in dynamic mode.
    reliable.retry = 1;
    reliable.retry_initial = 50 * MS;
    EXPECT_EQ_U32(1, resends_at_one_instant(&reliable, 3));
    test_mock_now += 30 * MS; // inside 50 ms: still suppressed
    const struct tt_ReliableCacheIndex* entry = reliable_cache_slot(pub.reliable_cache, pub.reliable_cache->depth, 1);
    EXPECT_TRUE(resend_suppressed(&node, entry, false));
    node.opts.reliable.retry_initial = 20 * MS; // the same entry against a 20 ms interval: due
    EXPECT_TRUE(!resend_suppressed(&node, entry, false));
}

// stuck_warn_interval: a gap open past its plain retries is logged at most once per interval.
static char* log_text;
static size_t log_size;
static FILE* log_file;

static void capture_log(void) {
    log_text = NULL;
    log_size = 0;
    log_file = open_memstream(&log_text, &log_size);
    tt_current_log_level = TT_LOG_WARNING;
    tt_log_set_output(log_file);
}

static int stuck_warnings(void) {
    (void)fclose(log_file);
    tt_log_set_output(stderr);
    tt_current_log_level = TT_LOG_INFO;
    int count = 0;
    for (const char* at = log_text != NULL ? strstr(log_text, "Still waiting on reliable seq_no") : NULL; at != NULL;
         at = strstr(at + 1, "Still waiting on reliable seq_no")) {
        count++;
    }
    free(log_text);
    return count;
}

static int warnings_over(const struct tt_ReliableOptions* reliable, uint64_t span) {
    struct tt_WriterProxy* proxy = reader_with_a_gap(reliable);
    capture_log();
    uint64_t start = test_mock_now;
    uint64_t wait = 0;
    while (proxy != NULL && test_mock_now - start < span) {
        silent_retries(proxy, &wait, 1);
    }
    return stuck_warnings();
}

static void test_the_stuck_warning_interval(void) {
    struct tt_ReliableOptions reliable = {.retry_interval = 10 * MS, .backoff_shift = tt_RELIABLE_NO_BACKOFF};
    // 12 s of retries every 10 ms: the first warning at once (stuck_warned_ns starts at 0), then one per interval.
    EXPECT_EQ_INT(3, warnings_over(&reliable, 12 * tt_SECOND)); // 5 s: at 0, 5 and 10 s
    reliable.stuck_warn_interval = 2 * tt_SECOND;
    EXPECT_EQ_INT(6, warnings_over(&reliable, 11 * tt_SECOND)); // 0, 2, 4, 6, 8, 10 s
}

// idle_heartbeat_period: a RELIABLE Publisher created with no period of its own takes the context's; off is
// tt_QOS_DURATION_INFINITE; the Publisher's own option still wins. idle_heartbeat_retry_multiple: the floor.
static uint64_t created_idle_period(const struct tt_ReliableOptions* reliable, uint64_t own) {
    init_node(reliable);
    static uint64_t storage[(size_t)16U * 1024U / sizeof(uint64_t)];
    const struct tt_QosProfile qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 8},
                                      .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    const struct tt_PublisherOptions options = {.idle_heartbeat_ns = own};
    size_t size = tt_Publisher_storage_size(&topic, &qos);
    EXPECT_TRUE(size <= sizeof(storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node), &pub, &topic, "idle", &qos,
                                                      &options, storage, size));
    uint64_t period = pub.idle_heartbeat_ns;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_destroy(&pub));
    return period;
}

static void test_the_idle_heartbeat_period_and_its_floor(void) {
    struct tt_ReliableOptions reliable = {0};
    EXPECT_EQ_U64(tt_IDLE_HEARTBEAT_PERIOD, created_idle_period(&reliable, 0));
    reliable.idle_heartbeat_period = 600 * MS;
    EXPECT_EQ_U64(600 * MS, created_idle_period(&reliable, 0));
    EXPECT_EQ_U64(70 * MS, created_idle_period(&reliable, 70 * MS)); // the Publisher's own wins
    reliable.idle_heartbeat_period = tt_QOS_DURATION_INFINITE;
    EXPECT_EQ_U64(0, created_idle_period(&reliable, 0)); // off

    struct tt_ReliableOptions floor = {0};
    init_node(&floor);
    memset(&pub, 0, sizeof(pub));
    pub.node = &node;
    pub.idle_heartbeat_ns = 1 * MS;
    EXPECT_EQ_U64((uint64_t)tt_IDLE_HEARTBEAT_RETRY_MULTIPLE * tt_RELIABLE_RETRY_INITIAL, idle_heartbeat_period(&pub));
    floor.idle_heartbeat_retry_multiple = 10;
    init_node(&floor);
    EXPECT_EQ_U64(10 * tt_RELIABLE_RETRY_INITIAL, idle_heartbeat_period(&pub));
}

// ---- Each check: a bad value refused, the boundary beside it accepted - through creation.

static void expect_created(const struct tt_ReliableOptions* reliable, tt_ret_t expected, const char* what) {
    reset_for_creation();
    struct tt_Context ctx;
    memset(&ctx, 0, sizeof(ctx));
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.reliable = *reliable;
    tt_ret_t ret = tt_Context_create_with(&ctx, &opts);
    if (ret != expected) {
        printf("  %s: created %d, expected %d\n", what, (int)ret, (int)expected);
    }
    EXPECT_EQ_INT(expected, ret);
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
    }
}

static void test_every_check_refuses_and_accepts_its_boundary(void) {
    const uint64_t wait_max = UINT64_MAX >> tt_RELIABLE_RETRY_BACKOFF_SHIFT; // at the default shift
    struct tt_ReliableOptions r;

    r = (struct tt_ReliableOptions) {.retry = UINT8_MAX};
    expect_created(&r, tt_RET_INVALID_ARGUMENT, "retry UINT8_MAX");
    r.retry = UINT8_MAX - 1;
    expect_created(&r, tt_RET_OK, "retry UINT8_MAX - 1");

    r = (struct tt_ReliableOptions) {.backoff_shift = 17};
    expect_created(&r, tt_RET_INVALID_ARGUMENT, "backoff_shift 17");
    r.backoff_shift = 16;
    expect_created(&r, tt_RET_OK, "backoff_shift 16");
    r.backoff_shift = tt_RELIABLE_NO_BACKOFF;
    expect_created(&r, tt_RET_OK, "backoff_shift none");

    r = (struct tt_ReliableOptions) {.deadline = wait_max + 1};
    expect_created(&r, tt_RET_INVALID_ARGUMENT, "deadline overflowing");
    r.deadline = wait_max;
    expect_created(&r, tt_RET_OK, "deadline at the limit");
    r = (struct tt_ReliableOptions) {.deadline = UINT64_MAX, .backoff_shift = tt_RELIABLE_NO_BACKOFF};
    expect_created(&r, tt_RET_OK, "deadline UINT64_MAX with no back-off");

    r = (struct tt_ReliableOptions) {.retry_interval = wait_max + 1};
    expect_created(&r, tt_RET_INVALID_ARGUMENT, "retry_interval overflowing");
    r.retry_interval = wait_max;
    expect_created(&r, tt_RET_OK, "retry_interval at the limit");

    r = (struct tt_ReliableOptions) {.retry_initial = wait_max + 1};
    expect_created(&r, tt_RET_INVALID_ARGUMENT, "retry_initial overflowing");
    r.retry_initial = wait_max;
    expect_created(&r, tt_RET_OK, "retry_initial at the limit");

    r = (struct tt_ReliableOptions) {.retry_granularity = (uint64_t)UINT32_MAX + 1U};
    expect_created(&r, tt_RET_INVALID_ARGUMENT, "retry_granularity above 32 bits");
    r.retry_granularity = UINT32_MAX;
    expect_created(&r, tt_RET_OK, "retry_granularity UINT32_MAX");

    r = (struct tt_ReliableOptions) {.timer_lateness_initial = (uint64_t)UINT32_MAX + 1U};
    expect_created(&r, tt_RET_INVALID_ARGUMENT, "timer_lateness_initial above 32 bits");
    r.timer_lateness_initial = UINT32_MAX;
    expect_created(&r, tt_RET_OK, "timer_lateness_initial UINT32_MAX");
}

// ---- Two contexts, two retry intervals.

static void test_two_contexts_keep_their_own(void) {
    reset_for_creation();
    struct tt_Context a;
    struct tt_Context b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    struct tt_InitOptions opts_a;
    struct tt_InitOptions opts_b;
    tt_InitOptions_default(&opts_a);
    tt_InitOptions_default(&opts_b);
    opts_a.reliable.retry_interval = 2 * MS;
    opts_b.reliable.deadline = 9 * MS;
    opts_b.net.port = 9200; // the mock does not bind, but keep them apart anyway
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&a, &opts_a));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&b, &opts_b));
    EXPECT_EQ_U64(2 * MS, tt_reliable_retry_interval_configured(&a));
    EXPECT_EQ_U64(9 * MS, tt_reliable_retry_interval_configured(&b));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&a));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&b));
}

int main(void) {
    test_defaults_are_the_compiled_settings();
    test_a_fixed_interval_and_the_deadline_set_the_retry_timer();
    test_the_initial_interval();
    test_granularity_lateness_and_ceiling();
    test_the_reader_backs_off_after_retry_up_to_backoff_shift();
    test_the_writer_suppresses_resends_past_retry();
    test_the_stuck_warning_interval();
    test_the_idle_heartbeat_period_and_its_floor();
    test_every_check_refuses_and_accepts_its_boundary();
    test_two_contexts_keep_their_own();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_reliable: all tests passed\n");
    return 0;
}
