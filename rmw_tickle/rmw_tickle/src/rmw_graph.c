/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 6: rmw_get_node_names()/rmw_get_node_names_with_enclaves()/
// rmw_count_publishers()/rmw_count_subscribers()/rmw_service_server_is_available(). The graph-
// changed guard condition itself is wired up in rmw_node.c's own discovery_callback() (Milestone
// 0(c)'s tt_DISCOVERY_CALLBACK), not here - this file only answers graph *queries*.
//
// TickLE's own wire protocol has no node-name concept at all (rmw_node.c's own rmw_create_node()
// doc comment - a node is identified purely by its numeric id) and struct tt_DiscoveredEntity
// (Milestone 0(c)) never learns a remote node's display name either, only its endpoints' own
// kind/name/type - so rmw_get_node_names() can only ever truthfully report the *local* node this
// process itself created, never any other node actually visible on the segment. A real, documented
// gap versus full ROS 2 graph introspection (e.g. `ros2 node list` against a live rmw_tickle graph
// would only ever show the one node each process queries from), not solved here.
//
// rmw_count_publishers()/_subscribers() need to count matches in *two* places: TickLE's own
// per-node discovery table (tt_Discovery, Milestone 0(c)) only ever records *remote* entities -
// other nodes' own announces - never this node's own locally-created ones (see struct tt_Discovery
// itself), so count_matching() below also scans tickle_node.endpoints[] directly for local
// matches, rather than needing a separate local bookkeeping list of its own.

#include <pthread.h> // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own <pthread.h> comment
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tickle/config.h> // tt_NODE_ID_INVALID, tt_MAX_DISCOVERED_ENTITIES
#include <tickle/tickle.h>

#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rcutils/types/rcutils_ret.h"
#include "rcutils/types/string_array.h"
#include "rmw/error_handling.h"
#include "rmw/init.h" // rmw_context_t
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t rmw_get_node_names(const rmw_node_t* node, rcutils_string_array_t* node_names,
                             rcutils_string_array_t* node_namespaces) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node_names, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node_namespaces, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;

    // See this file's own module doc comment: only the local node is ever truthfully knowable.
    if (rcutils_string_array_init(node_names, 1, &node_impl->allocator) != RCUTILS_RET_OK) {
        RMW_SET_ERROR_MSG("failed to allocate node_names");
        return RMW_RET_BAD_ALLOC;
    }
    if (rcutils_string_array_init(node_namespaces, 1, &node_impl->allocator) != RCUTILS_RET_OK) {
        RMW_SET_ERROR_MSG("failed to allocate node_namespaces");
        rcutils_string_array_fini(node_names);
        return RMW_RET_BAD_ALLOC;
    }
    node_names->data[0] = rcutils_strdup(node->name, node_impl->allocator);
    node_namespaces->data[0] = rcutils_strdup(node->namespace_, node_impl->allocator);
    if (NULL == node_names->data[0] || NULL == node_namespaces->data[0]) {
        RMW_SET_ERROR_MSG("failed to allocate node name/namespace string");
        rcutils_string_array_fini(node_names);
        rcutils_string_array_fini(node_namespaces);
        return RMW_RET_BAD_ALLOC;
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_get_node_names_with_enclaves(const rmw_node_t* node, rcutils_string_array_t* node_names,
                                           rcutils_string_array_t* node_namespaces, rcutils_string_array_t* enclaves) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(enclaves, RMW_RET_INVALID_ARGUMENT);

    rmw_ret_t ret = rmw_get_node_names(node, node_names, node_namespaces);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    if (rcutils_string_array_init(enclaves, 1, &node_impl->allocator) != RCUTILS_RET_OK) {
        RMW_SET_ERROR_MSG("failed to allocate enclaves");
        rcutils_string_array_fini(node_names);
        rcutils_string_array_fini(node_namespaces);
        return RMW_RET_BAD_ALLOC;
    }
    // No per-node security-enclave tracking of its own (rmw_tickle has no security/SROS2 support) -
    // "/" is the same default enclave name rcl itself falls back to for an unset one.
    const char* enclave = NULL != node->context->options.enclave ? node->context->options.enclave : "/";
    enclaves->data[0] = rcutils_strdup(enclave, node_impl->allocator);
    if (NULL == enclaves->data[0]) {
        RMW_SET_ERROR_MSG("failed to allocate enclave string");
        rcutils_string_array_fini(node_names);
        rcutils_string_array_fini(node_namespaces);
        rcutils_string_array_fini(enclaves);
        return RMW_RET_BAD_ALLOC;
    }
    return RMW_RET_OK;
}

// The actual scan, shared by count_matching() below and rmw_tickle_count_matching_locked()
// (rmw_tickle.h - RMW_EVENT_LIVELINESS_CHANGED's own periodic check, rmw_subscription.c). Assumes
// node_impl->mutex is already held by the caller - see rmw_tickle_count_matching_locked()'s own
// doc comment for why that one can't take it itself. Only ever counts *alive* discovery entries
// (struct tt_DiscoveredEntity.alive's own doc comment, tickle.h) - a tombstoned one (presumed
// dead via a liveliness timeout, not a normal departure) shouldn't count as "currently offered/
// requested" for rmw_count_publishers()/_subscribers() or RMW_EVENT_LIVELINESS_CHANGED's own
// alive_count either; see count_not_alive_matching_locked() below for its own counterpart. Local
// endpoints have no tombstone concept at all - they're either present in tickle_node.endpoints[]
// or destroyed outright, so no matching check is needed for them.
static size_t count_matching_locked(rmw_tickle_node_t* node_impl, const char* topic_name, uint8_t kind) {
    size_t matched = 0;
    for (uint32_t i = 0; i < node_impl->tickle_node.endpoint_count; ++i) {
        const struct tt_Endpoint* endpoint = node_impl->tickle_node.endpoints[i];
        if (endpoint->kind == kind && strcmp(endpoint->name, topic_name) == 0) {
            matched++;
        }
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &node_impl->discovery.entities[i];
        if (entity->node_id != tt_NODE_ID_INVALID && entity->alive && entity->kind == kind &&
            strcmp(entity->name, topic_name) == 0) {
            matched++;
        }
    }
    return matched;
}

