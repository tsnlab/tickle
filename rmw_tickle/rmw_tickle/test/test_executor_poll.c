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
//
// How "on time" is checked (2026-10-06). Each part used to assert a wall-clock bound - fired within 2 ms of its
// due time - once. Under PC load (check-gates runs its rows in parallel) a thread can wait longer than that to be
// scheduled, and part 1 failed once and passed on two reruns: the bound measured the machine, not the code. So each
// part now separates the two kinds of claim:
//   - what load cannot change is asserted on every attempt: the timer fired, not before it was due, on the right
//     thread, and while the wait it belongs to was still going on (well before the wait's own end, which is what a
//     poll that is not shortened to the next timer produces: late by the rest of the wait, 250 ms here);
//   - the tight bound is asserted on the best of ATTEMPTS attempts. A defect is late in every attempt; a scheduling
//     delay from load is late in some. Five attempts that each miss by a transient delay are the only false failure,
//     and each attempt prints its figure so a failure shows which kind it was.

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
#define TIMER_DUE_MS 50U         // part 1: a timer this far into the wait
#define WAIT_MS 300U             // part 1: the wait
#define AWAY_TIMER_DUE_MS 20U    // part 2: a timer this far into the executor's absence, past the lease
#define AWAY_MS 50U              // part 2: the absence
#define TRIGGER_AFTER_MS 20U     // parts 3 and 4: when the other thread triggers
#define WOKEN_WITHIN_MS 5U       // part 3: how soon after that the wait must end
#define BOTH_WITHIN_MS 10U       // part 4: how soon both waits must end
#define ON_TIME_MS 2U            // parts 1 and 2: how late a timer may fire, on the best attempt
#define ATTEMPTS 5               // attempts per part; the tight bound must hold on at least one
#define WAITED_FOR_GOOD_MS 1000U // parts 2-4: past this, nothing was going to happen (asserted on every attempt)

static _Atomic uint64_t fired_at_ns;
static _Atomic uintptr_t fired_on_thread;

