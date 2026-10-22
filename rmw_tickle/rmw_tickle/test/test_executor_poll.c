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
// The hold-over (2026-10-20, rmw_tickle.h): an rmw_wait() that polled and found something keeps the role.
//   5. if its caller then stays away, the watchdog gives the role to the poll thread: a timer due meanwhile fires
//      there, within the lease plus one watchdog slice;
//   6. if its caller comes straight back, the next rmw_wait() resumes the role: no hand-over, nothing ended;
//   7. a zero-timeout rmw_wait() that finds nothing ends a hold-over at once (nothing polls in such a wait);
//   8. another thread's rmw_wait() takes a hold-over over and polls, rather than waiting behind it.
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
//
// The hold-over parts are judged against how long this thread was really away (2026-10-21). A hold-over is ended by
// the watchdog once it is RMW_TICKLE_EXECUTOR_POLL_LEASE_NS old, which is the point of part 5 - and in parts 5-8 the
// lease also runs between rmw_wait() storing holdover_since_ns and this thread's next step, which the test meant to be
// "straight back". Under make check-gates (load 21-36) that step came 10+ ms later: part 6 then found the hold-over
// ended by the watchdog and the next wait handing over, and failed with "executor_handovers == handovers" although
// the product did what the lease says (2026-10-20). So an attempt reads the realized away time - from
// holdover_since_ns to a clock read after the step - and:
//   - a hold-over gone before the lease was over is a FAIL (only the lease ends one here);
//   - one the watchdog may have ended because the step came a lease or more late is VOID, not a result: the attempt
//     did not test what it was for, and is retried, up to VOID_TRIES; still VOID is a failure, never a pass;
//   - otherwise every assertion holds as before.
//
// Parts 6 and 8 start from a parked poll thread, and wait for it (2026-10-22). Part 6 failed once more under make
// check-gates, on "executor_handovers == handovers", with no VOID: the resumed wait had found core's poll taken and
// interrupted it. Its hold-over came from a wait that found the guard condition triggered at once and so never polled,
// after a 1 ms wait meant to leave the poll thread parked; when the poll thread is slow to return from the poll that
// wait interrupted (more than the 1 ms the wait gives it to park), it is still in that poll during the hold-over, and
// the hand-over is the product's correct answer to that, not a failed resume. Reproduced off the gate: 2 in 300 runs
// pinned to one CPU, and 20 in 20 with a 3 ms sleep injected into an interrupted tt_Context_poll() before it returns.
// So those parts now take the hold-over from a wait that polls until another thread triggers it - which interrupts a
// poll thread still in core's poll and, holding the role throughout, keeps it from polling again - and then wait until
// the poll thread reports itself parked, rather than assume a fixed sleep was enough (hold_over_parked()).

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
#define TRIGGER_AFTER_MS 20U     // parts 3, 4, 6 and 8: when the other thread triggers
#define WOKEN_WITHIN_MS 5U       // part 3: how soon after that the wait must end
#define BOTH_WITHIN_MS 10U       // part 4: how soon both waits must end
#define ON_TIME_MS 2U            // parts 1 and 2: how late a timer may fire, on the best attempt
#define ATTEMPTS 5               // attempts per part; the tight bound must hold on at least one
#define WAITED_FOR_GOOD_MS 1000U // parts 2-4: past this, nothing was going to happen (asserted on every attempt)
#define HOLDOVER_AWAY_MS 100U    // part 5: the absence after a hold-over
// Part 5: how late the timer may fire - the hold-over ends at the first watchdog slice past the lease, at most
// lease + slice (10 + 50 ms) after it began, and the timer was due AWAY_TIMER_DUE_MS in; plus ON_TIME_MS.
#define HOLDOVER_LATE_MS (10U + 50U - AWAY_TIMER_DUE_MS + ON_TIME_MS)
#define HOLDOVER_TRIGGER_MS 50U // part 8: when this thread triggers the other thread's wait
#define VOID_TRIES 20           // parts 6 and 7: attempts until one was not VOID
#define NOT_DECIDED UINT64_MAX  // a timed attempt that was VOID: best_of() counts it as a miss and tries again
#define PARK_CHECK_US 100U      // parts 6 and 8: how often to look whether the poll thread has parked
#define US_PER_MS 1000U
#define NS_PER_US 1000L

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

