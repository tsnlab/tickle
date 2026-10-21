/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions, bundle 5 (ROADMAP "Context options"): the QoS defaults (struct tt_QosDefaults), through
// tt_Context_create_with() and the mock HAL.
//   - the defaults are the config.h ones, field by field;
//   - every field, set to a value other than its default, changes the QoS an endpoint created with a zero in that
//     field resolves to - its fields and what tt_*_get_actual_qos() reports - and a profile's own nonzero value still
//     wins over it (no field written and never read);
//   - the storage rule: tt_*_storage_size() keep the config.h defaults, so on a context whose default is larger they
//     size short - creation with that storage is refused, with the field and the needed size named, and writes nothing
//     past it; tt_*_storage_size_for(context, ...) sizes what creation needs; a smaller context default fits in what
//     tt_*_storage_size() gives;
//   - each check refuses a bad value and accepts the boundary beside it (tests/mutants_init_options_qos.sh removes
//     each check and expects a failure);
//   - two contexts in one process with different defaults each keep their own.
// Storage is heap-allocated at exactly the size handed over, so a build with -fsanitize=address reports any write
// past it.

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_qos.sh passes a mutant

#define AUTO_ID 7
#define TOPIC "qos_defaults"
#define SAMPLE_BYTES 64
#define CANARY 0x5A

static struct tt_Context ctx;
static struct tt_Context other;
static struct tt_TypeSupport type;
static struct tt_Service service;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static struct tt_Client client;

static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return SAMPLE_BYTES;
}

static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0, len);
    return SAMPLE_BYTES;
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

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)seq_no;
    (void)data;
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

static int32_t response_decode(struct tt_Response* response, const uint8_t* payload, uint32_t len, bool native) {
    (void)response;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static void request_free(struct tt_Request* request) {
    (void)request;
}

static void response_free(struct tt_Response* response) {
    (void)response;
}

static void on_response(struct tt_Client* c, int8_t code, struct tt_Response* response) {
    (void)c;
    (void)code;
    (void)response;
}

static void reset(void) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = AUTO_ID;
    test_mock_link_resolves = true; // the default link resolves to 10.0.0.7/24, as an interface would
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
#if tt_CONTEXT_ID_CLAIM
    memset(test_mock_ids_held, 0, sizeof(test_mock_ids_held));
#endif
    memset(&type, 0, sizeof(type));
    type.name = "qos_defaults_type";
    type.data_size = 8;
    type.data_encode_size = sample_encode_size;
    type.data_encode = sample_encode;
    type.data_decode = sample_decode;
    type.data_free = free_nothing;
    type.max_encoded_size = SAMPLE_BYTES;
    memset(&service, 0, sizeof(service));
    service.name = "qos_defaults_service";
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
    memset(&pub, 0, sizeof(pub));
    memset(&sub, 0, sizeof(sub));
    memset(&client, 0, sizeof(client));
}

static tt_ret_t create_with(struct tt_Context* node, const struct tt_InitOptions* opts) {
    memset(node, 0, sizeof(*node));
    return tt_Context_create_with(node, opts);
}

static void destroy(struct tt_Context* node) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(node));
}

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts;
}

// A context created with these QoS defaults (the rest default).
static void create_qos(struct tt_Context* node, const struct tt_QosDefaults* qos) {
    reset();
    struct tt_InitOptions opts = defaults();
    opts.qos = *qos;
    EXPECT_EQ_INT(tt_RET_OK, create_with(node, &opts));
}

// Storage of exactly `size` bytes (malloc() is 8-aligned on every platform this runs on), so a write past it is
// AddressSanitizer's to see.
static void* storage_of(size_t size) {
    void* storage = malloc(size != 0 ? size : 1U);
    EXPECT_TRUE(storage != NULL);
    return storage;
}

