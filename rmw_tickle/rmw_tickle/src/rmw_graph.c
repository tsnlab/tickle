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

#include "rcutils/allocator.h" // rcutils_allocator_is_valid()
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rcutils/types/rcutils_ret.h"
#include "rcutils/types/string_array.h"
#include "rmw/error_handling.h"
#include "rmw/get_node_info_and_types.h" // rmw_get_*_names_and_types_by_node()
#include "rmw/get_service_names_and_types.h"
#include "rmw/get_topic_endpoint_info.h" // rmw_get_publishers/subscriptions_info_by_topic()
#include "rmw/get_topic_names_and_types.h"
#include "rmw/init.h" // rmw_context_t
#include "rmw/names_and_types.h"
#include "rmw/qos_profiles.h" // rmw_qos_profile_unknown
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/sanity_checks.h" // rmw_check_zero_rmw_string_array()
#include "rmw/topic_endpoint_info.h"
#include "rmw/topic_endpoint_info_array.h"
#include "rmw/types.h"
#include "rmw/validate_full_topic_name.h" // rmw_validate_full_topic_name()
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"
#include "rmw_tickle_c/rmw_tickle.h"

// rcutils_string_array_fini() is declared warn_unused_result - these cleanup-on-error paths
// intentionally don't propagate a second failure over the original allocation error already being
// returned, so capture-and-discard here once rather than repeating a (void)-cast dance at every
// call site (a plain (void) cast on the call itself doesn't satisfy GCC's warn_unused_result).
static void fini_string_array_ignore_result(rcutils_string_array_t* array) {
    rcutils_ret_t ret = rcutils_string_array_fini(array);
    (void)ret;
}

// Same reasoning as fini_string_array_ignore_result() above, for the two other warn_unused_result
// fini()s Milestone 37's own cleanup-on-error paths need.
static void fini_names_and_types_ignore_result(rmw_names_and_types_t* names_and_types) {
    rmw_ret_t ret = rmw_names_and_types_fini(names_and_types);
    (void)ret;
}

