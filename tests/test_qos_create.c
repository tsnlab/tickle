/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The creation API that takes a QoS profile (tt_Node_create_publisher() and its siblings, QoS redesign step 1).
//
// What must hold, decided before the code: the profile lands in the fields the hot path already reads (reliable,
// durable, keep_all, the durations, the cache, the reorder buffer) and in nothing else it reads; an endpoint that
// needs storage is refused without it, never made BEST_EFFORT (D7); a Publisher whose history.bytes cannot hold
// `depth` of its largest samples says so and reports the depth it keeps (D10); get_actual_qos reports what the fields
// hold, and resolving it again gives it back; and storage carved by the creation works: a RELIABLE stream made with
// the default profile over a lossy link arrives whole and in order. The control for that last one is the same stream
// from a BEST_EFFORT pair over the same link, which must lose samples - or the link lost nothing and the RELIABLE run
// showed nothing.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define RECEIVER_IP 0x0a000002
#define PORT 8282
#define TOPIC "qos_topic"
#define SAMPLE_BYTES 64
#define GARBAGE 0xA5

static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return SAMPLE_BYTES;
}

static uint32_t next_index;

static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0, len);
    memcpy(payload, &next_index, sizeof(next_index));
    return SAMPLE_BYTES;
}

static uint32_t decoded_index;

static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    if (len < sizeof(decoded_index)) {
        return -1;
    }
    memcpy(&decoded_index, payload, sizeof(decoded_index));
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

#define MAX_DELIVERED 512
static uint32_t delivered[MAX_DELIVERED];
static int delivered_count;

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
    if (delivered_count < MAX_DELIVERED) {
        delivered[delivered_count] = decoded_index;
    }
    delivered_count++;
}

static int32_t request_size(struct tt_Request* request) {
    (void)request;
    return 4;
}

static int32_t request_encode(struct tt_Request* request, uint8_t* payload, uint32_t len) {
    (void)request;
    (void)payload;
    (void)len;
    return 4;
}

