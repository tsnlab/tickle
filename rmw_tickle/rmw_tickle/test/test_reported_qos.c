/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The invariant this file exists for: **every QoS profile rmw_tickle reports is one rmw_tickle
// accepts.** Reporting and validating are separate functions written months apart, and nothing made
// them agree, so they did not.
//
// Found by g13's bag acceptance rather than by reading either of them (RMW_GAPS_PLAN.md). rosbag2
// records a publisher's reported QoS into the bag and offers that same profile back when it plays
// the bag. rmw_get_publishers_info_by_topic() reported liveliness as UNKNOWN for a discovered
// publisher - it filled in RELIABILITY and DURABILITY and left the rest at rmw_qos_profile_unknown -
// and rmw_tickle_validate_qos_profile() refuses UNKNOWN. So `ros2 bag play` asked us for a
// publisher with a profile we had written ourselves, we refused it with our own error message,
// and the topic was skipped: 73 messages recorded, 0 replayed, no error anywhere near the reader.
//
// The fix is not "special-case liveliness". LIVELINESS and the two durations are announced and
// participate in endpoint matching, which is exactly what this API guarantees discovery shares, so
// they were available all along. What is asserted here is the general property, because the next
// policy added to the announce will be subject to the same mistake: whatever we say about an
// endpoint, we have to be willing to hear back.
//
// Mutant: report liveliness as RMW_QOS_POLICY_LIVELINESS_UNKNOWN again - the round-trip assertion
// below fails, and so does the explicit liveliness one.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/hal.h>    // tt_get_ns()
#include <tickle/tickle.h> // tt_KIND_TOPIC_PUBLISHER, tt_UPDATE_QOS_*, tt_Discovery_reindex

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/time.h"
#include "rmw/topic_endpoint_info_array.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

#define FAKE_REMOTE_NODE_ID 7
#define NS_PER_SEC (1000ULL * 1000ULL * 1000ULL)
#define ANNOUNCED_LEASE_NS (4ULL * NS_PER_SEC)
// QoS plan D8's endpoints: a KEEP_LAST 7 publisher with a 250 ms lifespan, a KEEP_ALL subscription with a 1.5 s one.
#define KEEP_LAST_DEPTH 7
#define KEEP_LAST_LIFESPAN_NS (NS_PER_SEC / 4U)
#define KEEP_ALL_DEPTH 256
#define KEEP_ALL_LIFESPAN_NS (NS_PER_SEC + (NS_PER_SEC / 2U))
#define CONTROL_DEPTH 3

static int next_discovery_slot = 0;

// Since tt_VERSION 14 an announce carries a lifespan and a history depth too (QoS plan D8): what the table holds then.
struct announced_history {
    uint64_t lifespan_ns; // 0: infinite, as on the wire
    uint32_t depth;
};

static void inject_entity(rmw_tickle_context_impl_t* context_impl, const char* topic_name, uint8_t kind,
                          uint8_t qos_bits, uint64_t deadline_ns, uint64_t lease_ns, struct announced_history history) {
    tt_Context_lock(&context_impl->tickle_context);
    struct tt_DiscoveredEntity* entity = &context_impl->discovery.entities[next_discovery_slot++];
    entity->context_id = FAKE_REMOTE_NODE_ID;
    entity->endpoint_id = 0;
    entity->entity_id = (uint32_t)next_discovery_slot; // distinct, so the index holds each
    entity->kind = kind;
    entity->qos = qos_bits;
    entity->deadline_duration_ns = deadline_ns;
    entity->liveliness_lease_duration_ns = lease_ns;
    entity->lifespan_duration_ns = history.lifespan_ns;
    entity->history_depth = history.depth;
    entity->alive = true;
    // An entity that announced a lease is only alive while its node has been heard from and the
    // lease has not run out (node_entity_alive_locked(), tickle.c), and an endpoint-info query
    // skips entities that are not alive. Nothing here is testing liveliness, so say the node was
    // heard from just now rather than let that rule decide whether the QoS question gets asked.
    entity->last_asserted_ns = tt_get_ns();
    context_impl->tickle_context.update_seen[FAKE_REMOTE_NODE_ID] = true;
    context_impl->tickle_context.traffic_last_seen[FAKE_REMOTE_NODE_ID] = entity->last_asserted_ns;
    tt_Discovery_reindex(&context_impl->discovery);
    snprintf(entity->type, sizeof(entity->type), "test_reported_qos/msg/FakeMsg");
    snprintf(entity->name, sizeof(entity->name), "%s", topic_name);
    tt_Context_unlock(&context_impl->tickle_context);
}

