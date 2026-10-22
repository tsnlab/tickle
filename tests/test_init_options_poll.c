/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions, bundle 6: the poll group (struct tt_PollOptions), through the mock HAL.
//   - the defaults are the config.h constants, and a context created with the options holds them resolved;
//   - every field, set to a value other than its default, changes what tt_Context_poll() does - the controls are the
//     same scenario at the default and with the options left 0 (a context built by hand, node_init_locks() resolves):
//       rx_clock_refresh         how old the last datagram's stamp is at the end of a long drain,
//       rx_lock_chunk            how many times a backlog takes the state lock,
//       receive_timeout          how much back-to-back scheduler work a negative-timeout poll runs before it returns,
//       rx_check_ns              how long scheduler entries run before the poll looks at the socket, since it last
//                                looked - a check or a wait, in this call or an earlier one;
//   - every check refuses the value past its bound with tt_RET_INVALID_ARGUMENT and accepts the bound itself
//     (tests/mutants_init_options_poll.sh removes each check and expects this test to fail).
// The Linux HAL's reading of receive_timeout (the idle-socket recheck) is tests/test_init_options_socket.c's.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_poll.sh passes a mutant

#define STAMP_SOURCE 2
#define BACKLOG 40                  // datagrams queued behind the first
#define ARRIVAL_STEP tt_MICROSECOND // each a microsecond after the one before
#define BUSY_RUN_NS tt_MICROSECOND  // what one run of the busy entry costs in mock time
#define BUSY_RUN_LIMIT 5000         // the busy entry stops rescheduling itself here: a lost bound fails, never hangs
#define LONG_SLICE (300 * tt_MICROSECOND)

static struct tt_Context node;
static int entry_runs = 0;

// For a creation: the mock's address auto-detection answers an id, and every link resolves as an interface would.
static void reset_mock_for_create(void) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
}

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts;
}

// A context built as tests/test_poll_wait.c builds one, with the poll group a creation would have given it. NULL
// leaves the options 0 - a context built by hand, which node_init_locks() gives the defaults.
static void setup(const struct tt_PollOptions* poll) {
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    test_mock_reset();
    test_mock_receive_advances_clock = true;
    entry_runs = 0;
    if (poll != NULL) {
        node.opts.poll = *poll;
    }
}

static int32_t write_datagram_from(uint8_t* buffer, uint8_t source) {
    struct tt_Header* header = (struct tt_Header*)buffer;
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = source;
    struct tt_SubmessageHeader* submessage = (struct tt_SubmessageHeader*)(buffer + sizeof(*header));
    submessage->type = tt_SUBMESSAGE_TYPE_DATA;
    submessage->receiver = tt_SUBMESSAGE_ID_ALL;
    submessage->length = (uint16_t)(sizeof(*submessage) + sizeof(struct tt_DataHeader));
    struct tt_DataHeader* data = (struct tt_DataHeader*)(buffer + sizeof(*header) + sizeof(*submessage));
    memset(data, 0, sizeof(*data));
    data->endpoint_id = 0x1234; // nobody here subscribes: stamped, then dropped
    return (int32_t)(sizeof(*header) + sizeof(*submessage) + sizeof(*data));
}

// ---- The defaults.

static void test_defaults_are_the_compiled_constants(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32(tt_RX_CLOCK_REFRESH, opts.poll.rx_clock_refresh);
    EXPECT_EQ_U32(tt_RX_LOCK_CHUNK, opts.poll.rx_lock_chunk);
    EXPECT_EQ_U64(tt_RECEIVE_TIMEOUT, opts.poll.receive_timeout);
    EXPECT_EQ_U64(tt_RECEIVE_TIMEOUT, opts.poll.rx_check_ns); // the slice

    // Created with options: the context holds them, resolved, and a zero where a value was not given.
    reset_mock_for_create();
    struct tt_InitOptions given;
    memset(&given, 0, sizeof(given));
    given.poll.rx_lock_chunk = 3;
    given.poll.receive_timeout = LONG_SLICE;
    struct tt_Context created;
    memset(&created, 0, sizeof(created));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&created, &given));
    EXPECT_EQ_U32(tt_RX_CLOCK_REFRESH, created.opts.poll.rx_clock_refresh);
    EXPECT_EQ_U32(3, created.opts.poll.rx_lock_chunk);
    EXPECT_EQ_U64(LONG_SLICE, created.opts.poll.receive_timeout);
    EXPECT_EQ_U64(LONG_SLICE, created.opts.poll.rx_check_ns); // the slice it was given
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));
}

