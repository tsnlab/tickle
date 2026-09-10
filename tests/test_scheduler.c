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

// Whitebox: peek_scheduler()/pop_scheduler() are static. The scheduler is a binary min-heap;
// these check it always yields entries in time order regardless of insertion order, and that
// removing from the middle keeps the rest ordered.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include)

static void noop_a(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
}
static void noop_b(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
}

// Drains the scheduler via peek/pop and checks the times come out non-decreasing.
static void expect_drains_in_order(struct tt_Node* node, int expected_count) {
    uint64_t prev = 0;
    int count = 0;
    struct tt_TCB* tcb;
    while ((tcb = peek_scheduler(node)) != NULL) {
        EXPECT_TRUE(tcb->time >= prev);
        prev = tcb->time;
        count++;
        pop_scheduler(node);
    }
    EXPECT_EQ_INT(expected_count, count);
}

// Inserting in a jumbled order must still pop in ascending time order.
static void test_pops_in_time_order_regardless_of_insert_order(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    const uint64_t times[] = {50, 10, 90, 30, 30, 70, 1, 100, 40};
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        EXPECT_TRUE(tt_Node_schedule(&node, times[i], noop_a, NULL));
    }

    expect_drains_in_order(&node, (int)(sizeof(times) / sizeof(times[0])));
}

// Unscheduling a specific (function, param) from the middle of the heap must remove exactly
// those and leave the rest still in time order.
static void test_unschedule_middle_keeps_order(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    int marker = 0;
    tt_Node_schedule(&node, 10, noop_a, NULL);
    tt_Node_schedule(&node, 20, noop_b, &marker); // target
    tt_Node_schedule(&node, 30, noop_a, NULL);
    tt_Node_schedule(&node, 40, noop_b, &marker); // target
    tt_Node_schedule(&node, 50, noop_a, NULL);
    tt_Node_schedule(&node, 5, noop_b, &marker);  // target
    tt_Node_schedule(&node, 60, noop_a, NULL);

    EXPECT_TRUE(tt_Node_unschedule(&node, noop_b, &marker));
    EXPECT_EQ_INT(4, node.scheduler_tail);
    EXPECT_TRUE(!tt_Node_unschedule(&node, noop_b, &marker)); // none left

    expect_drains_in_order(&node, 4);
}

// The earliest entry is always at the root, whether it went in first, last, or in the middle.
static void test_earliest_always_at_root(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    tt_Node_schedule(&node, 100, noop_a, NULL);
    EXPECT_EQ_U32(100, (uint32_t)peek_scheduler(&node)->time);
    tt_Node_schedule(&node, 50, noop_a, NULL);
    EXPECT_EQ_U32(50, (uint32_t)peek_scheduler(&node)->time);
    tt_Node_schedule(&node, 75, noop_a, NULL);
    EXPECT_EQ_U32(50, (uint32_t)peek_scheduler(&node)->time);
    tt_Node_schedule(&node, 1, noop_a, NULL);
    EXPECT_EQ_U32(1, (uint32_t)peek_scheduler(&node)->time);

    pop_scheduler(&node);
    EXPECT_EQ_U32(50, (uint32_t)peek_scheduler(&node)->time);
}

int main(void) {
    test_pops_in_time_order_regardless_of_insert_order();
    test_unschedule_middle_keeps_order();
    test_earliest_always_at_root();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_scheduler: all tests passed\n");
    return 0;
}
