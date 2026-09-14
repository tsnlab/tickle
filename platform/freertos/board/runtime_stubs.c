/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Small picolibc runtime hooks this target still needs even though stdio_uart.c's
// FDEV_SETUP_STREAM bypasses the usual file-descriptor _read/_write/_open model entirely:
// abort() (pulled in by lwIP's default LWIP_PLATFORM_ASSERT) calls _exit(), and log.c's
// time(&now)/localtime(&now) call gettimeofday() and (indirectly) picolibc's own gmtime_r().

#include <stddef.h>
#include <time.h>

#include <sys/time.h>

void _exit(int status) {
    (void)status;
    for (;;) {
    }
}

// picolibc's own declaration uses reserved-identifier parameter names (__p/__tz); matching them
// here would itself trip bugprone-reserved-identifier, so this keeps its own descriptive names.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
int gettimeofday(struct timeval* result_tv, void* time_zone) {
    (void)time_zone;
    if (result_tv != NULL) {
        // No RTC on this target; log.c only uses this for a human-readable log timestamp, which
        // isn't meaningful here anyway (see hal_freertos.c's tt_get_ns() for the real clock this
        // platform actually relies on).
        result_tv->tv_sec = 0;
        result_tv->tv_usec = 0;
    }
    return 0;
}

// picolibc's own localtime()/gmtime_r() crash on this target (a store fault deep inside
// gmtime_r - some timezone-related global this freestanding build never gets to initialize).
// log.c only needs a valid, crash-free struct tm to format a timestamp string from, and always
// in UTC is fine (there's no wall-clock time to be a timezone offset from anyway, see
// gettimeofday() above) - so this replaces it with a minimal, allocation-free UTC-only
// converter instead of chasing picolibc's internals.
//
// Algorithm: Howard Hinnant's civil_from_days (public domain,
// https://howardhinnant.github.io/date_algorithms.html#civil_from_days).
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) -- see gettimeofday() above
struct tm* localtime(const time_t* timep) {
    static struct tm result;

    // Everything below is either this well-known algorithm's own short names
    // (y/m/d/z/era/doe/yoe/doy/mp) and constants, or the equally standard seconds-per-day/hour/
    // minute epoch-math every libc's localtime() uses - kept verbatim/inline rather than
    // renamed or extracted into constants so this stays checkable against the reference
    // (linked above) rather than becoming a paraphrase of it.
    // NOLINTBEGIN(readability-magic-numbers, readability-identifier-length)
    time_t seconds = *timep;

    long days = (long)(seconds / 86400);
    long rem = (long)(seconds % 86400);
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    result.tm_hour = (int)(rem / 3600);
    result.tm_min = (int)((rem % 3600) / 60);
    result.tm_sec = (int)(rem % 60);
    result.tm_wday = (int)(((days % 7) + 11) % 7); // 1970-01-01 was a Thursday
    result.tm_isdst = 0;

    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - (era * 146097));
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + (era * 400);
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    unsigned long d = (doy - (153 * mp + 2) / 5) + 1;
    unsigned long m = mp + (mp < 10 ? 3 : (unsigned long)-9);
    y += (long)(m <= 2);

    result.tm_year = (int)(y - 1900); // 1900: struct tm's own "years since" convention, not Hinnant's
    result.tm_mon = (int)(m - 1);
    result.tm_mday = (int)d;
    // NOLINTEND(readability-magic-numbers, readability-identifier-length)
    result.tm_yday = 0; // Not computed - unused by log.c's "%Y-%m-%d %H:%M:%S" format

    return &result;
}
