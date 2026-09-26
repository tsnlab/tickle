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
#include <tickle/trace.h>

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
#include "rmw_tickle_c/publisher_payload.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

// QoS roadmap #2 (DEADLINE) - runs once per pub_impl->deadline_period_ns (rescheduled
// unconditionally every time, a steady period, matching DDS's own "one miss per elapsed period
// with no write" semantics), checking whether rmw_publish() updated last_activity_time since the
// last check. Fires from inside tt_Node_poll() - poll_thread already holds context_impl->
// the node lock around that whole call (rmw_tickle_context_impl_t's own doc comment) - so last_
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
// Node_poll() - poll_thread already holds the node lock (rmw_tickle_context_impl_t's
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
//   client.c, at COMPARISON.md §6 item 9/10's own already-measured ~12.2MB.
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

// Phase 3 step 3 - core calls this from inside tt_Node_poll() (so the node lock is
// already held, same as check_publisher_deadline() above) the moment a KEEP_ALL Publisher that had
// refused a write becomes writable again. tt_Publisher.writable_callback's own doc comment limits a
// callback to "signal and return" - it must not re-enter TickLE - which is exactly all this does:
// bump the generation rmw_publish()'s wait loop watches, and broadcast.
//
// Taking wait_mutex here while holding the node lock is the established producer order in this package
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
// Under-reserving costs throughput, not retention, and this comment claimed the opposite until
// 2026-09-25. It said a sample that does not fit is sent but not retained (cache_reliable_sample()'s
// oversize branch, tickle.c) - but that branch needs length > arena_size, the WHOLE arena, which one
// sample cannot reach when the arena is (depth + 1) records and depth is at least 2048 here. What
// really happens is that a larger sample is retained and takes the room of several, so the arena
// fills long before the count bound does. Until the byte-aware refusal landed that silently evicted
// an unacknowledged sample; now tt_Publisher_publish() refuses instead (tt_RET_WOULD_BLOCK,
// counted as publish_refused_bytes), which is what KEEP_ALL promises. So a number too small here
// means this Publisher blocks sooner than its depth suggests - at arena_size / real record size.
//
// Since DATA_FRAG (DATAFRAG_PLAN.md section 13) the clamp is the largest message rmw_tickle can send -
// tt_MAX_SAMPLE_LENGTH less its own psn header - and what one message costs is tt_sample_cache_bytes() of
// it, psn included: a message larger than a datagram is cached as one record per fragment.
static uint32_t clamp_record_bytes(unsigned long long payload) {
    const unsigned long long largest = (unsigned long long)tt_MAX_SAMPLE_LENGTH - RMW_TICKLE_PSN_BYTES;
    if (payload > largest) {
        payload = largest;
    }
    return tt_sample_cache_bytes((uint32_t)payload + RMW_TICKLE_PSN_BYTES);
}

// How much of the arena to allocate up front, out of the `limit` the budget worked out. Small
// enough that a publisher which never fills it costs little, large enough that an ordinary one
// never grows: 64 KiB holds 44 full-size datagrams, or hundreds of typical samples. Growth doubles
// from here, so reaching a 12 MiB limit takes eight reallocations at most.
//
// Never less than one of this type's largest messages (`largest_record`), within the limit: a message
// the arena cannot hold is sent without being retained, and KEEP_LAST only grows once `depth` messages
// have gone out. An unbounded type's largest message used to be one 64 KiB record and fit; with the psn
// header and one record per fragment it is about 67 KB and would not.
#define RMW_TICKLE_INITIAL_ARENA_BYTES (64U * 1024U)

static uint32_t initial_arena_bytes(uint32_t limit, uint32_t largest_record) {
    uint32_t wanted = largest_record > RMW_TICKLE_INITIAL_ARENA_BYTES ? largest_record : RMW_TICKLE_INITIAL_ARENA_BYTES;
    return limit < wanted ? limit : wanted;
}

// Doubles this publisher's arena toward its limit, keeping everything retained, and returns whether
// it grew. Called only from publish_blocking() below, on the caller's thread and never from core's
// publish path, which allocates nothing by design. The node mutex is held: tt_ReliableCache_grow()
// is not safe against a concurrent poll.
static bool grow_reliable_cache(rmw_tickle_publisher_t* pub_impl) {
    struct tt_ReliableCache* cache = pub_impl->reliable_cache;
    if (NULL == cache || NULL == cache->arena || cache->arena_size >= cache->arena_limit) {
        return false; // nothing retained, or already at the limit the budget set
    }
    unsigned long long doubled = (unsigned long long)cache->arena_size * 2ULL;
    uint32_t next = doubled < (unsigned long long)cache->arena_limit ? (uint32_t)doubled : cache->arena_limit;

    rcutils_allocator_t* allocator = &pub_impl->allocator;
    uint8_t* grown = (uint8_t*)allocator->allocate(next, allocator->state);
    if (NULL == grown) {
        return false; // out of memory is not this publisher's problem to report: it just blocks
    }
    uint8_t* previous = cache->arena;
    if (tt_RET_OK != tt_ReliableCache_grow(cache, grown, next)) {
        allocator->deallocate(grown, allocator->state);
        return false;
    }
    allocator->deallocate(previous, allocator->state); // core copied out of it and never kept it
    return true;
}