static void fini_topic_endpoint_info_array_ignore_result(rmw_topic_endpoint_info_array_t* info_array,
                                                         rcutils_allocator_t* allocator) {
    rmw_ret_t ret = rmw_topic_endpoint_info_array_fini(info_array, allocator);
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

// Milestone 37 (rmw_tickle/PLAN.md) - the "names and types" function family Milestone 33 deferred
// as its own larger follow-on. A local tt_Endpoint has no type-name field of its own (struct tt_
// Endpoint's own doc comment, tickle.h) and no owning-node field either, so both are recovered by
// reversing the same offsetof() cast every discovery/data callback in this package already uses
// (e.g. rmw_node.c's own discovery_callback()) to reach the concrete rmw_tickle_publisher_t/
// _subscriber_t/_client_t/_service_t wrapper - mirrors tickle.c's own private endpoint_type_name()
// exactly, just against public structs only (that static isn't reachable from here). qos is only
// meaningful for topic endpoints (Publisher/Subscriber both carry a real, full rmw_qos_profile_t)
// - NULL for Client/Server, never dereferenced by any caller that asks for those kinds.
struct local_endpoint_details {
    const char* type_name;
    const char* owning_node_name;
    const char* owning_node_namespace;
    const rmw_qos_profile_t* qos;
};

static struct local_endpoint_details get_local_endpoint_details(struct tt_Endpoint* endpoint) {
    switch (endpoint->kind) {
    case tt_KIND_TOPIC_PUBLISHER: {
        rmw_tickle_publisher_t* pub_impl =
            (rmw_tickle_publisher_t*)((char*)endpoint - offsetof(rmw_tickle_publisher_t, tickle_publisher));
        return (struct local_endpoint_details) {pub_impl->topic.name, pub_impl->owning_node_name,
                                                pub_impl->owning_node_namespace, &pub_impl->qos};
    }
    case tt_KIND_TOPIC_SUBSCRIBER: {
        rmw_tickle_subscriber_t* sub_impl =
            (rmw_tickle_subscriber_t*)((char*)endpoint - offsetof(rmw_tickle_subscriber_t, tickle_subscriber));
        return (struct local_endpoint_details) {sub_impl->topic.name, sub_impl->owning_node_name,
                                                sub_impl->owning_node_namespace, &sub_impl->qos};
    }
    case tt_KIND_SERVICE_CLIENT: {
        rmw_tickle_client_t* client_impl =
            (rmw_tickle_client_t*)((char*)endpoint - offsetof(rmw_tickle_client_t, tickle_client));
        return (struct local_endpoint_details) {client_impl->service.name, client_impl->owning_node_name,
                                                client_impl->owning_node_namespace, NULL};
    }
    case tt_KIND_SERVICE_SERVER: {
        rmw_tickle_service_t* svc_impl =
            (rmw_tickle_service_t*)((char*)endpoint - offsetof(rmw_tickle_service_t, tickle_server));
        return (struct local_endpoint_details) {svc_impl->service.name, svc_impl->owning_node_name,
                                                svc_impl->owning_node_namespace, NULL};
    }
    default:
        return (struct local_endpoint_details) {NULL, NULL, NULL, NULL};
    }
}

// A deduplicated (name, type) pair - shared scan buffer for every names_and_types query below.
// Fixed-capacity: bounded by tt_MAX_ENDPOINT_COUNT (local) + tt_MAX_DISCOVERED_ENTITIES (remote),
// the same bound count_matching_locked()'s own two loops already assume.
struct name_type_entry {
    const char* name;
    const char* type;
};
#define tt_MAX_NAME_TYPE_ENTRIES (tt_MAX_ENDPOINT_COUNT + tt_MAX_DISCOVERED_ENTITIES)

// Appends (name, type) if this exact pair isn't already present, returning the new count. More
// than one endpoint can legitimately announce the very same (name, type) pair now (Milestone 35 -
// multiple local endpoints, or simply multiple remote participants on one topic), and each should
// only ever contribute one entry to a *names and types* result (unlike rmw_get_publishers_info_
// by_topic()/_subscriptions_info_by_topic() below, where every individual endpoint instance gets
// its own row).
static size_t add_name_type_entry(struct name_type_entry* entries, size_t count, const char* name, const char* type) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0 && strcmp(entries[i].type, type) == 0) {
            return count;
        }
    }
    entries[count].name = name;
    entries[count].type = type;
    return count + 1;
}

// The actual scan behind rmw_get_topic_names_and_types()/_service_names_and_types(): every
// distinct (name, type) pair among BOTH given kinds ("for which a publisher and/or a subscription
// exists" per rmw_get_topic_names_and_types()'s own doc comment - Publisher+Subscriber for
// topics, Server+Client for services), local endpoints and remote discovery alike. Caller must
// already hold context_impl->node_mutex for as long as the raw name/type pointers gathered here
// stay in use - build_names_and_types() below strdup()s them before any caller may unlock.
static size_t collect_graph_wide_name_types(rmw_tickle_context_impl_t* context_impl, uint8_t kind_a, uint8_t kind_b,
                                            struct name_type_entry* entries) {
    size_t count = 0;
    for (uint32_t i = 0; i < context_impl->tickle_node.endpoint_count; ++i) {
        struct tt_Endpoint* endpoint = context_impl->tickle_node.endpoints[i];
        if (endpoint->kind != kind_a && endpoint->kind != kind_b) {
            continue;
        }
        struct local_endpoint_details details = get_local_endpoint_details(endpoint);
        count = add_name_type_entry(entries, count, endpoint->name, details.type_name);
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->node_id == tt_NODE_ID_INVALID || !entity->alive) {
            continue;
        }
        if (entity->kind != kind_a && entity->kind != kind_b) {
            continue;
        }
        count = add_name_type_entry(entries, count, entity->name, entity->type);
    }
    return count;
}

