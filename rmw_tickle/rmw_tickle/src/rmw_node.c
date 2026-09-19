/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 2: rmw_create_node()/rmw_destroy_node(), a background thread per
// node driving tt_Node_poll(), and the per-node mutex every other entry point (rmw_publish() et
// al., Milestone 3+) must serialize against it with - see rmw_tickle.h's own rmw_tickle_node_t
// doc comment for the exact locking contract.

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h> // offsetof - mark_liveliness_lost()'s own tt_Publisher -> rmw_tickle_publisher_t recovery
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <tickle/config.h> // tt_RECEIVE_TIMEOUT, tt_LIVELINESS_MISS_THRESHOLD, tt_NODE_UPDATE_INTERVAL
#include <tickle/hal.h>    // tt_ret_t/tt_RET_OK, tt_get_ns()
#include <tickle/tickle.h>

#include "rcutils/allocator.h" // rcutils_allocator_t
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"      // rmw_context_t
#include "rmw/ret_types.h" // rmw_ret_t, RMW_RET_*
#include "rmw/rmw.h"
#include "rmw/types.h" // rmw_node_t
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"
#include "rmw_tickle_c/rmw_tickle.h"

// One process-wide _tt_CONFIG (include/tickle/config.h) means one tt_Node per process for now -
// rmw_tickle/PLAN.md's own "Deferred" note under the milestone table. Guards against a second
// rmw_create_node() call silently reusing/colliding with the first tt_Node rather than failing
// loudly. Process-wide rather than per-rmw_context_t: the constraint is about _tt_CONFIG, which
// doesn't belong to any one context either.
static atomic_bool g_tickle_node_created = false;

// tt_RECEIVE_TIMEOUT (config.h, 100us) is what tt_Node_poll() itself substitutes for any
// negative timeout - passed explicitly here (rather than -1, matching examples/*/*.c's own
// top-level poll loops) so this file doesn't depend on that substitution as an implicit,
// undocumented-at-the-call-site default. A macro (matching rmw_tickle.h's own RMW_TICKLE_*
// constants), not a `static const` variable - readability-identifier-naming's ConstantCase
// (lower_case) applies to the latter but not to a macro (MacroDefinitionCase: UPPER_CASE).
#define RMW_TICKLE_POLL_TIMEOUT_NS tt_RECEIVE_TIMEOUT

// rmw_tickle/PLAN.md's Milestone 6: "graph-changed guard condition wired to 0(c)'s callback".
// Runs on the poll thread, node->mutex already held (tt_Node_set_discovery()'s own contract - this
// fires from inside whichever tt_Node_poll() call just processed the UPDATE). node_id/endpoint_id/
// kind/departed aren't needed here - rmw's own contract is just "something in the graph changed,
// go re-query it", not "here's exactly what changed" (rmw_get_node_names() et al. are the
// re-query), so this simply triggers the guard condition unconditionally on every appear/refresh/
// depart - the same pattern rmw_trigger_guard_condition() (rmw_guard_condition.c) itself uses.
static void discovery_callback(struct tt_Node* node, uint8_t node_id, uint32_t endpoint_id, uint8_t kind, bool departed,
                               void* param) {
    (void)node;
    (void)node_id;
    (void)endpoint_id;
    (void)kind;
    (void)departed;
    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)param;
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)node_impl->context->impl;

    atomic_store(&context_impl->graph_guard_condition.has_triggered, true);
    pthread_mutex_lock(&context_impl->wait_mutex);
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);
}

// A deliberate, measured gap between one tt_Node_poll() cycle's unlock() and the next cycle's
// lock() - see poll_thread_main()'s own comment on why. 1us was enough to fully restore publish()
// throughput in that same benchmark (measured, not guessed) - negligible next to RMW_TICKLE_POLL_
// TIMEOUT_NS's own ~1ms-rounded-up cadence (config.h/hal_linux.c), so it doesn't meaningfully
// delay receive/flush responsiveness either, but real (nanosleep, not sched_yield()) - see below.
#define RMW_TICKLE_POLL_THREAD_YIELD_NS tt_MICROSECOND

