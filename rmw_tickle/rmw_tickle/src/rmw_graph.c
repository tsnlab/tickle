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
// Nodes (CONTEXT_NODE_PLAN.md stage 3, wire v11, 2026-09-27): a context's announce lists its nodes, each an entry
// of kind tt_KIND_NODE with its namespace, name and index, and every endpoint entry carries its node's index. So a
// remote node's name is known, and so is each remote endpoint's node: the live node entry with the same context id
// and node index (remote_node_of() below). rmw_get_node_names() and the *_by_node() queries answer for this process's
// nodes (context_impl->nodes[], Milestone 34) and for every live remote node alike. Before stage 3 the wire had no
// node concept and they answered for this process only.

#include <pthread.h> // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own <pthread.h> comment
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tickle/config.h> // tt_CONTEXT_ID_INVALID, tt_MAX_DISCOVERED_ENTITIES
#include <tickle/hal.h>    // tt_get_ns() - tt_Context_entity_alive()'s own "now" argument
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
#if defined(__has_include)
#if __has_include("rmw/get_service_endpoint_info.h")
#include "rmw/get_service_endpoint_info.h" // lyrical only - rmw_get_clients/servers_info_by_service()
#include "rmw/service_endpoint_info.h"
#include "rmw/service_endpoint_info_array.h"
#endif
#endif
#include "rmw/init.h" // rmw_context_t
#include "rmw/names_and_types.h"
#include "rmw/qos_policy_kind.h" // rmw_qos_policy_kind_t - qos_incompatible()'s own out-param
#include "rmw/qos_profiles.h"    // rmw_qos_profile_unknown
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/sanity_checks.h" // rmw_check_zero_rmw_string_array()
#include "rmw/time.h"          // rmw_time_t, RMW_DURATION_INFINITE
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
// Remote nodes (CONTEXT_NODE_PLAN.md stage 3, wire v11): a peer's announce lists its nodes as tt_KIND_NODE entries -
// namespace in `type`, name in `name`, and the node's index in its context - and every remote endpoint records the
// index of the node it belongs to. A remote endpoint's node is the live node entry with its context_id and node_index.
// Called with the context lock held.
static bool is_remote_node(const struct tt_DiscoveredEntity* entity) {
    return entity->context_id != tt_CONTEXT_ID_INVALID && entity->alive && entity->kind == tt_KIND_NODE;
}

static const struct tt_DiscoveredEntity* remote_node_of(rmw_tickle_context_impl_t* context_impl,
                                                        const struct tt_DiscoveredEntity* endpoint) {
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (is_remote_node(entity) && entity->context_id == endpoint->context_id &&
            entity->node_index == endpoint->node_index) {
            return entity;
        }
    }
    return NULL;
}

static size_t count_remote_nodes(rmw_tickle_context_impl_t* context_impl) {
    size_t count = 0;
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        count += is_remote_node(&context_impl->discovery.entities[i]) ? 1U : 0U;
    }
    return count;
}

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

    // This context's nodes, then every live remote node (stage 3). The registry lock before the context lock, the
    // order rmw_create_node()/rmw_destroy_node() take them in.
    pthread_mutex_lock(&context_impl->registry_mutex);
    tt_Context_lock(&context_impl->tickle_context);
    int count = atomic_load(&context_impl->node_count);
    size_t total = (size_t)count + count_remote_nodes(context_impl);
    if (rcutils_string_array_init(node_names, total, &node_impl->allocator) != RCUTILS_RET_OK) {
        tt_Context_unlock(&context_impl->tickle_context);
        pthread_mutex_unlock(&context_impl->registry_mutex);
        RMW_SET_ERROR_MSG("failed to allocate node_names");
        return RMW_RET_BAD_ALLOC;
    }
    if (rcutils_string_array_init(node_namespaces, total, &node_impl->allocator) != RCUTILS_RET_OK) {
        tt_Context_unlock(&context_impl->tickle_context);
        pthread_mutex_unlock(&context_impl->registry_mutex);
        RMW_SET_ERROR_MSG("failed to allocate node_namespaces");
        fini_string_array_ignore_result(node_names);
        return RMW_RET_BAD_ALLOC;
    }
    bool alloc_failed = false;
    size_t filled = 0;
    for (int i = 0; i < count && !alloc_failed; i++, filled++) {
        node_names->data[filled] = rcutils_strdup(context_impl->nodes[i]->rmw_node.name, node_impl->allocator);
        node_namespaces->data[filled] =
            rcutils_strdup(context_impl->nodes[i]->rmw_node.namespace_, node_impl->allocator);
        alloc_failed = NULL == node_names->data[filled] || NULL == node_namespaces->data[filled];
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES && filled < total && !alloc_failed; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (!is_remote_node(entity)) {
            continue;
        }
        node_names->data[filled] = rcutils_strdup(entity->name, node_impl->allocator);
        node_namespaces->data[filled] = rcutils_strdup(entity->type, node_impl->allocator);
        alloc_failed = NULL == node_names->data[filled] || NULL == node_namespaces->data[filled];
        filled++;
    }
    tt_Context_unlock(&context_impl->tickle_context);
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
// the node lock is already held by the caller - see rmw_tickle_count_matching_locked()'s
// own doc comment for why that one can't take it itself. Only ever counts *alive* discovery
// entries - Milestone 62 (rmw_tickle/PLAN.md's own "DDS semantic-parity backlog" row 3) switched
// this from reading struct tt_DiscoveredEntity.alive directly to tt_Context_entity_alive() (tickle.h),
// computed fresh against each entity's own liveliness_lease_duration_ns when it requested one,
// instead of only ever reflecting check_liveliness()'s own coarser ~3s node-level sweep - see that
// function's own doc comment for the full "why". A tombstoned/expired one shouldn't count as
// "currently offered/requested" for rmw_count_publishers()/_subscribers() or RMW_EVENT_LIVELINESS_
// CHANGED's own alive_count either; see count_not_alive_matching_locked() below for its own
// counterpart. Local endpoints have no tombstone concept at all - they're either present in
// tickle_context.endpoints[] or destroyed outright, so no matching check is needed for them.
static size_t count_matching_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name, uint8_t kind) {
    size_t matched = 0;
    for (uint32_t i = 0; i < context_impl->tickle_context.endpoint_count; ++i) {
        const struct tt_Endpoint* endpoint = context_impl->tickle_context.endpoints[i];
        if (endpoint->kind == kind && strcmp(endpoint->name, topic_name) == 0) {
            matched++;
        }
    }
    uint64_t now = tt_get_ns();
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->context_id != tt_CONTEXT_ID_INVALID &&
            tt_Context_entity_alive(&context_impl->tickle_context, entity, now) && entity->kind == kind &&
            strcmp(entity->name, topic_name) == 0) {
            matched++;
        }
    }
    return matched;
}

