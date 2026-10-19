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
// The short case (added 2026-10-18): below 596e9c9c's 10 ms threshold a wait for an entry still took the relative
// timeout, whose slack is never less than the thread's 50 us timer slack, so a 5 ms entry ran ~60 us late - the same as
// clock_nanosleep(), which carries that slack too, so the control above cannot see it. Its reference is a timerfd armed
// for the absolute deadline (no slack, ~10 us late). The three waits - TickLE's, the timerfd's and a bare relative
// epoll_pwait2() at the default slack - take turns, and each is read as its difference from the timerfd wait of the
// same round (2026-10-19, judge_short() says why):
//   PASS  median(tickle - timerfd_abs) <= max(SHORT_MARGIN_NS, median(epoll_pwait2_rel - timerfd_abs) / 2) at
//         SHORT_INTERVAL_NS, CPU as above.
//   FAIL  above it. On main (18f16b85, PC, nice 0) 5 ms entries ran 58-62 us late against a timerfd's 7-24 us.
//   VOID  median(epoll_pwait2_rel - timerfd_abs) no more than SHORT_SLACK_SHOWN_NS: no slack to catch.
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
#include <sys/prctl.h>
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
#define SHORT_INTERVAL_NS (5 * MS_NS)             // the short case: below the 10 ms threshold of 596e9c9c
#define SHORT_SAMPLES 40
#define SHORT_MARGIN_NS (25ULL * US_NS)      // tickle's wait may end this much after the same round's timerfd (median)
#define SHORT_SLACK_SHOWN_NS (30ULL * US_NS) // the bare relative wait must end more than this after it (median)

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
static const struct interval swept[] = {{1 * MS_NS, 40},  {2 * MS_NS, 40},  {5 * MS_NS, 30},
                                        {10 * MS_NS, 30}, {50 * MS_NS, 12}, {100 * MS_NS, 8},
                                        {200 * MS_NS, 8}, {500 * MS_NS, 6}, {1000 * MS_NS, 5}};

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
// The arms that are not TickLE's - the controls and the raw waits - run at the thread's default timer slack (50 us),
// whatever the poll has set it to: the HAL lowers the polling thread's slack (hal_linux.c TT_RX_TIMER_SLACK_NS,
// 2026-10-18), and this thread is that thread, so without this the positive control lost the very slack it exists to
// show (VOID at 5 ms, 13.9 us against the timerfd's 9.5) and clock_nanosleep() stopped being the DDS clients' pacing.
#define CONTROL_SLACK_NS 50000UL

static bool measure(enum arm arm, struct tt_Context* node, const struct raw_fds* fds, struct interval when,
                    uint64_t* med, uint64_t* most, uint64_t* cpu_per) {
    uint64_t late[MAX_SAMPLES];
    const bool control = arm != ARM_TICKLE && arm != ARM_EARLIER;
    const int poll_slack = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0); // NOLINT(misc-include-cleaner) - <sys/prctl.h>
    if (control) {
        (void)prctl(PR_SET_TIMERSLACK, CONTROL_SLACK_NS, 0, 0, 0); // NOLINT(misc-include-cleaner) - <sys/prctl.h>
    }
    const uint64_t cpu_before = thread_cpu_ns();
    bool ran = true;
    for (int i = 0; i < when.samples && ran; i++) {
        ran = run_arm(arm, node, fds, when.ns, &late[i]);
    }
    if (control && poll_slack > 0) {
        (void)prctl(PR_SET_TIMERSLACK, (unsigned long)poll_slack, 0, 0, 0); // NOLINT(misc-include-cleaner)
    }
    if (!ran) {
        return false;
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

// ---- the short case: the three arms interleaved, judged on paired differences

static int compare_i64(const void* a, const void* b) {
    int64_t left = *(const int64_t*)a;
    int64_t right = *(const int64_t*)b;
    return (left > right) - (left < right);
}

static int64_t median_i64(int64_t* values, int count) {
    qsort(values, (size_t)count, sizeof values[0], compare_i64);
    return values[count / 2];
}

// One wait of `arm` at the short interval. The two references run at the default timer slack, as measure() runs them,
// and the thread gets the poll's slack back after each; TickLE's wait adds its thread CPU time to *tickle_cpu.
static bool short_sample(enum arm arm, struct tt_Context* node, const struct raw_fds* fds, uint64_t* late,
                         uint64_t* tickle_cpu) {
    if (arm == ARM_TICKLE) {
        const uint64_t cpu_before = thread_cpu_ns();
        const bool ran = arm_tickle(node, SHORT_INTERVAL_NS, late);
        *tickle_cpu += thread_cpu_ns() - cpu_before;
        return ran;
    }
    const int poll_slack = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0); // NOLINT(misc-include-cleaner) - <sys/prctl.h>
    (void)prctl(PR_SET_TIMERSLACK, CONTROL_SLACK_NS, 0, 0, 0);   // NOLINT(misc-include-cleaner) - <sys/prctl.h>
    const bool ran = run_arm(arm, node, fds, SHORT_INTERVAL_NS, late);
    if (poll_slack > 0) {
        (void)prctl(PR_SET_TIMERSLACK, (unsigned long)poll_slack, 0, 0, 0); // NOLINT(misc-include-cleaner)
    }
    return ran;
}

