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
// Several requests at once (2026-10-16): a deferred callback returns immediately, so more requests arrive while
// earlier ones wait to be taken or answered - from several clients, and several from one (tt_CLIENT_MAX_OUTSTANDING
// each). server_callback() queues each, rmw_take_request() hands them out oldest first, and each stays answerable
// until core's tt_SERVER_DEFERRED_RESPONSE_TIMEOUT whatever arrives after it: its request_id.sequence_number carries
// the core request id (pack_sequence_id()). Until then one request waited at a time, a second was answered with an
// error the client took as its answer, and a request already taken could not be answered once the next arrived.

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tickle/config.h> // tt_MAX_SERVER_CACHE_COUNT, tt_MAX_CONTEXT_IDS
#include <tickle/hal.h>    // tt_ret_t/tt_RET_OK
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/event_callback_type.h" // rmw_event_callback_t (g2)
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw/validate_full_topic_name.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

// A tt_SERVER_CALLBACK must not return tt_CALL_TIMEOUT (-128), tt_CALL_DEFERRED (-127) or tt_CALL_IGNORED (-126)
// itself unless it means them (all reserved - see tickle.h's own doc comments); this is rmw_tickle's own sentinel for
// "conversion of the incoming request failed" - TickLE gets no usable response from this call, only that something
// went wrong server-side.
#define RMW_TICKLE_SERVER_CALLBACK_ERROR ((int8_t)-1)

// The gid of the client a request came from, in rmw_get_gid_for_client()'s layout, so that a server's
// request_id.writer_guid compares equal to the gid the client's own process reports - or all zeros when that
// cannot be told. A CallRequest names its source context and the service, not the client instance
// (tt_CallRequestHeader carries the server's endpoint_id and a seq_no), so the client is found by what
// the request does say: the one client of this service in that context, among this context's own
// endpoints when the request is local and among the discovered ones otherwise. Two clients of one service
// in one context cannot be told apart from the request, and naming either would be a gid that is
// confidently wrong - the defect the topic side had until 2026-10-02 - so that case stays zero.
// Called with the node lock held (server_callback()), which is what guards both tables.
static void requester_gid_locked(const rmw_tickle_service_t* svc, uint8_t source, uint8_t gid[RMW_GID_STORAGE_SIZE]) {
    memset(gid, 0, RMW_GID_STORAGE_SIZE);
    rmw_tickle_context_impl_t* context_impl = svc->node->context_impl;
    const char* service_name = svc->tickle_server.endpoint.name;
    uint32_t matches = 0;
    uint32_t entity_id = 0;
    if (source == context_impl->tickle_context.id) {
        for (uint32_t i = 0; i < context_impl->tickle_context.endpoint_count; ++i) {
            const struct tt_Endpoint* endpoint = context_impl->tickle_context.endpoints[i];
            if (endpoint->kind == tt_KIND_SERVICE_CLIENT && strcmp(endpoint->name, service_name) == 0) {
                matches++;
                entity_id = endpoint->entity_id;
            }
        }
    } else {
        uint64_t now = tt_get_ns();
        for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
            const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
            if (entity->context_id == source && entity->kind == tt_KIND_SERVICE_CLIENT &&
                strcmp(entity->name, service_name) == 0 &&
                tt_Context_entity_alive(&context_impl->tickle_context, entity, now)) {
                matches++;
                entity_id = entity->entity_id;
            }
        }
    }
    if (1 == matches) {
        gid[0] = source;
        memcpy(&gid[1], &entity_id, sizeof(entity_id));
    }
}

// A request's sequence id: the core request id - the client's context and its seq_no, unique among the calls in flight
// - in the low 24 bits, under a per-service count of requests, so it still grows with every request as a sequence
// number does, and rmw_send_response() finds the request from it alone (unpack_request_id()). Positive for the first
// 2^39 requests of a service.
static int64_t pack_sequence_id(int64_t count, tt_RequestId request_id) {
    return (int64_t)(((uint64_t)count << 24U) | ((uint64_t)request_id.receiver << 16U) | request_id.seq_no);
}