// QoS roadmap #3 (LIVELINESS) follow-up - RMW_EVENT_LIVELINESS_LOST (Milestone 30, implementing
// Milestone 28(b)'s own design sketch). How stale poll_thread_last_return_ns may get before
// watchdog_thread_main() below calls it a hang - reuses the exact same floor rmw_qos.c's own
// liveliness_lease_duration acceptance check already established as "TickLE core's own fastest
// possible peer-death detection latency" (tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL,
// 3 seconds today) rather than inventing a second, arbitrary number - a real hang is a much
// coarser, rarer event than a single missed discovery interval, so this floor is already loose
// enough to never false-positive on ordinary scheduling jitter.
#define RMW_TICKLE_WATCHDOG_STALE_THRESHOLD_NS ((uint64_t)tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL)
// How often watchdog_thread_main() wakes up to check - well under the threshold above (so a hang
// is still caught within roughly one threshold's worth of wall time, not several), but coarse
// enough that this thread costs nothing noticeable running alongside poll_thread. Reuses tt_NODE_
// UPDATE_INTERVAL itself - the same cadence this node's own self-announce already runs at - rather
// than a third arbitrary number.
#define RMW_TICKLE_WATCHDOG_CHECK_INTERVAL_NS tt_NODE_UPDATE_INTERVAL
// How finely watchdog_thread_main() slices its own sleep, purely so rmw_destroy_node() doesn't
// have to wait out a full RMW_TICKLE_WATCHDOG_CHECK_INTERVAL_NS for pthread_join() - re-checking
// watchdog_thread_running this often keeps shutdown responsive without needing an interrupt
// mechanism the way poll_thread's own tt_Node_interrupt() gives it.
#define RMW_TICKLE_WATCHDOG_SHUTDOWN_POLL_NS (50 * tt_MILLISECOND)

// Bumps liveliness_lost on every live Publisher this node owns - watchdog_thread_main()'s own
// hang-detected action. Runs on the watchdog thread, never poll_thread, so (unlike rmw_graph.c's
// own *_locked() variants, or check_publisher_deadline()'s own scheduled-from-inside-tt_Node_
// poll() callback) it must take `mutex` itself before touching tickle_node - same rule any other
// non-poll-thread access to it follows (rmw_tickle_node_t's own doc comment).
static void mark_liveliness_lost(rmw_tickle_node_t* node_impl) {
    pthread_mutex_lock(&node_impl->mutex);
    bool marked_any = false;
    for (uint32_t i = 0; i < node_impl->tickle_node.endpoint_count; i++) {
        struct tt_Endpoint* endpoint = node_impl->tickle_node.endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_PUBLISHER) {
            continue;
        }
        // endpoint aliases &((struct tt_Publisher*)endpoint)->endpoint (its first member), which
        // is itself &((rmw_tickle_publisher_t*)...)->tickle_publisher's own first member -
        // recovering the outer rmw_tickle_publisher_t this way matches subscriber_callback()'s
        // own identical offsetof() recovery (rmw_subscription.c).
        struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;
        rmw_tickle_publisher_t* pub_impl =
            (rmw_tickle_publisher_t*)((char*)pub - offsetof(rmw_tickle_publisher_t, tickle_publisher));
        atomic_fetch_add(&pub_impl->liveliness_lost.total_count, 1);
        atomic_fetch_add(&pub_impl->liveliness_lost.unread_count, 1);
        marked_any = true;
    }
    pthread_mutex_unlock(&node_impl->mutex);

    if (marked_any) {
        // Wake anyone blocked in rmw_wait() on this event becoming ready - same wait_mutex/
        // wait_cond check_publisher_deadline() (this file's own doc comment doesn't cover it,
        // rmw_publisher.c does) already broadcasts on for RMW_EVENT_OFFERED_DEADLINE_MISSED, for
        // the identical reason (rmw_tickle_context_impl_t's own doc comment, rmw_tickle.h).
        rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)node_impl->context->impl;
        pthread_mutex_lock(&context_impl->wait_mutex);
        pthread_cond_broadcast(&context_impl->wait_cond);
        pthread_mutex_unlock(&context_impl->wait_mutex);
    }
}

