// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// A scheduled entry runs at its deadline, against the real Linux HAL, run by test.sh (test-linux) in its namespace.
//
// Real failure (2026-10-17): the core ping client (examples/perf_hil/tickle/reliable_latency) sent each ping ~0.54 ms
// after the time tt_Context_schedule() was given for it, at a 1 s interval, where the DDS clients - pacing with
// clock_nanosleep() - sent theirs within the default 50 us timer slack. The poll loop waits for the next entry with a
// relative timeout (epoll_pwait2() or ppoll()), and Linux lets those end up to 0.1% of the timeout late (0.5% for a
// task with a positive nice value): select_estimate_accuracy() gives the wait's hrtimer a slack proportional to its
// length, where clock_nanosleep() and a timerfd take the thread's fixed timer slack (50 us by default) or none.
//
// The check, in one context with no peer:
//   tickle   - schedule an entry INTERVAL ahead, poll with timeout -1 until it has run, and record how late it ran:
//              the clock read in the entry minus the time it was scheduled for.
//   earlier  - as tickle, but the poll first waits for an entry 4 intervals ahead, and another thread then schedules
//              one INTERVAL ahead (2026-10-17, with the deadline-timer design: the wait must move its timer in).
//   control  - clock_nanosleep(TIMER_ABSTIME) to the same kind of deadline: the lateness this host gives a deadline
//              whose slack does not grow with the wait (the DDS clients' pacing).
//   positive - at the long intervals, a bare epoll_pwait2() with the same relative timeout: the defect itself, which
//              must show (more than 2 x LATE_MARGIN_NS past the control) for a pass to mean anything.
// The 1 ms case runs after the 500 ms one, so its waits start with a fired timer: a fire left pending would make
// each of them return at once and spin.
// Reading rules, enforced below and fixed before the first run:
//   PASS  for every interval, median(tickle) <= median(control) + LATE_MARGIN_NS, and the thread's CPU time per entry
//         <= CPU_PER_WAKE_NS - a precise wake must not be bought by spinning to the deadline.
//   FAIL  either bound broken at any interval. On main (2026-10-17, PC, nice 0) the 500 ms entries ran ~513 us late
//         against a control of ~70-105 us; that is the failure this exists to catch. The long interval was 300 ms in
//         the first version, where main failed by only ~80 us at nice 0 (346 us against a control of 116): too thin
//         a margin to trust on a CI host, so it was lengthened; the bounds themselves are as first written.
//   VOID  the control's own median above CONTROL_VOID_NS at any interval: the host is too loaded for a lateness of
//         a few hundred microseconds to be told from its scheduling delay; or the positive control not showing the
//         slack (exit 2, not a pass). Added after a loaded PC (load 10-25) let the no-long-rule mutant pass.
// --sweep runs more intervals and, beside the two arms, the raw waits the poll loop and the alternatives use, with no
// context - epoll_pwait2() and ppoll() with a relative timeout, a timerfd armed for the absolute deadline - and prints
// their lateness, which says which component the tickle arm's lateness comes from. The sweep prints and judges nothing.
//
// Exit 0 pass, 1 fail, 2 void, 3 setup failed.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming) - ppoll(), RUSAGE_THREAD
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/timerfd.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#define US_NS 1000ULL
#define MS_NS 1000000ULL
#define S_NS 1000000000ULL
#define LATE_MARGIN_NS (150ULL * US_NS)   // tickle's median may exceed the control's by this much
#define CONTROL_VOID_NS (1000ULL * US_NS) // a control median above this voids the run
#define CPU_PER_WAKE_NS (500ULL * US_NS)  // thread CPU per entry, user + system; ~20-90 us when it sleeps
#define MAX_SAMPLES 64
#define POSITIVE_CONTROL_FROM_NS (100ULL * MS_NS) // judged intervals from here on carry the positive control
#define CHECK_CONTEXT_ID 59                       // any valid id; this context never meets another

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_VOID = 2, CHECK_SETUP_FAILED = 3 };

struct interval {
    uint64_t ns;
    int samples;
};

enum arm { ARM_TICKLE, ARM_EARLIER, ARM_NANOSLEEP, ARM_EPOLL, ARM_PPOLL, ARM_TIMERFD, ARM_COUNT };

