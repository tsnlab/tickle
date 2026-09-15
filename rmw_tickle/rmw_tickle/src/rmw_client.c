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
// TickLE's own tt_Client supports exactly *one* outstanding call at a time (tt_Client_call()
// refuses a second call while one is already pending - see tickle.h's own tt_Client doc comment);
// rmw_tickle mirrors that limit directly rather than queueing several in-flight requests - a real,
// documented gap versus full ROS 2 rmw semantics (which allow a client several outstanding
// requests, matched back up by sequence number) - see rmw_tickle/PLAN.md.

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
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

// Runs on the node's poll thread, inside tt_Node_poll() (node->mutex already held) - see
// rmw_subscription.c's own subscriber_callback() for the identical "convert now, not later"
// reasoning (TickLE's response here aliases node->rx_buffer the same way a received topic message
// does). No waiting/signaling needed here, unlike server_callback() (rmw_service.c) - a client
// only ever *reads* the result later via rmw_take_response(), it never has to send anything back
// through this same callback.
static void client_callback(struct tt_Client* tt_client, int8_t return_code, struct tt_Response* response) {
    rmw_tickle_client_t* client_impl =
        (rmw_tickle_client_t*)((char*)tt_client - offsetof(rmw_tickle_client_t, tickle_client));

    pthread_mutex_lock(&client_impl->response_mutex);
    if (return_code == tt_CALL_TIMEOUT || NULL == response) {
        client_impl->response_success = false;
    } else {
        client_impl->response_success =
            client_impl->response_callbacks->from_tickle(response, client_impl->response_storage);
    }
    client_impl->response_ready = true;
    pthread_mutex_unlock(&client_impl->response_mutex);

    // Wake anyone blocked in rmw_wait() on this client's response - see rmw_tickle_context_impl_t's
    // own doc comment (rmw_tickle.h) for why the broadcast must happen under wait_mutex even though
    // response_ready itself is guarded by the separate response_mutex above.
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)client_impl->node->context->impl;
    pthread_mutex_lock(&context_impl->wait_mutex);
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);
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
    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
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

    rmw_tickle_client_t* client_impl =
        (rmw_tickle_client_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_client_t), allocator->state);
    if (NULL == client_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_client_t");
        return NULL;
    }
    client_impl->node = node_impl;
    client_impl->type_support = type_support;
    client_impl->request_callbacks = callbacks.request;
    client_impl->response_callbacks = callbacks.response;
    client_impl->allocator = *allocator;

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
    // call_retry_interval/call_retry_count left 0 (zero_allocate) - "0 means auto"/"0 means
    // tt_CALL_RETRY_COUNT" (tickle.h's own struct tt_Service doc comment), i.e. TickLE's defaults.

    client_impl->response_storage = allocator->zero_allocate(1, callbacks.response->ros_struct_size, allocator->state);
    if (NULL == client_impl->response_storage) {
        RMW_SET_ERROR_MSG("failed to allocate response storage");
        allocator->deallocate(client_impl, allocator->state);
        return NULL;
    }

    if (pthread_mutex_init(&client_impl->response_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize client response mutex");
        allocator->deallocate(client_impl->response_storage, allocator->state);
        allocator->deallocate(client_impl, allocator->state);
        return NULL;
    }

    client_impl->rmw_client.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    client_impl->rmw_client.data = client_impl;
    client_impl->rmw_client.service_name = rcutils_strdup(service_name, *allocator);
    if (NULL == client_impl->rmw_client.service_name) {
        RMW_SET_ERROR_MSG("failed to allocate service_name");
        pthread_mutex_destroy(&client_impl->response_mutex);
        allocator->deallocate(client_impl->response_storage, allocator->state);
        allocator->deallocate(client_impl, allocator->state);
        return NULL;
    }

    // Same tt_Node_interrupt()-then-lock pattern rmw_create_publisher()/_subscription() already
    // established - see rmw_tickle.h's own rmw_tickle_node_t doc comment.
    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_mutex_lock(&node_impl->mutex);
    tt_ret_t ret = tt_Node_create_client(&node_impl->tickle_node, &client_impl->tickle_client, &client_impl->service,
                                         client_impl->rmw_client.service_name, client_callback);
    pthread_mutex_unlock(&node_impl->mutex);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create_client() failed");
        allocator->deallocate((char*)client_impl->rmw_client.service_name, allocator->state);
        pthread_mutex_destroy(&client_impl->response_mutex);
        allocator->deallocate(client_impl->response_storage, allocator->state);
        allocator->deallocate(client_impl, allocator->state);
        return NULL;
    }

    return &client_impl->rmw_client;
}

