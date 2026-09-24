/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 3: rmw_create_publisher()/rmw_destroy_publisher()/rmw_publish().
// rmw_tickle_validate_qos_profile() (rmw_qos.c, Milestone 7) is what rejects anything this rmw
// doesn't support; rmw_create_publisher() below acts on the two policies that need more than a
// yes/no - RELIABILITY (QoS roadmap #5) and DURABILITY (QoS roadmap #4) - by wiring a shared
// struct tt_ReliableCache into the Publisher (one cache backs both, Milestone 24).

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h> // getenv()/strtoull() - resolve_max_blocking_ns()
#include <string.h>
#include <time.h> // clock_gettime()/struct timespec/nanosleep() - rmw_publisher_wait_for_all_acked()

#include <tickle/config.h> // tt_MAX_RELIABLE_HISTORY, tt_MAX_PEER_COUNT, tt_CALL_RETRY_INTERVAL, tt_SECOND
#include <tickle/hal.h>    // tt_ret_t/tt_RET_OK, tt_get_ns()
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/qos_policy_kind.h" // rmw_qos_policy_kind_t - check_publisher_qos_incompatible()
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/time.h" // rmw_time_total_nsec() - QoS roadmap #2 (DEADLINE)
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

// QoS roadmap #2 (DEADLINE) - runs once per pub_impl->deadline_period_ns (rescheduled
// unconditionally every time, a steady period, matching DDS's own "one miss per elapsed period
// with no write" semantics), checking whether rmw_publish() updated last_activity_time since the
// last check. Fires from inside tt_Node_poll() - poll_thread already holds context_impl->
// node_mutex around that whole call (rmw_tickle_context_impl_t's own doc comment) - so last_
// activity_time is safe to read here without a separate lock, same as rmw_publish() writing it
// under that same lock.
static void check_publisher_deadline(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)param;
    if (time - pub_impl->last_activity_time >= pub_impl->deadline_period_ns) {
        atomic_fetch_add(&pub_impl->deadline_missed.total_count, 1);
        atomic_fetch_add(&pub_impl->deadline_missed.unread_count, 1);
        // Wake anyone blocked in rmw_wait() on this event becoming ready - same wait_mutex/
        // wait_cond subscriber_callback() (rmw_subscription.c) already broadcasts on for a newly
        // queued message, for the identical reason (rmw_tickle_context_impl_t's own doc comment,
        // rmw_tickle.h).
        rmw_tickle_context_impl_t* context_impl = pub_impl->node->context_impl;
        pthread_mutex_lock(&context_impl->wait_mutex);
        pthread_cond_broadcast(&context_impl->wait_cond);
        pthread_mutex_unlock(&context_impl->wait_mutex);
    }
    // Deadline monitoring simply stops here on a reschedule failure (tt_MAX_SCHEDULER_LENGTH
    // exhausted) - no logging facility in this package to report it through, and no return path
    // out of a scheduled void callback anyway.
    (void)tt_Node_schedule(&pub_impl->node->context_impl->tickle_node, time + pub_impl->deadline_period_ns,
                           check_publisher_deadline, pub_impl);
}

// Milestone 31/28(a) observability follow-on - how often check_publisher_qos_incompatible() below
// re-scans the discovery table. No QoS-provided duration applies here (unlike DEADLINE/LIVELINESS,
// this isn't itself a QoS policy with its own configurable period) - tt_NODE_UPDATE_INTERVAL is
// the natural choice, the same cadence a newly (in)compatible remote Subscriber's own discovery
// announce would actually refresh at, so checking faster could never see a genuinely newer answer.
#define RMW_TICKLE_QOS_INCOMPATIBLE_CHECK_PERIOD_NS tt_NODE_UPDATE_INTERVAL

// Milestone 31/28(a) observability follow-on - RMW_EVENT_OFFERED_QOS_INCOMPATIBLE's own periodic
// check: no wire-level trigger exists for this (Milestone 31's own Publisher-side gate just
// silently skips upsert_peer()/deliver_durability_backlog()/send_initial_heartbeat() for an
// incompatible remote Subscriber, tickle.c), so this instead periodically re-derives a live count
// from the existing discovery table - the same delta-tracking pattern check_subscription_
// liveliness() (rmw_subscription.c) already established for RMW_EVENT_LIVELINESS_CHANGED, an
// analogous "no wire trigger, just periodically re-scan discovery" event. Fires from inside tt_
// Node_poll() - poll_thread already holds context_impl->node_mutex (rmw_tickle_context_impl_t's
// own doc comment) - so this calls the *_locked() variant directly, never the public rmw_tickle_
// count_incompatible_subscribers_locked() name's own "_locked" caller-already-holds-it contract
// implies otherwise.
static void check_publisher_qos_incompatible(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)param;
    rmw_qos_policy_kind_t last_kind = RMW_QOS_POLICY_INVALID;
    size_t current = rmw_tickle_count_incompatible_subscribers_locked(
        pub_impl->node->context_impl, pub_impl->rmw_publisher.topic_name, pub_impl->tickle_publisher.reliable,
        pub_impl->tickle_publisher.durable, pub_impl->tickle_publisher.liveliness_manual,
        pub_impl->tickle_publisher.deadline_duration_ns, pub_impl->tickle_publisher.liveliness_lease_duration_ns,
        &last_kind);
    rmw_tickle_qos_incompatible_status_t* status = &pub_impl->offered_qos_incompatible;
    if ((int)current > status->last_incompatible_count) {
        int delta = (int)current - status->last_incompatible_count;
        atomic_fetch_add(&status->base.total_count, delta);
        atomic_fetch_add(&status->base.unread_count, delta);
        status->last_policy_kind = last_kind;
        // Wake anyone blocked in rmw_wait() on this event becoming ready - same wait_mutex/
        // wait_cond check_publisher_deadline() above already broadcasts on, identical reasoning.
        rmw_tickle_context_impl_t* context_impl = pub_impl->node->context_impl;
        pthread_mutex_lock(&context_impl->wait_mutex);
        pthread_cond_broadcast(&context_impl->wait_cond);
        pthread_mutex_unlock(&context_impl->wait_mutex);
    }
    // current can also fall back to 0 (the remote Subscriber departed, or a QoS change made it
    // compatible again) - matching real DDS's own total_count being cumulative regardless, this
    // deliberately never *decrements* total_count/unread_count, only ever tracks the high-water
    // mark of newly-appeared incompatible matches, the same way check_subscription_liveliness()'s
    // own not_alive.total_count only counts departures, never "un-counts" a return.
    status->last_incompatible_count = (int)current;

    // Monitoring simply stops here on a reschedule failure - same reasoning as check_publisher_
    // deadline()'s own identical pattern above.
    (void)tt_Node_schedule(&pub_impl->node->context_impl->tickle_node,
                           time + RMW_TICKLE_QOS_INCOMPATIBLE_CHECK_PERIOD_NS, check_publisher_qos_incompatible,
                           pub_impl);
}

// DDS QoS policy coverage inventory (rmw_tickle/PLAN.md, 2026-09-21) gap 2 - KEEP_ALL conceptually
// asks for "retain everything," which a fixed-capacity cache can't literally do. A large-but-still-
// bounded cache, not literally unbounded, is the honest approximation the user chose over rejecting
// KEEP_ALL outright for Publishers.
//
// Phase 3 step 3 - "how large" turns out to depend on which policy is actually consuming the depth,
// so the single 8192 this used to be is now two constants:
//
//   DURABLE: a TRANSIENT_LOCAL Publisher replays its whole retained range to each late joiner
//   (deliver_durability_backlog(), tickle.c walks oldest_seq_no..newest_seq_no), so here depth is
//   literally "how much history a late joiner gets" and KEEP_ALL's "retain everything" cashes out
//   as a real, observable difference for every extra slot. This keeps the original 8192 - the same
//   order of magnitude as MAX_RELIABLE_DEPTH in examples/perf_hil/tickle/reliable_throughput/
//   client.c, at COMPARISON.MD §6 item 9/10's own already-measured ~12.2MB.
//
//   VOLATILE: depth past the ack window is memory that can never change behavior. KEEP_ALL's
//   back-pressure blocks at keep_all_bound() = min(cache depth, smallest announced tracking
//   window) (tickle.c), and rmw announces RMW_TICKLE_TRACKING_WORDS * 64 = 1024 samples, so a
//   reliable-but-volatile Publisher can never accumulate more than 1024 unacknowledged samples no
//   matter how deep its cache is. Retransmits are bounded by the same window. 8192 slots therefore
//   bought 7168 slots of arena (~10.5MB) that nothing could ever reach.
//
// These two and RMW_TICKLE_TRACKING_WORDS (rmw_tickle.h) are coupled in one direction: raising the
// tracking window above _VOLATILE's 2x headroom makes this the binding limit instead of the window,
// which silently lowers the blocking bound - so raise both together. Lowering the window is safe,
// it just leaves more unreachable headroom here.
#define RMW_TICKLE_KEEP_ALL_DEPTH_DURABLE 8192
#define RMW_TICKLE_KEEP_ALL_DEPTH_VOLATILE (2 * RMW_TICKLE_TRACKING_WORDS * tt_RELIABLE_BITMAP_WORD_BITS)