static tt_RequestId unpack_request_id(int64_t sequence_id) {
    tt_RequestId request_id = {(uint8_t)(((uint64_t)sequence_id >> 16U) & UINT8_MAX),
                               (uint16_t)((uint64_t)sequence_id & UINT16_MAX)};
    return request_id;
}

// Runs on the poll thread, the node lock already held (rmw_tickle.h's own threading
// model). See this file's own module doc comment for the full deferred-response design.
static int8_t server_callback(struct tt_Server* tt_server, struct tt_Request* request, struct tt_Response* response,
                              tt_RequestId request_id) {
    (void)response; // deferred - nothing to fill in synchronously here, see module doc comment
    rmw_tickle_service_t* svc =
        (rmw_tickle_service_t*)((char*)tt_server - offsetof(rmw_tickle_service_t, tickle_server));

    pthread_mutex_lock(&svc->request_mutex);
    if (svc->request_count == tt_MAX_SERVER_CACHE_COUNT) {
        // Full - as many waiting as core can hold deferred. Turned away unanswered: the client's retry brings it
        // back once the executor has taken some (an error answer would end the client's call).
        pthread_mutex_unlock(&svc->request_mutex);
        return tt_CALL_IGNORED;
    }
    rmw_tickle_service_request_t* entry =
        &svc->requests[(svc->request_head + svc->request_count) % tt_MAX_SERVER_CACHE_COUNT];
    if (NULL == entry->ros_request) {
        entry->ros_request = rmw_tickle_ros_message_create(svc->request_callbacks, &svc->allocator);
    }
    // `request` aliases node->rx_buffer (DESIGN.md's "Strings" rule) - must convert to an
    // independently-owned ROS-shaped copy now, synchronously, before this callback returns - the
    // same reasoning as rmw_subscription.c's own subscriber_callback().
    if (NULL == entry->ros_request || !svc->request_callbacks->from_tickle(request, entry->ros_request)) {
        pthread_mutex_unlock(&svc->request_mutex);
        return RMW_TICKLE_SERVER_CALLBACK_ERROR;
    }
    entry->sequence_id = pack_sequence_id(++svc->next_sequence_id, request_id);
    requester_gid_locked(svc, request_id.receiver, entry->writer_guid);
    svc->request_count++;
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
    rmw_tickle_poke_polling_executor(context_impl);
    // (g2) An EventsExecutor's callback, after request_mutex is released, so it may take the request.
    rmw_tickle_callback_slot_notify(&svc->on_new_request, 1);

    return tt_CALL_DEFERRED;
}

