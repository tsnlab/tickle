/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// How long tt_Node_poll() decides to wait (2026-09-25, the user's decisions: the scheduler already knows
// when the next thing is due, so wait exactly until then, and indefinitely when nothing is - new work from
// another thread arrives by interrupt). A negative timeout used to be a fixed 100us slice - ~10,000 wakes
// a second on an idle node, measured on the rig as 99.7% of the latency client's system time. It now
// waits for the next due scheduler entry, runs it and returns. A positive timeout must be exactly what it
// was.
//
// Through the mock HAL, whose tt_receive() records what it was asked to wait and - with
// test_mock_receive_advances_clock - lets that much time pass, as a real wait with nothing arriving would.
// This proves TickLE's own decision about how long to wait, not the HAL's actual sleeping.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) - whitebox, as test_node_interrupt.c
#include "test_mock.h"

static int entry_runs = 0;
static uint64_t entry_ran_at = 0;

static void count_entry(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)param;
    entry_runs++;
    entry_ran_at = time;
}

static void setup(struct tt_Node* node) {
    memset(node, 0, sizeof(*node));
    test_mock_reset();
    test_mock_receive_advances_clock = true;
    entry_runs = 0;
    entry_ran_at = 0;
}

// The case the change exists for. One entry due in 3ms: the poll waits exactly 3ms - one wait, not
// thirty 100us slices - runs the entry, and returns. Under the old contract the one wait would have been
// 100us and the poll would have returned with the entry still pending.
static void test_negative_poll_waits_until_the_next_entry(void) {
    struct tt_Node node;
    setup(&node);
    test_mock_now = 10 * tt_MILLISECOND;
    EXPECT_TRUE(tt_Node_schedule(&node, test_mock_now + (3 * tt_MILLISECOND), count_entry, NULL));

    tt_ret_t ret = tt_Node_poll(&node, -1);

    EXPECT_EQ_INT(tt_RET_TIMEOUT, ret);
    EXPECT_EQ_INT(1, test_mock_receive_call_count);                              // one wait
    EXPECT_EQ_U64(3 * tt_MILLISECOND, (uint64_t)test_mock_receive_last_timeout); // exactly to the entry
    EXPECT_EQ_INT(1, entry_runs);                                                // and the entry ran
    EXPECT_EQ_U64(13 * tt_MILLISECOND, entry_ran_at);
}

// Nothing scheduled: nothing to wake up for, so no deadline at all - 0, which tt_receive() takes as "no
// timeout". The mock answers at once; a real HAL would block until a datagram, a signal or an interrupt.
static void test_negative_poll_with_nothing_scheduled_blocks_indefinitely(void) {
    struct tt_Node node;
    setup(&node);

    tt_ret_t ret = tt_Node_poll(&node, -1);

    EXPECT_EQ_INT(tt_RET_TIMEOUT, ret);
    EXPECT_EQ_INT(1, test_mock_receive_call_count);
    EXPECT_EQ_U64(0, (uint64_t)test_mock_receive_last_timeout); // no deadline, not a ceiling
}

// However far off the next entry is, the wait is exactly that - there is no ceiling to cut it into
// pieces, which is where the ~10,000 idle wakes a second came from.
static void test_negative_poll_waits_exactly_for_a_distant_entry(void) {
    struct tt_Node node;
    setup(&node);
    EXPECT_TRUE(tt_Node_schedule(&node, 5 * tt_SECOND, count_entry, NULL));

    tt_ret_t ret = tt_Node_poll(&node, -1);

    EXPECT_EQ_INT(tt_RET_TIMEOUT, ret);
    EXPECT_EQ_INT(1, test_mock_receive_call_count); // one wait, not fifty thousand 100us slices
    EXPECT_EQ_U64(5 * tt_SECOND, (uint64_t)test_mock_receive_last_timeout);
    EXPECT_EQ_INT(1, entry_runs);
}

