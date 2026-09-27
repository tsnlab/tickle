/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g2 (RMW_GAPS_PLAN.md): the four callback setters rclcpp's and rclpy's EventsExecutor use. For each: a call per
// item with its count; what is already waiting when a callback is set reported once, at set time; NULL stops the
// calls; a callback may take what it is told about without deadlocking; and once set(NULL) has returned, no call is
// running or starts; and a message queued while its callback is being set is reported, not missed. Mutants, each killed
// here: set() not reporting the backlog; set(NULL) not clearing; set(NULL) clearing without the slot's mutex, so a
// running callback outlives it; set() reading the backlog before storing the callback, so a message queued between
// the two is reported by neither. That last one is killed with the gap widened to 20 us, as a preemption would
// (19996 of 20000 missed; the right order widened the same way misses 0); left narrow it was missed 0 of 20000 times on
// the PC, so the race row guards the order, not a timing.
//
// Messages are handed to the subscription through tt_Subscriber.callback, as test_publish_take_reuse.c does (a
// publisher on the same context is never delivered to). Requests and responses go over the wire, as in
// test_service_roundtrip.c. A deadlock fails by alarm().

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>   // nanosleep()
#include <unistd.h> // alarm()

#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/event.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

#define DEADLOCK_S 60 // the whole test; a callback that deadlocks ends it here
#define POLLS 60      // attempts, POLL_MS apart: three seconds
#define POLL_MS 50L
#define RESEND_EVERY 20
#define NS_PER_MS 1000000L
#define MS_PER_S 1000L
#define SLOW_CALLBACK_MS 100L
#define DEADLINE_MS 20
#define BACKLOG 3
#define ANY_VALUE 7
#define FIRST 20
#define SECOND 22
#define RACE_ROUNDS 20000

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

static rosidl_typesupport_tickle_c_message_callbacks_t message_callbacks = PAIR_CALLBACKS("callbacks/msg/Pair");
static rosidl_typesupport_tickle_c_message_callbacks_t request_callbacks = PAIR_CALLBACKS("callbacks/srv/Add_Request");
static rosidl_typesupport_tickle_c_message_callbacks_t response_callbacks =
    PAIR_CALLBACKS("callbacks/srv/Add_Response");