// Whether a KEEP_LAST publisher is retaining fewer samples than its depth promises because its
// arena is smaller than its budget allows - the growth signal for the policy that never blocks.
//
// publish_blocking()'s trigger cannot serve here: keep_all_writable() returns true immediately for
// KEEP_LAST ("never refuses a write"), so KEEP_LAST never sees tt_RET_WOULD_BLOCK and would sit at
// the initial slice forever, evicting by bytes long before its depth (Plan caught this in review -
// /rosout at depth 1000 would have retained about 290 logs where it retains 1000 today). The signal
// instead is retention itself: enough samples published for the count bound to be the one binding,
// and fewer than depth of them still held.
//
// Counted in messages (tt_ReliableCache.sample_depth and retained_samples), not seq_no: seq_no counts
// datagrams once messages fragment, and depth is a promise about messages.
static bool keep_last_wants_more_arena(const rmw_tickle_publisher_t* pub_impl) {
    const struct tt_ReliableCache* cache = pub_impl->reliable_cache;
    if (NULL == cache || pub_impl->tickle_publisher.keep_all || NULL == cache->arena ||
        cache->arena_size >= cache->arena_limit) {
        return false;
    }
    uint64_t published = pub_impl->next_publication_sequence_number - 1;
    if (0 == cache->sample_depth || published < cache->sample_depth) {
        return false; // not enough published yet for depth to be what limits retention
    }
    return cache->retained_samples < cache->sample_depth;
}

// What to call this publisher's type in a diagnostic before anything has been created.
static const char* type_name_of(const rmw_tickle_publisher_t* pub_impl) {
    return NULL != pub_impl->callbacks ? pub_impl->callbacks->ros_type_name : "?";
}

// The per-publisher storage sizing an application may pass in rmw_publisher_options_t.rmw_specific_
// publisher_payload (rmw_tickle_c/publisher_payload.h), or NULL when there is none to use. The field
// is a bare void* shared with every other rmw implementation, so a payload meant for one of those
// can arrive here: the magic is what tells them apart, and the size marker catches a payload built
// against a different version of the header. Either mismatch is a warning and a fall back to the
// environment, not a failure - the same "a malformed tuning knob must not stop a node starting"
// rule resolve_max_blocking_ns() follows, and a node that refuses to start is worse than one that
// ignores a knob it cannot read.
static const rmw_tickle_publisher_payload_t* publisher_payload(const rmw_tickle_publisher_t* pub_impl,
                                                               const char* type_name) {
    const void* raw = pub_impl->rmw_publisher.options.rmw_specific_publisher_payload;
    if (NULL == raw) {
        return NULL;
    }
    const rmw_tickle_publisher_payload_t* payload = (const rmw_tickle_publisher_payload_t*)raw;
    if (RMW_TICKLE_PUBLISHER_PAYLOAD_MAGIC != payload->magic) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                               "publisher of %s: rmw_specific_publisher_payload is not rmw_tickle's (magic 0x%08x, "
                               "expected 0x%08x) - ignored; was it meant for another rmw implementation?",
                               type_name, payload->magic, RMW_TICKLE_PUBLISHER_PAYLOAD_MAGIC);
        return NULL;
    }
    if (sizeof(*payload) != payload->struct_size) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                               "publisher of %s: rmw_specific_publisher_payload reads as %u bytes, this rmw_tickle's "
                               "is %zu - ignored; rebuild against this version of publisher_payload.h",
                               type_name, payload->struct_size, sizeof(*payload));
        return NULL;
    }
    return payload;
}

// What the cache reserves per retained sample, for either history policy: the application's own
// number for this publisher when it gave one, else the process-wide environment variable, else what
// the type can produce. Named for KEEP_ALL until 2026-09-25, when the payload made it KEEP_LAST's
// answer too.
static uint32_t keep_all_unbounded_default(void) {
    return (uint32_t)(tt_MAX_BUFFER_LENGTH < tt_ETHERNET_UDP_PAYLOAD ? tt_MAX_BUFFER_LENGTH : tt_ETHERNET_UDP_PAYLOAD);
}

// What one sample of this type can actually need: the generator's bound, or a whole datagram when
// it has none. Nothing reserves more than this, because nothing larger can arrive.
static unsigned long long type_ceiling_bytes(const rmw_tickle_publisher_t* pub_impl) {
    if (NULL != pub_impl->callbacks &&
        ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED != pub_impl->callbacks->tickle_max_encoded_size) {
        return (unsigned long long)pub_impl->callbacks->tickle_max_encoded_size;
    }
    return (unsigned long long)tt_MAX_BUFFER_LENGTH;
}

// The application's own per-publisher reservation (publisher_payload.h), clamped to the ceiling on
// the way up, or 0 when it gave none. Applies under either history policy: it is a statement about
// this publisher's samples, not about a policy.
static uint32_t payload_record_bytes(const rmw_tickle_publisher_t* pub_impl) {
    const rmw_tickle_publisher_payload_t* payload = publisher_payload(pub_impl, type_name_of(pub_impl));
    if (NULL == payload || 0 == payload->max_sample_bytes) {
        return 0;
    }
    unsigned long long asked = (unsigned long long)payload->max_sample_bytes;
    unsigned long long ceiling = type_ceiling_bytes(pub_impl);
    return clamp_record_bytes(asked < ceiling ? asked : ceiling);
}