// QoS roadmap #3 (LIVELINESS) follow-up - RMW_EVENT_LIVELINESS_LOST, Milestone 28(b)'s own design
// now implemented. Deliberately its own thread, never folded into poll_thread_main() above: the
// whole point is an observer that keeps running even if poll_thread itself is wedged (a stalled
// callback, or the whole tt_Node_poll() loop hung) - a check made *from* poll_thread could never
// see poll_thread fail to make that same check. Edge-triggered (already_lost latch): a sustained
// hang bumps liveliness_lost exactly once, matching real DDS's own "lease expired" semantics
// (an event, not a continuously-repeating one) rather than once per RMW_TICKLE_WATCHDOG_CHECK_
// INTERVAL_NS for as long as the hang lasts; recovering below the threshold re-arms it so a later,
// separate hang fires again.
static void* watchdog_thread_main(void* arg) {
    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)arg;
    bool already_lost = false;
    uint64_t next_check_ns = tt_get_ns() + RMW_TICKLE_WATCHDOG_CHECK_INTERVAL_NS;
    while (node_impl->watchdog_thread_running) {
        if (tt_get_ns() < next_check_ns) {
            struct timespec shutdown_poll_duration = {
                .tv_sec = 0,
                .tv_nsec = RMW_TICKLE_WATCHDOG_SHUTDOWN_POLL_NS,
            };
            nanosleep(&shutdown_poll_duration, NULL);
            continue;
        }
        next_check_ns = tt_get_ns() + RMW_TICKLE_WATCHDOG_CHECK_INTERVAL_NS;

        uint64_t last_return_ns = atomic_load(&node_impl->poll_thread_last_return_ns);
        bool stale = (tt_get_ns() - last_return_ns) >= RMW_TICKLE_WATCHDOG_STALE_THRESHOLD_NS;
        if (stale && !already_lost) {
            mark_liveliness_lost(node_impl);
        }
        already_lost = stale;
    }
    return NULL;
}

static void* poll_thread_main(void* arg) {
    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)arg;
    while (node_impl->poll_thread_running) {
        pthread_mutex_lock(&node_impl->mutex);
        // tt_RET_INTERRUPTED (another entry point below wants `mutex`, or rmw_destroy_node() is
        // stopping this thread) and tt_RET_TIMEOUT/tt_RET_OK (nothing due, or ordinary receive/
        // scheduler work) all just mean "loop back and check poll_thread_running again" here -
        // there's nothing this thread could usefully do differently for any other tt_Node_poll()
        // failure either, so no per-code special-casing beyond the loop condition itself.
        tt_Node_poll(&node_impl->tickle_node, RMW_TICKLE_POLL_TIMEOUT_NS);
        pthread_mutex_unlock(&node_impl->mutex);

        // QoS roadmap #3 (LIVELINESS) follow-up - RMW_EVENT_LIVELINESS_LOST. The one piece of
        // information watchdog_thread_main() below needs and can't get any other way: proof this
        // thread is still actually looping, not wedged inside tt_Node_poll() (or anywhere else in
        // this loop) - see rmw_tickle_node_t.poll_thread_last_return_ns's own doc comment.
        atomic_store(&node_impl->poll_thread_last_return_ns, tt_get_ns());

        // Found the hard way, benchmarking a real sustained publish rate (rmw_tickle/PLAN.md's
        // rmw-perf.yml): without a real gap here, this thread's own unlock()-then-immediately-
        // relock() pattern starves any *other* thread blocked in pthread_mutex_lock() on the same
        // mutex (rmw_publish() et al.) for tens to hundreds of milliseconds at a time - glibc's
        // mutex makes no fairness guarantee, and a thread re-locking a mutex it just released can
        // win the race against a parked waiter's own futex wake+reschedule far more often than
        // intuition suggests, especially under this call pattern's extremely short critical
        // section and high call frequency (~1000/s). Measured: publish() throughput at ~1000Hz
        // dropped to single digits/sec without this; a plain sched_yield() here did *not* fix it
        // (Linux's CFS scheduler treats it as close to a no-op) - only an actual timed sleep,
        // forcing a real scheduler-mediated handoff, did. The cost is negligible: this thread's
        // own poll cadence is already ~1ms (RMW_TICKLE_POLL_TIMEOUT_NS rounds up to tt_receive()'s
        // own 1ms poll() minimum - see hal_linux.c), so 1us more here is in the noise, and doesn't
        // touch actual network receive latency either way (a real incoming packet or tt_Node_
        // interrupt() still wakes tt_Node_poll() itself promptly; this sleep only delays how soon
        // *this thread* loops back to call it again).
        struct timespec yield_duration = {
            .tv_sec = 0,
            .tv_nsec = RMW_TICKLE_POLL_THREAD_YIELD_NS,
        };
        nanosleep(&yield_duration, NULL);
    }
    return NULL;
}