// Phase 3 step 3 - core calls this from inside tt_Node_poll() (so context_impl->node_mutex is
// already held, same as check_publisher_deadline() above) the moment a KEEP_ALL Publisher that had
// refused a write becomes writable again. tt_Publisher.writable_callback's own doc comment limits a
// callback to "signal and return" - it must not re-enter TickLE - which is exactly all this does:
// bump the generation rmw_publish()'s wait loop watches, and broadcast.
//
// Taking wait_mutex here while holding node_mutex is the established producer order in this package
// (rmw_tickle_context_impl_t's own doc comment: update entity-local state under the fine-grained
// lock, *then* take wait_mutex just to broadcast). rmw_publish()'s waiter below is written to match
// it, which is what keeps the two from deadlocking - see its own comment.
static void publisher_writable_callback(struct tt_Publisher* pub, void* param) {
    (void)pub;
    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)param;
    rmw_tickle_context_impl_t* context_impl = pub_impl->node->context_impl;
    pthread_mutex_lock(&context_impl->wait_mutex);
    pub_impl->writable_generation++;
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);
}

// How long rmw_publish() may block when a KEEP_ALL Publisher refuses a write, in nanoseconds.
// RMW_TICKLE_MAX_BLOCKING_MS overrides RMW_TICKLE_MAX_BLOCKING_MS_DEFAULT; "0" is meaningful (never
// block, fail immediately with RMW_RET_TIMEOUT), which is why this can't use a 0-as-unset sentinel
// and checks the string itself instead. Anything unparseable or out of range falls back to the
// default rather than failing publisher creation: a malformed tuning knob shouldn't stop a node
// from starting, and the value only ever costs latency, never correctness.
static uint64_t resolve_max_blocking_ns(void) {
    const char* env = getenv("RMW_TICKLE_MAX_BLOCKING_MS");
    if (NULL == env || '\0' == env[0]) {
        return (uint64_t)RMW_TICKLE_MAX_BLOCKING_MS_DEFAULT * (uint64_t)tt_MILLISECOND;
    }
    char* end = NULL;
    unsigned long long blocking_ms = strtoull(env, &end, 10);
    if (end == env || (end != NULL && '\0' != *end) || blocking_ms > RMW_TICKLE_MAX_BLOCKING_MS_LIMIT) {
        return (uint64_t)RMW_TICKLE_MAX_BLOCKING_MS_DEFAULT * (uint64_t)tt_MILLISECOND;
    }
    return (uint64_t)blocking_ms * (uint64_t)tt_MILLISECOND;
}

// Bytes to reserve per retained sample in a KEEP_ALL publisher's arena.
//
// Why this is a knob rather than a computed number: rmw cannot derive a type's maximum encoded
// size today. rosidl_typesupport_tickle_c exposes only tickle_encode_size(), which needs an actual
// message, so the bound has to be TickLE's own single-datagram ceiling. Generating a per-type
// maximum was investigated and deferred (rmw_tickle/PLAN.md): it is computable for a type whose
// every variable-length field resolves to a capacity, but not for one carrying a plain unbounded
// string - which includes sensor_msgs/msg/Image and std_srvs/srv/SetBool - because such a string
// is a char* aliasing external memory with no capacity at all, and the generator deliberately does
// not auto-derive one.
//
// An application does know its own types, though, and the difference is large: a KEEP_ALL DURABLE
// publisher reserves depth 8192 x 1472 B, about 12.06 MB, where a 76-byte telemetry type needs
// about 0.6 MB. So RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES lets a caller who knows say so, and
// nothing changes for anyone who doesn't.
//
// Setting it too small is safe by construction rather than by checking: a sample that does not fit
// the arena is sent but not retained (cache_reliable_sample()'s own oversize branch, tickle.c -
// it logs, counts not_cached_oversize, and leaves the rest of the cache untouched). So an
// under-sized value costs retention for the samples that overflow it, not correctness and not a
// crash. Clamped to tt_MAX_BUFFER_LENGTH because a larger value could only reserve for a sample
// TickLE cannot put in a datagram in the first place.
//
// Only consulted for KEEP_ALL, which is the only path that picks a depth large enough for the
// per-sample figure to matter: a KEEP_LAST publisher's arena is qos->depth samples, typically ten.
// That is also what keeps the name honest.
// Returns the per-record arena reservation for a KEEP_ALL publisher.
//
// Two sources, and which one applies is decided by the type rather than by precedence:
//
//   1. The generated maximum (callbacks->tickle_max_encoded_size). Exact, derived from the type's
//      own fields by layout.max_encoded_size() (tools/typesupport), and available for any type
//      whose every variable-length field resolves to a capacity. When it exists it is used, and
//      the environment variable below is not consulted - there is nothing a human can know about a
//      bounded type that beats a computed bound on it, and honouring a smaller hand-set value
//      would silently cost retention in the one case we had the right answer for.
//
//   2. RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES, for types the generator cannot bound: one carrying a
//      plain unbounded string, a string-element array, or a nested type containing either -
//      sensor_msgs/msg/Image and std_srvs/srv/SetBool among them. Here the application's knowledge
//      of its own data is the only information that exists, so it wins by default rather than by
//      preference.
//
// So the variable fills the gap the generator leaves; it does not override it. That is worth being
// precise about, because the alternative reading - "explicit setting always wins" - sounds more
// respectful of the caller and is worse: it lets a stale or mistaken value undercut a provable
// bound, on exactly the types where nobody needed to guess.
//
// The variable is a message *payload*, not a record: a cached record also carries a submessage
// header and a tt_DataHeader and is padded to the 4-byte submessage alignment, which
// tt_RELIABLE_RECORD_BYTES() adds. The generated number is a payload too, for the same reason.
// Getting that backwards under-reserves by 24 bytes a sample, at exactly the sizes this exists to
// serve.
//
// Everything is clamped to tt_MAX_BUFFER_LENGTH. A value past it - a wrong generated number, a
// typo in the environment - could only reserve for a sample TickLE cannot put in a datagram, and
// the clamp means neither can ever make the arena larger than it was before any of this existed.
//
// Under-reserving is safe by construction rather than by validation: a sample that does not fit is
// still sent, just not retained (cache_reliable_sample()'s oversize branch, tickle.c, which logs
// and counts not_cached_oversize). So a wrong number here costs retention, never correctness.
static uint32_t clamp_record_bytes(unsigned long long payload) {
    if (payload > (unsigned long long)tt_MAX_BUFFER_LENGTH) {
        return (uint32_t)tt_MAX_BUFFER_LENGTH;
    }
    uint32_t record = tt_RELIABLE_RECORD_BYTES(payload);
    return record > (uint32_t)tt_MAX_BUFFER_LENGTH ? (uint32_t)tt_MAX_BUFFER_LENGTH : record;
}

static uint32_t resolve_keep_all_record_bytes(const rmw_tickle_publisher_t* pub_impl) {
    if (NULL != pub_impl->callbacks &&
        ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED != pub_impl->callbacks->tickle_max_encoded_size) {
        return clamp_record_bytes((unsigned long long)pub_impl->callbacks->tickle_max_encoded_size);
    }

    const char* env = getenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES");
    if (NULL == env || '\0' == env[0]) {
        // No bound from either source. tt_MAX_BUFFER_LENGTH raw, without the record conversion,
        // because that is literally what this code passed before any of this existed - "unchanged"
        // has to mean byte-identical, not merely similar.
        return (uint32_t)tt_MAX_BUFFER_LENGTH;
    }
    char* end = NULL;
    unsigned long long payload = strtoull(env, &end, 10);
    if (end == env || (end != NULL && '\0' != *end) || payload == 0) {
        // Unparseable or zero: fall back rather than fail publisher creation, same reasoning as
        // resolve_max_blocking_ns() - a malformed tuning knob should not stop a node starting, and
        // this one can only cost retention.
        return (uint32_t)tt_MAX_BUFFER_LENGTH;
    }
    return clamp_record_bytes(payload);
}

