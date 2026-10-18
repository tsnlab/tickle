/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 4: rmw_create_client()/rmw_destroy_client()/rmw_send_request()/
// rmw_take_response(). Unlike the server side (rmw_service.c), this direction needs no bridging
// trick: TickLE's own tt_CLIENT_CALLBACK is already exactly the async "call completed, here's the
// answer (or none)" notification rmw_send_request()/rmw_take_response()'s own two-call split
// expects - it just stores the result for rmw_take_response() to poll, the same pattern rmw_
// subscription.c's subscriber_callback()/rmw_take() already use for topics.
//
// Several requests in flight (2026-10-16): core's tt_Client has tt_CLIENT_MAX_OUTSTANDING call slots, each with its
// own seq_no, and the callback names the call it reports (tt_Client.callback_seq_no). rmw_send_request() takes a slot
// and records which rmw sequence id that seq_no answers to; client_callback() files each response under its own
// sequence id, in arrival order, for rmw_take_response(). Until then a client had one call: rclcpp's
// async_send_request() twice in a row, or an action client's get_result for a second goal, failed.
//
// When every slot is in flight - or core refuses a call because one outstanding is tt_CLIENT_CALL_WINDOW calls old, its
// answer lost and its retry on the way (config.h) - rmw_send_request() waits up to RMW_TICKLE_CLIENT_SEND_BLOCK_NS
// (100 ms) for a call to end and then returns RMW_RET_ERROR - DDS's own answer to a writer whose resources are full
// (RELIABILITY max_blocking_time, 100 ms by default, then RETCODE_TIMEOUT, which rmw_fastrtps and rmw_cyclonedds
// report as an error). Failing at once
// would make rclcpp throw on a burst that a slot freed a microsecond later would have carried; waiting without a bound
// would hang a caller whose server is gone. The wait never depends on the caller's executor: a slot frees when the
// response arrives on the poll thread, not when it is taken. (DDS with its default KEEP_LAST service QoS never refuses
// at all - it has no bound on calls in flight; this one is the user's decision of 2026-10-16, a compile-time constant.)

#include <errno.h> // ETIMEDOUT
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h> // clock_gettime(), CLOCK_MONOTONIC

#include <tickle/config.h> // tt_SERVER_DEFERRED_RESPONSE_TIMEOUT
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

// Files `response` (TickLE's, aliasing the receive buffer) as the answer to `sequence_id`, converted now. The queue
// is a ring of response_capacity, KEEP_LAST: when it is full the oldest response untaken goes, as a DDS response
// reader of that depth drops it. response_mutex held. False if the conversion failed (nothing filed).
static bool file_response(rmw_tickle_client_t* client_impl, int64_t sequence_id, struct tt_Response* response) {
    if (client_impl->response_count == client_impl->response_capacity) {
        client_impl->response_head = (client_impl->response_head + 1) % client_impl->response_capacity;
        client_impl->response_count--;
        client_impl->responses_dropped++;
    }
    size_t index = (client_impl->response_head + client_impl->response_count) % client_impl->response_capacity;
    rmw_tickle_client_response_t* entry = &client_impl->responses[index];
    if (NULL == entry->ros_response) {
        entry->ros_response = rmw_tickle_ros_message_create(client_impl->response_callbacks, &client_impl->allocator);
        if (NULL == entry->ros_response) {
            return false;
        }
    }
    if (!client_impl->response_callbacks->from_tickle(response, entry->ros_response)) {
        return false;
    }
    entry->sequence_id = sequence_id;
    client_impl->response_count++;
    return true;
}