static rosidl_message_type_support_t message_handle = {.data = &message_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t request_handle = {.data = &request_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t response_handle = {.data = &response_callbacks,
                                                        .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_service_callbacks_t service_callbacks = {.ros_type_name = "callbacks/srv/Add"};
static rosidl_service_type_support_t service_handle = {.data = &service_callbacks,
                                                       .func = get_service_typesupport_handle_function,
                                                       .request_typesupport = &request_handle,
                                                       .response_typesupport = &response_handle};

static void sleep_ms(long millis) {
    struct timespec interval = {.tv_sec = millis / MS_PER_S, .tv_nsec = (millis % MS_PER_S) * NS_PER_MS};
    nanosleep(&interval, NULL);
}

// What one callback has been told: how many calls, the items they reported, and the count of the last.
struct record {
    atomic_int calls;
    atomic_size_t items;
    atomic_size_t last;
};
static void record_reset(struct record* record) {
    atomic_store(&record->calls, 0);
    atomic_store(&record->items, 0);
    atomic_store(&record->last, 0);
}
static void on_event(const void* user_data, size_t count) {
    struct record* record = (struct record*)user_data;
    atomic_fetch_add(&record->items, count);
    atomic_store(&record->last, count);
    atomic_fetch_add(&record->calls, 1);
}

// -- Subscription --

static void deliver(rmw_tickle_subscriber_t* sub_impl, uint32_t value) {
    static uint64_t psn = 1;
    uint8_t wire[RMW_TICKLE_PSN_BYTES + sizeof(struct pair)] = {0};
    uint32_t header = rmw_tickle_psn_write(psn++, wire);
    struct pair message = {.first = value, .second = 0};
    memcpy(wire + header, &message, sizeof(message));
    struct tt_Data* data = sub_impl->topic.data_decode_inplace(wire, header + (uint32_t)sizeof(message), true);
    assert(NULL != data);
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, /*time=*/0, 0, data);
}

// A callback that takes what it is told about, as an EventsExecutor may: the queue's mutex must not be held.
static rmw_subscription_t* take_target;
static atomic_int taken_in_callback;
static void take_on_event(const void* user_data, size_t count) {
    (void)user_data;
    for (size_t i = 0; i < count; i++) {
        struct pair message;
        bool taken = false;
        assert(RMW_RET_OK == rmw_take(take_target, &message, &taken, NULL));
        atomic_fetch_add(&taken_in_callback, taken ? 1 : 0);
    }
}

// A callback still running when set(NULL) is called: set(NULL) must wait for it, so the flag set after set(NULL)
// returns is never seen inside a call.
static atomic_bool slow_entered;
static atomic_bool cleared;
static atomic_int calls_after_clear;
static void slow_on_event(const void* user_data, size_t count) {
    (void)user_data;
    (void)count;
    atomic_store(&slow_entered, true);
    sleep_ms(SLOW_CALLBACK_MS);
    atomic_fetch_add(&calls_after_clear, atomic_load(&cleared) ? 1 : 0);
}
static void* deliver_one(void* sub_impl) {
    deliver((rmw_tickle_subscriber_t*)sub_impl, ANY_VALUE);
    return NULL;
}

// A message delivered while the callback is being set, RACE_ROUNDS times: each is in the queue afterwards, so each
// must have been reported, in the set-time count or by its own call.
static atomic_int race_go;
static atomic_int race_done;
static void* deliver_in_rounds(void* sub_impl) {
    for (int round = 1; round <= RACE_ROUNDS; round++) {
        while (atomic_load(&race_go) != round) {
        }
        deliver((rmw_tickle_subscriber_t*)sub_impl, (uint32_t)round);
        atomic_store(&race_done, round);
    }
    return NULL;
}

static void drain(rmw_subscription_t* sub);

static int missed_while_setting(rmw_subscription_t* sub) {
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> above; glibc declares it via a private header
    assert(0 == pthread_create(&thread, NULL, deliver_in_rounds, sub->data));
    struct record record;
    int missed = 0;
    for (int round = 1; round <= RACE_ROUNDS; round++) {
        assert(RMW_RET_OK == rmw_subscription_set_on_new_message_callback(sub, NULL, NULL));
        drain(sub);
        record_reset(&record);
        atomic_store(&race_go, round);
        assert(RMW_RET_OK == rmw_subscription_set_on_new_message_callback(sub, on_event, &record));
        while (atomic_load(&race_done) != round) {
        }
        missed += 0 == atomic_load(&record.items) ? 1 : 0;
    }
    assert(0 == pthread_join(thread, NULL));
    assert(RMW_RET_OK == rmw_subscription_set_on_new_message_callback(sub, NULL, NULL));
    drain(sub);
    return missed;
}

static void drain(rmw_subscription_t* sub) {
    bool taken = true;
    while (taken) {
        struct pair message;
        assert(RMW_RET_OK == rmw_take(sub, &message, &taken, NULL));
    }
}

static void test_subscription(rmw_node_t* node) {
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.depth = 10;
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &message_handle, "/callbacks_topic", &qos, &options);
    assert(NULL != sub);
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    struct record record;
    record_reset(&record);

    // Waiting before the callback is set: reported once, at set time, with how many.
    for (uint32_t i = 0; i < BACKLOG; i++) {
        deliver(sub_impl, i);
    }
    assert(RMW_RET_OK == rmw_subscription_set_on_new_message_callback(sub, on_event, &record));
    assert(1 == atomic_load(&record.calls) && BACKLOG == atomic_load(&record.last));

    // Then one call per message, with 1.
    deliver(sub_impl, BACKLOG);
    assert(2 == atomic_load(&record.calls) && 1 == atomic_load(&record.last));
    assert(BACKLOG + 1 == atomic_load(&record.items));

    // NULL stops the calls.
    assert(RMW_RET_OK == rmw_subscription_set_on_new_message_callback(sub, NULL, NULL));
    deliver(sub_impl, BACKLOG + 1);
    assert(2 == atomic_load(&record.calls));
    drain(sub);

    // A callback may take the message it is told about.
    take_target = sub;
    assert(RMW_RET_OK == rmw_subscription_set_on_new_message_callback(sub, take_on_event, NULL));
    deliver(sub_impl, 1);
    deliver(sub_impl, 2);
    assert(2 == atomic_load(&taken_in_callback));

    // Once set(NULL) returns, no call is running and none starts.
    assert(RMW_RET_OK == rmw_subscription_set_on_new_message_callback(sub, slow_on_event, NULL));
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> above; glibc declares it via a private header
    assert(0 == pthread_create(&thread, NULL, deliver_one, sub_impl));
    while (!atomic_load(&slow_entered)) {
        sleep_ms(1);
    }
    assert(RMW_RET_OK == rmw_subscription_set_on_new_message_callback(sub, NULL, NULL));
    atomic_store(&cleared, true);
    assert(0 == pthread_join(thread, NULL));
    deliver(sub_impl, 3);
    assert(0 == atomic_load(&calls_after_clear));
    drain(sub);

    int missed = missed_while_setting(sub);
    printf("subscription: %d of %d messages queued while the callback was set went unreported\n", missed, RACE_ROUNDS);
    assert(0 == missed);

    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    printf("subscription: backlog %d at set, per-message 1, NULL stops, take in callback, none after NULL\n", BACKLOG);
}

// -- Service and client, over the wire --

static bool wait_for_calls(struct record* record, int calls) {
    for (int attempt = 0; attempt < POLLS; attempt++) {
        if (atomic_load(&record->calls) >= calls) {
            return true;
        }
        sleep_ms(POLL_MS);
    }
    return false;
}

static void test_service_and_client(rmw_node_t* node) {
    rmw_qos_profile_t qos = rmw_qos_profile_services_default;
    rmw_service_t* service = rmw_create_service(node, &service_handle, "/callbacks_add", &qos);
    rmw_client_t* client = rmw_create_client(node, &service_handle, "/callbacks_add", &qos);
    assert(NULL != service && NULL != client);
    struct record on_request;
    struct record on_response;
    record_reset(&on_request);
    record_reset(&on_response);

    // Nothing waiting: no call at set time.
    assert(RMW_RET_OK == rmw_service_set_on_new_request_callback(service, on_event, &on_request));
    assert(RMW_RET_OK == rmw_client_set_on_new_response_callback(client, on_event, &on_response));
    assert(0 == atomic_load(&on_request.calls) && 0 == atomic_load(&on_response.calls));

    // A request: a call with 1. Resent until discovery has matched the two and the service has heard one.
    struct pair request = {.first = FIRST, .second = SECOND};
    int64_t sequence = 0;
    for (int attempt = 0; attempt < POLLS && 0 == atomic_load(&on_request.calls); attempt++) {
        if (0 == attempt % RESEND_EVERY) {
            assert(RMW_RET_OK == rmw_send_request(client, &request, &sequence));
        }
        sleep_ms(POLL_MS);
    }
    assert(atomic_load(&on_request.calls) >= 1 && 1 == atomic_load(&on_request.last));

    // Set again while the request waits: reported at set time, with 1.
    struct record backlog;
    record_reset(&backlog);
    assert(RMW_RET_OK == rmw_service_set_on_new_request_callback(service, on_event, &backlog));
    assert(1 == atomic_load(&backlog.calls) && 1 == atomic_load(&backlog.last));

    rmw_service_info_t info;
    struct pair got_request;
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_request(service, &info, &got_request, &taken));
    assert(taken);
    struct pair response = {.first = got_request.first + got_request.second, .second = 0};
    assert(RMW_RET_OK == rmw_send_response(service, &info.request_id, &response));

    // The response: a call with 1; set again while it waits, reported with 1.
    assert(wait_for_calls(&on_response, 1));
    assert(1 == atomic_load(&on_response.last));
    record_reset(&backlog);
    assert(RMW_RET_OK == rmw_client_set_on_new_response_callback(client, on_event, &backlog));
    assert(1 == atomic_load(&backlog.calls) && 1 == atomic_load(&backlog.last));
    struct pair got_response;
    taken = false;
    assert(RMW_RET_OK == rmw_take_response(client, &info, &got_response, &taken));
    assert(taken && FIRST + SECOND == got_response.first);

    // Taken: nothing waits, so a new callback is not called at set time. Then NULL.
    record_reset(&backlog);
    assert(RMW_RET_OK == rmw_client_set_on_new_response_callback(client, on_event, &backlog));
    assert(0 == atomic_load(&backlog.calls));
    assert(RMW_RET_OK == rmw_client_set_on_new_response_callback(client, NULL, NULL));
    assert(RMW_RET_OK == rmw_service_set_on_new_request_callback(service, NULL, NULL));

    assert(RMW_RET_OK == rmw_destroy_client(node, client));
    assert(RMW_RET_OK == rmw_destroy_service(node, service));
    printf("service and client: 1 per item, 1 at set while waiting, 0 when nothing waits\n");
}

