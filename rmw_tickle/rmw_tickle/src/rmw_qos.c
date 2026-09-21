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
#include <string.h> // strcmp() - rmw_tickle_resolve_best_available()'s own topic-name matching

#include <tickle/config.h> // tt_LIVELINESS_MISS_THRESHOLD, tt_NODE_UPDATE_INTERVAL - see Milestone
                           // 18's own note on why this needs a direct include, not just tickle.h
#include <tickle/tickle.h> // tt_Discovery/tt_DiscoveredEntity, tt_KIND_TOPIC_*, tt_UPDATE_QOS_*,
                           // tt_NODE_ID_INVALID, tt_MAX_DISCOVERED_ENTITIES - rmw_tickle_resolve_
                           // best_available()'s own discovery-table scan below

#include "rcutils/error_handling.h" // RCUTILS_CHECK_ARGUMENT_FOR_NULL
#include "rmw/error_handling.h"
#include "rmw/qos_profiles.h" // rmw_qos_profile_check_compatible(), rmw_qos_compatibility_type_t
#include "rmw/ret_types.h"
#include "rmw/time.h" // rmw_time_equal, RMW_QOS_*_DEFAULT
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t rmw_tickle_validate_qos_profile(const rmw_qos_profile_t* qos_profile, rmw_tickle_entity_kind_t entity_kind) {
    // QoS roadmap #5 (RELIABILITY) - done. A service/client's RELIABLE was already backed by tt_
    // Client_call()'s own pre-existing bounded retry (tt_CALL_RETRY_COUNT); a topic Publisher/
    // Subscriber's RELIABLE is now backed by TickLE core's own ACKNACK + retransmission (struct
    // tt_ReliableCache, tt_Subscriber.reliable - tickle.h) - see rmw_create_publisher()/rmw_
    // create_subscription() for how qos_profile->depth/->reliability get threaded into it. Every
    // rmw_qos_profile_t this validates has already had its own reliability requested explicitly
    // (SYSTEM_DEFAULT/UNKNOWN aren't RELIABLE or BEST_EFFORT - both pass through unchanged below).
    // DDS QoS policy coverage inventory gap 1 (rmw_tickle/PLAN.md, 2026-09-21) - BEST_AVAILABLE is
    // accepted for publishers/subscriptions (rmw_tickle_resolve_best_available(), below, replaces it
    // with a concrete value before anything else reads qos_profile->reliability) - rmw/types.h's own
    // doc comment on RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE explicitly excludes services/clients
    // ("not supported... default to the reliability value in rmw_qos_profile_services_default"), so
    // it's rejected there, same as TRANSIENT_LOCAL's own existing service/client exclusion below.
    bool reliability_ok = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT == qos_profile->reliability ||
                          RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT == qos_profile->reliability ||
                          RMW_QOS_POLICY_RELIABILITY_RELIABLE == qos_profile->reliability ||
                          (RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE == qos_profile->reliability &&
                           RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT != entity_kind);
    if (!reliability_ok) {
        RMW_SET_ERROR_MSG("rmw_tickle only supports RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT, "
                          "RMW_QOS_POLICY_RELIABILITY_RELIABLE, and (for publishers/subscriptions) "
                          "RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE - see rmw_tickle/PLAN.md's QoS "
                          "roadmap #5 (RELIABILITY)");
        return RMW_RET_UNSUPPORTED;
    }

    // QoS roadmap #4 (DURABILITY) - done for topics. A Publisher's TRANSIENT_LOCAL is now backed
    // by TickLE core's own retained-sample cache + discovery-triggered backlog delivery (struct
    // tt_ReliableCache, tt_Publisher.durable - tickle.h, shared with QoS roadmap #5's own
    // RELIABILITY cache since PLAN.md's Milestone 24) - see rmw_create_publisher() for how
    // qos_profile->depth threads into it; a Subscription needs no field at all
    // (tt_Subscriber.durable doesn't exist - backlog delivery is purely a Publisher-side decision).
    // Still VOLATILE-only for services/clients: tt_Client_call() is request/response, not pub/sub,
    // so "retained backlog for a late-joining client" has no TickLE-core mechanism behind it at
    // all (unlike RELIABILITY just above, where the RPC layer's own existing retry already served
    // as the service-side implementation). BEST_AVAILABLE mirrors TRANSIENT_LOCAL's own exclusion
    // for the identical reason (DDS QoS policy coverage inventory gap 1).
    bool durability_ok = RMW_QOS_POLICY_DURABILITY_VOLATILE == qos_profile->durability ||
                         RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT == qos_profile->durability ||
                         ((RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL == qos_profile->durability ||
                           RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE == qos_profile->durability) &&
                          RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT != entity_kind);
    if (!durability_ok) {
        RMW_SET_ERROR_MSG("rmw_tickle only supports RMW_QOS_POLICY_DURABILITY_VOLATILE and, for "
                          "publishers/subscriptions, RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL/"
                          "BEST_AVAILABLE - see rmw_tickle/PLAN.md's QoS roadmap #4 (DURABILITY)");
        return RMW_RET_UNSUPPORTED;
    }

    // QoS roadmap #3 (LIVELINESS) - done, both kinds this rmw's own rmw_qos_policy_liveliness_t
    // still defines (MANUAL_BY_PARTICIPANT/_BY_NODE were removed from the real rmw spec some time
    // ago - only AUTOMATIC and MANUAL_BY_TOPIC remain). AUTOMATIC is backed by generalizing check_
    // liveliness()'s own existing per-node peer-death detection (see rmw_subscription.c's own
    // RMW_EVENT_LIVELINESS_CHANGED handling). MANUAL_BY_TOPIC (Milestone 32) is backed by a real
    // rmw_publisher_assert_liveliness() (rmw_publisher.c) plus the same watchdog thread
    // AUTOMATIC's own RMW_EVENT_LIVELINESS_LOST uses (Milestone 30, check_liveliness_lost(),
    // rmw_node.c) - checking this Publisher's own explicit-assertion lease instead of node-wide
    // poll_thread health. Honest, documented limitation: this is entirely a *local* signal (this
    // process's own knowledge of whether it met its own obligation) - TickLE's wire protocol has
    // no per-endpoint liveliness signal at all, only node-wide presence, so a remote
    // Subscription's own RMW_EVENT_LIVELINESS_CHANGED can't distinguish "this one manual-
    // liveliness Publisher went not-alive" from "the whole node is still fine" the way real DDS's
    // own per-writer liveliness protocol can.
    // BEST_AVAILABLE mirrors MANUAL_BY_TOPIC's own service/client exclusion, same reasoning as
    // reliability/durability above (DDS QoS policy coverage inventory gap 1).
    bool liveliness_ok = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC == qos_profile->liveliness ||
                         RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == qos_profile->liveliness ||
                         RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT == qos_profile->liveliness ||
                         (RMW_QOS_POLICY_LIVELINESS_BEST_AVAILABLE == qos_profile->liveliness &&
                          RMW_TICKLE_ENTITY_SERVICE_OR_CLIENT != entity_kind);
    if (!liveliness_ok) {
        RMW_SET_ERROR_MSG("rmw_tickle only supports RMW_QOS_POLICY_LIVELINESS_AUTOMATIC/"
                          "MANUAL_BY_TOPIC and (for publishers/subscriptions) BEST_AVAILABLE - see "
                          "rmw_tickle/PLAN.md's QoS roadmap #3 (LIVELINESS)");
        return RMW_RET_UNSUPPORTED;
    }
    // A custom lease_duration is accepted, but only down to tt_LIVELINESS_MISS_THRESHOLD *
    // tt_NODE_UPDATE_INTERVAL - TickLE core's own fastest possible peer-death detection latency
    // (check_liveliness(), tickle.c). Rejected explicitly below that floor rather than silently
    // rounding it up to what TickLE can actually honor - same "reject, don't silently downgrade"
    // philosophy as RELIABLE's/DURABLE's own depth-cap rejection. DEFAULT (unspecified, {0,0}) and
    // an explicit RMW_DURATION_INFINITE both mean "no constraint" - always accepted.
    // RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE ({9223372036, 854775806}, one ns short of
    // RMW_DURATION_INFINITE) already clears this floor numerically without needing a special case -
    // resolved to a real value later (rmw_tickle_resolve_best_available()) before anything reads it.
    if (!rmw_time_equal(qos_profile->liveliness_lease_duration,
                        (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT) &&
        !rmw_time_equal(qos_profile->liveliness_lease_duration, (rmw_time_t)RMW_DURATION_INFINITE) &&
        rmw_time_total_nsec(qos_profile->liveliness_lease_duration) <
            (rmw_duration_t)((uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL)) {
        RMW_SET_ERROR_MSG("rmw_tickle's own liveliness_lease_duration floor is "
                          "tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL (TickLE core's "
                          "fastest possible peer-death detection) - see rmw_tickle/PLAN.md's QoS "
                          "roadmap #3 (LIVELINESS)");
        return RMW_RET_UNSUPPORTED;
    }

    // QoS roadmap #2 (DEADLINE) - done. Any finite value is accepted: a purely local rmw_tickle-
    // side timer (tt_Node_schedule(), no TickLE wire/network cadence to be bounded by) - see
    // rmw_publisher.c/rmw_subscription.c's own RMW_EVENT_OFFERED_DEADLINE_MISSED/REQUESTED_
    // DEADLINE_MISSED handling.

    // QoS roadmap #6 (LIFESPAN) - done. Any finite value is accepted, same "any finite value,
    // purely local" reasoning as DEADLINE just above - see tt_Publisher.lifespan_duration_ns's own
    // doc comment (tickle.h) and rmw_tickle_subscriber_t.lifespan_ns's own (below) for the actual
    // Publisher-side (reliable_cache expiry) and Subscription-side (queue expiry) mechanisms this
    // now backs.

    // QoS roadmap #1 (HISTORY/DEPTH) - depth is honored (see rmw_create_subscription()'s own
    // queue_capacity sizing), but KEEP_ALL asks for an *unbounded* queue, which a fixed-capacity
    // allocation can't provide - rejected here for Subscriptions (a service/client/publisher has no
    // reader-side queue at all in this rmw's model, so KEEP_ALL is meaningless for them and this
    // check doesn't apply). A Publisher's own KEEP_ALL is a different, meaningful request instead
    // (its own reliable/durable retained-sample cache) - DDS QoS policy coverage inventory gap 2
    // (rmw_tickle/PLAN.md, 2026-09-21): passes validation unrejected, honored by setup_reliable_
    // cache() (rmw_publisher.c) with a large-but-bounded RMW_TICKLE_KEEP_ALL_DEPTH cache instead of
    // silently downgrading to a small default depth the way it used to.
    if (RMW_TICKLE_ENTITY_SUBSCRIPTION == entity_kind && RMW_QOS_POLICY_HISTORY_KEEP_ALL == qos_profile->history) {
        RMW_SET_ERROR_MSG("rmw_tickle doesn't support RMW_QOS_POLICY_HISTORY_KEEP_ALL (an unbounded "
                          "queue) - see rmw_tickle/PLAN.md's QoS roadmap #1 (HISTORY/DEPTH)");
        return RMW_RET_UNSUPPORTED;
    }

    return RMW_RET_OK;
}

// Shared by rmw_tickle_resolve_best_available()'s own three enum-policy branches below - true iff
// every currently-discovered, alive entity of `opposite_kind` on `topic_name` has `qos_bit` set in
// its own announced tt_UpdateEntity.qos (tt_UPDATE_QOS_RELIABLE/_DURABLE/_LIVELINESS_MANUAL,
// refreshed on every UPDATE - struct tt_DiscoveredEntity's own doc comment, tickle.h). Zero matching
// entities vacuously returns true, matching rmw/types.h's own "[the strict policy] will be chosen
// if it matches with all discovered endpoints" wording literally - none to disagree with yet.
static bool all_matching_entities_offer(const struct tt_Discovery* discovery, const char* topic_name,
                                        uint8_t opposite_kind, uint8_t qos_bit) {
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        const struct tt_DiscoveredEntity* entity = &discovery->entities[i];
        if (tt_NODE_ID_INVALID == entity->node_id || !entity->alive || entity->kind != opposite_kind ||
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

// Shared by rmw_tickle_resolve_best_available()'s own two duration-sentinel branches below.
// `get_ns` selects deadline_duration_ns vs liveliness_lease_duration_ns (entity_deadline_ns()/
// entity_lease_ns() above). `want_max` selects the Subscription-side aggregation (max of
// discovered Publisher values) vs. the Publisher-side one (min of discovered Subscription values) -
// rmw/types.h's own doc comment on RMW_QOS_DEADLINE_BEST_AVAILABLE/_LIVELINESS_LEASE_DURATION_
// BEST_AVAILABLE spells out exactly which side gets which. A discovered entity's own 0 means "no
// requirement" (struct tt_DiscoveredEntity's own doc comment, tickle.h) - substituted with
// UINT64_MAX ("unconstrained") for this computation so it never wins a min (an unconstrained peer
// shouldn't narrow anything) but always wins a max (an unconstrained peer forces the whole result
// unconstrained too, since staying compatible with it requires matching its own lack of a bound) -
// converted back to the same 0 convention on the way out if the final result is still UINT64_MAX
// (every matching entity was itself unconstrained) or nothing matched at all.
static uint64_t resolve_best_available_duration(const struct tt_Discovery* discovery, const char* topic_name,
                                                uint8_t opposite_kind, bool want_max,
                                                uint64_t (*get_ns)(const struct tt_DiscoveredEntity*)) {
    uint64_t result = want_max ? 0 : UINT64_MAX;
    bool any_matched = false;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        const struct tt_DiscoveredEntity* entity = &discovery->entities[i];
        if (tt_NODE_ID_INVALID == entity->node_id || !entity->alive || entity->kind != opposite_kind ||
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

rmw_qos_profile_t rmw_tickle_resolve_best_available(const rmw_qos_profile_t* requested,
                                                    const struct tt_Discovery* discovery, const char* topic_name,
                                                    rmw_tickle_entity_kind_t entity_kind) {
    rmw_qos_profile_t resolved = *requested;
    bool is_publisher = RMW_TICKLE_ENTITY_PUBLISHER == entity_kind;
    uint8_t opposite_kind = is_publisher ? tt_KIND_TOPIC_SUBSCRIBER : tt_KIND_TOPIC_PUBLISHER;

    // RELIABILITY/DURABILITY/LIVELINESS - see this function's own doc comment (rmw_tickle.h). An
    // *offering* Publisher is always RxO-compatible choosing the strict option (rmw_qos_profile_
    // check_compatible()'s own rules above: an offered-strict value is never the one that conflicts
    // with a requested value, whatever that request is), so it resolves trivially without scanning
    // discovery at all; a *requesting* Subscription must actually check every discovered Publisher's
    // own concrete offer, since requesting strict would be incompatible with one that only offers
    // loose.
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

    // DEADLINE/LIVELINESS_LEASE_DURATION - genuinely asymmetric per rmw/types.h's own doc comment
    // (unlike the three enum policies above, BOTH sides do a real discovery-dependent computation,
    // not a discovery-independent trivial one): a Subscription takes the *maximum* of every
    // discovered Publisher's own value (the loosest bound still compatible with all of them); a
    // Publisher takes the *minimum* of every discovered Subscription's own value (only as strict as
    // the tightest real requirement demands, no stricter).
    if (rmw_time_equal(requested->deadline, (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE)) {
        uint64_t deadline_ns =
            resolve_best_available_duration(discovery, topic_name, opposite_kind, !is_publisher, entity_deadline_ns);
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