// A Publisher on `node` with `qos` and storage sized by tt_Publisher_storage_size_for() for it; its actual QoS.
static struct tt_QosProfile publisher_actual(struct tt_Context* node, const struct tt_QosProfile* qos, void** storage) {
    struct tt_QosProfile actual;
    memset(&actual, 0, sizeof(actual));
    size_t needed = tt_Publisher_storage_size_for(node, &type, qos);
    *storage = storage_of(needed);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(node), &pub, &type, TOPIC, qos, NULL,
                                                      *storage, needed));
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_get_actual_qos(&pub, &actual));
    return actual;
}

// ---- The defaults.

static void test_defaults_are_the_compiled_settings(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32(tt_QOS_DEPTH_DEFAULT, opts.qos.depth);
    EXPECT_EQ_U32(tt_QOS_KEEP_ALL_DEPTH_DEFAULT, opts.qos.keep_all_depth);
    EXPECT_EQ_U32(tt_QOS_HISTORY_BYTES_DEFAULT, opts.qos.history_bytes);
    EXPECT_EQ_U32(tt_QOS_KEEP_ALL_BYTES_DEFAULT, opts.qos.keep_all_bytes);
    EXPECT_EQ_U64(tt_QOS_MAX_BLOCKING_DEFAULT_NS, opts.qos.max_blocking_ns);

    // A zeroed group and NULL options resolve to them too, and storage sized by the context is what it always was.
    reset();
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, NULL));
    EXPECT_EQ_U32(tt_QOS_DEPTH_DEFAULT, ctx.opts.qos.depth);
    EXPECT_EQ_U64(tt_QOS_MAX_BLOCKING_DEFAULT_NS, ctx.opts.qos.max_blocking_ns);
    EXPECT_EQ_U64(tt_Publisher_storage_size(&type, NULL), tt_Publisher_storage_size_for(&ctx, &type, NULL));
    EXPECT_EQ_U64(tt_Publisher_storage_size(&type, NULL), tt_Publisher_storage_size_for(NULL, &type, NULL));
    EXPECT_EQ_U64(tt_Subscriber_storage_size(&type, NULL), tt_Subscriber_storage_size_for(&ctx, &type, NULL));
    EXPECT_EQ_U64(tt_Subscriber_storage_size(&type, NULL), tt_Subscriber_storage_size_for(NULL, &type, NULL));
    destroy(&ctx);
}

// ---- Each field, set, changes the resolved QoS of a zero in its profile field, and an explicit value still wins.

static void test_depth(void) {
    struct tt_QosDefaults qos = {.depth = 23};
    create_qos(&ctx, &qos);
    struct tt_QosProfile zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    void* storage = NULL;
    struct tt_QosProfile actual = publisher_actual(&ctx, &zeroed, &storage);
    EXPECT_EQ_U32(23, actual.history.depth);
    EXPECT_EQ_U32(23, pub.reliable_cache->sample_depth); // what the hot path counts by
    EXPECT_EQ_U32(24U * tt_sample_cache_bytes(SAMPLE_BYTES), pub.reliable_cache->arena_size); // (depth + 1) records
    free(storage);

    // A Subscriber's zero depth too: the segment drain's KEEP_LAST bound and its actual QoS.
    struct tt_SubscriberOptions sub_options = {.callback = on_data};
    size_t sub_needed = tt_Subscriber_storage_size_for(&ctx, &type, &zeroed);
    storage = storage_of(sub_needed);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&ctx), &sub, &type, TOPIC "_sub",
                                                       &zeroed, &sub_options, storage, sub_needed));
    EXPECT_EQ_U32(23, sub.keep_last_depth);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_get_actual_qos(&sub, &actual));
    EXPECT_EQ_U32(23, actual.history.depth);
    destroy(&ctx);
    free(storage);

    // The profile's own depth wins.
    create_qos(&ctx, &qos);
    struct tt_QosProfile explicit_depth = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 4}};
    actual = publisher_actual(&ctx, &explicit_depth, &storage);
    EXPECT_EQ_U32(4, actual.history.depth);
    destroy(&ctx);
    free(storage);
}

