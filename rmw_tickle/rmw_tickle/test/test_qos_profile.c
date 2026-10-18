/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// QoS redesign step 3 (2026-10-18): rmw_tickle creates every endpoint with core's tt_Node_create_*() and a struct
// tt_QosProfile, and core resolves SYSTEM_DEFAULT and BEST_AVAILABLE. Three things this must not have changed, each
// checked against what the code before the move did:
//
// 1. BEST_AVAILABLE. rmw_tickle_resolve_best_available() was dropped for tt_QosProfile_resolve(). Its last version is
//    kept below, word for word apart from its name, as the reference: for every combination of the five policies that
//    can be BEST_AVAILABLE (3^5 requests), on topics whose discovery table shows nothing, one endpoint, or two that
//    disagree, on both sides, the profile the endpoint keeps (pub_impl->qos / sub_impl->qos, what graph queries and
//    the compatibility checks read) must equal the reference's whole, and get_actual_qos must agree on those policies.
//    Control: the same comparison against a reference with one rule flipped (a Subscription taking the MIN of the
//    Publisher deadlines) must find differences, or the comparison cannot fail.
//
// 2. rmw_*_get_actual_qos() for rmw_qos_profile_default, _sensor_data and _services_default, and a KEEP_ALL profile.
//    Before step 3 they returned the profile as requested (BEST_AVAILABLE resolved). Now they read core. Every field
//    must equal the old answer, except a SYSTEM_DEFAULT kind, which is now reported resolved (rmw asks for that:
//    "the actual configuration applied when using RMW_*_SYSTEM_DEFAULT can only be resolved after the creation").
//    Printed as a table, old beside new.
//
// 3. The storage core lays out is the one rmw_tickle laid out: a depth-1000 publisher of an unbounded type (/rosout's
//    shape) indexes at most what its arena can fill and starts on the first slice of its arena, not all of it.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/config.h> // tt_CONTEXT_ID_INVALID
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

#define FAKE_REMOTE_NODE_ID 7
#define NS_PER_MS (1000LL * 1000LL)
#define NS_PER_SEC (1000LL * NS_PER_MS)
// The deadlines and leases the discovered endpoints announce, and the ones requested.
#define DEADLINE_A_NS (300 * NS_PER_MS)
#define DEADLINE_B_NS (700 * NS_PER_MS)
#define DEADLINE_C_NS (900 * NS_PER_MS)
#define DEADLINE_ASKED_NS (500 * NS_PER_MS)
#define LEASE_A_NS (5 * NS_PER_SEC)
#define LEASE_B_NS (8 * NS_PER_SEC)
#define LEASE_C_NS (6 * NS_PER_SEC)
#define LEASE_ASKED_NS (4 * NS_PER_SEC)
#define ROSOUT_DEPTH 1000 // rcl's /rosout publisher
#define TIME_TEXT 48
#define DEPTH_TEXT 24
#define WHAT_TEXT 96

struct fake_msg {
    uint8_t value;
};

static bool fake_convert(const void* source, void* dest) {
    ((struct fake_msg*)dest)->value = ((const struct fake_msg*)source)->value;
    return true;
}
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return 1;
}
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    payload[0] = ((struct fake_msg*)data)->value;
    return 1;
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)len;
    (void)is_native_endian;
    ((struct fake_msg*)data)->value = payload[0];
    return 1;
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}