// ---- rx_clock_refresh and rx_lock_chunk: one backlog drained, BACKLOG datagrams each ARRIVAL_STEP after the last.

struct drain_result {
    uint64_t stamp_age; // how much older than the last datagram's arrival its sender's stamp is
    uint64_t locks;     // takings of the state lock during the poll
};

static struct drain_result drain_backlog(const struct tt_PollOptions* poll) {
    setup(poll);
    node.id = 1;
    test_mock_now = tt_SECOND;
    int32_t len = write_datagram_from(node.rx_buffer, STAMP_SOURCE);
    test_mock_receive_return = len;
    test_mock_try_receive_remaining = BACKLOG;
    test_mock_try_receive_len = len;
    test_mock_try_receive_advance_ns = ARRIVAL_STEP;
    uint64_t locks_before = node.state_lock_stats.acquisitions;
    (void)tt_Context_poll(&node, -1);
    EXPECT_EQ_INT(0, test_mock_try_receive_remaining); // the whole backlog, in every arm
    struct drain_result result;
    result.stamp_age = tt_SECOND + (BACKLOG * ARRIVAL_STEP) - node.traffic_last_seen[STAMP_SOURCE];
    result.locks = node.state_lock_stats.acquisitions - locks_before;
    return result;
}

static void test_rx_clock_refresh_is_how_often_a_drain_reads_the_clock(void) {
    struct tt_PollOptions poll = defaults().poll;
    struct drain_result fallback = drain_backlog(NULL); // a context built by hand: the default
    struct drain_result standard = drain_backlog(&poll);
    poll.rx_clock_refresh = 1;
    struct drain_result every = drain_backlog(&poll);
    poll.rx_clock_refresh = tt_POLL_COUNT_MAX;
    struct drain_result never = drain_backlog(&poll);

    // Default (16): the last read came within 16 datagrams of the end, and not at it - 40 is not a multiple.
    EXPECT_EQ_U64(fallback.stamp_age, standard.stamp_age);
    EXPECT_TRUE(standard.stamp_age > 0 && standard.stamp_age < tt_RX_CLOCK_REFRESH * ARRIVAL_STEP);
    // 1: the clock read for every datagram, the stamp the last one's own arrival.
    EXPECT_EQ_U64(0, every.stamp_age);
    // Past the backlog: read once, when the wait returned with the first.
    EXPECT_EQ_U64(BACKLOG * ARRIVAL_STEP, never.stamp_age);
}

static void test_rx_lock_chunk_is_the_datagrams_under_one_lock(void) {
    struct tt_PollOptions poll = defaults().poll;
    struct drain_result fallback = drain_backlog(NULL);
    struct drain_result standard = drain_backlog(&poll);
    poll.rx_lock_chunk = 1;
    struct drain_result single = drain_backlog(&poll);
    poll.rx_lock_chunk = BACKLOG;
    struct drain_result whole = drain_backlog(&poll);

    EXPECT_EQ_U64(fallback.locks, standard.locks);
    // One taking per datagram at 1, at least BACKLOG; the default's chunks of 8 far fewer; one chunk the fewest.
    EXPECT_TRUE(single.locks >= BACKLOG);
    EXPECT_TRUE(standard.locks < single.locks);
    EXPECT_TRUE(standard.locks >= BACKLOG / tt_RX_LOCK_CHUNK);
    EXPECT_TRUE(whole.locks < standard.locks);
}