// Periodic Heartbeat for a RELIABLE publisher, off unless RMW_TICKLE_HEARTBEAT_PERIOD_NS is set.
//
// An EXPERIMENT switch, not yet a tuned default, and it exists to answer one question. A reader
// learns that a sample is gone - evicted from a KEEP_LAST cache - from the eviction Heartbeat the
// publisher sends back in answer to an ACKNACK. That is a single lossy exchange: if the ACKNACK or
// its reply is dropped, the reader falls back to acknack_retry (tt_RELIABLE_RETRY_INTERVAL x
// tt_RELIABLE_RETRY), and at a high rate a tracking window can fill before that retry wins -
// forcing a window-sized jump rather than a clean skip. RTPS backs the exchange up with Heartbeats
// the writer sends on its own, carrying firstSN; TickLE has the same thing in send_heartbeat(),
// carrying first_available_seq_no, and rmw_tickle has never switched it on.
//
// Whether it closes the gap is what the switch measures. Off by default so every existing
// measurement stays reproducible and the wire carries nothing new unless asked; the right default
// period, and whether to piggyback on DATA instead, are decisions for after that measurement.
// Zero or unparseable leaves it off, the same "a malformed tuning value must not stop a node
// starting" rule the other knobs in this file follow.
static uint64_t resolve_heartbeat_period_ns(void) {
    const char* env = getenv("RMW_TICKLE_HEARTBEAT_PERIOD_NS");
    if (NULL == env || '\0' == env[0]) {
        return 0;
    }
    char* end = NULL;
    unsigned long long period = strtoull(env, &end, 10);
    if (end == env || (end != NULL && '\0' != *end)) {
        return 0;
    }
    return (uint64_t)period;
}

// Split out of rmw_create_publisher() below purely to keep that function's own cognitive
// complexity under clang-tidy's threshold - see rmw_tickle_publisher_t.reliable_cache's own doc
// comment for the full "why" this exists at all. Returns false (with RMW_SET_ERROR_MSG already
// called) only on a real failure; true covers both "successfully set up" and "neither RELIABLE
// nor TRANSIENT_LOCAL was requested, nothing to do" - the caller doesn't need to tell those two
// apart, only whether to bail out and run its own (unrelated - tt_Node_create_publisher() et al.)
// cleanup.
static bool setup_reliable_cache(rmw_tickle_publisher_t* pub_impl, const rmw_qos_profile_t* qos_profile,
                                 rcutils_allocator_t* allocator) {
    if (RMW_QOS_POLICY_RELIABILITY_RELIABLE != qos_profile->reliability &&
        RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL != qos_profile->durability) {
        return true;
    }

    // DDS QoS policy coverage inventory gap 2 (rmw_tickle/PLAN.md) - rmw_tickle_validate_qos_
    // profile() (rmw_qos.c) only rejects HISTORY_KEEP_ALL for Subscriptions; a Publisher requesting
    // it used to pass validation and then silently fall through to the ordinary ->depth branch
    // below (defaulting to tt_MAX_RELIABLE_HISTORY=64 if ->depth was also unset) - a real "reject
    // explicitly, never silently downgrade" violation (this file's own design philosophy, rmw_
    // qos.c's header comment). Honored here instead of rejected, at the user's own explicit choice.
    bool keep_all = RMW_QOS_POLICY_HISTORY_KEEP_ALL == qos_profile->history;
    bool durable = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL == qos_profile->durability;
    size_t depth;
    if (keep_all) {
        depth = durable ? (size_t)RMW_TICKLE_KEEP_ALL_DEPTH_DURABLE : (size_t)RMW_TICKLE_KEEP_ALL_DEPTH_VOLATILE;
    } else {
        depth = qos_profile->depth != RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT ? qos_profile->depth
                                                                          : (size_t)tt_MAX_RELIABLE_HISTORY;
    }
    if (depth == 0 || depth > (size_t)UINT16_MAX) {
        RMW_SET_ERROR_MSG("rmw_tickle's RELIABLE/TRANSIENT_LOCAL publisher depth must be 1.."
                          "UINT16_MAX (struct tt_ReliableCache.depth/capacity's own uint16_t "
                          "width) - see rmw_tickle/PLAN.md's QoS roadmap #4/#5");
        return false;
    }

    pub_impl->reliable_cache =
        (struct tt_ReliableCache*)allocator->zero_allocate(1, sizeof(struct tt_ReliableCache), allocator->state);
    if (NULL == pub_impl->reliable_cache) {
        RMW_SET_ERROR_MSG("failed to allocate reliable_cache");
        return false;
    }
    pub_impl->reliable_cache->index = (struct tt_ReliableCacheIndex*)allocator->zero_allocate(
        depth, sizeof(struct tt_ReliableCacheIndex), allocator->state);
    if (NULL == pub_impl->reliable_cache->index) {
        RMW_SET_ERROR_MSG("failed to allocate reliable_cache index");
        allocator->deallocate(pub_impl->reliable_cache, allocator->state);
        pub_impl->reliable_cache = NULL; // so a future caller-side cleanup path can't double-free it
        return false;
    }
    // B1 (rmw_tickle/PLAN.md) - the encoded bytes live in a separate byte arena rather than a
    // 1472-byte buffer embedded in every index slot, sized at the type's own maximum. That maximum
    // is now a real per-type number for any type the generator can bound (callbacks->tickle_max_
    // encoded_size), which is what the ceiling below used to stand in for: a depth-8192 DURABLE
    // KEEP_ALL publisher of a 76-byte type reserved ~12.06 MB and needs ~0.6 MB. Types carrying a
    // plain unbounded string still have no bound and still get the ceiling. See
    // resolve_keep_all_record_bytes() above for which source applies when and why.
    //
    // (depth + 1) records either way - the wrap slack B1 added, so the byte bound still cannot
    // evict before the count bound, which is what `depth` promises.
    uint32_t record_bytes = keep_all ? resolve_keep_all_record_bytes(pub_impl) : (uint32_t)tt_MAX_BUFFER_LENGTH;
    uint32_t arena_bytes = tt_RELIABLE_CACHE_ARENA_BYTES(depth, record_bytes);
    pub_impl->reliable_cache->arena = (uint8_t*)allocator->allocate(arena_bytes, allocator->state);
    if (NULL == pub_impl->reliable_cache->arena) {
        RMW_SET_ERROR_MSG("failed to allocate reliable_cache arena");
        allocator->deallocate(pub_impl->reliable_cache->index, allocator->state);
        allocator->deallocate(pub_impl->reliable_cache, allocator->state);
        pub_impl->reliable_cache = NULL; // so a future caller-side cleanup path can't double-free it
        return false;
    }
    pub_impl->reliable_cache->arena_size = arena_bytes;
    pub_impl->reliable_cache->capacity = (uint16_t)depth;
    pub_impl->reliable_cache->depth = (uint16_t)depth;
    pub_impl->tickle_publisher.reliable_cache = pub_impl->reliable_cache;
    pub_impl->tickle_publisher.reliable = RMW_QOS_POLICY_RELIABILITY_RELIABLE == qos_profile->reliability;
    pub_impl->tickle_publisher.durable = durable;

    // Phase 3 step 3 - the half of HISTORY.KEEP_ALL that sizing the cache above can't express: with
    // this set, core refuses a write (tt_RET_WOULD_BLOCK) rather than evicting a sample no matched
    // Subscriber has acknowledged yet, which is what makes "keep all" a promise instead of a larger
    // ring buffer. rmw_publish() below turns that refusal into a bounded wait. Only meaningful for a
    // RELIABLE Publisher - a Subscriber that never acknowledges can't hold anything back - so a
    // DURABLE-but-BEST_EFFORT KEEP_ALL Publisher keeps its deep cache and its non-blocking writes,
    // which is the only behavior it could have.
    pub_impl->tickle_publisher.keep_all = keep_all && pub_impl->tickle_publisher.reliable;
    if (pub_impl->tickle_publisher.keep_all) {
        pub_impl->tickle_publisher.writable_callback = publisher_writable_callback;
        pub_impl->tickle_publisher.writable_callback_param = pub_impl;
    }
    return true;
}

