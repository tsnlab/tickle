/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 5: rmw_create_wait_set()/rmw_destroy_wait_set()/rmw_wait(). rmw_
// wait() waits directly on the one shared condition variable every waitable entity in this rmw
// broadcasts to when it produces new data (subscriber_callback()'s queue push, client_callback()'s
// response_ready, server_callback()'s request_available, rmw_trigger_guard_condition()) - see rmw_
// tickle_context_impl_t's own doc comment (rmw_tickle.h) for the multi-lock design that avoids a
// lost-wakeup race between an entity's own fine-grained lock and this shared condvar's lock.
//
// rmw_wait()'s own contract (rmw/rmw.h): each input array's entries that do *not* end up ready are
// set to NULL in place; entries that stay non-NULL identify what became ready. Each entry is the
// same rmw_tickle_*_t* our own rmw_create_*() functions already store as that entity's ->data (see
// e.g. rcl's own rcl_wait() - it fills these arrays with `handle->data`, not the rmw_*_t wrapper).
//
// A single readiness check must not itself destroy information it might need again on a later loop
// iteration (this function loops, re-checking everything, each time it's woken with nothing yet
// ready - spurious wakeups included): every per-entity check below is safe to call repeatedly with
// `finalize=false` (a pure peek, mutating nothing - not even a guard condition's own has_triggered,
// via atomic_load rather than atomic_exchange), and is only ever called once with `finalize=true`,
// right before this function actually returns, which both nulls out non-ready array entries and -
// for guard conditions specifically - atomically consumes (`has_triggered` back to false) whichever
// ones fired, so a guard condition observed ready in one rmw_wait() call doesn't immediately look
// ready again in the next one without being retriggered.

#include <errno.h> // NOLINT(misc-include-cleaner) -- ETIMEDOUT lives in a glibc-private header; <errno.h> is the correct public one, same as hal_linux.c's EINTR/EAGAIN
#include <pthread.h> // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own <pthread.h> comment
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <sys/timerfd.h>   // timerfd_settime() - executor-driven receive
#include <tickle/config.h> // tt_SECOND, tt_MILLISECOND
#include <tickle/hal.h>    // tt_get_ns()
#include <tickle/tickle.h> // tt_Node_poll(), tt_Node_interrupt()
#include <tickle/trace.h>  // TT_TRACE

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rmw/error_handling.h"
#include "rmw/event.h" // rmw_event_t, RMW_EVENT_* - check_events()
#include "rmw/init.h"  // rmw_context_t
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/time.h" // rmw_time_t
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

rmw_wait_set_t* rmw_create_wait_set(rmw_context_t* context, size_t max_conditions) {
    // rmw_tickle_wait_set_t's own doc comment: nothing here is sized by max_conditions - rcl owns
    // and sizes the entity arrays rmw_wait() scans each call, not this wait set.
    (void)max_conditions;

    if (NULL == context) {
        RMW_SET_ERROR_MSG("context is null");
        return NULL;
    }
    if (!rmw_tickle_identifier_matches(context->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }

    rcutils_allocator_t* allocator = &context->options.allocator;
    rmw_tickle_wait_set_t* wait_set_impl =
        (rmw_tickle_wait_set_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_wait_set_t), allocator->state);
    if (NULL == wait_set_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_wait_set_t");
        return NULL;
    }
    wait_set_impl->context_impl = (rmw_tickle_context_impl_t*)context->impl;
    wait_set_impl->allocator = *allocator;

    wait_set_impl->rmw_wait_set.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    wait_set_impl->rmw_wait_set.data = wait_set_impl;
    wait_set_impl->rmw_wait_set.guard_conditions = NULL; // unused - rmw_wait() takes its own arrays as params
    return &wait_set_impl->rmw_wait_set;
}

rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t* wait_set) {
    // RMW_RET_ERROR, not RMW_RET_INVALID_ARGUMENT - rmw.h's own doc comment on this function says
    // the latter, but test_rmw_implementation's own TestWaitSet.rmw_destroy_wait_set (Milestone 16)
    // expects the former, matching what rmw_fastrtps_cpp's own __rmw_destroy_wait_set() actually
    // does (RMW_CHECK_ARGUMENT_FOR_NULL(wait_set, RMW_RET_ERROR), confirmed by reading its real
    // source) - the doc comment is the stale one here, not the two real implementations agreeing
    // with the test.
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(wait_set, RMW_RET_ERROR);
    if (!rmw_tickle_identifier_matches(wait_set->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_wait_set_t* wait_set_impl = (rmw_tickle_wait_set_t*)wait_set->data;
    rcutils_allocator_t allocator = wait_set_impl->allocator;
    allocator.deallocate(wait_set_impl, allocator.state);
    return RMW_RET_OK;
}

// See this file's own module doc comment for the peek-vs-finalize contract every check_*()
// function below follows.

static bool check_subscriptions(rmw_subscriptions_t* subscriptions, bool finalize) {
    if (NULL == subscriptions) {
        return false;
    }
    bool any_ready = false;
    for (size_t i = 0; i < subscriptions->subscriber_count; ++i) {
        rmw_tickle_subscriber_t* sub = (rmw_tickle_subscriber_t*)subscriptions->subscribers[i];
        pthread_mutex_lock(&sub->queue_mutex);
        bool ready = sub->queue_count > 0;
        pthread_mutex_unlock(&sub->queue_mutex);
        if (ready) {
            any_ready = true;
        } else if (finalize) {
            subscriptions->subscribers[i] = NULL;
        }
    }
    return any_ready;
}

static bool check_services(rmw_services_t* services, bool finalize) {
    if (NULL == services) {
        return false;
    }
    bool any_ready = false;
    for (size_t i = 0; i < services->service_count; ++i) {
        rmw_tickle_service_t* svc = (rmw_tickle_service_t*)services->services[i];
        pthread_mutex_lock(&svc->request_mutex);
        bool ready = svc->request_available;
        pthread_mutex_unlock(&svc->request_mutex);
        if (ready) {
            any_ready = true;
        } else if (finalize) {
            services->services[i] = NULL;
        }
    }
    return any_ready;
}

static bool check_clients(rmw_clients_t* clients, bool finalize) {
    if (NULL == clients) {
        return false;
    }
    bool any_ready = false;
    for (size_t i = 0; i < clients->client_count; ++i) {
        rmw_tickle_client_t* client = (rmw_tickle_client_t*)clients->clients[i];
        pthread_mutex_lock(&client->response_mutex);
        bool ready = client->response_ready;
        pthread_mutex_unlock(&client->response_mutex);
        if (ready) {
            any_ready = true;
        } else if (finalize) {
            clients->clients[i] = NULL;
        }
    }
    return any_ready;
}

static bool check_guard_conditions(rmw_guard_conditions_t* guard_conditions, bool finalize) {
    if (NULL == guard_conditions) {
        return false;
    }
    bool any_ready = false;
    for (size_t i = 0; i < guard_conditions->guard_condition_count; ++i) {
        rmw_tickle_guard_condition_t* guard_cond = (rmw_tickle_guard_condition_t*)guard_conditions->guard_conditions[i];
        // finalize consumes (atomic_exchange); a plain peek must not (atomic_load) - see module
        // doc comment.
        bool ready =
            finalize ? atomic_exchange(&guard_cond->has_triggered, false) : atomic_load(&guard_cond->has_triggered);
        if (ready) {
            any_ready = true;
        } else if (finalize) {
            guard_conditions->guard_conditions[i] = NULL;
        }
    }
    return any_ready;
}

// QoS roadmap #2 (DEADLINE) + #3 (LIVELINESS) - the rmw_tickle_event_status_t whose unread_count
// governs this rmw_event_t's own readiness, or NULL for an event type this rmw doesn't implement
// (degrades to "never ready" rather than crashing - rmw_publisher_event_init()/rmw_subscription_
// event_init() already refuse to hand out an rmw_event_t for anything not covered here, so this
// should only ever see the cases below, but a defensive default costs nothing).
static rmw_tickle_event_status_t* event_status_for(const rmw_event_t* event) {
    switch (event->event_type) {
    case RMW_EVENT_OFFERED_DEADLINE_MISSED:
        return &((rmw_tickle_publisher_t*)event->data)->deadline_missed;
    case RMW_EVENT_LIVELINESS_LOST:
        return &((rmw_tickle_publisher_t*)event->data)->liveliness_lost;
    case RMW_EVENT_REQUESTED_DEADLINE_MISSED:
        return &((rmw_tickle_subscriber_t*)event->data)->deadline_missed;
    case RMW_EVENT_LIVELINESS_CHANGED: {
        rmw_tickle_liveliness_changed_status_t* status = &((rmw_tickle_subscriber_t*)event->data)->liveliness_changed;
        // Ready if *either* side has something unread - rmw_take_event() (rmw_event.c) reads
        // both alive/not_alive in one call, so readiness only needs to reflect "is there
        // anything at all to report," not which side.
        return atomic_load(&status->alive.unread_count) > 0 ? &status->alive : &status->not_alive;
    }
    case RMW_EVENT_OFFERED_QOS_INCOMPATIBLE:
        return &((rmw_tickle_publisher_t*)event->data)->offered_qos_incompatible.base;
    case RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE:
        return &((rmw_tickle_subscriber_t*)event->data)->requested_qos_incompatible.base;
    default:
        return NULL;
    }
}

// An event's own "ready" state is consumed by a *separate* rmw_take_event() call the app makes
// after rmw_wait() returns - not by rmw_wait() itself - exactly like check_subscriptions()'s own
// queue_count > 0 peek that rmw_take() (not rmw_wait()) actually drains. So, unlike check_guard_
// conditions() above, finalize here still only *peeks*: it nulls out entries that aren't ready
// (same contract every other check_*() follows) but never mutates unread_count itself.
static bool check_events(rmw_events_t* events, bool finalize) {
    if (NULL == events) {
        return false;
    }
    bool any_ready = false;
    for (size_t i = 0; i < events->event_count; ++i) {
        const rmw_event_t* event = (const rmw_event_t*)events->events[i];
        rmw_tickle_event_status_t* status = event_status_for(event);
        bool ready = NULL != status && atomic_load(&status->unread_count) > 0;
        if (ready) {
            any_ready = true;
        } else if (finalize) {
            events->events[i] = NULL;
        }
    }
    return any_ready;
}

static void finalize_all(rmw_subscriptions_t* subscriptions, rmw_guard_conditions_t* guard_conditions,
                         rmw_services_t* services, rmw_clients_t* clients, rmw_events_t* events) {
    check_subscriptions(subscriptions, true);
    check_guard_conditions(guard_conditions, true);
    check_services(services, true);
    check_clients(clients, true);
    check_events(events, true);
}

// Executor-driven receive (rmw_tickle_context_impl_t.executor_polling): one pass of rmw_wait()'s readiness
// check, under wait_mutex. Returns RMW_RET_OK if something is ready, RMW_RET_TIMEOUT if `expired` and
// nothing is, both with the wait set finalized; RMW_RET_ERROR (nothing finalized) to keep waiting.
static rmw_ret_t check_once(rmw_tickle_context_impl_t* context_impl, rmw_subscriptions_t* subscriptions,
                            rmw_guard_conditions_t* guard_conditions, rmw_services_t* services, rmw_clients_t* clients,
                            rmw_events_t* events, bool expired) {
    pthread_mutex_lock(&context_impl->wait_mutex);
    bool ready = check_subscriptions(subscriptions, false) || check_guard_conditions(guard_conditions, false) ||
                 check_services(services, false) || check_clients(clients, false) || check_events(events, false);
    if (ready || expired) {
        finalize_all(subscriptions, guard_conditions, services, clients, events);
    }
    pthread_mutex_unlock(&context_impl->wait_mutex);
    if (ready) {
        return RMW_RET_OK;
    }
    return expired ? RMW_RET_TIMEOUT : RMW_RET_ERROR;
}

// Executor-driven receive: arms the parked poll thread's lease timer to fire `in_ns` from now, or disarms it
// with 0. Relative: tt_get_ns() is not the timerfd's clock.
static void set_park_timer(rmw_tickle_context_impl_t* context_impl, uint64_t in_ns) {
    // NOLINTNEXTLINE(misc-include-cleaner) - struct itimerspec: <sys/timerfd.h> above, via a glibc-private header
    struct itimerspec timer = {{0, 0}, {(time_t)(in_ns / tt_SECOND), (long)(in_ns % tt_SECOND)}};
    (void)timerfd_settime(context_impl->park_timer_fd, 0, &timer, NULL);
}

// Executor-driven receive: after interrupting the poll thread's own poll, waits for it to park - a millisecond
// at most, then the caller simply tries again.
static void wait_for_poll_thread_to_park(rmw_tickle_context_impl_t* context_impl) {
    pthread_mutex_lock(&context_impl->wait_mutex);
    if (!atomic_load(&context_impl->poll_thread_parked)) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline); // NOLINT(misc-include-cleaner) - <time.h>
        deadline.tv_nsec += (long)tt_MILLISECOND;
        deadline.tv_sec += deadline.tv_nsec / (long)tt_SECOND;
        deadline.tv_nsec %= (long)tt_SECOND;
        (void)pthread_cond_timedwait(&context_impl->handover_cond, &context_impl->wait_mutex, &deadline);
    }
    pthread_mutex_unlock(&context_impl->wait_mutex);
}

