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
#include <stdint.h> // uint64_t - duration_offered_satisfies_requested()'s own nanosecond math
#include <stdio.h>  // snprintf() - rmw_qos_profile_check_compatible()'s own `reason` buffer

#include "rcutils/error_handling.h" // RCUTILS_CHECK_ARGUMENT_FOR_NULL
#include "rmw/error_handling.h"
#include "rmw/qos_profiles.h" // rmw_qos_profile_check_compatible(), rmw_qos_compatibility_type_t
#include "rmw/ret_types.h"
#include "rmw/time.h" // rmw_time_equal, RMW_QOS_*_DEFAULT
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t rmw_tickle_validate_qos_profile(const rmw_qos_profile_t* qos_profile, rmw_tickle_entity_kind_t entity_kind) {
    // QoS roadmap #5 (RELIABILITY): full ACK/NACK + retransmission (what a topic's Publisher/
    // Subscriber would need) isn't built - a topic has no retry mechanism of any kind, so only
    // BEST_EFFORT is honest to accept there. A service/client is different: TickLE's own tt_
    // Client_call() *already* retries a call up to tt_CALL_RETRY_COUNT times (tt_CALL_RETRY_
    // INTERVAL apart) regardless of what QoS was requested - a real, if bounded (not indefinite),
    // delivery-assurance mechanism topics simply don't have. Accepting RELIABLE for services/
    // clients reflects that existing behavior rather than adding anything new, and - found while
    // provisioning rmw_tickle/PLAN.md's rmw-perf.yml benchmark rig - is a practical necessity, not
    // just a nicety: a real rclcpp::Node unconditionally creates internal services (e.g. the type
    // description service, `rcl_node_type_description_service_init()`) at `rmw_qos_profile_
    // services_default` (RELIABLE), with no `rcl_node_options_t` flag to opt out - rejecting
    // RELIABLE for every service would make rmw_tickle unable to host *any* real rclcpp node at
    // all, not just ones that ask for it explicitly.
    bool reliability_ok = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT == qos_profile->reliability ||
                          RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT == qos_profile->reliability ||
                          (RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT == entity_kind &&
                           RMW_QOS_POLICY_RELIABILITY_RELIABLE == qos_profile->reliability);
    if (!reliability_ok) {
        RMW_SET_ERROR_MSG("rmw_tickle only supports RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT (and, "
                          "for services/clients, RELIABLE - backed by tt_Client_call()'s own "
                          "bounded retry) for now - see rmw_tickle/PLAN.md's QoS roadmap #5 "
                          "(RELIABILITY)");
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
    if (RMW_TICKLE_ENTITY_SUBSCRIPTION == entity_kind && RMW_QOS_POLICY_HISTORY_KEEP_ALL == qos_profile->history) {
        RMW_SET_ERROR_MSG("rmw_tickle doesn't support RMW_QOS_POLICY_HISTORY_KEEP_ALL (an unbounded "
                          "queue) - see rmw_tickle/PLAN.md's QoS roadmap #1 (HISTORY/DEPTH)");
        return RMW_RET_UNSUPPORTED;
    }

    return RMW_RET_OK;
}

#define NSEC_PER_SEC 1000000000ULL
#define QOS_REASON_MAX_LEN 128 // longest of the fixed strings below, rounded up

// rmw's own public "would these two QoS profiles actually work together" utility - unlike rmw_
// tickle_validate_qos_profile() above, this takes *arbitrary* profiles (not just ones this rmw
// itself is about to create an entity with), so it has to implement the real DDS-style RxO
// (request/offer) compatibility rules generally, not just rmw_tickle's own narrow accepted subset.
// {0, 0} (RMW_DURATION_UNSPECIFIED, what RMW_QOS_DEADLINE_DEFAULT/_LIFESPAN_DEFAULT/_LIVELINESS_
// LEASE_DURATION_DEFAULT all expand to - rmw/time.h) means "no constraint from this side", not a
// literal zero-length duration - treated as always compatible below, never compared numerically.
static bool duration_offered_satisfies_requested(rmw_time_t offered, rmw_time_t requested) {
    bool offered_unspecified = rmw_time_equal(offered, (rmw_time_t)RMW_DURATION_UNSPECIFIED);
    bool requested_unspecified = rmw_time_equal(requested, (rmw_time_t)RMW_DURATION_UNSPECIFIED);
    if (offered_unspecified || requested_unspecified) {
        return true;
    }
    // Offered must be at least as tight (<=) as requested - a publisher promising a shorter
    // deadline/liveliness lease than the subscriber asked for satisfies the request; a longer one
    // doesn't.
    uint64_t offered_ns = (offered.sec * NSEC_PER_SEC) + offered.nsec;
    uint64_t requested_ns = (requested.sec * NSEC_PER_SEC) + requested.nsec;
    return offered_ns <= requested_ns;
}

rmw_ret_t rmw_qos_profile_check_compatible(const rmw_qos_profile_t publisher_profile,
                                           const rmw_qos_profile_t subscription_profile,
                                           rmw_qos_compatibility_type_t* compatibility, char* reason,
                                           size_t reason_size) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(compatibility, RMW_RET_INVALID_ARGUMENT);
    if (NULL == reason && reason_size != 0) {
        RMW_SET_ERROR_MSG("reason is null but reason_size is not 0");
        return RMW_RET_INVALID_ARGUMENT;
    }

    char reasons[3][QOS_REASON_MAX_LEN];
    size_t reason_count = 0;

    // RELIABILITY: a BEST_EFFORT publisher can't satisfy a RELIABLE subscriber's request (the
    // reverse - RELIABLE publisher, BEST_EFFORT subscriber - is fine, the subscriber just gets
    // more reliability than it asked for).
    if (RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT == publisher_profile.reliability &&
        RMW_QOS_POLICY_RELIABILITY_RELIABLE == subscription_profile.reliability) {
        snprintf(reasons[reason_count++], sizeof(reasons[0]),
                 "ERROR: Best effort publisher offered, but reliable subscription requested");
    }

    // DURABILITY: rank VOLATILE < TRANSIENT_LOCAL - a VOLATILE publisher (delivers only to
    // currently-matched subscribers) can't satisfy a TRANSIENT_LOCAL subscriber (wants
    // previously-published samples backfilled too).
    if (RMW_QOS_POLICY_DURABILITY_VOLATILE == publisher_profile.durability &&
        RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL == subscription_profile.durability) {
        snprintf(reasons[reason_count++], sizeof(reasons[0]),
                 "ERROR: Volatile publisher offered, but transient local subscription requested");
    }

    // DEADLINE: the publisher's own promised max inter-publish gap must be at least as tight as
    // whatever the subscriber is willing to accept.
    if (!duration_offered_satisfies_requested(publisher_profile.deadline, subscription_profile.deadline)) {
        snprintf(reasons[reason_count++], sizeof(reasons[0]),
                 "ERROR: Publisher offered deadline is greater than subscription requested deadline");
    }

    // LIVELINESS: rank AUTOMATIC < MANUAL_BY_TOPIC - an AUTOMATIC publisher (TickLE's own process-
    // level liveliness) can't satisfy a subscriber that specifically wants per-topic manual
    // assertion; the lease duration itself follows the same offered<=requested rule as DEADLINE.
    if (RMW_QOS_POLICY_LIVELINESS_AUTOMATIC == publisher_profile.liveliness &&
        RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == subscription_profile.liveliness) {
        snprintf(reasons[reason_count++], sizeof(reasons[0]),
                 "ERROR: Automatic liveliness publisher offered, but manual by topic liveliness "
                 "subscription requested");
    } else if (!duration_offered_satisfies_requested(publisher_profile.liveliness_lease_duration,
                                                     subscription_profile.liveliness_lease_duration)) {
        snprintf(reasons[reason_count++], sizeof(reasons[0]),
                 "ERROR: Publisher offered liveliness lease duration is greater than subscription "
                 "requested liveliness lease duration");
    }

    *compatibility = (reason_count > 0) ? RMW_QOS_COMPATIBILITY_ERROR : RMW_QOS_COMPATIBILITY_OK;

    if (NULL != reason && reason_size > 0) {
        reason[0] = '\0';
        size_t used = 0;
        for (size_t i = 0; i < reason_count && used < reason_size - 1; i++) {
            int written = snprintf(reason + used, reason_size - used, "%s%s", used > 0 ? "; " : "", reasons[i]);
            if (written > 0) {
                used += (size_t)written;
            }
        }
    }

    return RMW_RET_OK;
}
