/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 2: rmw_create_node()/rmw_destroy_node(), a background thread
// driving tt_Node_poll(), and a mutex every other entry point (rmw_publish() et al.) must
// serialize against it with - see rmw_tickle.h's own rmw_tickle_context_impl_t doc comment for
// the exact locking contract. Milestone 34 promoted the tt_Node/poll_thread/mutex/discovery from
// one-per-rmw_tickle_node_t to one-per-rmw_tickle_context_impl_t, reference-counted across
// however many rmw_create_node() calls share that context - see that struct's own doc comment for
// why a logical "node" never needed its own transport identity in the first place.

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h> // offsetof - mark_automatic_publishers_lost()'s own tt_Publisher -> rmw_tickle_publisher_t recovery
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

// tt_RECEIVE_TIMEOUT (config.h, 100us) is what tt_Node_poll() itself substitutes for any
// negative timeout - passed explicitly here (rather than -1, matching examples/*/*.c's own
// top-level poll loops) so this file doesn't depend on that substitution as an implicit,
// undocumented-at-the-call-site default. A macro (matching rmw_tickle.h's own RMW_TICKLE_*
// constants), not a `static const` variable - readability-identifier-naming's ConstantCase
// (lower_case) applies to the latter but not to a macro (MacroDefinitionCase: UPPER_CASE).
#define RMW_TICKLE_POLL_TIMEOUT_NS tt_RECEIVE_TIMEOUT

// rmw_tickle/PLAN.md's Milestone 6: "graph-changed guard condition wired to 0(c)'s callback".
// Runs on the poll thread, context_impl->node_mutex already held (tt_Node_set_discovery()'s own
// contract - this fires from inside whichever tt_Node_poll() call just processed the UPDATE).
// node_id/endpoint_id/kind/departed aren't needed here - rmw's own contract is just "something in
// the graph changed, go re-query it", not "here's exactly what changed" (rmw_get_node_names() et
// al. are the re-query), so this simply triggers the guard condition unconditionally on every
// appear/refresh/depart - the same pattern rmw_trigger_guard_condition() (rmw_guard_condition.c)
// itself uses. `param` is context_impl directly now (Milestone 34) - tt_Node_set_discovery() is
// called once per context, not once per logical node, so there's no specific rmw_tickle_node_t to
// reach it through anymore (nor would one make sense: discovery was never actually scoped to one).
static void discovery_callback(struct tt_Node* node, uint8_t node_id, uint32_t endpoint_id, uint8_t kind, bool departed,
                               void* param) {
    (void)node;
    (void)node_id;
    (void)endpoint_id;
    (void)kind;
    (void)departed;
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)param;

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
// QoS roadmap #3 (LIVELINESS) follow-up, Milestone 32 - how long check_manual_publishers_lost()
// below waits for context_impl->node_mutex before giving up for this cycle - see its own doc
// comment for why this needs pthread_mutex_timedlock() specifically, neither a plain trylock() nor
// an untimed lock(). Generous next to poll_thread's own microsecond-scale hold per tt_Node_poll()
// call, tiny next to RMW_TICKLE_WATCHDOG_CHECK_INTERVAL_NS itself.
#define RMW_TICKLE_WATCHDOG_MANUAL_CHECK_TIMEOUT_NS (10 * tt_MILLISECOND)

static void bump_liveliness_lost(rmw_tickle_publisher_t* pub_impl) {
    atomic_fetch_add(&pub_impl->liveliness_lost.total_count, 1);
    atomic_fetch_add(&pub_impl->liveliness_lost.unread_count, 1);
}

static void broadcast_wait_cond(rmw_tickle_context_impl_t* context_impl) {
    // Wake anyone blocked in rmw_wait() on this event becoming ready - same wait_mutex/wait_cond
    // check_publisher_deadline() (this file's own doc comment doesn't cover it, rmw_publisher.c
    // does) already broadcasts on for RMW_EVENT_OFFERED_DEADLINE_MISSED, for the identical reason
    // (rmw_tickle_context_impl_t's own doc comment, rmw_tickle.h).
    pthread_mutex_lock(&context_impl->wait_mutex);
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);
}