rmw_publisher_t* rmw_create_publisher(const rmw_node_t* node, const rosidl_message_type_support_t* type_support,
                                      const char* topic_name, const rmw_qos_profile_t* qos_profile,
                                      const rmw_publisher_options_t* publisher_options) {
    if (NULL == node) {
        RMW_SET_ERROR_MSG("node is null");
        return NULL;
    }
    if (NULL == topic_name) {
        RMW_SET_ERROR_MSG("topic_name is null");
        return NULL;
    }
    if (NULL == qos_profile) {
        RMW_SET_ERROR_MSG("qos_profile is null");
        return NULL;
    }
    if (NULL == publisher_options) {
        RMW_SET_ERROR_MSG("publisher_options is null");
        return NULL;
    }
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }
    if (rmw_tickle_validate_qos_profile(qos_profile, RMW_TICKLE_ENTITY_PUBLISHER) != RMW_RET_OK) {
        return NULL; // error message already set
    }

    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = rmw_tickle_get_message_callbacks(type_support);
    if (NULL == callbacks) {
        return NULL; // error message already set
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rcutils_allocator_t* allocator = &node_impl->allocator;

    // DDS QoS policy coverage inventory gap 1 (rmw_tickle/PLAN.md, 2026-09-21) - resolve any RMW_
    // QOS_POLICY_*_BEST_AVAILABLE value/sentinel in qos_profile into a concrete one before anything
    // below reads it (rmw_tickle_resolve_best_available()'s own doc comment, rmw_tickle.h, has the
    // full algorithm). Reassigning the qos_profile parameter itself (not shadowing with a new name)
    // keeps every downstream reference below already correct with no further edits needed - a
    // profile that requested nothing as BEST_AVAILABLE gets its own values back unchanged.
    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    rmw_qos_profile_t resolved_qos = rmw_tickle_resolve_best_available(qos_profile, &node_impl->context_impl->discovery,
                                                                       topic_name, RMW_TICKLE_ENTITY_PUBLISHER);
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    qos_profile = &resolved_qos;

    rmw_tickle_publisher_t* pub_impl =
        (rmw_tickle_publisher_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_publisher_t), allocator->state);
    if (NULL == pub_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_publisher_t");
        return NULL;
    }
    pub_impl->node = node_impl;
    // Milestone 34 - see rmw_tickle_publisher_t.owning_node_name's own doc comment. Failure here
    // isn't fatal to publisher creation - just leaves attribution unavailable for this publisher,
    // matching the "not consumed by anything yet" scope that field's own doc comment describes;
    // nothing downstream depends on these being non-NULL today.
    pub_impl->owning_node_name = rcutils_strdup(node_impl->rmw_node.name, *allocator);
    pub_impl->owning_node_namespace = rcutils_strdup(node_impl->rmw_node.namespace_, *allocator);
    pub_impl->type_support = type_support;
    pub_impl->callbacks = callbacks;
    pub_impl->allocator = *allocator;
    pub_impl->qos = *qos_profile;

    // topic.name is callbacks->ros_type_name - a generated-code string literal, so it already
    // satisfies tickle.h's "Lifetime / ownership" rule (stay valid and unmoved until tt_Publisher_
    // destroy()) without needing its own copy - see rmw_tickle.h's own rmw_tickle_publisher_t.topic
    // doc comment.
    pub_impl->topic.name = callbacks->ros_type_name;
    pub_impl->topic.data_size = (uint32_t)callbacks->tickle_struct_size;
    pub_impl->topic.data_encode_size = callbacks->tickle_encode_size;
    pub_impl->topic.data_encode = callbacks->tickle_encode;
    pub_impl->topic.data_decode = callbacks->tickle_decode;
    pub_impl->topic.data_free = callbacks->tickle_free;
    // data_encode_inplace/data_decode_inplace stay NULL (zero_allocate) - no zero-copy support.

    pub_impl->rmw_publisher.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    pub_impl->rmw_publisher.data = pub_impl;
    pub_impl->rmw_publisher.topic_name = rcutils_strdup(topic_name, *allocator);
    pub_impl->rmw_publisher.options = *publisher_options;
    pub_impl->rmw_publisher.can_loan_messages = false;
    // Phase 3 step 3 - resolved once here rather than per publish: getenv() on the hot path would
    // be both wasteful and a lie (the value can't change meaningfully mid-run anyway). Set for every
    // Publisher, not just KEEP_ALL ones, so publish_blocking()'s own diagnostics can quote it
    // without first having to ask which kind it is.
    pub_impl->max_blocking_ns = resolve_max_blocking_ns();
    if (NULL == pub_impl->rmw_publisher.topic_name) {
        RMW_SET_ERROR_MSG("failed to allocate topic_name");
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }

    // Milestone 45 - see rmw_tickle_publisher_t.publish_scratch_buf's own doc comment. Allocated
    // once here (callbacks->tickle_struct_size is fixed for this Publisher's whole lifetime),
    // reused by every rmw_publish() call from here on instead of a fresh allocate() each time.
    pub_impl->publish_scratch_buf = allocator->allocate(callbacks->tickle_struct_size, allocator->state);
    if (NULL == pub_impl->publish_scratch_buf) {
        RMW_SET_ERROR_MSG("failed to allocate publish scratch buffer");
        allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }
    if (pthread_mutex_init(&pub_impl->publish_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize publisher publish_mutex");
        allocator->deallocate(pub_impl->publish_scratch_buf, allocator->state);
        allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }

    // Same tt_Node_interrupt()-then-lock pattern rmw_destroy_node() already established - see
    // rmw_tickle.h's own rmw_tickle_context_impl_t doc comment for the full contract.
    tt_Node_interrupt(&node_impl->context_impl->tickle_node);
    pthread_mutex_lock(&node_impl->context_impl->node_mutex);
    tt_ret_t ret = tt_Node_create_publisher(&node_impl->context_impl->tickle_node, &pub_impl->tickle_publisher,
                                            &pub_impl->topic, pub_impl->rmw_publisher.topic_name);
    pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create_publisher() failed");
        allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }

    // QoS roadmap #5 (RELIABILITY) / #4 (DURABILITY) - see rmw_tickle_publisher_t.reliable_cache's
    // own doc comment. One shared struct tt_ReliableCache backs both policies now (PLAN.md's
    // Milestone 24 - matches real DDS/RTPS's own single Writer History Cache), allocated once if
    // either is requested; depth defaults to tt_MAX_RELIABLE_HISTORY when unset
    // (RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT). ROS 2's own rmw_qos_profile_t.depth is a single shared
    // field regardless - both policies always read the exact same requested depth, so merging this
    // into one allocation/one check changes no observable behavior for a Publisher requesting just
    // one of the two, and fixes a real asymmetry for one requesting both: a depth between the old,
    // smaller DURABILITY cap and the old, larger RELIABILITY cap used to reject the whole publisher
    // outright even though RELIABILITY alone would have accepted it.
    //
    // rmw_tickle/PLAN.md's own "DDS semantic-parity backlog" row 2 - index[]/capacity/arena are now
    // caller-owned (struct tt_ReliableCache's own doc comment, tickle.h), not a single build-wide
    // tt_MAX_RELIABLE_HISTORY=64 ceiling every rmw_tickle Publisher used to be capped by alike -
    // this caller (rmw_tickle, which already accepts dynamic allocation everywhere else, unlike
    // TickLE core's own embedded-facing examples) allocates them sized to *whatever* real
    // depth the ROS 2 caller actually requested, no artificial rejection past 64 anymore, matching
    // real rmw_fastrtps_cpp/rmw_cyclonedds_cpp's own dynamic-depth support instead of trailing it.
    // The only remaining rejection is the hard type-width limit struct tt_ReliableCache.depth/
    // capacity (uint16_t) itself imposes - not a business-logic choice, so still explicit-reject
    // rather than silently clamp, matching this package's own established design philosophy for
    // genuinely out-of-range requests. setup_reliable_cache() (above) does the actual work, split
    // out purely to keep this function's own cognitive complexity under clang-tidy's threshold.
    if (!setup_reliable_cache(pub_impl, qos_profile, allocator)) {
        tt_Node_interrupt(&node_impl->context_impl->tickle_node);
        pthread_mutex_lock(&node_impl->context_impl->node_mutex);
        tt_Publisher_destroy(&pub_impl->tickle_publisher);
        pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
        allocator->deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator->state);
        allocator->deallocate(pub_impl, allocator->state);
        return NULL;
    }

    // QoS roadmap #2 (DEADLINE) - see rmw_tickle_publisher_t.deadline_period_ns's own doc comment.
    // rmw_qos.c already accepted any finite qos.deadline; RMW_QOS_DEADLINE_DEFAULT ({0,0}) leaves
    // deadline_period_ns at its zero_allocate() default (0 = not requested, no cost).
    rmw_duration_t deadline_ns = rmw_time_total_nsec(qos_profile->deadline);
    if (deadline_ns > 0) {
        pub_impl->deadline_period_ns = (uint64_t)deadline_ns;
        pub_impl->last_activity_time = tt_get_ns();
        tt_Node_interrupt(&node_impl->context_impl->tickle_node);
        pthread_mutex_lock(&node_impl->context_impl->node_mutex);
        // A failure here (tt_MAX_SCHEDULER_LENGTH exhausted) just leaves deadline monitoring
        // inactive for this Publisher - no logging facility in this package to report it through.
        (void)tt_Node_schedule(&node_impl->context_impl->tickle_node, tt_get_ns() + pub_impl->deadline_period_ns,
                               check_publisher_deadline, pub_impl);
        pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
    }

    // QoS roadmap #6 (LIFESPAN) - see tt_Publisher.lifespan_duration_ns's own doc comment
    // (tickle.h). rmw_qos.c already accepted any finite qos.lifespan; RMW_QOS_LIFESPAN_DEFAULT
    // ({0,0}) leaves lifespan_duration_ns at its zero_allocate() default (0 = not requested, no
    // cost) - a plain field TickLE core itself reads on demand (deliver_durability_backlog()/
    // process_acknack()), no rmw_tickle-side state or scheduling needed, unlike DEADLINE above.
    rmw_duration_t lifespan_ns = rmw_time_total_nsec(qos_profile->lifespan);
    if (lifespan_ns > 0) {
        pub_impl->tickle_publisher.lifespan_duration_ns = (uint64_t)lifespan_ns;
    }

    // QoS roadmap #3 (LIVELINESS) follow-up, Milestone 32 - MANUAL_BY_TOPIC. See rmw_tickle_
    // publisher_t.liveliness_lease_ns's own doc comment. AUTOMATIC (or SYSTEM_DEFAULT) leaves
    // liveliness_lease_ns at its zero_allocate() default (0), the disambiguating sentinel check_
    // liveliness_lost() (rmw_node.c) uses to pick the AUTOMATIC (node-wide poll_thread health)
    // path over the manual-lease one. Left at RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT ({0,0})
    // falls back to the same floor rmw_qos.c's own acceptance check already established, rather
    // than leaving liveliness_lease_ns at 0 too (which would be indistinguishable from AUTOMATIC
    // here).
    if (RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == qos_profile->liveliness) {
        rmw_duration_t lease_ns = rmw_time_total_nsec(qos_profile->liveliness_lease_duration);
        pub_impl->liveliness_lease_ns =
            lease_ns > 0 ? (uint64_t)lease_ns : (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL;
        atomic_store(&pub_impl->last_asserted_ns, tt_get_ns());
    }

    // QoS roadmap #2 (DEADLINE) / #3 (LIVELINESS) RxO, Milestone 49 - what this Publisher
    // announces on the wire (tickle_publisher.deadline_duration_ns/liveliness_lease_duration_ns/
    // .liveliness_manual, tickle.h) so a remote Subscriber's own discovery-driven RxO check can
    // compare against it - see tt_UpdateEntity's own doc comment for the wire format. For the
    // Publisher side specifically, these are the *exact same values* deadline_period_ns/
    // liveliness_lease_ns above already resolved (the real, enforced values, not the raw request) -
    // copied straight across rather than recomputed, since tickle_publisher.liveliness_manual ==
    // (qos_profile->liveliness == MANUAL_BY_TOPIC) is exactly what pub_impl->liveliness_lease_ns's
    // own "!= 0" sentinel already distinguishes.
    pub_impl->tickle_publisher.deadline_duration_ns = pub_impl->deadline_period_ns;
    pub_impl->tickle_publisher.liveliness_lease_duration_ns = pub_impl->liveliness_lease_ns;
    pub_impl->tickle_publisher.liveliness_manual = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == qos_profile->liveliness;

    // Armed last, and under the node mutex: tt_Publisher_set_heartbeat_period() schedules on the
    // node and refuses a publisher with no reliable_cache, so it has to follow both
    // tt_Node_create_publisher() and setup_reliable_cache(). Configuration set before the object it
    // configures is fully built is exactly how the reorder buffer shipped disconnected (9747c1ea).
    uint64_t heartbeat_ns = resolve_heartbeat_period_ns();
    if (heartbeat_ns != 0 && pub_impl->tickle_publisher.reliable_cache != NULL) {
        tt_Node_interrupt(&node_impl->context_impl->tickle_node);
        pthread_mutex_lock(&node_impl->context_impl->node_mutex);
        tt_ret_t hb = tt_Publisher_set_heartbeat_period(&pub_impl->tickle_publisher, heartbeat_ns);
        pthread_mutex_unlock(&node_impl->context_impl->node_mutex);
        if (hb != tt_RET_OK) {
            // Not fatal - the publisher works without it, exactly as it always has - but said, so a
            // run that asked for heartbeats and did not get them cannot be read as if it had.
            RCUTILS_LOG_WARN_NAMED("rmw_tickle", "RMW_TICKLE_HEARTBEAT_PERIOD_NS=%llu requested but not armed (%d)",
                                   (unsigned long long)heartbeat_ns, (int)hb);
        } else {
            RCUTILS_LOG_INFO_NAMED("rmw_tickle", "periodic heartbeat armed: every %llu ns",
                                   (unsigned long long)heartbeat_ns);
        }
    }

    return &pub_impl->rmw_publisher;
}