static uint32_t resolve_record_bytes(const rmw_tickle_publisher_t* pub_impl) {
    uint32_t chosen = payload_record_bytes(pub_impl);
    if (0 != chosen) {
        return chosen;
    }
    if (NULL != pub_impl->callbacks &&
        ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED != pub_impl->callbacks->tickle_max_encoded_size) {
        return clamp_record_bytes((unsigned long long)pub_impl->callbacks->tickle_max_encoded_size);
    }

    const char* env = getenv("RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES");
    if (NULL == env || '\0' == env[0]) {
        // No bound from either source: the standard 1472-byte datagram, raw, without the record
        // conversion - what this code reserved before any of this existed, when that was also
        // tt_MAX_BUFFER_LENGTH. Deliberately not tt_MAX_BUFFER_LENGTH now that rmw_tickle builds
        // it at 65507 (2026-09-24): KEEP_ALL is not budgeted (it may not drop an unacknowledged
        // sample), so a depth-8192 DURABLE publisher of a type with a plain string would reserve
        // 536 MB. Every sample that could exist before 65507 is retained exactly as before; a
        // larger one - newly possible - fills the arena faster than the count bound, so this
        // Publisher blocks sooner (setup_reliable_cache() warns), and either this payload's
        // max_sample_bytes or RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES raises it.
        return keep_all_unbounded_default();
    }
    char* end = NULL;
    unsigned long long env_bytes = strtoull(env, &end, 10);
    if (end == env || (end != NULL && '\0' != *end) || env_bytes == 0) {
        // Unparseable or zero: fall back rather than fail publisher creation, same reasoning as
        // resolve_max_blocking_ns() - a malformed tuning knob should not stop a node starting, and
        // this one can only cost retention. To the same default as unset, not to something else.
        return keep_all_unbounded_default();
    }
    return clamp_record_bytes(env_bytes);
}

// Byte budget for a KEEP_LAST publisher's retained samples (RMW_TICKLE_CACHE_BYTES, default 1 MiB -
// read by rmw_tickle_cache_budget_bytes(), shared with a service's response cache -
// the storage design the user approved on 2026-09-24). The arena used to be (depth + 1) x
// tt_MAX_BUFFER_LENGTH whatever the type: 1.5 MB for /rosout's depth of 1000 at 1472, and 65 MB at
// the 65507 rmw_tickle is moving to. Now it is (depth + 1) records of the type's own bound, capped
// at the budget, and never below one record - so any single legal sample can still be retained.
//
// What the cap costs, stated where it is decided: core evicts oldest-first by bytes as well as by
// count (cache_reliable_sample(), tickle.c), so a publisher whose samples are large keeps fewer
// than `depth` of them - DDS's RESOURCE_LIMITS.max_samples by another name. That also narrows
// RELIABLE, not only a late joiner's history: a sample evicted by bytes before its NACK arrives is
// answered with an eviction Heartbeat, i.e. lost to that reader. With 64 KiB samples 1 MiB holds
// 16, fine at camera rates and tight at max rate (Plan's review).
//
// KEEP_ALL is budgeted too, but differently - see resolve_keep_all_arena_bytes() below: its promise is
// that nothing unacknowledged is ever dropped, so its budget is reached by blocking the writer, not
// by evicting.
// The arena bytes one message of this type can need under KEEP_LAST: its generated bound when it has one,
// the largest sample otherwise - and below either, the application's own max_sample_bytes when it gave
// one, which is the only thing the payload changes here. Deliberately not resolve_record_bytes(): that
// one falls back to KEEP_ALL's 1472-byte default for an unbounded type, because KEEP_ALL is not budgeted
// and would otherwise reserve hundreds of megabytes. KEEP_LAST is budgeted, so it can afford the largest
// sample per record and the cap in resolve_keep_last_arena_bytes() is what bounds it.
static uint32_t keep_last_record_bytes(const rmw_tickle_publisher_t* pub_impl) {
    uint32_t record = payload_record_bytes(pub_impl);
    return 0 != record ? record : clamp_record_bytes(type_ceiling_bytes(pub_impl));
}

static uint32_t resolve_keep_last_arena_bytes(const rmw_tickle_publisher_t* pub_impl, size_t depth) {
    unsigned long long record = keep_last_record_bytes(pub_impl);
    unsigned long long full = ((unsigned long long)depth + 1ULL) * record;

    const rmw_tickle_publisher_payload_t* payload = publisher_payload(pub_impl, type_name_of(pub_impl));
    unsigned long long budget = NULL != payload && 0 != payload->cache_bytes
                                    ? (unsigned long long)payload->cache_bytes
                                    : rmw_tickle_cache_budget_bytes(); // rmw_typesupport.c
    if (budget < record) {
        budget = record;
    }
    return (uint32_t)(full < budget ? full : budget);
}