// ---- receive_timeout and rx_check_ns: an entry that is due again as soon as it has run.

static void busy_entry(struct tt_Context* context, uint64_t time, void* param) {
    (void)param;
    entry_runs++;
    test_mock_now += BUSY_RUN_NS;
    if (entry_runs < BUSY_RUN_LIMIT) {
        EXPECT_TRUE(tt_Context_schedule(context, time, busy_entry, NULL));
    }
}

// Scheduler runs before a negative-timeout poll on a busy context returns, nothing arriving.
static int busy_runs(const struct tt_PollOptions* poll) {
    setup(poll);
    EXPECT_TRUE(tt_Context_schedule(&node, 0, busy_entry, NULL));
    EXPECT_EQ_INT(tt_RET_TIMEOUT, tt_Context_poll(&node, -1));
    return entry_runs;
}

static void test_receive_timeout_is_the_busy_slice(void) {
    struct tt_PollOptions poll = defaults().poll;
    const int fallback = busy_runs(NULL);
    const int standard = busy_runs(&poll);
    poll.receive_timeout = LONG_SLICE;
    const int longer = busy_runs(&poll);
    poll.receive_timeout = 10 * tt_MICROSECOND;
    const int shorter = busy_runs(&poll);

    EXPECT_EQ_INT(fallback, standard);
    EXPECT_EQ_INT((int)(tt_RECEIVE_TIMEOUT / BUSY_RUN_NS), standard); // the first run past the slice ends it
    EXPECT_EQ_INT((int)(LONG_SLICE / BUSY_RUN_NS), longer);
    EXPECT_EQ_INT(10, shorter);
}

// Scheduler runs, at BUSY_RUN_NS each, before the poll's forced look at the socket finds the one datagram waiting there
// and returns - the poll having last looked just before it began (a look `looked_ago` before the poll; 0: at its
// start). A long positive timeout, so the busy slice of a negative one does not end the poll first.
static int runs_before_a_look(const struct tt_PollOptions* poll) {
    setup(poll);
    node.id = 1;
    test_mock_now = 10U * tt_MILLISECOND;
    node.rx_looked_ns = test_mock_now;
    int32_t len = write_datagram_from(node.rx_buffer, STAMP_SOURCE);
    test_mock_try_receive_remaining = 1; // what the look reads; tt_receive() is never reached
    test_mock_try_receive_len = len;
    EXPECT_TRUE(tt_Context_schedule(&node, 0, busy_entry, NULL));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_poll(&node, 100 * (int64_t)tt_MILLISECOND));
    EXPECT_EQ_INT(0, test_mock_try_receive_remaining);
    return entry_runs;
}

static void test_rx_check_ns_is_the_time_before_a_look(void) {
    struct tt_PollOptions poll = defaults().poll;
    const int fallback = runs_before_a_look(NULL);
    const int standard = runs_before_a_look(&poll);
    poll.rx_check_ns = 3U * BUSY_RUN_NS;
    const int sooner = runs_before_a_look(&poll);
    poll.rx_check_ns = 250U * BUSY_RUN_NS; // past the 100 us slice: a positive timeout has none
    const int later = runs_before_a_look(&poll);

    EXPECT_EQ_INT(fallback, standard);
    EXPECT_EQ_INT((int)(tt_RECEIVE_TIMEOUT / BUSY_RUN_NS), standard); // the slice, in runs
    EXPECT_EQ_INT(3, sooner);
    EXPECT_EQ_INT(250, later);
}

