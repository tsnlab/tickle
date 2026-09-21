/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// DDS QoS policy coverage inventory (rmw_tickle/PLAN.md, 2026-09-21) gap 1: rmw_tickle_resolve_
// best_available() (rmw_qos.c) resolves RMW_QOS_POLICY_*_BEST_AVAILABLE (reliability/durability/
// liveliness) and the two _BEST_AVAILABLE duration sentinels (deadline/liveliness_lease_duration)
// against context_impl->discovery at entity-creation time. Same synthetic-discovered-entity
// injection technique test_events.c already established (struct tt_DiscoveredEntity is public,
// rmw_tickle_c/rmw_tickle.h - injecting straight into context_impl->discovery.entities[] under
// node_mutex is indistinguishable from a real remote UPDATE to anything that reads this table) -
// applied here *before* rmw_create_publisher()/rmw_create_subscription() runs, since resolution
// happens once at creation time, not on an ongoing basis. rmw_publisher_get_actual_qos()/rmw_
// subscription_get_actual_qos() (both just return pub_impl->qos/sub_impl->qos, set from the
// already-resolved qos_profile) is how each case's own resolved value is observed.

#include <assert.h>
#include <pthread.h> // node_mutex around the direct discovery-table injection below
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE, tt_KIND_TOPIC_*, tt_UPDATE_QOS_*

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h" // struct tt_DiscoveredEntity injection below
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define FAKE_REMOTE_NODE_ID 7 // any value other than tt_NODE_ID_INVALID - see test_events.c's own identical note

#define NS_PER_MS (1000LL * 1000LL)
#define NS_PER_SEC (1000LL * NS_PER_MS)
#define SHORT_DEADLINE_NS (300 * NS_PER_MS) // deadline_sub_max_topic's own smaller discovered value
#define LONG_DEADLINE_NS (700 * NS_PER_MS)  // ...and its larger one - the resolved max
#define SHORT_LEASE_NS (5 * NS_PER_SEC)     // lease_pub_min_topic's own smaller discovered value
#define LONG_LEASE_NS (8 * NS_PER_SEC)      // ...and its larger one - the resolved min stays SHORT_LEASE_NS

struct fake_ros_msg {
    uint8_t value;
};
struct fake_tickle_msg {
    uint8_t value;
};