// The short case: CHECK_PASS, CHECK_FAIL or CHECK_VOID (see the header), CHECK_SETUP_FAILED.
//
// The arms take turns, one wait each per round (the first arm rotating), and the verdict reads each round's
// differences from its timerfd wait, not three medians taken 200 ms apart. A host can wake a sleeping thread at one of
// two speeds and stay at each for tens of milliseconds: on the PC (a VM, 2026-10-19) every arm - the timerfd too, and
// a bare relative epoll_pwait2() at the poll's 1 ns slack - ran ~10-15 us late in one and ~40-45 us in the other, in
// runs of consecutive waits, on any CPU. Measured one after another, each arm's median fell in whichever mode its
// 200 ms caught: TickLE 40-43 us against a timerfd's 11 failed on the PC as on CI (58.3 against 19.5, 2026-10-18),
// while a timerfd's own median read 36 us in other runs. TickLE's wait is that bare 1 ns-slack wait (one plain
// relative epoll_pwait2() per entry, no deadline-timer fire). Paired, a mode both waits of a round share cancels.
static int judge_short(struct tt_Context* node, const struct raw_fds* fds) {
    static const enum arm order[3] = {ARM_TICKLE, ARM_TIMERFD, ARM_EPOLL};
    uint64_t late[3][SHORT_SAMPLES];
    int64_t tickle_less_timerfd[SHORT_SAMPLES];
    int64_t raw_less_timerfd[SHORT_SAMPLES];
    uint64_t tickle_cpu_total = 0;
    for (int round = 0; round < SHORT_SAMPLES; round++) {
        for (int turn = 0; turn < 3; turn++) {
            const int which = (round + turn) % 3;
            if (!short_sample(order[which], node, fds, &late[which][round], &tickle_cpu_total)) {
                printf("sched_wake_check: SETUP FAILED - the short case, arm %s: %s\n", arm_names[order[which]],
                       strerror(errno));
                return CHECK_SETUP_FAILED;
            }
        }
        tickle_less_timerfd[round] = (int64_t)late[0][round] - (int64_t)late[1][round];
        raw_less_timerfd[round] = (int64_t)late[2][round] - (int64_t)late[1][round];
    }
    const uint64_t tickle_cpu = tickle_cpu_total / SHORT_SAMPLES;
    const double interval_ms = (double)SHORT_INTERVAL_NS / (double)MS_NS;
    for (int which = 0; which < 3; which++) {
        const uint64_t most = maximum(late[which], SHORT_SAMPLES);
        const uint64_t med = median(late[which], SHORT_SAMPLES);
        printf("sched_wake_check: arm=%s interval_ms=%.0f n=%d late_p50_us=%.1f late_max_us=%.1f (interleaved)\n",
               arm_names[order[which]], interval_ms, SHORT_SAMPLES, (double)med / (double)US_NS,
               (double)most / (double)US_NS);
    }
    const int64_t tickle_over = median_i64(tickle_less_timerfd, SHORT_SAMPLES);
    const int64_t raw_over = median_i64(raw_less_timerfd, SHORT_SAMPLES);
    printf("sched_wake_check: short pairs n=%d tickle_minus_timerfd_p50_us=%.1f "
           "epoll_pwait2_rel_minus_timerfd_p50_us=%.1f "
           "tickle_cpu_per_wait_us=%.1f\n",
           SHORT_SAMPLES, (double)tickle_over / (double)US_NS, (double)raw_over / (double)US_NS,
           (double)tickle_cpu / (double)US_NS);
    // Defined at nice 0. At a positive nice value Linux gives a relative timeout 0.5% of its length as slack instead
    // of 0.1%, so the HAL's 1 ns thread slack leaves a 5 ms wait ~25 us late (test-linux under the session's nice 5,
    // 2026-10-18: 62.8 us against the timerfd's 35.7) - a known limit of that design (DESIGN.md 2), not this check's
    // subject. test.sh runs it at nice 0.
    errno = 0;
    const int nice_value = getpriority(PRIO_PROCESS, 0); // NOLINT(misc-include-cleaner) - <sys/resource.h>
    if (errno == 0 && nice_value > 0) {
        printf("sched_wake_check: VOID at %.0f ms (short) - run at nice %d; the short case is defined at nice 0\n",
               interval_ms, nice_value);
        return CHECK_VOID;
    }
    int result = CHECK_PASS;
    // The bound is the larger of SHORT_MARGIN_NS and half the way from the timerfd to the bare relative wait: the
    // check asks whether TickLE's entries sit nearer the absolute deadline than the slack-stretched wait. A fixed
    // 25 us alone failed on a busy CI runner (caa134d6, 2026-10-18: TickLE 64.1, timerfd 29.5, relative 105.6 us),
    // while a lost slack setting reads like the relative wait and still fails either way.
    const int64_t half_way = raw_over > 0 ? raw_over / 2 : 0;
    const int64_t short_bound = half_way > (int64_t)SHORT_MARGIN_NS ? half_way : (int64_t)SHORT_MARGIN_NS;
    if (tickle_over > short_bound) {
        printf("sched_wake_check: FAIL at %.0f ms (short) - entries ran %.1f us later than a timerfd for the absolute "
               "deadline in the same round (median of %d rounds); allowed %.1f us\n",
               interval_ms, (double)tickle_over / (double)US_NS, SHORT_SAMPLES, (double)short_bound / (double)US_NS);
        result = CHECK_FAIL;
    }
    if (tickle_cpu > CPU_PER_WAKE_NS) {
        printf("sched_wake_check: FAIL at %.0f ms (short) - %.1f us of CPU per entry (bound %.0f us): the wait spins\n",
               interval_ms, (double)tickle_cpu / (double)US_NS, (double)CPU_PER_WAKE_NS / (double)US_NS);
        result = CHECK_FAIL;
    }
    if (result == CHECK_PASS && raw_over <= (int64_t)SHORT_SLACK_SHOWN_NS) {
        printf(
            "sched_wake_check: VOID at %.0f ms (short) - a bare relative wait ran only %.1f us later than the timerfd "
            "in the same round (median): the host shows no slack to catch\n",
            interval_ms, (double)raw_over / (double)US_NS);
        result = CHECK_VOID;
    }
    return result;
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
    const int short_result = judge_short(node, &fds);
    if (short_result == CHECK_SETUP_FAILED) {
        return CHECK_SETUP_FAILED;
    }
    if (failures != 0 || short_result == CHECK_FAIL) {
        return CHECK_FAIL;
    }
    if (void_run || short_result == CHECK_VOID) {
        return CHECK_VOID;
    }
    printf("sched_wake_check: PASS - every entry median within %.0f us of clock_nanosleep()'s and, at %.0f ms, within "
           "%.0f us (or half the slack shown) of an absolute timerfd's in the same round, without spinning\n",
           (double)LATE_MARGIN_NS / (double)US_NS, (double)SHORT_INTERVAL_NS / (double)MS_NS,
           (double)SHORT_MARGIN_NS / (double)US_NS);
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