rmw_ret_t rmw_destroy_publisher(rmw_node_t* node, rmw_publisher_t* publisher) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier) ||
        !rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;

    tt_Node_interrupt(&pub_impl->node->context_impl->tickle_node);
    pthread_mutex_lock(&pub_impl->node->context_impl->node_mutex);
    // QoS roadmap #2 (DEADLINE) - cancel a still-armed check before the publisher it closes over
    // is freed below; a no-op if deadline_period_ns was never set (tt_Node_unschedule() just finds
    // nothing matching).
    if (pub_impl->deadline_period_ns != 0) {
        tt_Node_unschedule(&pub_impl->node->context_impl->tickle_node, check_publisher_deadline, pub_impl);
    }
    tt_Publisher_destroy(&pub_impl->tickle_publisher);
    pthread_mutex_unlock(&pub_impl->node->context_impl->node_mutex);

    pthread_mutex_destroy(&pub_impl->publish_mutex); // Milestone 45 - see its own doc comment

    rcutils_allocator_t allocator = pub_impl->allocator;
    allocator.deallocate((char*)pub_impl->rmw_publisher.topic_name, allocator.state);
    // index[]/arena freed before the struct that (used to) point at them - reliable_cache is NULL
    // when neither RELIABLE nor TRANSIENT_LOCAL was requested, in which case both are also still
    // NULL (zero_allocate()'s own default, never touched) - every deallocate() call is then a
    // no-op, see their own doc comment.
    if (NULL != pub_impl->reliable_cache) {
        allocator.deallocate(pub_impl->reliable_cache->index, allocator.state);
        allocator.deallocate(pub_impl->reliable_cache->arena, allocator.state);
    }
    allocator.deallocate(pub_impl->reliable_cache, allocator.state);   // NULL is a no-op, see its own doc comment
    allocator.deallocate(pub_impl->owning_node_name, allocator.state); // NULL is a no-op too (a failed strdup)
    allocator.deallocate(pub_impl->owning_node_namespace, allocator.state);
    allocator.deallocate(pub_impl->publish_scratch_buf, allocator.state); // Milestone 45
    allocator.deallocate(pub_impl, allocator.state);
    return RMW_RET_OK;
}