struct judged_case {
    struct interval when;
    enum arm arm;
};

// The judged cases: a short interval, where the relative wait's slack (1 us) is below the thread's 50 us; a long one,
// where 0.1% of it (500 us) is ten times that; and an entry another thread schedules EARLIER than the one the poll
// is already waiting for (ARM_EARLIER, added 2026-10-17 with the deadline-timer design: the wait must move its
// deadline in, not sleep on to the one it armed first).
// The short interval runs after the long one, so that its waits start with the long one's timer fired: a fire left
// pending would end each of them at once, and the wait would spin, which the CPU bound catches.
static const struct judged_case judged[] = {{{500 * MS_NS, 5}, ARM_TICKLE},
                                            {{1 * MS_NS, 20}, ARM_TICKLE},
                                            {{500 * MS_NS, 4}, ARM_EARLIER}};
static const struct interval swept[] = {{1 * MS_NS, 40},  {5 * MS_NS, 30},  {10 * MS_NS, 30}, {50 * MS_NS, 12},
                                        {100 * MS_NS, 8}, {200 * MS_NS, 8}, {500 * MS_NS, 6}, {1000 * MS_NS, 5}};

static uint64_t thread_cpu_ns(void) {
    struct rusage usage; // NOLINT(misc-include-cleaner) - <sys/resource.h>
    if (getrusage(RUSAGE_THREAD, &usage) != 0) {
        return 0;
    }
    return ((uint64_t)usage.ru_utime.tv_sec * S_NS) + ((uint64_t)usage.ru_utime.tv_usec * US_NS) +
           ((uint64_t)usage.ru_stime.tv_sec * S_NS) + ((uint64_t)usage.ru_stime.tv_usec * US_NS);
}

static int compare_u64(const void* a, const void* b) {
    uint64_t left = *(const uint64_t*)a;
    uint64_t right = *(const uint64_t*)b;
    return (left > right) - (left < right);
}

static uint64_t median(uint64_t* values, int count) {
    qsort(values, (size_t)count, sizeof values[0], compare_u64);
    return values[count / 2];
}

static uint64_t maximum(const uint64_t* values, int count) {
    uint64_t most = 0;
    for (int i = 0; i < count; i++) {
        most = values[i] > most ? values[i] : most;
    }
    return most;
}

static void to_timespec(uint64_t when_ns, struct timespec* ts) {
    ts->tv_sec = (time_t)(when_ns / S_NS);
    ts->tv_nsec = (long)(when_ns % S_NS);
}

// ---- tickle: an entry scheduled INTERVAL ahead, run by tt_Context_poll(-1)

static uint64_t g_ran_at = 0;

static void entry(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_ran_at = tt_get_ns();
}

static bool arm_tickle(struct tt_Context* node, uint64_t interval, uint64_t* late) {
    g_ran_at = 0;
    const uint64_t due = tt_get_ns() + interval;
    if (!tt_Context_schedule(node, due, entry, NULL)) {
        return false;
    }
    while (g_ran_at == 0) {
        (void)tt_Context_poll(node, -1);
    }
    *late = g_ran_at - due;
    return true;
}

// ---- earlier: the poll waits for an entry EARLIER_FAR_FACTOR intervals ahead; after EARLIER_AFTER_NS another thread
// schedules one INTERVAL ahead, which wakes the poll (tt_Context_schedule()); the late one is judged.

#define EARLIER_AFTER_NS (50 * MS_NS)
#define EARLIER_FAR_FACTOR 4U

struct earlier_job {
    struct tt_Context* node;
    uint64_t interval;
    uint64_t due; // 0: not scheduled
};

static void far_entry(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
}

static void* schedule_earlier(void* arg) {
    struct earlier_job* job = arg;
    struct timespec pause;
    to_timespec(EARLIER_AFTER_NS, &pause);
    (void)nanosleep(&pause, NULL);
    const uint64_t due = tt_get_ns() + job->interval;
    job->due = due; // read by the polling thread only after pthread_join()
    if (!tt_Context_schedule(job->node, due, entry, NULL)) {
        job->due = 0;
    }
    return NULL;
}