rmw_node_t* rmw_create_node(rmw_context_t* context, const char* name, const char* node_namespace) {
    if (NULL == context) {
        RMW_SET_ERROR_MSG("context is null");
        return NULL;
    }
    if (NULL == name) {
        RMW_SET_ERROR_MSG("name is null");
        return NULL;
    }
    if (NULL == node_namespace) {
        RMW_SET_ERROR_MSG("node_namespace is null");
        return NULL;
    }
    if (!rmw_tickle_identifier_matches(context->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }
    // Also never checked (found the same way, right next to the name/namespace gap below): a
    // context that's already been rmw_shutdown() (but not yet rmw_context_fini()'d - a fully
    // finalized one is already caught above, since rmw_context_fini() nulls implementation_
    // identifier too) used to silently accept rmw_create_node() anyway - the exact same "wastes
    // the one-node-per-process slot on a node the caller correctly expected to be rejected"
    // problem as an invalid name/namespace, below.
    if (((rmw_tickle_context_impl_t*)context->impl)->shutdown) {
        RMW_SET_ERROR_MSG("context has been shut down");
        return NULL;
    }

    // Neither was ever actually checked (test_rmw_implementation's own test_create_destroy_node.
    // cpp's create_with_bad_arguments is what found this): an invalid name/namespace (spaces,
    // reserved characters, ...) used to silently succeed instead of being rejected - and, worse,
    // consumed the one-node-per-process slot just below on its way to being silently discarded by
    // the caller, wedging every *later* test in the same process that needed to create a real one.
    int validation_result = RMW_NODE_NAME_VALID;
    size_t invalid_index = 0;
    if (RMW_RET_OK != rmw_validate_node_name(name, &validation_result, &invalid_index)) {
        return NULL; // rmw_validate_node_name() already set its own error message
    }
    if (RMW_NODE_NAME_VALID != validation_result) {
        RMW_SET_ERROR_MSG(rmw_node_name_validation_result_string(validation_result));
        return NULL;
    }
    if (RMW_RET_OK != rmw_validate_namespace(node_namespace, &validation_result, &invalid_index)) {
        return NULL; // rmw_validate_namespace() already set its own error message
    }
    if (RMW_NAMESPACE_VALID != validation_result) {
        RMW_SET_ERROR_MSG(rmw_namespace_validation_result_string(validation_result));
        return NULL;
    }

    bool already_created = false;
    if (!atomic_compare_exchange_strong(&g_tickle_node_created, &already_created, true)) {
        // rmw_tickle/PLAN.md's "Deferred: multiple ROS 2 nodes per process" - a component
        // container wanting several ROS nodes in one process needs one tt_Node (own socket, own
        // poll thread) per node, which this milestone doesn't build yet.
        RMW_SET_ERROR_MSG("rmw_tickle supports only one node per process for now");
        return NULL;
    }

    rcutils_allocator_t* allocator = &context->options.allocator;
    rmw_tickle_node_t* node_impl =
        (rmw_tickle_node_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_node_t), allocator->state);
    if (NULL == node_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_node_t");
        atomic_store(&g_tickle_node_created, false);
        return NULL;
    }
    node_impl->allocator = *allocator;
    node_impl->context = context;

    node_impl->rmw_node.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    node_impl->rmw_node.data = node_impl;
    node_impl->rmw_node.context = context;
    node_impl->rmw_node.name = rcutils_strdup(name, *allocator);
    node_impl->rmw_node.namespace_ = rcutils_strdup(node_namespace, *allocator);
    if (NULL == node_impl->rmw_node.name || NULL == node_impl->rmw_node.namespace_) {
        RMW_SET_ERROR_MSG("failed to allocate node name/namespace");
        goto fail;
    }

    if (pthread_mutex_init(&node_impl->mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize node mutex");
        goto fail;
    }

    // tt_Node_create() reads its setup (node id, bind address, broadcast address) entirely from
    // the process-wide _tt_CONFIG (rmw_init()'s own TICKLE_BROADCAST_ADDR handling, or its
    // compiled-in defaults) - see tickle.h's "Lifetime / ownership" note. `name`/`namespace_`
    // above are rmw-level bookkeeping only; TickLE's own wire protocol has no node-name concept,
    // identifying a node purely by its numeric id.
    tt_ret_t ret = tt_Node_create(&node_impl->tickle_node);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create() failed");
        pthread_mutex_destroy(&node_impl->mutex);
        goto fail;
    }

    // discovery is already zeroed (zero_allocate() above) - tt_Node_set_discovery()'s own
    // precondition. See rmw_tickle_node_t's own doc comment on the field.
    ret = tt_Node_set_discovery(&node_impl->tickle_node, &node_impl->discovery, discovery_callback, node_impl);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_set_discovery() failed");
        tt_Node_destroy(&node_impl->tickle_node);
        pthread_mutex_destroy(&node_impl->mutex);
        goto fail;
    }

    // Set before either thread starts - watchdog_thread_main() must never see a stale zero-
    // initialized value (zero_allocate() above) and immediately mistake node startup itself for a
    // hang (tt_get_ns() - 0 is enormous).
    atomic_store(&node_impl->poll_thread_last_return_ns, tt_get_ns());

    node_impl->poll_thread_running = true;
    if (pthread_create(&node_impl->poll_thread, NULL, poll_thread_main, node_impl) != 0) {
        RMW_SET_ERROR_MSG("failed to start poll thread");
        node_impl->poll_thread_running = false;
        tt_Node_destroy(&node_impl->tickle_node);
        pthread_mutex_destroy(&node_impl->mutex);
        goto fail;
    }

    // QoS roadmap #3 (LIVELINESS) follow-up - RMW_EVENT_LIVELINESS_LOST's own watchdog (Milestone
    // 30). A failure here just leaves that one event permanently un-fireable for this node - not
    // fatal to node creation itself (every other rmw_tickle feature still works without it), same
    // "degrade, don't fail the whole node" reasoning DEADLINE's own tt_Node_schedule() failure
    // path already uses elsewhere (rmw_publisher.c/rmw_subscription.c).
    node_impl->watchdog_thread_running = true;
    if (pthread_create(&node_impl->watchdog_thread, NULL, watchdog_thread_main, node_impl) != 0) {
        node_impl->watchdog_thread_running = false;
    }

    return &node_impl->rmw_node;

