/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Several requests in flight through rmw_tickle (the user's decision of 2026-10-16; DESIGN.md section 7, RMW.md
// "Services"): until then a client had one call and a service one waiting request, so rclcpp's async_send_request()
// twice in a row failed, and an action client waiting on get_result for two goals failed or stalled.
//
// In one process, a service and two clients of it on one node. Checked, at the build's tt_CLIENT_MAX_OUTSTANDING (N):
// - async_send_request() N times in a row: every rmw_send_request() succeeds at once with its own sequence id, the
//   service takes all N before answering any, answers them in reverse order, and each response comes back under the
//   sequence id of the request it answers;
// - call N + 1 while N are in flight and unanswered: rmw_send_request() waits RMW_TICKLE_CLIENT_SEND_BLOCK_NS, then
//   fails with RMW_RET_ERROR; once one is answered, the next call goes at once;
// - action-like get_result: two "goals" each have a result request waiting on the service (taken, not answered -
//   the goal is still running) while a second client calls the same service too; the goals finish in reverse
//   order, and every response reaches its own request;
// - a BEST_EFFORT client (D6), the second case's control the other way: call N + 1 goes at once, ending the oldest
//   call, and only that call's answer is dropped.
// Runs in the rmw suite's private netns (run_rmw_suite.sh).

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h> // nanosleep(), clock_gettime()

#include <tickle/config.h> // tt_CLIENT_MAX_OUTSTANDING, tt_CLIENT_CALL_WINDOW
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

#define N tt_CLIENT_MAX_OUTSTANDING
#define POLLS 200 // attempts, POLL_MS apart: four seconds
#define POLL_MS 20L
#define NS_PER_MS 1000000L
#define MARK 0xABCDU // the response's second field, to tell it from a request echoed back
#define NS_PER_SEC 1000000000ULL
#define ADDEND 7U           // every request's second field
#define IN_A_ROW_FIRST 100U // first fields of each case's requests, so a response names its request
#define REFUSED_FIRST 200U
#define EXTRA_FIRST 300U
#define GOALS_FIRST 400U
#define OTHER_FIRST 500U
#define BEST_EFFORT_FIRST 600U
#define WARM_RESEND_POLLS 50 // one second

struct pair {
    uint32_t first;
    uint32_t second;
};

static bool convert(const void* source, void* dest) {
    *(struct pair*)dest = *(const struct pair*)source;
    return true;
}
static int32_t encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(struct pair);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    memcpy(payload, data, sizeof(struct pair));
    return (int32_t)sizeof(struct pair);
}
static int32_t decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct pair)) {
        return -1;
    }
    memcpy(data, payload, sizeof(struct pair));
    return (int32_t)sizeof(struct pair);
}
static void free_nothing(struct tt_Data* data) {
    (void)data;
}

#define PAIR_CALLBACKS(type_name)                                                    \
    {                                                                                \
        .ros_type_name = (type_name),                                                \
        .tickle_struct_size = sizeof(struct pair),                                   \
        .ros_struct_size = sizeof(struct pair),                                      \
        .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function) & convert,     \
        .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function) & convert, \
        .tickle_encode_size = (tt_DATA_ENCODE_SIZE) & encode_size,                   \
        .tickle_encode = (tt_DATA_ENCODE) & encode,                                  \
        .tickle_decode = (tt_DATA_DECODE) & decode,                                  \
        .tickle_free = (tt_DATA_FREE) & free_nothing,                                \
        .tickle_max_encoded_size = sizeof(struct pair),                              \
        .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,                            \
    }

static rosidl_typesupport_tickle_c_message_callbacks_t request_callbacks = PAIR_CALLBACKS("multi/srv/Add_Request");
static rosidl_typesupport_tickle_c_message_callbacks_t response_callbacks = PAIR_CALLBACKS("multi/srv/Add_Response");
static rosidl_message_type_support_t request_handle = {.data = &request_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t response_handle = {.data = &response_callbacks,
                                                        .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_service_callbacks_t service_callbacks = {.ros_type_name = "multi/srv/Add"};
static rosidl_service_type_support_t service_handle = {.data = &service_callbacks,
                                                       .func = get_service_typesupport_handle_function,
                                                       .request_typesupport = &request_handle,
                                                       .response_typesupport = &response_handle};

static void sleep_poll(void) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = POLL_MS * NS_PER_MS};
    nanosleep(&interval, NULL);
}

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now); // NOLINT(misc-include-cleaner) - <time.h>
    return ((uint64_t)now.tv_sec * NS_PER_SEC) + (uint64_t)now.tv_nsec;
}