// count_matching_locked()'s own tombstone counterpart - QoS roadmap #3 (LIVELINESS)'s own
// RMW_EVENT_LIVELINESS_CHANGED.not_alive_count (a live snapshot, rmw_subscription.c/rmw_event.c),
// now backed by real data (struct tt_DiscoveredEntity.alive's own doc comment) instead of always
// 0. Local endpoints are never counted here for the same reason count_matching_locked() never
// checks them for aliveness - no tombstone concept applies to them.
static size_t count_not_alive_matching_locked(rmw_tickle_node_t* node_impl, const char* topic_name, uint8_t kind) {
    size_t matched = 0;
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &node_impl->discovery.entities[i];
        if (entity->node_id != tt_NODE_ID_INVALID && !entity->alive && entity->kind == kind &&
            strcmp(entity->name, topic_name) == 0) {
            matched++;
        }
    }
    return matched;
}

// rmw_tickle.h's own declaration - see this file's own module doc comment for why both the local
// endpoint table and the remote discovery table need scanning. Callable from any thread except
// the poll thread itself mid-tt_Node_poll() (see count_matching_locked()'s own doc comment).
size_t rmw_tickle_count_matching_locked(rmw_tickle_node_t* node_impl, const char* topic_name, uint8_t kind) {
    return count_matching_locked(node_impl, topic_name, kind);
}

// rmw_tickle.h's own declaration - see count_not_alive_matching_locked()'s own doc comment.
size_t rmw_tickle_count_not_alive_matching_locked(rmw_tickle_node_t* node_impl, const char* topic_name, uint8_t kind) {
    return count_not_alive_matching_locked(node_impl, topic_name, kind);
}

// Shared by rmw_count_publishers()/rmw_count_subscribers() - see this file's own module doc
// comment for why both the local endpoint table and the remote discovery table need scanning.
static rmw_ret_t count_matching(const rmw_node_t* node, const char* topic_name, uint8_t kind, size_t* count) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(count, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;

    // discovery.entities[]/tickle_node.endpoints[] are both written from the poll thread (UPDATE
    // processing / endpoint creation) - same tt_Node_interrupt()-then-lock contract every other
    // rmw_tickle_c entry point touching tickle_node follows (rmw_tickle.h's own rmw_tickle_node_t
    // doc comment).
    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_mutex_lock(&node_impl->mutex);
    size_t matched = count_matching_locked(node_impl, topic_name, kind);
    pthread_mutex_unlock(&node_impl->mutex);
    *count = matched;
    return RMW_RET_OK;
}

rmw_ret_t rmw_count_publishers(const rmw_node_t* node, const char* topic_name, size_t* count) {
    return count_matching(node, topic_name, tt_KIND_TOPIC_PUBLISHER, count);
}

rmw_ret_t rmw_count_subscribers(const rmw_node_t* node, const char* topic_name, size_t* count) {
    return count_matching(node, topic_name, tt_KIND_TOPIC_SUBSCRIBER, count);
}

rmw_ret_t rmw_service_server_is_available(const rmw_node_t* node, const rmw_client_t* client, bool* is_available) {
    // RMW_RET_ERROR, not RMW_RET_INVALID_ARGUMENT - see rmw_destroy_wait_set()'s own comment
    // (rmw_wait_set.c) for the identical doc-vs-real-implementations discrepancy; test_rmw_
    // implementation's own TestClientUse.service_server_is_available_bad_args (Milestone 16)
    // expects RMW_RET_ERROR for all three of these.
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_ERROR);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_ERROR);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(is_available, RMW_RET_ERROR);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier) ||
        !rmw_tickle_identifier_matches(client->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)client->data;
    // A service is identified by *both* its ROS type name and its ROS service name (tt_hash_id()
    // mixes both, matching ROS 2's own "type and name must both match to connect" rule - see
    // rmw_tickle.h's own rmw_tickle_client_t.service doc comment) - client_impl->service.name is
    // the type name; client->service_name (== the rmw_client_t's own field) is the service name.
    const char* type_name = client_impl->service.name;
    const char* service_name = client->service_name;

    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_mutex_lock(&node_impl->mutex);

    bool found = false;
    for (uint32_t i = 0; i < node_impl->tickle_node.endpoint_count && !found; ++i) {
        const struct tt_Endpoint* endpoint = node_impl->tickle_node.endpoints[i];
        // A local endpoint has no separate "type name" of its own to compare here (tt_Endpoint
        // only carries the service/topic *name*, not its type - see struct tt_Endpoint) - a local
        // server matches on name+kind alone, mirroring how tt_Node_create_server() itself only
        // ever registers one server per (name), never per (name, type) pair on this side.
        if (endpoint->kind == tt_KIND_SERVICE_SERVER && strcmp(endpoint->name, service_name) == 0) {
            found = true;
        }
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES && !found; ++i) {
        const struct tt_DiscoveredEntity* entity = &node_impl->discovery.entities[i];
        if (entity->node_id != tt_NODE_ID_INVALID && entity->kind == tt_KIND_SERVICE_SERVER &&
            strcmp(entity->name, service_name) == 0 && strcmp(entity->type, type_name) == 0) {
            found = true;
        }
    }

    pthread_mutex_unlock(&node_impl->mutex);
    *is_available = found;
    return RMW_RET_OK;
}
