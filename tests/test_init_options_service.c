/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions bundle 4 (ROADMAP "Context options"): the service options, struct tt_ServiceOptions.
//   - the defaults are the compiled tt_CALL_* / tt_SERVER_* settings, and tt_Context_create_with() keeps the resolved
//     values in the context;
//   - every field, set to a value other than its default, changes what the context's clients and servers do (no field
//     written and never read): an unanswered call's sends and how long it lasts, a first wait, a wait's ceiling, how
//     long a server keeps an answer, when a deferred request is given up. Each is compared with the same arm at the
//     defaults, which must read exactly what the build's constants say;
//   - each check that replaced a config.h static_assert refuses a bad value with tt_RET_INVALID_ARGUMENT and accepts
//     the boundary beside it (tests/mutants_init_options_service.sh removes each check and expects a failure here);
//   - a retry schedule other than the build's is created, with a warning: a peer's server reckons it from its own.
// The calls run on a whitebox context (built by hand, as tests/test_call_retry_adaptive.c builds one) holding the
// options a created context resolved, so the scheduler holds only the call's own timers.

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_service.sh passes a mutant

#define NEVER UINT64_MAX
#define US 1000ULL
#define MS 1000000ULL
#define AUTO_ID 7

static struct tt_Context ctx;  // created: what tt_Context_create_with() resolves
static struct tt_Context node; // whitebox: the calls run here, on ctx's options
static struct tt_Service service;
static struct tt_Client client;
static struct tt_Server server;
static int callbacks;
static int last_return_code;

static void reset_mock(void) {
    test_mock_reset();
    test_mock_now = 1000 * MS;
    test_mock_node_id = AUTO_ID;
    test_mock_link_resolves = true; // the default link resolves to 10.0.0.7/24, as an interface would
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
#if tt_CONTEXT_ID_CLAIM
    memset(test_mock_ids_held, 0, sizeof(test_mock_ids_held));
#endif
}

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts;
}

static int32_t stub_request_encode_size(struct tt_Request* request) {
    (void)request;
    return 4;
}

static int32_t stub_request_encode(struct tt_Request* request, uint8_t* payload, const uint32_t len) {
    (void)request;
    if (len < 4) {
        return -1;
    }
    memset(payload, 0xab, 4);
    return 4;
}

static void on_answer(struct tt_Client* answered, int8_t return_code, struct tt_Response* response) {
    (void)answered;
    (void)response;
    last_return_code = (int)return_code;
    callbacks++;
}

// Creates ctx with `opts` (which must be accepted), then builds the whitebox node, a client and a server on its
// resolved options. The service sets no retry settings of its own: the context's apply.
static void setup(const struct tt_InitOptions* opts) {
    reset_mock();
    memset(&ctx, 0, sizeof(ctx));
    tt_ret_t ret = tt_Context_create_with(&ctx, opts);
    EXPECT_EQ_INT(tt_RET_OK, ret);
    struct tt_InitOptions resolved = ctx.opts;
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
    }
    reset_mock();
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.opts = resolved;
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    memset(&service, 0, sizeof(service));
    service.name = "options_service";
    service.request_encode_size = stub_request_encode_size;
    service.request_encode = stub_request_encode;
    service.response_size = 1;
    memset(&client, 0, sizeof(client));
    client.endpoint.kind = tt_KIND_SERVICE_CLIENT;
    client.endpoint.id = 0x12345678;
    client.node = &node;
    client.service = &service;
    client.callback = on_answer;
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&client;
    memset(&server, 0, sizeof(server));
    server.node = &node;
    server.service = &service;
}

static uint64_t first_retry_after; // run_unanswered_call(): when after the call its first retry went out