// Bumps liveliness_lost on every AUTOMATIC Publisher (liveliness_lease_ns == 0) across every
// logical node sharing this context - watchdog_thread_main()'s own node-wide-hang action
// (Milestone 30, now context-wide per Milestone 34 - poll_thread health was always a property of
// the one shared tt_Node, never actually per-logical-node). Called *only* once node_stale has
// already been computed true by the caller, lock-free, before this - a deliberate "decide first,
// act after" split: this function's own pthread_mutex_lock() below may have to wait out however
// long node_mutex is actually held (a real hang could be seconds), and by the time it returns,
// poll_thread may already have resumed and refreshed poll_thread_last_return_ns back to a fresh
// value - but that no longer matters, since the "was it stale" decision was already made and
// handed in, not re-derived here. (A version of this function that re-checked staleness itself
// under the lock was tried and reverted - see PLAN.md's own note - it raced exactly like that:
// poll_thread wins the reacquire race the instant the lock frees, erasing the staleness signal
// before this function's own re-check could see it.)
static void mark_automatic_publishers_lost(rmw_tickle_context_impl_t* context_impl) {
    pthread_mutex_lock(&context_impl->node_mutex);
    bool marked_any = false;
    for (uint32_t i = 0; i < context_impl->tickle_node.endpoint_count; i++) {
        struct tt_Endpoint* endpoint = context_impl->tickle_node.endpoints[i];
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
        if (pub_impl->liveliness_lease_ns == 0) {
            bump_liveliness_lost(pub_impl);
            marked_any = true;
        }
    }
    pthread_mutex_unlock(&context_impl->node_mutex);
    if (marked_any) {
        broadcast_wait_cond(context_impl);
    }
}

// QoS roadmap #3 (LIVELINESS) follow-up, Milestone 32 - the MANUAL_BY_TOPIC counterpart to
// mark_automatic_publishers_lost() above, checked every watchdog cycle rather than only on an
// edge transition (each manual Publisher's own lease is independent, so there's no single node-
// wide "already lost" gate to check before locking the way the automatic case has). Deliberately
// pthread_mutex_timedlock(), not a plain trylock(): last_asserted_ns only ever changes via an
// explicit rmw_publisher_assert_liveliness() call (a rare, deliberate application action, not
// something racing to "heal" itself the instant this lock frees the way poll_thread_last_return_ns
// does for the automatic case above), but a bare trylock() here loses to poll_thread almost every
// single time regardless: poll_thread_main() holds this exact mutex for the duration of every
// tt_Node_poll() call (up to RMW_TICKLE_POLL_TIMEOUT_NS) and releases it for only RMW_TICKLE_POLL_
// THREAD_YIELD_NS (1us) before relocking - trylock() isn't a queued waiter the way a blocking lock
// is, so it has no fair shot at that 1us gap the way rmw_publish() et al.'s own blocking pthread_
// mutex_lock() calls do (futex wake semantics give a real waiter a turn; a bare trylock() just
// samples the lock state once and gives up). pthread_mutex_timedlock() with a short, bounded
// timeout gets the best of both: it registers as a real waiter (reliably wins under this same
// ordinary poll_thread contention, unlike trylock()), but still can't block this thread
// indefinitely during a genuine long hang the way an untimed lock() would - RMW_TICKLE_WATCHDOG_
// MANUAL_CHECK_TIMEOUT_NS is generous next to poll_thread's own ~microsecond-scale hold, but tiny
// next to RMW_TICKLE_WATCHDOG_CHECK_INTERVAL_NS, so a miss here just tries again next cycle.
static void check_manual_publishers_lost(rmw_tickle_context_impl_t* context_impl) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline); // NOLINT(misc-include-cleaner) - see rmw_wait_set.c's own comment
    deadline.tv_nsec += RMW_TICKLE_WATCHDOG_MANUAL_CHECK_TIMEOUT_NS;
    deadline.tv_sec += deadline.tv_nsec / (long)tt_SECOND;
    deadline.tv_nsec %= (long)tt_SECOND;
    if (pthread_mutex_timedlock(&context_impl->node_mutex, &deadline) != 0) {
        return;
    }
    uint64_t now = tt_get_ns();
    bool marked_any = false;
    for (uint32_t i = 0; i < context_impl->tickle_node.endpoint_count; i++) {
        struct tt_Endpoint* endpoint = context_impl->tickle_node.endpoints[i];
        if (endpoint == NULL || endpoint->kind != tt_KIND_TOPIC_PUBLISHER) {
            continue;
        }
        struct tt_Publisher* pub = (struct tt_Publisher*)endpoint;
        rmw_tickle_publisher_t* pub_impl =
            (rmw_tickle_publisher_t*)((char*)pub - offsetof(rmw_tickle_publisher_t, tickle_publisher));
        if (pub_impl->liveliness_lease_ns == 0) {
            continue; // AUTOMATIC - mark_automatic_publishers_lost()'s own concern, not this one
        }
        bool manual_stale = (now - atomic_load(&pub_impl->last_asserted_ns)) >= pub_impl->liveliness_lease_ns;
        if (manual_stale && !pub_impl->liveliness_lost_latched) {
            bump_liveliness_lost(pub_impl);
            marked_any = true;
        }
        pub_impl->liveliness_lost_latched = manual_stale;
    }
    pthread_mutex_unlock(&context_impl->node_mutex);
    if (marked_any) {
        broadcast_wait_cond(context_impl);
    }
}