// The actual scan behind the four *_by_node() queries below: every distinct (name, type) pair for
// ONE kind, owned by the given LOCAL node specifically. Remote (discovered) entities are never
// included here - TickLE's wire protocol has no node-name concept at all (this file's own module
// doc comment), so there is no way to know which remote node a discovered entity belongs to, only
// that it exists - an honest, documented gap, not something this function tries to paper over.
// Same locking contract as collect_graph_wide_name_types() above.
static size_t collect_by_node_name_types(rmw_tickle_context_impl_t* context_impl, uint8_t kind,
                                         const char* owning_node_name, const char* owning_node_namespace,
                                         struct name_type_entry* entries) {
    size_t count = 0;
    for (uint32_t i = 0; i < context_impl->tickle_node.endpoint_count; ++i) {
        struct tt_Endpoint* endpoint = context_impl->tickle_node.endpoints[i];
        if (endpoint->kind != kind) {
            continue;
        }
        struct local_endpoint_details details = get_local_endpoint_details(endpoint);
        if (strcmp(details.owning_node_name, owning_node_name) != 0 ||
            strcmp(details.owning_node_namespace, owning_node_namespace) != 0) {
            continue;
        }
        count = add_name_type_entry(entries, count, endpoint->name, details.type_name);
    }
    return count;
}

// Whether entries[0..upto) already contains this exact name - shared by build_names_and_types()
// below's own two passes (counting distinct names, then filling them), each needing the identical
// "is this the first occurrence" check.
static bool name_already_seen(const struct name_type_entry* entries, size_t upto, const char* name) {
    for (size_t i = 0; i < upto; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

// How many entries (starting from `from`) share entries[from].name - i.e. how many distinct types
// that one name is associated with, since entries[] is only deduplicated by (name, type) pair,
// not by name alone (see add_name_type_entry()'s own doc comment).
static size_t count_types_for_name(const struct name_type_entry* entries, size_t entry_count, size_t from) {
    size_t type_count = 0;
    for (size_t j = from; j < entry_count; j++) {
        if (strcmp(entries[j].name, entries[from].name) == 0) {
            type_count++;
        }
    }
    return type_count;
}

// Fills one already-rcutils_string_array_init()'d types[] entry with every type entries[from].name
// is associated with. Returns false on allocation failure - the caller's own single, shared
// RMW_SET_ERROR_MSG() covers this alongside its own two other allocation points, rather than
// setting (and immediately overwriting) an equivalent message here too.
static bool fill_types_for_name(const struct name_type_entry* entries, size_t entry_count, size_t from,
                                rcutils_allocator_t* allocator, rcutils_string_array_t* types_out) {
    size_t type_index = 0;
    for (size_t j = from; j < entry_count; j++) {
        if (strcmp(entries[j].name, entries[from].name) != 0) {
            continue;
        }
        types_out->data[type_index] = rcutils_strdup(entries[j].type, *allocator);
        if (NULL == types_out->data[type_index]) {
            return false;
        }
        type_index++;
    }
    return true;
}

// Builds the final rmw_names_and_types_t from a flat, (name,type)-pair-deduplicated scan buffer:
// groups by distinct NAME (ignoring type), since more than one endpoint can legitimately announce
// the very same name with genuinely different types too (an unusual misconfiguration, not
// filtered out here - matches real DDS's own "report what's actually there" graph-query
// semantics) - rmw_names_and_types_t's own shape needs exactly one names[] entry with a small
// types[i] array alongside it, not a flat list. Must be called while still holding whatever lock
// kept entries[]'s own name/type pointers valid - this is where they finally get strdup()'d.
static rmw_ret_t build_names_and_types(struct name_type_entry* entries, size_t entry_count,
                                       rcutils_allocator_t* allocator, rmw_names_and_types_t* result) {
    size_t distinct_count = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (!name_already_seen(entries, i, entries[i].name)) {
            distinct_count++;
        }
    }

    if (rmw_names_and_types_init(result, distinct_count, allocator) != RMW_RET_OK) {
        return RMW_RET_BAD_ALLOC; // rmw_names_and_types_init() already set its own error message
    }

    size_t name_index = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (name_already_seen(entries, i, entries[i].name)) {
            continue;
        }

        size_t type_count = count_types_for_name(entries, entry_count, i);
        result->names.data[name_index] = rcutils_strdup(entries[i].name, *allocator);
        if (NULL == result->names.data[name_index] ||
            rcutils_string_array_init(&result->types[name_index], type_count, allocator) != RCUTILS_RET_OK ||
            !fill_types_for_name(entries, entry_count, i, allocator, &result->types[name_index])) {
            RMW_SET_ERROR_MSG("failed to allocate names_and_types entry");
            fini_names_and_types_ignore_result(result);
            return RMW_RET_BAD_ALLOC;
        }
        name_index++;
    }

    return RMW_RET_OK;
}

