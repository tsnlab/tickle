/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// QoS roadmap #3 (LIVELINESS) follow-up - Milestone 30, implementing Milestone 28(b)'s own design
// sketch for RMW_EVENT_LIVELINESS_LOST: a watchdog thread (rmw_node.c) independent of poll_thread
// notices when poll_thread has stopped making progress and marks every live Publisher sharing that
// context LIVELINESS_LOST. Unlike test_events.c's own black-box event-plumbing smoke tests, proving
// this actually fires needs a real, genuine stall - not something reachable through the public rmw
// API alone - so this file reaches into rmw_tickle_c/rmw_tickle.h directly (a legitimate public
// header of this package, not a private implementation detail) to hold node_impl->context_impl->
// the node lock from this test's own thread for longer than the watchdog's own stale threshold, the
// same mutex poll_thread's own loop (rmw_node.c's poll_thread_main()) needs to reacquire before it
// can call tt_Context_poll() again - a real, if externally-induced, stall of poll_thread's own
// progress, not a forged timestamp standing in for one.

#include <assert.h>
#include <stdatomic.h> // atomic_load - poll_thread_last_return_ns, the stall's own premise below
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h> // struct timespec/nanosleep/time_t - the mutex-hold hang simulation below

#include <tickle/config.h> // tt_LIVELINESS_MISS_THRESHOLD, tt_CONTEXT_UPDATE_INTERVAL, tt_SECOND
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/event.h"
#include "rmw/events_statuses/liveliness_lost.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

struct fake_ros_msg {
    uint8_t value;
};
struct fake_tickle_msg {
    uint8_t value;
};

static bool fake_to_tickle(const void* ros_message, void* tickle_message) {
    ((struct fake_tickle_msg*)tickle_message)->value = ((const struct fake_ros_msg*)ros_message)->value;
    return true;
}
static bool fake_from_tickle(const void* tickle_message, void* ros_message) {
    ((struct fake_ros_msg*)ros_message)->value = ((const struct fake_tickle_msg*)tickle_message)->value;
    return true;
}
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint8_t);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    payload[0] = ((struct fake_tickle_msg*)data)->value;
    return (int32_t)sizeof(uint8_t);
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)len;
    (void)is_native_endian;
    ((struct fake_tickle_msg*)data)->value = payload[0];
    return (int32_t)sizeof(uint8_t);
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}