// count_matching_locked()'s own tombstone counterpart - QoS roadmap #3 (LIVELINESS)'s own
// RMW_EVENT_LIVELINESS_CHANGED.not_alive_count (a live snapshot, rmw_subscription.c/rmw_event.c),
// now backed by real data (tt_Context_entity_alive(), see count_matching_locked()'s own doc comment
// for why this reads that instead of struct tt_DiscoveredEntity.alive directly since Milestone 62)
// instead of always 0. Local endpoints are never counted here for the same reason count_matching_
// locked() never checks them for aliveness - no tombstone concept applies to them.
static size_t count_not_alive_matching_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                              uint8_t kind) {
    size_t matched = 0;
    uint64_t now = tt_get_ns();
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->context_id != tt_CONTEXT_ID_INVALID &&
            !tt_Context_entity_alive(&context_impl->tickle_context, entity, now) && entity->kind == kind &&
            strcmp(entity->name, topic_name) == 0) {
            matched++;
        }
    }
    return matched;
}

// rmw_tickle.h's own declaration - see this file's own module doc comment for why both the local
// endpoint table and the remote discovery table need scanning. Callable from any thread except
// the poll thread itself mid-tt_Context_poll() (see count_matching_locked()'s own doc comment).
size_t rmw_tickle_count_matching_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name, uint8_t kind) {
    return count_matching_locked(context_impl, topic_name, kind);
}

// rmw_tickle.h's own declaration - see count_not_alive_matching_locked()'s own doc comment.
size_t rmw_tickle_count_not_alive_matching_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                                  uint8_t kind) {
    return count_not_alive_matching_locked(context_impl, topic_name, kind);
}

// Milestone 31/28(a) + #2 (DEADLINE)/#3 (LIVELINESS) RxO (Milestone 49) observability follow-on -
// RMW_EVENT_OFFERED_QOS_INCOMPATIBLE/RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE's own real DDS RxO
// comparison, generalized to whichever side is "requesting" vs "offering" - true if a pairing
// requesting these values can never be satisfied by one offering them. Mirrors tickle.c's own
// subscriber_incompatible_with_publisher()/deadline_liveliness_incompatible() exactly (same
// fields, same "requested but not offered" direction, same priority order when several mismatch -
// real DDS's own wording only ever promises "one of the policies", not a specific one) - a
// separate implementation, not a shared call, since TickLE core has no rmw_qos_profile_t concept
// at all (ROS/rmw-agnostic) to call this file's own code with, or vice versa. Evaluated here
// against struct tt_DiscoveredEntity's own fields (already populated by Milestone 31/49) instead
// of a live DATA/UPDATE packet's own sender - see this function's own two callers below for which
// side's own local values play which role. Reports which policy was found incompatible via
// *out_kind.
static bool qos_incompatible(bool requested_reliable, bool requested_durable, bool requested_manual,
                             uint64_t requested_deadline_ns, uint64_t requested_lease_ns, bool offered_reliable,
                             bool offered_durable, bool offered_manual, uint64_t offered_deadline_ns,
                             uint64_t offered_lease_ns, rmw_qos_policy_kind_t* out_kind) {
    if (requested_reliable && !offered_reliable) {
        *out_kind = RMW_QOS_POLICY_RELIABILITY;
        return true;
    }
    if (requested_durable && !offered_durable) {
        *out_kind = RMW_QOS_POLICY_DURABILITY;
        return true;
    }
    if (requested_deadline_ns != 0 && (offered_deadline_ns == 0 || offered_deadline_ns > requested_deadline_ns)) {
        *out_kind = RMW_QOS_POLICY_DEADLINE;
        return true;
    }
    if (requested_manual && !offered_manual) {
        *out_kind = RMW_QOS_POLICY_LIVELINESS;
        return true;
    }
    if (requested_lease_ns != 0 && (offered_lease_ns == 0 || offered_lease_ns > requested_lease_ns)) {
        *out_kind = RMW_QOS_POLICY_LIVELINESS;
        return true;
    }
    return false;
}

// rmw_tickle.h's own declaration - RMW_EVENT_OFFERED_QOS_INCOMPATIBLE's own live count: how many
// currently-alive discovered remote Subscribers on `topic_name` request something this Publisher
// (offering `offered_*`) doesn't. Same "poll-thread-only, no locking of its own" rule as count_
// matching_locked() - called only from check_publisher_qos_incompatible() (rmw_publisher.c),
// which already holds the node lock via the same tt_Context_schedule()-callback contract
// that function's own doc comment explains.
size_t rmw_tickle_count_incompatible_subscribers_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                                        bool offered_reliable, bool offered_durable,
                                                        bool offered_manual, uint64_t offered_deadline_ns,
                                                        uint64_t offered_lease_ns,
                                                        rmw_qos_policy_kind_t* out_last_policy_kind) {
    size_t matched = 0;
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->context_id == tt_CONTEXT_ID_INVALID || !entity->alive || entity->kind != tt_KIND_TOPIC_SUBSCRIBER ||
            strcmp(entity->name, topic_name) != 0) {
            continue;
        }
        bool requested_reliable = (entity->qos & tt_UPDATE_QOS_RELIABLE) != 0;
        bool requested_durable = (entity->qos & tt_UPDATE_QOS_DURABLE) != 0;
        bool requested_manual = (entity->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0;
        rmw_qos_policy_kind_t kind;
        if (qos_incompatible(requested_reliable, requested_durable, requested_manual, entity->deadline_duration_ns,
                             entity->liveliness_lease_duration_ns, offered_reliable, offered_durable, offered_manual,
                             offered_deadline_ns, offered_lease_ns, &kind)) {
            matched++;
            *out_last_policy_kind = kind;
        }
    }
    return matched;
}