static int32_t response_decode(struct tt_Response* response, const uint8_t* payload, uint32_t len, bool native) {
    (void)response;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static int32_t request_decode(struct tt_Request* request, const uint8_t* payload, uint32_t len, bool native) {
    (void)request;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static int32_t response_size(struct tt_Response* response) {
    (void)response;
    return 4;
}

static int32_t response_encode(struct tt_Response* response, uint8_t* payload, uint32_t len) {
    (void)response;
    (void)payload;
    (void)len;
    return 4;
}

static void request_free(struct tt_Request* request) {
    (void)request;
}

static void response_free(struct tt_Response* response) {
    (void)response;
}

static void on_response(struct tt_Client* client, int8_t code, struct tt_Response* response) {
    (void)client;
    (void)code;
    (void)response;
}

static int8_t on_request(struct tt_Server* server, struct tt_Request* request, struct tt_Response* response,
                         tt_RequestId id) {
    (void)server;
    (void)request;
    (void)response;
    (void)id;
    return 0;
}

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_TypeSupport type;
static struct tt_Service service;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static struct tt_Client client;
static struct tt_Server server;
// Caller storage, generously sized: each test checks what the size helpers ask for against it.
static uint64_t storage[((size_t)512 * 1024) / sizeof(uint64_t)];

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

static void init(void) {
    test_mock_reset();
    init_bare_node(&sender, SENDER_ID);
    init_bare_node(&receiver, RECEIVER_ID);
    memset(&type, 0, sizeof(type));
    type.name = "qos_type";
    type.data_size = 8;
    type.data_encode_size = sample_encode_size;
    type.data_encode = sample_encode;
    type.data_decode = sample_decode;
    type.data_free = free_nothing;
    type.max_encoded_size = SAMPLE_BYTES;
    memset(&service, 0, sizeof(service));
    service.name = "qos_service";
    service.request_size = 4;
    service.response_size = 4;
    service.request_encode_size = request_size;
    service.request_encode = request_encode;
    service.request_decode = request_decode;
    service.request_free = request_free;
    service.response_encode_size = response_size;
    service.response_encode = response_encode;
    service.response_decode = response_decode;
    service.response_free = response_free;
    service.call_retry_interval = 123456; // a deprecated create uses these; tt_Node_create_client() must not
    service.call_retry_count = 7;
    memset(&pub, GARBAGE, sizeof(pub)); // a field creation forgets is visibly wrong rather than accidentally zero
    memset(&sub, GARBAGE, sizeof(sub));
    memset(storage, GARBAGE, sizeof(storage));
}

static bool inside_storage(const void* p, size_t bytes) {
    const uint8_t* begin = (const uint8_t*)storage;
    const uint8_t* q = (const uint8_t*)p;
    return q >= begin && q + bytes <= begin + sizeof(storage);
}

static void expect_resolves_to_itself(const struct tt_QosProfile* actual, uint8_t kind) {
    struct tt_QosProfile again;
    EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(actual, kind, NULL, &again));
    EXPECT_TRUE(tt_QosProfile_equal(&again, actual));
}

// D7: the default profile is RELIABLE KEEP_LAST 10, which retains samples, so it needs storage and is refused without
// it - never made BEST_EFFORT behind the caller's back.
static void test_default_publisher_needs_storage(void) {
    init();
    struct tt_Node* node = tt_Context_default_node(&sender);
    size_t needed = tt_Publisher_storage_size(&type, NULL);
    EXPECT_TRUE(needed > 0 && needed <= sizeof(storage));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Node_create_publisher(node, &pub, &type, TOPIC, NULL, NULL, NULL, 0));
    EXPECT_EQ_U32(0, sender.endpoint_count); // a refused create adds nothing
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_publisher(node, &pub, &type, TOPIC, NULL, NULL, storage, needed - 1));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_publisher(node, &pub, &type, TOPIC, NULL, NULL, (uint8_t*)storage + 4, needed));
    EXPECT_EQ_U32(0, sender.endpoint_count);

    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(node, &pub, &type, TOPIC, NULL, NULL, storage, needed));
    EXPECT_EQ_U32(1, sender.endpoint_count);
    EXPECT_TRUE(pub.reliable && !pub.durable && !pub.keep_all && !pub.liveliness_manual && !pub.batch);
    EXPECT_EQ_U32(tt_UPDATE_QOS_RELIABLE, endpoint_qos_bits((struct tt_Endpoint*)&pub)); // what the announce carries
    EXPECT_TRUE(pub.deadline_duration_ns == 0 && pub.lifespan_duration_ns == 0 &&
                pub.liveliness_lease_duration_ns == 0);
    EXPECT_TRUE(pub.reliable_cache != NULL && inside_storage(pub.reliable_cache, sizeof(*pub.reliable_cache)));
    EXPECT_TRUE(inside_storage(pub.reliable_cache->arena, pub.reliable_cache->arena_size));
    EXPECT_EQ_U32(10, pub.reliable_cache->sample_depth);
    EXPECT_EQ_U32(11U * tt_sample_cache_bytes(SAMPLE_BYTES), pub.reliable_cache->arena_size); // (depth + 1) records
    EXPECT_TRUE(pub.idle_heartbeat_ns == tt_IDLE_HEARTBEAT_PERIOD);
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        EXPECT_EQ_INT(tt_CONTEXT_ID_INVALID, pub.peer_acks[i].context_id);
    }

    struct tt_QosProfile actual;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos(&pub, &actual));
    EXPECT_EQ_INT((int)tt_QOS_HISTORY_KEEP_LAST, (int)actual.history.kind);
    EXPECT_EQ_U32(10, actual.history.depth);
    EXPECT_EQ_U32(pub.reliable_cache->arena_size, actual.history.bytes);
    EXPECT_EQ_INT((int)tt_QOS_RELIABILITY_RELIABLE, (int)actual.reliability.kind);
    EXPECT_EQ_U64(tt_QOS_MAX_BLOCKING_DEFAULT_NS, actual.reliability.max_blocking_ns);
    EXPECT_EQ_U64(tt_QOS_DURATION_INFINITE, actual.deadline.period_ns);
    EXPECT_EQ_U64(tt_QOS_DURATION_INFINITE, actual.liveliness.lease_ns);
    expect_resolves_to_itself(&actual, tt_KIND_TOPIC_PUBLISHER);
}

// BEST_EFFORT VOLATILE retains nothing: no storage, and none is asked for. A periodic HEARTBEAT has nothing to announce
// there, so asking for one is refused rather than ignored.
static void test_sensor_data_publisher_needs_none(void) {
    init();
    struct tt_Node* node = tt_Context_default_node(&sender);
    EXPECT_EQ_U32(0, (uint32_t)tt_Publisher_storage_size(&type, &tt_QOS_PROFILE_SENSOR_DATA));
    struct tt_PublisherOptions options = {.heartbeat_period_ns = 10 * tt_MILLISECOND};
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_publisher(node, &pub, &type, TOPIC, &tt_QOS_PROFILE_SENSOR_DATA, &options, NULL, 0));
    options.heartbeat_period_ns = 0;
    options.batch = true;
    options.idle_heartbeat_ns = tt_QOS_DURATION_INFINITE; // off
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_publisher(node, &pub, &type, TOPIC, &tt_QOS_PROFILE_SENSOR_DATA, &options, NULL, 0));
    EXPECT_TRUE(!pub.reliable && pub.reliable_cache == NULL && pub.batch && pub.idle_heartbeat_ns == 0);
    struct tt_QosProfile actual;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos(&pub, &actual));
    EXPECT_EQ_INT((int)tt_QOS_RELIABILITY_BEST_EFFORT, (int)actual.reliability.kind);
    EXPECT_EQ_U32(5, actual.history.depth);
    expect_resolves_to_itself(&actual, tt_KIND_TOPIC_PUBLISHER);
}

