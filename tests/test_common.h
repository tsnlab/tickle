/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

// Shared helpers for focused, framework-free C unit-test binaries. Each test_*.c is its own
// standalone executable (see the `test` target in the Makefile) - no test runner/registry, no
// external dependency, just a main() that calls its own test functions in order.

#include <stdint.h>
#include <stdio.h>

// Defined in exactly one place per test binary; the test .c file does that before including
// this header (see the DEFINE_STORAGE convention below).
#ifdef TEST_COMMON_DEFINE_STORAGE
int test_failures = 0;
#else
extern int test_failures;
#endif

#define EXPECT_TRUE(expr)                                                             \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr, "%s:%d: expected true: %s\n", __FILE__, __LINE__, #expr); \
            test_failures++;                                                          \
        }                                                                             \
    } while (0)

#define EXPECT_EQ_U32(expected, actual)                                                                          \
    do {                                                                                                         \
        uint32_t expected_value_ = (expected);                                                                   \
        uint32_t actual_value_ = (actual);                                                                       \
        if (expected_value_ != actual_value_) {                                                                  \
            fprintf(stderr, "%s:%d: expected %u, got %u\n", __FILE__, __LINE__, expected_value_, actual_value_); \
            test_failures++;                                                                                     \
        }                                                                                                        \
    } while (0)

#define EXPECT_EQ_INT(expected, actual)                                                                          \
    do {                                                                                                         \
        int expected_value_ = (expected);                                                                        \
        int actual_value_ = (actual);                                                                            \
        if (expected_value_ != actual_value_) {                                                                  \
            fprintf(stderr, "%s:%d: expected %d, got %d\n", __FILE__, __LINE__, expected_value_, actual_value_); \
            test_failures++;                                                                                     \
        }                                                                                                        \
    } while (0)

// Returns a process exit status make can use directly: 0 if every EXPECT_* in this binary
// passed, 1 (with a summary line) otherwise.
static inline int test_result(void) {
    if (test_failures != 0) {
        fprintf(stderr, "%d assertion(s) failed\n", test_failures);
        return 1;
    }

    return 0;
}