// Executor-driven receive: waits by polling the node on this thread, so a message is received, delivered
// and found ready here, with no poll thread -> executor wake in between. Returns false without waiting if
// another rmw_wait() holds the role; the caller then waits on wait_cond as before. deadline_ns is
// CLOCK_MONOTONIC, UINT64_MAX for no timeout.
static bool wait_by_polling(rmw_tickle_context_impl_t* context_impl, rmw_subscriptions_t* subscriptions,
                            rmw_guard_conditions_t* guard_conditions, rmw_services_t* services, rmw_clients_t* clients,
                            rmw_events_t* events, uint64_t deadline_ns, rmw_ret_t* result) {
    bool idle = false;
    if (!atomic_compare_exchange_strong(&context_impl->executor_polling, &idle, true)) {
        return false;
    }
    atomic_fetch_add(&context_impl->executor_poll_waits, 1);
    set_park_timer(context_impl, 0); // taken back within the lease: the poll thread stays parked
    bool interrupted = false;
    while (true) {
        uint64_t now = tt_get_ns();
        *result =
            check_once(context_impl, subscriptions, guard_conditions, services, clients, events, now >= deadline_ns);
        if (RMW_RET_ERROR != *result) {
            break;
        }
        int64_t rest = UINT64_MAX == deadline_ns ? -1 : (int64_t)(deadline_ns - now);
        tt_ret_t polled = tt_Node_poll(&context_impl->tickle_node, rest);
        if (tt_RET_BUSY == polled) {
            // The poll thread is still inside its own poll: end it once, and wait for it to park
            // (rmw_node.c's park_for_polling_executor() announces that on handover_cond).
            if (!interrupted) {
                (void)tt_Node_interrupt(&context_impl->tickle_node);
                interrupted = true;
            }
            wait_for_poll_thread_to_park(context_impl);
        } else {
            // The node is being polled: what the watchdog wants to know (LIVELINESS_LOST, rmw_node.c).
            atomic_store(&context_impl->poll_thread_last_return_ns, tt_get_ns());
        }
    }
    // Released: the lease starts now. Nobody is woken; the parked poll thread's timer is set for the end of
    // the lease, and taking the role again disarms it (rmw_node.c's park_for_polling_executor()).
    uint64_t left = tt_get_ns();
    atomic_store(&context_impl->executor_left_ns, left);
    atomic_store(&context_impl->executor_polling, false);
    set_park_timer(context_impl, RMW_TICKLE_EXECUTOR_POLL_LEASE_NS);
    if (RMW_RET_OK == *result) {
        TT_TRACE(tt_TRACE_EXEC_WAKE);
    }
    return true;
}