// The look is kept across calls: a caller polling with timeouts shorter than rx_check_ns, an entry always due, is
// looked for once the calls together have run rx_check_ns - here the default 100 us, in polls of 20 us. A look counted
// from each call's start would never come (the count of entries it replaced did, after 8). Polls until the datagram is
// read.
static void test_short_polls_look_across_calls(void) {
    setup(NULL);
    node.id = 1;
    test_mock_now = 10U * tt_MILLISECOND;
    node.rx_looked_ns = test_mock_now;
    int32_t len = write_datagram_from(node.rx_buffer, STAMP_SOURCE);
    test_mock_try_receive_remaining = 1;
    test_mock_try_receive_len = len;
    EXPECT_TRUE(tt_Context_schedule(&node, 0, busy_entry, NULL));
    int polls = 0;
    while (test_mock_try_receive_remaining > 0 && polls < 50) {
        (void)tt_Context_poll(&node, 20 * (int64_t)tt_MICROSECOND);
        polls++;
    }
    EXPECT_EQ_INT(0, test_mock_try_receive_remaining);
    // The fifth call ends at the 100 us its timeout and the look reach together, and the sixth runs one entry before
    // its first check (an entry between two looks): 101.
    EXPECT_EQ_INT((int)(tt_RECEIVE_TIMEOUT / BUSY_RUN_NS) + 1, entry_runs);
    EXPECT_EQ_INT(6, polls);
}

// A wait is a look, and so is a check that finds nothing: after a poll that waited, a busy poll runs a whole
// rx_check_ns before it checks; and a busy poll with nothing ever arriving checks once per rx_check_ns, not on every
// step after the first. The control for both is the first assertion of the test above (a look just before).
static void test_a_wait_and_an_empty_check_are_looks(void) {
    setup(NULL);
    node.id = 1;
    test_mock_now = 10U * tt_MILLISECOND;
    node.rx_looked_ns = 0;                                                               // long ago
    EXPECT_EQ_INT(tt_RET_TIMEOUT, tt_Context_poll(&node, 20 * (int64_t)tt_MICROSECOND)); // nothing scheduled: waits
    int32_t len = write_datagram_from(node.rx_buffer, STAMP_SOURCE);
    test_mock_try_receive_remaining = 1;
    test_mock_try_receive_len = len;
    entry_runs = 0;
    EXPECT_TRUE(tt_Context_schedule(&node, 0, busy_entry, NULL));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_poll(&node, 100 * (int64_t)tt_MILLISECOND));
    EXPECT_EQ_INT((int)(tt_RECEIVE_TIMEOUT / BUSY_RUN_NS), entry_runs); // counted from the wait's end, not long ago

    // The same after a wait that ended with a datagram (20 us in): counted from the datagram, not from long ago.
    setup(NULL);
    node.id = 1;
    test_mock_now = 10U * tt_MILLISECOND;
    node.rx_looked_ns = 0;
    test_mock_receive_return = write_datagram_from(RX_LANDING(&node), STAMP_SOURCE);
    test_mock_receive_data_advance_ns = 20U * tt_MICROSECOND;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_poll(&node, tt_MILLISECOND)); // nothing scheduled: waits, and the datagram
    test_mock_receive_return = -1;
    test_mock_receive_data_advance_ns = 0;
    len = write_datagram_from(node.rx_buffer, STAMP_SOURCE);
    test_mock_try_receive_remaining = 1;
    test_mock_try_receive_len = len;
    entry_runs = 0;
    EXPECT_TRUE(tt_Context_schedule(&node, 0, busy_entry, NULL));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_poll(&node, 100 * (int64_t)tt_MILLISECOND));
    EXPECT_EQ_INT((int)(tt_RECEIVE_TIMEOUT / BUSY_RUN_NS), entry_runs);

    // Nothing arrives: 1,000 runs at 1 us check about ten times at 100 us.
    setup(NULL);
    node.id = 1;
    test_mock_now = 10U * tt_MILLISECOND;
    node.rx_looked_ns = test_mock_now;
    EXPECT_TRUE(tt_Context_schedule(&node, 0, busy_entry, NULL));
    (void)tt_Context_poll(&node, 1000 * (int64_t)tt_MICROSECOND);
    printf("  busy poll of 1 ms, nothing arriving: %d runs, %d checks\n", entry_runs, test_mock_try_receive_calls);
    EXPECT_TRUE(entry_runs >= 900);
    EXPECT_TRUE(test_mock_try_receive_calls >= 9 && test_mock_try_receive_calls <= 11);
}