// Runs on the poll thread, inside tt_Context_poll() (the node lock already held) - see
// rmw_subscription.c's own subscriber_callback() for the identical "convert now, not later"
// reasoning (TickLE's response here aliases node->rx_buffer the same way a received topic message
// does). No waiting/signaling needed here, unlike server_callback() (rmw_service.c) - a client
// only ever *reads* the result later via rmw_take_response(), it never has to send anything back
// through this same callback.
static void client_callback(struct tt_Client* tt_client, int8_t return_code, struct tt_Response* response) {
    rmw_tickle_client_t* client_impl =
        (rmw_tickle_client_t*)((char*)tt_client - offsetof(rmw_tickle_client_t, tickle_client));

    pthread_mutex_lock(&client_impl->response_mutex);
    // The call this reports, by the seq_no core names (tt_Client.callback_seq_no): its slot is free again, whatever
    // the outcome, and whoever waits in rmw_send_request() for one may go.
    int64_t sequence_id = -1;
    for (uint32_t i = 0; i < tt_CLIENT_MAX_OUTSTANDING; i++) {
        rmw_tickle_client_call_t* call = &client_impl->calls[i];
        if (call->used && call->core_seq_no == tt_client->callback_seq_no) {
            sequence_id = call->sequence_id;
            call->used = false;
            client_impl->calls_in_flight--;
            pthread_cond_signal(&client_impl->slot_cond);
            break;
        }
    }
    bool filed = false;
    if (sequence_id >= 0 && return_code != tt_CALL_TIMEOUT && NULL != response) {
        filed = file_response(client_impl, sequence_id, response);
    }
    // A call that timed out (tt_CALL_TIMEOUT), or that the server answered with an error, files nothing: rmw has no
    // "failed" response to hand out, and nothing is taken for it, as before.
    pthread_mutex_unlock(&client_impl->response_mutex);
    if (!filed) {
        return;
    }

    // Wake anyone blocked in rmw_wait() on this client's response - see rmw_tickle_context_impl_t's
    // own doc comment (rmw_tickle.h) for why the broadcast must happen under wait_mutex even though
    // the queue itself is guarded by the separate response_mutex above.
    rmw_tickle_context_impl_t* context_impl = client_impl->node->context_impl;
    pthread_mutex_lock(&context_impl->wait_mutex);
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);
    rmw_tickle_poke_polling_executor(context_impl);
    // (g2) An EventsExecutor's callback, after response_mutex is released, so it may take the response.
    rmw_tickle_callback_slot_notify(&client_impl->on_new_response, 1);
}

// slot_cond waits on CLOCK_MONOTONIC: a wall-clock step must not stretch or cut rmw_send_request()'s bounded wait.
static bool init_slot_cond(rmw_tickle_client_t* client_impl) {
    pthread_condattr_t attr; // NOLINT(misc-include-cleaner) - <pthread.h> above
    if (pthread_condattr_init(&attr) != 0) {
        return false;
    }
    bool done = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 && // NOLINT(misc-include-cleaner) - <time.h>
                pthread_cond_init(&client_impl->slot_cond, &attr) == 0;
    pthread_condattr_destroy(&attr);
    return done;
}

// The mutex, the condition and the response queue with every ROS message it created, once nothing can reach them.
static void destroy_queue(rmw_tickle_client_t* client_impl) {
    pthread_cond_destroy(&client_impl->slot_cond);
    pthread_mutex_destroy(&client_impl->response_mutex);
    for (size_t i = 0; i < client_impl->response_capacity; i++) {
        if (NULL != client_impl->responses[i].ros_response) {
            rmw_tickle_ros_message_destroy(client_impl->response_callbacks, client_impl->responses[i].ros_response,
                                           &client_impl->allocator);
        }
    }
    client_impl->allocator.deallocate(client_impl->responses, client_impl->allocator.state);
    client_impl->responses = NULL;
}

