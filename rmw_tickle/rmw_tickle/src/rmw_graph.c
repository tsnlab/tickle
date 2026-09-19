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
// kind/name/type - so rmw_get_node_names() can only ever truthfully report this *process's own*
// logical nodes, never any other process's. Milestone 34 closed part of this gap as a side effect
// of promoting the shared tt_Node/discovery up to rmw_tickle_context_impl_t: every logical node
// sharing this context is now enumerable (context_impl->nodes[]), not just the one specific
// rmw_node_t handle a caller happened to pass in - a real, documented gap versus full ROS 2 graph
// introspection remains regardless (e.g. `ros2 node list` against a live rmw_tickle graph would
// only ever show this one process's own nodes, never a remote process's).
//
// rmw_count_publishers()/_subscribers() need to count matches in *two* places: TickLE's own
// discovery table (tt_Discovery, Milestone 0(c), now context-scoped) only ever records *remote*
// entities - other nodes' own announces - never this process's own locally-created ones (see
// struct tt_Discovery itself), so count_matching() below also scans tickle_node.endpoints[]
// directly for local matches, rather than needing a separate local bookkeeping list of its own.

#include <pthread.h> // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own <pthread.h> comment
#include <stdatomic.h>
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
#include "rmw/sanity_checks.h" // rmw_check_zero_rmw_string_array()
#include "rmw/types.h"
#include "rmw/validate_full_topic_name.h" // rmw_validate_full_topic_name()
#include "rmw_tickle_c/rmw_tickle.h"

// rcutils_string_array_fini() is declared warn_unused_result - these cleanup-on-error paths
// intentionally don't propagate a second failure over the original allocation error already being
// returned, so capture-and-discard here once rather than repeating a (void)-cast dance at every
// call site (a plain (void) cast on the call itself doesn't satisfy GCC's warn_unused_result).
static void fini_string_array_ignore_result(rcutils_string_array_t* array) {
    rcutils_ret_t ret = rcutils_string_array_fini(array);
    (void)ret;
}