// D10: depth counts samples and history.bytes caps them. Too few bytes for `depth` of the largest sample: created,
// warned, and the actual QoS says how many are kept.
static void test_history_bytes_cap_the_depth(void) {
    init();
    struct tt_Node* node = tt_Context_default_node(&sender);
    const uint32_t record = tt_sample_cache_bytes(SAMPLE_BYTES);
    struct tt_QosProfile qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 100, .bytes = 21 * record},
                                .durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL},
                                .deadline = {.period_ns = 50 * tt_MILLISECOND},
                                .lifespan = {.duration_ns = 70 * tt_MILLISECOND}};
    size_t needed = tt_Publisher_storage_size(&type, &qos);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(node, &pub, &type, TOPIC, &qos, NULL, storage, needed));
    EXPECT_EQ_U32(21U * record, pub.reliable_cache->arena_size);
    EXPECT_EQ_U32(100, pub.reliable_cache->sample_depth); // the count bound stays depth; the bytes bind first
    EXPECT_TRUE(pub.durable && pub.deadline_duration_ns == 50 * tt_MILLISECOND);
    EXPECT_TRUE(pub.lifespan_duration_ns == 70 * tt_MILLISECOND);
    struct tt_QosProfile actual;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos(&pub, &actual));
    EXPECT_EQ_U32(20, actual.history.depth); // 21 records hold 20 samples and the wrap slack
    EXPECT_EQ_U32(21U * record, actual.history.bytes);
    EXPECT_EQ_INT((int)tt_QOS_DURABILITY_TRANSIENT_LOCAL, (int)actual.durability.kind);
    expect_resolves_to_itself(&actual, tt_KIND_TOPIC_PUBLISHER);
}

// D10 for a type with no bound (max_encoded_size 0): whether history.bytes keeps `depth` samples depends on how large
// each published one is, so creation neither warns nor reports fewer than asked - every rclcpp node warned for
// /rosout and /parameter_events - and a sample that does not fit is counted when it is published. The control is the
// same profile on the bounded type, which must still warn and report fewer: without it a silent log would only show
// that nothing was logged at all.
static long create_logged(const struct tt_QosProfile* qos) {
    struct tt_Node* node = tt_Context_default_node(&sender);
    FILE* log = tmpfile();
    EXPECT_TRUE(log != NULL);
    if (log == NULL) {
        return -1;
    }
    tt_LogLevel level = tt_current_log_level;
    tt_log_set_level(TT_LOG_WARNING);
    tt_log_set_output(log);
    size_t needed = tt_Publisher_storage_size(&type, qos);
    EXPECT_TRUE(needed > 0 && needed <= sizeof(storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(node, &pub, &type, TOPIC, qos, NULL, storage, needed));
    long written = ftell(log);
    tt_log_set_output(stderr);
    tt_log_set_level(level);
    (void)fclose(log);
    return written;
}

static void test_unbounded_type_keeps_the_depth(void) {
    // history.bytes 1: the arena is one record of the largest sample, which a 64-byte sample's depth overruns.
    const uint32_t fits = tt_sample_cache_bytes(tt_MAX_SAMPLE_LENGTH) / tt_sample_cache_bytes(SAMPLE_BYTES);
    struct tt_QosProfile qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = fits + 10U, .bytes = 1},
                                .durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL}};
    struct tt_QosProfile actual;

    init(); // control: bounded at 64 bytes, the same profile keeps fewer, says so, and reports it
    long bounded_log = create_logged(&qos);
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos(&pub, &actual));
    EXPECT_TRUE(bounded_log > 0);
    EXPECT_TRUE(actual.history.depth < qos.history.depth);

    init(); // a size its binding chose for an unbounded type (rmw_tickle's record budget): judged as unbounded
    type.max_encoded_size_estimated = true;
    long estimated_log = create_logged(&qos);
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos(&pub, &actual));
    printf("test_qos_create: max_encoded_size %u estimated: logged %ld bytes, reports %u\n", (unsigned)SAMPLE_BYTES,
           estimated_log, (unsigned)actual.history.depth);
    EXPECT_EQ_INT(0, (int)estimated_log);
    EXPECT_EQ_U32(qos.history.depth, actual.history.depth);

    init();
    type.max_encoded_size = 0;
    long unbounded_log = create_logged(&qos);
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos(&pub, &actual));
    printf("test_qos_create: depth %u, history.bytes 1: bounded type logged %ld bytes, unbounded %ld, reports %u\n",
           (unsigned)qos.history.depth, bounded_log, unbounded_log, (unsigned)actual.history.depth);
    EXPECT_EQ_INT(0, (int)unbounded_log);
    EXPECT_EQ_U32(qos.history.depth, actual.history.depth);
    EXPECT_EQ_U32(qos.history.depth, pub.reliable_cache->sample_depth);
    expect_resolves_to_itself(&actual, tt_KIND_TOPIC_PUBLISHER);

    // The shortfall is still counted where it happens: publishing `depth` 64-byte samples overruns the arena.
    EXPECT_EQ_U32(0, pub.reliable_cache->depth_shortfalls);
    for (uint32_t i = 0; i < qos.history.depth; i++) {
        next_index = i + 1U;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&next_index));
    }
    printf("test_qos_create: unbounded type, %u published: depth_shortfalls %u, %u retained\n",
           (unsigned)qos.history.depth, (unsigned)pub.reliable_cache->depth_shortfalls,
           (unsigned)pub.reliable_cache->retained_samples);
    EXPECT_TRUE(pub.reliable_cache->depth_shortfalls > 0);
}