rmw_client_t* rmw_create_client(const rmw_node_t* node, const rosidl_service_type_support_t* type_support,
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
    // See rmw_service.c's own rmw_create_service() for why this is needed - the identical gap,
    // found by the same pass (Milestone 16) via test_client.cpp's own create_with_bad_arguments.
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
    if (!rmw_tickle_get_service_callbacks(type_support, "client", service_name, &callbacks)) {
        return NULL; // error message already set, and it names this endpoint
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rcutils_allocator_t* allocator = &node_impl->allocator;

    rmw_tickle_client_t* client_impl =
        (rmw_tickle_client_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_client_t), allocator->state);
    if (NULL == client_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_client_t");
        return NULL;
    }
    client_impl->node = node_impl;
    // Milestone 34 - see rmw_tickle_publisher_t.owning_node_name's own doc comment.
    client_impl->owning_node_name = rcutils_strdup(node_impl->rmw_node.name, *allocator);
    client_impl->owning_node_namespace = rcutils_strdup(node_impl->rmw_node.namespace_, *allocator);
    client_impl->type_support = type_support;
    client_impl->request_callbacks = callbacks.request;
    client_impl->response_callbacks = callbacks.response;
    client_impl->allocator = *allocator;
    client_impl->qos = *qos_policies;

    client_impl->service.name = callbacks.service->ros_type_name; // see rmw_tickle_client_t.service's own doc comment
    client_impl->service.request_size = (uint32_t)callbacks.request->tickle_struct_size;
    client_impl->service.response_size = (uint32_t)callbacks.response->tickle_struct_size;
    client_impl->service.request_encode_size = (tt_REQUEST_ENCODE_SIZE)callbacks.request->tickle_encode_size;
    client_impl->service.request_encode = (tt_REQUEST_ENCODE)callbacks.request->tickle_encode;
    client_impl->service.request_decode = (tt_REQUEST_DECODE)callbacks.request->tickle_decode;
    client_impl->service.request_free = (tt_REQUEST_FREE)callbacks.request->tickle_free;
    client_impl->service.response_encode_size = (tt_RESPONSE_ENCODE_SIZE)callbacks.response->tickle_encode_size;
    client_impl->service.response_encode = (tt_RESPONSE_ENCODE)callbacks.response->tickle_encode;
    client_impl->service.response_decode = (tt_RESPONSE_DECODE)callbacks.response->tickle_decode;
    client_impl->service.response_free = (tt_RESPONSE_FREE)callbacks.response->tickle_free;
    // How long a call is kept alive, retrying, before core gives up on it. ROS 2 has no rmw-level
    // service timeout: rmw_send_request() promises a response whenever the service sends one, and a
    // timeout is the application's business (rclcpp's future). And the server always answers through
    // tt_Server_send_response() - deferred until the ROS executor has taken the request and run the
    // callback - so the answer can take as long as that callback does. Core's defaults (TickLE's own
    // synchronous services: a few retries from srtt, each twice the last, all within 1 s) would abandon
    // any call slower than that, and the late response would be ignored.
    //
    // So a call lives as long as the server keeps it: retried every RMW_TICKLE_CLIENT_RETRY_INTERVAL_NS
    // until tt_SERVER_DEFERRED_RESPONSE_TIMEOUT, after which the server has dropped it too. A retry of
    // a request still pending on the server does not re-run its callback (find_pending_slot(),
    // tickle.c); once answered, a retry gets the cached response.
    client_impl->service.call_retry_interval = RMW_TICKLE_CLIENT_RETRY_INTERVAL_NS;
    client_impl->service.call_retry_count =
        (uint32_t)(tt_SERVER_DEFERRED_RESPONSE_TIMEOUT / RMW_TICKLE_CLIENT_RETRY_INTERVAL_NS);

    // Responses kept untaken: the QoS depth, as a DDS response reader keeps (KEEP_LAST), never fewer than the calls
    // that can be in flight; RMW_TICKLE_CLIENT_RESPONSE_DEFAULT_DEPTH when the profile gives no depth.
    size_t depth = RMW_QOS_POLICY_HISTORY_KEEP_ALL != qos_policies->history && 0 != qos_policies->depth
                       ? qos_policies->depth
                       : RMW_TICKLE_CLIENT_RESPONSE_DEFAULT_DEPTH;
    client_impl->response_capacity = depth < tt_CLIENT_MAX_OUTSTANDING ? tt_CLIENT_MAX_OUTSTANDING : depth;
    client_impl->responses = (rmw_tickle_client_response_t*)allocator->zero_allocate(
        client_impl->response_capacity, sizeof(rmw_tickle_client_response_t), allocator->state);
    if (NULL == client_impl->responses) {
        RMW_SET_ERROR_MSG("failed to allocate the response queue");
        allocator->deallocate(client_impl, allocator->state);
        return NULL;
    }

    rmw_tickle_callback_slot_init(&client_impl->on_new_response); // (g2), before a response can arrive
    if (pthread_mutex_init(&client_impl->response_mutex, NULL) != 0 || !init_slot_cond(client_impl)) {
        RMW_SET_ERROR_MSG("failed to initialize client response mutex");
        allocator->deallocate(client_impl->responses, allocator->state);
        allocator->deallocate(client_impl, allocator->state);
        return NULL;
    }

    client_impl->rmw_client.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    client_impl->rmw_client.data = client_impl;
    client_impl->rmw_client.service_name = rcutils_strdup(service_name, *allocator);
    if (NULL == client_impl->rmw_client.service_name) {
        RMW_SET_ERROR_MSG("failed to allocate service_name");
        destroy_queue(client_impl);
        allocator->deallocate(client_impl, allocator->state);
        return NULL;
    }

    // Where core keeps the outstanding requests for a retry: one slot per call that can be in flight, each this
    // service's largest request (stage (iv) of the storage design the user approved on 2026-09-24; core's inline one
    // is built tiny - CMakeLists.txt). Not budgeted, unlike a server's cache: a request that does not fit is refused
    // by tt_Client_call(), so each slot has to hold the largest one the type allows - a bounded type's own size, and
    // a datagram (65,512 bytes) for an unbounded one. Core splits the area evenly (tt_Client_set_storage()).
    uint32_t request_cache_bytes =
        tt_CLIENT_MAX_OUTSTANDING *
        rmw_tickle_message_slot_bytes(client_impl->request_callbacks,
                                      sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallRequestHeader));
    client_impl->request_cache = (uint8_t*)allocator->allocate(request_cache_bytes, allocator->state);
    if (NULL == client_impl->request_cache) {
        RMW_SET_ERROR_MSG("failed to allocate client request storage");
        allocator->deallocate((char*)client_impl->rmw_client.service_name, allocator->state);
        destroy_queue(client_impl);
        allocator->deallocate(client_impl, allocator->state);
        return NULL;
    }

    // Created with its QoS, its options and that storage (QoS redesign step 3): the retry settings above become this
    // client's own (tt_ClientOptions, no longer read from the tt_Service), and core lays the outstanding requests out
    // in request_cache exactly as tt_Client_set_storage() did - tt_CLIENT_MAX_OUTSTANDING slots of max_request_bytes
    // plus framing, which is the slot computed above.
    struct tt_QosProfile profile;
    rmw_tickle_qos_to_tt(qos_policies, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT, NULL, &profile);
    struct tt_ClientOptions options;
    memset(&options, 0, sizeof(options));
    options.callback = client_callback;
    options.call_retry_interval = client_impl->service.call_retry_interval;
    options.call_retry_count = client_impl->service.call_retry_count;
    options.max_request_bytes = (request_cache_bytes / tt_CLIENT_MAX_OUTSTANDING) -
                                (uint32_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallRequestHeader));
    size_t storage_size = tt_Client_storage_size(&options);

    tt_Context_lock(&node_impl->context_impl->tickle_context);
    tt_ret_t ret = tt_RET_INVALID_ARGUMENT;
    if (storage_size <= request_cache_bytes) {
        ret = tt_Node_create_client(node_impl->core_node, &client_impl->tickle_client, &client_impl->service,
                                    client_impl->rmw_client.service_name, &profile, &options,
                                    client_impl->request_cache, storage_size);
    }
    tt_Context_unlock(&node_impl->context_impl->tickle_context);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("tt_Node_create_client() failed (%d) for service '%s'", (int)ret,
                                             service_name);
        allocator->deallocate(client_impl->request_cache, allocator->state);
        allocator->deallocate((char*)client_impl->rmw_client.service_name, allocator->state);
        destroy_queue(client_impl);
        allocator->deallocate(client_impl, allocator->state);
        return NULL;
    }

    return &client_impl->rmw_client;
}