// Phase 3 step 3 - the one place that reports "a KEEP_ALL publish gave up". Split out so the two
// call sites in publish_blocking() below word it identically.
//
// The message matters more than the code here, and not for the usual reasons. rclcpp turns any
// non-OK rmw return into a generic exception (publisher.hpp's own `if (RCL_RET_OK != status)
// throw_from_rcl_error(...)`), and only BAD_ALLOC/INVALID_ARGUMENT/INVALID_ROS_ARGS get their own
// exception types - RMW_RET_TIMEOUT lands in the catch-all rclcpp::exceptions::RCLError. Worse,
// throw_from_rcl_error() throws a bare std::runtime_error with *no* text at all if the rcutils error
// state happens to be unset. So for an application author this string is the entire diagnosis, and
// it says what was actually wrong (a Subscriber isn't keeping up), not just which call failed.
static rmw_ret_t publish_timed_out(const rmw_tickle_publisher_t* pub_impl, uint64_t waited_ns) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "rmw_tickle: HISTORY.KEEP_ALL publisher on topic '%s' blocked %ums waiting for subscriber "
        "acknowledgements and gave up - every retained sample is still unacknowledged, which means a "
        "matched subscriber is too slow or has stalled. Publish faster than a subscriber can take and "
        "KEEP_ALL has nothing left to do but wait. Raise RMW_TICKLE_MAX_BLOCKING_MS (currently %ums, 0 "
        "means never block), use HISTORY.KEEP_LAST to drop old samples instead, or fix the subscriber.",
        pub_impl->rmw_publisher.topic_name, (unsigned)(waited_ns / (uint64_t)tt_MILLISECOND),
        (unsigned)(pub_impl->max_blocking_ns / (uint64_t)tt_MILLISECOND));
    return RMW_RET_TIMEOUT;
}

// Phase 3 step 3 - one write attempt under node_mutex, split out of publish_blocking() below so
// that function stays a plain loop. Returns tt_Publisher_publish()'s own code.
//
// Unusual contract, and the reason it's worth reading before publish_blocking(): on
// tt_RET_WOULD_BLOCK this returns with context_impl->wait_mutex STILL HELD, and *generation set to
// the writable_generation observed under it. That is not tidiness lost - it is the whole
// correctness argument. The refusal is cleared by an ACKNACK that poll_thread processes under
// node_mutex, so node_mutex cannot be held while waiting (the wait could only end via work that
// can't start until the wait ends). But merely dropping it opens a window where the wakeup lands
// between the refusal and the sleep and is lost, stalling a publisher for the full timeout instead
// of microseconds. Taking wait_mutex *before* releasing node_mutex closes it: the producer
// (publisher_writable_callback()) runs under node_mutex and then takes wait_mutex to bump the
// generation and broadcast, so it is either already counted in *generation, or still blocked on
// wait_mutex until pthread_cond_timedwait() releases it. node_mutex -> wait_mutex is also the
// package-wide producer order (rmw_tickle_context_impl_t's own doc comment), so this cannot
// deadlock against it; the reverse order is what would.
static tt_ret_t publish_attempt(rmw_tickle_publisher_t* pub_impl, void* tickle_buf, uint64_t* generation) {
    rmw_tickle_context_impl_t* context_impl = pub_impl->node->context_impl;

    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_mutex_lock(&context_impl->node_mutex);
    tt_ret_t ret = tt_Publisher_publish(&pub_impl->tickle_publisher, (struct tt_Data*)tickle_buf);
    // QoS roadmap #2 (DEADLINE) - see rmw_tickle_publisher_t.last_activity_time's own doc comment.
    // Under the same lock check_publisher_deadline() reads it under, harmless to set even when
    // deadline_period_ns is 0 (unused in that case).
    if (tt_RET_OK == ret) {
        pub_impl->last_activity_time = tt_get_ns();
    }
    if (tt_RET_WOULD_BLOCK == ret) {
        pthread_mutex_lock(&context_impl->wait_mutex);
        *generation = pub_impl->writable_generation;
    }
    pthread_mutex_unlock(&context_impl->node_mutex);
    return ret;
}

// Sleeps until this Publisher is reported writable again or `deadline` passes, and returns true if
// it was woken rather than timed out. Called with wait_mutex held (see publish_attempt() above) and
// always returns having released it.
//
// Loops on the generation rather than trusting a single wake: pthread_cond_timedwait() may return
// spuriously, and wait_cond is broadcast for every waitable thing in this context (subscriber
// queues, guard conditions, events - rmw_tickle_context_impl_t's own doc comment), so the
// overwhelming majority of wakeups here belong to somebody else.
static bool wait_for_writable(rmw_tickle_publisher_t* pub_impl, uint64_t generation, const struct timespec* deadline) {
    rmw_tickle_context_impl_t* context_impl = pub_impl->node->context_impl;

    int wait_ret = 0;
    while (generation == pub_impl->writable_generation && 0 == wait_ret) {
        wait_ret = pthread_cond_timedwait(&context_impl->wait_cond, &context_impl->wait_mutex, deadline);
    }
    pthread_mutex_unlock(&context_impl->wait_mutex);

    // ETIMEDOUT is the expected failure. Any other (there is no legitimate one here - EINVAL would
    // mean the condvar or the deadline is malformed) is treated the same way, so a broken wait
    // degrades to "give up once more attempt has been made" rather than an unbounded spin.
    return 0 == wait_ret;
}

// Fills in `deadline` - CLOCK_REALTIME, not CLOCK_MONOTONIC. wait_cond is initialised with a NULL
// attr (rmw_init.c), so pthread_cond_timedwait() measures against CLOCK_REALTIME, and every other
// timed waiter in this package computes its deadline the same way. A monotonic clock would be the
// better default, but switching it has to change the condvar and all of those waiters at once to
// stay coherent - it is not something this path can do unilaterally.
static void writable_deadline(uint64_t max_blocking_ns, struct timespec* deadline) {
    clock_gettime(CLOCK_REALTIME, deadline); // NOLINT(misc-include-cleaner) - see rmw_wait_set.c's own comment
    deadline->tv_sec += (time_t)(max_blocking_ns / (uint64_t)tt_SECOND);
    deadline->tv_nsec += (long)(max_blocking_ns % (uint64_t)tt_SECOND);
    deadline->tv_sec += deadline->tv_nsec / (long)tt_SECOND;
    deadline->tv_nsec %= (long)tt_SECOND;
}

// Phase 3 step 3 - the publish attempt plus the bounded retry HISTORY.KEEP_ALL needs, split out of
// rmw_publish() below so that function keeps to argument checking and message conversion. Called
// with pub_impl->publish_mutex held (it reads the shared scratch buffer) and neither node_mutex nor
// wait_mutex held. Returns an rmw code directly, with RMW_SET_ERROR_MSG() already called on every
// failure path. See publish_attempt() for the locking, which is the subtle part.
static rmw_ret_t publish_blocking(rmw_tickle_publisher_t* pub_impl, void* tickle_buf) {
    bool deadline_set = false;
    bool expired = false;
    struct timespec deadline;

    while (true) {
        uint64_t generation = 0;
        tt_ret_t ret = publish_attempt(pub_impl, tickle_buf, &generation); // holds wait_mutex iff WOULD_BLOCK
        if (tt_RET_OK == ret) {
            return RMW_RET_OK;
        }
        if (tt_RET_WOULD_BLOCK != ret) {
            RMW_SET_ERROR_MSG("tt_Publisher_publish() failed");
            return RMW_RET_ERROR;
        }

        // Refused: every retained sample is still unacknowledged by some matched Subscriber. Note
        // that `expired` gives the deadline one attempt past its own expiry rather than reporting a
        // timeout straight out of the wait - a sample that became publishable in the same instant
        // should go out, not be reported as a failure.
        if (expired || 0 == pub_impl->max_blocking_ns) {
            pthread_mutex_unlock(&pub_impl->node->context_impl->wait_mutex);
            return publish_timed_out(pub_impl, pub_impl->max_blocking_ns);
        }
        if (!deadline_set) {
            writable_deadline(pub_impl->max_blocking_ns, &deadline);
            deadline_set = true;
        }
        expired = !wait_for_writable(pub_impl, generation, &deadline); // releases wait_mutex
    }
}