// Byte budget for a KEEP_ALL publisher's unacknowledged samples (2026-09-25, the user's decision to
// change the defaults in TickLE's favour).
//
// It used to be unbudgeted: (depth + 1) records, for fear that a byte bound would drop what KEEP_ALL
// promises to keep. That fear was about EVICTION. Core has since learned to REFUSE instead - a KEEP_ALL
// publish that the arena's bytes cannot admit is refused with tt_RET_WOULD_BLOCK before anything is
// encoded, exactly as one past the count bound is (tt_Publisher_publish(), tickle.c) - so a byte
// budget now costs nothing KEEP_ALL promises. The writer waits sooner at large sample sizes; it never
// loses a sample.
//
// Why it matters: at KEEP_ALL with the ack window full, the arena's touched pages ARE the
// unacknowledged samples, so peak RSS is bytes-in-flight and lazy reservation cannot lower it - only
// fewer unacknowledged bytes can. Unbudgeted, a 2800-byte stream could hold the whole 1024-sample ack
// window, ~2.9 MB. CycloneDDS's own default bounds the same thing at 500 kB (its WhcHigh watermark);
// the default here, RMW_TICKLE_KEEP_ALL_BYTES = 512 KiB, is the same order. Small samples are
// untouched: at a 100-byte record 512 KiB is ~5,200 records, far past the count bound, which still
// binds first. Nothing about throughput should notice either - even 185 in-flight 2800-byte samples
// are orders of magnitude past this link's bandwidth-delay product - but that is a prediction for
// the rig to check, not a measurement.
//
// VOLATILE only. A TRANSIENT_LOCAL publisher's depth is literally how much history a late joiner is
// replayed (RMW_TICKLE_KEEP_ALL_DEPTH_DURABLE's comment above), so budgeting it by bytes would change
// what a reader receives, not just when a writer waits. That is a different decision and it is not
// made here.
//
// The per-publisher payload's cache_bytes overrides the environment, and the budget never drops below
// one record, so any single legal sample can still be admitted.
static uint32_t resolve_keep_all_arena_bytes(const rmw_tickle_publisher_t* pub_impl, size_t depth, bool durable) {
    unsigned long long record = (unsigned long long)resolve_record_bytes(pub_impl);
    unsigned long long full = ((unsigned long long)depth + 1ULL) * record;
    if (durable) {
        return (uint32_t)full;
    }
    const rmw_tickle_publisher_payload_t* payload = publisher_payload(pub_impl, type_name_of(pub_impl));
    unsigned long long budget = NULL != payload && 0 != payload->cache_bytes ? (unsigned long long)payload->cache_bytes
                                                                             : rmw_tickle_keep_all_budget_bytes();
    if (budget < record) {
        budget = record;
    }
    return (uint32_t)(full < budget ? full : budget);
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
// It does close the gap (Plan's sweep at d55d8ac5), but so does the piggybacked Heartbeat below at
// a small fraction of the wire cost, and that one is what ships on by default. This one stays off
// unless asked: what it adds over the piggyback is a Heartbeat from a publisher that has stopped
// sending, and lower mean latency at low rates (a loss is seen at the next tick rather than at the
// next sample), paid for with a datagram per period.
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

// Heartbeat piggybacked on every Nth sample of a RELIABLE publisher. On by default at N=64;
// RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY=0 turns it off, any other number replaces 64.
//
// The alternative to the periodic switch above, measured against it: a periodic Heartbeat costs a
// whole datagram per period whatever the data rate, while a piggybacked one rides a datagram being
// sent anyway and follows the data rate (tt_Publisher.heartbeat_piggyback_every, tickle.h). What
// it cannot cover is a publisher that has stopped - which is why the two are separate switches,
// so they can be compared and, if the measurement says so, combined.
//
// Default on is the user's decision (2026-09-24, translated: "turn it on by default"), taken on Plan's measurement at
// 9afacfe1..118507ed: at max rate N=64 took window jumps from 505-767 per run to 0-1, the same as
// a 1ms periodic Heartbeat, and it costs 0.37 B per sample (+0.035%) with no extra datagram, where
// the periodic one nearly doubles the datagram count at 1000/s. Periodic stays opt-in.
//
// Unset or empty means the default. A value that does not parse also falls back to the default,
// with a warning, rather than to off: the "a malformed tuning value must not stop a node starting"
// rule the other knobs here follow, applied to a switch whose normal state is on - a typo should
// not silently remove loss recovery that nobody asked to remove.
#define RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY_DEFAULT 64U

static uint32_t resolve_heartbeat_piggyback_every(void) {
    const char* env = getenv("RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY");
    if (NULL == env || '\0' == env[0]) {
        return RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY_DEFAULT;
    }
    char* end = NULL;
    unsigned long long every = strtoull(env, &end, 10);
    if (end == env || (end != NULL && '\0' != *end) || every > UINT32_MAX) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                               "RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY='%s' is not a valid sample count; using %u", env,
                               (unsigned)RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY_DEFAULT);
        return RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY_DEFAULT;
    }
    return (uint32_t)every;
}