// rmw_tickle.h's own declaration - RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE's own counterpart: how
// many currently-alive discovered remote Publishers on `topic_name` offer less than this
// Subscription (requesting `requested_*`) needs. Called only from check_subscription_qos_
// incompatible() (rmw_subscription.c) - same threading rule.
size_t rmw_tickle_count_incompatible_publishers_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                                       bool requested_reliable, bool requested_durable,
                                                       bool requested_manual, uint64_t requested_deadline_ns,
                                                       uint64_t requested_lease_ns,
                                                       rmw_qos_policy_kind_t* out_last_policy_kind) {
    size_t matched = 0;
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->context_id == tt_CONTEXT_ID_INVALID || !entity->alive || entity->kind != tt_KIND_TOPIC_PUBLISHER ||
            strcmp(entity->name, topic_name) != 0) {
            continue;
        }
        bool offered_reliable = (entity->qos & tt_UPDATE_QOS_RELIABLE) != 0;
        bool offered_durable = (entity->qos & tt_UPDATE_QOS_DURABLE) != 0;
        bool offered_manual = (entity->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0;
        rmw_qos_policy_kind_t kind;
        if (qos_incompatible(requested_reliable, requested_durable, requested_manual, requested_deadline_ns,
                             requested_lease_ns, offered_reliable, offered_durable, offered_manual,
                             entity->deadline_duration_ns, entity->liveliness_lease_duration_ns, &kind)) {
            matched++;
            *out_last_policy_kind = kind;
        }
    }
    return matched;
}

// The tt_Context_interrupt()-then-lock-then-scan-then-unlock sequence every count_matching_locked()
// caller in this file needs - split out once both rmw_count_publishers()/_subscribers() (below)
// and rmw_publisher_count_matched_subscriptions()/rmw_subscription_count_matched_publishers()
// (rmw_tickle/PLAN.md's remaining-rmw-API-surface backlog) needed the identical sequence, just
// against a different topic_name each time - always the same shared context_impl now (Milestone
// 34), never a specific rmw_tickle_node_t.
static size_t count_matching_via_context_impl(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                              uint8_t kind) {
    tt_Context_lock(&context_impl->tickle_context);
    size_t matched = count_matching_locked(context_impl, topic_name, kind);
    tt_Context_unlock(&context_impl->tickle_context);
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

// (g3, RMW_GAPS_PLAN.md) What "matched" means, for rmw_*_count_matched_*() and the MATCHED events alike: an
// endpoint of the other kind on the same topic, with the same type, whose QoS is compatible (the RxO rule behind
// QOS_INCOMPATIBLE, qos_incompatible() above), and - a remote one - alive. Until g3 these counts went by topic name
// alone, so a type-mismatched or incompatible endpoint counted as matched; the vendors match neither.
//
// One side of a pairing, as qos_incompatible() compares them.
struct pairing_qos {
    bool reliable;
    bool durable;
    bool manual;
    uint64_t deadline_ns;
    uint64_t lease_ns;
};

static struct pairing_qos entity_qos(const struct tt_DiscoveredEntity* entity) {
    return (struct pairing_qos) {(entity->qos & tt_UPDATE_QOS_RELIABLE) != 0,
                                 (entity->qos & tt_UPDATE_QOS_DURABLE) != 0,
                                 (entity->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0, entity->deadline_duration_ns,
                                 entity->liveliness_lease_duration_ns};
}
static struct pairing_qos publisher_qos(const struct tt_Publisher* pub) {
    return (struct pairing_qos) {pub->reliable, pub->durable, pub->liveliness_manual, pub->deadline_duration_ns,
                                 pub->liveliness_lease_duration_ns};
}
static struct pairing_qos subscriber_qos(const struct tt_Subscriber* sub) {
    return (struct pairing_qos) {sub->reliable, sub->durable, sub->liveliness_manual, sub->deadline_duration_ns,
                                 sub->liveliness_lease_duration_ns};
}
static bool compatible(const struct pairing_qos* requested, const struct pairing_qos* offered) {
    rmw_qos_policy_kind_t kind;
    return !qos_incompatible(requested->reliable, requested->durable, requested->manual, requested->deadline_ns,
                             requested->lease_ns, offered->reliable, offered->durable, offered->manual,
                             offered->deadline_ns, offered->lease_ns, &kind);
}

// A local topic endpoint's type name and QoS, recovered from its rmw wrapper (get_local_endpoint_details() below
// does the same for the graph queries).
static const char* local_type_and_qos(const struct tt_Endpoint* endpoint, struct pairing_qos* qos) {
    if (endpoint->kind == tt_KIND_TOPIC_PUBLISHER) {
        const rmw_tickle_publisher_t* pub_impl =
            (const rmw_tickle_publisher_t*)((const char*)endpoint - offsetof(rmw_tickle_publisher_t, tickle_publisher));
        *qos = publisher_qos(&pub_impl->tickle_publisher);
        return pub_impl->topic.name;
    }
    const rmw_tickle_subscriber_t* sub_impl =
        (const rmw_tickle_subscriber_t*)((const char*)endpoint - offsetof(rmw_tickle_subscriber_t, tickle_subscriber));
    *qos = subscriber_qos(&sub_impl->tickle_subscriber);
    return sub_impl->topic.name;
}

// For one local topic endpoint: the endpoints of the other kind it is matched with, and those on its topic with
// another type. A remote Subscription counts for a Publisher only once the core has registered its node as that
// Publisher's peer - when its samples start going there by unicast; counting it on discovery alone (as until
// 2026-09-26) reported a match before the peer was registered, so a caller waiting for one - the ping-pong's ping -
// sent its first sample by broadcast (Plan's M6 capture: 5 of 24 pings). With the peer table full the core
// broadcasts to everyone, so every discovered Subscription counts.
struct match_counts {
    size_t matched;
    size_t other_type;
};

static bool publisher_has_peer(const struct tt_Publisher* pub, uint8_t context_id) {
    size_t peers = 0;
    for (int i = 0; i < tt_MAX_PEER_COUNT; ++i) {
        if (pub->peers[i].context_id == context_id) {
            return true;
        }
        peers += pub->peers[i].context_id != tt_CONTEXT_ID_INVALID;
    }
    return peers >= tt_MAX_PEER_COUNT;
}

static struct match_counts count_matches_locked(rmw_tickle_context_impl_t* context_impl,
                                                const struct tt_Endpoint* self) {
    struct match_counts counts = {0, 0};
    struct pairing_qos own;
    const char* own_type = local_type_and_qos(self, &own);
    bool self_publishes = self->kind == tt_KIND_TOPIC_PUBLISHER;
    uint8_t other_kind = self_publishes ? tt_KIND_TOPIC_SUBSCRIBER : tt_KIND_TOPIC_PUBLISHER;
    for (uint32_t i = 0; i < context_impl->tickle_context.endpoint_count; ++i) {
        const struct tt_Endpoint* endpoint = context_impl->tickle_context.endpoints[i];
        if (endpoint->kind != other_kind || strcmp(endpoint->name, self->name) != 0) {
            continue;
        }
        struct pairing_qos other;
        const char* other_type = local_type_and_qos(endpoint, &other);
        if (strcmp(other_type, own_type) != 0) {
            counts.other_type++;
        } else if (self_publishes ? compatible(&other, &own) : compatible(&own, &other)) {
            counts.matched++;
        }
    }
    uint64_t now = tt_get_ns();
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->context_id == tt_CONTEXT_ID_INVALID || entity->kind != other_kind ||
            strcmp(entity->name, self->name) != 0 ||
            !tt_Context_entity_alive(&context_impl->tickle_context, entity, now)) {
            continue;
        }
        if (strcmp(entity->type, own_type) != 0) {
            counts.other_type++;
            continue;
        }
        struct pairing_qos other = entity_qos(entity);
        bool matched = self_publishes ? compatible(&other, &own) &&
                                            publisher_has_peer((const struct tt_Publisher*)self, entity->context_id)
                                      : compatible(&own, &other);
        counts.matched += matched ? 1U : 0U;
    }
    return counts;
}