struct taken_request {
    rmw_service_info_t info;
    struct pair request;
};

// Takes `count` requests from the service, polling; asserts they all came.
static void take_requests(rmw_service_t* service, struct taken_request* into, int count) {
    int got = 0;
    for (int attempt = 0; attempt < POLLS && got < count; attempt++) {
        bool taken = false;
        assert(RMW_RET_OK == rmw_take_request(service, &into[got].info, &into[got].request, &taken));
        if (taken) {
            got++;
        } else {
            sleep_poll();
        }
    }
    assert(got == count);
}

static void answer(rmw_service_t* service, struct taken_request* request) {
    struct pair response = {.first = request->request.first + request->request.second, .second = MARK};
    assert(RMW_RET_OK == rmw_send_response(service, &request->info.request_id, &response));
}

// Takes responses from `client` until `count` have come; checks each against the request its sequence id names
// (sent[k] has sequence id sequences[k]) and returns how many matched.
static int take_responses(rmw_client_t* client, const int64_t* sequences, const struct pair* sent, int count) {
    int got = 0;
    int matched = 0;
    for (int attempt = 0; attempt < POLLS && got < count; attempt++) {
        rmw_service_info_t info;
        struct pair response = {0, 0};
        bool taken = false;
        assert(RMW_RET_OK == rmw_take_response(client, &info, &response, &taken));
        if (!taken) {
            sleep_poll();
            continue;
        }
        got++;
        for (int k = 0; k < count; k++) {
            if (sequences[k] == info.request_id.sequence_number) {
                matched += response.first == sent[k].first + sent[k].second && MARK == response.second ? 1 : 0;
            }
        }
    }
    assert(got == count);
    return matched;
}

// N in a row, all in flight at once; taken together, answered in reverse order.
static void n_in_a_row(rmw_client_t* client, rmw_service_t* service) {
    int64_t sequences[N];
    struct pair sent[N];
    for (int k = 0; k < N; k++) {
        sent[k] = (struct pair) {.first = IN_A_ROW_FIRST + (uint32_t)k, .second = ADDEND};
        uint64_t before = now_ns();
        assert(RMW_RET_OK == rmw_send_request(client, &sent[k], &sequences[k]));
        assert(now_ns() - before < RMW_TICKLE_CLIENT_SEND_BLOCK_NS / 2); // a free slot: no wait
        for (int j = 0; j < k; j++) {
            assert(sequences[j] != sequences[k]);
        }
    }
    struct taken_request requests[N];
    take_requests(service, requests, N);
    for (int k = 0; k < N; k++) {
        for (int j = 0; j < k; j++) {
            assert(requests[j].info.request_id.sequence_number != requests[k].info.request_id.sequence_number);
        }
    }
    for (int k = N - 1; k >= 0; k--) {
        answer(service, &requests[k]);
    }
    assert(N == take_responses(client, sequences, sent, N));
    printf("test_service_multi_call: %d in a row, answered in reverse, each under its own sequence id\n", N);
}

// N in flight, unanswered: call N + 1 waits for a slot and then fails; one answer frees a slot for it.
static void one_more_waits_then_fails(rmw_client_t* client, rmw_service_t* service) {
    int64_t sequences[N + 1];
    struct pair sent[N + 1];
    for (int k = 0; k < N; k++) {
        sent[k] = (struct pair) {.first = REFUSED_FIRST + (uint32_t)k, .second = ADDEND};
        assert(RMW_RET_OK == rmw_send_request(client, &sent[k], &sequences[k]));
    }
    struct taken_request requests[N];
    take_requests(service, requests, N); // taken, not answered: every slot stays in flight

    sent[N] = (struct pair) {.first = EXTRA_FIRST, .second = ADDEND};
    uint64_t before = now_ns();
    rmw_ret_t refused = rmw_send_request(client, &sent[N], &sequences[N]);
    uint64_t waited = now_ns() - before;
    assert(RMW_RET_ERROR == refused);
    rmw_reset_error();
    assert(waited >= (RMW_TICKLE_CLIENT_SEND_BLOCK_NS * 9U) / 10U);

    answer(service, &requests[0]);
    assert(1 == take_responses(client, sequences, sent, 1)); // its slot is free once the response is in
    assert(RMW_RET_OK == rmw_send_request(client, &sent[N], &sequences[N]));
    struct taken_request last;
    take_requests(service, &last, 1);
    for (int k = 1; k < N; k++) {
        answer(service, &requests[k]);
    }
    answer(service, &last);
    assert(N == take_responses(client, &sequences[1], &sent[1], N));
    printf("test_service_multi_call: call %d waited %.0f ms and failed; went once a slot was free\n", N + 1,
           (double)waited / (double)NS_PER_MS);
}

