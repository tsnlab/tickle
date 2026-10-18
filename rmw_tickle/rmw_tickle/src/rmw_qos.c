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

#include <stdarg.h> // va_list - qos_reason_append()
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>  // vsnprintf() - rmw_qos_profile_check_compatible()'s own `reason` buffer
#include <string.h> // memset() - rmw_tickle_qos_to_tt()

#include <tickle/config.h> // tt_CONTEXT_TX_INTERVAL - see Milestone
                           // 18's own note on why this needs a direct include, not just tickle.h
#include <tickle/tickle.h> // struct tt_QosProfile - rmw_tickle_qos_to_tt() / _from_tt()

#include "rcutils/error_handling.h" // RCUTILS_CHECK_ARGUMENT_FOR_NULL
#include "rmw/error_handling.h"
#include "rmw/qos_profiles.h"           // rmw_qos_profile_check_compatible(), rmw_qos_compatibility_type_t
#include "rmw/qos_string_conversions.h" // rmw_qos_*_policy_to_str() - the warnings' policy names
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
    // accepted for publishers/subscriptions (core's tt_QosProfile_resolve(), at creation, replaces it
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
    // A custom lease_duration is accepted down to tt_LIVELINESS_LEASE_DIVISOR (6) tt_CONTEXT_TX_INTERVALs, 6 ms
    // by default. The core honours any lease above that (LIVELINESS_PLAN.md, 2026-09-26): it runs from the last sign of
    // life, is checked by a timer at the expiry, and a node sends its summary tt_LIVELINESS_LEASE_DIVISOR times a
    // lease, which cannot go below one tx tick. The floor used to be tt_LIVELINESS_MISS_THRESHOLD *
    // tt_CONTEXT_UPDATE_INTERVAL (3 s), when the core checked once a second and cut every lease at its node-level
    // limit. Rejected below the floor rather than silently rounded up - same "reject, don't silently downgrade"
    // philosophy as RELIABLE's/DURABLE's own depth-cap rejection. DEFAULT (unspecified, {0,0}) and
    // an explicit RMW_DURATION_INFINITE both mean "no constraint" - always accepted.
    // RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE ({9223372036, 854775806}, one ns short of
    // RMW_DURATION_INFINITE) already clears this floor numerically without needing a special case -
    // resolved to a real value at creation (tt_QosProfile_resolve()) before anything reads it.
    if (!rmw_time_equal(qos_profile->liveliness_lease_duration,
                        (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT) &&
        !rmw_time_equal(qos_profile->liveliness_lease_duration, (rmw_time_t)RMW_DURATION_INFINITE) &&
        rmw_time_total_nsec(qos_profile->liveliness_lease_duration) <
            (rmw_duration_t)(tt_LIVELINESS_LEASE_DIVISOR * (uint64_t)tt_CONTEXT_TX_INTERVAL)) {
        RMW_SET_ERROR_MSG("rmw_tickle's own liveliness_lease_duration floor is "
                          "tt_LIVELINESS_LEASE_DIVISOR * tt_CONTEXT_TX_INTERVAL (a node's summary goes out "
                          "tt_LIVELINESS_LEASE_DIVISOR times its "
                          "shortest lease) - see rmw_tickle/LIVELINESS_PLAN.md");
        return RMW_RET_UNSUPPORTED;
    }

    // QoS roadmap #2 (DEADLINE) - done. Any finite value is accepted: a purely local rmw_tickle-
    // side timer (tt_Context_schedule(), no TickLE wire/network cadence to be bounded by) - see
    // rmw_publisher.c/rmw_subscription.c's own RMW_EVENT_OFFERED_DEADLINE_MISSED/REQUESTED_
    // DEADLINE_MISSED handling.

    // QoS roadmap #6 (LIFESPAN) - done. Any finite value is accepted, same "any finite value,
    // purely local" reasoning as DEADLINE just above - see tt_Publisher.lifespan_duration_ns's own
    // doc comment (tickle.h) and rmw_tickle_subscriber_t.lifespan_ns's own (below) for the actual
    // Publisher-side (reliable_cache expiry) and Subscription-side (queue expiry) mechanisms this
    // now backs.

    // QoS roadmap #1 (HISTORY/DEPTH) - both policies are accepted now, on either kind of entity.
    //
    // KEEP_ALL used to be refused on a Subscription as "an unbounded queue", and that reading was
    // wrong (g13, RMW_GAPS_PLAN.md). In DDS, KEEP_ALL is bounded by RESOURCE_LIMITS like anything
    // else; what distinguishes it from KEEP_LAST is not capacity but what a full cache does -
    // KEEP_LAST overwrites the oldest unread sample and KEEP_ALL refuses to, holding the writer
    // back instead. That promise is keepable in bounded memory, which is why refusing it cost us
    // `ros2 bag record` for nothing. rmw_create_subscription() sizes the queue from
    // RMW_TICKLE_READER_KEEP_ALL_BYTES and declines arriving samples when it is full; a RELIABLE
    // writer then retransmits, a BEST_EFFORT one cannot and the sample is dropped and counted.
    //
    // A Publisher's KEEP_ALL is a different, already-honoured request (its retained-sample cache,
    // setup_reliable_cache() in rmw_publisher.c), and a service or client has no reader-side queue
    // in this rmw's model, so HISTORY says nothing about them either way.

    return RMW_RET_OK;
}

