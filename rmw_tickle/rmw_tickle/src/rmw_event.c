/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// QoS roadmap #2 (DEADLINE) + #3 (LIVELINESS): rmw_take_event()/rmw_event_fini()/rmw_get_zero_
// initialized_event()/rmw_event_type_is_supported(). rmw_publisher_event_init()/rmw_subscription_
// event_init() (rmw_publisher.c/rmw_subscription.c) are what actually accept an event type and
// wire an rmw_event_t's own .data/.event_type; rmw_wait_set.c's own check_events() is what
// reports readiness. This file is only the "read the current status" half.

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rcutils/error_handling.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/event_callback_type.h"
#include "rmw/events_statuses/incompatible_qos.h"
#include "rmw/events_statuses/liveliness_changed.h"
#include "rmw/events_statuses/liveliness_lost.h"
#include "rmw/events_statuses/offered_deadline_missed.h"
#include "rmw/events_statuses/requested_deadline_missed.h"
#include "rmw/ret_types.h"
#include "rmw_tickle_c/rmw_tickle.h"

rmw_event_t rmw_get_zero_initialized_event(void) {
    rmw_event_t event;
    memset(&event, 0, sizeof(event));
    return event;
}

bool rmw_event_type_is_supported(rmw_event_type_t event_type) {
    switch (event_type) {
    case RMW_EVENT_OFFERED_DEADLINE_MISSED:
    case RMW_EVENT_REQUESTED_DEADLINE_MISSED:
    case RMW_EVENT_LIVELINESS_LOST:
    case RMW_EVENT_LIVELINESS_CHANGED:
    case RMW_EVENT_OFFERED_QOS_INCOMPATIBLE:
    case RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE:
        return true;
    default:
        return false;
    }
}

// Fills a plain rmw_tickle_event_status_t-backed status (offered/requested deadline missed,
// liveliness lost - all three share the exact same {total_count, total_count_change} shape).
static void take_simple_status(rmw_tickle_event_status_t* status, int32_t* total_count, int32_t* total_count_change) {
    *total_count = atomic_load(&status->total_count);
    *total_count_change = atomic_exchange(&status->unread_count, 0);
}

rmw_ret_t rmw_take_event(const rmw_event_t* event_handle, void* event_info, bool* taken) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(event_handle, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(event_info, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(event_handle->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    switch (event_handle->event_type) {
    case RMW_EVENT_OFFERED_DEADLINE_MISSED: {
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)event_handle->data;
        rmw_offered_deadline_missed_status_t* status = (rmw_offered_deadline_missed_status_t*)event_info;
        take_simple_status(&pub_impl->deadline_missed, &status->total_count, &status->total_count_change);
        break;
    }
    case RMW_EVENT_LIVELINESS_LOST: {
        // Always {0, 0} - see rmw_tickle_publisher_t.liveliness_lost's own doc comment
        // (rmw_tickle.h) for why this rmw's AUTOMATIC liveliness structurally can never
        // observe its own loss.
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)event_handle->data;
        rmw_liveliness_lost_status_t* status = (rmw_liveliness_lost_status_t*)event_info;
        take_simple_status(&pub_impl->liveliness_lost, &status->total_count, &status->total_count_change);
        break;
    }
    case RMW_EVENT_REQUESTED_DEADLINE_MISSED: {
        rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)event_handle->data;
        rmw_requested_deadline_missed_status_t* status = (rmw_requested_deadline_missed_status_t*)event_info;
        take_simple_status(&sub_impl->deadline_missed, &status->total_count, &status->total_count_change);
        break;
    }
    case RMW_EVENT_LIVELINESS_CHANGED: {
        rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)event_handle->data;
        rmw_tickle_liveliness_changed_status_t* tickle_status = &sub_impl->liveliness_changed;
        rmw_liveliness_changed_status_t* status = (rmw_liveliness_changed_status_t*)event_info;
        status->alive_count = atomic_load(&tickle_status->alive_count);
        status->not_alive_count = atomic_load(&tickle_status->not_alive_count);
        status->alive_count_change = atomic_exchange(&tickle_status->alive.unread_count, 0);
        status->not_alive_count_change = atomic_exchange(&tickle_status->not_alive.unread_count, 0);
        break;
    }
    case RMW_EVENT_OFFERED_QOS_INCOMPATIBLE: {
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)event_handle->data;
        rmw_tickle_qos_incompatible_status_t* tickle_status = &pub_impl->offered_qos_incompatible;
        rmw_offered_qos_incompatible_event_status_t* status = (rmw_offered_qos_incompatible_event_status_t*)event_info;
        take_simple_status(&tickle_status->base, &status->total_count, &status->total_count_change);
        status->last_policy_kind = tickle_status->last_policy_kind;
        break;
    }
    case RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE: {
        rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)event_handle->data;
        rmw_tickle_qos_incompatible_status_t* tickle_status = &sub_impl->requested_qos_incompatible;
        rmw_requested_qos_incompatible_event_status_t* status =
            (rmw_requested_qos_incompatible_event_status_t*)event_info;
        take_simple_status(&tickle_status->base, &status->total_count, &status->total_count_change);
        status->last_policy_kind = tickle_status->last_policy_kind;
        break;
    }
    default:
        RMW_SET_ERROR_MSG("rmw_tickle does not support this event type");
        return RMW_RET_UNSUPPORTED;
    }

    // Readiness was already established by rmw_wait() (rmw_wait_set.c's own check_events()) -
    // matches this API's own common convention of always reporting taken=true once dispatch here
    // succeeds, same as this rmw's rmw_take()/rmw_take_response()/rmw_take_request() never leave
    // *taken ambiguous either.
    *taken = true;
    return RMW_RET_OK;
}