static bool arm_earlier(struct tt_Context* node, uint64_t interval, uint64_t* late) {
    g_ran_at = 0;
    if (!tt_Context_schedule(node, tt_get_ns() + (EARLIER_FAR_FACTOR * interval), far_entry, NULL)) {
        return false;
    }
    struct earlier_job job = {node, interval, 0};
    pthread_t helper; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    if (pthread_create(&helper, NULL, schedule_earlier, &job) != 0) {
        return false;
    }
    while (g_ran_at == 0) {
        (void)tt_Context_poll(node, -1);
    }
    (void)pthread_join(helper, NULL);
    (void)tt_Context_unschedule(node, far_entry, NULL);
    if (job.due == 0) {
        return false;
    }
    *late = g_ran_at - job.due;
    return true;
}

// ---- control: clock_nanosleep() to the absolute deadline

static bool arm_nanosleep(uint64_t interval, uint64_t* late) {
    const uint64_t due = tt_get_ns() + interval;
    struct timespec until;
    to_timespec(due, &until);
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_MONOTONIC, TIMER_ABSTIME: <time.h>
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR) {
    }
    *late = tt_get_ns() - due;
    return true;
}

// ---- the raw waits (--sweep only): what the poll loop's wait and its alternatives give the same deadline

static bool arm_epoll_relative(int epoll_fd, uint64_t interval, uint64_t* late) {
    const uint64_t due = tt_get_ns() + interval;
    for (;;) {
        const uint64_t now = tt_get_ns();
        if (now >= due) {
            break;
        }
        struct timespec rest;
        to_timespec(due - now, &rest);
        struct epoll_event event;
        if (epoll_pwait2(epoll_fd, &event, 1, &rest, NULL) < 0 && errno != EINTR) {
            return false;
        }
    }
    *late = tt_get_ns() - due;
    return true;
}

static bool arm_ppoll_relative(int idle_fd, uint64_t interval, uint64_t* late) {
    const uint64_t due = tt_get_ns() + interval;
    for (;;) {
        const uint64_t now = tt_get_ns();
        if (now >= due) {
            break;
        }
        struct timespec rest;
        to_timespec(due - now, &rest);
        struct pollfd pfd = {.fd = idle_fd, .events = POLLIN, .revents = 0}; // NOLINT(misc-include-cleaner) - <poll.h>
        if (ppoll(&pfd, 1, &rest, NULL) < 0 && errno != EINTR) {             // NOLINT(misc-include-cleaner) - <poll.h>
            return false;
        }
    }
    *late = tt_get_ns() - due;
    return true;
}

static bool arm_timerfd_absolute(int epoll_fd, int timer_fd, uint64_t interval, uint64_t* late) {
    const uint64_t due = tt_get_ns() + interval;
    struct itimerspec deadline = {{0, 0}, {0, 0}}; // NOLINT(misc-include-cleaner) - <sys/timerfd.h>
    to_timespec(due, &deadline.it_value);
    if (timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &deadline, NULL) != 0) {
        return false;
    }
    struct epoll_event event;
    while (epoll_pwait2(epoll_fd, &event, 1, NULL, NULL) <= 0) {
    }
    *late = tt_get_ns() - due;
    uint64_t expirations = 0;
    if (read(timer_fd, &expirations, sizeof expirations) < 0) {
        return false;
    }
    return true;
}

static const char* const arm_names[ARM_COUNT] = {"tickle",           "tickle_earlier", "clock_nanosleep_abs",
                                                 "epoll_pwait2_rel", "ppoll_rel",      "timerfd_abs"};

struct raw_fds {
    int idle_fd;        // an eventfd nothing writes: the waits' only member besides the timer
    int epoll_fd;       // idle_fd
    int timer_fd;       // armed per wait
    int timer_epoll_fd; // timer_fd
};

