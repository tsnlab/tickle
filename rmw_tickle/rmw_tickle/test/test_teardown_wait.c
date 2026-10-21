/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// An rmw_wait() that holds the executor poll role while the context's last node is destroyed (2026-10-21).
//
// rclcpp's graph listener does exactly this: its thread blocks in rmw_wait() with no timeout on the context's guard
// conditions, and on executor-driven receive that wait polls the TickLE context itself. When the last node went,
// rmw_destroy_node() destroyed the shared TickLE context with that wait still polling it. tt_Context_destroy()
// interrupts the context and then closes its descriptors; the woken wait found nothing ready and polled again. If it
// re-entered epoll_wait() after the sockets were closed and before the epoll descriptor was (a window of ~0.1 ms),
// nothing was left that could wake it, and it slept for ever: rclcpp::shutdown() then triggered the listener's guard
// condition, whose wake went to the closed eventfd ("Cannot send wake signal: Bad file descriptor"), and joined the
// thread for ever. check_ros2_interfaces.sh -A's client hung so in 3 of 100 runs on the PC. Outside that window the
// wait spun on a destroyed context until the trigger, which is why the error line was also seen in runs that passed.
//
// The window is too narrow to hit on purpose, so this test checks the rule whose breach opens it: no thread is inside
// tt_Context_poll() when tt_Context_destroy() starts, and none enters it afterwards. Both are interposed here (rmw
// reaches them through the PLT) and counted, which makes the check deterministic: before the fix part 2 found the
// wait inside the poll at the destroy on every run.
//
//   1. control: a wait inside the poll is ended by a guard condition triggered from another thread - so part 2's
//      wait would end too, if anything still reached it - and the interposed poll saw it (if not, this test could not
//      look, and fails rather than passes);
//   2. the same wait, inside the poll while the last node is destroyed: no poll in flight at the destroy, none
//      entered after it, and the wait ends on the trigger;
//   3. a node created again on the same context: a blocking wait polls it again (executor_poll_waits rises), so the
//      teardown left the poll role free rather than taken for good.
//
// Each wait is joined with a deadline, so a defect fails the test instead of hanging the suite.

// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming) - glibc feature-test macro, for RTLD_NEXT
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <tickle/hal.h>       // tt_get_ns()
#include <tickle/hal_linux.h> // tt_thread_self()
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

#define MS (1000ULL * 1000ULL)
#define MS_PER_S 1000U
#define NS_PER_MS 1000000L
#define INSIDE_WITHIN_MS 2000U // how long a waiter may take to be inside the poll
#define JOIN_WITHIN_MS 5000U   // how long a triggered wait may take to return
#define SPIN_LOOK_MS 20U       // part 2: how long after the destroy to watch for polls before triggering

// What the interposers saw.
static atomic_int polls_in_flight;
static _Atomic uint64_t polls_entered;
static atomic_bool torn_down;                    // from a tt_Context_destroy() start until the test re-creates a node
static atomic_int polls_in_flight_at_destroy;    // at the start of the last tt_Context_destroy()
static _Atomic uint64_t polls_entered_torn_down; // tt_Context_poll() calls made while torn_down

tt_ret_t tt_Context_poll(struct tt_Context* node, int64_t timeout) {
    static tt_ret_t (*real)(struct tt_Context*, int64_t) = NULL;
    if (NULL == real) {
        real = (tt_ret_t(*)(struct tt_Context*, int64_t))dlsym(RTLD_NEXT, "tt_Context_poll");
        assert(NULL != real);
    }
    if (atomic_load(&torn_down)) {
        atomic_fetch_add(&polls_entered_torn_down, 1);
    }
    atomic_fetch_add(&polls_entered, 1);
    atomic_fetch_add(&polls_in_flight, 1);
    tt_ret_t result = real(node, timeout);
    atomic_fetch_sub(&polls_in_flight, 1);
    return result;
}