// One call nobody answers, to its end: how many requests went out, and how long after the call its callback ran.
static void run_unanswered_call(int* sends, uint64_t* took) {
    struct tt_Request request;
    uint64_t start = test_mock_now;
    int sends_before = test_mock_send_call_count;
    callbacks = 0;
    first_retry_after = 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&client, &request));
    while (callbacks == 0) {
        struct tt_TCB* due = peek_scheduler(&node);
        if (due == NULL) {
            break;
        }
        struct tt_TCB fire = *due;
        pop_scheduler(&node);
        test_mock_now = fire.time;
        int sent = test_mock_send_call_count;
        fire.function(&node, fire.time, fire.param);
        if (first_retry_after == 0 && test_mock_send_call_count - sends_before == 2 &&
            sent != test_mock_send_call_count) {
            first_retry_after = test_mock_now - start;
        }
    }
    EXPECT_EQ_INT(1, callbacks);
    EXPECT_EQ_INT((int)tt_CALL_TIMEOUT, last_return_code);
    *sends = test_mock_send_call_count - sends_before;
    *took = test_mock_now - start;
}

// ---- Defaults.

static void test_defaults_are_the_compiled_settings(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32((uint32_t)tt_CALL_RETRY_INTERVAL, opts.service.call_retry_interval);
    EXPECT_EQ_U32((uint32_t)tt_CALL_RETRY_COUNT, opts.service.call_retry_count);
    EXPECT_EQ_U32(tt_CALL_RETRY_GRANULARITY != 0 ? (uint32_t)tt_CALL_RETRY_GRANULARITY : tt_CALL_RETRY_GRANULARITY_AUTO,
                  opts.service.call_retry_granularity);
    EXPECT_EQ_U32((uint32_t)tt_CALL_RETRY_MAX_SRTT_MULTIPLE, opts.service.call_retry_max_srtt_multiple);
    EXPECT_EQ_U32((uint32_t)tt_SERVER_CACHE_GAP_MULTIPLE, opts.service.server_cache_gap_multiple);
    EXPECT_EQ_U64((uint64_t)tt_CALL_DEADLINE_PER_SEND, opts.service.call_deadline_per_send);
    EXPECT_EQ_U64((uint64_t)tt_SERVER_DEFERRED_RESPONSE_TIMEOUT, opts.service.server_deferred_response_timeout);

    // A zeroed group resolves to the same at creation, and the context keeps it.
    struct tt_InitOptions zeroed = defaults();
    memset(&zeroed.service, 0, sizeof(zeroed.service));
    setup(&zeroed);
    const struct tt_ServiceOptions* kept = &node.opts.service;
    EXPECT_EQ_U32(opts.service.call_retry_interval, kept->call_retry_interval);
    EXPECT_EQ_U32(opts.service.call_retry_count, kept->call_retry_count);
    EXPECT_EQ_U32(opts.service.call_retry_granularity, kept->call_retry_granularity);
    EXPECT_EQ_U32(opts.service.call_deadline_per_send, kept->call_deadline_per_send);
    EXPECT_EQ_U32(opts.service.call_retry_max_srtt_multiple, kept->call_retry_max_srtt_multiple);
    EXPECT_EQ_U32(opts.service.server_cache_gap_multiple, kept->server_cache_gap_multiple);
    EXPECT_EQ_U64(opts.service.server_deferred_response_timeout, kept->server_deferred_response_timeout);
}

// ---- Each field changes behaviour; the same arm at the defaults reads the build's constants.

// count: an unanswered call sends count + 1 requests.
static void test_call_retry_count_is_the_sends_of_a_call(void) {
    int sends = 0;
    uint64_t took = 0;
    struct tt_InitOptions opts = defaults();
    setup(&opts);
    run_unanswered_call(&sends, &took);
    EXPECT_EQ_INT(tt_CALL_RETRY_COUNT + 1, sends);

    opts.service.call_retry_count = 5;
    setup(&opts);
    run_unanswered_call(&sends, &took);
    EXPECT_EQ_INT(6, sends);
}