static bool run_arm(enum arm arm, struct tt_Context* node, const struct raw_fds* fds, uint64_t interval,
                    uint64_t* late) {
    switch (arm) {
    case ARM_TICKLE:
        return arm_tickle(node, interval, late);
    case ARM_EARLIER:
        return arm_earlier(node, interval, late);
    case ARM_NANOSLEEP:
        return arm_nanosleep(interval, late);
    case ARM_EPOLL:
        return arm_epoll_relative(fds->epoll_fd, interval, late);
    case ARM_PPOLL:
        return arm_ppoll_relative(fds->idle_fd, interval, late);
    case ARM_TIMERFD:
        return arm_timerfd_absolute(fds->timer_epoll_fd, fds->timer_fd, interval, late);
    default:
        return false;
    }
}

// One arm at one interval: `samples` waits, their median and maximum lateness and the thread CPU per wait.
static bool measure(enum arm arm, struct tt_Context* node, const struct raw_fds* fds, struct interval when,
                    uint64_t* med, uint64_t* most, uint64_t* cpu_per) {
    uint64_t late[MAX_SAMPLES];
    const uint64_t cpu_before = thread_cpu_ns();
    for (int i = 0; i < when.samples; i++) {
        if (!run_arm(arm, node, fds, when.ns, &late[i])) {
            return false;
        }
    }
    *cpu_per = (thread_cpu_ns() - cpu_before) / (uint64_t)when.samples;
    *most = maximum(late, when.samples);
    *med = median(late, when.samples);
    printf("sched_wake_check: arm=%s interval_ms=%.0f n=%d late_p50_us=%.1f late_max_us=%.1f cpu_per_wait_us=%.1f\n",
           arm_names[arm], (double)when.ns / (double)MS_NS, when.samples, (double)*med / (double)US_NS,
           (double)*most / (double)US_NS, (double)*cpu_per / (double)US_NS);
    return true;
}