static void inject_publisher(rmw_tickle_context_impl_t* context_impl, const char* topic_name, uint8_t qos_bits,
                             uint64_t deadline_ns, uint64_t lease_ns) {
    inject_entity(context_impl, topic_name, tt_KIND_TOPIC_PUBLISHER, qos_bits, deadline_ns, lease_ns,
                  (struct announced_history) {0, 0});
}

static bool durations_equal(rmw_time_t a, rmw_time_t b) {
    return a.sec == b.sec && a.nsec == b.nsec;
}

// The whole point, in one function: take what we said about an endpoint and hand it back to the
// validator that decides what we accept. Printed rather than only asserted, so a failure says which
// policy disagreed instead of only that something did.
static void reported_profile_must_be_acceptable(const rmw_qos_profile_t* qos, const char* what) {
    printf("  %s: reliability=%d durability=%d liveliness=%d deadline=%llu.%09llu lease=%llu.%09llu\n", what,
           (int)qos->reliability, (int)qos->durability, (int)qos->liveliness, (unsigned long long)qos->deadline.sec,
           (unsigned long long)qos->deadline.nsec, (unsigned long long)qos->liveliness_lease_duration.sec,
           (unsigned long long)qos->liveliness_lease_duration.nsec);
    rmw_qos_profile_t offered = *qos;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&offered, RMW_TICKLE_ENTITY_PUBLISHER));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&offered, RMW_TICKLE_ENTITY_SUBSCRIPTION));
}

static rmw_qos_profile_t reported_qos_of_kind(const char* topic_name, rcutils_allocator_t* allocator, rmw_node_t* node,
                                              bool subscriptions) {
    rmw_topic_endpoint_info_array_t info = rmw_get_zero_initialized_topic_endpoint_info_array();
    if (subscriptions) {
        assert(RMW_RET_OK == rmw_get_subscriptions_info_by_topic(node, allocator, topic_name, false, &info));
    } else {
        assert(RMW_RET_OK == rmw_get_publishers_info_by_topic(node, allocator, topic_name, false, &info));
    }
    assert(1 == info.size);
    rmw_qos_profile_t qos = info.info_array[0].qos_profile;
    assert(RMW_RET_OK == rmw_topic_endpoint_info_array_fini(&info, allocator));
    return qos;
}