size_t rmw_tickle_count_matched_subscriptions_locked(rmw_tickle_publisher_t* pub_impl) {
    return count_matches_locked(pub_impl->node->context_impl, &pub_impl->tickle_publisher.endpoint).matched;
}

size_t rmw_tickle_count_matched_publishers_locked(rmw_tickle_subscriber_t* sub_impl) {
    return count_matches_locked(sub_impl->node->context_impl, &sub_impl->tickle_subscriber.endpoint).matched;
}

static void raise_event(rmw_tickle_context_impl_t* context_impl, rmw_tickle_event_status_t* status, int count) {
    atomic_fetch_add(&status->unread_count, count);
    pthread_mutex_lock(&context_impl->wait_mutex);
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);
    rmw_tickle_poke_polling_executor(context_impl);
    rmw_tickle_callback_slot_notify(&status->callback, (size_t)count); // (g2)
}

// A new count for one endpoint's MATCHED and INCOMPATIBLE_TYPE: every increase adds to total_count, and every change
// of the matched count is one unread change.
static void apply_counts(rmw_tickle_context_impl_t* context_impl, rmw_tickle_matched_status_t* matched,
                         rmw_tickle_incompatible_type_status_t* other_type, struct match_counts counts) {
    int previous = atomic_load(&matched->current_count);
    int current = (int)counts.matched;
    if (current != previous) {
        atomic_store(&matched->current_count, current);
        if (current > previous) {
            atomic_fetch_add(&matched->base.total_count, current - previous);
        }
        raise_event(context_impl, &matched->base, current > previous ? current - previous : previous - current);
    }
    int incompatible = (int)counts.other_type;
    if (incompatible > other_type->last_count) {
        atomic_fetch_add(&other_type->base.total_count, incompatible - other_type->last_count);
        raise_event(context_impl, &other_type->base, incompatible - other_type->last_count);
    }
    other_type->last_count = incompatible;
}

void rmw_tickle_update_matches_locked(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                      uint8_t local_kind) {
    for (uint32_t i = 0; i < context_impl->tickle_context.endpoint_count; ++i) {
        struct tt_Endpoint* endpoint = context_impl->tickle_context.endpoints[i];
        if ((endpoint->kind != tt_KIND_TOPIC_PUBLISHER && endpoint->kind != tt_KIND_TOPIC_SUBSCRIBER) ||
            (local_kind != 0 && endpoint->kind != local_kind) ||
            (topic_name != NULL && strcmp(endpoint->name, topic_name) != 0)) {
            continue;
        }
        struct match_counts counts = count_matches_locked(context_impl, endpoint);
        if (endpoint->kind == tt_KIND_TOPIC_PUBLISHER) {
            rmw_tickle_publisher_t* pub_impl =
                (rmw_tickle_publisher_t*)((char*)endpoint - offsetof(rmw_tickle_publisher_t, tickle_publisher));
            apply_counts(context_impl, &pub_impl->matched, &pub_impl->incompatible_type, counts);
        } else {
            rmw_tickle_subscriber_t* sub_impl =
                (rmw_tickle_subscriber_t*)((char*)endpoint - offsetof(rmw_tickle_subscriber_t, tickle_subscriber));
            apply_counts(context_impl, &sub_impl->matched, &sub_impl->incompatible_type, counts);
        }
    }
}