rmw_ret_t rmw_publish(const rmw_publisher_t* publisher, const void* ros_message,
                      rmw_publisher_allocation_t* allocation) {
    (void)allocation; // pre-allocated-message optimization, not implemented
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = pub_impl->callbacks;

    // Milestone 45 - publish_scratch_buf's own doc comment (rmw_tickle.h): reused every call
    // instead of a fresh allocate()/deallocate() pair. Safe to reuse the instant tt_Publisher_
    // publish() below returns - it never retains a pointer to `data` past its own call (either
    // encodes straight from it inline, or memcpy()s the *encoded wire bytes* into reliable_cache,
    // tickle.c's own cache_reliable_sample() - never `data` itself), and every field to_tickle()
    // writes here either aliases the caller's own ros_message (a plain unbounded string, DESIGN.md's
    // "Strings" rule) or copies into a fixed in-place buffer - nothing this struct itself owns that
    // a second call's own to_tickle() would need to free first.
    pthread_mutex_lock(&pub_impl->publish_mutex);
    void* tickle_buf = pub_impl->publish_scratch_buf;

    if (!callbacks->to_tickle(ros_message, tickle_buf)) {
        // A bounds-check failure (a variable array/bounded string longer than TickLE's resolved
        // capacity) - see ros2_adapter.py's own emit_to_tickle() doc comment.
        RMW_SET_ERROR_MSG("failed to convert ROS message to TickLE wire struct (capacity exceeded?)");
        pthread_mutex_unlock(&pub_impl->publish_mutex);
        return RMW_RET_ERROR;
    }

    rmw_ret_t ret = publish_blocking(pub_impl, tickle_buf);
    pthread_mutex_unlock(&pub_impl->publish_mutex);
    return ret;
}

// A real rclcpp::Publisher construction (rcl_publisher_init()) calls this unconditionally, not
// just optionally/best-effort like most of the other rmw_publisher_*() extras - discovered while
// provisioning rmw_tickle/PLAN.md's rmw-perf.yml benchmark rig (the first time this rmw was
// exercised via a real rclcpp C++ node rather than this package's own C-level unit tests).
// rmw_tickle never negotiates/downgrades a requested QoS policy, so "actual" is always just
// whatever rmw_create_publisher() already validated and stored.
rmw_ret_t rmw_publisher_get_actual_qos(const rmw_publisher_t* publisher, rmw_qos_profile_t* qos) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    *qos = pub_impl->qos;
    return RMW_RET_OK;
}

// Another call rcl_publisher_init() makes unconditionally (via rclcpp::Publisher's own
// constructor, to populate every future rmw_message_info_t.publisher_gid it hands out) - see
// rmw_publisher_get_actual_qos()'s own doc comment for how this was found. Milestone 47
// (rmw_tickle/PLAN.md) - (node id, endpoint id) alone is *not* a unique identity for this
// publisher: endpoint_id is a pure name hash, deliberately shared by any two Publishers with the
// same topic+endpoint name (tickle.h's own struct tt_Endpoint doc comment on .id) - the exact
// cross-instance identity gap that milestone root-caused and fixed at the wire level. entity_id
// (also on struct tt_Endpoint, assigned per-instance by TickLE core) is the real per-instance
// identity; using it here instead closes the identical latent gid-collision this rmw layer had -
// two co-existing Publisher instances of the same topic would otherwise report the same gid to
// rclcpp, which real DDS's own per-Writer GUID never does. Zero-extended into gid.data's remaining
// bytes, matching rmw_subscription.c's own zeroed rmw_message_info_t.publisher_gid for a received
// message from the *sending* side's point of view instead.
rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t* publisher, rmw_gid_t* gid) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    memset(gid, 0, sizeof(*gid));
    gid->implementation_identifier = RMW_TICKLE_IDENTIFIER;
    uint8_t node_id = pub_impl->node->context_impl->tickle_node.id;
    uint32_t entity_id = pub_impl->tickle_publisher.endpoint.entity_id;
    gid->data[0] = node_id;
    memcpy(&gid->data[1], &entity_id, sizeof(entity_id));
    return RMW_RET_OK;
}

// Milestone 51 (rmw_tickle/PLAN.md) - was previously entirely missing, same "unresolved dlsym"
// discovery as rmw_get_gid_for_client() (rmw_client.c's own doc comment on this same milestone).
// A plain identifier-checked memcmp() - rmw_get_gid_for_publisher()/rmw_get_gid_for_client() above
// already guarantee every real gid->data byte beyond the two populated ones is zeroed, so this
// needs no per-entity-kind knowledge of its own to compare correctly.
rmw_ret_t rmw_compare_gids_equal(const rmw_gid_t* gid1, const rmw_gid_t* gid2, bool* result) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid1, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid2, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(result, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(gid1->implementation_identifier) ||
        !rmw_tickle_identifier_matches(gid2->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    *result = memcmp(gid1->data, gid2->data, RMW_GID_STORAGE_SIZE) == 0;
    return RMW_RET_OK;
}

// A real rclcpp::Publisher constructor calls this once per QoS event type NodeOptions/QoS asks
// for (offered_deadline_missed, liveliness_lost, ...). QoS roadmap #2 (DEADLINE) and #3
// (LIVELINESS) are done - RMW_EVENT_OFFERED_DEADLINE_MISSED/RMW_EVENT_LIVELINESS_LOST are real,
// queryable events now (rmw_take_event(), rmw_event.c); everything else this rmw hasn't
// implemented still returns RMW_RET_UNSUPPORTED, this API's own documented way to say exactly
// that (rmw/event.h) - unlike most other not-yet-implemented rmw_*() extras, the *symbol* itself
// still has to exist (an unresolved dlsym is fatal to rclcpp here, a returned RMW_RET_UNSUPPORTED
// is not).
rmw_ret_t rmw_publisher_event_init(rmw_event_t* rmw_event, const rmw_publisher_t* publisher,
                                   rmw_event_type_t event_type) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(rmw_event, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    switch (event_type) {
    case RMW_EVENT_OFFERED_DEADLINE_MISSED:
    case RMW_EVENT_LIVELINESS_LOST:
        rmw_event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
        rmw_event->data = publisher->data;
        rmw_event->event_type = event_type;
        return RMW_RET_OK;
    case RMW_EVENT_OFFERED_QOS_INCOMPATIBLE: {
        rmw_event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
        rmw_event->data = publisher->data;
        rmw_event->event_type = event_type;
        // Lazy, idempotent start - see rmw_tickle_publisher_t.offered_qos_incompatible_monitoring_
        // started's own doc comment (rmw_tickle.h). Only the first rmw_publisher_event_init() call
        // for this event type actually arms the periodic check; a later one just rewires the same
        // rmw_event_t.
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
        if (!pub_impl->offered_qos_incompatible_monitoring_started) {
            pub_impl->offered_qos_incompatible_monitoring_started = true;
            tt_Node_interrupt(&pub_impl->node->context_impl->tickle_node);
            pthread_mutex_lock(&pub_impl->node->context_impl->node_mutex);
            // A failure here just leaves this monitoring inactive for this Publisher - same
            // reasoning as check_publisher_deadline()'s own scheduling failure handling.
            (void)tt_Node_schedule(&pub_impl->node->context_impl->tickle_node,
                                   tt_get_ns() + RMW_TICKLE_QOS_INCOMPATIBLE_CHECK_PERIOD_NS,
                                   check_publisher_qos_incompatible, pub_impl);
            pthread_mutex_unlock(&pub_impl->node->context_impl->node_mutex);
        }
        return RMW_RET_OK;
    }
    default:
        RMW_SET_ERROR_MSG("rmw_tickle does not support this publisher QoS event yet");
        return RMW_RET_UNSUPPORTED;
    }
}

// QoS roadmap #3 (LIVELINESS) follow-up, Milestone 32 - MANUAL_BY_TOPIC (the only manual kind
// this rmw's own rmw_qos_policy_liveliness_t still defines - see rmw_qos.c's own doc comment on
// MANUAL_BY_PARTICIPANT/_BY_NODE having been removed from the real rmw spec). A no-op (RMW_RET_OK)
// for an AUTOMATIC Publisher (liveliness_lease_ns == 0) - matches real DDS, where asserting on an
// AUTOMATIC entity is harmless. For a manual one, a plain atomic store into last_asserted_ns (no
// lock needed - see its own doc comment, rmw_tickle.h) is the entire mechanism: MANUAL_BY_TOPIC's
// own defining trait is that this Publisher's lease is refreshed only by its own explicit
// assertion, independent of every other entity on the node.
rmw_ret_t rmw_publisher_assert_liveliness(const rmw_publisher_t* publisher) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    if (pub_impl->liveliness_lease_ns != 0) {
        atomic_store(&pub_impl->last_asserted_ns, tt_get_ns());
    }
    return RMW_RET_OK;
}