// A wait that ends before its entry is due, with nothing received, was cut short - the HALs report
// EINTR as a timeout. The poll must hand control back rather than wait again, or under an indefinite
// wait Ctrl-C would never reach the caller's loop. The mock's clock standing still is exactly that:
// the wait returned, and no time passed. The call limit turns the regression - waiting again, forever -
// into a failed assertion instead of a hung test.
static void test_negative_poll_returns_when_a_wait_is_cut_short(void) {
    struct tt_Node node;
    setup(&node);
    test_mock_receive_advances_clock = false; // the wait ends without the time passing: a signal
    test_mock_receive_limit = 3;
    EXPECT_TRUE(tt_Node_schedule(&node, 50 * tt_MILLISECOND, count_entry, NULL));

    tt_ret_t ret = tt_Node_poll(&node, -1);

    EXPECT_EQ_INT(tt_RET_TIMEOUT, ret);             // handed back, not interrupted by the backstop
    EXPECT_EQ_INT(1, test_mock_receive_call_count); // after the one wait that was cut short
    EXPECT_EQ_INT(0, entry_runs);                   // and the entry, not yet due, did not run early
}

// Everything already due runs before the poll returns - a burst of simultaneous timers costs one return,
// not one per timer - and nothing waits at all. An entry that is not due yet is left alone.
static void test_negative_poll_runs_every_due_entry_then_returns(void) {
    struct tt_Node node;
    setup(&node);
    test_mock_now = 5 * tt_MILLISECOND;
    EXPECT_TRUE(tt_Node_schedule(&node, test_mock_now, count_entry, NULL));
    EXPECT_TRUE(tt_Node_schedule(&node, test_mock_now - 1, count_entry, NULL));
    EXPECT_TRUE(tt_Node_schedule(&node, test_mock_now + (2 * tt_MILLISECOND), count_entry, NULL));

    tt_ret_t ret = tt_Node_poll(&node, -1);

    EXPECT_EQ_INT(tt_RET_TIMEOUT, ret);
    EXPECT_EQ_INT(2, entry_runs);                   // both due entries
    EXPECT_EQ_INT(0, test_mock_receive_call_count); // and no wait was needed
    EXPECT_EQ_INT(1, node.scheduler_tail);          // the later one is still pending
}

// A busy node - an entry that keeps rescheduling itself as due, as a max-rate publisher does - hands
// control back after the old 100us slice of that work, not after the 100ms idle ceiling. Only idle
// waiting got longer. Each run here takes 30us of mock time, so the fourth run crosses 100us.
#define BUSY_RUN_NS (30 * tt_MICROSECOND)
// Stops rescheduling after this many runs, so that losing the cap - the regression this test is for -
// fails as "ran 1000 times" instead of hanging the test binary on a loop that never ends.
#define BUSY_RUN_LIMIT 1000
static void busy_entry(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    entry_runs++;
    test_mock_now += BUSY_RUN_NS;
    if (entry_runs < BUSY_RUN_LIMIT) {
        EXPECT_TRUE(tt_Node_schedule(node, time, busy_entry, NULL)); // due again at once
    }
}
static void test_negative_poll_on_a_busy_node_keeps_the_old_slice(void) {
    struct tt_Node node;
    setup(&node);
    EXPECT_TRUE(tt_Node_schedule(&node, 0, busy_entry, NULL));

    tt_ret_t ret = tt_Node_poll(&node, -1);

    EXPECT_EQ_INT(tt_RET_TIMEOUT, ret);
    EXPECT_EQ_INT(4, entry_runs);                         // 4 x 30us: the first run past 100us
    EXPECT_TRUE(test_mock_now < 10 * tt_RECEIVE_TIMEOUT); // back after the old slice, not later
    EXPECT_EQ_INT(1, node.scheduler_tail);                // and the work is still there for next time
}

// An interrupt still ends a negative-timeout poll at once.
static void test_negative_poll_is_still_interruptible(void) {
    struct tt_Node node;
    setup(&node);
    test_mock_receive_return = -3;
    EXPECT_TRUE(tt_Node_schedule(&node, 50 * tt_MILLISECOND, count_entry, NULL));

    EXPECT_EQ_INT(tt_RET_INTERRUPTED, tt_Node_poll(&node, -1));
    EXPECT_EQ_INT(0, entry_runs);
}

