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
// rmw_send_response(). Unlike rmw_client.c, this direction used to need a real bridge: TickLE's
// own tt_SERVER_CALLBACK originally had to synchronously fill a response and return before
// returning, with no "send it later" API of its own, while ROS 2's rmw contract splits the
// equivalent into two independent calls (rmw_take_request(), then later rmw_send_response()) a
// real rclcpp service handler calls back-to-back within one executor callback slot.
// server_callback() below used to bridge the two by blocking the *entire node's* poll thread
// (pthread_cond_timedwait()) between them - not just this one service's traffic, everything else
// that node owned too, for up to a bounded timeout.
//
// Milestone 17 (rmw_tickle/PLAN.md) closed that gap in TickLE core itself instead: a
// tt_SERVER_CALLBACK may now return tt_CALL_DEFERRED and answer later, from any thread, via
// tt_Server_send_response() - exactly the two-call split rmw already wanted. server_callback()
// below just converts the incoming request and defers immediately; rmw_send_response() converts
// the ROS response and calls tt_Server_send_response() directly, no waiting on either side.
// TickLE core itself now owns the "give up and reclaim the slot" timeout (tt_SERVER_
// DEFERRED_RESPONSE_TIMEOUT, tickle/config.h) that used to live here as RMW_TICKLE_SERVICE_
// RESPONSE_TIMEOUT_NS.
//
// One thing the old blocking design got "for free" that this one has to do explicitly: the whole
// poll thread being stuck inside one server_callback() call meant a second CallRequest for the
// same service simply couldn't arrive while the first was still outstanding. A deferred callback
// returns immediately, so the poll thread is free to receive more datagrams meanwhile -
// server_callback() below rejects a second request outright while the first one is still
// un-answered, preserving the exact same single-outstanding-request-at-a-time limit
// rmw_tickle_client_t's own tt_Client_call() already has (a deliberate, documented, unchanged
// scope boundary - queueing several in-flight requests is not what this milestone set out to fix).

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tickle/hal.h> // tt_ret_t/tt_RET_OK
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

// A tt_SERVER_CALLBACK must not return tt_CALL_TIMEOUT (-128) or tt_CALL_DEFERRED (-127) itself
// (both reserved - see tickle.h's own doc comments); this is rmw_tickle's own sentinel for
// "conversion of the incoming request failed" / "a request is already outstanding, rejecting this
// second one outright" - either way TickLE gets no usable response from this call, only that
// something went wrong server-side.
#define RMW_TICKLE_SERVER_CALLBACK_ERROR ((int8_t)-1)

