// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// Which clock each of the Linux HAL's clock functions reads (DESIGN.md section 2, "Time"), run by test_samehost.sh.
//
// The unit tests link a mock HAL with two clocks of its own (tests/test_clock_step.c), so they cannot say whether
// hal_linux.c gives every deadline CLOCK_MONOTONIC and only the wire's timestamps CLOCK_REALTIME - and swapping the
// two compiles, runs, and passes every test that never steps the date. So this asks the HAL directly:
//
//   1. tt_get_ns() lies between two CLOCK_MONOTONIC readings taken around it
//   2. tt_get_wall_ns() lies between two CLOCK_REALTIME readings taken around it
//   3. tt_timer_resolution_ns() is clock_getres(CLOCK_MONOTONIC)
//
// Each is decidable only while the two clocks are far apart, which they are on any host whose date is set (the epoch
// against the time since boot); a host where they are within a minute of each other makes the check void (exit 2), not
// a pass.
//
// Exit 0 pass, 1 fail, 2 void.
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <tickle/hal.h>

#define NS_PER_S 1000000000ULL
#define DISTINCT_NS (60ULL * NS_PER_S)

static uint64_t read_clock(clockid_t clock) { // NOLINT(misc-include-cleaner) - clockid_t: <time.h>
    struct timespec ts;
    clock_gettime(clock, &ts); // NOLINT(misc-include-cleaner) - <time.h>
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

int main(void) {
    // NOLINTBEGIN(misc-include-cleaner) - CLOCK_MONOTONIC/CLOCK_REALTIME: <time.h>
    const uint64_t mono_before = read_clock(CLOCK_MONOTONIC);
    const uint64_t deadline_clock = tt_get_ns();
    const uint64_t mono_after = read_clock(CLOCK_MONOTONIC);
    const uint64_t real_before = read_clock(CLOCK_REALTIME);
    const uint64_t date_clock = tt_get_wall_ns();
    const uint64_t real_after = read_clock(CLOCK_REALTIME);
    struct timespec res;
    const uint64_t resolution =
        clock_getres(CLOCK_MONOTONIC, &res) == 0 ? ((uint64_t)res.tv_sec * NS_PER_S) + (uint64_t)res.tv_nsec : 1;
    // NOLINTEND(misc-include-cleaner)

    const uint64_t apart = real_before > mono_before ? real_before - mono_before : mono_before - real_before;
    printf("clock_check: tt_get_ns() %llu in [%llu, %llu] (CLOCK_MONOTONIC); tt_get_wall_ns() %llu in [%llu, %llu] "
           "(CLOCK_REALTIME); the two clocks %.0f s apart; tt_timer_resolution_ns() %llu, clock_getres %llu\n",
           (unsigned long long)deadline_clock, (unsigned long long)mono_before, (unsigned long long)mono_after,
           (unsigned long long)date_clock, (unsigned long long)real_before, (unsigned long long)real_after,
           (double)apart / (double)NS_PER_S, (unsigned long long)tt_timer_resolution_ns(),
           (unsigned long long)(resolution != 0 ? resolution : 1));
    if (apart < DISTINCT_NS) {
        printf(
            "clock_check: VOID - the two clocks are within %llu s of each other, so neither reading tells them apart\n",
            DISTINCT_NS / NS_PER_S);
        return 2;
    }
    int failures = 0;
    if (deadline_clock < mono_before || deadline_clock > mono_after) {
        printf("clock_check: FAIL - tt_get_ns() is not CLOCK_MONOTONIC\n");
        failures++;
    }
    if (date_clock < real_before || date_clock > real_after) {
        printf("clock_check: FAIL - tt_get_wall_ns() is not CLOCK_REALTIME\n");
        failures++;
    }
    if (tt_timer_resolution_ns() != (resolution != 0 ? resolution : 1)) {
        printf("clock_check: FAIL - tt_timer_resolution_ns() is not CLOCK_MONOTONIC's resolution\n");
        failures++;
    }
    if (failures == 0) {
        printf("clock_check: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
