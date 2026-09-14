/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: handle_receive_result() is static - these pin the one subtle rule tt_Node_poll()'s
// loop depends on (an interrupt must end the poll even when a scheduler entry was also about to
// fire "soon" - the same short-wait window that legitimately swallows a plain timeout). The
// full-tt_Node_poll() tests below cover the same behavior end to end via the mock HAL; see
// src/hal_linux.c's/hal_freertos.c's own real, socket-based implementation (not exercised here -
// this only proves TickLE's own dispatch logic, not the actual cross-thread wakeup).
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include)

static void test_interrupt_ends_poll_even_with_scheduler_wakeup_pending(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    tt_ret_t result = 1234; // a value none of tt_ret_t's own members use, so a bug leaving it
                            // untouched is easy to spot rather than accidentally matching.
    bool ended = handle_receive_result(&node, -3, 0, 0, /*woke_for_scheduler=*/true, &result);
    EXPECT_TRUE(ended);
    EXPECT_EQ_INT(tt_RET_INTERRUPTED, result);
}

static void test_interrupt_ends_poll_with_no_scheduler_wakeup_pending(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    tt_ret_t result = 1234;
    bool ended = handle_receive_result(&node, -3, 0, 0, /*woke_for_scheduler=*/false, &result);
    EXPECT_TRUE(ended);
    EXPECT_EQ_INT(tt_RET_INTERRUPTED, result);
}

// Contrast case: a *plain* timeout (-1, not an interrupt) woken only because a scheduler entry
// was due must still be swallowed (the poll loop keeps going) - confirms the -3 branch above is
// actually a distinct code path, not a side effect of some other change to this function.
static void test_plain_timeout_still_swallowed_on_scheduler_wakeup(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    tt_ret_t result = 1234;
    bool ended = handle_receive_result(&node, -1, 0, 0, /*woke_for_scheduler=*/true, &result);
    EXPECT_TRUE(!ended);
}

static void test_plain_timeout_ends_poll_without_scheduler_wakeup(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    tt_ret_t result = 1234;
    bool ended = handle_receive_result(&node, -1, 0, 0, /*woke_for_scheduler=*/false, &result);
    EXPECT_TRUE(ended);
    EXPECT_EQ_INT(tt_RET_TIMEOUT, result);
}

static void test_io_error_unaffected(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    tt_ret_t result = 1234;
    bool ended = handle_receive_result(&node, -2, 0, 0, /*woke_for_scheduler=*/false, &result);
    EXPECT_TRUE(ended);
    EXPECT_EQ_INT(tt_RET_IO_ERROR, result);
}

// End to end through tt_Node_poll() itself (still via the mock HAL - see this file's own top
// comment on what that does and doesn't prove).
static void test_node_poll_returns_interrupted(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    test_mock_reset();
    test_mock_receive_return = -3;

    tt_ret_t ret = tt_Node_poll(&node, 1000000000); // 1s - the mock never really waits regardless
    EXPECT_EQ_INT(tt_RET_INTERRUPTED, ret);
}

static void test_node_interrupt_calls_hal_wake_signal(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    test_mock_reset();

    tt_ret_t ret = tt_Node_interrupt(&node);
    EXPECT_EQ_INT(tt_RET_OK, ret);
    EXPECT_EQ_INT(1, test_mock_wake_signal_call_count);
}

static void test_node_interrupt_rejects_null(void) {
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Node_interrupt(NULL));
}

int main(void) {
    test_interrupt_ends_poll_even_with_scheduler_wakeup_pending();
    test_interrupt_ends_poll_with_no_scheduler_wakeup_pending();
    test_plain_timeout_still_swallowed_on_scheduler_wakeup();
    test_plain_timeout_ends_poll_without_scheduler_wakeup();
    test_io_error_unaffected();
    test_node_poll_returns_interrupted();
    test_node_interrupt_calls_hal_wake_signal();
    test_node_interrupt_rejects_null();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_node_interrupt: all tests passed\n");
    return 0;
}
