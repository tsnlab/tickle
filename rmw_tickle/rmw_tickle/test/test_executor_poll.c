/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Executor-driven receive (RMW_TICKLE_EXECUTOR_POLL=1, rmw_tickle/RMW_PERF_PLAN.md, 2026-09-26): a blocking
// rmw_wait() polls the node itself. What must still hold, in one process, with guard conditions and core
// scheduler entries standing in for traffic:
//   1. core's timers run while an executor holds the poll role - on the executor's thread, on time;
//   2. once the executor has been away longer than the lease (a long callback), the poll thread serves them;
//   3. a guard condition triggered from another thread wakes an rmw_wait() that is polling;
//   4. two rmw_wait() calls at once - one polls, the other waits on the condition variable - both return.

#include <assert.h>
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
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

#define MS (1000ULL * 1000ULL)
#define MS_PER_S 1000U
#define NS_PER_MS 1000000L
#define TIMER_DUE_MS 50U      // part 1: a timer this far into the wait
#define WAIT_MS 300U          // part 1: the wait
#define AWAY_TIMER_DUE_MS 20U // part 2: a timer this far into the executor's absence, past the lease
#define AWAY_MS 50U           // part 2: the absence
#define TRIGGER_AFTER_MS 20U  // parts 3 and 4: when the other thread triggers
#define WOKEN_WITHIN_MS 5U    // part 3: how soon after that the wait must end
#define BOTH_WITHIN_MS 10U    // part 4: how soon both waits must end

static _Atomic uint64_t fired_at_ns;
static _Atomic uintptr_t fired_on_thread;

static void record_firing(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    atomic_store(&fired_at_ns, tt_get_ns());
    atomic_store(&fired_on_thread, tt_thread_self());
}

static void sleep_ms(unsigned millis) {
    struct timespec pause = {.tv_sec = millis / MS_PER_S, .tv_nsec = (long)(millis % MS_PER_S) * NS_PER_MS};
    nanosleep(&pause, NULL);
}

struct trigger_later {
    rmw_guard_condition_t* guard;
    unsigned after_ms;
};

static void* trigger_after(void* arg) {
    struct trigger_later* later = arg;
    sleep_ms(later->after_ms);
    assert(RMW_RET_OK == rmw_trigger_guard_condition(later->guard));
    return NULL;
}

struct waiter {
    rmw_wait_set_t* wait_set;
    rmw_guard_condition_t* guard;
    rmw_ret_t result;
};

static void* wait_on_guard(void* arg) {
    struct waiter* waiter = arg;
    void* storage[1] = {waiter->guard};
    rmw_guard_conditions_t guards = {.guard_condition_count = 1, .guard_conditions = storage};
    rmw_time_t timeout = {2, 0};
    waiter->result = rmw_wait(NULL, &guards, NULL, NULL, NULL, waiter->wait_set, &timeout);
    return NULL;
}

int main(void) {
    assert(0 == setenv("RMW_TICKLE_EXECUTOR_POLL", "1", 1));
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_executor_poll", "/");
    assert(NULL != node);
    rmw_tickle_context_impl_t* impl = (rmw_tickle_context_impl_t*)context.impl;
    assert(impl->executor_poll_enabled);
    struct tt_Node* tickle_node = &impl->tickle_node;

    rmw_guard_condition_t* guard = rmw_create_guard_condition(&context);
    rmw_wait_set_t* wait_set = rmw_create_wait_set(&context, 0);
    void* storage[1] = {guard};
    rmw_guard_conditions_t guards = {.guard_condition_count = 1, .guard_conditions = storage};
    const uintptr_t main_thread = tt_thread_self();

    // 1. A timer due 50 ms into a 300 ms wait fires then, on this thread: the executor holds the role.
    uint64_t due = tt_get_ns() + (TIMER_DUE_MS * MS);
    assert(tt_Node_schedule(tickle_node, due, record_firing, NULL));
    rmw_time_t timeout = {0, WAIT_MS * MS};
    assert(RMW_RET_TIMEOUT == rmw_wait(NULL, &guards, NULL, NULL, NULL, wait_set, &timeout));
    assert(atomic_load(&fired_at_ns) >= due && atomic_load(&fired_at_ns) <= due + (2 * MS));
    assert(atomic_load(&fired_on_thread) == main_thread);
    assert(atomic_load(&impl->executor_poll_waits) >= 1);

    // 2. Away for 50 ms (a long callback, well past the 10 ms lease): a timer due meanwhile is served by the
    //    poll thread, on time.
    atomic_store(&fired_at_ns, 0);
    due = tt_get_ns() + (AWAY_TIMER_DUE_MS * MS); // after the 10 ms lease: the poll thread has it by then
    assert(tt_Node_schedule(tickle_node, due, record_firing, NULL));
    sleep_ms(AWAY_MS);
    assert(atomic_load(&fired_at_ns) >= due && atomic_load(&fired_at_ns) <= due + (2 * MS));
    assert(atomic_load(&fired_on_thread) != main_thread);

    // 3. A guard condition triggered from another thread 20 ms into a 2 s wait ends it within a few ms.
    storage[0] = guard;
    struct trigger_later later = {guard, TRIGGER_AFTER_MS};
    pthread_t trigger; // NOLINT(misc-include-cleaner) - <pthread.h> above; glibc declares it via a private header
    uint64_t start = tt_get_ns();
    assert(0 == pthread_create(&trigger, NULL, trigger_after, &later));
    rmw_time_t long_timeout = {2, 0};
    assert(RMW_RET_OK == rmw_wait(NULL, &guards, NULL, NULL, NULL, wait_set, &long_timeout));
    uint64_t waited = tt_get_ns() - start;
    assert(waited >= TRIGGER_AFTER_MS * MS && waited <= (TRIGGER_AFTER_MS + WOKEN_WITHIN_MS) * MS);
    assert(0 == pthread_join(trigger, NULL));

    // 4. Two waits at once on their own guard conditions and wait sets: both end when triggered.
    rmw_guard_condition_t* other_guard = rmw_create_guard_condition(&context);
    rmw_wait_set_t* other_wait_set = rmw_create_wait_set(&context, 0);
    struct waiter first = {wait_set, guard, RMW_RET_ERROR};
    struct waiter second = {other_wait_set, other_guard, RMW_RET_ERROR};
    pthread_t first_thread;  // NOLINT(misc-include-cleaner) - as above
    pthread_t second_thread; // NOLINT(misc-include-cleaner) - as above
    assert(0 == pthread_create(&first_thread, NULL, wait_on_guard, &first));
    assert(0 == pthread_create(&second_thread, NULL, wait_on_guard, &second));
    sleep_ms(TRIGGER_AFTER_MS);
    start = tt_get_ns();
    assert(RMW_RET_OK == rmw_trigger_guard_condition(guard));
    assert(RMW_RET_OK == rmw_trigger_guard_condition(other_guard));
    assert(0 == pthread_join(first_thread, NULL));
    assert(0 == pthread_join(second_thread, NULL));
    assert(RMW_RET_OK == first.result && RMW_RET_OK == second.result);
    assert(tt_get_ns() - start <= BOTH_WITHIN_MS * MS);

    assert(RMW_RET_OK == rmw_destroy_wait_set(other_wait_set));
    assert(RMW_RET_OK == rmw_destroy_guard_condition(other_guard));
    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_guard_condition(guard));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("executor-driven receive: PASS\n");
    return 0;
}