// A KEEP_LAST cache sized for its depth keeps the depth by its ring and arena alone (publisher_assure_depth()), and
// skips the per-sample bookkeeping that would establish it: what it retains must be exactly what the bookkeeping keeps.
// The control is the same stream with the assurance taken away. A sample outside the assurance ends it for good. The
// arena starts on a 64-byte boundary whatever the storage's alignment.
static void publish_indices(uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        next_index = i + 1U;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&next_index));
    }
}

static void test_depth_assured(void) {
    const struct tt_QosProfile qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 10},
                                      .durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL}};
    uint32_t retained[2];
    uint32_t oldest[2];
    for (int arm = 0; arm < 2; arm++) { // 0: assured; 1: the control, the bookkeeping on every sample
        init();
        struct tt_Node* node = tt_Context_default_node(&sender);
        size_t needed = tt_Publisher_storage_size(&type, &qos);
        void* misaligned = (uint8_t*)storage + 8; // 8-aligned and not 64: the arena must still be
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(node, &pub, &type, TOPIC, &qos, NULL, misaligned, needed));
        struct tt_ReliableCache* cache = pub.reliable_cache;
        EXPECT_EQ_U32(0, (uint32_t)((uintptr_t)cache->arena % 64U));
        EXPECT_TRUE(cache->arena + cache->arena_size <= (uint8_t*)misaligned + needed);
        EXPECT_EQ_U32(1, cache->depth_assured_datagrams);
        EXPECT_TRUE(cache->depth_assured_len >= SAMPLE_BYTES);
        if (arm == 1) {
            cache->depth_assured_datagrams = 0;
        }
        publish_indices(25);
        EXPECT_EQ_U32(arm == 0 ? 1U : 0U, cache->depth_assured_datagrams);
        EXPECT_EQ_U32(0, cache->depth_shortfalls);
        retained[arm] = cache->retained_samples;
        oldest[arm] = cache->oldest_seq_no;
        EXPECT_EQ_U32(cache->newest_seq_no - 9U, cache->oldest_seq_no);
    }
    printf("test_qos_create: KEEP_LAST 10, 25 published: assured keeps %u from seq %u, control %u from seq %u\n",
           (unsigned)retained[0], (unsigned)oldest[0], (unsigned)retained[1], (unsigned)oldest[1]);
    EXPECT_EQ_U32(10, retained[0]);
    EXPECT_EQ_U32(retained[1], retained[0]);
    EXPECT_EQ_U32(oldest[1], oldest[0]);

    init(); // a sample longer than the assurance covers: the bookkeeping from then on
    struct tt_Node* node = tt_Context_default_node(&sender);
    size_t needed = tt_Publisher_storage_size(&type, &qos);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(node, &pub, &type, TOPIC, &qos, NULL, storage, needed));
    pub.reliable_cache->depth_assured_len = SAMPLE_BYTES - 1U;
    publish_indices(1);
    EXPECT_EQ_U32(0, pub.reliable_cache->depth_assured_datagrams);

    init(); // KEEP_ALL is never assured: it does not evict
    struct tt_QosProfile keep_all = qos;
    keep_all.history.kind = tt_QOS_HISTORY_KEEP_ALL;
    node = tt_Context_default_node(&sender);
    needed = tt_Publisher_storage_size(&type, &keep_all);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(node, &pub, &type, TOPIC, &keep_all, NULL, storage, needed));
    EXPECT_EQ_U32(0, pub.reliable_cache->depth_assured_datagrams);
}