// Nothing separately heap-allocated - event_handle->data points at the already-owned publisher/
// subscriber (rmw_publisher_event_init()/rmw_subscription_event_init()'s own doc comment), so
// there's nothing for this to free. The *symbol* still has to exist regardless - same reasoning as
// rmw_publisher_event_init() itself (an unresolved dlsym is fatal to rclcpp, a no-op RMW_RET_OK is
// not).
rmw_ret_t rmw_event_fini(rmw_event_t* event) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(event, RMW_RET_INVALID_ARGUMENT);
    *event = rmw_get_zero_initialized_event();
    return RMW_RET_OK;
}

// ---- (g2, RMW_GAPS_PLAN.md) callback slots - see rmw_tickle_callback_slot_t (rmw_tickle.h).

void rmw_tickle_callback_slot_init(rmw_tickle_callback_slot_t* slot) {
    (void)pthread_mutex_init(&slot->mutex, NULL);
    atomic_init(&slot->callback, NULL);
    slot->user_data = NULL;
}

void rmw_tickle_callback_slot_fini(rmw_tickle_callback_slot_t* slot) {
    (void)pthread_mutex_destroy(&slot->mutex);
}

void rmw_tickle_callback_slot_notify(rmw_tickle_callback_slot_t* slot, size_t count) {
    // Called from the poll thread for every delivery, so the common case - no callback set - is a load and a branch.
    // A setter that stores its callback after this load reads the item in its own count (see _set below).
    if (NULL == atomic_load(&slot->callback) || 0 == count) {
        return;
    }
    pthread_mutex_lock(&slot->mutex);
    rmw_event_callback_t callback = atomic_load_explicit(&slot->callback, memory_order_relaxed);
    if (NULL != callback) { // re-read: set(NULL) may have returned since
        callback(slot->user_data, count);
    }
    pthread_mutex_unlock(&slot->mutex);
}

void rmw_tickle_callback_slot_set(rmw_tickle_callback_slot_t* slot, rmw_event_callback_t callback,
                                  const void* user_data, rmw_tickle_waiting_t waiting, const void* entity) {
    pthread_mutex_lock(&slot->mutex);
    slot->user_data = callback != NULL ? user_data : NULL;
    atomic_store(&slot->callback, callback);
    // Read after the store: an item queued before it is counted here, one queued after it notifies - so none is missed.
    size_t backlog = NULL != callback ? waiting(entity) : 0U;
    if (backlog > 0) {
        callback(user_data, backlog); // items already waiting when it was set - once, now
    }
    pthread_mutex_unlock(&slot->mutex);
}

static size_t unread_of(const rmw_tickle_event_status_t* status) {
    int unread = atomic_load(&status->unread_count);
    return unread > 0 ? (size_t)unread : 0U;
}
static size_t event_waiting(const void* status) {
    return unread_of((const rmw_tickle_event_status_t*)status);
}
static size_t liveliness_changed_waiting(const void* status) {
    const rmw_tickle_liveliness_changed_status_t* changed = (const rmw_tickle_liveliness_changed_status_t*)status;
    return unread_of(&changed->alive) + unread_of(&changed->not_alive);
}

// The slot an event's callback lives in, and how to read the events already unread - the count rmw_take_event() would
// report.
static rmw_tickle_callback_slot_t* event_slot(const rmw_event_t* event, rmw_tickle_waiting_t* waiting,
                                              const void** entity) {
    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)event->data;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)event->data;
    rmw_tickle_event_status_t* status = NULL;
    switch (event->event_type) {
    case RMW_EVENT_OFFERED_DEADLINE_MISSED:
        status = &pub_impl->deadline_missed;
        break;
    case RMW_EVENT_LIVELINESS_LOST:
        status = &pub_impl->liveliness_lost;
        break;
    case RMW_EVENT_OFFERED_QOS_INCOMPATIBLE:
        status = &pub_impl->offered_qos_incompatible.base;
        break;
    case RMW_EVENT_REQUESTED_DEADLINE_MISSED:
        status = &sub_impl->deadline_missed;
        break;
    case RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE:
        status = &sub_impl->requested_qos_incompatible.base;
        break;
    case RMW_EVENT_LIVELINESS_CHANGED:
        *waiting = liveliness_changed_waiting;
        *entity = &sub_impl->liveliness_changed;
        return &sub_impl->liveliness_changed.alive.callback;
    default:
        return NULL;
    }
    *waiting = event_waiting;
    *entity = status;
    return &status->callback;
}

rmw_ret_t rmw_event_set_callback(rmw_event_t* event, rmw_event_callback_t callback, const void* user_data) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(event, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(event->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    rmw_tickle_waiting_t waiting = NULL;
    const void* entity = NULL;
    rmw_tickle_callback_slot_t* slot = event_slot(event, &waiting, &entity);
    if (NULL == slot) {
        RMW_SET_ERROR_MSG("event type not supported by rmw_tickle");
        return RMW_RET_UNSUPPORTED;
    }
    rmw_tickle_callback_slot_set(slot, callback, user_data, waiting, entity);
    return RMW_RET_OK;
}