rmw_ret_t rmw_destroy_client(rmw_node_t* node, rmw_client_t* client) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier) ||
        !rmw_tickle_identifier_matches(client->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)client->data;

    tt_Context_lock(&client_impl->node->context_impl->tickle_context);
    tt_Client_destroy(&client_impl->tickle_client);
    tt_Context_unlock(&client_impl->node->context_impl->tickle_context);

    destroy_queue(client_impl);
    rmw_tickle_callback_slot_fini(&client_impl->on_new_response);

    rcutils_allocator_t allocator = client_impl->allocator;
    allocator.deallocate((char*)client_impl->rmw_client.service_name, allocator.state);
    allocator.deallocate(client_impl->request_cache, allocator.state); // after tt_Client_destroy() above
    allocator.deallocate(client_impl->owning_node_name, allocator.state);
    allocator.deallocate(client_impl->owning_node_namespace, allocator.state);
    allocator.deallocate(client_impl, allocator.state);
    return RMW_RET_OK;
}

// When rmw_send_request() stops waiting for a call slot: RMW_TICKLE_CLIENT_SEND_BLOCK_NS from now, CLOCK_MONOTONIC.
static struct timespec slot_deadline(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline); // NOLINT(misc-include-cleaner) - <time.h>
    uint64_t nsec = (uint64_t)deadline.tv_nsec + RMW_TICKLE_CLIENT_SEND_BLOCK_NS;
    deadline.tv_sec += (time_t)(nsec / tt_SECOND);
    deadline.tv_nsec = (long)(nsec % tt_SECOND);
    return deadline;
}