tt_ret_t tt_Context_destroy(struct tt_Context* node) {
    static tt_ret_t (*real)(struct tt_Context*) = NULL;
    if (NULL == real) {
        real = (tt_ret_t(*)(struct tt_Context*))dlsym(RTLD_NEXT, "tt_Context_destroy");
        assert(NULL != real);
    }
    atomic_store(&torn_down, true);
    atomic_store(&polls_in_flight_at_destroy, atomic_load(&polls_in_flight));
    return real(node);
}

struct waiter {
    rmw_wait_set_t* wait_set;
    rmw_guard_condition_t* guard;
    rmw_tickle_context_impl_t* impl;
    _Atomic uintptr_t thread; // the waiter's tt_thread_self(), 0 until it is about to wait
    rmw_ret_t result;
    atomic_bool done;
};

static void sleep_ms(unsigned millis) {
    struct timespec pause = {(time_t)(millis / MS_PER_S), (long)(millis % MS_PER_S) * NS_PER_MS};
    nanosleep(&pause, NULL);
}

static void* wait_forever(void* arg) {
    struct waiter* waiter = (struct waiter*)arg;
    void* storage[1] = {waiter->guard};
    rmw_guard_conditions_t guards = {.guard_condition_count = 1, .guard_conditions = storage};
    atomic_store(&waiter->thread, tt_thread_self());
    waiter->result = rmw_wait(NULL, &guards, NULL, NULL, NULL, waiter->wait_set, NULL); // no timeout
    atomic_store(&waiter->done, true);
    return NULL;
}

// Whether the waiter is inside core's poll - the state the defect needs - within INSIDE_WITHIN_MS.
static bool waiter_inside_poll(struct waiter* waiter) {
    uint64_t deadline = tt_get_ns() + (INSIDE_WITHIN_MS * MS);
    while (tt_get_ns() < deadline) {
        uintptr_t thread = atomic_load(&waiter->thread);
        if (0 != thread && __atomic_load_n(&waiter->impl->tickle_context.poller_thread, __ATOMIC_RELAXED) == thread) {
            return true;
        }
        sleep_ms(1);
    }
    return false;
}

// Joins the waiter if its wait returns within JOIN_WITHIN_MS; false if it is still waiting then (it is left running:
// the test fails).
static bool joined_in_time(struct waiter* waiter, pthread_t thread) { // NOLINT(misc-include-cleaner) - <pthread.h>
    uint64_t deadline = tt_get_ns() + (JOIN_WITHIN_MS * MS);
    while (!atomic_load(&waiter->done)) {
        if (tt_get_ns() >= deadline) {
            return false;
        }
        sleep_ms(1);
    }
    assert(0 == pthread_join(thread, NULL));
    return true;
}

static pthread_t start_waiter(struct waiter* waiter) { // NOLINT(misc-include-cleaner) - <pthread.h> above
    atomic_store(&waiter->thread, 0);
    atomic_store(&waiter->done, false);
    waiter->result = RMW_RET_ERROR;
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> above
    assert(0 == pthread_create(&thread, NULL, wait_forever, waiter));
    return thread;
}

// 1. Control. Fails also when the wait was not seen inside the interposed poll: the test could not look.
static bool part_1_control(struct waiter* waiter) {
    uint64_t entered_before = atomic_load(&polls_entered);
    pthread_t thread = start_waiter(waiter); // NOLINT(misc-include-cleaner) - <pthread.h> above
    bool inside = waiter_inside_poll(waiter);
    bool seen = atomic_load(&polls_entered) != entered_before;
    assert(RMW_RET_OK == rmw_trigger_guard_condition(waiter->guard));
    if (!joined_in_time(waiter, thread)) {
        printf("teardown wait: FAIL - part 1 (control): a triggered wait inside the poll did not return\n");
        return false;
    }
    if (!inside || !seen) {
        printf("teardown wait: FAIL - part 1 (control): the wait was not seen inside the poll (inside=%d, "
               "interposed=%d) - this test cannot look\n",
               inside ? 1 : 0, seen ? 1 : 0);
        return false;
    }
    assert(RMW_RET_OK == waiter->result);
    printf("teardown wait: part 1 (control) - a wait inside the poll returned on the trigger\n");
    return true;
}

