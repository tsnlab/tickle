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
