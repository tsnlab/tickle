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

#include <errno.h>
#include <pthread.h> // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own <pthread.h> comment
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include <tickle/config.h> // tt_SECOND

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rmw/error_handling.h"
#include "rmw/init.h" // rmw_context_t
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
    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }

    rcutils_allocator_t* allocator = &context->options.allocator;
    rmw_tickle_wait_set_t* ws =
        (rmw_tickle_wait_set_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_wait_set_t), allocator->state);
    if (NULL == ws) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_wait_set_t");
        return NULL;
    }
    ws->context_impl = (rmw_tickle_context_impl_t*)context->impl;
    ws->allocator = *allocator;

    ws->rmw_wait_set.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    ws->rmw_wait_set.data = ws;
    ws->rmw_wait_set.guard_conditions = NULL; // unused - rmw_wait() takes its own arrays as params
    return &ws->rmw_wait_set;
}

rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t* wait_set) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(wait_set, RMW_RET_INVALID_ARGUMENT);
    if (strcmp(wait_set->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_wait_set_t* ws = (rmw_tickle_wait_set_t*)wait_set->data;
    rcutils_allocator_t allocator = ws->allocator;
    allocator.deallocate(ws, allocator.state);
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
        rmw_tickle_guard_condition_t* gc = (rmw_tickle_guard_condition_t*)guard_conditions->guard_conditions[i];
        // finalize consumes (atomic_exchange); a plain peek must not (atomic_load) - see module
        // doc comment.
        bool ready = finalize ? atomic_exchange(&gc->has_triggered, false) : atomic_load(&gc->has_triggered);
        if (ready) {
            any_ready = true;
        } else if (finalize) {
            guard_conditions->guard_conditions[i] = NULL;
        }
    }
    return any_ready;
}

// rmw_tickle doesn't implement rmw_*_event_init() yet (no PLAN.md milestone covers events) - this
// array should always be empty in practice; handled defensively (never ready) rather than assumed.
static void finalize_events(rmw_events_t* events) {
    if (NULL == events) {
        return;
    }
    for (size_t i = 0; i < events->event_count; ++i) {
        events->events[i] = NULL;
    }
}

static void finalize_all(rmw_subscriptions_t* subscriptions, rmw_guard_conditions_t* guard_conditions,
                         rmw_services_t* services, rmw_clients_t* clients, rmw_events_t* events) {
    check_subscriptions(subscriptions, true);
    check_guard_conditions(guard_conditions, true);
    check_services(services, true);
    check_clients(clients, true);
    finalize_events(events);
}

rmw_ret_t rmw_wait(rmw_subscriptions_t* subscriptions, rmw_guard_conditions_t* guard_conditions,
                   rmw_services_t* services, rmw_clients_t* clients, rmw_events_t* events, rmw_wait_set_t* wait_set,
                   const rmw_time_t* wait_timeout) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(wait_set, RMW_RET_INVALID_ARGUMENT);
    if (strcmp(wait_set->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_wait_set_t* ws = (rmw_tickle_wait_set_t*)wait_set->data;
    rmw_tickle_context_impl_t* context_impl = ws->context_impl;

    bool poll_only = NULL != wait_timeout && 0 == wait_timeout->sec && 0 == wait_timeout->nsec;
    struct timespec deadline;
    if (NULL != wait_timeout && !poll_only) {
        clock_gettime(CLOCK_REALTIME, &deadline); // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own comment
        deadline.tv_sec += (time_t)wait_timeout->sec;
        deadline.tv_nsec += (long)wait_timeout->nsec;
        deadline.tv_sec += deadline.tv_nsec / (long)tt_SECOND;
        deadline.tv_nsec %= (long)tt_SECOND;
    }

    pthread_mutex_lock(&context_impl->wait_mutex);
    while (true) {
        bool ready_subs = check_subscriptions(subscriptions, false);
        bool ready_gcs = check_guard_conditions(guard_conditions, false);
        bool ready_svcs = check_services(services, false);
        bool ready_clients = check_clients(clients, false);
        if (ready_subs || ready_gcs || ready_svcs || ready_clients) {
            finalize_all(subscriptions, guard_conditions, services, clients, events);
            pthread_mutex_unlock(&context_impl->wait_mutex);
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
        if (ETIMEDOUT == wait_ret) {
            finalize_all(subscriptions, guard_conditions, services, clients, events);
            pthread_mutex_unlock(&context_impl->wait_mutex);
            return RMW_RET_TIMEOUT;
        }
        // Spurious wakeup or a real signal for an entity we don't yet know about - loop back and
        // recheck everything from scratch.
    }
}