// Waits on slot_cond for a call to end. response_mutex held. False once the deadline has passed.
static bool wait_for_a_call_to_end(rmw_tickle_client_t* client_impl, const struct timespec* deadline) {
    return ETIMEDOUT != pthread_cond_timedwait(&client_impl->slot_cond, &client_impl->response_mutex, deadline);
}

// Takes a call slot, waiting while all are in flight, until `deadline`. response_mutex held. False if none came free.
static bool reserve_slot_locked(rmw_tickle_client_t* client_impl, const struct timespec* deadline) {
    while (client_impl->calls_in_flight >= tt_CLIENT_MAX_OUTSTANDING) {
        if (!wait_for_a_call_to_end(client_impl, deadline)) {
            break;
        }
    }
    if (client_impl->calls_in_flight >= tt_CLIENT_MAX_OUTSTANDING) {
        return false;
    }
    client_impl->calls_in_flight++;
    return true;
}

// Hands `tickle_buf` to core as a new call in a slot already reserved, and records it under a new sequence id
// (*sequence_id), under the context lock, which client_callback() runs under too - so it cannot report the call before
// the record exists. On failure the reservation goes back.
static tt_ret_t call_in_reserved_slot(rmw_tickle_client_t* client_impl, void* tickle_buf, int64_t* sequence_id) {
    tt_Context_lock(&client_impl->node->context_impl->tickle_context);
    uint16_t core_seq_no = 0;
    tt_ret_t ret = tt_Client_call_id(&client_impl->tickle_client, (struct tt_Request*)tickle_buf, &core_seq_no);
    pthread_mutex_lock(&client_impl->response_mutex);
    if (ret == tt_RET_OK) {
        *sequence_id = ++client_impl->next_sequence_id;
        for (uint32_t i = 0; i < tt_CLIENT_MAX_OUTSTANDING; i++) {
            if (!client_impl->calls[i].used) {
                client_impl->calls[i] = (rmw_tickle_client_call_t) {true, core_seq_no, *sequence_id};
                break;
            }
        }
    } else {
        client_impl->calls_in_flight--;
        pthread_cond_signal(&client_impl->slot_cond);
    }
    pthread_mutex_unlock(&client_impl->response_mutex);
    tt_Context_unlock(&client_impl->node->context_impl->tickle_context);
    return ret;
}