static bool raw_fds_open(struct raw_fds* fds) {
    fds->idle_fd = eventfd(0, EFD_CLOEXEC);
    fds->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    fds->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    fds->timer_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event idle_event = {.events = EPOLLIN, .data = {.u32 = 0}};
    struct epoll_event timer_event = {.events = EPOLLIN, .data = {.u32 = 1}};
    if (fds->idle_fd < 0 || fds->epoll_fd < 0 || fds->timer_fd < 0 || fds->timer_epoll_fd < 0 ||
        epoll_ctl(fds->epoll_fd, EPOLL_CTL_ADD, fds->idle_fd, &idle_event) != 0 ||
        epoll_ctl(fds->timer_epoll_fd, EPOLL_CTL_ADD, fds->timer_fd, &timer_event) != 0) {
        printf("sched_wake_check: SETUP FAILED - raw wait descriptors: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static int sweep(struct tt_Context* node) {
    struct raw_fds fds = {-1, -1, -1, -1};
    if (!raw_fds_open(&fds)) {
        return CHECK_SETUP_FAILED;
    }
    printf("sched_wake_check: sweep, timerslack_ns of this thread: see /proc/self/timerslack_ns\n");
    for (size_t i = 0; i < sizeof swept / sizeof swept[0]; i++) {
        for (int arm = 0; arm < ARM_COUNT; arm++) {
            if (arm == ARM_EARLIER) {
                continue; // judged only
            }
            uint64_t med = 0;
            uint64_t most = 0;
            uint64_t cpu = 0;
            if (!measure((enum arm)arm, node, &fds, swept[i], &med, &most, &cpu)) {
                printf("sched_wake_check: SETUP FAILED - arm %s: %s\n", arm_names[arm], strerror(errno));
                return CHECK_SETUP_FAILED;
            }
        }
    }
    return CHECK_PASS;
}

static int judge(struct tt_Context* node) {
    int failures = 0;
    bool void_run = false;
    struct raw_fds fds = {-1, -1, -1, -1};
    if (!raw_fds_open(&fds)) {
        return CHECK_SETUP_FAILED;
    }
    for (size_t i = 0; i < sizeof judged / sizeof judged[0]; i++) {
        uint64_t tickle_med = 0;
        uint64_t tickle_max = 0;
        uint64_t tickle_cpu = 0;
        uint64_t control_med = 0;
        uint64_t control_max = 0;
        uint64_t control_cpu = 0;
        if (!measure(judged[i].arm, node, NULL, judged[i].when, &tickle_med, &tickle_max, &tickle_cpu) ||
            !measure(ARM_NANOSLEEP, node, NULL, judged[i].when, &control_med, &control_max, &control_cpu)) {
            printf("sched_wake_check: SETUP FAILED - could not schedule or sleep\n");
            return CHECK_SETUP_FAILED;
        }
        const double interval_ms = (double)judged[i].when.ns / (double)MS_NS;
        if (control_med > CONTROL_VOID_NS) {
            printf("sched_wake_check: VOID at %.0f ms - the control itself ran %.1f us late (bound %.0f us)\n",
                   interval_ms, (double)control_med / (double)US_NS, (double)CONTROL_VOID_NS / (double)US_NS);
            void_run = true;
            continue;
        }
        // The positive control, at the long intervals: a bare epoll_pwait2() with the same relative timeout must itself
        // run more than twice LATE_MARGIN_NS past the control, or this host does not show the slack the check exists
        // to catch and a pass would mean nothing. Under load other timers' interrupts expire the slack early
        // (2026-10-17, load 10-25: main read 254-310 us at 500 ms against 560 us quiet, and the mutant without the
        // long-wait rule passed while the bare wait ran 224 us late against a control of 65 us).
        if (judged[i].when.ns >= POSITIVE_CONTROL_FROM_NS) {
            uint64_t raw_med = 0;
            uint64_t raw_max = 0;
            uint64_t raw_cpu = 0;
            if (!measure(ARM_EPOLL, node, &fds, judged[i].when, &raw_med, &raw_max, &raw_cpu)) {
                printf("sched_wake_check: SETUP FAILED - the bare relative wait\n");
                return CHECK_SETUP_FAILED;
            }
            if (raw_med <= control_med + (2 * LATE_MARGIN_NS)) {
                printf("sched_wake_check: VOID at %.0f ms - a bare relative wait ran only %.1f us late against the "
                       "control's %.1f us: the host shows no slack to catch\n",
                       interval_ms, (double)raw_med / (double)US_NS, (double)control_med / (double)US_NS);
                void_run = true;
            }
        }
        if (tickle_med > control_med + LATE_MARGIN_NS) {
            printf("sched_wake_check: FAIL at %.0f ms (%s) - entries ran %.1f us late (median), the control %.1f us; "
                   "allowed %.0f us more than the control\n",
                   interval_ms, arm_names[judged[i].arm], (double)tickle_med / (double)US_NS,
                   (double)control_med / (double)US_NS, (double)LATE_MARGIN_NS / (double)US_NS);
            failures++;
        }
        if (tickle_cpu > CPU_PER_WAKE_NS) {
            printf(
                "sched_wake_check: FAIL at %.0f ms (%s) - %.1f us of CPU per entry (bound %.0f us): the wait spins\n",
                interval_ms, arm_names[judged[i].arm], (double)tickle_cpu / (double)US_NS,
                (double)CPU_PER_WAKE_NS / (double)US_NS);
            failures++;
        }
    }
    if (failures != 0) {
        return CHECK_FAIL;
    }
    if (void_run) {
        return CHECK_VOID;
    }
    printf("sched_wake_check: PASS - every entry median within %.0f us of clock_nanosleep()'s, without spinning\n",
           (double)LATE_MARGIN_NS / (double)US_NS);
    return CHECK_PASS;
}

int main(int argc, char** argv) {
    const bool do_sweep = argc > 1 && strcmp(argv[1], "--sweep") == 0;
    static char loopback_broadcast[] = "127.255.255.255";
    char* broadcast = getenv("SCHED_WAKE_BROADCAST");
    _tt_CONFIG.broadcast = broadcast != NULL ? broadcast : loopback_broadcast;
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("sched_wake_check: SETUP FAILED - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    // Settle: the context's own start-up entries (announcements) run before anything is measured. 100 ms was too short:
    // the first 500 ms case ran on main at a median of 214 us late (max 568), its waits cut short by those entries.
    const uint64_t settle_until = tt_get_ns() + (1000 * MS_NS);
    while (tt_get_ns() < settle_until) {
        (void)tt_Context_poll(&node, (int64_t)(10 * MS_NS));
    }
    int result = do_sweep ? sweep(&node) : judge(&node);
    tt_Context_destroy(&node);
    return result;
}