static void record_firing(struct tt_Context* node, uint64_t time, void* param) {
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

struct fixture {
    rmw_tickle_context_impl_t* impl;
    rmw_context_t* context;
    rmw_guard_condition_t* guard;
    rmw_wait_set_t* wait_set;
    uintptr_t main_thread;
};

// Runs a wait that times out, with the guard condition in it (never triggered here); the wait set finalizes
// its entries, so the array is filled again every time.
static rmw_ret_t wait_untriggered(const struct fixture* fix, uint64_t wait_ns) {
    void* storage[1] = {fix->guard};
    rmw_guard_conditions_t guards = {.guard_condition_count = 1, .guard_conditions = storage};
    rmw_time_t timeout = {wait_ns / (MS_PER_S * MS), wait_ns % (MS_PER_S * MS)};
    return rmw_wait(NULL, &guards, NULL, NULL, NULL, fix->wait_set, &timeout);
}

// Part 1, one attempt: returns how late the timer fired, in ns.
static uint64_t timer_during_wait(const struct fixture* fix) {
    atomic_store(&fired_at_ns, 0);
    atomic_store(&fired_on_thread, 0);
    uint64_t due = tt_get_ns() + (TIMER_DUE_MS * MS);
    assert(tt_Context_schedule(&fix->impl->tickle_context, due, record_firing, NULL));
    assert(RMW_RET_TIMEOUT == wait_untriggered(fix, WAIT_MS * MS));
    uint64_t returned = tt_get_ns();
    uint64_t fired = atomic_load(&fired_at_ns);
    assert(fired >= due && fired < returned);
    assert((fired - due) < ((WAIT_MS - TIMER_DUE_MS) * MS) / 2);
    assert(atomic_load(&fired_on_thread) == fix->main_thread);
    return fired - due;
}

// Part 2, one attempt: the executor takes the role and leaves it, then stays away; returns how late the timer
// fired, in ns.
static uint64_t timer_while_away(const struct fixture* fix) {
    assert(RMW_RET_TIMEOUT == wait_untriggered(fix, 1 * MS)); // holds the role, then leaves: the lease starts
    atomic_store(&fired_at_ns, 0);
    atomic_store(&fired_on_thread, 0);
    uint64_t due = tt_get_ns() + (AWAY_TIMER_DUE_MS * MS); // after the 10 ms lease: the poll thread has it by then
    assert(tt_Context_schedule(&fix->impl->tickle_context, due, record_firing, NULL));
    sleep_ms(AWAY_MS);
    // Nothing on this thread polls from here on, so whatever fires it is the poll thread - however long load
    // delays it.
    for (unsigned waited = AWAY_MS; atomic_load(&fired_at_ns) == 0 && waited < WAITED_FOR_GOOD_MS; waited++) {
        sleep_ms(1);
    }
    uint64_t fired = atomic_load(&fired_at_ns);
    assert(fired != 0 && fired >= due);
    assert(atomic_load(&fired_on_thread) != fix->main_thread);
    return fired - due;
}

// Part 3, one attempt: returns how long after the trigger the wait ended, in ns.
static uint64_t woken_by_trigger(const struct fixture* fix) {
    void* storage[1] = {fix->guard};
    rmw_guard_conditions_t guards = {.guard_condition_count = 1, .guard_conditions = storage};
    struct trigger_later later = {fix->guard, TRIGGER_AFTER_MS};
    pthread_t trigger; // NOLINT(misc-include-cleaner) - <pthread.h> above; glibc declares it via a private header
    uint64_t start = tt_get_ns();
    assert(0 == pthread_create(&trigger, NULL, trigger_after, &later));
    rmw_time_t long_timeout = {2, 0};
    assert(RMW_RET_OK == rmw_wait(NULL, &guards, NULL, NULL, NULL, fix->wait_set, &long_timeout));
    uint64_t waited = tt_get_ns() - start;
    assert(0 == pthread_join(trigger, NULL));
    assert(waited >= TRIGGER_AFTER_MS * MS && waited < WAITED_FOR_GOOD_MS * MS);
    return waited - (TRIGGER_AFTER_MS * MS);
}

// Part 4, one attempt: returns how long after the triggers both waits had ended, in ns.
static uint64_t both_woken(const struct fixture* fix) {
    rmw_guard_condition_t* other_guard = rmw_create_guard_condition(fix->context);
    rmw_wait_set_t* other_wait_set = rmw_create_wait_set(fix->context, 0);
    struct waiter first = {fix->wait_set, fix->guard, RMW_RET_ERROR};
    struct waiter second = {other_wait_set, other_guard, RMW_RET_ERROR};
    pthread_t first_thread;  // NOLINT(misc-include-cleaner) - as above
    pthread_t second_thread; // NOLINT(misc-include-cleaner) - as above
    assert(0 == pthread_create(&first_thread, NULL, wait_on_guard, &first));
    assert(0 == pthread_create(&second_thread, NULL, wait_on_guard, &second));
    sleep_ms(TRIGGER_AFTER_MS);
    uint64_t start = tt_get_ns();
    assert(RMW_RET_OK == rmw_trigger_guard_condition(fix->guard));
    assert(RMW_RET_OK == rmw_trigger_guard_condition(other_guard));
    assert(0 == pthread_join(first_thread, NULL));
    assert(0 == pthread_join(second_thread, NULL));
    uint64_t took = tt_get_ns() - start;
    assert(RMW_RET_OK == first.result && RMW_RET_OK == second.result);
    assert(took < WAITED_FOR_GOOD_MS * MS);
    assert(RMW_RET_OK == rmw_destroy_wait_set(other_wait_set));
    assert(RMW_RET_OK == rmw_destroy_guard_condition(other_guard));
    return took;
}

// Runs one part's attempts until one is within bound_ns, at most ATTEMPTS of them, printing each figure; true if
// one was.
static bool best_of(const char* part, uint64_t (*attempt)(const struct fixture*), const struct fixture* fix,
                    uint64_t bound_ns) {
    for (int i = 1; i <= ATTEMPTS; i++) {
        uint64_t late = attempt(fix);
        printf("%s: attempt %d: %.3f ms (bound %.3f ms)\n", part, i, (double)late / (double)MS,
               (double)bound_ns / (double)MS);
        (void)fflush(stdout); // an assert() that follows aborts, and would take the figures with it
        if (late <= bound_ns) {
            return true;
        }
    }
    return false;
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

    rmw_guard_condition_t* guard = rmw_create_guard_condition(&context);
    rmw_wait_set_t* wait_set = rmw_create_wait_set(&context, 0);
    struct fixture fix = {impl, &context, guard, wait_set, tt_thread_self()};

    // 1. A timer due 50 ms into a 300 ms wait fires then, on this thread: the executor holds the role.
    assert(best_of("1. timer during the wait", timer_during_wait, &fix, ON_TIME_MS * MS));
    assert(atomic_load(&impl->executor_poll_waits) >= 1);

    // 2. Away for 50 ms (a long callback, well past the 10 ms lease): a timer due meanwhile is served by the
    //    poll thread, on time.
    assert(best_of("2. timer while away", timer_while_away, &fix, ON_TIME_MS * MS));

    // 3. A guard condition triggered from another thread 20 ms into a 2 s wait ends it within a few ms.
    assert(best_of("3. woken by a trigger", woken_by_trigger, &fix, WOKEN_WITHIN_MS * MS));

    // 4. Two waits at once on their own guard conditions and wait sets: both end when triggered.
    assert(best_of("4. both waits woken", both_woken, &fix, BOTH_WITHIN_MS * MS));

    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_guard_condition(guard));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("executor-driven receive: PASS\n");
    return 0;
}