// What qos_zero_dirty_pages() is for: clearing storage that arrives zeroed adds nothing to the resident set, where a
// memset of it makes every page resident (QoS step 2b: ~6 MB of peak RSS at p4). The memset is the control - it shows
// the instrument sees pages being made resident - and a page that holds anything is cleared.
static long resident_kb(void) {
    FILE* statm = fopen("/proc/self/statm", "r");
    long pages = -1;
    long resident = -1;
    if (statm != NULL) {
        if (fscanf(statm, "%ld %ld", &pages, &resident) != 2) {
            resident = -1;
        }
        (void)fclose(statm);
    }
    return resident < 0 ? -1 : resident * (sysconf(_SC_PAGESIZE) / 1024);
}

static void test_zeroed_storage_stays_untouched(void) {
    const size_t bytes = (size_t)8 * 1024 * 1024;
    uint8_t* fresh = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t* control = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_TRUE(fresh != MAP_FAILED && control != MAP_FAILED);
    if (fresh == MAP_FAILED || control == MAP_FAILED) {
        return;
    }
    (void)madvise(fresh, bytes, MADV_NOHUGEPAGE); // a huge page would make one written byte 2 MB resident
    (void)madvise(control, bytes, MADV_NOHUGEPAGE);
    long before = resident_kb();
    qos_zero_dirty_pages(fresh, bytes);
    long after_clear = resident_kb();
    memset(control, 0, bytes);
    long after_memset = resident_kb();
    printf("test_qos_create: 8 MB of fresh storage: cleared +%ld KB resident, memset (control) +%ld KB\n",
           after_clear - before, after_memset - after_clear);
    EXPECT_TRUE(before > 0);
    EXPECT_TRUE(after_clear - before < 512);
    EXPECT_TRUE(after_memset - after_clear > 7L * 1024L);

    fresh[(5 * 4096) + 17] = 0x5A; // a dirty page, and a dirty last byte: both cleared, the rest still untouched
    fresh[bytes - 1] = 0x01;
    qos_zero_dirty_pages(fresh, bytes);
    EXPECT_TRUE(qos_bytes_zero(fresh, bytes));
    (void)munmap(fresh, bytes);
    (void)munmap(control, bytes);
}

// The default Subscriber is RELIABLE with a reorder buffer, which lives in storage; KEEP_LAST tells the segment drain
// its depth. tt_QOS_REORDER_NONE needs nothing, and reports itself.
static void test_subscriber_storage(void) {
    init();
    struct tt_Node* node = tt_Context_default_node(&receiver);
    struct tt_SubscriberOptions options = {.callback = on_data};
    size_t needed = tt_Subscriber_storage_size(&type, NULL);
    EXPECT_EQ_U32((uint32_t)(tt_RELIABLE_BITMAP_BITS * tt_REORDER_SLOT_SIZE(SAMPLE_BYTES)), (uint32_t)needed);
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_subscriber(node, &sub, &type, TOPIC, NULL, &options, NULL, 0));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_subscriber(node, &sub, &type, TOPIC, NULL, NULL, storage, needed));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(node, &sub, &type, TOPIC, NULL, &options, storage, needed));
    EXPECT_TRUE(sub.reliable && !sub.durable && sub.callback == on_data && sub.accept_callback == NULL);
    EXPECT_TRUE(sub.tracking_bitmaps == NULL); // the builtin window
    EXPECT_TRUE(inside_storage(sub.reorder_storage, needed));
    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_BITS, sub.reorder_slots);
    EXPECT_EQ_U32((uint32_t)tt_REORDER_SLOT_SIZE(SAMPLE_BYTES), sub.reorder_slot_bytes);
    EXPECT_EQ_U32(10, sub.keep_last_depth);
    EXPECT_EQ_U32(0, sub.reorder_held);
    // The storage was all GARBAGE (init()), so every slot header said "held": each is free now. A free slot's
    // payload is never read before it is written, and creation leaves it as it was - clearing it cost the rig
    // benches ~4.6 M instructions a process at 4,096 slots (qos_free_reorder_slots()).
    bool all_free = true;
    for (uint16_t i = 0; i < sub.reorder_slots; i++) {
        all_free = all_free && !reorder_slot_at(&sub, i)->occupied;
    }
    EXPECT_TRUE(all_free);
    EXPECT_EQ_U32(GARBAGE, reorder_slot_payload(reorder_slot_at(&sub, sub.reorder_slots - 1U))[0]);
    struct tt_QosProfile actual;
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_get_actual_qos(&sub, &actual));
    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_WORDS, actual.reliability.tracking_words);
    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_BITS, actual.reliability.reorder_slots);
    expect_resolves_to_itself(&actual, tt_KIND_TOPIC_SUBSCRIBER);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub));

    // A wider window and no reorder buffer: only the window is in storage.
    struct tt_QosProfile qos = {
        .history = {.kind = tt_QOS_HISTORY_KEEP_ALL},
        .reliability = {.tracking_words = 2 * tt_RELIABLE_BITMAP_WORDS, .reorder_slots = tt_QOS_REORDER_NONE}};
    needed = tt_Subscriber_storage_size(&type, &qos);
    EXPECT_EQ_U32((uint32_t)((size_t)tt_MAX_PEER_COUNT * 2U * tt_RELIABLE_BITMAP_WORDS * sizeof(uint64_t)),
                  (uint32_t)needed);
    memset(&sub, GARBAGE, sizeof(sub));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(node, &sub, &type, TOPIC, &qos, &options, storage, needed));
    EXPECT_TRUE(sub.tracking_bitmaps == storage && sub.tracking_words == 2 * tt_RELIABLE_BITMAP_WORDS);
    EXPECT_TRUE(sub.reorder_storage == NULL && sub.reorder_slots == 0);
    EXPECT_EQ_U32(0, sub.keep_last_depth); // KEEP_ALL: the drain skips nothing
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_get_actual_qos(&sub, &actual));
    EXPECT_EQ_U32(tt_QOS_REORDER_NONE, actual.reliability.reorder_slots);
    EXPECT_EQ_INT((int)tt_QOS_HISTORY_KEEP_ALL, (int)actual.history.kind);
    expect_resolves_to_itself(&actual, tt_KIND_TOPIC_SUBSCRIBER);
}