rmw_ret_t rmw_publisher_count_matched_subscriptions(const rmw_publisher_t* publisher, size_t* subscription_count) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription_count, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    tt_Context_lock(&pub_impl->node->context_impl->tickle_context);
    *subscription_count = rmw_tickle_count_matched_subscriptions_locked(pub_impl);
    tt_Context_unlock(&pub_impl->node->context_impl->tickle_context);
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
    tt_Context_lock(&sub_impl->node->context_impl->tickle_context);
    *publisher_count = rmw_tickle_count_matched_publishers_locked(sub_impl);
    tt_Context_unlock(&sub_impl->node->context_impl->tickle_context);
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
// already hold the node lock for as long as the raw name/type pointers gathered here
// stay in use - build_names_and_types() below strdup()s them before any caller may unlock.
static size_t collect_graph_wide_name_types(rmw_tickle_context_impl_t* context_impl, uint8_t kind_a, uint8_t kind_b,
                                            struct name_type_entry* entries) {
    size_t count = 0;
    for (uint32_t i = 0; i < context_impl->tickle_context.endpoint_count; ++i) {
        struct tt_Endpoint* endpoint = context_impl->tickle_context.endpoints[i];
        if (endpoint->kind != kind_a && endpoint->kind != kind_b) {
            continue;
        }
        struct local_endpoint_details details = get_local_endpoint_details(endpoint);
        count = add_name_type_entry(entries, count, endpoint->name, details.type_name);
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->context_id == tt_CONTEXT_ID_INVALID || !entity->alive) {
            continue;
        }
        if (entity->kind != kind_a && entity->kind != kind_b) {
            continue;
        }
        count = add_name_type_entry(entries, count, entity->name, entity->type);
    }
    return count;
}

// The actual scan behind the four *_by_node() queries below: every distinct (name, type) pair for ONE kind owned by
// the named node - a local one, or a live remote one (stage 3: its endpoints are those with its context id and node
// index). Same locking contract as collect_graph_wide_name_types() above.
static size_t collect_by_node_name_types(rmw_tickle_context_impl_t* context_impl, uint8_t kind,
                                         const char* owning_node_name, const char* owning_node_namespace,
                                         struct name_type_entry* entries) {
    size_t count = 0;
    for (uint32_t i = 0; i < context_impl->tickle_context.endpoint_count; ++i) {
        struct tt_Endpoint* endpoint = context_impl->tickle_context.endpoints[i];
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
    // A remote node's endpoints (stage 3): every live endpoint of `kind` whose (context id, node index) is that of a
    // live remote node entry with this name and namespace - more than one context may host a node so named.
    for (uint32_t slot = 0; slot < tt_MAX_DISCOVERED_ENTITIES; ++slot) {
        const struct tt_DiscoveredEntity* owner = &context_impl->discovery.entities[slot];
        if (!is_remote_node(owner) || strcmp(owner->name, owning_node_name) != 0 ||
            strcmp(owner->type, owning_node_namespace) != 0) {
            continue;
        }
        for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES; ++i) {
            const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
            if (entity->context_id == owner->context_id && entity->alive && entity->kind == kind &&
                entity->node_index == owner->node_index) {
                count = add_name_type_entry(entries, count, entity->name, entity->type);
            }
        }
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

    tt_Context_lock(&context_impl->tickle_context);
    size_t entry_count =
        collect_graph_wide_name_types(context_impl, tt_KIND_TOPIC_PUBLISHER, tt_KIND_TOPIC_SUBSCRIBER, entries);
    ret = build_names_and_types(entries, entry_count, allocator, topic_names_and_types);
    tt_Context_unlock(&context_impl->tickle_context);
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

    tt_Context_lock(&context_impl->tickle_context);
    size_t entry_count =
        collect_graph_wide_name_types(context_impl, tt_KIND_SERVICE_SERVER, tt_KIND_SERVICE_CLIENT, entries);
    ret = build_names_and_types(entries, entry_count, allocator, service_names_and_types);
    tt_Context_unlock(&context_impl->tickle_context);
    return ret;
}

// Whether a node of this name and namespace exists: one of this process's (context_impl->nodes[], Milestone 34) or a
// live remote one (stage 3). Anything else is RMW_RET_NODE_NAME_NON_EXISTENT for the four *_by_node() queries below.
static bool node_name_is_registered(rmw_tickle_context_impl_t* context_impl, const char* node_name,
                                    const char* node_namespace) {
    pthread_mutex_lock(&context_impl->registry_mutex);
    int count = atomic_load(&context_impl->node_count);
    bool found = false;
    for (int i = 0; i < count && !found; i++) {
        found = strcmp(context_impl->nodes[i]->rmw_node.name, node_name) == 0 &&
                strcmp(context_impl->nodes[i]->rmw_node.namespace_, node_namespace) == 0;
    }
    // Or a live remote node (stage 3).
    tt_Context_lock(&context_impl->tickle_context);
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES && !found; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        found =
            is_remote_node(entity) && strcmp(entity->name, node_name) == 0 && strcmp(entity->type, node_namespace) == 0;
    }
    tt_Context_unlock(&context_impl->tickle_context);
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
    tt_Context_lock(&context_impl->tickle_context);
    size_t entry_count =
        collect_by_node_name_types(context_impl, tt_KIND_TOPIC_SUBSCRIBER, node_name, node_namespace, entries);
    ret = build_names_and_types(entries, entry_count, allocator, topic_names_and_types);
    tt_Context_unlock(&context_impl->tickle_context);
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
    tt_Context_lock(&context_impl->tickle_context);
    size_t entry_count =
        collect_by_node_name_types(context_impl, tt_KIND_TOPIC_PUBLISHER, node_name, node_namespace, entries);
    ret = build_names_and_types(entries, entry_count, allocator, topic_names_and_types);
    tt_Context_unlock(&context_impl->tickle_context);
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
    tt_Context_lock(&context_impl->tickle_context);
    size_t entry_count =
        collect_by_node_name_types(context_impl, tt_KIND_SERVICE_SERVER, node_name, node_namespace, entries);
    ret = build_names_and_types(entries, entry_count, allocator, service_names_and_types);
    tt_Context_unlock(&context_impl->tickle_context);
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
    tt_Context_lock(&context_impl->tickle_context);
    size_t entry_count =
        collect_by_node_name_types(context_impl, tt_KIND_SERVICE_CLIENT, node_name, node_namespace, entries);
    ret = build_names_and_types(entries, entry_count, allocator, service_names_and_types);
    tt_Context_unlock(&context_impl->tickle_context);
    return ret;
}