// interval: the seed, the first wait before any answer, and the unit of every wait after it.
static void test_call_retry_interval_is_the_seed(void) {
    struct tt_InitOptions opts = defaults();
    setup(&opts);
    EXPECT_EQ_U64((uint64_t)tt_CALL_RETRY_INTERVAL, compute_retry_interval(&client, 0));

    int sends = 0;
    uint64_t took = 0;
    run_unanswered_call(&sends, &took);
    EXPECT_EQ_U64((uint64_t)tt_CALL_RETRY_INTERVAL, first_retry_after);

    opts.service.call_retry_interval = 2 * MS;
    setup(&opts);
    EXPECT_EQ_U64(2 * MS, compute_retry_interval(&client, 0));
    EXPECT_EQ_U64(4 * MS, compute_retry_interval(&client, 1));
    run_unanswered_call(&sends, &took);
    EXPECT_EQ_U64(2 * MS, first_retry_after);
}

// deadline per send: an auto call ends within (count + 1) x it. A seed of 200 ms doubles past a 300 ms deadline per
// send at its second wait; the call ends at 4 x 300 ms = 1.2 s, where the defaults' 250 ms ends it at 1 s.
static void test_call_deadline_per_send_ends_the_call(void) {
    int sends = 0;
    uint64_t took = 0;
    struct tt_InitOptions opts = defaults();
    opts.service.call_retry_interval = 200 * MS;
    setup(&opts);
    EXPECT_EQ_U64((uint64_t)(tt_CALL_RETRY_COUNT + 1) * tt_CALL_DEADLINE_PER_SEND, call_deadline(&client));
    run_unanswered_call(&sends, &took);
    EXPECT_EQ_U64((uint64_t)(tt_CALL_RETRY_COUNT + 1) * tt_CALL_DEADLINE_PER_SEND, took);

    opts.service.call_deadline_per_send = 300 * MS;
    setup(&opts);
    EXPECT_EQ_U64((uint64_t)(tt_CALL_RETRY_COUNT + 1) * 300 * MS, call_deadline(&client));
    run_unanswered_call(&sends, &took);
    EXPECT_EQ_U64((uint64_t)(tt_CALL_RETRY_COUNT + 1) * 300 * MS, took);
}

// max srtt multiple: a wait's ceiling. Before any answer it is the seed times it.
static void test_call_retry_max_srtt_multiple_is_the_ceiling(void) {
    struct tt_InitOptions opts = defaults();
    setup(&opts);
    EXPECT_EQ_U64((uint64_t)tt_CALL_RETRY_INTERVAL * tt_CALL_RETRY_MAX_SRTT_MULTIPLE,
                  compute_retry_interval(&client, 20));

    opts.service.call_retry_max_srtt_multiple = 2;
    setup(&opts);
    EXPECT_EQ_U64(2ULL * tt_CALL_RETRY_INTERVAL, compute_retry_interval(&client, 20));
    EXPECT_EQ_U64(2ULL * tt_CALL_RETRY_INTERVAL, compute_retry_interval(&client, 1));
}

// granularity: a call's G, the least margin over srtt of its first wait. AUTO is twice the reliable retry's G.
static void test_call_retry_granularity_is_the_first_waits_margin(void) {
    struct tt_InitOptions opts = defaults();
    setup(&opts);
    client.latency = (uint32_t)(10 * US);
    client.latency_var = 0;
    uint64_t auto_g = 2U * reliable_retry_granularity(&node);
    EXPECT_EQ_U64(tt_CALL_RETRY_GRANULARITY != 0 ? (uint64_t)tt_CALL_RETRY_GRANULARITY : auto_g,
                  call_retry_granularity(&node));
    EXPECT_EQ_U64((10 * US) + call_retry_granularity(&node), compute_retry_interval(&client, 0));

    opts.service.call_retry_granularity = (uint32_t)(3 * MS);
    setup(&opts);
    client.latency = (uint32_t)(10 * US);
    client.latency_var = 0;
    EXPECT_EQ_U64(3 * MS, call_retry_granularity(&node));
    EXPECT_EQ_U64((10 * US) + (3 * MS), compute_retry_interval(&client, 0));

    opts.service.call_retry_granularity = tt_CALL_RETRY_GRANULARITY_AUTO;
    setup(&opts);
    EXPECT_EQ_U64(2U * reliable_retry_granularity(&node), call_retry_granularity(&node));
}