// ---- QoS redesign step 3: rmw_qos_profile_t <-> struct tt_QosProfile (rmw_tickle.h says what each side owns).

// The lease rmw_tickle has always enforced, and announced, for a MANUAL_BY_TOPIC Publisher that gave none.
#define RMW_TICKLE_MANUAL_DEFAULT_LEASE_NS ((uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_CONTEXT_UPDATE_INTERVAL)

// A duration for core: unspecified and infinite are infinite, `best_available` (the policy's sentinel, when it has
// one) core's tt_QOS_DURATION_BEST_AVAILABLE.
static uint64_t duration_to_tt(rmw_time_t duration, bool has_best_available, rmw_time_t best_available) {
    if (has_best_available && rmw_time_equal(duration, best_available)) {
        return tt_QOS_DURATION_BEST_AVAILABLE;
    }
    if (rmw_time_equal(duration, (rmw_time_t)RMW_DURATION_UNSPECIFIED) ||
        rmw_time_equal(duration, (rmw_time_t)RMW_DURATION_INFINITE)) {
        return tt_QOS_DURATION_INFINITE;
    }
    rmw_duration_t nanoseconds = rmw_time_total_nsec(duration);
    return nanoseconds > 0 ? (uint64_t)nanoseconds : tt_QOS_DURATION_INFINITE;
}

// Infinite goes back as unspecified, which is what rmw_tickle reported for every default duration before core
// resolved them, and what rmw reads as infinite.
static rmw_time_t duration_from_tt(uint64_t nanoseconds) {
    if (tt_QOS_DURATION_INFINITE == nanoseconds || nanoseconds > (uint64_t)INT64_MAX) {
        return (rmw_time_t)RMW_DURATION_UNSPECIFIED;
    }
    return rmw_time_from_nsec((rmw_duration_t)nanoseconds);
}

static enum tt_QosReliabilityKind reliability_to_tt(rmw_qos_reliability_policy_t reliability) {
    switch (reliability) {
    case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
        return tt_QOS_RELIABILITY_RELIABLE;
    case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
        return tt_QOS_RELIABILITY_BEST_EFFORT;
    case RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE:
        return tt_QOS_RELIABILITY_BEST_AVAILABLE;
    default:
        return tt_QOS_RELIABILITY_SYSTEM_DEFAULT; // rmw_qos_profile_default's: RELIABLE (decision D7)
    }
}