// Milestone 34's own (node_id, endpoint_id) gid encoding, reused verbatim from rmw_get_gid_for_
// publisher() (rmw_publisher.c) - a real, if not RTPS-shaped, unique-within-this-TickLE-network
// identity for any endpoint, local or remote (struct tt_DiscoveredEntity carries the same two
// fields for exactly this reason).
// The gid a tool uses to tell one endpoint from another, in rmw_get_gid_for_publisher()'s layout -
// which the comment here used to claim was "reused verbatim" while this encoded something else.
//
// `entity_id`, not endpoint_id. endpoint_id is hash(topic/service name + endpoint name), so two
// publishers of one topic share it BY CONSTRUCTION and so shared a gid - the collision Milestone 47
// removed from rmw_get_gid_for_publisher() by moving it off that hash, reintroduced here. Measured
// 2026-10-02 by rmw_gap_acceptance.sh names: rmw_tickle FAIL (two_writers_share_a_graph_gid) where
// the rmw_cyclonedds_cpp control PASSED.
//
// Both halves now: local endpoints give their own endpoint->entity_id, and discovered ones give
// tt_DiscoveredEntity.entity_id, which the announce had always carried (tt_UpdateEntity.entity_id,
// Phase 2) and the discovery cache had not kept.
//
// Note what this does NOT buy, because the obvious stronger claim is false: a sample's gid and the
// graph's gid for the same writer are still not required to be equal, and the control proves it -
// CycloneDDS reports 011074bc... in the graph and ee9da3db... on the sample, two encodings of one
// writer. Equality is not an rmw guarantee and a test asserting it fails everywhere. Uniqueness is
// the property the field exists for, and it is the one that was broken.
static void encode_gid(uint8_t node_id, uint32_t entity_id, uint8_t gid[RMW_GID_STORAGE_SIZE]) {
    memset(gid, 0, RMW_GID_STORAGE_SIZE);
    gid[0] = node_id;
    memcpy(&gid[1], &entity_id, sizeof(entity_id));
}

// Fills one rmw_topic_endpoint_info_t entry - shared by both the local-endpoint and discovered-
// entity halves of get_topic_endpoint_info_by_topic() below.
static rmw_ret_t populate_topic_endpoint_info(rcutils_allocator_t* allocator, const char* node_name,
                                              const char* node_namespace, const char* topic_type,
                                              rmw_endpoint_type_t endpoint_type, uint8_t node_id, uint32_t entity_id,
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
    encode_gid(node_id, entity_id, gid);
    if (RMW_RET_OK != rmw_topic_endpoint_info_set_gid(info, gid, RMW_GID_STORAGE_SIZE)) {
        return RMW_RET_BAD_ALLOC;
    }
    return RMW_RET_OK;
}

// rmw_qos.c has its own NSEC_PER_SEC for the same split; these are two translation units and
// neither exports it, so it is named per file rather than shared through a header for one constant.
#define GRAPH_NSEC_PER_SEC 1000000000ULL

// An announced duration as rmw states one. TickLE's wire convention is that 0 means "no
// requirement", and rmw/DDS spell that RMW_DURATION_INFINITE rather than zero - a literal 0 here
// would read as "a deadline of no time at all", which is the opposite. CycloneDDS reports the same
// infinity for an unset deadline, which is what the bag acceptance's control arm shows.
static rmw_time_t announced_duration(uint64_t duration_ns) {
    if (0 == duration_ns) {
        return (rmw_time_t)RMW_DURATION_INFINITE;
    }
    return (rmw_time_t) {.sec = duration_ns / GRAPH_NSEC_PER_SEC, .nsec = duration_ns % GRAPH_NSEC_PER_SEC};
}