// Makes the call, waiting until RMW_TICKLE_CLIENT_SEND_BLOCK_NS from now for core to take it: for a free slot, and -
// tt_RET_ILLEGAL_STATUS with a slot free - for the client's oldest outstanding call to end, which core asks for when
// that call is tt_CLIENT_CALL_WINDOW calls old (config.h), so the server cannot have let its answer go.
static tt_ret_t call_waiting_for_room(rmw_tickle_client_t* client_impl, void* tickle_buf, int64_t* sequence_id) {
    struct timespec deadline = slot_deadline();
    while (true) {
        pthread_mutex_lock(&client_impl->response_mutex);
        bool reserved = reserve_slot_locked(client_impl, &deadline);
        pthread_mutex_unlock(&client_impl->response_mutex);
        if (!reserved) {
            return tt_RET_ILLEGAL_STATUS;
        }
        tt_ret_t ret = call_in_reserved_slot(client_impl, tickle_buf, sequence_id);
        if (ret != tt_RET_ILLEGAL_STATUS) {
            return ret;
        }
        pthread_mutex_lock(&client_impl->response_mutex);
        bool ended = wait_for_a_call_to_end(client_impl, &deadline);
        pthread_mutex_unlock(&client_impl->response_mutex);
        if (!ended) {
            return tt_RET_ILLEGAL_STATUS;
        }
    }
}

rmw_ret_t rmw_send_request(const rmw_client_t* client, const void* ros_request, int64_t* sequence_id) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(sequence_id, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(client->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)client->data;
    const rosidl_typesupport_tickle_c_message_callbacks_t* request_callbacks = client_impl->request_callbacks;

    void* tickle_buf =
        client_impl->allocator.allocate(request_callbacks->tickle_struct_size, client_impl->allocator.state);
    if (NULL == tickle_buf) {
        RMW_SET_ERROR_MSG("failed to allocate scratch TickLE request struct");
        return RMW_RET_BAD_ALLOC;
    }
    if (!request_callbacks->to_tickle(ros_request, tickle_buf)) {
        RMW_SET_ERROR_MSG("failed to convert ROS request to TickLE wire struct (capacity exceeded?)");
        client_impl->allocator.deallocate(tickle_buf, client_impl->allocator.state);
        return RMW_RET_ERROR;
    }

    // A call slot, waiting up to RMW_TICKLE_CLIENT_SEND_BLOCK_NS for one (this file's module comment).
    int64_t seq = 0;
    tt_ret_t ret = call_waiting_for_room(client_impl, tickle_buf, &seq);
    client_impl->allocator.deallocate(tickle_buf, client_impl->allocator.state);
    if (ret == tt_RET_ILLEGAL_STATUS) {
        RMW_SET_ERROR_MSG("this client's calls (tt_CLIENT_MAX_OUTSTANDING) stayed in flight for 100 ms");
        return RMW_RET_ERROR;
    }
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Client_call() failed");
        return RMW_RET_ERROR;
    }
    *sequence_id = seq;
    return RMW_RET_OK;
}