// Shared bad-argument checks for rmw_get_topic_names_and_types()/_service_names_and_types() - no
// node_name/namespace to validate here, unlike the *_by_node family below.
static rmw_ret_t validate_names_and_types_args(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                               rmw_names_and_types_t* names_and_types) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(names_and_types, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RCUTILS_CHECK_ALLOCATOR_WITH_MSG(allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
    if (RMW_RET_OK != rmw_names_and_types_check_zero(names_and_types)) {
        return RMW_RET_INVALID_ARGUMENT;
    }
    return RMW_RET_OK;
}

// rmw_get_topic_names_and_types()'s own no_demangle has nothing to do either way - rmw_tickle
// applies no ROS-specific topic name mangling/prefixing at all (rmw_tickle/PLAN.md's own
// "Supported subset"), so there's nothing for no_demangle=true to opt out of undoing.
rmw_ret_t rmw_get_topic_names_and_types(const rmw_node_t* node, rcutils_allocator_t* allocator, bool no_demangle,
                                        rmw_names_and_types_t* topic_names_and_types) {
    (void)no_demangle;
    rmw_ret_t ret = validate_names_and_types_args(node, allocator, topic_names_and_types);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;
    struct name_type_entry entries[tt_MAX_NAME_TYPE_ENTRIES];

    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);
    size_t entry_count =
        collect_graph_wide_name_types(context_impl, tt_KIND_TOPIC_PUBLISHER, tt_KIND_TOPIC_SUBSCRIBER, entries);
    ret = build_names_and_types(entries, entry_count, allocator, topic_names_and_types);
    pthread_mutex_unlock(&context_impl->node_mutex);
    return ret;
}

rmw_ret_t rmw_get_service_names_and_types(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                          rmw_names_and_types_t* service_names_and_types) {
    rmw_ret_t ret = validate_names_and_types_args(node, allocator, service_names_and_types);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;
    struct name_type_entry entries[tt_MAX_NAME_TYPE_ENTRIES];

    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);
    size_t entry_count =
        collect_graph_wide_name_types(context_impl, tt_KIND_SERVICE_SERVER, tt_KIND_SERVICE_CLIENT, entries);
    ret = build_names_and_types(entries, entry_count, allocator, service_names_and_types);
    pthread_mutex_unlock(&context_impl->node_mutex);
    return ret;
}

// Milestone 34's own node registry - context_impl->nodes[] - is the only "which nodes exist" list
// TickLE has (this file's own module doc comment: the wire protocol has no node-name concept at
// all, so a *remote* node's own name can never be known, only that some remote entity exists).
// The four *_by_node() queries below can therefore only ever truthfully answer for a node in
// *this* process; anything else genuinely is RMW_RET_NODE_NAME_NON_EXISTENT, not a gap to paper
// over with a guess.
static bool node_name_is_registered(rmw_tickle_context_impl_t* context_impl, const char* node_name,
                                    const char* node_namespace) {
    pthread_mutex_lock(&context_impl->registry_mutex);
    int count = atomic_load(&context_impl->node_count);
    bool found = false;
    for (int i = 0; i < count && !found; i++) {
        found = strcmp(context_impl->nodes[i]->rmw_node.name, node_name) == 0 &&
                strcmp(context_impl->nodes[i]->rmw_node.namespace_, node_namespace) == 0;
    }
    pthread_mutex_unlock(&context_impl->registry_mutex);
    return found;
}