static void test_keep_all_depth(void) {
    struct tt_QosDefaults qos = {.keep_all_depth = 77};
    create_qos(&ctx, &qos);
    struct tt_QosProfile keep_all = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL}};
    void* storage = NULL;
    struct tt_QosProfile actual = publisher_actual(&ctx, &keep_all, &storage);
    EXPECT_EQ_U32(77, actual.history.depth);
    EXPECT_TRUE(pub.keep_all);
    EXPECT_EQ_U32(77, pub.reliable_cache->capacity); // KEEP_ALL's count bound is the ring: depth samples' datagrams
    destroy(&ctx);
    free(storage);

    // KEEP_LAST does not read it.
    create_qos(&ctx, &qos);
    actual = publisher_actual(&ctx, NULL, &storage);
    EXPECT_EQ_U32(tt_QOS_DEPTH_DEFAULT, actual.history.depth);
    destroy(&ctx);
    free(storage);
}

static void test_history_bytes(void) {
    const uint32_t record = tt_sample_cache_bytes(SAMPLE_BYTES);
    struct tt_QosDefaults qos = {.history_bytes = 5U * record};
    create_qos(&ctx, &qos);
    void* storage = NULL;
    struct tt_QosProfile actual = publisher_actual(&ctx, NULL, &storage);
    EXPECT_EQ_U32(5U * record, pub.reliable_cache->arena_size);
    EXPECT_EQ_U32(5U * record, actual.history.bytes);
    EXPECT_EQ_U32(4, actual.history.depth); // five records keep four samples and the wrap slack (D10)
    destroy(&ctx);
    free(storage);

    // The profile's own bytes win.
    create_qos(&ctx, &qos);
    struct tt_QosProfile explicit_bytes = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .bytes = 8U * record}};
    actual = publisher_actual(&ctx, &explicit_bytes, &storage);
    EXPECT_EQ_U32(8U * record, actual.history.bytes);
    destroy(&ctx);
    free(storage);

    // A Client's services profile has no bytes of its own: it reports the context's.
    create_qos(&ctx, &qos);
    struct tt_ClientOptions client_options = {.callback = on_response};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(tt_Context_default_node(&ctx), &client, &service, "svc", NULL,
                                                   &client_options, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_get_actual_qos(&client, &actual));
    EXPECT_EQ_U32(5U * record, actual.history.bytes);
    destroy(&ctx);
}

static void test_keep_all_bytes(void) {
    const uint32_t record = tt_sample_cache_bytes(SAMPLE_BYTES);
    struct tt_QosDefaults qos = {.keep_all_bytes = 7U * record};
    create_qos(&ctx, &qos);
    struct tt_QosProfile keep_all = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL}};
    void* storage = NULL;
    struct tt_QosProfile actual = publisher_actual(&ctx, &keep_all, &storage);
    EXPECT_EQ_U32(7U * record, pub.reliable_cache->arena_size);
    EXPECT_EQ_U32(7U * record, actual.history.bytes);
    destroy(&ctx);
    free(storage);

    // KEEP_LAST does not read it.
    create_qos(&ctx, &qos);
    actual = publisher_actual(&ctx, NULL, &storage);
    EXPECT_EQ_U32(11U * record, actual.history.bytes); // (10 + 1) records, under the 1 MiB default
    destroy(&ctx);
    free(storage);
}

static void test_max_blocking(void) {
    struct tt_QosDefaults qos = {.max_blocking_ns = 250 * tt_MILLISECOND};
    create_qos(&ctx, &qos);
    void* storage = NULL;
    struct tt_QosProfile actual = publisher_actual(&ctx, NULL, &storage);
    EXPECT_EQ_U64(250 * tt_MILLISECOND, actual.reliability.max_blocking_ns);
    EXPECT_EQ_U64(250 * tt_MILLISECOND, pub.qos_max_blocking_ns);
    destroy(&ctx);
    free(storage);

    create_qos(&ctx, &qos);
    struct tt_QosProfile explicit_blocking = {.reliability = {.max_blocking_ns = 3 * tt_MILLISECOND}};
    actual = publisher_actual(&ctx, &explicit_blocking, &storage);
    EXPECT_EQ_U64(3 * tt_MILLISECOND, actual.reliability.max_blocking_ns);
    destroy(&ctx);
    free(storage);
}

