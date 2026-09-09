/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Shared, locale-free thousands-grouping for example output (see README's "Result output") -
// printf's own locale-based `'` grouping flag (e.g. "%'llu") depends on setlocale(LC_NUMERIC, ...)
// support that picolibc (the FreeRTOS target's libc) doesn't reliably provide, so this formats
// the digits explicitly instead. Header-only (no platform-specific code, nothing to link) so any
// example on either platform can just #include it directly - see its own build for the exact
// relative path (examples/{linux,freertos}/<protocol>/ are both two levels under examples/, so
// "../../format.h" reaches this file from either).
#pragma once

#include <stdint.h>

// UINT64_MAX is 20 digits; grouping adds a ',' every 3 digits, i.e. 6 more; +1 for the nul.
#define TT_GROUPED_MAX_DIGITS 20
#define TT_GROUPED_BUF_LEN 27

// Formats num into buf with a ',' every three digits (e.g. 1234567 -> "1,234,567") and returns
// buf, so this can be used directly as a printf() argument: printf("%s", tt_format_grouped(num,
// buf)). buf must be at least TT_GROUPED_BUF_LEN bytes.
static inline char* tt_format_grouped(uint64_t num, char buf[TT_GROUPED_BUF_LEN]) {
    char digits[TT_GROUPED_MAX_DIGITS]; // least-significant first
    int ndigits = 0;

    if (num == 0) {
        digits[ndigits++] = '0';
    } else {
        while (num > 0) {
            digits[ndigits++] = (char)('0' + (num % 10));
            num /= 10;
        }
    }

    int pos = 0;
    for (int i = ndigits - 1; i >= 0; i--) {
        buf[pos++] = digits[i];
        if (i > 0 && i % 3 == 0) {
            buf[pos++] = ',';
        }
    }
    buf[pos] = '\0';

    return buf;
}

// '.' plus 3 fractional digits on top of tt_format_grouped()'s own bound.
#define TT_GROUPED_F3_BUF_LEN (TT_GROUPED_BUF_LEN + 4)

// Like tt_format_grouped(), but for a non-negative value formatted to exactly 3 decimal digits
// (e.g. 1234567.891 -> "1,234,567.891") - the MB/Mbps figures in the perf examples, which can run
// well past 999 at full throughput. Splits into integer and fractional parts and formats each
// manually rather than deferring to printf's own grouping flag, for the same portability reason
// tt_format_grouped() itself exists (see this file's own top comment) - and because "%'f" isn't a
// thing even where glibc's "%'d" is. value must be non-negative (every call site here is a
// count/rate); buf must be at least TT_GROUPED_F3_BUF_LEN bytes.
static inline char* tt_format_grouped_f3(double value, char buf[TT_GROUPED_F3_BUF_LEN]) {
    char int_buf[TT_GROUPED_BUF_LEN];

    // Round to the nearest thousandth up front (not truncate-then-round-fraction separately) so
    // the integer and fractional parts printed always agree with each other, e.g. 0.9999 becomes
    // "1.000" rather than the integer part truncating to "0" while the fraction rounds to "1000".
    const double thousandths_per_unit = 1000.0;
    const double round_half_up = 0.5;
    uint64_t total_thousandths = (uint64_t)((value * thousandths_per_unit) + round_half_up);
    uint64_t integer_part = total_thousandths / (uint64_t)thousandths_per_unit;
    uint64_t frac_part = total_thousandths % (uint64_t)thousandths_per_unit;

    tt_format_grouped(integer_part, int_buf);

    int pos = 0;
    for (; int_buf[pos] != '\0'; pos++) {
        buf[pos] = int_buf[pos];
    }

    const uint64_t hundreds_place = 100;
    const uint64_t tens_place = 10;
    buf[pos++] = '.';
    buf[pos++] = (char)('0' + (frac_part / hundreds_place));
    buf[pos++] = (char)('0' + ((frac_part / tens_place) % tens_place));
    buf[pos++] = (char)('0' + (frac_part % tens_place));
    buf[pos] = '\0';

    return buf;
}
