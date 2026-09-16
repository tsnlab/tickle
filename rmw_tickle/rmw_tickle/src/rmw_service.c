/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 4: rmw_create_service()/rmw_destroy_service()/rmw_take_request()/
// rmw_send_response(). Unlike rmw_client.c, this direction needs a real bridge: TickLE's own
// tt_SERVER_CALLBACK must synchronously fill a response and return before returning - it has no
// "send it later" API - while ROS 2's rmw contract splits the equivalent into two independent
// calls (rmw_take_request(), then later rmw_send_response()) that a real rclcpp service handler
// calls back-to-back within one executor callback slot. server_callback() below (running on the
// node's poll thread, node->mutex already held per rmw_tickle.h's own threading model) bridges the
// two with a bounded pthread_cond_timedwait(): it publishes the (already converted, see below) ROS
// request and blocks until rmw_send_response() (called from whatever thread owns the executor)
// signals a response is ready, or RMW_TICKLE_SERVICE_RESPONSE_TIMEOUT_NS elapses.
//
// Consequence documented in rmw_tickle.h's own doc comment on RMW_TICKLE_SERVICE_RESPONSE_TIMEOUT_
// NS: a slow or asynchronously-deferred handler blocks the *entire node's* poll thread (not just
// this one service) for up to that timeout - not just this one service's traffic. Acceptable for
// the synchronous single-threaded-executor pattern this rmw targets first (rmw_tickle/PLAN.md);
// documented, not solved, for the general case.

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <tickle/config.h> // tt_SECOND
#include <tickle/hal.h>    // tt_ret_t/tt_RET_OK
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw/validate_full_topic_name.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

// A tt_SERVER_CALLBACK must not return tt_CALL_TIMEOUT (-128, reserved - see tickle.h's own doc
// comment on it) itself; this is rmw_tickle's own sentinel for "conversion of the incoming request
// failed" / "no rmw_send_response() arrived before the bridge timed out" - either way TickLE gets
// no usable response from this call, only that something went wrong server-side.
#define RMW_TICKLE_SERVER_CALLBACK_ERROR ((int8_t)-1)

// Runs on the node's poll thread, node->mutex already held (rmw_tickle.h's own threading model).
// See this file's own module doc comment for the full bridge rationale.
static int8_t server_callback(struct tt_Server* tt_server, struct tt_Request* request, struct tt_Response* response) {
    rmw_tickle_service_t* svc =
        (rmw_tickle_service_t*)((char*)tt_server - offsetof(rmw_tickle_service_t, tickle_server));

    pthread_mutex_lock(&svc->request_mutex);

    // `request` aliases node->rx_buffer (DESIGN.md's "Strings" rule) - must convert to an
    // independently-owned ROS-shaped copy now, synchronously, before this callback returns - the
    // same reasoning as rmw_subscription.c's own subscriber_callback().
    if (!svc->request_callbacks->from_tickle(request, svc->request_storage)) {
        pthread_mutex_unlock(&svc->request_mutex);
        return RMW_TICKLE_SERVER_CALLBACK_ERROR;
    }

    svc->current_sequence_id = ++svc->next_sequence_id;
    svc->request_available = true;
    svc->response_ready = false;
    pthread_mutex_unlock(&svc->request_mutex);

    // Wake anyone blocked in rmw_wait() on this service's request - deliberately *not* held while
    // still holding request_mutex above: rmw_wait()'s own check_services() locks context_impl-
    // >wait_mutex first and svc->request_mutex second (see rmw_tickle_context_impl_t's own doc
    // comment), so taking them in the opposite order here would risk an AB-BA deadlock against a
    // concurrently running rmw_wait(). request_mutex is re-locked immediately below, before the
    // response-wait loop starts; response_ready is re-checked fresh once re-locked, so a response
    // that raced in during this brief gap (extremely unlikely, but not otherwise ruled out) is
    // still observed correctly rather than waited on unnecessarily.
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)svc->node->context->impl;
    pthread_mutex_lock(&context_impl->wait_mutex);
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);

    pthread_mutex_lock(&svc->request_mutex);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline); // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own comment
    deadline.tv_nsec += RMW_TICKLE_SERVICE_RESPONSE_TIMEOUT_NS;
    // tt_SECOND (nanoseconds per second) instead of a raw 1000000000 literal - already the
    // project's own name for this exact quantity (config.h), and RMW_TICKLE_SERVICE_RESPONSE_
    // TIMEOUT_NS above is itself defined as a multiple of it.
    deadline.tv_sec += (time_t)(deadline.tv_nsec / (long)tt_SECOND);
    deadline.tv_nsec %= (long)tt_SECOND;

    int wait_ret = 0;
    while (!svc->response_ready && wait_ret == 0) {
        wait_ret = pthread_cond_timedwait(&svc->request_cond, &svc->request_mutex, &deadline);
    }

    int8_t result = RMW_TICKLE_SERVER_CALLBACK_ERROR;
    if (svc->response_ready) {
        // Already TickLE-shaped (rmw_send_response() did the to_tickle() conversion) - copy
        // verbatim into TickLE's own out-parameter, no further conversion needed here.
        memcpy(response, svc->response_storage, svc->response_callbacks->tickle_struct_size);
        result = 0;
    }

    svc->request_available = false;
    svc->response_ready = false;
    pthread_mutex_unlock(&svc->request_mutex);
    return result;
}