// Action-like: the result requests of two goals wait at the service (taken, the goals still running) while another
// client calls too; the goals finish in reverse order.
static void action_like_get_result(rmw_client_t* result_client, rmw_client_t* other_client, rmw_service_t* service) {
    int goals = N >= 2 ? 2 : 1;
    int64_t sequences[2];
    struct pair sent[2];
    for (int goal = 0; goal < goals; goal++) {
        sent[goal] = (struct pair) {.first = GOALS_FIRST + (uint32_t)goal, .second = ADDEND};
        assert(RMW_RET_OK == rmw_send_request(result_client, &sent[goal], &sequences[goal]));
    }
    int64_t other_sequence = 0;
    struct pair other_sent = {.first = OTHER_FIRST, .second = ADDEND};
    assert(RMW_RET_OK == rmw_send_request(other_client, &other_sent, &other_sequence));

    struct taken_request requests[3];
    take_requests(service, requests, goals + 1);
    // Find which taken request is the other client's: its writer_guid is that client's gid.
    rmw_gid_t other_gid;
    assert(RMW_RET_OK == rmw_get_gid_for_client(other_client, &other_gid));
    int other = -1;
    for (int k = 0; k < goals + 1; k++) {
        if (requests[k].request.first == other_sent.first) {
            other = k;
            assert(0 == memcmp(requests[k].info.request_id.writer_guid, other_gid.data, RMW_GID_STORAGE_SIZE) ||
                   0 == memcmp(requests[k].info.request_id.writer_guid, (uint8_t[RMW_GID_STORAGE_SIZE]) {0},
                               RMW_GID_STORAGE_SIZE));
        }
    }
    assert(other >= 0);
    answer(service, &requests[other]); // the other client's call is answered while the goals run
    assert(1 == take_responses(other_client, &other_sequence, &other_sent, 1));
    for (int k = goals; k >= 0; k--) { // the goals finish, the later first
        if (k != other) {
            answer(service, &requests[k]);
        }
    }
    assert(goals == take_responses(result_client, sequences, sent, goals));
    printf("test_service_multi_call: %d result requests waited together with another client's call\n", goals);
}

// Answers any request waiting at the service and takes any response waiting at the client; true if either was there,
// and *responded whether a response was.
static bool serve_and_take(rmw_client_t* client, rmw_service_t* service, bool* responded) {
    struct taken_request request;
    bool request_taken = false;
    assert(RMW_RET_OK == rmw_take_request(service, &request.info, &request.request, &request_taken));
    if (request_taken) {
        answer(service, &request);
    }
    rmw_service_info_t info;
    struct pair response = {0, 0};
    bool response_taken = false;
    assert(RMW_RET_OK == rmw_take_response(client, &info, &response, &response_taken));
    *responded = response_taken;
    return request_taken || response_taken;
}