rmw_ret_t rmw_wait(rmw_subscriptions_t* subscriptions, rmw_guard_conditions_t* guard_conditions,
                   rmw_services_t* services, rmw_clients_t* clients, rmw_events_t* events, rmw_wait_set_t* wait_set,
                   const rmw_time_t* wait_timeout) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(wait_set, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(wait_set->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_wait_set_t* wait_set_impl = (rmw_tickle_wait_set_t*)wait_set->data;
    rmw_tickle_context_impl_t* context_impl = wait_set_impl->context_impl;

    bool poll_only = NULL != wait_timeout && 0 == wait_timeout->sec && 0 == wait_timeout->nsec;
    struct timespec deadline;
    if (NULL != wait_timeout && !poll_only) {
        clock_gettime(CLOCK_REALTIME, &deadline); // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own comment
        deadline.tv_sec += (time_t)wait_timeout->sec;
        deadline.tv_nsec += (long)wait_timeout->nsec;
        deadline.tv_sec += deadline.tv_nsec / (long)tt_SECOND;
        deadline.tv_nsec %= (long)tt_SECOND;
    }

    if (!poll_only && context_impl->executor_poll_enabled) {
        uint64_t deadline_ns = UINT64_MAX;
        if (NULL != wait_timeout) {
            deadline_ns = tt_get_ns() + ((uint64_t)wait_timeout->sec * tt_SECOND) + (uint64_t)wait_timeout->nsec;
        }
        rmw_ret_t result = RMW_RET_ERROR;
        if (wait_by_polling(context_impl, subscriptions, guard_conditions, services, clients, events, deadline_ns,
                            &result)) {
            return result;
        }
    }

    pthread_mutex_lock(&context_impl->wait_mutex);
    while (true) {
        bool ready_subs = check_subscriptions(subscriptions, false);
        bool ready_gcs = check_guard_conditions(guard_conditions, false);
        bool ready_svcs = check_services(services, false);
        bool ready_clients = check_clients(clients, false);
        bool ready_events = check_events(events, false);
        if (ready_subs || ready_gcs || ready_svcs || ready_clients || ready_events) {
            finalize_all(subscriptions, guard_conditions, services, clients, events);
            pthread_mutex_unlock(&context_impl->wait_mutex);
            TT_TRACE(tt_TRACE_EXEC_WAKE);
            return RMW_RET_OK;
        }

        if (poll_only) {
            finalize_all(subscriptions, guard_conditions, services, clients, events);
            pthread_mutex_unlock(&context_impl->wait_mutex);
            return RMW_RET_TIMEOUT;
        }

        int wait_ret = NULL == wait_timeout
                           ? pthread_cond_wait(&context_impl->wait_cond, &context_impl->wait_mutex)
                           : pthread_cond_timedwait(&context_impl->wait_cond, &context_impl->wait_mutex, &deadline);
        // NOLINTNEXTLINE(misc-include-cleaner) -- see the <errno.h> include above
        if (ETIMEDOUT == wait_ret) {
            finalize_all(subscriptions, guard_conditions, services, clients, events);
            pthread_mutex_unlock(&context_impl->wait_mutex);
            return RMW_RET_TIMEOUT;
        }
        // Spurious wakeup or a real signal for an entity we don't yet know about - loop back and
        // recheck everything from scratch.
    }
}