// QoS roadmap #3 (LIVELINESS) follow-up - RMW_EVENT_LIVELINESS_LOST, Milestone 28(b)'s own design
// (AUTOMATIC, Milestone 30) plus MANUAL_BY_TOPIC (Milestone 32) both implemented, now checked
// context-wide (Milestone 34) since poll_thread health and the tt_Node it polls are both shared.
// Deliberately its own thread, never folded into poll_thread_main() above: the whole point is an
// observer that keeps running even if poll_thread itself is wedged (a stalled callback, or the
// whole tt_Node_poll() loop hung) - a check made *from* poll_thread could never see poll_thread
// fail to make that same check, and a manual-liveliness lease needs checking on its own schedule
// regardless of poll_thread's health anyway. node_wide_already_lost is computed fresh, lock-free,
// every cycle - see mark_automatic_publishers_lost()'s own doc comment for why the actual lock/
// mark step only ever runs on the exact cycle staleness first transitions true, never re-deriving
// it after blocking on the lock.
static void* watchdog_thread_main(void* arg) {
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)arg;
    bool node_wide_already_lost = false;
    uint64_t next_check_ns = tt_get_ns() + RMW_TICKLE_WATCHDOG_CHECK_INTERVAL_NS;
    while (context_impl->watchdog_thread_running) {
        if (tt_get_ns() < next_check_ns) {
            struct timespec shutdown_poll_duration = {
                .tv_sec = 0,
                .tv_nsec = RMW_TICKLE_WATCHDOG_SHUTDOWN_POLL_NS,
            };
            nanosleep(&shutdown_poll_duration, NULL);
            continue;
        }
        next_check_ns = tt_get_ns() + RMW_TICKLE_WATCHDOG_CHECK_INTERVAL_NS;

        bool node_stale = (tt_get_ns() - atomic_load(&context_impl->poll_thread_last_return_ns)) >=
                          RMW_TICKLE_WATCHDOG_STALE_THRESHOLD_NS;
        if (node_stale && !node_wide_already_lost) {
            mark_automatic_publishers_lost(context_impl);
        }
        node_wide_already_lost = node_stale;

        check_manual_publishers_lost(context_impl);
    }
    return NULL;
}