static enum tt_QosDurabilityKind durability_to_tt(rmw_qos_durability_policy_t durability) {
    switch (durability) {
    case RMW_QOS_POLICY_DURABILITY_VOLATILE:
        return tt_QOS_DURABILITY_VOLATILE;
    case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL:
        return tt_QOS_DURABILITY_TRANSIENT_LOCAL;
    case RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE:
        return tt_QOS_DURABILITY_BEST_AVAILABLE;
    default:
        return tt_QOS_DURABILITY_SYSTEM_DEFAULT; // VOLATILE
    }
}

static enum tt_QosLivelinessKind liveliness_to_tt(rmw_qos_liveliness_policy_t liveliness) {
    switch (liveliness) {
    case RMW_QOS_POLICY_LIVELINESS_AUTOMATIC:
        return tt_QOS_LIVELINESS_AUTOMATIC;
    case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC:
        return tt_QOS_LIVELINESS_MANUAL_BY_TOPIC;
    case RMW_QOS_POLICY_LIVELINESS_BEST_AVAILABLE:
        return tt_QOS_LIVELINESS_BEST_AVAILABLE;
    default:
        return tt_QOS_LIVELINESS_SYSTEM_DEFAULT; // AUTOMATIC
    }
}

void rmw_tickle_qos_to_tt(const rmw_qos_profile_t* qos, rmw_tickle_entity_kind_t kind,
                          const rmw_tickle_qos_extensions_t* ext, struct tt_QosProfile* out) {
    static const rmw_tickle_qos_extensions_t no_extensions;
    const rmw_tickle_qos_extensions_t* extensions = NULL != ext ? ext : &no_extensions;
    memset(out, 0, sizeof(*out));

    // HISTORY: rmw_tickle has only ever told KEEP_ALL apart; anything else is KEEP_LAST. rmw has no depth for
    // KEEP_ALL, so the endpoint's bound is used there.
    bool keep_all = RMW_QOS_POLICY_HISTORY_KEEP_ALL == qos->history;
    out->history.kind = keep_all ? tt_QOS_HISTORY_KEEP_ALL : tt_QOS_HISTORY_KEEP_LAST;
    size_t depth = keep_all || 0 == qos->depth ? (size_t)extensions->depth : qos->depth;
    if (RMW_TICKLE_ENTITY_PUBLISHER != kind && depth > (size_t)UINT16_MAX) {
        depth = UINT16_MAX; // the rmw queue keeps all of it; core counts it in 16 bits
    }
    out->history.depth = depth > (size_t)UINT32_MAX ? UINT32_MAX : (uint32_t)depth; // core refuses > UINT16_MAX
    out->history.bytes = extensions->history_bytes;

    out->reliability.kind = reliability_to_tt(qos->reliability);
    if (RMW_TICKLE_ENTITY_PUBLISHER == kind && NULL != ext) {
        out->reliability.max_blocking_ns = 0 != ext->max_blocking_ns ? ext->max_blocking_ns : 1U;
    }
    out->reliability.tracking_words = extensions->tracking_words;
    out->reliability.reorder_slots = extensions->reorder_slots;

    out->durability.kind = durability_to_tt(qos->durability);
    out->deadline.period_ns = duration_to_tt(qos->deadline, true, (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE);
    out->lifespan.duration_ns = duration_to_tt(qos->lifespan, false, (rmw_time_t)RMW_DURATION_UNSPECIFIED);
    out->liveliness.kind = liveliness_to_tt(qos->liveliness);
    out->liveliness.lease_ns = duration_to_tt(qos->liveliness_lease_duration, true,
                                              (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE);
    if (RMW_TICKLE_ENTITY_PUBLISHER == kind && tt_QOS_LIVELINESS_MANUAL_BY_TOPIC == out->liveliness.kind &&
        rmw_time_equal(qos->liveliness_lease_duration, (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT)) {
        out->liveliness.lease_ns = RMW_TICKLE_MANUAL_DEFAULT_LEASE_NS;
    }
}

void rmw_tickle_qos_from_tt(const struct tt_QosProfile* actual, const rmw_qos_profile_t* requested,
                            rmw_qos_profile_t* out) {
    *out = *requested;
    bool keep_all = tt_QOS_HISTORY_KEEP_ALL == actual->history.kind;
    out->history = keep_all ? RMW_QOS_POLICY_HISTORY_KEEP_ALL : RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    // KEEP_ALL's depth means nothing to rmw; a subscription's depth past core's 16 bits is the rmw queue's.
    if (!keep_all && !(requested->depth > (size_t)UINT16_MAX && actual->history.depth == UINT16_MAX)) {
        out->depth = actual->history.depth;
    }
    out->reliability = tt_QOS_RELIABILITY_RELIABLE == actual->reliability.kind ? RMW_QOS_POLICY_RELIABILITY_RELIABLE
                                                                               : RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    out->durability = tt_QOS_DURABILITY_TRANSIENT_LOCAL == actual->durability.kind
                          ? RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL
                          : RMW_QOS_POLICY_DURABILITY_VOLATILE;
    out->deadline = duration_from_tt(actual->deadline.period_ns);
    out->lifespan = duration_from_tt(actual->lifespan.duration_ns);
    out->liveliness = tt_QOS_LIVELINESS_MANUAL_BY_TOPIC == actual->liveliness.kind
                          ? RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC
                          : RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    out->liveliness_lease_duration = duration_from_tt(actual->liveliness.lease_ns);
}

uint64_t rmw_tickle_max_blocking_from_tt(const struct tt_QosProfile* actual) {
    return actual->reliability.max_blocking_ns <= 1U ? 0 : actual->reliability.max_blocking_ns;
}

#define QOS_WARNINGS_MAX_LEN 512 // every warning at once, with the longest policy names

// rmw_qos_profile_check_compatible()'s `reason`, written as rmw asks: every finding as "ERROR: ...;" or
// "WARNING: ...;", errors first, cut short at the buffer's end rather than refused.
struct qos_reasons {
    char* buf;
    size_t size;
    size_t used;
};

// The findings so far: errors straight into the caller's `reason`, warnings into a buffer of their own, appended
// once the errors are all in.
struct qos_findings {
    struct qos_reasons errors;
    struct qos_reasons warnings;
    bool error;
    bool warning;
};

// NOLINTNEXTLINE(cert-dcl50-cpp) -- a C printf-style helper; the format strings are this file's own literals
static void qos_reason_append(struct qos_reasons* reasons, const char* format, ...) {
    if (NULL == reasons->buf || reasons->size == 0 || reasons->used >= reasons->size - 1) {
        return;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(reasons->buf + reasons->used, reasons->size - reasons->used, format, args);
    va_end(args);
    if (written > 0) {
        size_t room = reasons->size - reasons->used - 1;
        reasons->used += (size_t)written < room ? (size_t)written : room;
    }
}

static const char* policy_str(const char* name) {
    return NULL != name ? name : "unknown";
}

// A kind whose effect cannot be known from the profile alone: SYSTEM_DEFAULT and UNKNOWN, which rmw names, and
// BEST_AVAILABLE, which this rmw decides at creation from the endpoints discovery knows then (QoS plan D2).
static bool reliability_unknown(rmw_qos_reliability_policy_t kind) {
    return RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT == kind || RMW_QOS_POLICY_RELIABILITY_UNKNOWN == kind ||
           RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE == kind;
}

static bool durability_unknown(rmw_qos_durability_policy_t kind) {
    return RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT == kind || RMW_QOS_POLICY_DURABILITY_UNKNOWN == kind ||
           RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE == kind;
}

static bool liveliness_unknown(rmw_qos_liveliness_policy_t kind) {
    return RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT == kind || RMW_QOS_POLICY_LIVELINESS_UNKNOWN == kind ||
           RMW_QOS_POLICY_LIVELINESS_BEST_AVAILABLE == kind;
}

// One kind policy, by rmw's rule: an error when the offer is the weak value and the request the strong one; else a
// warning when an unknown kind decides the outcome - both unknown, an unknown offer to the strong request, or the weak
// offer to an unknown request. `name` names the policy in the reason; *_str are the two kinds as rmw spells them.
static void kind_findings(bool error, bool pub_unknown, bool sub_unknown, bool pub_weak, bool sub_strong,
                          const char* error_text, const char* name, const char* pub_str, const char* sub_str,
                          struct qos_findings* findings) {
    if (error) {
        findings->error = true;
        qos_reason_append(&findings->errors, "%s", error_text);
    } else if ((pub_unknown && sub_unknown) || (pub_unknown && sub_strong) || (pub_weak && sub_unknown)) {
        findings->warning = true;
        qos_reason_append(&findings->warnings, "WARNING: Publisher %s is %s and subscription %s is %s;", name,
                          policy_str(pub_str), name, policy_str(sub_str));
    }
}

// A DEADLINE period or LIVELINESS lease (`name`) as core resolves it (duration_to_tt(): unspecified and infinite are
// both infinite). An infinite offer cannot meet a finite request, nor a longer one a shorter. A BEST_AVAILABLE request
// takes the loosest offer there is, so it is always met; a BEST_AVAILABLE offer takes the tightest request discovery
// knows at its creation, so whether it meets this one depends on the order of creation: a warning.
static void duration_findings(rmw_time_t offered, rmw_time_t requested, rmw_time_t best_available, const char* name,
                              struct qos_findings* findings) {
    uint64_t offered_ns = duration_to_tt(offered, true, best_available);
    uint64_t requested_ns = duration_to_tt(requested, true, best_available);
    if (tt_QOS_DURATION_BEST_AVAILABLE == requested_ns || tt_QOS_DURATION_INFINITE == requested_ns) {
        return;
    }
    if (tt_QOS_DURATION_BEST_AVAILABLE == offered_ns) {
        findings->warning = true;
        qos_reason_append(&findings->warnings, "WARNING: Publisher %s is best available, subscription requests one;",
                          name);
    } else if (tt_QOS_DURATION_INFINITE == offered_ns) {
        findings->error = true;
        qos_reason_append(&findings->errors, "ERROR: Subscription requests a %s, publisher offers none;", name);
    } else if (offered_ns > requested_ns) {
        findings->error = true;
        qos_reason_append(&findings->errors, "ERROR: Subscription %s is shorter than the publisher's;", name);
    }
}

// rmw's own "would these two QoS profiles work together" utility, for arbitrary profiles. ERROR is what this rmw
// refuses to connect: the same rules core applies to an announced pair (tickle.c's qos_incompatible_policy()) -
// RELIABLE requested and BEST_EFFORT offered, TRANSIENT_LOCAL and VOLATILE, a period or lease requested and a longer
// or infinite one offered, MANUAL_BY_TOPIC and AUTOMATIC. WARNING is a pair that may or may not connect because a kind
// is SYSTEM_DEFAULT, UNKNOWN or BEST_AVAILABLE, or a duration BEST_AVAILABLE - rmw's rules for the first two (a
// warning only where the known side does not already decide it), extended to the third. Every finding is listed,
// errors before warnings, each ending in ';'.
rmw_ret_t rmw_qos_profile_check_compatible(const rmw_qos_profile_t publisher_profile,
                                           const rmw_qos_profile_t subscription_profile,
                                           rmw_qos_compatibility_type_t* compatibility, char* reason,
                                           size_t reason_size) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(compatibility, RMW_RET_INVALID_ARGUMENT);
    if (NULL == reason && reason_size != 0) {
        RMW_SET_ERROR_MSG("reason is null but reason_size is not 0");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (NULL != reason && reason_size > 0) {
        reason[0] = '\0';
    }
    char warning_text[QOS_WARNINGS_MAX_LEN];
    warning_text[0] = '\0';
    struct qos_findings findings = {.errors = {reason, reason_size, 0},
                                    .warnings = {warning_text, sizeof(warning_text), 0}};
    const rmw_qos_profile_t* pub = &publisher_profile;
    const rmw_qos_profile_t* sub = &subscription_profile;

    bool pub_weak = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT == pub->reliability;
    bool sub_strong = RMW_QOS_POLICY_RELIABILITY_RELIABLE == sub->reliability;
    kind_findings(pub_weak && sub_strong, reliability_unknown(pub->reliability), reliability_unknown(sub->reliability),
                  pub_weak, sub_strong, "ERROR: Best effort publisher and reliable subscription;", "reliability",
                  rmw_qos_reliability_policy_to_str(pub->reliability),
                  rmw_qos_reliability_policy_to_str(sub->reliability), &findings);

    pub_weak = RMW_QOS_POLICY_DURABILITY_VOLATILE == pub->durability;
    sub_strong = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL == sub->durability;
    kind_findings(pub_weak && sub_strong, durability_unknown(pub->durability), durability_unknown(sub->durability),
                  pub_weak, sub_strong, "ERROR: Volatile publisher and transient local subscription;", "durability",
                  rmw_qos_durability_policy_to_str(pub->durability), rmw_qos_durability_policy_to_str(sub->durability),
                  &findings);

    duration_findings(pub->deadline, sub->deadline, (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE, "deadline", &findings);

    pub_weak = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC == pub->liveliness;
    sub_strong = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == sub->liveliness;
    kind_findings(pub_weak && sub_strong, liveliness_unknown(pub->liveliness), liveliness_unknown(sub->liveliness),
                  pub_weak, sub_strong, "ERROR: Automatic liveliness publisher and manual by topic subscription;",
                  "liveliness", rmw_qos_liveliness_policy_to_str(pub->liveliness),
                  rmw_qos_liveliness_policy_to_str(sub->liveliness), &findings);
    duration_findings(pub->liveliness_lease_duration, sub->liveliness_lease_duration,
                      (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE, "liveliness lease duration",
                      &findings);

    if (findings.error) {
        *compatibility = RMW_QOS_COMPATIBILITY_ERROR;
    } else {
        *compatibility = findings.warning ? RMW_QOS_COMPATIBILITY_WARNING : RMW_QOS_COMPATIBILITY_OK;
    }
    qos_reason_append(&findings.errors, "%s", warning_text);
    return RMW_RET_OK;
}

uint64_t rmw_tickle_wire_lease_ns(rmw_time_t lease) {
    if (rmw_time_equal(lease, (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT) ||
        rmw_time_equal(lease, (rmw_time_t)RMW_DURATION_INFINITE)) {
        return 0;
    }
    rmw_duration_t lease_ns = rmw_time_total_nsec(lease);
    return lease_ns > 0 ? (uint64_t)lease_ns : 0;
}

void rmw_tickle_qos_resolve_request(const rmw_qos_profile_t* requested, const struct tt_QosProfile* actual,
                                    rmw_qos_profile_t* out) {
    rmw_qos_profile_t resolved;
    rmw_tickle_qos_from_tt(actual, requested, &resolved);
    *out = *requested;
    if (RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE == requested->reliability) {
        out->reliability = resolved.reliability;
    }
    if (RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE == requested->durability) {
        out->durability = resolved.durability;
    }
    if (RMW_QOS_POLICY_LIVELINESS_BEST_AVAILABLE == requested->liveliness) {
        out->liveliness = resolved.liveliness;
    }
    if (rmw_time_equal(requested->deadline, (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE)) {
        out->deadline = resolved.deadline;
    }
    if (rmw_time_equal(requested->liveliness_lease_duration,
                       (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE)) {
        out->liveliness_lease_duration = resolved.liveliness_lease_duration;
    }
}
