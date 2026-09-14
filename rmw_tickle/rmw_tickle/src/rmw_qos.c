/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 7: QoS rejection logic. Every create function (rmw_create_
// publisher()/_subscription()/_client()/_service()) calls rmw_tickle_validate_qos_profile() before
// doing any real work - anything the "QoS roadmap" table (PLAN.md) hasn't implemented yet is
// rejected outright (RMW_RET_UNSUPPORTED) rather than silently ignored, matching this package's own
// "Design philosophy" table ("Rejects anything outside the currently-supported set explicitly...
// rather than silently downgrading it").

#include <stdbool.h>

#include "rmw/error_handling.h"
#include "rmw/ret_types.h"
#include "rmw/time.h" // rmw_time_equal, RMW_QOS_*_DEFAULT
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t rmw_tickle_validate_qos_profile(const rmw_qos_profile_t* qos_profile, bool is_subscription) {
    // QoS roadmap #5 (RELIABILITY) - ACK/NACK + retransmission isn't built; only BEST_EFFORT
    // (TickLE's own only mode today) is honest to accept.
    if (RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT != qos_profile->reliability &&
        RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT != qos_profile->reliability) {
        RMW_SET_ERROR_MSG("rmw_tickle only supports RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT for now "
                          "- see rmw_tickle/PLAN.md's QoS roadmap #5 (RELIABILITY)");
        return RMW_RET_UNSUPPORTED;
    }

    // QoS roadmap #4 (DURABILITY) - no retained-sample cache/backlog delivery exists.
    if (RMW_QOS_POLICY_DURABILITY_VOLATILE != qos_profile->durability &&
        RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT != qos_profile->durability) {
        RMW_SET_ERROR_MSG("rmw_tickle only supports RMW_QOS_POLICY_DURABILITY_VOLATILE for now - "
                          "see rmw_tickle/PLAN.md's QoS roadmap #4 (DURABILITY)");
        return RMW_RET_UNSUPPORTED;
    }

    // QoS roadmap #3 (LIVELINESS) - only Phase 0's node-level timeout exists, matching AUTOMATIC;
    // no per-entity liveliness (MANUAL_BY_TOPIC et al.) has been built on top of it yet.
    if (RMW_QOS_POLICY_LIVELINESS_AUTOMATIC != qos_profile->liveliness &&
        RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT != qos_profile->liveliness) {
        RMW_SET_ERROR_MSG("rmw_tickle only supports RMW_QOS_POLICY_LIVELINESS_AUTOMATIC for now - "
                          "see rmw_tickle/PLAN.md's QoS roadmap #3 (LIVELINESS)");
        return RMW_RET_UNSUPPORTED;
    }
    if (!rmw_time_equal(qos_profile->liveliness_lease_duration,
                        (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT)) {
        RMW_SET_ERROR_MSG("rmw_tickle doesn't support a custom liveliness_lease_duration yet - see "
                          "rmw_tickle/PLAN.md's QoS roadmap #3 (LIVELINESS)");
        return RMW_RET_UNSUPPORTED;
    }

    // QoS roadmap #2 (DEADLINE) - no elapsed-time monitoring since last publish/receive exists.
    if (!rmw_time_equal(qos_profile->deadline, (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT)) {
        RMW_SET_ERROR_MSG("rmw_tickle doesn't support a finite DEADLINE yet - see rmw_tickle/"
                          "PLAN.md's QoS roadmap #2 (DEADLINE)");
        return RMW_RET_UNSUPPORTED;
    }

    // QoS roadmap #6 (LIFESPAN) - needs #1/#4's storage to already exist, neither does.
    if (!rmw_time_equal(qos_profile->lifespan, (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT)) {
        RMW_SET_ERROR_MSG("rmw_tickle doesn't support a finite LIFESPAN yet - see rmw_tickle/"
                          "PLAN.md's QoS roadmap #6 (LIFESPAN)");
        return RMW_RET_UNSUPPORTED;
    }

    // QoS roadmap #1 (HISTORY/DEPTH) - depth is honored (see rmw_create_subscription()'s own
    // queue_capacity sizing), but KEEP_ALL asks for an *unbounded* queue, which a fixed-capacity
    // allocation can't provide - only meaningful for subscriptions (a service/client/publisher has
    // no reader-side queue at all in this rmw's model).
    if (is_subscription && RMW_QOS_POLICY_HISTORY_KEEP_ALL == qos_profile->history) {
        RMW_SET_ERROR_MSG("rmw_tickle doesn't support RMW_QOS_POLICY_HISTORY_KEEP_ALL (an unbounded "
                          "queue) - see rmw_tickle/PLAN.md's QoS roadmap #1 (HISTORY/DEPTH)");
        return RMW_RET_UNSUPPORTED;
    }

    return RMW_RET_OK;
}