// Split out of rmw_create_publisher() to keep its cognitive complexity under clang-tidy's
// threshold. A plain field, schedules nothing - but set after tt_Node_create_publisher(), which
// initialises it, and after setup_reliable_cache(), whose cache it checks.
//
// Only a publisher with a reliable_cache can piggyback (the Heartbeat names its oldest cached
// sample) - RELIABLE, or TRANSIENT_LOCAL - so a BEST_EFFORT VOLATILE one neither arms nor logs.
//
// Exact wording of both lines is load-bearing: the heartbeat sweep VOIDs a run whose log does not
// carry the armed line with the N it asked for, and verifies an off arm by the off line. Change
// them only together with that script.
static void arm_heartbeat_piggyback(rmw_tickle_publisher_t* pub_impl) {
    if (NULL == pub_impl->tickle_publisher.reliable_cache) {
        return;
    }
    uint32_t piggyback_every = resolve_heartbeat_piggyback_every();
    pub_impl->tickle_publisher.heartbeat_piggyback_every = piggyback_every;
    if (piggyback_every != 0) {
        RCUTILS_LOG_INFO_NAMED("rmw_tickle", "heartbeat piggyback armed: every %u samples", (unsigned)piggyback_every);
    } else {
        RCUTILS_LOG_INFO_NAMED("rmw_tickle", "heartbeat piggyback off");
    }
}

static int32_t encode_size_with_psn(struct tt_Data* data) {
    const rmw_tickle_outgoing_message_t* message = (const rmw_tickle_outgoing_message_t*)data;
    int32_t size = message->callbacks->tickle_encode_size((struct tt_Data*)message->tickle);
    return size < 0 ? size : size + RMW_TICKLE_PSN_BYTES;
}

static int32_t encode_with_psn(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    const rmw_tickle_outgoing_message_t* message = (const rmw_tickle_outgoing_message_t*)data;
    if (len < RMW_TICKLE_PSN_BYTES) {
        return -1;
    }
    memcpy(payload, &message->publication_sequence_number, RMW_TICKLE_PSN_BYTES);
    int32_t encoded = message->callbacks->tickle_encode((struct tt_Data*)message->tickle,
                                                        payload + RMW_TICKLE_PSN_BYTES, len - RMW_TICKLE_PSN_BYTES);
    return encoded < 0 ? encoded : encoded + RMW_TICKLE_PSN_BYTES;
}