rmw_ret_t rmw_destroy_client(rmw_node_t* node, rmw_client_t* client) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0 ||
        strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)client->data;

    tt_Node_interrupt(&client_impl->node->tickle_node);
    pthread_mutex_lock(&client_impl->node->mutex);
    tt_Client_destroy(&client_impl->tickle_client);
    pthread_mutex_unlock(&client_impl->node->mutex);

    pthread_mutex_destroy(&client_impl->response_mutex);

    rcutils_allocator_t allocator = client_impl->allocator;
    allocator.deallocate((char*)client_impl->rmw_client.service_name, allocator.state);
    allocator.deallocate(client_impl->response_storage, allocator.state);
    allocator.deallocate(client_impl, allocator.state);
    return RMW_RET_OK;
}

rmw_ret_t rmw_send_request(const rmw_client_t* client, const void* ros_request, int64_t* sequence_id) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(sequence_id, RMW_RET_INVALID_ARGUMENT);
    if (strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
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

    pthread_mutex_lock(&client_impl->response_mutex);
    client_impl->response_ready = false;
    int64_t seq = ++client_impl->next_sequence_id;
    client_impl->response_sequence_id = seq;
    pthread_mutex_unlock(&client_impl->response_mutex);

    tt_Node_interrupt(&client_impl->node->tickle_node);
    pthread_mutex_lock(&client_impl->node->mutex);
    tt_ret_t ret = tt_Client_call(&client_impl->tickle_client, (struct tt_Request*)tickle_buf);
    pthread_mutex_unlock(&client_impl->node->mutex);
    client_impl->allocator.deallocate(tickle_buf, client_impl->allocator.state);

    if (ret != tt_RET_OK) {
        // Most likely tt_RET_INVALID_ARGUMENT-equivalent for a call already pending - see this
        // file's own module doc comment on the single-outstanding-call limit.
        RMW_SET_ERROR_MSG("tt_Client_call() failed - a call may already be pending");
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
    if (strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)client->data;

    pthread_mutex_lock(&client_impl->response_mutex);
    if (!client_impl->response_ready) {
        pthread_mutex_unlock(&client_impl->response_mutex);
        *taken = false;
        return RMW_RET_OK;
    }
    bool success = client_impl->response_success;
    int64_t seq = client_impl->response_sequence_id;
    if (success) {
        // Same "ros_response assumed fresh/blank" caveat as rmw_subscription.c's own rmw_take_
        // with_info() - see its doc comment there.
        memcpy(ros_response, client_impl->response_storage, client_impl->response_callbacks->ros_struct_size);
    }
    client_impl->response_ready = false; // consumed
    pthread_mutex_unlock(&client_impl->response_mutex);

    if (!success) {
        // The call timed out (tt_CALL_TIMEOUT) - rmw has no distinct "failed" signal on this
        // path, only "was something taken or not", so this surfaces the same way "nothing has
        // arrived yet" does.
        *taken = false;
        return RMW_RET_OK;
    }

    *taken = true;
    memset(request_header, 0, sizeof(*request_header));
    request_header->request_id.sequence_number = seq;
    // source_timestamp/received_timestamp/writer_guid left zeroed - no real timestamp/GID
    // tracking on this path yet (matches rmw_publisher.c's own message_info gap).
    return RMW_RET_OK;
}