static void* poll_thread_main(void* arg) {
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)arg;
    while (context_impl->poll_thread_running) {
        pthread_mutex_lock(&context_impl->node_mutex);
        // tt_RET_INTERRUPTED (another entry point below wants `node_mutex`, or rmw_destroy_node()
        // is stopping this thread) and tt_RET_TIMEOUT/tt_RET_OK (nothing due, or ordinary receive/
        // scheduler work) all just mean "loop back and check poll_thread_running again" here -
        // there's nothing this thread could usefully do differently for any other tt_Node_poll()
        // failure either, so no per-code special-casing beyond the loop condition itself.
        tt_Node_poll(&context_impl->tickle_node, RMW_TICKLE_POLL_TIMEOUT_NS);
        pthread_mutex_unlock(&context_impl->node_mutex);

        // QoS roadmap #3 (LIVELINESS) follow-up - RMW_EVENT_LIVELINESS_LOST. The one piece of
        // information watchdog_thread_main() below needs and can't get any other way: proof this
        // thread is still actually looping, not wedged inside tt_Node_poll() (or anywhere else in
        // this loop) - see rmw_tickle_context_impl_t.poll_thread_last_return_ns's own doc comment.
        atomic_store(&context_impl->poll_thread_last_return_ns, tt_get_ns());

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

// Milestone 34 - the real, one-time-per-context setup previously done unconditionally inside
// rmw_create_node() itself: tt_Node_create()/tt_Node_set_discovery()/starting poll_thread and
// watchdog_thread. Called only when context_impl->node_count is about to go from 0 to 1 (the
// caller, rmw_create_node() below, already holds registry_mutex). On any failure, callers must
// not proceed to actually register the new logical node - context_impl is left exactly as it was
// found (zeroed, from rmw_init()'s own allocation), safe to retry on a later rmw_create_node()
// call.
static rmw_ret_t start_shared_tickle_node(rmw_tickle_context_impl_t* context_impl) {
    if (pthread_mutex_init(&context_impl->node_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize node mutex");
        return RMW_RET_ERROR;
    }

    // tt_Node_create() reads its setup (node id, bind address, broadcast address) entirely from
    // the process-wide _tt_CONFIG (rmw_init()'s own TICKLE_BROADCAST_ADDR handling, or its
    // compiled-in defaults) - see tickle.h's "Lifetime / ownership" note.
    tt_ret_t ret = tt_Node_create(&context_impl->tickle_node);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_create() failed");
        pthread_mutex_destroy(&context_impl->node_mutex);
        return RMW_RET_ERROR;
    }

    // discovery is already zeroed (rmw_init()'s own allocation) - tt_Node_set_discovery()'s own
    // precondition. See rmw_tickle_context_impl_t's own doc comment on the field.
    ret = tt_Node_set_discovery(&context_impl->tickle_node, &context_impl->discovery, discovery_callback, context_impl);
    if (ret != tt_RET_OK) {
        RMW_SET_ERROR_MSG("tt_Node_set_discovery() failed");
        tt_Node_destroy(&context_impl->tickle_node);
        pthread_mutex_destroy(&context_impl->node_mutex);
        return RMW_RET_ERROR;
    }

    // Set before either thread starts - watchdog_thread_main() must never see a stale zero-
    // initialized value and immediately mistake node startup itself for a hang (tt_get_ns() - 0 is
    // enormous).
    atomic_store(&context_impl->poll_thread_last_return_ns, tt_get_ns());

    context_impl->poll_thread_running = true;
    if (pthread_create(&context_impl->poll_thread, NULL, poll_thread_main, context_impl) != 0) {
        RMW_SET_ERROR_MSG("failed to start poll thread");
        context_impl->poll_thread_running = false;
        tt_Node_destroy(&context_impl->tickle_node);
        pthread_mutex_destroy(&context_impl->node_mutex);
        return RMW_RET_ERROR;
    }

    // QoS roadmap #3 (LIVELINESS) follow-up - RMW_EVENT_LIVELINESS_LOST's own watchdog (Milestone
    // 30). A failure here just leaves that one event permanently un-fireable for this context -
    // not fatal to node creation itself (every other rmw_tickle feature still works without it),
    // same "degrade, don't fail the whole node" reasoning DEADLINE's own tt_Node_schedule() failure
    // path already uses elsewhere (rmw_publisher.c/rmw_subscription.c).
    context_impl->watchdog_thread_running = true;
    if (pthread_create(&context_impl->watchdog_thread, NULL, watchdog_thread_main, context_impl) != 0) {
        context_impl->watchdog_thread_running = false;
    }

    return RMW_RET_OK;
}

// The exact inverse of start_shared_tickle_node() above - called only when context_impl->
// node_count is about to drop from 1 to 0 (the caller, rmw_destroy_node() below, already holds
// registry_mutex).
static void stop_shared_tickle_node(rmw_tickle_context_impl_t* context_impl) {
    // Same tt_Node_interrupt()-then-lock pattern rmw_publish() et al. use - see rmw_tickle.h's own
    // rmw_tickle_context_impl_t doc comment. poll_thread_running is set first so the thread's own
    // loop condition is already false by the time it next wakes, whichever of the (possibly
    // several) tt_Node_poll() calls the interrupt actually cuts short.
    context_impl->poll_thread_running = false;
    tt_Node_interrupt(&context_impl->tickle_node);
    pthread_join(context_impl->poll_thread, NULL);

    // watchdog_thread_running is only ever true here if start_shared_tickle_node() actually
    // managed to start it (see its own doc comment there) - nothing to join otherwise. No tt_Node_
    // interrupt() equivalent needed: watchdog_thread_main() never blocks in tt_Node_poll() or any
    // other tickle_node call, it just re-checks this flag every RMW_TICKLE_WATCHDOG_SHUTDOWN_
    // POLL_NS.
    if (context_impl->watchdog_thread_running) {
        context_impl->watchdog_thread_running = false;
        pthread_join(context_impl->watchdog_thread, NULL);
    }

    tt_Node_destroy(&context_impl->tickle_node);
    pthread_mutex_destroy(&context_impl->node_mutex);
}

// Milestone 34 - registry_mutex must already be held. Returns true iff some entry in nodes[]
// already has this exact (name, namespace) pair - rmw_create_node()'s own duplicate-rejection
// check, matching this whole package's established "reject explicitly, don't silently accept a
// broken duplicate" philosophy rather than letting two logical nodes silently share one identity.
static bool node_name_already_registered(rmw_tickle_context_impl_t* context_impl, const char* name,
                                         const char* node_namespace) {
    int count = atomic_load(&context_impl->node_count);
    for (int i = 0; i < count; i++) {
        rmw_tickle_node_t* existing = context_impl->nodes[i];
        if (strcmp(existing->rmw_node.name, name) == 0 && strcmp(existing->rmw_node.namespace_, node_namespace) == 0) {
            return true;
        }
    }
    return false;
}

// Milestone 34 - registry_mutex must already be held. Appends node_impl to context_impl->nodes[],
// growing the allocator-owned array (doubling, starting from a small initial capacity) if needed.
// Returns false (RMW_SET_ERROR_MSG already set) only on allocation failure.
static bool register_node(rmw_tickle_context_impl_t* context_impl, rmw_tickle_node_t* node_impl) {
    int count = atomic_load(&context_impl->node_count);
    if ((size_t)count == context_impl->nodes_capacity) {
        size_t new_capacity = context_impl->nodes_capacity == 0 ? 4 : context_impl->nodes_capacity * 2;
        rmw_tickle_node_t** new_nodes = (rmw_tickle_node_t**)context_impl->allocator.reallocate(
            (void*)context_impl->nodes, new_capacity * sizeof(rmw_tickle_node_t*), context_impl->allocator.state);
        if (NULL == new_nodes) {
            RMW_SET_ERROR_MSG("failed to grow node registry");
            return false;
        }
        context_impl->nodes = new_nodes;
        context_impl->nodes_capacity = new_capacity;
    }
    context_impl->nodes[count] = node_impl;
    atomic_store(&context_impl->node_count, count + 1);
    return true;
}

// Milestone 34 - registry_mutex must already be held. Removes node_impl from context_impl->
// nodes[] (found by pointer identity, not name/namespace - two distinct rmw_tickle_node_t
// instances could in principle share a name if the duplicate-rejection check above were ever
// bypassed, so identity is the only unambiguous key), compacting the array by shifting later
// entries down one slot. A no-op (impossible in practice - every rmw_tickle_node_t this is ever
// called with came from register_node() above) if node_impl isn't found.
static void unregister_node(rmw_tickle_context_impl_t* context_impl, rmw_tickle_node_t* node_impl) {
    int count = atomic_load(&context_impl->node_count);
    for (int i = 0; i < count; i++) {
        if (context_impl->nodes[i] == node_impl) {
            for (int j = i; j < count - 1; j++) {
                context_impl->nodes[j] = context_impl->nodes[j + 1];
            }
            atomic_store(&context_impl->node_count, count - 1);
            return;
        }
    }
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
    // identifier too) used to silently accept rmw_create_node() anyway.
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)context->impl;
    if (context_impl->shutdown) {
        RMW_SET_ERROR_MSG("context has been shut down");
        return NULL;
    }

    // Neither was ever actually checked (test_rmw_implementation's own test_create_destroy_node.
    // cpp's create_with_bad_arguments is what found this): an invalid name/namespace (spaces,
    // reserved characters, ...) used to silently succeed instead of being rejected.
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

    rcutils_allocator_t* allocator = &context->options.allocator;
    rmw_tickle_node_t* node_impl =
        (rmw_tickle_node_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_node_t), allocator->state);
    if (NULL == node_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_node_t");
        return NULL;
    }
    node_impl->allocator = *allocator;
    node_impl->context_impl = context_impl;

    node_impl->rmw_node.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    node_impl->rmw_node.data = node_impl;
    node_impl->rmw_node.context = context;
    node_impl->rmw_node.name = rcutils_strdup(name, *allocator);
    node_impl->rmw_node.namespace_ = rcutils_strdup(node_namespace, *allocator);
    if (NULL == node_impl->rmw_node.name || NULL == node_impl->rmw_node.namespace_) {
        RMW_SET_ERROR_MSG("failed to allocate node name/namespace");
        goto fail;
    }

    // Milestone 34 - registry_mutex guards node_count/nodes[] (and gates the real tt_Node_create()
    // et al. below) for however many rmw_create_node() calls race against each other or against
    // rmw_destroy_node() on a sibling node.
    pthread_mutex_lock(&context_impl->registry_mutex);
    if (node_name_already_registered(context_impl, name, node_namespace)) {
        pthread_mutex_unlock(&context_impl->registry_mutex);
        RMW_SET_ERROR_MSG("a node with this name and namespace already exists in this context");
        goto fail;
    }
    if (atomic_load(&context_impl->node_count) == 0) {
        rmw_ret_t ret = start_shared_tickle_node(context_impl);
        if (ret != RMW_RET_OK) {
            pthread_mutex_unlock(&context_impl->registry_mutex);
            goto fail;
        }
    }
    if (!register_node(context_impl, node_impl)) {
        // Only reachable failure here is the allocator failing to grow nodes[] - if this was also
        // the very first node, undo the tt_Node_create() et al. just done above rather than
        // leaving a fully-started shared node with zero registered owners.
        if (atomic_load(&context_impl->node_count) == 0) {
            stop_shared_tickle_node(context_impl);
        }
        pthread_mutex_unlock(&context_impl->registry_mutex);
        goto fail;
    }
    pthread_mutex_unlock(&context_impl->registry_mutex);

    return &node_impl->rmw_node;