static void sleep_us(unsigned micros) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = (long)micros * NS_PER_US};
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
    _Atomic uintptr_t thread; // the waiting thread's tt_thread_self()
};

static void* wait_on_guard(void* arg) {
    struct waiter* waiter = arg;
    atomic_store(&waiter->thread, tt_thread_self());
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

// After an rmw_wait() that found its guard condition triggered: whether that wait still holds the role over. The
// holder is read before the clock, so a hold-over found gone was gone by `now`: only the watchdog may have ended it,
// and only a lease or more after it began - asserted, as anything else is a defect. Then the attempt is VOID (this
// thread came back too late to test what it meant to), and this prints why.
static bool still_held_over(const struct fixture* fix, uint64_t ended_before, const char* part) {
    bool held = atomic_load(&fix->impl->holdover_thread) == fix->main_thread;
    uint64_t now = tt_get_ns();
    if (held) {
        return true;
    }
    uint64_t away = now - atomic_load(&fix->impl->holdover_since_ns);
    assert(atomic_load(&fix->impl->holdovers_ended) > ended_before);
    assert(away >= RMW_TICKLE_EXECUTOR_POLL_LEASE_NS);
    printf("%s: VOID (this thread looked %.3f ms after the hold-over began, past the %.0f ms lease, and the watchdog "
           "had ended it)\n",
           part, (double)away / (double)MS, (double)RMW_TICKLE_EXECUTOR_POLL_LEASE_NS / (double)MS);
    (void)fflush(stdout);
    return false;
}

// Parts 6 and 8: holds the role over from a wait that polled - another thread triggers the guard condition
// TRIGGER_AFTER_MS into it - and returns once the poll thread is parked; false if VOID (the watchdog ended the
// hold-over first, still_held_over()). That wait held the role from its start, so a poll thread still in core's poll
// was interrupted and found busy until it left; with the role still taken it parks and stays parked.
static bool hold_over_parked(const struct fixture* fix, uint64_t ended, const char* part) {
    struct trigger_later later = {fix->guard, TRIGGER_AFTER_MS};
    pthread_t trigger; // NOLINT(misc-include-cleaner) - as above
    assert(0 == pthread_create(&trigger, NULL, trigger_after, &later));
    assert(RMW_RET_OK == wait_untriggered(fix, WAITED_FOR_GOOD_MS * MS)); // polls until the trigger
    assert(0 == pthread_join(trigger, NULL));
    for (unsigned waited = 0;
         !atomic_load(&fix->impl->poll_thread_parked) && atomic_load(&fix->impl->holdover_thread) == fix->main_thread &&
         waited < WAITED_FOR_GOOD_MS * US_PER_MS;
         waited += PARK_CHECK_US) {
        sleep_us(PARK_CHECK_US);
    }
    if (!still_held_over(fix, ended, part)) {
        return false;
    }
    assert(atomic_load(&fix->impl->poll_thread_parked)); // held over all along, and still not parked: a defect
    return true;
}

// Part 5, one attempt: a wait that finds the guard condition triggered keeps the role, and its caller stays away;
// returns how late a timer due meanwhile fired, in ns, or NOT_DECIDED if the hold-over was over before the timer was
// scheduled.
static uint64_t timer_after_holdover(const struct fixture* fix) {
    uint64_t kept = atomic_load(&fix->impl->holdovers);
    uint64_t ended = atomic_load(&fix->impl->holdovers_ended);
    assert(RMW_RET_OK == rmw_trigger_guard_condition(fix->guard));
    assert(RMW_RET_OK == wait_untriggered(fix, WAITED_FOR_GOOD_MS * MS)); // ready at once
    assert(atomic_load(&fix->impl->holdovers) == kept + 1);
    atomic_store(&fired_at_ns, 0);
    atomic_store(&fired_on_thread, 0);
    uint64_t due = tt_get_ns() + (AWAY_TIMER_DUE_MS * MS);
    assert(tt_Context_schedule(&fix->impl->tickle_context, due, record_firing, NULL));
    bool held = still_held_over(fix, ended, "5. timer after a hold-over"); // the timer went in during the hold-over
    sleep_ms(HOLDOVER_AWAY_MS);
    for (unsigned waited = HOLDOVER_AWAY_MS; atomic_load(&fired_at_ns) == 0 && waited < WAITED_FOR_GOOD_MS; waited++) {
        sleep_ms(1);
    }
    uint64_t fired = atomic_load(&fired_at_ns);
    assert(fired != 0 && fired >= due);
    assert(atomic_load(&fired_on_thread) != fix->main_thread);
    assert(atomic_load(&fix->impl->holdover_thread) == 0);
    assert(atomic_load(&fix->impl->holdovers_ended) == ended + 1); // the watchdog's doing, not a caller's
    return held ? fired - due : NOT_DECIDED;
}

// Part 6, one attempt: a caller that comes straight back resumes its hold-over - no hand-over, nothing ended - and a
// wait that then times out releases the role as before. False if VOID: the caller came back a lease or more after
// the hold-over began, and the watchdog had ended it (the lease, which part 5 checks, not a resume).
static bool holdover_resumed(const struct fixture* fix) {
    const char* part = "6. hold-over resumed";
    // The hold-over begins with the poll thread parked (part 5 left it polling), so a hand-over below is a resume that
    // did not resume.
    uint64_t ended = atomic_load(&fix->impl->holdovers_ended);
    if (!hold_over_parked(fix, ended, part)) {
        return false;
    }
    uint64_t handovers = atomic_load(&fix->impl->executor_handovers);
    uint64_t since = atomic_load(&fix->impl->holdover_since_ns);
    rmw_ret_t resumed = wait_untriggered(fix, 1 * MS); // resumed, then released
    uint64_t back = tt_get_ns() - since;               // the resume, if any, was before this
    assert(RMW_RET_TIMEOUT == resumed);
    if (atomic_load(&fix->impl->holdovers_ended) != ended && back >= RMW_TICKLE_EXECUTOR_POLL_LEASE_NS) {
        printf("%s: VOID (this thread was back within %.3f ms of the hold-over, past the lease, and the watchdog had "
               "ended it)\n",
               part, (double)back / (double)MS);
        (void)fflush(stdout);
        return false;
    }
    assert(atomic_load(&fix->impl->holdover_thread) == 0);
    assert(!atomic_load(&fix->impl->executor_polling));
    assert(atomic_load(&fix->impl->executor_handovers) == handovers);
    assert(atomic_load(&fix->impl->holdovers_ended) == ended);
    printf("%s: back within %.3f ms, no hand-over, nothing ended\n", part, (double)back / (double)MS);
    return true;
}

// Part 7, one attempt: a zero-timeout wait that finds nothing ends a hold-over at once. False if VOID: the zero wait
// came a lease or more after the hold-over began, so the watchdog may have been the one that ended it.
static bool holdover_ended_by_zero_wait(const struct fixture* fix) {
    const char* part = "7. hold-over ended by a zero wait";
    uint64_t ended = atomic_load(&fix->impl->holdovers_ended);
    assert(RMW_RET_OK == rmw_trigger_guard_condition(fix->guard));
    assert(RMW_RET_OK == wait_untriggered(fix, WAITED_FOR_GOOD_MS * MS));
    if (!still_held_over(fix, ended, part)) {
        return false;
    }
    uint64_t since = atomic_load(&fix->impl->holdover_since_ns);
    assert(RMW_RET_TIMEOUT == wait_untriggered(fix, 0));
    uint64_t back = tt_get_ns() - since;
    assert(atomic_load(&fix->impl->holdover_thread) == 0);
    assert(!atomic_load(&fix->impl->executor_polling));
    assert(atomic_load(&fix->impl->holdovers_ended) == ended + 1); // one end, by whichever ended it
    if (back >= RMW_TICKLE_EXECUTOR_POLL_LEASE_NS) {
        printf("%s: VOID (the zero wait was done %.3f ms after the hold-over began, past the lease: the watchdog may "
               "have ended it)\n",
               part, (double)back / (double)MS);
        (void)fflush(stdout);
        return false;
    }
    printf("%s: ended within %.3f ms\n", part, (double)back / (double)MS);
    return true;
}

// Part 8, one attempt: this thread holds the role over, another thread's wait takes it over and polls - a timer due
// during that wait fires on the other thread; returns how late, in ns, or NOT_DECIDED if the watchdog ended the
// hold-over before the other thread took it (a take-over clears it without counting an end).
static uint64_t holdover_taken_over(const struct fixture* fix) {
    const char* part = "8. hold-over taken over";
    uint64_t ended = atomic_load(&fix->impl->holdovers_ended);
    // From a parked poll thread (part 7's zero wait woke it): the take-over polls without interrupting anyone first.
    if (!hold_over_parked(fix, ended, part)) {
        return NOT_DECIDED;
    }
    rmw_guard_condition_t* other_guard = rmw_create_guard_condition(fix->context);
    rmw_wait_set_t* other_wait_set = rmw_create_wait_set(fix->context, 0);
    struct waiter other = {other_wait_set, other_guard, RMW_RET_ERROR, 0};
    atomic_store(&fired_at_ns, 0);
    atomic_store(&fired_on_thread, 0);
    uint64_t due = tt_get_ns() + (AWAY_TIMER_DUE_MS * MS);
    assert(tt_Context_schedule(&fix->impl->tickle_context, due, record_firing, NULL));
    pthread_t thread; // NOLINT(misc-include-cleaner) - as above
    assert(0 == pthread_create(&thread, NULL, wait_on_guard, &other));
    sleep_ms(HOLDOVER_TRIGGER_MS);
    assert(RMW_RET_OK == rmw_trigger_guard_condition(other_guard));
    assert(0 == pthread_join(thread, NULL));
    assert(RMW_RET_OK == other.result);
    uint64_t fired = atomic_load(&fired_at_ns);
    assert(fired != 0 && fired >= due);
    bool taken_over = atomic_load(&fix->impl->holdovers_ended) == ended;
    if (taken_over) {
        assert(atomic_load(&fired_on_thread) == atomic_load(&other.thread)); // the taker polled
    } else {
        // The other thread started a lease or more after the hold-over began (the 50 ms sleep is past it anyway):
        // it found the role ended, not held over.
        assert(tt_get_ns() - atomic_load(&fix->impl->holdover_since_ns) >= RMW_TICKLE_EXECUTOR_POLL_LEASE_NS);
        printf("%s: VOID (the watchdog ended the hold-over before the other thread's wait took it)\n", part);
        (void)fflush(stdout);
    }
    assert(RMW_RET_OK == rmw_destroy_wait_set(other_wait_set));
    assert(RMW_RET_OK == rmw_destroy_guard_condition(other_guard));
    return taken_over ? fired - due : NOT_DECIDED;
}

// Runs one part's attempts until one is within bound_ns, at most ATTEMPTS of them, printing each figure; true if
// one was. A VOID attempt (NOT_DECIDED) is a miss.
static bool best_of(const char* part, uint64_t (*attempt)(const struct fixture*), const struct fixture* fix,
                    uint64_t bound_ns) {
    for (int i = 1; i <= ATTEMPTS; i++) {
        uint64_t late = attempt(fix);
        if (NOT_DECIDED == late) {
            printf("%s: attempt %d: VOID\n", part, i);
        } else {
            printf("%s: attempt %d: %.3f ms (bound %.3f ms)\n", part, i, (double)late / (double)MS,
                   (double)bound_ns / (double)MS);
        }
        (void)fflush(stdout); // an assert() that follows aborts, and would take the figures with it
        if (late <= bound_ns) {
            return true;
        }
    }
    return false;
}

// Runs an untimed part until one attempt is not VOID, at most VOID_TRIES; false if every one was.
static bool until_decided(bool (*attempt)(const struct fixture*), const struct fixture* fix) {
    for (int i = 1; i <= VOID_TRIES; i++) {
        if (attempt(fix)) {
            return true;
        }
    }
    return false;
}

int main(void) {
    assert(0 == setenv("RMW_TICKLE_EXECUTOR_POLL", "1", 1));
    assert(0 == setenv("RMW_TICKLE_EXECUTOR_HOLDOVER", "1", 1));
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

    // 5-8. The hold-over.
    assert(impl->executor_holdover_enabled);
    assert(best_of("5. timer after a hold-over", timer_after_holdover, &fix, HOLDOVER_LATE_MS * MS));
    assert(until_decided(holdover_resumed, &fix));
    assert(until_decided(holdover_ended_by_zero_wait, &fix));
    assert(best_of("8. hold-over taken over", holdover_taken_over, &fix, ON_TIME_MS * MS));

    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_guard_condition(guard));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("executor-driven receive: PASS\n");
    return 0;
}