// Loaned (zero-copy) messages: rmw_publisher_t.can_loan_messages is always false (tt_Node_create_
// publisher() never sets it true - no shared-memory/zero-copy transport exists), and unlike most
// other not-yet-implemented rmw_*() extras, these three symbols still have to actually exist -
// same reasoning as rmw_publisher_event_init() just above (an unresolved dlsym is fatal to
// rmw_implementation's own dispatch, a returned RMW_RET_UNSUPPORTED is not). Found via test_rmw_
// implementation's own TestPublisherUseLoan fixture, which calls all three expecting exactly this
// return before GTEST_SKIP()-ing the rest of its own loan-specific test cases - a missing symbol
// crashed that fixture's SetUp() outright (a NULL function pointer call) instead of failing a
// single assertion.
rmw_ret_t rmw_borrow_loaned_message(const rmw_publisher_t* publisher, const rosidl_message_type_support_t* type_support,
                                    void** ros_message) {
    (void)type_support;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    *ros_message = NULL;
    RMW_SET_ERROR_MSG("rmw_tickle does not support loaned messages");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_return_loaned_message_from_publisher(const rmw_publisher_t* publisher, void* loaned_message) {
    (void)loaned_message;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RMW_SET_ERROR_MSG("rmw_tickle does not support loaned messages");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_publish_loaned_message(const rmw_publisher_t* publisher, void* ros_message,
                                     rmw_publisher_allocation_t* allocation) {
    (void)ros_message;
    (void)allocation;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    RMW_SET_ERROR_MSG("rmw_tickle does not support loaned messages");
    return RMW_RET_UNSUPPORTED;
}

// How often rmw_publisher_wait_for_all_acked() below re-solicits (tt_Publisher_request_ack(),
// tickle.h) and re-checks the peers' ack state while waiting - tt_CALL_RETRY_INTERVAL (5ms), the
// cadence acknack_retry() (tickle.c) also used until Phase 1-b gave the core Subscriber its own,
// shorter tt_RELIABLE_RETRY_INTERVAL (rmw_tickle/PLAN.md). Deliberately left at 5ms here: this is
// an rmw-side wait loop, not loss recovery, and changing it is a separate, unmeasured decision.
#define RMW_TICKLE_WAIT_FOR_ACKED_POLL_INTERVAL_NS tt_CALL_RETRY_INTERVAL

// True once every currently-matched peer has acked at least up through target_seq_no. Phase 3
// prerequisite (c) (rmw_tickle/PLAN.md) moved that bookkeeping from an array index-aligned with
// pub->peers[] to a table keyed by node_id (struct tt_PeerAck, tickle.h), so this asks core rather
// than pairing the two arrays by index - which would now read the wrong peer's ack. Caller must
// already hold context_impl->node_mutex: tt_Publisher_is_acked_by_all_peers() reads the same
// fields process_acknack() (tickle.c) updates from inside tt_Node_poll(), under that same lock.
static bool all_peers_acked_locked(const struct tt_Publisher* pub, uint32_t target_seq_no) {
    return tt_Publisher_is_acked_by_all_peers(pub, target_seq_no);
}

// rmw_tickle/PLAN.md's remaining-rmw-API-surface backlog - previously missing as a symbol
// entirely (Milestone 15's own note), then an honest RMW_RET_UNSUPPORTED for RELIABLE (Milestone
// 33 - TickLE core's own ack bookkeeping ran the other way around: each reliable_sender-tracking
// Subscriber owned its own ack_seq_no/received_bitmap, tickle.h, but a Publisher only ever saw
// ACKNACKs reactively via process_acknack(), with no aggregated "which peers have fully caught
// up" view of its own). Now real: pub->peer_acks[] (tickle.h) is that aggregation, and tt_
// Publisher_request_ack() (tickle.h)'s own solicited, response-required Heartbeat (tt_HEARTBEAT_
// FLAG_FINAL clear - real RTPS's own wait_for_acknowledgments() mechanism) is what elicits an
// ACKNACK even from an already-healthy peer that would otherwise never send one at all (see that
// function's own doc comment). BEST_EFFORT has nothing to guarantee (matches the real spec: no
// delivery acknowledgment exists for BEST_EFFORT at all) - returns immediately, same as before.
//
// wait_timeout's own {0,0} means non-blocking poll-once (matches rcl_publisher_wait_for_all_
// acked()'s own explicit "timeout is 0 -> non-blocking" doc comment, rcl/publisher.h - the rmw
// layer's own rmw/rmw.h doesn't repeat that detail itself, but rmw_wait()'s own identical {0,0}
// convention, rmw_wait_set.c, is the same idea one layer down); any other value waits up to that
// duration, polling on RMW_TICKLE_WAIT_FOR_ACKED_POLL_INTERVAL_NS's own cadence rather than a
// condition-variable wait - deliberately not context_impl->wait_cond/wait_mutex: nothing broadcasts
// it on an ACKNACK arrival specifically (unlike a new subscriber-queue message or a triggered
// guard condition), and holding wait_mutex across a nested context_impl->node_mutex acquisition
// here would invert the lock order every other broadcast site depends on (poll_thread's own
// callbacks, e.g. check_publisher_deadline() above, already lock node_mutex first, wait_mutex
// second - see rmw_tickle_context_impl_t's own doc comment, rmw_tickle.h) - a bounded poll avoids
// that risk entirely, at the cost of up to one poll interval of extra latency once truly acked,
// same trade-off watchdog_thread_main() (rmw_node.c) already accepts for its own periodic checks.
rmw_ret_t rmw_publisher_wait_for_all_acked(const rmw_publisher_t* publisher, rmw_time_t wait_timeout) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(publisher->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)publisher->data;
    if (!pub_impl->tickle_publisher.reliable) {
        return RMW_RET_OK;
    }

    rmw_tickle_context_impl_t* context_impl = pub_impl->node->context_impl;

    bool poll_only = 0 == wait_timeout.sec && 0 == wait_timeout.nsec;
    struct timespec deadline;
    if (!poll_only) {
        clock_gettime(CLOCK_REALTIME, &deadline); // NOLINT(misc-include-cleaner) - see rmw_wait_set.c's own comment
        deadline.tv_sec += (time_t)wait_timeout.sec;
        deadline.tv_nsec += (long)wait_timeout.nsec;
        deadline.tv_sec += deadline.tv_nsec / (long)tt_SECOND;
        deadline.tv_nsec %= (long)tt_SECOND;
    }

    while (true) {
        tt_Node_interrupt(&context_impl->tickle_node);
        pthread_mutex_lock(&context_impl->node_mutex);
        // pub_impl->tickle_publisher.seq_no - the most recently published sample as of *this*
        // check, not re-read on every loop iteration below: a concurrent rmw_publish() growing it
        // mid-wait must not move this call's own target (matches real DDS - this only ever waits
        // for what was already written when called).
        uint32_t target_seq_no = pub_impl->tickle_publisher.seq_no;
        bool acked = target_seq_no == 0 || all_peers_acked_locked(&pub_impl->tickle_publisher, target_seq_no);
        if (!acked) {
            tt_Publisher_request_ack(&pub_impl->tickle_publisher);
        }
        pthread_mutex_unlock(&context_impl->node_mutex);

        if (acked) {
            return RMW_RET_OK;
        }
        if (poll_only) {
            return RMW_RET_TIMEOUT;
        }

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now); // NOLINT(misc-include-cleaner)
        if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            return RMW_RET_TIMEOUT;
        }

        struct timespec sleep_duration = {.tv_sec = 0, .tv_nsec = (long)RMW_TICKLE_WAIT_FOR_ACKED_POLL_INTERVAL_NS};
        nanosleep(&sleep_duration, NULL);
    }
}