// ---- The checks: each bound refused one past it and accepted at it.

static void expect_created(const struct tt_InitOptions* opts, tt_ret_t expected, const char* what) {
    reset_mock_for_create();
    struct tt_Context created;
    memset(&created, 0, sizeof(created));
    tt_ret_t ret = tt_Context_create_with(&created, opts);
    if (ret != expected) {
        fprintf(stderr, "%s: returned %d, expected %d\n", what, (int)ret, (int)expected);
        test_failures++;
    }
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));
    }
}

static void test_validation(void) {
    struct tt_InitOptions opts = defaults();
    opts.poll.rx_clock_refresh = tt_POLL_COUNT_MAX + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "poll.rx_clock_refresh above the bound");
    opts.poll.rx_clock_refresh = tt_POLL_COUNT_MAX;
    expect_created(&opts, tt_RET_OK, "poll.rx_clock_refresh at the bound");

    opts = defaults();
    opts.poll.rx_lock_chunk = tt_POLL_COUNT_MAX + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "poll.rx_lock_chunk above the bound");
    opts.poll.rx_lock_chunk = tt_POLL_COUNT_MAX;
    expect_created(&opts, tt_RET_OK, "poll.rx_lock_chunk at the bound");

    opts = defaults();
    opts.poll.receive_timeout = tt_POLL_RECEIVE_TIMEOUT_MAX + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "poll.receive_timeout above the bound");
    opts.poll.receive_timeout = tt_POLL_RECEIVE_TIMEOUT_MAX;
    expect_created(&opts, tt_RET_OK, "poll.receive_timeout at the bound");

    opts = defaults();
    opts.poll.rx_check_ns = tt_POLL_RECEIVE_TIMEOUT_MAX + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "poll.rx_check_ns above the bound");
    opts.poll.rx_check_ns = tt_POLL_RECEIVE_TIMEOUT_MAX;
    expect_created(&opts, tt_RET_OK, "poll.rx_check_ns at the bound");
}

// However short rx_check_ns is against the loop's own step, an entry runs between two checks: 1 ns, each clock read
// 10 ns later than the last, nothing arriving, a poll of 100 us. Without the entry between, every step past the first
// is a check and the due entry never runs again.
static void test_an_entry_runs_between_two_checks(void) {
    struct tt_PollOptions poll = defaults().poll;
    poll.rx_check_ns = 1;
    setup(&poll);
    node.id = 1;
    test_mock_now = 10U * tt_MILLISECOND;
    node.rx_looked_ns = test_mock_now;
    test_mock_clock_step_ns = 10;
    EXPECT_TRUE(tt_Context_schedule(&node, 0, busy_entry, NULL));
    (void)tt_Context_poll(&node, 100 * (int64_t)tt_MICROSECOND);
    test_mock_clock_step_ns = 0;
    printf("  rx_check_ns 1, poll of 100 us: %d runs, %d checks\n", entry_runs, test_mock_try_receive_calls);
    EXPECT_TRUE(entry_runs >= 50);
    EXPECT_TRUE(test_mock_try_receive_calls <= entry_runs + 1);
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_defaults_are_the_compiled_constants();
    test_rx_clock_refresh_is_how_often_a_drain_reads_the_clock();
    test_rx_lock_chunk_is_the_datagrams_under_one_lock();
    test_receive_timeout_is_the_busy_slice();
    test_rx_check_ns_is_the_time_before_a_look();
    test_short_polls_look_across_calls();
    test_a_wait_and_an_empty_check_are_looks();
    test_an_entry_runs_between_two_checks();
    test_validation();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_poll: all tests passed\n");
    return 0;
}