// 2. The last node destroyed under a wait inside the poll.
static bool part_2_destroy_under_the_wait(struct waiter* waiter, rmw_node_t* node) {
    pthread_t thread = start_waiter(waiter); // NOLINT(misc-include-cleaner) - <pthread.h> above
    assert(waiter_inside_poll(waiter));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    sleep_ms(SPIN_LOOK_MS); // a wait still polling the destroyed context would do so meanwhile
    int at_destroy = atomic_load(&polls_in_flight_at_destroy);
    uint64_t after_destroy = atomic_load(&polls_entered_torn_down);
    assert(atomic_load(&torn_down)); // the interposed destroy ran, so the two counts above are a look
    assert(RMW_RET_OK == rmw_trigger_guard_condition(waiter->guard));
    bool returned = joined_in_time(waiter, thread);
    printf("teardown wait: part 2 - polls in flight at the destroy %d, polls entered after it %llu, wait returned %d\n",
           at_destroy, (unsigned long long)after_destroy, returned ? 1 : 0);
    if (0 != at_destroy || 0 != after_destroy || !returned) {
        printf("teardown wait: FAIL - part 2: the context was destroyed under a wait still polling it\n");
        return false;
    }
    assert(RMW_RET_OK == waiter->result);
    return true;
}

// 3. A node again on the same context: the poll role is free, so a blocking wait polls it.
static bool part_3_poll_again(struct waiter* waiter) {
    uint64_t polled_before = atomic_load(&waiter->impl->executor_poll_waits);
    pthread_t thread = start_waiter(waiter); // NOLINT(misc-include-cleaner) - <pthread.h> above
    bool inside = waiter_inside_poll(waiter);
    assert(RMW_RET_OK == rmw_trigger_guard_condition(waiter->guard));
    if (!joined_in_time(waiter, thread)) {
        printf("teardown wait: FAIL - part 3: a triggered wait on the re-created node did not return\n");
        return false;
    }
    assert(RMW_RET_OK == waiter->result);
    if (!inside || atomic_load(&waiter->impl->executor_poll_waits) == polled_before) {
        printf("teardown wait: FAIL - part 3: the wait on the re-created node did not poll (inside=%d)\n",
               inside ? 1 : 0);
        return false;
    }
    printf("teardown wait: part 3 - a wait on the re-created node polled it\n");
    return true;
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_tickle_context_impl_t* impl = (rmw_tickle_context_impl_t*)context.impl;

    rmw_node_t* node = rmw_create_node(&context, "test_teardown_wait", "/");
    assert(NULL != node);
    if (!impl->executor_poll_enabled) {
        printf("teardown wait: FAIL - executor-driven receive is off (RMW_TICKLE_EXECUTOR_POLL=0): nothing tested\n");
        return 1;
    }
    rmw_guard_condition_t* guard = rmw_create_guard_condition(&context);
    assert(NULL != guard);
    rmw_wait_set_t* wait_set = rmw_create_wait_set(&context, 1);
    assert(NULL != wait_set);
    struct waiter waiter = {.wait_set = wait_set, .guard = guard, .impl = impl};

    if (!part_1_control(&waiter) || !part_2_destroy_under_the_wait(&waiter, node)) {
        return 1;
    }
    atomic_store(&torn_down, false);
    node = rmw_create_node(&context, "test_teardown_wait_again", "/");
    assert(NULL != node);
    if (!part_3_poll_again(&waiter)) {
        return 1;
    }
    assert(RMW_RET_OK == rmw_destroy_node(node));

    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_guard_condition(guard));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("teardown wait: PASS\n");
    return 0;
}