// BEST_AVAILABLE is decided from the discovery table at creation: a Subscriber is RELIABLE only if every alive matched
// Publisher on its topic offers it. The control is the same table with the BEST_EFFORT writer on another topic.
static struct tt_Discovery discovery;

static void discovered_publisher(int slot, const char* topic, uint8_t qos, uint64_t deadline_ns) {
    struct tt_DiscoveredEntity* entity = &discovery.entities[slot];
    entity->context_id = (uint8_t)(10 + slot);
    entity->kind = tt_KIND_TOPIC_PUBLISHER;
    entity->alive = true;
    entity->qos = qos;
    entity->deadline_duration_ns = deadline_ns;
    (void)snprintf(entity->name, sizeof(entity->name), "%s", topic);
}

static void test_best_available_reads_discovery(void) {
    init();
    memset(&discovery, 0, sizeof(discovery));
    receiver.discovery = &discovery;
    struct tt_Node* node = tt_Context_default_node(&receiver);
    struct tt_SubscriberOptions options = {.callback = on_data};
    struct tt_QosProfile qos = {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_AVAILABLE},
                                .deadline = {.period_ns = tt_QOS_DURATION_BEST_AVAILABLE}};
    size_t needed = tt_Subscriber_storage_size(&type, &qos); // sized for the strict choice
    EXPECT_TRUE(needed > 0);

    discovered_publisher(0, TOPIC, tt_UPDATE_QOS_RELIABLE, 20 * tt_MILLISECOND);
    discovered_publisher(1, "elsewhere", 0, 0); // control: a BEST_EFFORT writer on another topic changes nothing
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(node, &sub, &type, TOPIC, &qos, &options, storage, needed));
    EXPECT_TRUE(sub.reliable);
    EXPECT_TRUE(sub.deadline_duration_ns == 20 * tt_MILLISECOND); // the loosest writer's period
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub));

    discovered_publisher(2, TOPIC, 0, 40 * tt_MILLISECOND); // a BEST_EFFORT writer on this topic
    memset(&sub, GARBAGE, sizeof(sub));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(node, &sub, &type, TOPIC, &qos, &options, storage, needed));
    EXPECT_TRUE(!sub.reliable && sub.reorder_storage == NULL);
    EXPECT_TRUE(sub.deadline_duration_ns == 40 * tt_MILLISECOND);
    receiver.discovery = NULL;
}