// Same fake type support shape as test_events.c's own - see its own doc comment on fake_type_
// support() for why this is hand-built rather than a real generated one.
static rosidl_typesupport_tickle_c_message_callbacks_t fake_callbacks = {
    .ros_type_name = "test_liveliness_lost_watchdog/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};

static rosidl_message_type_support_t fake_handle = {
    .data = &fake_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static const rosidl_message_type_support_t* fake_type_support(void) {
    if (NULL == fake_handle.typesupport_identifier) {
        fake_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    }
    return &fake_handle;
}

#define NS_PER_MS (1000L * 1000L)
#define SANITY_CHECK_TIMEOUT_MS 100

static rmw_qos_profile_t base_qos(void) {
    rmw_qos_profile_t qos;
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = 1;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT;
    qos.lifespan = (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT;
    qos.avoid_ros_namespace_conventions = false;
    return qos;
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_liveliness_lost_watchdog", "/");
    assert(NULL != node);

    rmw_wait_set_t* wait_set = rmw_create_wait_set(&context, 0);
    assert(NULL != wait_set);

    rmw_qos_profile_t pub_qos = base_qos();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, fake_type_support(), "watchdog_topic", &pub_qos, &pub_opts);
    assert(NULL != pub);

    rmw_event_t liveliness_lost_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&liveliness_lost_event, pub, RMW_EVENT_LIVELINESS_LOST));

    // Sanity check: a perfectly healthy node (nothing has stalled yet) must not report a spurious
    // loss - rmw_wait() times out on this short wait with nothing ready (RMW_RET_TIMEOUT, not
    // RMW_RET_OK - that's this call's own "nothing became ready before the deadline" result).
    void* events_storage[1] = {&liveliness_lost_event};
    rmw_events_t events = {.event_count = 1, .events = events_storage};
    rmw_time_t short_timeout = {0, (uint64_t)SANITY_CHECK_TIMEOUT_MS * NS_PER_MS};
    assert(RMW_RET_TIMEOUT == rmw_wait(NULL, NULL, NULL, NULL, &events, wait_set, &short_timeout));
    assert(NULL == events.events[0]); // rmw_wait() nulls out anything not ready

    // Induce a real stall: hold the node lock (rmw_tickle_c/rmw_tickle.h) from this thread for
    // longer than the watchdog's own stale threshold - poll_thread_main() (rmw_node.c) needs this
    // exact mutex before it can call tt_Context_poll() again, so holding it externally genuinely
    // blocks poll_thread's own progress, the same way an unrelated bug wedging *anything* that
    // holds this mutex too long would. tt_LIVELINESS_MISS_THRESHOLD * tt_CONTEXT_UPDATE_INTERVAL (3
    // real seconds today) is the exact floor rmw_node.c's own RMW_TICKLE_WATCHDOG_STALE_
    // THRESHOLD_NS uses (not exported as its own symbol - it's a translation-unit-local #define -
    // so this recomputes it from the same two public tickle/config.h constants that back it,
    // rather than hardcoding "3 seconds" and silently drifting from it if either ever changes).
    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;
    uint64_t stale_threshold_ns = (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_CONTEXT_UPDATE_INTERVAL;

    // Two things are asserted about each stall, and they are kept apart on purpose.
    //
    // 1. The watchdog wakes for the moment the stall crosses the threshold: watchdog_silence_wakes rises during
    //    the stall (counted when that check starts, before it blocks on the lock held here). Until 2026-10-17
    //    the watchdog only looked once a tick, and a stall that crossed the threshold and ended before the next tick
    //    was never reported: this test's old stall, the threshold plus one second from the lock, was 0.99 s past the
    //    threshold from the poll thread's last return (which comes 10 ms after the lock, when the executor lease of
    //    the rmw_wait() above runs out), and the tick ran 1.012 s quiet and longer under load. It failed in a
    //    check-gates run on 2026-10-16, and in 71 of 200 runs beside 16 CPU hogs on main 9d051337. This half asks
    //    why the watchdog woke, not how promptly: a wake up to a tick late still passes it, and a watchdog that only
    //    ticks fails it every time. (Reading the planned wake time instead failed when a tick fell 0.1 ms before the
    //    crossing: the plan equalled the crossing for only that long.)
    //
    // 2. The event arrives. Each stall runs a whole tick past the threshold, so this half leaves the watchdog's wake
    //    a tick of latency: a stall a quarter or half tick past the threshold asked that the wake come within that
    //    margin, and beside 32 CPU hogs it once came 604 ms late and failed. A watchdog that never fires fails here.
    //
    // Each stall is measured from the poll thread's own last return, which is where the watchdog's clock starts.
    const uint64_t stall_past_threshold_ns = (uint64_t)tt_CONTEXT_UPDATE_INTERVAL;
    // Patience for the event after the stall ends: the watchdog may have been blocked on this same mutex (inside
    // mark_automatic_publishers_lost(), rmw_node.c) waiting for the unlock, and rmw_wait() with a real timeout gives
    // it room to finish and broadcast wait_cond without racing a fixed short sleep. A bound on the test's patience,
    // not on the watchdog: a watchdog that never fires still fails here.
    const uint64_t recovery_ns = 4U * (uint64_t)tt_CONTEXT_UPDATE_INTERVAL;
    rmw_time_t recovery_timeout = {recovery_ns / tt_SECOND, recovery_ns % tt_SECOND};
    // How long the poll thread's last return must hold still before a stall counts from it (see the loop below), and
    // how often a stall looks at the clock.
    const struct timespec settle = {.tv_sec = 0, .tv_nsec = (long)(2U * RMW_TICKLE_EXECUTOR_POLL_LEASE_NS)};

    uint64_t previous_unlock_ns = 0;
    for (int32_t stall = 1; stall <= 2; stall++) {
        // Each stall starts from a return of its own. The event can be ready before the poll thread has returned
        // from the previous stall - the watchdog was queued on the lock ahead of it - and a stall locked then would
        // inherit the first one's start and be over before it began (seen once in 64 runs). The poll thread returns
        // at least once a tick; two ticks bound the wait.
        const uint64_t fresh_return_deadline_ns = tt_get_ns() + (2U * (uint64_t)tt_CONTEXT_UPDATE_INTERVAL);
        while (atomic_load(&context_impl->poll_thread_last_return_ns) <= previous_unlock_ns) {
            assert(tt_get_ns() < fresh_return_deadline_ns);
            nanosleep(&settle, NULL);
        }
        tt_Context_lock(&context_impl->tickle_context);
        // The stall starts at the poll thread's last return, and that is final once the thread has left its park
        // (poll_thread_parked false; leaving stores the return just after) and the return has held still for a
        // settle. A single fixed settle was not enough under load: the park was left up to 78 ms after the lock,
        // and the test then saw the return move mid-stall (6 of 200 runs beside 16 CPU hogs).
        uint64_t stalled_since_ns = atomic_load(&context_impl->poll_thread_last_return_ns);
        for (;;) {
            nanosleep(&settle, NULL);
            uint64_t settled_ns = atomic_load(&context_impl->poll_thread_last_return_ns);
            if (!atomic_load(&context_impl->poll_thread_parked) && settled_ns == stalled_since_ns) {
                break;
            }
            stalled_since_ns = settled_ns;
            assert(tt_get_ns() < fresh_return_deadline_ns);
        }
        const uint64_t silence_wakes_before = atomic_load(&context_impl->watchdog_silence_wakes);
        const uint64_t stall_end_ns = stalled_since_ns + stale_threshold_ns + stall_past_threshold_ns;
        while (tt_get_ns() < stall_end_ns) {
            nanosleep(&settle, NULL);
        }
        // The premise: the poll thread made no progress for the whole stall. If it returned meanwhile, the lock
        // did not stall it and nothing below would be about the watchdog.
        assert(atomic_load(&context_impl->poll_thread_last_return_ns) == stalled_since_ns);
        previous_unlock_ns = tt_get_ns();
        tt_Context_unlock(&context_impl->tickle_context);
        assert(atomic_load(&context_impl->watchdog_silence_wakes) > silence_wakes_before); // 1. above

        events.events[0] = &liveliness_lost_event; // 2. above
        assert(RMW_RET_OK == rmw_wait(NULL, NULL, NULL, NULL, &events, wait_set, &recovery_timeout));
        assert(NULL != events.events[0]);

        rmw_liveliness_lost_status_t lost_status;
        bool taken = false;
        assert(RMW_RET_OK == rmw_take_event(&liveliness_lost_event, &lost_status, &taken));
        assert(taken);
        assert(lost_status.total_count == stall); // once per stall: the second is a new loss, not a repeat
        assert(lost_status.total_count_change == 1);
    }

    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));

    // QoS roadmap #3 (LIVELINESS) follow-up, Milestone 32 - MANUAL_BY_TOPIC. Unlike the AUTOMATIC
    // case above, this doesn't need any the node lock trickery at all: check_manual_publishers_
    // lost() (rmw_node.c) checks each manual Publisher's own last_asserted_ns independently of
    // poll_thread's health, so a plain wait (no assertion at all) past the lease is a real,
    // unforced test of the actual obligation this QoS kind imposes.
    rmw_qos_profile_t manual_qos = base_qos();
    manual_qos.liveliness = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC;
    // rmw_qos.c's own floor - the shortest lease this rmw accepts, same reasoning as the AUTOMATIC
    // threshold above (recomputed, not hardcoded, for the same "don't silently drift" reason).
    uint64_t manual_lease_ns = (uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_CONTEXT_UPDATE_INTERVAL;
    manual_qos.liveliness_lease_duration.sec = manual_lease_ns / tt_SECOND;
    manual_qos.liveliness_lease_duration.nsec = manual_lease_ns % tt_SECOND;

    rmw_publisher_t* neglected_pub =
        rmw_create_publisher(node, fake_type_support(), "neglected_topic", &manual_qos, &pub_opts);
    assert(NULL != neglected_pub);
    rmw_event_t neglected_lost_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&neglected_lost_event, neglected_pub, RMW_EVENT_LIVELINESS_LOST));

    // Never asserted at all - must go LIVELINESS_LOST once its own lease elapses, entirely on its own schedule (no
    // external stall needed, unlike AUTOMATIC's own node-wide check above). The same two halves as the stalls above:
    // the watchdog wakes for the lease's end (watchdog_lease_wakes rises), and the event arrives within the lease
    // plus four ticks - patience, not a bound on the watchdog. The patience was
    // once the lease plus one second, which left 43 ms after the check the tick-only watchdog made at +3.96 s
    // (measured 2026-10-08), and 2 of 3 check-gates runs failed on it that day. A watchdog that never fires fails.
    const uint64_t lease_wakes_before = atomic_load(&context_impl->watchdog_lease_wakes);
    const uint64_t neglected_ns = manual_lease_ns + (4U * (uint64_t)tt_CONTEXT_UPDATE_INTERVAL);
    rmw_time_t neglected_timeout = {.sec = neglected_ns / tt_SECOND, .nsec = neglected_ns % tt_SECOND};
    void* neglected_events_storage[1] = {&neglected_lost_event};
    rmw_events_t neglected_events = {.event_count = 1, .events = neglected_events_storage};
    assert(RMW_RET_OK == rmw_wait(NULL, NULL, NULL, NULL, &neglected_events, wait_set, &neglected_timeout));
    assert(NULL != neglected_events.events[0]);
    assert(atomic_load(&context_impl->watchdog_lease_wakes) > lease_wakes_before);

    rmw_liveliness_lost_status_t neglected_status;
    bool neglected_taken = false;
    assert(RMW_RET_OK == rmw_take_event(&neglected_lost_event, &neglected_status, &neglected_taken));
    assert(neglected_taken);
    assert(neglected_status.total_count >= 1);

    assert(RMW_RET_OK == rmw_destroy_publisher(node, neglected_pub));

    // The mirror image: a Publisher that keeps calling rmw_publisher_assert_liveliness() faster
    // than its own lease must never go LIVELINESS_LOST, even well past when it would have without
    // those calls - proves the assertion API actually keeps a manual Publisher alive, not just
    // that a neglected one goes lost.
    rmw_publisher_t* asserted_pub =
        rmw_create_publisher(node, fake_type_support(), "asserted_topic", &manual_qos, &pub_opts);
    assert(NULL != asserted_pub);
    rmw_event_t asserted_lost_event = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&asserted_lost_event, asserted_pub, RMW_EVENT_LIVELINESS_LOST));

    struct timespec assert_interval = {.tv_sec = 1, .tv_nsec = 0}; // well under manual_lease_ns (>= 3s)
    for (int i = 0; i < (int)((manual_lease_ns + tt_SECOND) / tt_SECOND); i++) {
        assert(RMW_RET_OK == rmw_publisher_assert_liveliness(asserted_pub));
        nanosleep(&assert_interval, NULL);
    }

    void* asserted_events_storage[1] = {&asserted_lost_event};
    rmw_events_t asserted_events = {.event_count = 1, .events = asserted_events_storage};
    rmw_time_t asserted_check_timeout = {0, (uint64_t)SANITY_CHECK_TIMEOUT_MS * NS_PER_MS};
    assert(RMW_RET_TIMEOUT == rmw_wait(NULL, NULL, NULL, NULL, &asserted_events, wait_set, &asserted_check_timeout));
    assert(NULL == asserted_events.events[0]);

    assert(RMW_RET_OK == rmw_destroy_publisher(node, asserted_pub));

    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_liveliness_lost_watchdog: all tests passed\n");
    return 0;
}