static rmw_qos_profile_t reported_qos_of(const char* topic_name, rcutils_allocator_t* allocator, rmw_node_t* node) {
    return reported_qos_of_kind(topic_name, allocator, node, false);
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_reported_qos", "/");
    assert(NULL != node);
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)context.impl;

    // An ordinary publisher: RELIABLE, VOLATILE, AUTOMATIC liveliness, nothing announced for the
    // durations. This is what `ros2 bag record` meets in the field and what the bag then stores.
    inject_publisher(context_impl, "plain_topic", tt_UPDATE_QOS_RELIABLE, 0, 0);
    rmw_qos_profile_t plain = reported_qos_of("plain_topic", &allocator, node);

    // The general invariant goes first, deliberately. Asserting the individual policies before it
    // means a regression fails on whichever value happens to be checked earliest and the round trip
    // is never evaluated at all - the same way g13's own no-overwrite test could not fail on its
    // own claim until it was reordered. The invariant is the thing this file is for; the specific
    // values below say which policy broke it.
    reported_profile_must_be_acceptable(&plain, "plain");

    assert(RMW_QOS_POLICY_RELIABILITY_RELIABLE == plain.reliability);
    assert(RMW_QOS_POLICY_DURABILITY_VOLATILE == plain.durability);
    // The specific value that broke playback. UNKNOWN here is the defect, not a safe default.
    assert(RMW_QOS_POLICY_LIVELINESS_AUTOMATIC == plain.liveliness);
    // 0 on the wire means "no requirement", which rmw and DDS spell as infinite, not as zero - a
    // literal 0 would read as a deadline of no time at all. CycloneDDS reports the same infinity.
    assert(durations_equal((rmw_time_t)RMW_DURATION_INFINITE, plain.deadline));
    assert(durations_equal((rmw_time_t)RMW_DURATION_INFINITE, plain.liveliness_lease_duration));

    // A publisher that announced MANUAL_BY_TOPIC and a real lease: the reported profile must say so
    // rather than flattening every publisher to one answer.
    inject_publisher(context_impl, "manual_topic", tt_UPDATE_QOS_RELIABLE | tt_UPDATE_QOS_LIVELINESS_MANUAL, 0,
                     ANNOUNCED_LEASE_NS);
    rmw_qos_profile_t manual = reported_qos_of("manual_topic", &allocator, node);

    reported_profile_must_be_acceptable(&manual, "manual");
    assert(RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == manual.liveliness);
    assert(4 == manual.liveliness_lease_duration.sec && 0 == manual.liveliness_lease_duration.nsec);

    // And a BEST_EFFORT, TRANSIENT_LOCAL one, so the round trip is not asserted over a single shape.
    inject_publisher(context_impl, "durable_topic", tt_UPDATE_QOS_DURABLE, 0, 0);
    rmw_qos_profile_t durable = reported_qos_of("durable_topic", &allocator, node);

    reported_profile_must_be_acceptable(&durable, "durable");
    assert(RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT == durable.reliability);
    assert(RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL == durable.durability);

    // QoS plan D8 (tt_VERSION 14): history, depth and lifespan, which were UNKNOWN. A KEEP_LAST 7 publisher with a
    // 250 ms lifespan, a KEEP_ALL subscription with a 1.5 s one, and - the control - a KEEP_LAST 3 publisher with no
    // lifespan, which must read back infinite and its own depth, so neither value above is a constant.
    inject_entity(context_impl, "history_topic", tt_KIND_TOPIC_PUBLISHER, tt_UPDATE_QOS_RELIABLE, 0, 0,
                  (struct announced_history) {KEEP_LAST_LIFESPAN_NS, KEEP_LAST_DEPTH});
    inject_entity(context_impl, "history_topic", tt_KIND_TOPIC_SUBSCRIBER, tt_UPDATE_QOS_KEEP_ALL, 0, 0,
                  (struct announced_history) {KEEP_ALL_LIFESPAN_NS, KEEP_ALL_DEPTH});
    inject_entity(context_impl, "history_control", tt_KIND_TOPIC_PUBLISHER, 0, 0, 0,
                  (struct announced_history) {0, CONTROL_DEPTH});
    rmw_qos_profile_t keep_last = reported_qos_of("history_topic", &allocator, node);
    rmw_qos_profile_t keep_all = reported_qos_of_kind("history_topic", &allocator, node, true);
    rmw_qos_profile_t control = reported_qos_of("history_control", &allocator, node);
    printf("  history: publisher history=%d depth=%zu lifespan=%llu.%09llu; subscription history=%d depth=%zu "
           "lifespan=%llu.%09llu; control history=%d depth=%zu lifespan infinite=%d\n",
           (int)keep_last.history, keep_last.depth, (unsigned long long)keep_last.lifespan.sec,
           (unsigned long long)keep_last.lifespan.nsec, (int)keep_all.history, keep_all.depth,
           (unsigned long long)keep_all.lifespan.sec, (unsigned long long)keep_all.lifespan.nsec, (int)control.history,
           control.depth, durations_equal((rmw_time_t)RMW_DURATION_INFINITE, control.lifespan));
    reported_profile_must_be_acceptable(&keep_last, "keep_last");
    reported_profile_must_be_acceptable(&keep_all, "keep_all");
    reported_profile_must_be_acceptable(&control, "control");
    assert(RMW_QOS_POLICY_HISTORY_KEEP_LAST == keep_last.history && KEEP_LAST_DEPTH == keep_last.depth);
    assert(0 == keep_last.lifespan.sec && 250000000ULL == keep_last.lifespan.nsec);
    assert(RMW_QOS_POLICY_HISTORY_KEEP_ALL == keep_all.history);
    assert(1 == keep_all.lifespan.sec && 500000000ULL == keep_all.lifespan.nsec);
    assert(RMW_QOS_POLICY_HISTORY_KEEP_LAST == control.history && CONTROL_DEPTH == control.depth);
    assert(durations_equal((rmw_time_t)RMW_DURATION_INFINITE, control.lifespan));

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_reported_qos: PASS\n");
    return 0;
}