// Services: the services default, the retry settings from the options and not from the shared tt_Service, caller
// storage only when a size is given, and the profiles a service cannot have refused.
static void test_client_and_server(void) {
    init();
    struct tt_Node* node = tt_Context_default_node(&sender);
    struct tt_ClientOptions copts = {.callback = on_response, .call_retry_interval = 5000000, .call_retry_count = 2};
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Node_create_client(node, &client, &service, "svc", NULL, NULL, NULL, 0));
    struct tt_QosProfile durable = {.durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL}};
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_client(node, &client, &service, "svc", &durable, &copts, NULL, 0));
    EXPECT_EQ_U32(0, (uint32_t)tt_Client_storage_size(&copts));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(node, &client, &service, "svc", NULL, &copts, NULL, 0));
    EXPECT_TRUE(client.cache_storage == NULL); // the inline cache
    EXPECT_EQ_U32(2, call_retry_count(&client));
    EXPECT_EQ_U64(5000000, compute_retry_interval(&client, 3)); // the option, every time - not the service's 123456
    struct tt_QosProfile actual;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_get_actual_qos(&client, &actual));
    struct tt_QosProfile services;
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_QosProfile_resolve(&tt_QOS_PROFILE_SERVICES_DEFAULT, tt_KIND_SERVICE_CLIENT, NULL, &services));
    EXPECT_TRUE(tt_QosProfile_equal(&actual, &services));
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_destroy(&client));

    // The deprecated create keeps reading the service's settings, which its callers may change after creation.
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_client(&sender, &client, &service, "svc", on_response));
    EXPECT_EQ_U32(7, call_retry_count(&client));
    EXPECT_EQ_U64(123456, compute_retry_interval(&client, 0));
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_get_actual_qos(&client, &actual)); // zeroed: reported as the services default
    EXPECT_TRUE(tt_QosProfile_equal(&actual, &services));
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_destroy(&client));

    copts.max_request_bytes = 100;
    size_t cneeded = tt_Client_storage_size(&copts);
    EXPECT_TRUE(cneeded > 0 && cneeded % 8 == 0);
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_client(node, &client, &service, "svc", NULL, &copts, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(node, &client, &service, "svc", NULL, &copts, storage, cneeded));
    EXPECT_TRUE(client.cache_storage == (uint8_t*)storage && client.cache_length == cneeded);
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_destroy(&client));

    struct tt_ServerOptions sopts = {.callback = on_request, .max_response_bytes = 200};
    size_t sneeded = tt_Server_storage_size(&sopts);
    EXPECT_EQ_U32((uint32_t)(tt_MAX_SERVER_CACHE_COUNT * server_entry_of(&sopts)), (uint32_t)sneeded);
    struct tt_QosProfile best_effort = {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}};
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_server(node, &server, &service, "svc", &best_effort, &sopts, storage, sneeded));
    EXPECT_TRUE(server.cache_storage == (uint8_t*)storage && server.cache_entry_length == server_entry_of(&sopts));
    EXPECT_EQ_INT(tt_RET_OK, tt_Server_get_actual_qos(&server, &actual));
    EXPECT_EQ_INT((int)tt_QOS_RELIABILITY_BEST_EFFORT, (int)actual.reliability.kind);
    EXPECT_EQ_INT(tt_RET_OK, tt_Server_destroy(&server));
}

// A deprecated create, QoS written into the fields afterwards as its callers do: the actual QoS reads the fields.
static void test_actual_qos_of_a_deprecated_create(void) {
    init();
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&sender, &pub, &type, TOPIC));
    struct tt_QosProfile actual;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos(&pub, &actual));
    EXPECT_EQ_INT((int)tt_QOS_RELIABILITY_BEST_EFFORT, (int)actual.reliability.kind);
    pub.durable = true;
    pub.deadline_duration_ns = 3 * tt_MILLISECOND;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos(&pub, &actual));
    EXPECT_EQ_INT((int)tt_QOS_DURABILITY_TRANSIENT_LOCAL, (int)actual.durability.kind);
    EXPECT_EQ_U64(3 * tt_MILLISECOND, actual.deadline.period_ns);
}

// --- a stream over a lossy simulated link, as test_durable_late_joiner.c builds it -----------------------------------

#define SIM_QUEUE 4096
#define STREAM_SAMPLES 200
struct sim_datagram {
    uint8_t from;
    uint32_t len;
    uint8_t bytes[tt_MAX_BUFFER_LENGTH];
};
static struct sim_datagram sim_queue[SIM_QUEUE];
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;
static uint32_t sim_rng;
static uint32_t sim_lost_count;

static void sim_capture(const void* buf, size_t len) {
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
}

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        sim_rng = (sim_rng * 1103515245U) + 12345U;
        if (((sim_rng >> 16) % 1000U) < 100U) { // 10% of every datagram, both ways
            sim_lost_count++;
            continue;
        }
        struct tt_Context* to = d->from == SENDER_ID ? &receiver : &sender;
        sim_acting = to->id;
        process_packet(to, d->bytes, 0, d->len, d->from == SENDER_ID ? SENDER_IP : RECEIVER_IP, PORT, tt_TRANSPORT_UDP);
    }
}