// Every ROS request the queue created (each on its entry's first use).
static void destroy_requests(rmw_tickle_service_t* svc) {
    for (size_t i = 0; i < tt_MAX_SERVER_CACHE_COUNT; i++) {
        if (NULL != svc->requests[i].ros_request) {
            rmw_tickle_ros_message_destroy(svc->request_callbacks, svc->requests[i].ros_request, &svc->allocator);
            svc->requests[i].ros_request = NULL;
        }
    }
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
    if (!rmw_tickle_get_service_callbacks(type_support, "service", service_name, &callbacks)) {
        return NULL; // error message already set, and it names this endpoint
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
    svc->qos = *qos_policies;

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

    svc->response_storage = allocator->zero_allocate(1, callbacks.response->tickle_struct_size, allocator->state);
    if (NULL == svc->response_storage) {
        RMW_SET_ERROR_MSG("failed to allocate response storage");
        allocator->deallocate(svc, allocator->state);
        return NULL;
    }

    rmw_tickle_callback_slot_init(&svc->on_new_request); // (g2), before a request can arrive
    if (pthread_mutex_init(&svc->request_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize service request mutex");
        destroy_requests(svc);
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
        destroy_requests(svc);
        allocator->deallocate(svc->response_storage, allocator->state);
        allocator->deallocate(svc, allocator->state);
        return NULL;
    }

    // Core's cached responses for this server, sized for this service (stage (iv) of
    // the storage design the user approved on 2026-09-24): rmw_tickle builds core with a tiny inline
    // default (CMakeLists.txt), because the default sized for any message is 192 x
    // tt_MAX_BUFFER_LENGTH per server - 12.6 MB at 65507.
    //   - a cache entry holds one encoded response: this response type's largest, within an even
    //     share of RMW_TICKLE_CACHE_BYTES across the slots. A response larger than that is still
    //     sent, just not kept for a retry (tt_Server_set_storage(), tickle.h). At 1472 an entry is a
    //     whole datagram, as before.
    // A deferred response needs no storage: tt_Server_send_response() encodes it from the caller's struct.
    const size_t response_framing = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallResponseHeader);
    uint32_t cache_entry = rmw_tickle_message_slot_bytes(svc->response_callbacks, response_framing);
    unsigned long long share = rmw_tickle_cache_budget_bytes() / tt_MAX_SERVER_CACHE_COUNT;
    share -= share % RMW_TICKLE_ROUND_UP_8(1U); // whole 8-byte units
    unsigned long long smallest = RMW_TICKLE_ROUND_UP_8(response_framing);
    if (share < smallest) {
        share = smallest;
    }
    if (cache_entry > share) {
        cache_entry = (uint32_t)share;
    }
    svc->response_cache =
        (uint8_t*)allocator->allocate((size_t)tt_MAX_SERVER_CACHE_COUNT * cache_entry, allocator->state);
    if (NULL == svc->response_cache) {
        RMW_SET_ERROR_MSG("failed to allocate service response storage");
        allocator->deallocate(svc->response_cache, allocator->state);
        allocator->deallocate((char*)svc->rmw_service.service_name, allocator->state);
        pthread_mutex_destroy(&svc->request_mutex);
        destroy_requests(svc);
        allocator->deallocate(svc->response_storage, allocator->state);
        allocator->deallocate(svc, allocator->state);
        return NULL;
    }

    tt_Context_lock(&node_impl->context_impl->tickle_context);
    tt_ret_t ret = tt_Node_create_server_legacy(node_impl->core_node, &svc->tickle_server, &svc->service,
                                                svc->rmw_service.service_name, server_callback);
    if (ret == tt_RET_OK) {
        // After create, which resets the server to its inline storage, and under the same lock, so
        // the poll thread cannot hand it a request in between.
        ret = tt_Server_set_storage(&svc->tickle_server, svc->response_cache, cache_entry);
        if (ret != tt_RET_OK) {
            tt_Server_destroy(&svc->tickle_server);
        }
    }
    tt_Context_unlock(&node_impl->context_impl->tickle_context);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Context_create_server()/tt_Server_set_storage() failed");
        allocator->deallocate(svc->response_cache, allocator->state);
        allocator->deallocate((char*)svc->rmw_service.service_name, allocator->state);
        pthread_mutex_destroy(&svc->request_mutex);
        destroy_requests(svc);
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

    tt_Context_lock(&svc->node->context_impl->tickle_context);
    tt_Server_destroy(&svc->tickle_server);
    tt_Context_unlock(&svc->node->context_impl->tickle_context);

    pthread_mutex_destroy(&svc->request_mutex);
    rmw_tickle_callback_slot_fini(&svc->on_new_request);

    rcutils_allocator_t allocator = svc->allocator;
    allocator.deallocate((char*)svc->rmw_service.service_name, allocator.state);
    destroy_requests(svc);
    allocator.deallocate(svc->response_storage, allocator.state);
    allocator.deallocate(svc->response_cache, allocator.state); // after tt_Server_destroy() above
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
    if (0 == svc->request_count) {
        pthread_mutex_unlock(&svc->request_mutex);
        *taken = false;
        return RMW_RET_OK;
    }
    rmw_tickle_service_request_t* entry = &svc->requests[svc->request_head];
    // Same "ros_request assumed fresh/blank" caveat as rmw_subscription.c's own rmw_take_with_
    // info() - see its doc comment there.
    rmw_tickle_ros_message_move(svc->request_callbacks, ros_request, entry->ros_request);
    int64_t seq = entry->sequence_id;
    uint8_t writer_guid[RMW_GID_STORAGE_SIZE];
    memcpy(writer_guid, entry->writer_guid, sizeof(writer_guid));
    // Taken: the entry is free for the next request. Its answer needs nothing of it - rmw_send_response() finds the
    // request in core from the sequence id alone.
    svc->request_head = (svc->request_head + 1) % tt_MAX_SERVER_CACHE_COUNT;
    svc->request_count--;
    pthread_mutex_unlock(&svc->request_mutex);

    *taken = true;
    memset(request_header, 0, sizeof(*request_header));
    request_header->request_id.sequence_number = seq;
    // The requesting client's gid, as its own process reports it (requester_gid_locked()); zeros when the
    // request cannot say which client sent it. source_timestamp/received_timestamp stay zero: a CallRequest
    // carries no source time.
    memcpy(request_header->request_id.writer_guid, writer_guid, sizeof(writer_guid));
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

    // The request this answers, from its sequence id alone (pack_sequence_id()). One that core no longer holds -
    // answered already, timed out at tt_SERVER_DEFERRED_RESPONSE_TIMEOUT, or never this service's - is not found by
    // tt_Server_send_response() below.
    if (request_header->sequence_number <= 0) {
        RMW_SET_ERROR_MSG("no matching in-flight request for this response");
        return RMW_RET_ERROR;
    }
    tt_RequestId request_id = unpack_request_id(request_header->sequence_number);

    // response_storage is a scratch conversion buffer, not synchronized by request_mutex - see
    // rmw_tickle.h's own rmw_tickle_service_t doc comment on why that's fine (only this function
    // ever touches it, and rmw's own contract doesn't promise concurrent rmw_send_response() calls
    // on the same service are safe to begin with).
    if (!svc->response_callbacks->to_tickle(ros_response, svc->response_storage)) {
        RMW_SET_ERROR_MSG("failed to convert ROS response to TickLE wire struct (capacity exceeded?)");
        return RMW_RET_ERROR;
    }

    // Encodes and sends the answer before it returns - see tt_Server_send_response()'s own doc comment (tickle.h).
    // tt_RET_NOT_FOUND: the request is no longer held (see above), reported as RMW_RET_ERROR.
    tt_ret_t ret =
        tt_Server_send_response(&svc->tickle_server, request_id, 0, (struct tt_Response*)svc->response_storage);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Server_send_response() found no matching in-flight request - it may have timed out");
        return RMW_RET_ERROR;
    }
    return RMW_RET_OK;
}