// tickle_max_encoded_size 0: ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED, a type like std_msgs/String.
static rosidl_typesupport_tickle_c_message_callbacks_t fake_callbacks = {
    .ros_type_name = "test_qos_profile/msg/Fake",
    .tickle_struct_size = sizeof(struct fake_msg),
    .ros_struct_size = sizeof(struct fake_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_convert,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_convert,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};
static rosidl_message_type_support_t fake_handle = {
    .data = &fake_callbacks,
    .func = get_message_typesupport_handle_function,
};

static rosidl_typesupport_tickle_c_message_callbacks_t request_callbacks;
static rosidl_typesupport_tickle_c_message_callbacks_t response_callbacks;
static rosidl_message_type_support_t request_handle = {.data = &request_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t response_handle = {.data = &response_callbacks,
                                                        .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_service_callbacks_t service_callbacks = {.ros_type_name =
                                                                                "test_qos_profile/srv/Fake"};
static rosidl_service_type_support_t service_handle = {.data = &service_callbacks,
                                                       .func = get_service_typesupport_handle_function,
                                                       .request_typesupport = &request_handle,
                                                       .response_typesupport = &response_handle};

static rmw_node_t* node;
static rmw_tickle_context_impl_t* context_impl;

// ---- 1. BEST_AVAILABLE: the reference, rmw_tickle_resolve_best_available() and its two helpers as rmw_qos.c had
// them until step 3 (renamed). `flip_subscription_deadline` is the control's planted defect.

static bool flip_subscription_deadline;

static bool all_matching_entities_offer(const struct tt_Discovery* discovery, const char* topic_name,
                                        uint8_t opposite_kind, uint8_t qos_bit) {
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        const struct tt_DiscoveredEntity* entity = &discovery->entities[i];
        if (tt_CONTEXT_ID_INVALID == entity->context_id || !entity->alive || entity->kind != opposite_kind ||
            0 != strcmp(entity->name, topic_name)) {
            continue;
        }
        if (0 == (entity->qos & qos_bit)) {
            return false;
        }
    }
    return true;
}

static uint64_t entity_deadline_ns(const struct tt_DiscoveredEntity* entity) {
    return entity->deadline_duration_ns;
}
static uint64_t entity_lease_ns(const struct tt_DiscoveredEntity* entity) {
    return entity->liveliness_lease_duration_ns;
}

static uint64_t resolve_best_available_duration(const struct tt_Discovery* discovery, const char* topic_name,
                                                uint8_t opposite_kind, bool want_max,
                                                uint64_t (*get_ns)(const struct tt_DiscoveredEntity*)) {
    uint64_t result = want_max ? 0 : UINT64_MAX;
    bool any_matched = false;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        const struct tt_DiscoveredEntity* entity = &discovery->entities[i];
        if (tt_CONTEXT_ID_INVALID == entity->context_id || !entity->alive || entity->kind != opposite_kind ||
            0 != strcmp(entity->name, topic_name)) {
            continue;
        }
        any_matched = true;
        uint64_t raw = get_ns(entity);
        uint64_t effective = 0 == raw ? UINT64_MAX : raw;
        if (want_max ? (effective > result) : (effective < result)) {
            result = effective;
        }
    }
    return (!any_matched || UINT64_MAX == result) ? 0 : result;
}

static rmw_qos_profile_t reference_resolve_best_available(const rmw_qos_profile_t* requested,
                                                          const struct tt_Discovery* discovery, const char* topic_name,
                                                          rmw_tickle_entity_kind_t entity_kind) {
    rmw_qos_profile_t resolved = *requested;
    bool is_publisher = RMW_TICKLE_ENTITY_PUBLISHER == entity_kind;
    uint8_t opposite_kind = is_publisher ? tt_KIND_TOPIC_SUBSCRIBER : tt_KIND_TOPIC_PUBLISHER;
    if (RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE == requested->reliability) {
        resolved.reliability =
            (is_publisher || all_matching_entities_offer(discovery, topic_name, opposite_kind, tt_UPDATE_QOS_RELIABLE))
                ? RMW_QOS_POLICY_RELIABILITY_RELIABLE
                : RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    }
    if (RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE == requested->durability) {
        resolved.durability =
            (is_publisher || all_matching_entities_offer(discovery, topic_name, opposite_kind, tt_UPDATE_QOS_DURABLE))
                ? RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL
                : RMW_QOS_POLICY_DURABILITY_VOLATILE;
    }
    if (RMW_QOS_POLICY_LIVELINESS_BEST_AVAILABLE == requested->liveliness) {
        resolved.liveliness = (is_publisher || all_matching_entities_offer(discovery, topic_name, opposite_kind,
                                                                           tt_UPDATE_QOS_LIVELINESS_MANUAL))
                                  ? RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC
                                  : RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    }
    if (rmw_time_equal(requested->deadline, (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE)) {
        bool want_max = !is_publisher;
        if (flip_subscription_deadline && !is_publisher) {
            want_max = false;
        }
        uint64_t deadline_ns =
            resolve_best_available_duration(discovery, topic_name, opposite_kind, want_max, entity_deadline_ns);
        resolved.deadline = rmw_time_from_nsec((rmw_duration_t)deadline_ns);
    }
    if (rmw_time_equal(requested->liveliness_lease_duration,
                       (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE)) {
        uint64_t lease_ns =
            resolve_best_available_duration(discovery, topic_name, opposite_kind, !is_publisher, entity_lease_ns);
        resolved.liveliness_lease_duration = rmw_time_from_nsec((rmw_duration_t)lease_ns);
    }
    return resolved;
}

static int next_discovery_slot = 0;

static void inject(uint8_t kind, const char* topic_name, uint8_t qos_bits, uint64_t deadline_ns, uint64_t lease_ns) {
    tt_Context_lock(&context_impl->tickle_context);
    struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[next_discovery_slot++];
    entity->context_id = FAKE_REMOTE_NODE_ID;
    entity->endpoint_id = (uint32_t)next_discovery_slot;
    entity->kind = kind;
    entity->qos = qos_bits;
    entity->deadline_duration_ns = deadline_ns;
    entity->liveliness_lease_duration_ns = lease_ns;
    entity->alive = true;
    snprintf(entity->type, sizeof(entity->type), "test_qos_profile/msg/Fake");
    snprintf(entity->name, sizeof(entity->name), "%s", topic_name);
    tt_Discovery_reindex(&context_impl->discovery);
    tt_Context_unlock(&context_impl->tickle_context);
}

static bool same_time(rmw_time_t a, rmw_time_t b) {
    return rmw_time_equal(a, b);
}

static bool same_profile(const rmw_qos_profile_t* a, const rmw_qos_profile_t* b) {
    return a->history == b->history && a->depth == b->depth && a->reliability == b->reliability &&
           a->durability == b->durability && same_time(a->deadline, b->deadline) &&
           same_time(a->lifespan, b->lifespan) && a->liveliness == b->liveliness &&
           same_time(a->liveliness_lease_duration, b->liveliness_lease_duration) &&
           a->avoid_ros_namespace_conventions == b->avoid_ros_namespace_conventions;
}

// The BEST_AVAILABLE policies of the actual QoS agree with the reference's resolution of them.
static bool actual_agrees(const rmw_qos_profile_t* actual, const rmw_qos_profile_t* reference) {
    return actual->reliability == reference->reliability && actual->durability == reference->durability &&
           actual->liveliness == reference->liveliness && same_time(actual->deadline, reference->deadline) &&
           same_time(actual->liveliness_lease_duration, reference->liveliness_lease_duration);
}

#define CHOICES 3U // per policy: a plain value, another, BEST_AVAILABLE

// The `place`th base-3 digit of `index`: which choice request `index` makes for that policy.
static unsigned choice_of(unsigned index, unsigned place) {
    while (place-- > 0) {
        index /= CHOICES;
    }
    return index % CHOICES;
}

static rmw_qos_profile_t request_number(unsigned index) {
    static const rmw_qos_reliability_policy_t reliability[3] = {RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT,
                                                                RMW_QOS_POLICY_RELIABILITY_RELIABLE,
                                                                RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE};
    static const rmw_qos_durability_policy_t durability[3] = {RMW_QOS_POLICY_DURABILITY_VOLATILE,
                                                              RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL,
                                                              RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE};
    static const rmw_qos_liveliness_policy_t liveliness[3] = {RMW_QOS_POLICY_LIVELINESS_AUTOMATIC,
                                                              RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC,
                                                              RMW_QOS_POLICY_LIVELINESS_BEST_AVAILABLE};
    const rmw_time_t deadline[3] = {(rmw_time_t)RMW_QOS_DEADLINE_DEFAULT, rmw_time_from_nsec(DEADLINE_ASKED_NS),
                                    (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE};
    const rmw_time_t lease[3] = {(rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT,
                                 rmw_time_from_nsec(LEASE_ASKED_NS),
                                 (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE};
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.reliability = reliability[choice_of(index, 0)];
    qos.durability = durability[choice_of(index, 1)];
    qos.liveliness = liveliness[choice_of(index, 2)];
    qos.deadline = deadline[choice_of(index, 3)];
    qos.liveliness_lease_duration = lease[choice_of(index, 4)];
    return qos;
}

#define REQUESTS 243U                       // CHOICES^5
#define DEADLINE_FLIPS (REQUESTS / CHOICES) // the requests whose deadline is BEST_AVAILABLE

// Every request on `topic`, from one side; returns how many kept profiles differ from the reference.
static unsigned compare_side(const char* topic, rmw_tickle_entity_kind_t kind, bool check_actual) {
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    unsigned differences = 0;
    for (unsigned index = 0; index < REQUESTS; index++) {
        rmw_qos_profile_t request = request_number(index);
        tt_Context_lock(&context_impl->tickle_context);
        rmw_qos_profile_t reference = reference_resolve_best_available(&request, &context_impl->discovery, topic, kind);
        tt_Context_unlock(&context_impl->tickle_context);
        rmw_qos_profile_t kept;
        rmw_qos_profile_t actual;
        if (RMW_TICKLE_ENTITY_PUBLISHER == kind) {
            rmw_publisher_t* pub = rmw_create_publisher(node, &fake_handle, topic, &request, &pub_opts);
            assert(NULL != pub);
            kept = ((rmw_tickle_publisher_t*)pub->data)->qos;
            assert(RMW_RET_OK == rmw_publisher_get_actual_qos(pub, &actual));
            assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
        } else {
            rmw_subscription_t* sub = rmw_create_subscription(node, &fake_handle, topic, &request, &sub_opts);
            assert(NULL != sub);
            kept = ((rmw_tickle_subscriber_t*)sub->data)->qos;
            assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
            assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
        }
        if (!same_profile(&kept, &reference)) {
            differences++;
        }
        // A MANUAL_BY_TOPIC publisher without a lease reports the lease rmw_tickle enforces for it, not {0, 0}.
        bool manual_default_lease =
            RMW_TICKLE_ENTITY_PUBLISHER == kind && RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == reference.liveliness &&
            same_time(reference.liveliness_lease_duration, (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT);
        if (check_actual && !manual_default_lease && !actual_agrees(&actual, &reference)) {
            fprintf(stderr, "get_actual_qos disagrees on %s, request %u\n", topic, index);
            assert(false);
        }
    }
    return differences;
}

static void check_best_available(void) {
    // Publishers seen by a Subscription: none / one offering everything / two that disagree on each policy.
    inject(tt_KIND_TOPIC_PUBLISHER, "/ba_one_pub",
           tt_UPDATE_QOS_RELIABLE | tt_UPDATE_QOS_DURABLE | tt_UPDATE_QOS_LIVELINESS_MANUAL, DEADLINE_A_NS, LEASE_A_NS);
    inject(tt_KIND_TOPIC_PUBLISHER, "/ba_two_pubs", tt_UPDATE_QOS_RELIABLE | tt_UPDATE_QOS_LIVELINESS_MANUAL,
           DEADLINE_A_NS, LEASE_A_NS);
    inject(tt_KIND_TOPIC_PUBLISHER, "/ba_two_pubs", tt_UPDATE_QOS_DURABLE, DEADLINE_B_NS, LEASE_B_NS);
    inject(tt_KIND_TOPIC_PUBLISHER, "/ba_unbounded_pub", tt_UPDATE_QOS_RELIABLE, DEADLINE_A_NS, 0);
    inject(tt_KIND_TOPIC_PUBLISHER, "/ba_unbounded_pub", tt_UPDATE_QOS_RELIABLE, 0, LEASE_C_NS);
    // Subscriptions seen by a Publisher.
    inject(tt_KIND_TOPIC_SUBSCRIBER, "/ba_two_subs", tt_UPDATE_QOS_RELIABLE, DEADLINE_A_NS, LEASE_A_NS);
    inject(tt_KIND_TOPIC_SUBSCRIBER, "/ba_two_subs", 0, 0, LEASE_B_NS);
    inject(tt_KIND_TOPIC_SUBSCRIBER, "/ba_one_sub", tt_UPDATE_QOS_DURABLE, DEADLINE_C_NS, 0);

    static const char* const sub_topics[] = {"/ba_none", "/ba_one_pub", "/ba_two_pubs", "/ba_unbounded_pub"};
    static const char* const pub_topics[] = {"/ba_none", "/ba_two_subs", "/ba_one_sub"};
    unsigned compared = 0;
    for (size_t i = 0; i < sizeof(sub_topics) / sizeof(sub_topics[0]); i++) {
        assert(0U == compare_side(sub_topics[i], RMW_TICKLE_ENTITY_SUBSCRIPTION, true));
        compared += REQUESTS;
    }
    for (size_t i = 0; i < sizeof(pub_topics) / sizeof(pub_topics[0]); i++) {
        assert(0U == compare_side(pub_topics[i], RMW_TICKLE_ENTITY_PUBLISHER, true));
        compared += REQUESTS;
    }
    printf("BEST_AVAILABLE: %u endpoints, kept profile identical to rmw_tickle_resolve_best_available()'s\n", compared);

    // Control: the planted defect is seen. Requests whose deadline is BEST_AVAILABLE on /ba_two_pubs (300 and
    // 700 ms): the max is 700, the flipped min 300 - 81 of the 243.
    flip_subscription_deadline = true;
    unsigned seen = compare_side("/ba_two_pubs", RMW_TICKLE_ENTITY_SUBSCRIPTION, false);
    flip_subscription_deadline = false;
    printf("BEST_AVAILABLE control (reference takes the min deadline): %u of %u differ (expected %u)\n", seen, REQUESTS,
           DEADLINE_FLIPS);
    assert(DEADLINE_FLIPS == seen);
}

// ---- 2. get_actual_qos, old against new.

static const char* reliability_name(rmw_qos_reliability_policy_t value) {
    static const char* const names[] = {"SYSTEM_DEFAULT", "RELIABLE", "BEST_EFFORT", "UNKNOWN", "BEST_AVAILABLE"};
    return (unsigned)value < 5U ? names[value] : "?";
}
static const char* durability_name(rmw_qos_durability_policy_t value) {
    static const char* const names[] = {"SYSTEM_DEFAULT", "TRANSIENT_LOCAL", "VOLATILE", "UNKNOWN", "BEST_AVAILABLE"};
    return (unsigned)value < 5U ? names[value] : "?";
}
static const char* liveliness_name(rmw_qos_liveliness_policy_t value) {
    switch (value) {
    case RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT:
        return "SYSTEM_DEFAULT";
    case RMW_QOS_POLICY_LIVELINESS_AUTOMATIC:
        return "AUTOMATIC";
    case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC:
        return "MANUAL_BY_TOPIC";
    default:
        return "?";
    }
}
static const char* history_name(rmw_qos_history_policy_t value) {
    static const char* const names[] = {"SYSTEM_DEFAULT", "KEEP_LAST", "KEEP_ALL", "UNKNOWN"};
    return (unsigned)value < 4U ? names[value] : "?";
}

static void print_row(const char* name, const char* old_value, const char* new_value) {
    printf("  %-12s %-16s %-16s %s\n", name, old_value, new_value, 0 == strcmp(old_value, new_value) ? "" : "<-");
}

static void print_time_row(const char* name, rmw_time_t old_value, rmw_time_t new_value) {
    char old_text[TIME_TEXT];
    char new_text[TIME_TEXT];
    snprintf(old_text, sizeof(old_text), "{%llu, %llu}", (unsigned long long)old_value.sec,
             (unsigned long long)old_value.nsec);
    snprintf(new_text, sizeof(new_text), "{%llu, %llu}", (unsigned long long)new_value.sec,
             (unsigned long long)new_value.nsec);
    print_row(name, old_text, new_text);
}

// `old` is what get_actual_qos returned before step 3: the request itself (nothing here is BEST_AVAILABLE).
static void compare_actual(const char* what, const rmw_qos_profile_t* old, const rmw_qos_profile_t* now) {
    printf("%s\n  %-12s %-16s %-16s\n", what, "policy", "old", "new");
    char old_text[DEPTH_TEXT];
    char new_text[DEPTH_TEXT];
    print_row("history", history_name(old->history), history_name(now->history));
    snprintf(old_text, sizeof(old_text), "%zu", old->depth);
    snprintf(new_text, sizeof(new_text), "%zu", now->depth);
    print_row("depth", old_text, new_text);
    print_row("reliability", reliability_name(old->reliability), reliability_name(now->reliability));
    print_row("durability", durability_name(old->durability), durability_name(now->durability));
    print_time_row("deadline", old->deadline, now->deadline);
    print_time_row("lifespan", old->lifespan, now->lifespan);
    print_row("liveliness", liveliness_name(old->liveliness), liveliness_name(now->liveliness));
    print_time_row("lease", old->liveliness_lease_duration, now->liveliness_lease_duration);

    // Equal, field by field, except a SYSTEM_DEFAULT kind, which must now be reported as rmw's default resolves it.
    assert(now->history ==
           (RMW_QOS_POLICY_HISTORY_SYSTEM_DEFAULT == old->history ? RMW_QOS_POLICY_HISTORY_KEEP_LAST : old->history));
    assert(now->depth == old->depth);
    assert(now->reliability == (RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT == old->reliability
                                    ? RMW_QOS_POLICY_RELIABILITY_RELIABLE
                                    : old->reliability));
    assert(now->durability == (RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT == old->durability
                                   ? RMW_QOS_POLICY_DURABILITY_VOLATILE
                                   : old->durability));
    assert(same_time(now->deadline, old->deadline));
    assert(same_time(now->lifespan, old->lifespan));
    assert(now->liveliness == (RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT == old->liveliness
                                   ? RMW_QOS_POLICY_LIVELINESS_AUTOMATIC
                                   : old->liveliness));
    assert(same_time(now->liveliness_lease_duration, old->liveliness_lease_duration));
    assert(now->avoid_ros_namespace_conventions == old->avoid_ros_namespace_conventions);
}

static void check_actual_qos(void) {
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_qos_profile_t keep_all = rmw_qos_profile_default;
    keep_all.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
    keep_all.depth = 0; // rclcpp's KeepAll()
    const struct {
        const char* name;
        rmw_qos_profile_t qos;
    } topic_profiles[] = {
        {"rmw_qos_profile_default", rmw_qos_profile_default},
        {"rmw_qos_profile_sensor_data", rmw_qos_profile_sensor_data},
        {"KEEP_ALL (RELIABLE, depth 0)", keep_all},
    };
    for (size_t i = 0; i < sizeof(topic_profiles) / sizeof(topic_profiles[0]); i++) {
        char what[WHAT_TEXT];
        rmw_qos_profile_t actual;
        rmw_publisher_t* pub = rmw_create_publisher(node, &fake_handle, "/actual", &topic_profiles[i].qos, &pub_opts);
        assert(NULL != pub);
        assert(RMW_RET_OK == rmw_publisher_get_actual_qos(pub, &actual));
        snprintf(what, sizeof(what), "publisher, %s", topic_profiles[i].name);
        compare_actual(what, &topic_profiles[i].qos, &actual);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));

        rmw_subscription_t* sub =
            rmw_create_subscription(node, &fake_handle, "/actual", &topic_profiles[i].qos, &sub_opts);
        assert(NULL != sub);
        assert(RMW_RET_OK == rmw_subscription_get_actual_qos(sub, &actual));
        snprintf(what, sizeof(what), "subscription, %s", topic_profiles[i].name);
        compare_actual(what, &topic_profiles[i].qos, &actual);
        assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    }

    rmw_qos_profile_t actual;
    rmw_service_t* service =
        rmw_create_service(node, &service_handle, "/actual_srv", &rmw_qos_profile_services_default);
    assert(NULL != service);
    assert(RMW_RET_OK == rmw_service_request_subscription_get_actual_qos(service, &actual));
    compare_actual("service, rmw_qos_profile_services_default", &rmw_qos_profile_services_default, &actual);
    assert(RMW_RET_OK == rmw_service_response_publisher_get_actual_qos(service, &actual));
    compare_actual("service response, rmw_qos_profile_services_default", &rmw_qos_profile_services_default, &actual);
    rmw_client_t* client = rmw_create_client(node, &service_handle, "/actual_srv", &rmw_qos_profile_services_default);
    assert(NULL != client);
    assert(RMW_RET_OK == rmw_client_request_publisher_get_actual_qos(client, &actual));
    compare_actual("client, rmw_qos_profile_services_default", &rmw_qos_profile_services_default, &actual);
    assert(RMW_RET_OK == rmw_destroy_client(node, client));
    assert(RMW_RET_OK == rmw_destroy_service(node, service));
}

// ---- 3. The storage core lays out for /rosout's shape.

static void check_rosout_shape(void) {
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_qos_profile_t rosout = rmw_qos_profile_default;
    rosout.depth = ROSOUT_DEPTH;
    rosout.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
    rmw_publisher_t* pub = rmw_create_publisher(node, &fake_handle, "/rosout_shape", &rosout, &pub_opts);
    assert(NULL != pub);
    const rmw_tickle_publisher_t* pub_impl = (const rmw_tickle_publisher_t*)pub->data;
    const struct tt_ReliableCache* cache = pub_impl->reliable_cache;
    assert(NULL != cache);
    printf("/rosout shape (unbounded type, depth 1000): index %u slots, arena %u of %u bytes reserved up front\n",
           (unsigned)cache->capacity, (unsigned)cache->arena_size, (unsigned)cache->arena_limit);
    // (depth + 1) x 45 datagrams would be 45,045; what a 1 MiB arena can fill is 2 x 1001 + 1 MiB / 1,4xx.
    assert(cache->capacity < 3000U);
    assert(cache->arena_size < cache->arena_limit); // lazily: the first slice
    assert(cache->arena == pub_impl->arena_base);
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    fake_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    service_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    request_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    response_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    request_callbacks = fake_callbacks;
    request_callbacks.ros_type_name = "test_qos_profile/srv/Fake_Request";
    response_callbacks = fake_callbacks;
    response_callbacks.ros_type_name = "test_qos_profile/srv/Fake_Response";

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    node = rmw_create_node(&context, "test_qos_profile", "/");
    assert(NULL != node);
    context_impl = ((rmw_tickle_node_t*)node->data)->context_impl;

    check_best_available();
    check_actual_qos();
    check_rosout_shape();

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_qos_profile: all tests passed\n");
    return 0;
}
