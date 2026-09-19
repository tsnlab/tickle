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
#include <stdio.h>
#include <string.h>

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
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT));

    // QoS roadmap #3 (LIVELINESS) - done, every kind this rmw_qos_policy_liveliness_t still
    // defines. A custom liveliness_lease_duration is accepted down to tt_LIVELINESS_MISS_THRESHOLD
    // * tt_NODE_UPDATE_INTERVAL (TickLE core's own fastest possible peer-death detection latency,
    // config.h) - 1 second is below that floor
    // (3 seconds by default) and stays rejected; 5 seconds clears it and is accepted, for every
    // entity kind (validation only - the actual RMW_EVENT_LIVELINESS_CHANGED monitoring this backs
    // is Publisher/Subscription-only, rmw_subscription.c).
    qos = valid_profile();
    qos.liveliness_lease_duration.sec = 1;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));

    qos = valid_profile();
    qos.liveliness_lease_duration.sec = 5;
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

    // KEEP_ALL is only rejected for a subscription (an unbounded reader queue) - a publisher/
    // client/service has no reader-side queue at all in this rmw's model, so it's harmless there.
    qos = valid_profile();
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_SUBSCRIPTION));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, RMW_TICKLE_ENTITY_PUBLISHER));

    printf("rmw_tickle_validate_qos_profile: PASS\n");
    return 0;
}