// gap multiple: an answer is kept this many times the longest recent retry gap, when that is longer than the client's
// schedule.
static void test_server_cache_gap_multiple_is_the_kept_answers_life(void) {
    struct tt_InitOptions opts = defaults();
    setup(&opts);
    server.client_retry_gap = tt_SECOND;
    EXPECT_EQ_U64((uint64_t)tt_SERVER_CACHE_GAP_MULTIPLE * tt_SECOND, server_cache_lifetime(&server));

    opts.service.server_cache_gap_multiple = 2;
    setup(&opts);
    server.client_retry_gap = tt_SECOND;
    EXPECT_EQ_U64(2 * tt_SECOND, server_cache_lifetime(&server));
}

// The server's reckoning of how long its clients may retry is its own context's: on the auto path the call's deadline
// and one deadline per send more, (count + 2) x the deadline per send (the service setting none of its own).
static void test_server_window_reads_its_own_contexts_schedule(void) {
    struct tt_InitOptions opts = defaults();
    setup(&opts);
    EXPECT_EQ_U64((uint64_t)(tt_CALL_RETRY_COUNT + 2) * tt_CALL_DEADLINE_PER_SEND, server_client_window(&server));

    opts.service.call_retry_count = 6;
    opts.service.call_deadline_per_send = 100 * MS;
    setup(&opts);
    EXPECT_EQ_U64(800 * MS, server_client_window(&server));
}

// deferred timeout: a request whose callback deferred its answer is given up this long after it came.
static void test_server_deferred_response_timeout_is_the_pending_slots_life(void) {
    struct tt_InitOptions opts = defaults();
    tt_RequestId request_id;
    memset(&request_id, 0, sizeof(request_id));
    setup(&opts);
    EXPECT_TRUE(defer_call_response(&server, request_id, 0, 0x0a000002U, 40000));
    struct tt_TCB* due = peek_scheduler(&node);
    EXPECT_TRUE(due != NULL && due->function == pending_response_timeout);
    EXPECT_EQ_U64(test_mock_now + tt_SERVER_DEFERRED_RESPONSE_TIMEOUT, due != NULL ? due->time : 0);

    opts.service.server_deferred_response_timeout = 700 * MS;
    setup(&opts);
    EXPECT_TRUE(defer_call_response(&server, request_id, 0, 0x0a000002U, 40000));
    due = peek_scheduler(&node);
    EXPECT_TRUE(due != NULL && due->function == pending_response_timeout);
    EXPECT_EQ_U64(test_mock_now + (700 * MS), due != NULL ? due->time : 0);
}

// A Client's own settings still override the context's.
static void test_a_clients_own_settings_override_the_contexts(void) {
    struct tt_InitOptions opts = defaults();
    opts.service.call_retry_count = 7;
    setup(&opts);
    service.call_retry_count = 2;
    int sends = 0;
    uint64_t took = 0;
    run_unanswered_call(&sends, &took);
    EXPECT_EQ_INT(3, sends);
}

// ---- Validation: each refused, and the boundary beside it accepted.

static void expect_created(const struct tt_InitOptions* opts, tt_ret_t expected, const char* what) {
    reset_mock();
    memset(&ctx, 0, sizeof(ctx));
    tt_ret_t ret = tt_Context_create_with(&ctx, opts);
    if (ret != expected) {
        fprintf(stderr, "%s: returned %d, expected %d\n", what, (int)ret, (int)expected);
        test_failures++;
    }
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
    }
}

