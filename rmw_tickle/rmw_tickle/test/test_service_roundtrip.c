/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A real service call through rmw_tickle, in one process: client and service on one node, request
// out, request taken, response sent (deferred - rmw always answers through tt_Server_send_response()),
// response taken. Written with stage (iv) of the storage design (2026-09-24), which moved every
// byte of that path - the client's request cache, the server's pending and cached responses - out
// of core's inline arrays (built 8 bytes wide in rmw_tickle) into storage attached per service. If
// the attachment were missing or mis-sized, this is the call that would fail; no other rmw test
// makes one.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h> // nanosleep()

#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
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

#define FIRST 20
#define SECOND 22
#define MARK 0xABCDU // the response's second field, to tell it from a request echoed back
#define POLLS 60     // attempts, POLL_MS apart: three seconds
#define POLL_MS 50L
#define RESEND_EVERY 20 // polls between request resends until the service has heard one
#define NS_PER_MS 1000000L

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

static rosidl_typesupport_tickle_c_message_callbacks_t request_callbacks = PAIR_CALLBACKS("roundtrip/srv/Add_Request");
static rosidl_typesupport_tickle_c_message_callbacks_t response_callbacks =
    PAIR_CALLBACKS("roundtrip/srv/Add_Response");
static rosidl_message_type_support_t request_handle = {.data = &request_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t response_handle = {.data = &response_callbacks,
                                                        .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_service_callbacks_t service_callbacks = {.ros_type_name = "roundtrip/srv/Add"};
static rosidl_service_type_support_t service_handle = {.data = &service_callbacks,
                                                       .func = get_service_typesupport_handle_function,
                                                       .request_typesupport = &request_handle,
                                                       .response_typesupport = &response_handle};

static void sleep_poll(void) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = POLL_MS * NS_PER_MS};
    nanosleep(&interval, NULL);
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
    rmw_node_t* node = rmw_create_node(&context, "test_service_roundtrip", "/");
    assert(NULL != node);

    rmw_qos_profile_t qos = rmw_qos_profile_services_default;
    rmw_service_t* service = rmw_create_service(node, &service_handle, "/roundtrip_add", &qos);
    rmw_client_t* client = rmw_create_client(node, &service_handle, "/roundtrip_add", &qos);
    assert(NULL != service && NULL != client);

    // The storage really is the attached, per-service kind: sized for this 8-byte type, not the
    // inline arrays (8 bytes in this build) and not a datagram.
    rmw_tickle_service_t* svc = (rmw_tickle_service_t*)service->data;
    assert(svc->tickle_server.cache_storage == svc->response_cache && NULL != svc->response_cache);
    assert(svc->tickle_server.pending_storage == svc->pending_responses && NULL != svc->pending_responses);
    assert(svc->tickle_server.pending_entry_length == sizeof(struct pair));
    assert(svc->tickle_server.cache_entry_length < (uint32_t)tt_MAX_BUFFER_LENGTH);
    rmw_tickle_client_t* cli = (rmw_tickle_client_t*)client->data;
    assert(cli->tickle_client.cache_storage == cli->request_cache && NULL != cli->request_cache);

    // Discovery has to match the client to the server before a request can go anywhere.
    struct pair request = {.first = FIRST, .second = SECOND};
    int64_t sequence = 0;
    bool taken = false;
    rmw_service_info_t info;
    struct pair got_request;
    for (int attempt = 0; attempt < POLLS && !taken; attempt++) {
        if (0 == attempt % RESEND_EVERY) {
            assert(RMW_RET_OK == rmw_send_request(client, &request, &sequence)); // resend until heard
        }
        sleep_poll();
        assert(RMW_RET_OK == rmw_take_request(service, &info, &got_request, &taken));
    }
    assert(taken && FIRST == got_request.first && SECOND == got_request.second);

    struct pair response = {.first = got_request.first + got_request.second, .second = MARK};
    assert(RMW_RET_OK == rmw_send_response(service, &info.request_id, &response));

    struct pair got_response = {0, 0};
    taken = false;
    for (int attempt = 0; attempt < POLLS && !taken; attempt++) {
        sleep_poll();
        assert(RMW_RET_OK == rmw_take_response(client, &info, &got_response, &taken));
    }
    assert(taken && FIRST + SECOND == got_response.first && MARK == got_response.second);

    assert(RMW_RET_OK == rmw_destroy_client(node, client));
    assert(RMW_RET_OK == rmw_destroy_service(node, service));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_service_roundtrip: PASS (response %u)\n", got_response.first);
    return 0;
}