rmw_ret_t rmw_take_response(const rmw_client_t* client, rmw_service_info_t* request_header, void* ros_response,
                            bool* taken) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(request_header, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_response, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(client->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)client->data;

    pthread_mutex_lock(&client_impl->response_mutex);
    if (0 == client_impl->response_count) {
        pthread_mutex_unlock(&client_impl->response_mutex);
        *taken = false;
        return RMW_RET_OK;
    }
    // The oldest response filed, under its own call's sequence id - which may not be the oldest call: responses come
    // in the order they arrive.
    rmw_tickle_client_response_t* entry = &client_impl->responses[client_impl->response_head];
    int64_t seq = entry->sequence_id;
    // Same "ros_response assumed fresh/blank" caveat as rmw_subscription.c's own rmw_take_
    // with_info() - see its doc comment there.
    rmw_tickle_ros_message_move(client_impl->response_callbacks, ros_response, entry->ros_response);
    client_impl->response_head = (client_impl->response_head + 1) % client_impl->response_capacity;
    client_impl->response_count--;
    pthread_mutex_unlock(&client_impl->response_mutex);

    *taken = true;
    memset(request_header, 0, sizeof(*request_header));
    request_header->request_id.sequence_number = seq;
    // A request id names the request, and this client wrote it: its writer_guid is this client's own gid, the
    // same one the server side reports for the request (rmw_service.c, rmw_take_request()), as DDS rmws do.
    // source_timestamp/received_timestamp stay zero: a CallResponse carries no source time.
    rmw_gid_t own_gid;
    if (RMW_RET_OK == rmw_get_gid_for_client(client, &own_gid)) {
        memcpy(request_header->request_id.writer_guid, own_gid.data, sizeof(request_header->request_id.writer_guid));
    }
    return RMW_RET_OK;
}

// Milestone 51 (rmw_tickle/PLAN.md) - was previously entirely missing (an unresolved dlsym, fatal
// to any real caller), found by real CI once TestUniqueIdentifierAPI's own stale RELIABLE-based
// skip was removed and this fixture actually ran for the first time. Mirrors rmw_publisher.c's own
// rmw_get_gid_for_publisher() exactly - see its doc comment for the full (node_id, entity_id)
// reasoning (Milestone 47's own per-instance identity fix, not just a name hash).
rmw_ret_t rmw_get_gid_for_client(const rmw_client_t* client, rmw_gid_t* gid) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(client->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)client->data;
    memset(gid, 0, sizeof(*gid));
    gid->implementation_identifier = RMW_TICKLE_IDENTIFIER;
    uint8_t node_id = client_impl->node->context_impl->tickle_context.id;
    uint32_t entity_id = client_impl->tickle_client.endpoint.entity_id;
    gid->data[0] = node_id;
    memcpy(&gid->data[1], &entity_id, sizeof(entity_id));
    return RMW_RET_OK;
}

// The client-side pair rcl_client_init() asks for - see rmw_service.c's service_actual_qos().
static rmw_ret_t client_actual_qos(const rmw_client_t* client, rmw_qos_profile_t* qos) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(client->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    const rmw_tickle_client_t* client_impl = (const rmw_tickle_client_t*)client->data;
    struct tt_QosProfile actual;
    if (tt_RET_OK != tt_Client_get_actual_qos(&client_impl->tickle_client, &actual)) {
        RMW_SET_ERROR_MSG("tt_Client_get_actual_qos() failed");
        return RMW_RET_ERROR;
    }
    rmw_tickle_qos_from_tt(&actual, &client_impl->qos, qos);
    return RMW_RET_OK;
}

rmw_ret_t rmw_client_request_publisher_get_actual_qos(const rmw_client_t* client, rmw_qos_profile_t* qos) {
    return client_actual_qos(client, qos);
}

rmw_ret_t rmw_client_response_subscription_get_actual_qos(const rmw_client_t* client, rmw_qos_profile_t* qos) {
    return client_actual_qos(client, qos);
}

static size_t response_waiting(const void* entity) {
    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)entity;
    pthread_mutex_lock(&client_impl->response_mutex);
    size_t waiting = client_impl->response_count;
    pthread_mutex_unlock(&client_impl->response_mutex);
    return waiting;
}

// (g2, RMW_GAPS_PLAN.md) rclcpp's and rclpy's EventsExecutor: called with 1 per response; set while some are waiting,
// called once with how many.
rmw_ret_t rmw_client_set_on_new_response_callback(rmw_client_t* client, rmw_event_callback_t callback,
                                                  const void* user_data) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(client->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)client->data;
    rmw_tickle_callback_slot_set(&client_impl->on_new_response, callback, user_data, response_waiting, client_impl);
    return RMW_RET_OK;
}