static void test_validation(void) {
    struct tt_InitOptions opts = defaults();
    opts.service.call_retry_interval = 10 * MS;
    opts.service.call_deadline_per_send = (10 * MS) - 1;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "service.call_deadline_per_send below the seed");
    opts.service.call_deadline_per_send = 10 * MS;
    expect_created(&opts, tt_RET_OK, "service.call_deadline_per_send = the seed");

    // A zero in the options is the default, so the next three can only be broken by a compiled default of 0
    // (-Dtt_CALL_RETRY_MAX_SRTT_MULTIPLE=0, ...): the checks read the resolved group, which is what is handed here.
    struct tt_ServiceOptions resolved = defaults().service;
    EXPECT_EQ_INT(tt_RET_OK, validate_service_options(&resolved));
    struct tt_ServiceOptions broken = resolved;
    broken.call_retry_max_srtt_multiple = 0;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, validate_service_options(&broken));
    broken.call_retry_max_srtt_multiple = 1;
    EXPECT_EQ_INT(tt_RET_OK, validate_service_options(&broken));
    broken = resolved;
    broken.server_cache_gap_multiple = 0;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, validate_service_options(&broken));
    broken.server_cache_gap_multiple = 1;
    EXPECT_EQ_INT(tt_RET_OK, validate_service_options(&broken));
    broken = resolved;
    broken.server_deferred_response_timeout = 0;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, validate_service_options(&broken));
    broken.server_deferred_response_timeout = 1;
    EXPECT_EQ_INT(tt_RET_OK, validate_service_options(&broken));
}

// ---- Interop: a schedule other than the build's is created, and said.

// Whether creating with `opts` logged the interop warning.
static bool warned(const struct tt_InitOptions* opts) {
    FILE* log = tmpfile();
    if (log == NULL) {
        test_failures++;
        return false;
    }
    FILE* before = tt_log_output;
    tt_log_set_output(log);
    expect_created(opts, tt_RET_OK, "a schedule other than the build's");
    tt_log_set_output(before);
    char text[4096] = {0};
    if (fseek(log, 0, SEEK_SET) != 0) {
        fclose(log);
        test_failures++;
        return false;
    }
    size_t got = fread(text, 1, sizeof(text) - 1, log);
    fclose(log);
    text[got] = '\0';
    return strstr(text, "retry schedule differs from the build's") != NULL;
}

static void test_a_schedule_other_than_the_builds_warns(void) {
    tt_LogLevel level = tt_current_log_level;
    tt_current_log_level = TT_LOG_WARNING;
    struct tt_InitOptions opts = defaults();
    EXPECT_TRUE(!warned(&opts)); // the control: the build's own schedule says nothing
    opts.service.call_retry_count = tt_CALL_RETRY_COUNT + 1;
    EXPECT_TRUE(warned(&opts));
    opts = defaults();
    opts.service.call_retry_interval = tt_CALL_RETRY_INTERVAL + 1;
    EXPECT_TRUE(warned(&opts));
    opts = defaults();
    opts.service.call_retry_max_srtt_multiple = tt_CALL_RETRY_MAX_SRTT_MULTIPLE + 1;
    EXPECT_TRUE(warned(&opts));
    opts = defaults();
    opts.service.call_deadline_per_send = tt_CALL_DEADLINE_PER_SEND + 1;
    EXPECT_TRUE(warned(&opts));
    opts = defaults();
    opts.service.server_cache_gap_multiple = tt_SERVER_CACHE_GAP_MULTIPLE + 1; // the server's own: no warning
    EXPECT_TRUE(!warned(&opts));
    tt_current_log_level = level;
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR; // a refusal is still said; the warning test raises it itself
    test_defaults_are_the_compiled_settings();
    test_call_retry_count_is_the_sends_of_a_call();
    test_call_retry_interval_is_the_seed();
    test_call_deadline_per_send_ends_the_call();
    test_call_retry_max_srtt_multiple_is_the_ceiling();
    test_call_retry_granularity_is_the_first_waits_margin();
    test_server_cache_gap_multiple_is_the_kept_answers_life();
    test_server_window_reads_its_own_contexts_schedule();
    test_server_deferred_response_timeout_is_the_pending_slots_life();
    test_a_clients_own_settings_override_the_contexts();
    test_validation();
    test_a_schedule_other_than_the_builds_warns();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_service: all tests passed\n");
    return 0;
}