rmw_service_t* rmw_create_service(const rmw_node_t* node, const rosidl_service_type_support_t* type_support,
                                  const char* service_name, const rmw_qos_profile_t* qos_policies) {
    if (NULL == node) {
        RMW_SET_ERROR_MSG("node is null");
        return NULL;
    }
    if (NULL == service_name) {
        RMW_SET_ERROR_MSG("service_name is null");
        return NULL;
    }
    if (NULL == qos_policies) {
        RMW_SET_ERROR_MSG("qos_policies is null");
        return NULL;
    }
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }
    // Never actually checked before test_rmw_implementation's own test_service.cpp's create_with_
    // bad_arguments exercised it for the first time (Milestone 16) - an empty, space-containing, or
    // relative (no leading '/') service_name used to silently succeed instead of being rejected,
    // the exact same class of gap Milestone 15 already found and fixed for rmw_create_node()'s own
    // name/namespace.
    int validation_result = RMW_TOPIC_VALID;
    size_t invalid_index = 0;
    if (RMW_RET_OK != rmw_validate_full_topic_name(service_name, &validation_result, &invalid_index)) {
        return NULL; // rmw_validate_full_topic_name() already set its own error message
    }
    if (RMW_TOPIC_VALID != validation_result) {
        RMW_SET_ERROR_MSG(rmw_full_topic_name_validation_result_string(validation_result));
        return NULL;
    }
    if (rmw_tickle_validate_qos_profile(qos_policies, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT) != RMW_RET_OK) {
        return NULL; // error message already set
    }

    rmw_tickle_service_typesupport_t callbacks;
    if (!rmw_tickle_get_service_callbacks(type_support, &callbacks)) {
        return NULL; // error message already set
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rcutils_allocator_t* allocator = &node_impl->allocator;

    rmw_tickle_service_t* svc =
        (rmw_tickle_service_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_service_t), allocator->state);
    if (NULL == svc) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_service_t");
        return NULL;
    }
    svc->node = node_impl;
    svc->type_support = type_support;
    svc->request_callbacks = callbacks.request;
    svc->response_callbacks = callbacks.response;
    svc->allocator = *allocator;

    svc->service.name = callbacks.service->ros_type_name; // see rmw_tickle_client_t.service's own doc comment
    svc->service.request_size = (uint32_t)callbacks.request->tickle_struct_size;
    svc->service.response_size = (uint32_t)callbacks.response->tickle_struct_size;
    svc->service.request_encode_size = (tt_REQUEST_ENCODE_SIZE)callbacks.request->tickle_encode_size;
    svc->service.request_encode = (tt_REQUEST_ENCODE)callbacks.request->tickle_encode;
    svc->service.request_decode = (tt_REQUEST_DECODE)callbacks.request->tickle_decode;
    svc->service.request_free = (tt_REQUEST_FREE)callbacks.request->tickle_free;
    svc->service.response_encode_size = (tt_RESPONSE_ENCODE_SIZE)callbacks.response->tickle_encode_size;
    svc->service.response_encode = (tt_RESPONSE_ENCODE)callbacks.response->tickle_encode;
    svc->service.response_decode = (tt_RESPONSE_DECODE)callbacks.response->tickle_decode;
    svc->service.response_free = (tt_RESPONSE_FREE)callbacks.response->tickle_free;

    svc->request_storage = allocator->zero_allocate(1, callbacks.request->ros_struct_size, allocator->state);
    svc->response_storage = allocator->zero_allocate(1, callbacks.response->tickle_struct_size, allocator->state);
    if (NULL == svc->request_storage || NULL == svc->response_storage) {
        RMW_SET_ERROR_MSG("failed to allocate request/response storage");
        allocator->deallocate(svc->request_storage, allocator->state);
        allocator->deallocate(svc->response_storage, allocator->state);
        allocator->deallocate(svc, allocator->state);
        return NULL;
    }

    if (pthread_mutex_init(&svc->request_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize service request mutex");
        allocator->deallocate(svc->request_storage, allocator->state);
        allocator->deallocate(svc->response_storage, allocator->state);
        allocator->deallocate(svc, allocator->state);
        return NULL;
    }
    if (pthread_cond_init(&svc->request_cond, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize service request condition variable");
        pthread_mutex_destroy(&svc->request_mutex);
        allocator->deallocate(svc->request_storage, allocator->state);
        allocator->deallocate(svc->response_storage, allocator->state);
        allocator->deallocate(svc, allocator->state);
        return NULL;
    }

    svc->rmw_service.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    svc->rmw_service.data = svc;
    svc->rmw_service.service_name = rcutils_strdup(service_name, *allocator);
    if (NULL == svc->rmw_service.service_name) {
        RMW_SET_ERROR_MSG("failed to allocate service_name");
        pthread_cond_destroy(&svc->request_cond);
        pthread_mutex_destroy(&svc->request_mutex);
        allocator->deallocate(svc->request_storage, allocator->state);
        allocator->deallocate(svc->response_storage, allocator->state);
        allocator->deallocate(svc, allocator->state);
        return NULL;
    }

    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_mutex_lock(&node_impl->mutex);
    tt_ret_t ret = tt_Node_create_server(&node_impl->tickle_node, &svc->tickle_server, &svc->service,
                                         svc->rmw_service.service_name, server_callback);
    pthread_mutex_unlock(&node_impl->mutex);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create_server() failed");
        allocator->deallocate((char*)svc->rmw_service.service_name, allocator->state);
        pthread_cond_destroy(&svc->request_cond);
        pthread_mutex_destroy(&svc->request_mutex);
        allocator->deallocate(svc->request_storage, allocator->state);
        allocator->deallocate(svc->response_storage, allocator->state);
        allocator->deallocate(svc, allocator->state);
        return NULL;
    }

    return &svc->rmw_service;
}