// Milestone 34 - reports every logical node currently registered under this rmw_node_t's own
// context (context_impl->nodes[]), not just the one handle the caller happened to pass in - see
// this file's own module doc comment for why that's now knowable, and still not any *other*
// process's own nodes.
rmw_ret_t rmw_get_node_names(const rmw_node_t* node, rcutils_string_array_t* node_names,
                             rcutils_string_array_t* node_namespaces) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node_names, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node_namespaces, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    // A caller-supplied array that already holds data (not freshly rcutils_get_zero_initialized_
    // string_array()'d) is an invalid argument, not something to silently overwrite/leak -
    // rmw_check_zero_rmw_string_array() already sets its own error message on failure. Checked
    // before touching either array, so a rejected node_names leaves node_namespaces untouched too
    // (test_rmw_implementation's own get_node_names_with_bad_arguments relies on exactly this).
    if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_names)) {
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_namespaces)) {
        return RMW_RET_INVALID_ARGUMENT;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;

    pthread_mutex_lock(&context_impl->registry_mutex);
    int count = atomic_load(&context_impl->node_count);
    if (rcutils_string_array_init(node_names, (size_t)count, &node_impl->allocator) != RCUTILS_RET_OK) {
        pthread_mutex_unlock(&context_impl->registry_mutex);
        RMW_SET_ERROR_MSG("failed to allocate node_names");
        return RMW_RET_BAD_ALLOC;
    }
    if (rcutils_string_array_init(node_namespaces, (size_t)count, &node_impl->allocator) != RCUTILS_RET_OK) {
        pthread_mutex_unlock(&context_impl->registry_mutex);
        RMW_SET_ERROR_MSG("failed to allocate node_namespaces");
        fini_string_array_ignore_result(node_names);
        return RMW_RET_BAD_ALLOC;
    }
    bool alloc_failed = false;
    for (int i = 0; i < count && !alloc_failed; i++) {
        node_names->data[i] = rcutils_strdup(context_impl->nodes[i]->rmw_node.name, node_impl->allocator);
        node_namespaces->data[i] = rcutils_strdup(context_impl->nodes[i]->rmw_node.namespace_, node_impl->allocator);
        alloc_failed = NULL == node_names->data[i] || NULL == node_namespaces->data[i];
    }
    pthread_mutex_unlock(&context_impl->registry_mutex);
    if (alloc_failed) {
        RMW_SET_ERROR_MSG("failed to allocate node name/namespace string");
        fini_string_array_ignore_result(node_names);
        fini_string_array_ignore_result(node_namespaces);
        return RMW_RET_BAD_ALLOC;
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_get_node_names_with_enclaves(const rmw_node_t* node, rcutils_string_array_t* node_names,
                                           rcutils_string_array_t* node_namespaces, rcutils_string_array_t* enclaves) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(enclaves, RMW_RET_INVALID_ARGUMENT);
    // Checked before rmw_get_node_names() below touches node_names/node_namespaces at all, so a
    // rejected enclaves leaves both of those untouched too - same reasoning as rmw_get_node_
    // names()'s own node_names/node_namespaces ordering.
    if (RMW_RET_OK != rmw_check_zero_rmw_string_array(enclaves)) {
        return RMW_RET_INVALID_ARGUMENT;
    }

    rmw_ret_t ret = rmw_get_node_names(node, node_names, node_namespaces);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    // Milestone 34 - sized to match node_names/node_namespaces above (every logical node sharing
    // this context), not just 1 - rmw's own contract requires all three arrays the same length.
    if (rcutils_string_array_init(enclaves, node_names->size, &node_impl->allocator) != RCUTILS_RET_OK) {
        RMW_SET_ERROR_MSG("failed to allocate enclaves");
        fini_string_array_ignore_result(node_names);
        fini_string_array_ignore_result(node_namespaces);
        return RMW_RET_BAD_ALLOC;
    }
    // No per-node security-enclave tracking of its own (rmw_tickle has no security/SROS2 support) -
    // "/" is the same default enclave name rcl itself falls back to for an unset one. Every logical
    // node sharing one context also shares this one process-wide enclave (rmw's own init_options
    // are per-context, not per-node), so the same string is correct for every entry.
    const char* enclave = NULL != node->context->options.enclave ? node->context->options.enclave : "/";
    bool alloc_failed = false;
    for (size_t i = 0; i < enclaves->size && !alloc_failed; i++) {
        enclaves->data[i] = rcutils_strdup(enclave, node_impl->allocator);
        alloc_failed = NULL == enclaves->data[i];
    }
    if (alloc_failed) {
        RMW_SET_ERROR_MSG("failed to allocate enclave string");
        fini_string_array_ignore_result(node_names);
        fini_string_array_ignore_result(node_namespaces);
        fini_string_array_ignore_result(enclaves);
        return RMW_RET_BAD_ALLOC;
    }
    return RMW_RET_OK;
}

// The actual scan, shared by count_matching() below and rmw_tickle_count_matching_locked()
// (rmw_tickle.h - RMW_EVENT_LIVELINESS_CHANGED's own periodic check, rmw_subscription.c). Assumes
// context_impl->node_mutex is already held by the caller - see rmw_tickle_count_matching_locked()'s
// own doc comment for why that one can't take it itself. Only ever counts *alive* discovery
// entries (struct tt_DiscoveredEntity.alive's own doc comment, tickle.h) - a tombstoned one
// (presumed dead via a liveliness timeout, not a normal departure) shouldn't count as "currently
// offered/requested" for rmw_count_publishers()/_subscribers() or RMW_EVENT_LIVELINESS_CHANGED's
// own alive_count either; see count_not_alive_matching_locked() below for its own counterpart.
// Local endpoints have no tombstone concept at all - they're either present in tickle_node.
// endpoints[] or destroyed outright, so no matching check is needed for them.
static size_t count_matching_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name, uint8_t kind) {
    size_t matched = 0;
    for (uint32_t i = 0; i < context_impl->tickle_node.endpoint_count; ++i) {
        const struct tt_Endpoint* endpoint = context_impl->tickle_node.endpoints[i];
        if (endpoint->kind == kind && strcmp(endpoint->name, topic_name) == 0) {
            matched++;
        }
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
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
static size_t count_not_alive_matching_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                              uint8_t kind) {
    size_t matched = 0;
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
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
size_t rmw_tickle_count_matching_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name, uint8_t kind) {
    return count_matching_locked(context_impl, topic_name, kind);
}

// rmw_tickle.h's own declaration - see count_not_alive_matching_locked()'s own doc comment.
size_t rmw_tickle_count_not_alive_matching_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                                  uint8_t kind) {
    return count_not_alive_matching_locked(context_impl, topic_name, kind);
}

// The tt_Node_interrupt()-then-lock-then-scan-then-unlock sequence every count_matching_locked()
// caller in this file needs - split out once both rmw_count_publishers()/_subscribers() (below)
// and rmw_publisher_count_matched_subscriptions()/rmw_subscription_count_matched_publishers()
// (rmw_tickle/PLAN.md's remaining-rmw-API-surface backlog) needed the identical sequence, just
// against a different topic_name each time - always the same shared context_impl now (Milestone
// 34), never a specific rmw_tickle_node_t.
static size_t count_matching_via_context_impl(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                              uint8_t kind) {
    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);
    size_t matched = count_matching_locked(context_impl, topic_name, kind);
    pthread_mutex_unlock(&context_impl->node_mutex);
    return matched;
}