// The control: a POSITIVE timeout is exactly what it was. With an entry due in 3ms inside a 1s budget,
// the poll runs the entry and then keeps waiting out the rest of its budget rather than returning early -
// which is also what distinguishes it from the negative case above.
static void test_positive_poll_is_unchanged(void) {
    struct tt_Node node;
    setup(&node);
    EXPECT_TRUE(tt_Node_schedule(&node, 3 * tt_MILLISECOND, count_entry, NULL));

    tt_ret_t ret = tt_Node_poll(&node, (int64_t)tt_SECOND);

    EXPECT_EQ_INT(tt_RET_TIMEOUT, ret);
    EXPECT_EQ_INT(1, entry_runs);
    EXPECT_EQ_INT(2, test_mock_receive_call_count); // to the entry, then the rest of the budget
    EXPECT_EQ_U64(tt_SECOND - (3 * tt_MILLISECOND), (uint64_t)test_mock_receive_last_timeout);
}

// node_flush() is armed on demand (2026-09-25). It used to reschedule itself every tt_NODE_TX_INTERVAL
// whether or not anything was waiting, so a node was never idle: an exact wait for the next scheduler
// entry still woke a thousand times a second. Measured with strace on the uint64 example subscriber, idle
// for 3s on real sockets: 15,652 ppoll calls before, 9 after.

// Stages one small batched submessage the way a publisher that is not flushing immediately does.
static void stage_one_batched_submessage(struct tt_Node* node) {
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    struct tt_SubmessageHeader* header = start_encode(node, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL);
    EXPECT_TRUE(header != NULL);
    uint8_t* body = (uint8_t*)encode(node, 4);
    EXPECT_TRUE(body != NULL);
    memset(body, 0, 4);
    EXPECT_TRUE(end_encode(node, header, false, NULL, 0)); // batched, not flushed now
}

// The latency the old tick gave a batched submessage was 0 to one interval - it ran on a fixed grid - and
// arming "now + one interval" instead would have made it a full interval every time: +0.5ms on average for
// every batching publisher, hidden inside an efficiency change. So the flush lands on the grid boundary.
static void test_flush_is_armed_on_the_old_grid(void) {
    struct tt_Node node;
    setup(&node);
    test_mock_now = (12 * tt_MILLISECOND) + (300 * tt_MICROSECOND); // 12.3ms: mid-interval
    EXPECT_EQ_INT(0, node.scheduler_tail);                          // nothing armed while nothing waits

    stage_one_batched_submessage(&node);

    EXPECT_TRUE(node.flush_scheduled);
    EXPECT_EQ_INT(1, node.scheduler_tail);
    EXPECT_EQ_U64(13 * tt_MILLISECOND, node.scheduler[0].time); // the next boundary, not 13.3ms
}

// A second batched submessage in the same interval rides the flush already armed; and once the flush has
// sent everything, nothing is left ticking - the node is idle until something is sent again.
static void test_flush_arms_once_and_does_not_tick_when_idle(void) {
    struct tt_Node node;
    setup(&node);
    test_mock_now = 20 * tt_MILLISECOND;

    stage_one_batched_submessage(&node);
    struct tt_SubmessageHeader* header = start_encode(&node, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL);
    EXPECT_TRUE(header != NULL && encode(&node, 4) != NULL);
    EXPECT_TRUE(end_encode(&node, header, false, NULL, 0));
    EXPECT_EQ_INT(1, node.scheduler_tail); // still one flush, not two

    EXPECT_EQ_INT(tt_RET_TIMEOUT, tt_Node_poll(&node, -1)); // waits to the boundary and flushes
    EXPECT_EQ_INT(1, test_mock_send_call_count);            // both submessages, one datagram
    EXPECT_TRUE(!node.flush_scheduled);
    EXPECT_EQ_INT(0, node.scheduler_tail); // and nothing re-armed with the buffer empty
}

int main(void) {
    test_negative_poll_waits_until_the_next_entry();
    test_negative_poll_with_nothing_scheduled_blocks_indefinitely();
    test_negative_poll_waits_exactly_for_a_distant_entry();
    test_negative_poll_returns_when_a_wait_is_cut_short();
    test_negative_poll_runs_every_due_entry_then_returns();
    test_negative_poll_on_a_busy_node_keeps_the_old_slice();
    test_negative_poll_is_still_interruptible();
    test_positive_poll_is_unchanged();
    test_flush_is_armed_on_the_old_grid();
    test_flush_arms_once_and_does_not_tick_when_idle();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_poll_wait: all tests passed\n");
    return 0;
}