fail:
    if (node_impl->rmw_node.name != NULL) {
        allocator->deallocate((char*)node_impl->rmw_node.name, allocator->state);
    }
    if (node_impl->rmw_node.namespace_ != NULL) {
        allocator->deallocate((char*)node_impl->rmw_node.namespace_, allocator->state);
    }
    allocator->deallocate(node_impl, allocator->state);
    return NULL;
}

rmw_ret_t rmw_destroy_node(rmw_node_t* node) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rmw_tickle_context_impl_t* context_impl = node_impl->context_impl;
    rcutils_allocator_t allocator = node_impl->allocator;

    // Milestone 34 - the real tt_Node_destroy() et al. only happens once every logical node
    // sharing it is gone (node_count back to 0), not unconditionally the way a single, always-
    // exactly-one node's own rmw_destroy_node() used to.
    pthread_mutex_lock(&context_impl->registry_mutex);
    unregister_node(context_impl, node_impl);
    if (atomic_load(&context_impl->node_count) == 0) {
        stop_shared_tickle_node(context_impl);
    }
    pthread_mutex_unlock(&context_impl->registry_mutex);

    allocator.deallocate((char*)node_impl->rmw_node.name, allocator.state);
    allocator.deallocate((char*)node_impl->rmw_node.namespace_, allocator.state);
    allocator.deallocate(node_impl, allocator.state);

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