// The BEST_EFFORT counterpart of one_more_waits_then_fails() (D6: its requests are sent once, so a call still
// outstanding has most likely been lost): call N + 1 goes at once, ending the oldest call, whose answer - it comes
// after all here - is dropped; the other N are answered under their own sequence ids.
static void best_effort_ends_the_oldest(rmw_client_t* client, rmw_service_t* service) {
    int64_t sequences[N + 1];
    struct pair sent[N + 1];
    for (int k = 0; k < N; k++) {
        sent[k] = (struct pair) {.first = BEST_EFFORT_FIRST + (uint32_t)k, .second = ADDEND};
        assert(RMW_RET_OK == rmw_send_request(client, &sent[k], &sequences[k]));
    }
    struct taken_request requests[N + 1];
    take_requests(service, requests, N); // taken, not answered: every slot stays in flight

    sent[N] = (struct pair) {.first = BEST_EFFORT_FIRST + N, .second = ADDEND};
    uint64_t before = now_ns();
    assert(RMW_RET_OK == rmw_send_request(client, &sent[N], &sequences[N]));
    uint64_t waited = now_ns() - before;
    assert(waited < RMW_TICKLE_CLIENT_SEND_BLOCK_NS / 2); // no wait for a slot
    take_requests(service, &requests[N], 1);
    for (int k = 0; k <= N; k++) {
        answer(service, &requests[k]); // the first call's answer too: dropped, its call was ended
    }
    assert(N == take_responses(client, &sequences[1], &sent[1], N));
    struct timespec settle = {.tv_sec = 0, .tv_nsec = 200L * NS_PER_MS};
    nanosleep(&settle, NULL);
    rmw_service_info_t info;
    struct pair response = {0, 0};
    bool taken = true;
    assert(RMW_RET_OK == rmw_take_response(client, &info, &response, &taken));
    assert(!taken); // nothing for the ended call
    printf("test_service_multi_call: BEST_EFFORT call %d went in %.1f ms, ending the oldest; %d answered\n", N + 1,
           (double)waited / (double)NS_PER_MS, N);
}

int main(void) {
    request_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    response_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    service_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;

    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_service_multi_call", "/");
    assert(NULL != node);

    rmw_qos_profile_t qos = rmw_qos_profile_services_default;
    rmw_service_t* service = rmw_create_service(node, &service_handle, "/multi_call_add", &qos);
    rmw_client_t* client = rmw_create_client(node, &service_handle, "/multi_call_add", &qos);
    rmw_client_t* other_client = rmw_create_client(node, &service_handle, "/multi_call_add", &qos);
    assert(NULL != service && NULL != client && NULL != other_client);

    // Warm-up: one call, retried by core until discovery has matched the client and the service.
    int64_t sequence = 0;
    struct pair warm = {.first = 1, .second = 2};
    assert(RMW_RET_OK == rmw_send_request(client, &warm, &sequence));
    struct taken_request first;
    take_requests(service, &first, 1);
    answer(service, &first);
    assert(1 == take_responses(client, &sequence, &warm, 1));

    n_in_a_row(client, service);
    one_more_waits_then_fails(client, service);
    action_like_get_result(client, other_client, service);

    // A BEST_EFFORT client of the same service (D6). Its warm-up call is not resent, so it is made again until one is
    // answered: the first can go before discovery has matched the client to the service.
    rmw_qos_profile_t best_effort_qos = rmw_qos_profile_services_default;
    best_effort_qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    rmw_client_t* best_effort = rmw_create_client(node, &service_handle, "/multi_call_add", &best_effort_qos);
    assert(NULL != best_effort);
    rmw_qos_profile_t actual;
    assert(RMW_RET_OK == rmw_client_request_publisher_get_actual_qos(best_effort, &actual));
    assert(RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT == actual.reliability);
    bool warmed = false;
    for (int attempt = 0; attempt < POLLS && !warmed; attempt++) {
        if (attempt % WARM_RESEND_POLLS == 0) { // a call, then a second of polls for its answer
            assert(RMW_RET_OK == rmw_send_request(best_effort, &warm, &sequence));
        }
        (void)serve_and_take(best_effort, service, &warmed);
        sleep_poll();
    }
    assert(warmed);
    for (int quiet = 0; quiet < WARM_RESEND_POLLS / 2;) { // a warm-up call answered late must not reach the case
        bool responded = false;
        quiet = serve_and_take(best_effort, service, &responded) ? 0 : quiet + 1;
        sleep_poll();
    }
    best_effort_ends_the_oldest(best_effort, service);
    assert(RMW_RET_OK == rmw_destroy_client(node, best_effort));

    assert(RMW_RET_OK == rmw_destroy_client(node, other_client));
    assert(RMW_RET_OK == rmw_destroy_client(node, client));
    assert(RMW_RET_OK == rmw_destroy_service(node, service));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_service_multi_call: PASS (tt_CLIENT_MAX_OUTSTANDING %d)\n", N);
    return 0;
}
