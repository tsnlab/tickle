#pragma once

// Shared helpers for focused C unit-test binaries.
// Keep this header small so tests can stay framework-free.

#include <stdint.h>
#include <stdio.h>

// Define storage in exactly one test binary; other files can include this header as extern declarations.
#ifdef TEST_COMMON_DEFINE_STORAGE
int test_failures = 0;
#else
extern int test_failures;
#endif

// Minimal assertions keep test files dependency-free and easy to add incrementally.
#define EXPECT_TRUE(expr)                                                             \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr, "%s:%d: expected true: %s\n", __FILE__, __LINE__, #expr); \
            test_failures++;                                                          \
        }                                                                             \
    } while (0)

#define EXPECT_EQ_U32(expected, actual)                                                                        \
    do {                                                                                                       \
        uint32_t expected_value = (expected);                                                                  \
        uint32_t actual_value = (actual);                                                                      \
        if (expected_value != actual_value) {                                                                  \
            fprintf(stderr, "%s:%d: expected %u, got %u\n", __FILE__, __LINE__, expected_value, actual_value); \
            test_failures++;                                                                                   \
        }                                                                                                      \
    } while (0)

// Return a process status that make can use directly.
static inline int test_result(void) {
    if (test_failures != 0) {
        fprintf(stderr, "%d test assertions failed\n", test_failures);
        return 1;
    }

    return 0;
}