fail:
    if (node_impl->rmw_node.name != NULL) {
        allocator->deallocate((char*)node_impl->rmw_node.name, allocator->state);
    }
    if (node_impl->rmw_node.namespace_ != NULL) {
        allocator->deallocate((char*)node_impl->rmw_node.namespace_, allocator->state);
    }
    allocator->deallocate(node_impl, allocator->state);
    atomic_store(&g_tickle_node_created, false);
    return NULL;
}

rmw_ret_t rmw_destroy_node(rmw_node_t* node) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rcutils_allocator_t allocator = node_impl->allocator;

    // Same tt_Node_interrupt()-then-lock pattern rmw_publish() et al. (Milestone 3+) will use -
    // see rmw_tickle.h's own rmw_tickle_node_t doc comment. poll_thread_running is set first so
    // the thread's own loop condition is already false by the time it next wakes, whichever of
    // the (possibly several) tt_Node_poll() calls the interrupt actually cuts short.
    node_impl->poll_thread_running = false;
    tt_Node_interrupt(&node_impl->tickle_node);
    pthread_join(node_impl->poll_thread, NULL);

    // watchdog_thread_running is only ever true here if rmw_create_node() actually managed to
    // start it (see its own doc comment there) - nothing to join otherwise. No tt_Node_interrupt()
    // equivalent needed: watchdog_thread_main() never blocks in tt_Node_poll() or any other
    // tickle_node call, it just re-checks this flag every RMW_TICKLE_WATCHDOG_SHUTDOWN_POLL_NS.
    if (node_impl->watchdog_thread_running) {
        node_impl->watchdog_thread_running = false;
        pthread_join(node_impl->watchdog_thread, NULL);
    }

    tt_Node_destroy(&node_impl->tickle_node);
    pthread_mutex_destroy(&node_impl->mutex);

    allocator.deallocate((char*)node_impl->rmw_node.name, allocator.state);
    allocator.deallocate((char*)node_impl->rmw_node.namespace_, allocator.state);
    allocator.deallocate(node_impl, allocator.state);

    atomic_store(&g_tickle_node_created, false);
    return RMW_RET_OK;
}

const rmw_guard_condition_t* rmw_node_get_graph_guard_condition(const rmw_node_t* node) {
    if (NULL == node) {
        RMW_SET_ERROR_MSG("node is null");
        return NULL;
    }
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)node->context->impl;
    return &context_impl->graph_guard_condition.rmw_guard_condition;
}