// Core's cache counts seq_no, and every datagram of a fragmented message takes its own
// (DATAFRAG_PLAN.md section 13), so a KEEP_LAST ring of `depth` would hold depth / k messages of k
// fragments. Its ring is sized in datagrams instead - depth times the datagrams one reserved message
// takes (the record's bytes stand in for its CDR, which can overstate that by one datagram, never
// understate it) - and HISTORY.depth is bounded in messages separately (sample_depth, setup_reliable_cache()). The ring
// then only has to hold what that bound and the arena let in: per message its first record and a short
// last one, plus one full continuation per fragment payload the arena can hold. Without that cap an
// unbounded type - /rosout, depth 1000 - would index 47 datagrams a message it can never retain.
//
// KEEP_ALL's depth stays as it was, now counted in datagrams: it is a resource limit rather than a
// promise about messages, and it blocks rather than evicts. A VOLATILE one can never have more
// unacknowledged datagrams outstanding than a reader's tracking window, which counts seq_no too, so a
// larger ring would be memory nothing reaches. Capped by the ring's uint16 width either way.
static uint16_t resolve_ring_slots(const rmw_tickle_publisher_t* pub_impl, size_t depth, bool keep_all,
                                   uint32_t arena_bytes) {
    unsigned long long ring = (unsigned long long)depth;
    if (!keep_all) {
        const unsigned long long continuation =
            (unsigned long long)tt_CONTROL_MAX_LENGTH -
            (sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_FragContHeader));
        unsigned long long per_message =
            (unsigned long long)depth * tt_sample_datagrams(keep_last_record_bytes(pub_impl));
        unsigned long long most = (2ULL * depth) + (arena_bytes / continuation);
        ring = per_message < most ? per_message : most;
    }
    if (ring > (unsigned long long)UINT16_MAX) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                               "publisher of %s: a history depth of %zu messages needs %llu datagram slots, more than "
                               "the %u a cache can index - it keeps fewer messages of its largest size",
                               type_name_of(pub_impl), depth, ring, (unsigned)UINT16_MAX);
        ring = UINT16_MAX;
    }
    return (uint16_t)ring;
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

    // B1 (rmw_tickle/PLAN.md) - the encoded bytes live in a separate byte arena rather than a
    // 1472-byte buffer embedded in every index slot, sized at the type's own maximum. That maximum
    // is now a real per-type number for any type the generator can bound (callbacks->tickle_max_
    // encoded_size), which is what the ceiling below used to stand in for: a depth-8192 DURABLE
    // KEEP_ALL publisher of a 76-byte type reserved ~12.06 MB and needs ~0.6 MB. Types carrying a
    // plain unbounded string still have no bound and still get the ceiling. See
    // resolve_record_bytes() above for which source applies when and why.
    //
    // (depth + 1) records either way - the wrap slack B1 added, so the byte bound still cannot
    // evict before the count bound, which is what `depth` promises.
    uint32_t arena_bytes = keep_all ? resolve_keep_all_arena_bytes(pub_impl, depth, durable)
                                    : resolve_keep_last_arena_bytes(pub_impl, depth);
    // The ring counts datagrams (resolve_ring_slots() says how many and why).
    uint16_t ring = resolve_ring_slots(pub_impl, depth, keep_all, arena_bytes);

    pub_impl->reliable_cache =
        (struct tt_ReliableCache*)allocator->zero_allocate(1, sizeof(struct tt_ReliableCache), allocator->state);
    if (NULL == pub_impl->reliable_cache) {
        RMW_SET_ERROR_MSG("failed to allocate reliable_cache");
        return false;
    }
    pub_impl->reliable_cache->index = (struct tt_ReliableCacheIndex*)allocator->zero_allocate(
        (size_t)ring, sizeof(struct tt_ReliableCacheIndex), allocator->state);
    if (NULL == pub_impl->reliable_cache->index) {
        RMW_SET_ERROR_MSG("failed to allocate reliable_cache index");
        allocator->deallocate(pub_impl->reliable_cache, allocator->state);
        pub_impl->reliable_cache = NULL; // so a future caller-side cleanup path can't double-free it
        return false;
    }
    // Said at attach, where it is decidable, rather than discovered on the first publish that hits
    // it (Plan's request, 2026-09-25): a sample too large for the whole arena is sent without being
    // cached at all (B1, tickle.c), so a reader that misses it can never recover it. Whether that
    // can happen is a question about this cache and this type, and both are known here. The arena
    // may legitimately be smaller than (depth + 1) full-size records - that only costs retention -
    // so the test is against ONE such record, which is where retention stops working entirely.
    if ((unsigned long long)arena_bytes < (unsigned long long)clamp_record_bytes(type_ceiling_bytes(pub_impl))) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                               "publisher of %s: its retained-sample cache is %u bytes, but one sample of this type "
                               "can need %u - such a sample is sent without being retained, so a reader that misses "
                               "it cannot ask for it again. Raise the budget (the payload's cache_bytes, or "
                               "RMW_TICKLE_CACHE_BYTES) to at least that.",
                               type_name_of(pub_impl), (unsigned)arena_bytes,
                               (unsigned)clamp_record_bytes(type_ceiling_bytes(pub_impl)));
    }
    // Reserved lazily: the limit is what the budget above worked out, but only the first slice of
    // it is allocated now, and publish_blocking() grows toward the limit if the traffic ever asks
    // (tt_ReliableCache_grow(), tickle.c). A KEEP_ALL publisher of an unbounded type would
    // otherwise take 3 MB at creation whether it retains anything or not. The limit itself never
    // moves, so back-pressure still arrives exactly where it did.
    uint32_t initial_bytes = initial_arena_bytes(arena_bytes, clamp_record_bytes(type_ceiling_bytes(pub_impl)));
    pub_impl->reliable_cache->arena = (uint8_t*)allocator->allocate(initial_bytes, allocator->state);
    if (NULL == pub_impl->reliable_cache->arena) {
        RMW_SET_ERROR_MSG("failed to allocate reliable_cache arena");
        allocator->deallocate(pub_impl->reliable_cache->index, allocator->state);
        allocator->deallocate(pub_impl->reliable_cache, allocator->state);
        pub_impl->reliable_cache = NULL; // so a future caller-side cleanup path can't double-free it
        return false;
    }
    pub_impl->reliable_cache->arena_size = initial_bytes;
    pub_impl->reliable_cache->arena_limit = arena_bytes;
    pub_impl->reliable_cache->capacity = ring;
    pub_impl->reliable_cache->depth = ring;
    // The ring counts datagrams, so it would keep up to `ring` single-datagram messages: HISTORY.depth
    // is a bound in messages, which core enforces separately when told it (tt_ReliableCache.sample_depth).
    // KEEP_ALL has no such bound to enforce - its depth is a resource limit and it blocks, never evicts.
    if (!keep_all) {
        pub_impl->reliable_cache->sample_depth = (uint16_t)(depth < (size_t)UINT16_MAX ? depth : UINT16_MAX);
    }
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
    const rmw_tickle_publisher_payload_t* sizing = publisher_payload(pub_impl, type_name_of(pub_impl));
    bool application_chose_the_size = NULL != sizing && 0 != sizing->max_sample_bytes;
    if (keep_all && tt_MAX_BUFFER_LENGTH > tt_ETHERNET_UDP_PAYLOAD && !application_chose_the_size &&
        resolve_record_bytes(pub_impl) < (uint32_t)tt_MAX_BUFFER_LENGTH &&
        (NULL == pub_impl->callbacks ||
         ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED == pub_impl->callbacks->tickle_max_encoded_size)) {
        // resolve_record_bytes() says why: said out loud, because the cost is otherwise
        // invisible until this publisher starts blocking far below its depth.
        RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                               "KEEP_ALL publisher of %s: its type has no size bound, so only %u bytes are reserved "
                               "per sample; a larger one takes the room of several, so publishing blocks once the "
                               "cache is full rather than at depth %u. Set RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES to "
                               "the largest sample this publisher sends (it reserves that much per sample)",
                               NULL != pub_impl->callbacks ? pub_impl->callbacks->ros_type_name : "?",
                               (unsigned)resolve_record_bytes(pub_impl), (unsigned)depth);
    }
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
    if (NULL == callbacks || !rmw_tickle_check_callbacks_usable(callbacks)) {
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
    tt_Node_lock(&node_impl->context_impl->tickle_node);
    rmw_qos_profile_t resolved_qos = rmw_tickle_resolve_best_available(qos_profile, &node_impl->context_impl->discovery,
                                                                       topic_name, RMW_TICKLE_ENTITY_PUBLISHER);
    tt_Node_unlock(&node_impl->context_impl->tickle_node);
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
    pub_impl->topic.data_encode_size = encode_size_with_psn;
    pub_impl->topic.data_encode = encode_with_psn;
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
    pub_impl->next_publication_sequence_number = 1;
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
    tt_Node_lock(&node_impl->context_impl->tickle_node);
    tt_ret_t ret = tt_Node_create_publisher(&node_impl->context_impl->tickle_node, &pub_impl->tickle_publisher,
                                            &pub_impl->topic, pub_impl->rmw_publisher.topic_name);
    tt_Node_unlock(&node_impl->context_impl->tickle_node);
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
        tt_Node_lock(&node_impl->context_impl->tickle_node);
        tt_Publisher_destroy(&pub_impl->tickle_publisher);
        tt_Node_unlock(&node_impl->context_impl->tickle_node);
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
        tt_Node_lock(&node_impl->context_impl->tickle_node);
        // A failure here (tt_MAX_SCHEDULER_LENGTH exhausted) just leaves deadline monitoring
        // inactive for this Publisher - no logging facility in this package to report it through.
        (void)tt_Node_schedule(&node_impl->context_impl->tickle_node, tt_get_ns() + pub_impl->deadline_period_ns,
                               check_publisher_deadline, pub_impl);
        tt_Node_unlock(&node_impl->context_impl->tickle_node);
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
    // The lease goes on the wire for AUTOMATIC too (2026-09-26): until then only a MANUAL_BY_TOPIC
    // Publisher announced one, so a remote Subscription held an AUTOMATIC one to the core's node-level
    // limit whatever lease it asked for. A MANUAL one keeps announcing the lease it enforces locally.
    pub_impl->tickle_publisher.liveliness_lease_duration_ns =
        pub_impl->liveliness_lease_ns != 0 ? pub_impl->liveliness_lease_ns
                                           : rmw_tickle_wire_lease_ns(qos_profile->liveliness_lease_duration);
    pub_impl->tickle_publisher.liveliness_manual = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == qos_profile->liveliness;

    // Armed last, and under the node mutex: tt_Publisher_set_heartbeat_period() schedules on the
    // node and refuses a publisher with no reliable_cache, so it has to follow both
    // tt_Node_create_publisher() and setup_reliable_cache(). Configuration set before the object it
    // configures is fully built is exactly how the reorder buffer shipped disconnected (9747c1ea).
    arm_heartbeat_piggyback(pub_impl);

    uint64_t heartbeat_ns = resolve_heartbeat_period_ns();
    if (heartbeat_ns != 0 && pub_impl->tickle_publisher.reliable_cache != NULL) {
        tt_Node_lock(&node_impl->context_impl->tickle_node);
        tt_ret_t armed = tt_Publisher_set_heartbeat_period(&pub_impl->tickle_publisher, heartbeat_ns);
        tt_Node_unlock(&node_impl->context_impl->tickle_node);
        if (armed != tt_RET_OK) {
            // Not fatal - the publisher works without it, exactly as it always has - but said, so a
            // run that asked for heartbeats and did not get them cannot be read as if it had.
            RCUTILS_LOG_WARN_NAMED("rmw_tickle", "RMW_TICKLE_HEARTBEAT_PERIOD_NS=%llu requested but not armed (%d)",
                                   (unsigned long long)heartbeat_ns, (int)armed);
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

    tt_Node_lock(&pub_impl->node->context_impl->tickle_node);
    // QoS roadmap #2 (DEADLINE) - cancel a still-armed check before the publisher it closes over
    // is freed below; a no-op if deadline_period_ns was never set (tt_Node_unschedule() just finds
    // nothing matching).
    if (pub_impl->deadline_period_ns != 0) {
        tt_Node_unschedule(&pub_impl->node->context_impl->tickle_node, check_publisher_deadline, pub_impl);
    }
    tt_Publisher_destroy(&pub_impl->tickle_publisher);
    tt_Node_unlock(&pub_impl->node->context_impl->tickle_node);

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

// Phase 3 step 3 - one write attempt under the node lock, split out of publish_blocking() below so
// that function stays a plain loop. Returns tt_Publisher_publish()'s own code.
//
// Unusual contract, and the reason it's worth reading before publish_blocking(): on
// tt_RET_WOULD_BLOCK this returns with context_impl->wait_mutex STILL HELD, and *generation set to
// the writable_generation observed under it. That is not tidiness lost - it is the whole
// correctness argument. The refusal is cleared by an ACKNACK that poll_thread processes under
// the node lock, so the node lock cannot be held while waiting (the wait could only end via work that
// can't start until the wait ends). But merely dropping it opens a window where the wakeup lands
// between the refusal and the sleep and is lost, stalling a publisher for the full timeout instead
// of microseconds. Taking wait_mutex *before* releasing the node lock closes it: the producer
// (publisher_writable_callback()) runs under the node lock and then takes wait_mutex to bump the
// generation and broadcast, so it is either already counted in *generation, or still blocked on
// wait_mutex until pthread_cond_timedwait() releases it. The node lock -> wait_mutex is also the
// package-wide producer order (rmw_tickle_context_impl_t's own doc comment), so this cannot
// deadlock against it; the reverse order is what would.
static tt_ret_t publish_attempt(rmw_tickle_publisher_t* pub_impl, void* tickle_buf, uint64_t* generation) {
    rmw_tickle_context_impl_t* context_impl = pub_impl->node->context_impl;

    tt_Node_lock(&context_impl->tickle_node);
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
    tt_Node_unlock(&context_impl->tickle_node);
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
// with pub_impl->publish_mutex held (it reads the shared scratch buffer) and neither the node lock nor
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
        // Before waiting: this publisher may have been allocated only the first slice of the budget
        // it is entitled to (setup_reliable_cache()), and this is the moment that proves it needs
        // more. Growing is bounded by the limit the budget fixed, so a KEEP_ALL publisher still
        // blocks - just at the size it was always allowed, rather than at the size it happened to
        // have been given. Retry immediately if it grew: the wait below is for an acknowledgement,
        // which is not what was missing.
        pthread_mutex_unlock(&pub_impl->node->context_impl->wait_mutex);
        tt_Node_lock(&pub_impl->node->context_impl->tickle_node);
        bool grew = grow_reliable_cache(pub_impl);
        tt_Node_unlock(&pub_impl->node->context_impl->tickle_node);
        if (grew) {
            continue;
        }
        pthread_mutex_lock(&pub_impl->node->context_impl->wait_mutex);

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
    TT_TRACE(tt_TRACE_PUBLISH);
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
    rmw_tickle_outgoing_message_t outgoing = {pub_impl->next_publication_sequence_number, callbacks, tickle_buf};

    if (!callbacks->to_tickle(ros_message, tickle_buf)) {
        // A bounds-check failure (a variable array/bounded string longer than TickLE's resolved
        // capacity) - see ros2_adapter.py's own emit_to_tickle() doc comment.
        RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
            "cannot publish this %s: a sequence or string in it is longer than the capacity TickLE generated the "
            "type with - raise it with a capacity file (TICKLE_CAPACITIES_PATH) and rebuild the package",
            callbacks->ros_type_name);
        pthread_mutex_unlock(&pub_impl->publish_mutex);
        return RMW_RET_ERROR;
    }

    rmw_ret_t ret = publish_blocking(pub_impl, &outgoing);
    if (RMW_RET_OK == ret) {
        pub_impl->next_publication_sequence_number++;
    }

    // The KEEP_LAST half of lazy reservation (keep_last_wants_more_arena() above). Two field reads
    // on the ordinary path; the lock and the allocation happen only when retention has actually
    // fallen short, which for a given publisher can happen at most a handful of times - the arena
    // doubles and the limit does not move.
    if (RMW_RET_OK == ret && keep_last_wants_more_arena(pub_impl)) {
        tt_Node_lock(&pub_impl->node->context_impl->tickle_node);
        (void)grow_reliable_cache(pub_impl);
        tt_Node_unlock(&pub_impl->node->context_impl->tickle_node);
    }
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
            tt_Node_lock(&pub_impl->node->context_impl->tickle_node);
            // A failure here just leaves this monitoring inactive for this Publisher - same
            // reasoning as check_publisher_deadline()'s own scheduling failure handling.
            (void)tt_Node_schedule(&pub_impl->node->context_impl->tickle_node,
                                   tt_get_ns() + RMW_TICKLE_QOS_INCOMPATIBLE_CHECK_PERIOD_NS,
                                   check_publisher_qos_incompatible, pub_impl);
            tt_Node_unlock(&pub_impl->node->context_impl->tickle_node);
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
        // ...and to the remote side too: a HEARTBEAT with the liveliness flag, which is what keeps this
        // Publisher alive at the Subscriptions that watch it (LIVELINESS_PLAN.md amendment 4). The core
        // sends nothing within a third of the lease of the last publish or assertion.
        (void)tt_Publisher_assert_liveliness(&pub_impl->tickle_publisher);
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
// already hold the node lock: tt_Publisher_is_acked_by_all_peers() reads the same
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
// guard condition), and holding wait_mutex across a nested node-lock acquisition
// here would invert the lock order every other broadcast site depends on (poll_thread's own
// callbacks, e.g. check_publisher_deadline() above, already take the node lock first, wait_mutex
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
        tt_Node_lock(&context_impl->tickle_node);
        // pub_impl->tickle_publisher.seq_no - the most recently published sample as of *this*
        // check, not re-read on every loop iteration below: a concurrent rmw_publish() growing it
        // mid-wait must not move this call's own target (matches real DDS - this only ever waits
        // for what was already written when called).
        uint32_t target_seq_no = pub_impl->tickle_publisher.seq_no;
        bool acked = target_seq_no == 0 || all_peers_acked_locked(&pub_impl->tickle_publisher, target_seq_no);
        if (!acked) {
            tt_Publisher_request_ack(&pub_impl->tickle_publisher);
        }
        tt_Node_unlock(&context_impl->tickle_node);

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