// Shared bad-argument checks for the four *_by_node() queries below - node_name/node_namespace
// validated the same way rmw_create_node() already validates them (rmw_node.c), matching
// rmw_get_subscriber_names_and_types_by_node()'s own documented RMW_RET_INVALID_ARGUMENT
// conditions exactly. Does NOT check RMW_RET_NODE_NAME_NON_EXISTENT - callers do that themselves,
// after this, only once they also know which kind/allocator they'll actually query with.
static rmw_ret_t validate_by_node_args(const rmw_node_t* node, rcutils_allocator_t* allocator, const char* node_name,
                                       const char* node_namespace, rmw_names_and_types_t* names_and_types) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node_name, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node_namespace, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(names_and_types, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RCUTILS_CHECK_ALLOCATOR_WITH_MSG(allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);

    int validation_result = RMW_NODE_NAME_VALID;
    size_t invalid_index = 0;
    if (RMW_RET_OK != rmw_validate_node_name(node_name, &validation_result, &invalid_index)) {
        return RMW_RET_INVALID_ARGUMENT; // rmw_validate_node_name() already set its own error message
    }
    if (RMW_NODE_NAME_VALID != validation_result) {
        RMW_SET_ERROR_MSG(rmw_node_name_validation_result_string(validation_result));
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (RMW_RET_OK != rmw_validate_namespace(node_namespace, &validation_result, &invalid_index)) {
        return RMW_RET_INVALID_ARGUMENT; // rmw_validate_namespace() already set its own error message
    }
    if (RMW_NAMESPACE_VALID != validation_result) {
        RMW_SET_ERROR_MSG(rmw_namespace_validation_result_string(validation_result));
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (RMW_RET_OK != rmw_names_and_types_check_zero(names_and_types)) {
        return RMW_RET_INVALID_ARGUMENT;
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_get_subscriber_names_and_types_by_node(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                                     const char* node_name, const char* node_namespace,
                                                     bool no_demangle, rmw_names_and_types_t* topic_names_and_types) {
    (void)no_demangle; // see rmw_get_topic_names_and_types()'s own doc comment
    rmw_ret_t ret = validate_by_node_args(node, allocator, node_name, node_namespace, topic_names_and_types);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;
    if (!node_name_is_registered(context_impl, node_name, node_namespace)) {
        RMW_SET_ERROR_MSG("node not found");
        return RMW_RET_NODE_NAME_NON_EXISTENT;
    }

    struct name_type_entry entries[tt_MAX_NAME_TYPE_ENTRIES];
    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);
    size_t entry_count =
        collect_by_node_name_types(context_impl, tt_KIND_TOPIC_SUBSCRIBER, node_name, node_namespace, entries);
    ret = build_names_and_types(entries, entry_count, allocator, topic_names_and_types);
    pthread_mutex_unlock(&context_impl->node_mutex);
    return ret;
}

rmw_ret_t rmw_get_publisher_names_and_types_by_node(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                                    const char* node_name, const char* node_namespace, bool no_demangle,
                                                    rmw_names_and_types_t* topic_names_and_types) {
    (void)no_demangle; // see rmw_get_topic_names_and_types()'s own doc comment
    rmw_ret_t ret = validate_by_node_args(node, allocator, node_name, node_namespace, topic_names_and_types);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;
    if (!node_name_is_registered(context_impl, node_name, node_namespace)) {
        RMW_SET_ERROR_MSG("node not found");
        return RMW_RET_NODE_NAME_NON_EXISTENT;
    }

    struct name_type_entry entries[tt_MAX_NAME_TYPE_ENTRIES];
    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);
    size_t entry_count =
        collect_by_node_name_types(context_impl, tt_KIND_TOPIC_PUBLISHER, node_name, node_namespace, entries);
    ret = build_names_and_types(entries, entry_count, allocator, topic_names_and_types);
    pthread_mutex_unlock(&context_impl->node_mutex);
    return ret;
}

rmw_ret_t rmw_get_service_names_and_types_by_node(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                                  const char* node_name, const char* node_namespace,
                                                  rmw_names_and_types_t* service_names_and_types) {
    rmw_ret_t ret = validate_by_node_args(node, allocator, node_name, node_namespace, service_names_and_types);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;
    if (!node_name_is_registered(context_impl, node_name, node_namespace)) {
        RMW_SET_ERROR_MSG("node not found");
        return RMW_RET_NODE_NAME_NON_EXISTENT;
    }

    struct name_type_entry entries[tt_MAX_NAME_TYPE_ENTRIES];
    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);
    size_t entry_count =
        collect_by_node_name_types(context_impl, tt_KIND_SERVICE_SERVER, node_name, node_namespace, entries);
    ret = build_names_and_types(entries, entry_count, allocator, service_names_and_types);
    pthread_mutex_unlock(&context_impl->node_mutex);
    return ret;
}

rmw_ret_t rmw_get_client_names_and_types_by_node(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                                 const char* node_name, const char* node_namespace,
                                                 rmw_names_and_types_t* service_names_and_types) {
    rmw_ret_t ret = validate_by_node_args(node, allocator, node_name, node_namespace, service_names_and_types);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;
    if (!node_name_is_registered(context_impl, node_name, node_namespace)) {
        RMW_SET_ERROR_MSG("node not found");
        return RMW_RET_NODE_NAME_NON_EXISTENT;
    }

    struct name_type_entry entries[tt_MAX_NAME_TYPE_ENTRIES];
    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);
    size_t entry_count =
        collect_by_node_name_types(context_impl, tt_KIND_SERVICE_CLIENT, node_name, node_namespace, entries);
    ret = build_names_and_types(entries, entry_count, allocator, service_names_and_types);
    pthread_mutex_unlock(&context_impl->node_mutex);
    return ret;
}