// rcl_service_init() asks for both on every service (rcl service.c), so a service could not be
// created through rcl at all without them - every default rclcpp::Node starts several. TickLE
// has no request reader or response writer of its own to report on: both answer with the profile
// the service was created with (rmw_tickle_service_t.qos).
static rmw_ret_t service_actual_qos(const rmw_service_t* service, rmw_qos_profile_t* qos) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(service->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    *qos = ((const rmw_tickle_service_t*)service->data)->qos;
    return RMW_RET_OK;
}

rmw_ret_t rmw_service_request_subscription_get_actual_qos(const rmw_service_t* service, rmw_qos_profile_t* qos) {
    return service_actual_qos(service, qos);
}

rmw_ret_t rmw_service_response_publisher_get_actual_qos(const rmw_service_t* service, rmw_qos_profile_t* qos) {
    return service_actual_qos(service, qos);
}

static size_t request_waiting(const void* entity) {
    rmw_tickle_service_t* svc = (rmw_tickle_service_t*)entity;
    pthread_mutex_lock(&svc->request_mutex);
    size_t waiting = svc->request_count;
    pthread_mutex_unlock(&svc->request_mutex);
    return waiting;
}

// (g2, RMW_GAPS_PLAN.md) rclcpp's and rclpy's EventsExecutor: called with 1 per request; set while some are waiting,
// called once with how many.
rmw_ret_t rmw_service_set_on_new_request_callback(rmw_service_t* service, rmw_event_callback_t callback,
                                                  const void* user_data) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(service->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    rmw_tickle_service_t* svc = (rmw_tickle_service_t*)service->data;
    rmw_tickle_callback_slot_set(&svc->on_new_request, callback, user_data, request_waiting, svc);
    return RMW_RET_OK;
}