static void sim_run_due(struct tt_Context* node) {
    bool has_next = false;
    uint64_t next = 0;
    sim_acting = node->id;
    while (run_due_entry(node, test_mock_now, &has_next, &next)) {
    }
    sim_drain();
}

static bool sim_step(void) {
    uint64_t a = UINT64_MAX;
    uint64_t b = UINT64_MAX;
    if (!sched_next_time(&sender, &a)) {
        a = UINT64_MAX;
    }
    if (!sched_next_time(&receiver, &b)) {
        b = UINT64_MAX;
    }
    uint64_t next = a < b ? a : b;
    if (next == UINT64_MAX) {
        return false;
    }
    if (next > test_mock_now) {
        test_mock_now = next;
    }
    sim_run_due(&sender);
    sim_run_due(&receiver);
    return true;
}

// Returns how many samples arrived; *in_order says whether they arrived 1, 2, 3, ... without a gap.
static int run_stream(const struct tt_QosProfile* qos, bool* in_order) {
    init();
    struct tt_Node* pub_node = tt_Context_default_node(&sender);
    struct tt_Node* sub_node = tt_Context_default_node(&receiver);
    size_t pub_bytes = tt_Publisher_storage_size(&type, qos);
    size_t sub_bytes = tt_Subscriber_storage_size(&type, qos);
    EXPECT_TRUE(pub_bytes + QOS_ALIGN8(sub_bytes) <= sizeof(storage));
    uint8_t* sub_storage = (uint8_t*)storage + QOS_ALIGN8(pub_bytes);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(pub_node, &pub, &type, TOPIC, qos, NULL, storage, pub_bytes));
    struct tt_SubscriberOptions options = {.callback = on_data};
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(sub_node, &sub, &type, TOPIC, qos, &options, sub_storage, sub_bytes));
    // Matched as register_subscriber_peer_on_publisher() matches it (test_durable_late_joiner.c does the same).
    EXPECT_TRUE(claim_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id) != NULL);
    EXPECT_TRUE(upsert_peer(pub.peers, RECEIVER_ID, RECEIVER_IP, PORT));

    delivered_count = 0;
    sim_head = sim_tail = 0;
    sim_rng = 7;
    sim_lost_count = 0;
    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
    for (uint32_t i = 1; i <= STREAM_SAMPLES; i++) {
        sim_run_due(&sender);
        sim_run_due(&receiver);
        sim_acting = SENDER_ID;
        next_index = i;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&next_index));
        sim_drain();
        test_mock_now += 100 * tt_MICROSECOND;
    }
    uint64_t end = test_mock_now + (2 * tt_SECOND);
    while (test_mock_now < end && (delivered_count == 0 || delivered[delivered_count - 1] != STREAM_SAMPLES)) {
        if (!sim_step()) {
            break;
        }
    }
    test_mock_send_hook = NULL;
    *in_order = delivered_count <= MAX_DELIVERED;
    for (int i = 0; i < delivered_count && i < MAX_DELIVERED; i++) {
        *in_order = *in_order && delivered[i] == (uint32_t)i + 1U;
    }
    return delivered_count;
}

static void test_a_default_stream_survives_loss(void) {
    struct tt_QosProfile reliable = {.history = {.depth = 64}}; // the default profile, deep enough for the burst
    bool in_order = false;
    int got = run_stream(&reliable, &in_order);
    uint32_t lost = sim_lost_count;
    printf("test_qos_create: RELIABLE (default profile): %d of %d, in order %d, datagrams lost %u\n", got,
           STREAM_SAMPLES, in_order, lost);
    EXPECT_EQ_INT(STREAM_SAMPLES, got);
    EXPECT_TRUE(in_order);
    EXPECT_TRUE(lost > 0);

    struct tt_QosProfile best_effort = {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}};
    int control = run_stream(&best_effort, &in_order);
    printf("test_qos_create: control, BEST_EFFORT: %d of %d, datagrams lost %u\n", control, STREAM_SAMPLES,
           sim_lost_count);
    EXPECT_TRUE(control < STREAM_SAMPLES); // the link lost samples, so the RELIABLE run recovered them
}

int main(void) {
    test_default_publisher_needs_storage();
    test_sensor_data_publisher_needs_none();
    test_history_bytes_cap_the_depth();
    test_unbounded_type_keeps_the_depth();
    test_zeroed_storage_stays_untouched();
    test_depth_assured();
    test_subscriber_storage();
    test_best_available_reads_discovery();
    test_client_and_server();
    test_actual_qos_of_a_deprecated_create();
    test_a_default_stream_survives_loss();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_qos_create: all tests passed\n");
    return 0;
}