// Milestone 34's own (node_id, endpoint_id) gid encoding, reused verbatim from rmw_get_gid_for_
// publisher() (rmw_publisher.c) - a real, if not RTPS-shaped, unique-within-this-TickLE-network
// identity for any endpoint, local or remote (struct tt_DiscoveredEntity carries the same two
// fields for exactly this reason).
static void encode_gid(uint8_t node_id, uint32_t endpoint_id, uint8_t gid[RMW_GID_STORAGE_SIZE]) {
    memset(gid, 0, RMW_GID_STORAGE_SIZE);
    gid[0] = node_id;
    memcpy(&gid[1], &endpoint_id, sizeof(endpoint_id));
}

// Fills one rmw_topic_endpoint_info_t entry - shared by both the local-endpoint and discovered-
// entity halves of get_topic_endpoint_info_by_topic() below.
static rmw_ret_t populate_topic_endpoint_info(rcutils_allocator_t* allocator, const char* node_name,
                                              const char* node_namespace, const char* topic_type,
                                              rmw_endpoint_type_t endpoint_type, uint8_t node_id, uint32_t endpoint_id,
                                              const rmw_qos_profile_t* qos, rmw_topic_endpoint_info_t* info) {
    *info = rmw_get_zero_initialized_topic_endpoint_info();
    if (RMW_RET_OK != rmw_topic_endpoint_info_set_node_name(info, node_name, allocator) ||
        RMW_RET_OK != rmw_topic_endpoint_info_set_node_namespace(info, node_namespace, allocator) ||
        RMW_RET_OK != rmw_topic_endpoint_info_set_topic_type(info, topic_type, allocator) ||
        RMW_RET_OK != rmw_topic_endpoint_info_set_endpoint_type(info, endpoint_type) ||
        RMW_RET_OK != rmw_topic_endpoint_info_set_qos_profile(info, qos)) {
        return RMW_RET_BAD_ALLOC; // each setter already set its own error message
    }
    uint8_t gid[RMW_GID_STORAGE_SIZE];
    encode_gid(node_id, endpoint_id, gid);
    if (RMW_RET_OK != rmw_topic_endpoint_info_set_gid(info, gid, RMW_GID_STORAGE_SIZE)) {
        return RMW_RET_BAD_ALLOC;
    }
    return RMW_RET_OK;
}

