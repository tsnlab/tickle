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
#include <string.h>

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

// B1 (rmw_tickle/PLAN.md) - declares and zero-initializes a depth-`n` reliable cache for a test:
// the index array, the byte arena, and the struct pointing at both. The arena is sized for `n`
// records of up to 64 payload bytes (plus the wrap slack tt_RELIABLE_CACHE_ARENA_BYTES() adds),
// comfortably above the 4-byte payloads these tests publish - a test that wants a deliberately
// tight arena sets `name.arena_size` down afterwards.
#define TEST_RELIABLE_CACHE(name, n)                                                                \
    struct tt_ReliableCacheIndex name##_index_storage[(n)];                                         \
    uint8_t name##_arena_storage[tt_RELIABLE_CACHE_ARENA_BYTES((n), tt_RELIABLE_RECORD_BYTES(64))]; \
    struct tt_ReliableCache name;                                                                   \
    memset(name##_index_storage, 0, sizeof(name##_index_storage));                                  \
    memset(name##_arena_storage, 0, sizeof(name##_arena_storage));                                  \
    memset(&(name), 0, sizeof(name));                                                               \
    (name).index = name##_index_storage;                                                            \
    (name).capacity = (uint16_t)(n);                                                                \
    (name).depth = (uint16_t)(n);                                                                   \
    (name).arena = name##_arena_storage;                                                            \
    (name).arena_size = (uint32_t)sizeof(name##_arena_storage)

// Returns a process exit status make can use directly: 0 if every EXPECT_* in this binary
// passed, 1 (with a summary line) otherwise.
static inline int test_result(void) {
    if (test_failures != 0) {
        fprintf(stderr, "%d assertion(s) failed\n", test_failures);
        return 1;
    }

    return 0;
}