// Runs on the poll thread, context_impl->node_mutex already held (rmw_tickle.h's own threading
// model). See this file's own module doc comment for the full deferred-response design.
static int8_t server_callback(struct tt_Server* tt_server, struct tt_Request* request, struct tt_Response* response,
                              tt_RequestId request_id) {
    (void)response; // deferred - nothing to fill in synchronously here, see module doc comment
    rmw_tickle_service_t* svc =
        (rmw_tickle_service_t*)((char*)tt_server - offsetof(rmw_tickle_service_t, tickle_server));

    pthread_mutex_lock(&svc->request_mutex);

    if (svc->request_available) {
        // See this file's own module doc comment on why this guard exists now, and why rejecting
        // outright (not queueing) matches an already-documented, deliberately unchanged limit.
        pthread_mutex_unlock(&svc->request_mutex);
        return RMW_TICKLE_SERVER_CALLBACK_ERROR;
    }

    // `request` aliases node->rx_buffer (DESIGN.md's "Strings" rule) - must convert to an
    // independently-owned ROS-shaped copy now, synchronously, before this callback returns - the
    // same reasoning as rmw_subscription.c's own subscriber_callback().
    if (!svc->request_callbacks->from_tickle(request, svc->request_storage)) {
        pthread_mutex_unlock(&svc->request_mutex);
        return RMW_TICKLE_SERVER_CALLBACK_ERROR;
    }

    svc->current_sequence_id = ++svc->next_sequence_id;
    svc->pending_request_id = request_id;
    svc->request_available = true;
    pthread_mutex_unlock(&svc->request_mutex);

    // Wake anyone blocked in rmw_wait() on this service's request - deliberately *not* held while
    // still holding request_mutex above: rmw_wait()'s own check_services() locks context_impl-
    // >wait_mutex first and svc->request_mutex second (see rmw_tickle_context_impl_t's own doc
    // comment), so taking them in the opposite order here would risk an AB-BA deadlock against a
    // concurrently running rmw_wait().
    rmw_tickle_context_impl_t* context_impl = svc->node->context_impl;
    pthread_mutex_lock(&context_impl->wait_mutex);
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);

    return tt_CALL_DEFERRED;
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
    // Milestone 34 - see rmw_tickle_publisher_t.owning_node_name's own doc comment.
    svc->owning_node_name = rcutils_strdup(node_impl->rmw_node.name, *allocator);
    svc->owning_node_namespace = rcutils_strdup(node_impl->rmw_node.namespace_, *allocator);
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

    svc->rmw_service.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    svc->rmw_service.data = svc;
    svc->rmw_service.service_name = rcutils_strdup(service_name, *allocator);
    if (NULL == svc->rmw_service.service_name) {
        RMW_SET_ERROR_MSG("failed to allocate service_name");
        pthread_mutex_destroy(&svc->request_mutex);
        allocator->deallocate(svc->request_storage, allocator->state);
        allocator->deallocate(svc->response_storage, allocator->state);
        allocator->deallocate(svc, allocator->state);
        return NULL;
    }

    tt_Node_interrupt(&node_impl->context_impl->tickle_node);
    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    tt_ret_t ret = tt_Node_create_server(&node_impl->context_impl->tickle_node, &svc->tickle_server, &svc->service,
                                         svc->rmw_service.service_name, server_callback);
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create_server() failed");
        allocator->deallocate((char*)svc->rmw_service.service_name, allocator->state);
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

    tt_Node_interrupt(&svc->node->context_impl->tickle_node);
    pthread_mutex_lock(&svc->node->context_impl->node_mutex);
    tt_Server_destroy(&svc->tickle_server);
    pthread_mutex_unlock(&svc->node->context_impl->node_mutex);

    pthread_mutex_destroy(&svc->request_mutex);

    rcutils_allocator_t allocator = svc->allocator;
    allocator.deallocate((char*)svc->rmw_service.service_name, allocator.state);
    allocator.deallocate(svc->request_storage, allocator.state);
    allocator.deallocate(svc->response_storage, allocator.state);
    allocator.deallocate(svc->owning_node_name, allocator.state);
    allocator.deallocate(svc->owning_node_namespace, allocator.state);
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
    // Consumed - frees server_callback() to accept a new request (see its own module-doc-comment
    // note on why it now rejects a second one outright rather than the old blocking design's
    // "the poll thread physically can't receive one" side effect). rmw_send_response() still
    // matches by sequence_number below, not this flag, so a response for *this* request remains
    // deliverable even after it flips false.
    svc->request_available = false;
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
        // Stale/unknown header - this request most likely already timed out at the TickLE level
        // (tt_SERVER_DEFERRED_RESPONSE_TIMEOUT, tickle/config.h) and had its slot reclaimed, or
        // (a real, documented, unchanged race - see this file's own module doc comment and rmw_
        // take_request()'s own note on why request_available alone can't guard this) a *new*
        // request already arrived and moved current_sequence_id on before this call got here.
        // Either way, tt_Server_send_response() below would also just report tt_RET_NOT_FOUND -
        // caught here first so the error message can be specific about *why*.
        pthread_mutex_unlock(&svc->request_mutex);
        RMW_SET_ERROR_MSG("no matching in-flight request for this response - it may have timed out");
        return RMW_RET_ERROR;
    }
    tt_RequestId request_id = svc->pending_request_id;
    pthread_mutex_unlock(&svc->request_mutex);

    // response_storage is a scratch conversion buffer, not synchronized by request_mutex - see
    // rmw_tickle.h's own rmw_tickle_service_t doc comment on why that's fine (only this function
    // ever touches it, and rmw's own contract doesn't promise concurrent rmw_send_response() calls
    // on the same service are safe to begin with).
    if (!svc->response_callbacks->to_tickle(ros_response, svc->response_storage)) {
        RMW_SET_ERROR_MSG("failed to convert ROS response to TickLE wire struct (capacity exceeded?)");
        return RMW_RET_ERROR;
    }

    // Queues the real answer for TickLE core's own poll thread to encode and send - see tt_Server_
    // send_response()'s own doc comment (tickle.h) for the full handoff. Not a failure of this
    // call if it comes back tt_RET_NOT_FOUND (the same stale/raced request this function's own
    // sequence_number check above is meant to catch first) - reported as RMW_RET_ERROR either way.
    tt_ret_t ret =
        tt_Server_send_response(&svc->tickle_server, request_id, 0, (struct tt_Response*)svc->response_storage);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Server_send_response() found no matching in-flight request - it may have timed out");
        return RMW_RET_ERROR;
    }
    return RMW_RET_OK;
}
