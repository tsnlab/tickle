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
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, false));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, true));

    qos = valid_profile();
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, false));

    qos = valid_profile();
    qos.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, false));

    qos = valid_profile();
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, false));

    qos = valid_profile();
    qos.liveliness_lease_duration.sec = 1;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, false));

    qos = valid_profile();
    qos.deadline.sec = 1;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, false));

    qos = valid_profile();
    qos.lifespan.sec = 1;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, false));

    // KEEP_ALL is only rejected for a subscription (an unbounded reader queue) - a publisher/
    // client/service has no reader-side queue at all in this rmw's model, so it's harmless there.
    qos = valid_profile();
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
    assert(RMW_RET_UNSUPPORTED == rmw_tickle_validate_qos_profile(&qos, true));
    assert(RMW_RET_OK == rmw_tickle_validate_qos_profile(&qos, false));

    printf("rmw_tickle_validate_qos_profile: PASS\n");
    return 0;
}