// The actual scan+build behind rmw_get_publishers_info_by_topic()/_subscriptions_info_by_topic()
// below - unlike the names_and_types family above, every individual matching endpoint instance
// (local or remote) gets its own row here, none deduplicated by name. A remote entity's node_name/node_namespace are
// its node's (stage 3, remote_node_of()), or "" if that node's entry is not known (empty, not NULL - the setters
// require a real C string). A remote entity's own qos_profile only ever
// has RELIABILITY/DURABILITY populated for real (struct tt_DiscoveredEntity.qos's own doc comment)
// - starting from rmw_qos_profile_unknown and overriding just those two matches this API's own
// documented allowance ("the only QoS policies guaranteed to be shared during discovery are the
// ones that participate in endpoint matching").
static rmw_ret_t get_topic_endpoint_info_by_topic(rmw_tickle_context_impl_t* context_impl, const char* topic_name,
                                                  uint8_t kind, rmw_endpoint_type_t endpoint_type,
                                                  rcutils_allocator_t* allocator,
                                                  rmw_topic_endpoint_info_array_t* info_array) {
    tt_Context_lock(&context_impl->tickle_context);

    size_t match_count = count_matching_locked(context_impl, topic_name, kind);
    rmw_ret_t ret = rmw_topic_endpoint_info_array_init_with_size(info_array, match_count, allocator);
    if (ret != RMW_RET_OK) {
        tt_Context_unlock(&context_impl->tickle_context);
        return ret; // already set its own error message
    }

    size_t index = 0;
    for (uint32_t i = 0; i < context_impl->tickle_context.endpoint_count && index < match_count; ++i) {
        struct tt_Endpoint* endpoint = context_impl->tickle_context.endpoints[i];
        if (endpoint->kind != kind || strcmp(endpoint->name, topic_name) != 0) {
            continue;
        }
        struct local_endpoint_details details = get_local_endpoint_details(endpoint);
        ret = populate_topic_endpoint_info(allocator, details.owning_node_name, details.owning_node_namespace,
                                           details.type_name, endpoint_type, context_impl->tickle_context.id,
                                           endpoint->entity_id, details.qos, &info_array->info_array[index]);
        if (ret != RMW_RET_OK) {
            tt_Context_unlock(&context_impl->tickle_context);
            fini_topic_endpoint_info_array_ignore_result(info_array, allocator);
            return ret;
        }
        index++;
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES && index < match_count; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->context_id == tt_CONTEXT_ID_INVALID || !entity->alive || entity->kind != kind ||
            strcmp(entity->name, topic_name) != 0) {
            continue;
        }
        rmw_qos_profile_t qos = rmw_qos_profile_unknown;
        qos.reliability = (entity->qos & tt_UPDATE_QOS_RELIABLE) != 0 ? RMW_QOS_POLICY_RELIABILITY_RELIABLE
                                                                      : RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
        qos.durability = (entity->qos & tt_UPDATE_QOS_DURABLE) != 0 ? RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL
                                                                    : RMW_QOS_POLICY_DURABILITY_VOLATILE;
        // LIVELINESS and the two durations are announced as well (struct tt_DiscoveredEntity's own
        // qos/deadline_duration_ns/liveliness_lease_duration_ns), and they participate in endpoint
        // matching, so they are exactly what this API guarantees discovery shares - leaving them
        // UNKNOWN was not caution, it was a value we do not have to invent being left out.
        //
        // It was also incoherent, which is how it was found (RMW_GAPS_PLAN g13, the bag
        // acceptance). rosbag2 records the publisher's reported QoS into the bag and offers it
        // again on playback, so `ros2 bag play` asked rmw_tickle to create a publisher with
        // liveliness UNKNOWN - and rmw_tickle_validate_qos_profile() refuses UNKNOWN, so we
        // rejected a profile we had ourselves reported. Recording worked, playback published
        // nothing, and the topic was skipped with a QoS error naming our own validator.
        qos.liveliness = (entity->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0
                             ? RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC
                             : RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
        qos.deadline = announced_duration(entity->deadline_duration_ns);
        qos.liveliness_lease_duration = announced_duration(entity->liveliness_lease_duration_ns);
        const struct tt_DiscoveredEntity* owner = remote_node_of(context_impl, entity); // stage 3
        ret = populate_topic_endpoint_info(allocator, owner != NULL ? owner->name : "",
                                           owner != NULL ? owner->type : "", entity->type, endpoint_type,
                                           entity->context_id, entity->entity_id, &qos, &info_array->info_array[index]);
        if (ret != RMW_RET_OK) {
            tt_Context_unlock(&context_impl->tickle_context);
            fini_topic_endpoint_info_array_ignore_result(info_array, allocator);
            return ret;
        }
        index++;
    }

    tt_Context_unlock(&context_impl->tickle_context);
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

// (g3, RMW_GAPS_PLAN.md) Lyrical's service-side counterparts of the two topic queries above: every client or server of
// a service, local and remote, one row each, with its node as stage 3 names it. A TickLE client or server is one
// endpoint, so each row has endpoint_count 1 (a DDS vendor lists two, its request and reply readers/writers). QoS: a
// local one's is the services default rmw_tickle serves them with; a remote one's is unknown, as its announce does
// not carry it. Jazzy's rmw has neither function nor header.
#if defined(__has_include)
#if __has_include("rmw/get_service_endpoint_info.h")
static rmw_ret_t populate_service_endpoint_info(rcutils_allocator_t* allocator, const char* node_name,
                                                const char* node_namespace, const char* service_type,
                                                rmw_endpoint_type_t endpoint_type, uint8_t node_id, uint32_t entity_id,
                                                const rmw_qos_profile_t* qos, rmw_service_endpoint_info_t* info) {
    *info = rmw_get_zero_initialized_service_endpoint_info();
    uint8_t gid[RMW_GID_STORAGE_SIZE];
    encode_gid(node_id, entity_id, gid);
    if (RMW_RET_OK != rmw_service_endpoint_info_set_node_name(info, node_name, allocator) ||
        RMW_RET_OK != rmw_service_endpoint_info_set_node_namespace(info, node_namespace, allocator) ||
        RMW_RET_OK != rmw_service_endpoint_info_set_service_type(info, service_type, allocator) ||
        RMW_RET_OK != rmw_service_endpoint_info_set_endpoint_type(info, endpoint_type) ||
        RMW_RET_OK != rmw_service_endpoint_info_set_endpoint_count(info, 1) ||
        RMW_RET_OK != rmw_service_endpoint_info_set_gids(info, gid, 1, RMW_GID_STORAGE_SIZE, allocator) ||
        RMW_RET_OK != rmw_service_endpoint_info_set_qos_profiles(info, qos, 1, allocator)) {
        return RMW_RET_BAD_ALLOC; // each setter already set its own error message
    }
    return RMW_RET_OK;
}

static void fini_service_endpoint_info_array_ignore_result(rmw_service_endpoint_info_array_t* info_array,
                                                           rcutils_allocator_t* allocator) {
    rmw_ret_t ret = rmw_service_endpoint_info_array_fini(info_array, allocator);
    (void)ret;
}

static bool is_remote_service_endpoint(rmw_tickle_context_impl_t* context_impl,
                                       const struct tt_DiscoveredEntity* entity, uint8_t kind, const char* service_name,
                                       uint64_t now) {
    return entity->context_id != tt_CONTEXT_ID_INVALID && entity->kind == kind &&
           strcmp(entity->name, service_name) == 0 &&
           tt_Context_entity_alive(&context_impl->tickle_context, entity, now);
}

static rmw_ret_t fill_service_endpoint_info_locked(rmw_tickle_context_impl_t* context_impl, const char* service_name,
                                                   uint8_t kind, rmw_endpoint_type_t endpoint_type,
                                                   rcutils_allocator_t* allocator,
                                                   rmw_service_endpoint_info_array_t* info_array) {
    size_t index = 0;
    for (uint32_t i = 0; i < context_impl->tickle_context.endpoint_count && index < info_array->size; ++i) {
        struct tt_Endpoint* endpoint = context_impl->tickle_context.endpoints[i];
        if (endpoint->kind != kind || strcmp(endpoint->name, service_name) != 0) {
            continue;
        }
        struct local_endpoint_details details = get_local_endpoint_details(endpoint);
        rmw_ret_t ret = populate_service_endpoint_info(
            allocator, details.owning_node_name, details.owning_node_namespace, details.type_name, endpoint_type,
            context_impl->tickle_context.id, endpoint->entity_id, &rmw_qos_profile_services_default,
            &info_array->info_array[index++]);
        if (ret != RMW_RET_OK) {
            return ret;
        }
    }
    uint64_t now = tt_get_ns();
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES && index < info_array->size; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (!is_remote_service_endpoint(context_impl, entity, kind, service_name, now)) {
            continue;
        }
        const struct tt_DiscoveredEntity* owner = remote_node_of(context_impl, entity); // stage 3
        rmw_ret_t ret = populate_service_endpoint_info(
            allocator, owner != NULL ? owner->name : "", owner != NULL ? owner->type : "", entity->type, endpoint_type,
            entity->context_id, entity->entity_id, &rmw_qos_profile_unknown, &info_array->info_array[index++]);
        if (ret != RMW_RET_OK) {
            return ret;
        }
    }
    return RMW_RET_OK;
}

