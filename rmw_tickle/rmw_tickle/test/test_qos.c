/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 10: a scoped test for rmw_tickle_validate_qos_profile() (rmw_qos.c,
// Milestone 7) - every "QoS roadmap" rejection this rmw actually enforces, exercised directly
// (no node/network needed - the function is a pure predicate over an rmw_qos_profile_t).

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/config.h> // tt_CONTEXT_TX_INTERVAL, tt_CONTEXT_UPDATE_INTERVAL, tt_LIVELINESS_MISS_THRESHOLD
#include <tickle/tickle.h> // struct tt_Context, tt_InitOptions_default(), struct tt_QosProfile

#include "rmw/ret_types.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

// Everything this rmw currently accepts - see rmw_qos.c's own comments for the roadmap item each
// field maps to.
static rmw_qos_profile_t valid_profile(void) {
    rmw_qos_profile_t qos;
    memset(&qos, 0, sizeof(qos));
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = 10;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    // deadline/lifespan/liveliness_lease_duration stay {0, 0} (RMW_DURATION_UNSPECIFIED) via memset.
    return qos;
}

int main(void) {
    rmw_qos_profile_t qos = valid_profile();
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT));

    // RELIABLE is accepted for every entity kind now (QoS roadmap #5, done): services/clients via
    // tt_Client_call()'s own pre-existing bounded retry, topics via TickLE core's own ACKNACK +
    // retransmission (struct tt_ReliableCache/tt_Subscriber.reliable - tickle.h; see
    // rmw_create_publisher()/rmw_create_subscription() for how this gets wired up).
    qos = valid_profile();
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT));

    // TRANSIENT_LOCAL is accepted for publishers/subscriptions now (QoS roadmap #4, done): backed
    // by TickLE core's own retained-sample cache + discovery-triggered backlog delivery (struct
    // tt_DurableCache - tickle.h; see rmw_create_publisher() for how this gets wired up). Still
    // rejected for services/clients - tt_Client_call() is request/response, not pub/sub, so
    // there's no TickLE-core mechanism a "retained backlog for a late-joining client" could mean.
    qos = valid_profile();
    qos.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT));

    // MANUAL_BY_TOPIC - done (Milestone 32), backed by a real rmw_publisher_assert_liveliness().
    // MANUAL_BY_PARTICIPANT/_BY_NODE aren't tested here since this rmw_qos_policy_liveliness_t no
    // longer even defines them (removed from the real rmw spec) - nothing to construct a profile
    // with.
    qos = valid_profile();
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    // ... but not for a service or client: nothing asserts a service endpoint's liveliness (D6).
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT));

    // QoS roadmap #3 (LIVELINESS) - done, every kind this rmw_qos_policy_liveliness_t still
    // defines. A custom liveliness_lease_duration is accepted down to tt_LIVELINESS_LEASE_DIVISOR *
    // tt_CONTEXT_TX_INTERVAL (a node's summary goes out that many times a lease, LIVELINESS_PLAN.md) - 1 ms is below
    // that floor and stays rejected; 1 second, rejected until the 3 s floor went on 2026-09-26, is
    // accepted, for every entity kind (validation only - the actual RMW_EVENT_LIVELINESS_CHANGED
    // monitoring this backs is Publisher/Subscription-only, rmw_subscription.c).
    qos = valid_profile();
    const unsigned int one_millisecond_ns = 1000U * 1000U;
    qos.liveliness_lease_duration.nsec = one_millisecond_ns;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));

    qos = valid_profile();
    qos.liveliness_lease_duration.sec = 1;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT));

    // QoS roadmap #2 (DEADLINE) - done, any finite value accepted (a purely local rmw_tickle-side
    // timer, no TickLE wire/network cadence to be bounded by) - see rmw_publisher.c/rmw_
    // subscription.c's own RMW_EVENT_OFFERED_DEADLINE_MISSED/REQUESTED_DEADLINE_MISSED handling.
    qos = valid_profile();
    qos.deadline.sec = 1;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT));

    // QoS roadmap #6 (LIFESPAN) - done, any finite value accepted, same reasoning as DEADLINE just
    // above - see tt_Publisher.lifespan_duration_ns's own doc comment (tickle.h) for the actual
    // reliable_cache-expiry/rmw_tickle_subscriber_t.lifespan_ns's own (queue-expiry) mechanisms.
    qos = valid_profile();
    qos.lifespan.sec = 1;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT));

    // KEEP_ALL is accepted on every kind of entity (g13, RMW_GAPS_PLAN.md). A subscription's was
    // refused here until 2026-09-29 as "an unbounded reader queue", which misread the policy: DDS
    // bounds KEEP_ALL by RESOURCE_LIMITS like anything else, and what makes it KEEP_ALL is that a
    // full cache declines rather than overwriting an unread sample. rmw_create_subscription() sizes
    // it from RMW_TICKLE_READER_KEEP_ALL_BYTES - see test_keep_all_reader.c. A publisher's own
    // KEEP_ALL remains its retained-sample cache (test_history_keep_all.c), and a service or client
    // has no reader-side queue for HISTORY to mean anything about.
    qos = valid_profile();
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT));

    // tt_InitOptions bundle 2: the floor and the node silence are the core context's (its tt_InitOptions.discovery),
    // and NULL is the default's. A context with a longer tx interval refuses the 6 ms the default accepts, and one with
    // a smaller divisor accepts the 1 ms the default refuses.
    static struct tt_Context context;
    memset(&context, 0, sizeof(context));
    tt_InitOptions_default(&context.opts);
    const uint64_t default_floor_ns = rmw_tickle_lease_floor_ns(NULL);
    assert(rmw_tickle_lease_floor_ns(&context) == default_floor_ns);
    assert(rmw_tickle_node_silence_ns(&context) == (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_CONTEXT_UPDATE_INTERVAL);
    qos = valid_profile();
    qos.liveliness_lease_duration.nsec = default_floor_ns;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile_for(&qos, RMW_TICKLE_ENTITY_PUBLISHER, &context));
    context.opts.discovery.tx_interval_ns = 2U * tt_CONTEXT_TX_INTERVAL;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile_for(&qos, RMW_TICKLE_ENTITY_PUBLISHER, &context));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));
    tt_InitOptions_default(&context.opts);
    context.opts.discovery.lease_divisor = 1;
    qos.liveliness_lease_duration.nsec = tt_CONTEXT_TX_INTERVAL;
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile_for(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION, &context));
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    context.opts.discovery.update_interval_ns = 2U * tt_CONTEXT_UPDATE_INTERVAL;
    context.opts.discovery.miss_threshold = 4;
    const uint64_t silence_ns = tt_CONTEXT_UPDATE_INTERVAL * 2U * 4U; // miss_threshold 4 x 2 s
    assert(rmw_tickle_node_silence_ns(&context) == silence_ns);
    // A MANUAL_BY_TOPIC Publisher that names no lease gets the context's silence.
    qos = valid_profile();
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC;
    rmw_tickle_qos_extensions_t ext;
    memset(&ext, 0, sizeof(ext));
    struct tt_QosProfile profile;
    rmw_tickle_qos_to_tt(&qos, RMW_TICKLE_ENTITY_PUBLISHER, &ext, &profile);
    assert(profile.liveliness.lease_ns == rmw_tickle_node_silence_ns(NULL));
    ext.manual_default_lease_ns = rmw_tickle_node_silence_ns(&context);
    rmw_tickle_qos_to_tt(&qos, RMW_TICKLE_ENTITY_PUBLISHER, &ext, &profile);
    assert(profile.liveliness.lease_ns == silence_ns);

    printf("rmw_tickle_validate_qos_profile: PASS\n");
    return 0;
}