// Shared by rmw_count_publishers()/_subscribers()/_clients()/_services() - see this file's own
// module doc comment for why both the local endpoint table and the remote discovery table need
// scanning.
static rmw_ret_t count_matching(const rmw_node_t* node, const char* topic_name, uint8_t kind, size_t* count) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(count, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    // Never checked before (test_rmw_implementation's own count_publishers_with_bad_arguments et
    // al. are what found this, same shape as rmw_create_node()'s own rmw_validate_node_name()/
    // rmw_validate_namespace() precedent): a malformed topic/service name (spaces, reserved
    // characters, ...) used to silently succeed instead of being rejected. Shared by all four
    // rmw_count_*() callers below - service names are validated as topic names too (real rmw
    // implementations treat a service as a topic pair under the hood, same naming rules apply).
    int validation_result = RMW_TOPIC_VALID;
    size_t invalid_index = 0;
    if (RMW_RET_OK != rmw_validate_full_topic_name(topic_name, &validation_result, &invalid_index)) {
        return RMW_RET_INVALID_ARGUMENT; // rmw_validate_full_topic_name() already set its own error message
    }
    if (RMW_TOPIC_VALID != validation_result) {
        RMW_SET_ERROR_MSG(rmw_full_topic_name_validation_result_string(validation_result));
        return RMW_RET_INVALID_ARGUMENT;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    *count = count_matching_via_context_impl(node_impl->context_impl, topic_name, kind);
    return RMW_RET_OK;
}

rmw_ret_t rmw_count_publishers(const rmw_node_t* node, const char* topic_name, size_t* count) {
    return count_matching(node, topic_name, tt_KIND_TOPIC_PUBLISHER, count);
}

rmw_ret_t rmw_count_subscribers(const rmw_node_t* node, const char* topic_name, size_t* count) {
    return count_matching(node, topic_name, tt_KIND_TOPIC_SUBSCRIBER, count);
}

// rmw_tickle/PLAN.md's remaining-rmw-API-surface backlog - previously missing symbols entirely
// (Milestone 15's own note). Same shape as rmw_count_publishers()/_subscribers() above, just
// scanning for the two service-side kinds instead of the two topic-side ones - no new mechanism
// needed, count_matching()/count_matching_locked() already scan both local endpoints and remote
// discovery regardless of kind.
rmw_ret_t rmw_count_clients(const rmw_node_t* node, const char* service_name, size_t* count) {
    return count_matching(node, service_name, tt_KIND_SERVICE_CLIENT, count);
}

rmw_ret_t rmw_count_services(const rmw_node_t* node, const char* service_name, size_t* count) {
    return count_matching(node, service_name, tt_KIND_SERVICE_SERVER, count);
}

// rmw_tickle/PLAN.md's remaining-rmw-API-surface backlog - previously missing symbols entirely.
// Deliberately the *same* same-topic-name-and-kind computation rmw_count_publishers()/
// _subscribers() above already use, just scoped to "the topic this one Publisher/Subscription
// itself is on" instead of an arbitrary caller-given topic_name - not QoS-compatibility-filtered
// (would need re-deriving each remote match's own offered/requested bits from struct tt_
// DiscoveredEntity.qos and comparing, QoS roadmap #1's own RxO matching machinery, Milestone 31),
// matching rmw_count_publishers()/_subscribers()'s own pre-existing, unfiltered scope exactly
// rather than introducing an inconsistency between two otherwise-identical counting functions.
rmw_ret_t rmw_publisher_count_matched_subscriptions(const rmw_publisher_t* publisher, size_t* subscription_count) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription_count, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    *subscription_count =
        count_matching_via_context_impl(pub_impl->node->context_impl, publisher->topic_name, tt_KIND_TOPIC_SUBSCRIBER);
    return RMW_RET_OK;
}

rmw_ret_t rmw_subscription_count_matched_publishers(const rmw_subscription_t* subscription, size_t* publisher_count) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher_count, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;
    *publisher_count = count_matching_via_context_impl(sub_impl->node->context_impl, subscription->topic_name,
                                                       tt_KIND_TOPIC_PUBLISHER);
    return RMW_RET_OK;
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
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;
    rmw_tickle_client_t* client_impl = (rmw_tickle_client_t*)client->data;
    // A service is identified by *both* its ROS type name and its ROS service name (tt_hash_id()
    // mixes both, matching ROS 2's own "type and name must both match to connect" rule - see
    // rmw_tickle.h's own rmw_tickle_client_t.service doc comment) - client_impl->service.name is
    // the type name; client->service_name (== the rmw_client_t's own field) is the service name.
    const char* type_name = client_impl->service.name;
    const char* service_name = client->service_name;

    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);

    bool found = false;
    for (uint32_t i = 0; i < context_impl->tickle_node.endpoint_count && !found; ++i) {
        const struct tt_Endpoint* endpoint = context_impl->tickle_node.endpoints[i];
        // A local endpoint has no separate "type name" of its own to compare here (tt_Endpoint
        // only carries the service/topic *name*, not its type - see struct tt_Endpoint) - a local
        // server matches on name+kind alone, mirroring how tt_Node_create_server() itself only
        // ever registers one server per (name), never per (name, type) pair on this side.
        if (endpoint->kind == tt_KIND_SERVICE_SERVER && strcmp(endpoint->name, service_name) == 0) {
            found = true;
        }
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES && !found; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->node_id != tt_NODE_ID_INVALID && entity->kind == tt_KIND_SERVICE_SERVER &&
            strcmp(entity->name, service_name) == 0 && strcmp(entity->type, type_name) == 0) {
            found = true;
        }
    }

    pthread_mutex_unlock(&context_impl->node_mutex);
    *is_available = found;
    return RMW_RET_OK;
}