static rmw_ret_t get_service_endpoint_info(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                           const char* service_name, uint8_t kind, rmw_endpoint_type_t endpoint_type,
                                           rmw_service_endpoint_info_array_t* info_array) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(service_name, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(info_array, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RCUTILS_CHECK_ALLOCATOR_WITH_MSG(allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
    if (RMW_RET_OK != rmw_service_endpoint_info_array_check_zero(info_array)) {
        return RMW_RET_INVALID_ARGUMENT;
    }
    rmw_tickle_context_impl_t* context_impl = ((rmw_tickle_node_t*)node->data)->context_impl;
    tt_Context_lock(&context_impl->tickle_context);
    size_t count = count_matching_locked(context_impl, service_name, kind);
    rmw_ret_t ret = rmw_service_endpoint_info_array_init_with_size(info_array, count, allocator);
    if (ret == RMW_RET_OK) {
        ret = fill_service_endpoint_info_locked(context_impl, service_name, kind, endpoint_type, allocator, info_array);
        if (ret != RMW_RET_OK) {
            fini_service_endpoint_info_array_ignore_result(info_array, allocator);
        }
    }
    tt_Context_unlock(&context_impl->tickle_context);
    return ret;
}

rmw_ret_t rmw_get_clients_info_by_service(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                          const char* service_name, bool no_mangle,
                                          rmw_service_endpoint_info_array_t* clients_info) {
    (void)no_mangle; // see rmw_get_topic_names_and_types()'s own doc comment
    return get_service_endpoint_info(node, allocator, service_name, tt_KIND_SERVICE_CLIENT, RMW_ENDPOINT_CLIENT,
                                     clients_info);
}

rmw_ret_t rmw_get_servers_info_by_service(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                          const char* service_name, bool no_mangle,
                                          rmw_service_endpoint_info_array_t* servers_info) {
    (void)no_mangle;
    return get_service_endpoint_info(node, allocator, service_name, tt_KIND_SERVICE_SERVER, RMW_ENDPOINT_SERVER,
                                     servers_info);
}
#endif
#endif

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

    tt_Context_lock(&context_impl->tickle_context);

    bool found = false;
    for (uint32_t i = 0; i < context_impl->tickle_context.endpoint_count && !found; ++i) {
        const struct tt_Endpoint* endpoint = context_impl->tickle_context.endpoints[i];
        // A local endpoint has no separate "type name" of its own to compare here (tt_Endpoint
        // only carries the service/topic *name*, not its type - see struct tt_Endpoint) - a local
        // server matches on name+kind alone, mirroring how tt_Context_create_server() itself only
        // ever registers one server per (name), never per (name, type) pair on this side.
        if (endpoint->kind == tt_KIND_SERVICE_SERVER && strcmp(endpoint->name, service_name) == 0) {
            found = true;
        }
    }
    for (uint32_t i = 0; i < tt_MAX_DISCOVERED_ENTITIES && !found; ++i) {
        const struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[i];
        if (entity->context_id != tt_CONTEXT_ID_INVALID && entity->kind == tt_KIND_SERVICE_SERVER &&
            strcmp(entity->name, service_name) == 0 && strcmp(entity->type, type_name) == 0) {
            found = true;
        }
    }

    tt_Context_unlock(&context_impl->tickle_context);
    *is_available = found;
    return RMW_RET_OK;
}