// -- Events: a deadline nobody meets --

static void test_event(rmw_node_t* node) {
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.deadline.sec = 0;
    qos.deadline.nsec = (uint64_t)DEADLINE_MS * NS_PER_MS;
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &message_handle, "/callbacks_deadline", &qos, &options);
    assert(NULL != sub);
    rmw_event_t event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_subscription_event_init(&event, sub, RMW_EVENT_REQUESTED_DEADLINE_MISSED));
    struct record record;
    record_reset(&record);

    // Several deadlines missed before the callback is set: one call at set time, with all of them.
    sleep_ms(DEADLINE_MS * 5L);
    assert(RMW_RET_OK == rmw_event_set_callback(&event, on_event, &record));
    assert(1 == atomic_load(&record.calls) && atomic_load(&record.last) >= 2);

    // Then a call per miss, with 1.
    size_t at_set = atomic_load(&record.items);
    assert(wait_for_calls(&record, 3));
    assert(1 == atomic_load(&record.last) && atomic_load(&record.items) >= at_set + 2);

    // NULL stops them.
    assert(RMW_RET_OK == rmw_event_set_callback(&event, NULL, NULL));
    int calls = atomic_load(&record.calls);
    sleep_ms(DEADLINE_MS * 5L);
    assert(calls == atomic_load(&record.calls));

    assert(RMW_RET_OK == rmw_event_fini(&event));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    printf("event: missed deadlines reported at set time and one per miss, NULL stops\n");
}

int main(void) {
    alarm(DEADLOCK_S);
    message_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    request_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    response_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    service_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;

    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_event_callbacks", "/");
    assert(NULL != node);

    test_subscription(node);
    test_service_and_client(node);
    test_event(node);

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_event_callbacks: PASS\n");
    return 0;
}