static bool fake_to_tickle(const void* ros_message, void* tickle_message) {
    ((struct fake_tickle_msg*)tickle_message)->value = ((const struct fake_ros_msg*)ros_message)->value;
    return true;
}
static bool fake_from_tickle(const void* tickle_message, void* ros_message) {
    ((struct fake_ros_msg*)ros_message)->value = ((const struct fake_tickle_msg*)tickle_message)->value;
    return true;
}
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint8_t);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    payload[0] = ((struct fake_tickle_msg*)data)->value;
    return (int32_t)sizeof(uint8_t);
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)len;
    (void)is_native_endian;
    ((struct fake_tickle_msg*)data)->value = payload[0];
    return (int32_t)sizeof(uint8_t);
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t fake_callbacks = {
    .ros_type_name = "test_best_available/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};

static rosidl_message_type_support_t fake_handle = {
    .data = &fake_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static const rosidl_message_type_support_t* fake_type_support(void) {
    if (NULL == fake_handle.typesupport_identifier) {
        fake_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    }
    return &fake_handle;
}

static rmw_qos_profile_t base_qos(void) {
    rmw_qos_profile_t qos;
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = 10;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT;
    qos.lifespan = (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT;
    qos.avoid_ros_namespace_conventions = false;
    return qos;
}

// Injects one synthetic discovered entity of `kind` on `topic_name` into the next free slot (a
// running counter, not a caller-picked index - nothing here cares which physical slot an entity
// lands in, only that distinct calls don't collide), offering/requesting `qos_bits` (tt_UPDATE_
// QOS_RELIABLE/_DURABLE/_LIVELINESS_MANUAL, OR'd together) with the given deadline/lease nanosecond
// values (0 = unspecified, matching struct tt_DiscoveredEntity's own convention) - same technique/
// locking as test_events.c's own identical injection.
static int next_discovery_slot = 0;

static void inject_discovered_entity(rmw_tickle_context_impl_t* context_impl, uint8_t kind, const char* topic_name,
                                     uint8_t qos_bits, uint64_t deadline_duration_ns,
                                     uint64_t liveliness_lease_duration_ns) {
    pthread_mutex_lock(&context_impl->node_mutex);
    struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[next_discovery_slot++];
    entity->node_id = FAKE_REMOTE_NODE_ID;
    entity->endpoint_id = 0;
    entity->kind = kind;
    entity->qos = qos_bits;
    entity->deadline_duration_ns = deadline_duration_ns;
    entity->liveliness_lease_duration_ns = liveliness_lease_duration_ns;
    entity->alive = true;
    snprintf(entity->type, sizeof(entity->type), "test_best_available/msg/FakeMsg");
    snprintf(entity->name, sizeof(entity->name), "%s", topic_name);
    pthread_mutex_unlock(&context_impl->node_mutex);
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_best_available", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* type_support = fake_type_support();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;

    // --- RELIABILITY ---

    // A Publisher requesting BEST_AVAILABLE always resolves to RELIABLE regardless of discovery
    // (offering RELIABLE is RxO-compatible with any Subscription request, so it's always safe) -
    // zero discovered Subscriptions here proves it's not merely "discovery happened to say so".
    {
        rmw_qos_profile_t qos = base_qos();
        qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE;
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "reliability_pub_topic", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_publisher_get_actual_qos(pub, &actual));
        assert(RMW_QOS_POLICY_RELIABILITY_RELIABLE == actual.reliability);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // A Subscription requesting BEST_AVAILABLE with zero discovered Publishers resolves to RELIABLE
    // (vacuous truth - "matches all discovered endpoints" with none to disagree).
    {
        rmw_qos_profile_t qos = base_qos();
        qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE;
        rmw_subscription_t* sub =
            rmw_create_subscription(node, type_support, "reliability_sub_vacuous_topic", &qos, &sub_opts);
        assert(NULL != sub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
        assert(RMW_QOS_POLICY_RELIABILITY_RELIABLE == actual.reliability);
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    }

    // A Subscription requesting BEST_AVAILABLE with a discovered Publisher offering only BEST_
    // EFFORT must fall back to BEST_EFFORT - requesting RELIABLE would be incompatible with it.
    {
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_PUBLISHER, "reliability_sub_besteffort_topic",
                                 /*qos_bits=*/0, 0, 0);
        rmw_qos_profile_t qos = base_qos();
        qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE;
        rmw_subscription_t* sub =
            rmw_create_subscription(node, type_support, "reliability_sub_besteffort_topic", &qos, &sub_opts);
        assert(NULL != sub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
        assert(RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT == actual.reliability);
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    }

    // A Subscription requesting BEST_AVAILABLE with a discovered Publisher offering RELIABLE
    // resolves to RELIABLE.
    {
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_PUBLISHER, "reliability_sub_reliable_topic",
                                 tt_UPDATE_QOS_RELIABLE, 0, 0);
        rmw_qos_profile_t qos = base_qos();
        qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE;
        rmw_subscription_t* sub =
            rmw_create_subscription(node, type_support, "reliability_sub_reliable_topic", &qos, &sub_opts);
        assert(NULL != sub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
        assert(RMW_QOS_POLICY_RELIABILITY_RELIABLE == actual.reliability);
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    }

    // --- DURABILITY (same algorithm, one representative pair) ---

    {
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_PUBLISHER, "durability_sub_topic", /*qos_bits=*/0, 0, 0);
        rmw_qos_profile_t qos = base_qos();
        qos.durability = RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE;
        rmw_subscription_t* sub = rmw_create_subscription(node, type_support, "durability_sub_topic", &qos, &sub_opts);
        assert(NULL != sub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
        assert(RMW_QOS_POLICY_DURABILITY_VOLATILE == actual.durability); // discovered Publisher offers neither bit
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));

        rmw_qos_profile_t pub_qos = base_qos();
        pub_qos.durability = RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE;
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "durability_pub_topic", &pub_qos, &pub_opts);
        assert(NULL != pub);
        rmw_qos_profile_t pub_actual;
        assert(RMW_RET_OK == rmw_publisher_get_actual_qos(pub, &pub_actual));
        assert(RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL == pub_actual.durability); // Publisher always trivially strict
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // --- LIVELINESS (same algorithm, one representative pair) ---

    {
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_PUBLISHER, "liveliness_sub_topic",
                                 tt_UPDATE_QOS_LIVELINESS_MANUAL, 0, 0);
        rmw_qos_profile_t qos = base_qos();
        qos.liveliness = RMW_QOS_POLICY_LIVELINESS_BEST_AVAILABLE;
        rmw_subscription_t* sub = rmw_create_subscription(node, type_support, "liveliness_sub_topic", &qos, &sub_opts);
        assert(NULL != sub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
        assert(RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == actual.liveliness); // discovered Publisher offers it
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    }

    // --- DEADLINE sentinel (Subscription side: max of discovered Publisher deadlines) ---

    // Zero discovered Publishers -> no constraint (0,0), not an invented value.
    {
        rmw_qos_profile_t qos = base_qos();
        qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE;
        rmw_subscription_t* sub =
            rmw_create_subscription(node, type_support, "deadline_sub_vacuous_topic", &qos, &sub_opts);
        assert(NULL != sub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
        assert(rmw_time_equal(actual.deadline, (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT));
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    }

    // Two discovered Publishers, SHORT_DEADLINE_NS and LONG_DEADLINE_NS -> resolves to the larger
    // (max).
    {
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_PUBLISHER, "deadline_sub_max_topic", 0, SHORT_DEADLINE_NS,
                                 0);
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_PUBLISHER, "deadline_sub_max_topic", 0, LONG_DEADLINE_NS,
                                 0);
        rmw_qos_profile_t qos = base_qos();
        qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE;
        rmw_subscription_t* sub =
            rmw_create_subscription(node, type_support, "deadline_sub_max_topic", &qos, &sub_opts);
        assert(NULL != sub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
        assert(LONG_DEADLINE_NS == rmw_time_total_nsec(actual.deadline));
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    }

    // One discovered Publisher with an unconstrained (0) deadline alongside one with a real
    // SHORT_DEADLINE_NS value -> the unconstrained one must dominate (collapses the whole result to
    // "no constraint"), not get skipped the way a min-computation would skip it.
    {
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_PUBLISHER, "deadline_sub_unconstrained_topic", 0,
                                 SHORT_DEADLINE_NS, 0);
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_PUBLISHER, "deadline_sub_unconstrained_topic", 0,
                                 /*deadline_duration_ns=*/0, 0);
        rmw_qos_profile_t qos = base_qos();
        qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE;
        rmw_subscription_t* sub =
            rmw_create_subscription(node, type_support, "deadline_sub_unconstrained_topic", &qos, &sub_opts);
        assert(NULL != sub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
        assert(rmw_time_equal(actual.deadline, (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT));
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    }

    // --- LIVELINESS_LEASE_DURATION sentinel (Publisher side: min of discovered Subscription leases) ---

    // Two discovered Subscriptions, SHORT_LEASE_NS and LONG_LEASE_NS -> resolves to the smaller
    // (min).
    {
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_SUBSCRIBER, "lease_pub_min_topic", 0, 0, SHORT_LEASE_NS);
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_SUBSCRIBER, "lease_pub_min_topic", 0, 0, LONG_LEASE_NS);
        rmw_qos_profile_t qos = base_qos();
        qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE;
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "lease_pub_min_topic", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_publisher_get_actual_qos(pub, &actual));
        assert(SHORT_LEASE_NS == rmw_time_total_nsec(actual.liveliness_lease_duration));
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // One discovered Subscription unconstrained (0) alongside one requesting SHORT_LEASE_NS -> the
    // real requirement must still win (an unconstrained peer doesn't narrow a min, it's skipped).
    {
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_SUBSCRIBER, "lease_pub_skip_unconstrained_topic", 0, 0,
                                 /*liveliness_lease_duration_ns=*/0);
        inject_discovered_entity(context_impl, tt_KIND_TOPIC_SUBSCRIBER, "lease_pub_skip_unconstrained_topic", 0, 0,
                                 SHORT_LEASE_NS);
        rmw_qos_profile_t qos = base_qos();
        qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE;
        rmw_publisher_t* pub =
            rmw_create_publisher(node, type_support, "lease_pub_skip_unconstrained_topic", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_publisher_get_actual_qos(pub, &actual));
        assert(SHORT_LEASE_NS == rmw_time_total_nsec(actual.liveliness_lease_duration));
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // Zero discovered Subscriptions -> no constraint (0,0).
    {
        rmw_qos_profile_t qos = base_qos();
        qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE;
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "lease_pub_vacuous_topic", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_qos_profile_t actual;
        assert(RMW_RET_OK == rmw_publisher_get_actual_qos(pub, &actual));
        assert(rmw_time_equal(actual.liveliness_lease_duration, (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT));
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_best_available: PASS\n");
    return 0;
}