// The actual scan+build behind rmw_get_publishers_info_by_topic()/_subscriptions_info_by_topic()
// below - unlike the names_and_types family above, every individual matching endpoint instance
// (local or remote) gets its own row here, none deduplicated by name. A remote entity's own
// node_name/node_namespace are always "" (empty, not NULL - the setters require a real C string) -
// TickLE's wire protocol has no node-name concept at all, so this is the honest answer, not a
// guess (see this file's own module doc comment). A remote entity's own qos_profile only ever
// has RELIABILITY/DURABILITY populated for real (struct tt_DiscoveredEntity.qos's own doc comment)
// - starting from rmw_qos_profile_unknown and overriding just those two matches this API's own
// documented allowance ("the only QoS policies guaranteed to be shared during discovery are the
// ones that participate in endpoint matching").
static rmw_ret_t get_topic_endpoint_info_by_topic(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                                  uint8_t kind, rmw_endpoint_type_t endpoint_type,
                                                  rcutils_allocator_t* allocator,
                                                  rmw_topic_endpoint_info_array_t* info_array) {
    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);

    size_t match_count = count_matching_locked(context_impl, topic_name, kind);
    rmw_ret_t ret = rmw_topic_endpoint_info_array_init_with_size(info_array, match_count, allocator);
    if (ret != RMW_RET_OK) {
        pthread_mutex_unlock(&context_impl->node_mutex);
        return ret; // already set its own error message
    }

    size_t index = 0;
    for (uint32_t i = 0; i < context_impl->tickle_node.endpoint_count && index < match_count; ++i) {
        struct tt_Endpoint* endpoint = context_impl->tickle_node.endpoints[i];
        if (endpoint->kind != kind || strcmp(endpoint->name, topic_name) != 0) {
            continue;
        }
        struct local_endpoint_details details = get_local_endpoint_details(endpoint);
        ret = populate_topic_endpoint_info(allocator, details.owning_node_name, details.owning_node_namespace,
                                           details.type_name, endpoint_type, context_impl->tickle_node.id, endpoint->id,
                                           details.qos, &info_array->info_array[index]);
        if (ret != RMW_RET_OK) {
            pthread_mutex_unlock(&context_impl->node_mutex);
            fini_topic_endpoint_info_array_ignore_result(info_array, allocator);
            return ret;
        }
        index++;
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES && index < match_count; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->node_id == tt_NODE_ID_INVALID || !entity->alive || entity->kind != kind ||
            strcmp(entity->name, topic_name) != 0) {
            continue;
        }
        rmw_qos_profile_t qos = rmw_qos_profile_unknown;
        qos.reliability = (entity->qos & tt_UPDATE_QOS_RELIABLE) != 0 ? RMW_QOS_POLICY_RELIABILITY_RELIABLE
                                                                      : RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
        qos.durability = (entity->qos & tt_UPDATE_QOS_DURABLE) != 0 ? RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL
                                                                    : RMW_QOS_POLICY_DURABILITY_VOLATILE;
        ret = populate_topic_endpoint_info(allocator, "", "", entity->type, endpoint_type, entity->node_id,
                                           entity->endpoint_id, &qos, &info_array->info_array[index]);
        if (ret != RMW_RET_OK) {
            pthread_mutex_unlock(&context_impl->node_mutex);
            fini_topic_endpoint_info_array_ignore_result(info_array, allocator);
            return ret;
        }
        index++;
    }

    pthread_mutex_unlock(&context_impl->node_mutex);
    return RMW_RET_OK;
}

static rmw_ret_t validate_info_by_topic_args(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                             const char* topic_name, rmw_topic_endpoint_info_array_t* info_array) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(info_array, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RCUTILS_CHECK_ALLOCATOR_WITH_MSG(allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
    if (RMW_RET_OK != rmw_topic_endpoint_info_array_check_zero(info_array)) {
        return RMW_RET_INVALID_ARGUMENT;
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_get_publishers_info_by_topic(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                           const char* topic_name, bool no_mangle,
                                           rmw_topic_endpoint_info_array_t* publishers_info) {
    (void)no_mangle; // see rmw_get_topic_names_and_types()'s own doc comment
    rmw_ret_t ret = validate_info_by_topic_args(node, allocator, topic_name, publishers_info);
    if (ret != RMW_RET_OK) {
        return ret;
    }
    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    return get_topic_endpoint_info_by_topic(node_impl->context_impl, topic_name, tt_KIND_TOPIC_PUBLISHER,
                                            RMW_ENDPOINT_PUBLISHER, allocator, publishers_info);
}

rmw_ret_t rmw_get_subscriptions_info_by_topic(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                              const char* topic_name, bool no_mangle,
                                              rmw_topic_endpoint_info_array_t* subscriptions_info) {
    (void)no_mangle; // see rmw_get_topic_names_and_types()'s own doc comment
    rmw_ret_t ret = validate_info_by_topic_args(node, allocator, topic_name, subscriptions_info);
    if (ret != RMW_RET_OK) {
        return ret;
    }
    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    return get_topic_endpoint_info_by_topic(node_impl->context_impl, topic_name, tt_KIND_TOPIC_SUBSCRIBER,
                                            RMW_ENDPOINT_SUBSCRIPTION, allocator, subscriptions_info);
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