rmw_ret_t rmw_destroy_service(rmw_node_t* node, rmw_service_t* service) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier) ||
        !rmw_tickle_identifier_matches(service->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_service_t* svc = (rmw_tickle_service_t*)service->data;

    tt_Node_interrupt(&svc->node->tickle_node);
    pthread_mutex_lock(&svc->node->mutex);
    tt_Server_destroy(&svc->tickle_server);
    pthread_mutex_unlock(&svc->node->mutex);

    pthread_cond_destroy(&svc->request_cond);
    pthread_mutex_destroy(&svc->request_mutex);

    rcutils_allocator_t allocator = svc->allocator;
    allocator.deallocate((char*)svc->rmw_service.service_name, allocator.state);
    allocator.deallocate(svc->request_storage, allocator.state);
    allocator.deallocate(svc->response_storage, allocator.state);
    allocator.deallocate(svc, allocator.state);
    return RMW_RET_OK;
}

rmw_ret_t rmw_take_request(const rmw_service_t* service, rmw_service_info_t* request_header, void* ros_request,
                           bool* taken) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(request_header, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(service->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_service_t* svc = (rmw_tickle_service_t*)service->data;

    pthread_mutex_lock(&svc->request_mutex);
    if (!svc->request_available) {
        pthread_mutex_unlock(&svc->request_mutex);
        *taken = false;
        return RMW_RET_OK;
    }
    // Same "ros_request assumed fresh/blank" caveat as rmw_subscription.c's own rmw_take_with_
    // info() - see its doc comment there.
    memcpy(ros_request, svc->request_storage, svc->request_callbacks->ros_struct_size);
    int64_t seq = svc->current_sequence_id;
    svc->request_available = false; // consumed - server_callback() is still blocked waiting
    pthread_mutex_unlock(&svc->request_mutex);

    *taken = true;
    memset(request_header, 0, sizeof(*request_header));
    request_header->request_id.sequence_number = seq;
    // source_timestamp/received_timestamp/writer_guid left zeroed - no real timestamp/GID
    // tracking on this path yet (matches rmw_publisher.c's own message_info gap).
    return RMW_RET_OK;
}

rmw_ret_t rmw_send_response(const rmw_service_t* service, rmw_request_id_t* request_header, void* ros_response) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(request_header, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_response, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(service->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_service_t* svc = (rmw_tickle_service_t*)service->data;

    pthread_mutex_lock(&svc->request_mutex);
    if (request_header->sequence_number != svc->current_sequence_id) {
        // Stale/unknown header - server_callback()'s own bridge most likely already timed out
        // (RMW_TICKLE_SERVICE_RESPONSE_TIMEOUT_NS) and moved on.
        pthread_mutex_unlock(&svc->request_mutex);
        RMW_SET_ERROR_MSG("no matching in-flight request for this response - it may have timed out");
        return RMW_RET_ERROR;
    }

    if (!svc->response_callbacks->to_tickle(ros_response, svc->response_storage)) {
        pthread_mutex_unlock(&svc->request_mutex);
        RMW_SET_ERROR_MSG("failed to convert ROS response to TickLE wire struct (capacity exceeded?)");
        return RMW_RET_ERROR;
    }
    svc->response_ready = true;
    pthread_cond_signal(&svc->request_cond);
    pthread_mutex_unlock(&svc->request_mutex);
    return RMW_RET_OK;
}