// ---- The storage rule.

static long create_logged(struct tt_Context* node, void* storage, size_t size, tt_ret_t expected, char* text,
                          size_t text_size) {
    FILE* log = tmpfile();
    EXPECT_TRUE(log != NULL);
    if (log == NULL) {
        return -1;
    }
    tt_LogLevel level = tt_current_log_level;
    tt_log_set_level(TT_LOG_WARNING);
    tt_log_set_output(log);
    EXPECT_EQ_INT(expected, tt_Node_create_publisher(tt_Context_default_node(node), &pub, &type, TOPIC, NULL, NULL,
                                                     storage, size));
    long written = ftell(log);
    EXPECT_EQ_INT(0, fseek(log, 0, SEEK_SET));
    size_t got = fread(text, 1, text_size - 1U, log);
    text[got] = '\0';
    tt_log_set_output(stderr);
    tt_log_set_level(level);
    (void)fclose(log);
    return written;
}

// A context default above config.h's: tt_Publisher_storage_size() sizes short, creation with it is refused - nothing
// added, nothing written past the storage, the field and the size needed named - and the _for() size is created.
static void test_larger_default_refuses_short_storage(void) {
    // depth 40 needs (40 + 1) records, more than the 10 + 1 tt_Publisher_storage_size() assumes.
    const struct tt_QosDefaults raised = {.depth = 40};
    create_qos(&ctx, &raised);
    size_t compiled = tt_Publisher_storage_size(&type, NULL);
    size_t needed = tt_Publisher_storage_size_for(&ctx, &type, NULL);
    EXPECT_TRUE(needed > compiled);

    uint8_t* storage = storage_of(compiled + 64U);
    memset(storage, CANARY, compiled + 64U);
    char text[1024];
    long written = create_logged(&ctx, storage, compiled, tt_RET_INVALID_ARGUMENT, text, sizeof(text));
    EXPECT_EQ_U32(0, ctx.endpoint_count);
    bool untouched = true;
    for (size_t i = compiled; i < compiled + 64U; i++) {
        untouched = untouched && storage[i] == CANARY;
    }
    EXPECT_TRUE(untouched);
    EXPECT_TRUE(written > 0);
    char needed_text[32];
    (void)snprintf(needed_text, sizeof(needed_text), "%lu needed", (unsigned long)needed);
    EXPECT_TRUE(strstr(text, needed_text) != NULL);
    EXPECT_TRUE(strstr(text, "tt_InitOptions.qos.depth, 40") != NULL);
    free(storage);

    // The size the context asks for: created, with the context's depth.
    storage = storage_of(needed);
    (void)create_logged(&ctx, storage, needed, tt_RET_OK, text, sizeof(text));
    EXPECT_EQ_U32(40, pub.reliable_cache->sample_depth);
    destroy(&ctx);
    free(storage);

    // The same refusal from a raised KEEP_ALL byte cap, which does grow the arena, names its field.
    struct tt_QosDefaults keep_all_raised = {.keep_all_depth = 4000, .keep_all_bytes = 4000U * 1024U};
    create_qos(&ctx, &keep_all_raised);
    struct tt_QosProfile keep_all = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL}};
    compiled = tt_Publisher_storage_size(&type, &keep_all);
    needed = tt_Publisher_storage_size_for(&ctx, &type, &keep_all);
    EXPECT_TRUE(needed > compiled);
    storage = storage_of(compiled);
    FILE* log = tmpfile();
    EXPECT_TRUE(log != NULL);
    if (log != NULL) {
        tt_log_set_output(log);
        EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Node_create_publisher(tt_Context_default_node(&ctx), &pub, &type,
                                                                        TOPIC, &keep_all, NULL, storage, compiled));
        EXPECT_EQ_INT(0, fseek(log, 0, SEEK_SET));
        size_t got = fread(text, 1, sizeof(text) - 1U, log);
        text[got] = '\0';
        tt_log_set_output(stderr);
        (void)fclose(log);
        EXPECT_TRUE(strstr(text, "tt_InitOptions.qos.keep_all_depth, 4000") != NULL);
        EXPECT_TRUE(strstr(text, "tt_InitOptions.qos.keep_all_bytes, 4096000") != NULL);
    }
    EXPECT_EQ_U32(0, ctx.endpoint_count);
    destroy(&ctx);
    free(storage);
}

// A context default below config.h's needs less: what tt_Publisher_storage_size() gives is enough.
static void test_smaller_default_fits_compiled_size(void) {
    struct tt_QosDefaults qos = {.depth = 3};
    create_qos(&ctx, &qos);
    size_t compiled = tt_Publisher_storage_size(&type, NULL);
    EXPECT_TRUE(tt_Publisher_storage_size_for(&ctx, &type, NULL) < compiled);
    void* storage = storage_of(compiled);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&ctx), &pub, &type, TOPIC, NULL, NULL,
                                                      storage, compiled));
    EXPECT_EQ_U32(3, pub.reliable_cache->sample_depth);
    destroy(&ctx);
    free(storage);
}

// ---- The checks: each refuses the value past its limit and accepts the limit.

static void expect_created(const struct tt_QosDefaults* qos, tt_ret_t expected, const char* what) {
    reset();
    struct tt_InitOptions opts = defaults();
    opts.qos = *qos;
    tt_ret_t ret = create_with(&ctx, &opts);
    if (ret != expected) {
        fprintf(stderr, "%s: returned %d, expected %d\n", what, (int)ret, (int)expected);
        test_failures++;
    }
    if (ret == tt_RET_OK) {
        destroy(&ctx);
    }
}

static void test_checks(void) {
    struct tt_QosDefaults qos = {.depth = UINT16_MAX};
    expect_created(&qos, tt_RET_OK, "qos.depth UINT16_MAX");
    qos.depth = UINT16_MAX + 1U;
    expect_created(&qos, tt_RET_INVALID_ARGUMENT, "qos.depth above UINT16_MAX");
    qos = (struct tt_QosDefaults) {.keep_all_depth = UINT16_MAX};
    expect_created(&qos, tt_RET_OK, "qos.keep_all_depth UINT16_MAX");
    qos.keep_all_depth = UINT16_MAX + 1U;
    expect_created(&qos, tt_RET_INVALID_ARGUMENT, "qos.keep_all_depth above UINT16_MAX");
    // The byte caps and max_blocking_ns take what a profile takes.
    qos = (struct tt_QosDefaults) {.history_bytes = UINT32_MAX, .keep_all_bytes = UINT32_MAX, .max_blocking_ns = 1};
    expect_created(&qos, tt_RET_OK, "byte caps UINT32_MAX, max_blocking_ns 1");
}

// ---- Two contexts, two sets of defaults.

static void test_two_contexts_keep_their_own(void) {
    struct tt_QosDefaults a = {.depth = 6};
    struct tt_QosDefaults b = {.depth = 9};
    create_qos(&ctx, &a);
    struct tt_InitOptions opts = defaults();
    opts.qos = b;
    opts.net.port = 9100;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&other, &opts));
    struct tt_QosProfile actual;
    void* storage_a = NULL;
    actual = publisher_actual(&ctx, NULL, &storage_a);
    EXPECT_EQ_U32(6, actual.history.depth);
    void* storage_b = NULL;
    actual = publisher_actual(&other, NULL, &storage_b);
    EXPECT_EQ_U32(9, actual.history.depth);
    destroy(&other);
    destroy(&ctx);
    free(storage_a);
    free(storage_b);
}

int main(void) {
    test_defaults_are_the_compiled_settings();
    test_depth();
    test_keep_all_depth();
    test_history_bytes();
    test_keep_all_bytes();
    test_max_blocking();
    test_larger_default_refuses_short_storage();
    test_smaller_default_fits_compiled_size();
    test_checks();
    test_two_contexts_keep_their_own();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_qos: all tests passed\n");
    return 0;
}
