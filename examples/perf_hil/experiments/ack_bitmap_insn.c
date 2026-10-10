/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ack_bitmap_insn.c - user-space instructions per call of update_reliable_ack(), the subscriber's per-DATA reliable
// bookkeeping (2026-10-10). A PC perf profile of a 1 MB RELIABLE KEEP_LAST 10 transfer (~700 datagrams a sample) put
// ~11% of core CPU there: a shift and a scan of the whole receive bitmap per datagram. Instructions, counted by
// perf_event_open() around the calls alone, not time: the claim being tested is about work per datagram, and an
// instruction count does not move with clock frequency or the machine's other load.
//
// Cases, each at a window of W words (4: core's default, 16: rmw_tickle's, 64: the maximum):
//   in_order  - every seq_no is the next expected one: the common case the change is for.
//   gap_open  - the watermark's sample is lost and every later one arrives ahead of it, filling the window: the
//               recovery case, where words are genuinely in use.
//   control   - find_writer_proxy(), which update_reliable_ack() calls and the change does not touch. Its count
//               must be the same in both builds; if it moves, the builds differ in something other than the change.
//
// Prints one line per case: "<case> w=<W> insn_per_call=<x> calls=<n>". Built twice and interleaved by
// ack_bitmap_insn.sh (A: the base tickle.c, B: the working tree).
//
// Build from the repo root:
//   gcc -O2 -DNDEBUG -I include -I src -o /tmp/ack_insn examples/perf_hil/experiments/ack_bitmap_insn.c \
//       src/encoding.c src/log.c -lm

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h> // TT_LOG_NONE
#include <tickle/tickle.h>

#include "log.h" // tt_current_log_level

#define TEST_MOCK_DEFINE_STORAGE
#include "../../../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: update_reliable_ack() is static
#include "../../../tests/test_mock.h"

#define REMOTE_NODE_ID 2
#define SENDER_IP 0x0a000001U
#define SENDER_PORT 12345
#define IN_ORDER_CALLS 200000U
#define CONTROL_CALLS 200000U
#define GAP_FILLS 400U
#define LOCAL_NODE_ID 1
#define ENDPOINT_ID 0xaabbccddU
#define BITS_PER_WORD 64U

static struct tt_Context node;
static struct tt_Topic topic;
static struct tt_Subscriber sub;
static uint64_t tracking[tt_MAX_PEER_COUNT * tt_RELIABLE_BITMAP_MAX_WORDS];
static int counter = -1;

static void counter_open(void) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    counter = (int)syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
    if (counter < 0) {
        perror("perf_event_open");
    }
}

static void counter_start(void) {
    (void)ioctl(counter, PERF_EVENT_IOC_ENABLE, 0);
}

static uint64_t counter_stop(void) {
    (void)ioctl(counter, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t value = 0;
    if (read(counter, &value, sizeof(value)) != (ssize_t)sizeof(value)) {
        return 0;
    }
    return value;
}

static void counter_reset(void) {
    (void)ioctl(counter, PERF_EVENT_IOC_RESET, 0);
}

static void rig_init(uint16_t words) {
    test_mock_reset();
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = LOCAL_NODE_ID;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node.rx_seq_span = 1; // what the socket path sets for every datagram
    memset(&topic, 0, sizeof(topic));
    topic.name = "bench";
    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = ENDPOINT_ID;
    sub.node = &node;
    sub.topic = &topic;
    sub.reliable = true;
    if (words != tt_RELIABLE_BITMAP_WORDS) {
        sub.tracking_bitmaps = tracking;
        sub.tracking_words = words;
    }
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub.writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&sub;
    // First contact, outside the count: the proxy is created and its watermark set.
    (void)update_reliable_ack(&node, &sub, 1, REMOTE_NODE_ID, 0, SENDER_IP, SENDER_PORT);
    find_writer_proxy(&sub, REMOTE_NODE_ID, 0)->keep_all = tt_WRITER_KEEP_ALL_NO;
}

static volatile uint32_t sink;

static void in_order(uint16_t words) {
    rig_init(words);
    counter_reset();
    counter_start();
    for (uint32_t i = 0; i < IN_ORDER_CALLS; i++) {
        sink += update_reliable_ack(&node, &sub, 2 + i, REMOTE_NODE_ID, 0, SENDER_IP, SENDER_PORT) ? 1U : 0U;
    }
    uint64_t insn = counter_stop();
    printf("in_order w=%u insn_per_call=%.2f calls=%u\n", (unsigned)words, (double)insn / IN_ORDER_CALLS,
           IN_ORDER_CALLS);
}

static void gap_open(uint16_t words) {
    uint32_t per_fill = ((uint32_t)words * BITS_PER_WORD) - 1U; // offsets 1 .. window-1, the watermark's sample missing
    uint64_t insn = 0;
    for (uint32_t fill = 0; fill < GAP_FILLS; fill++) {
        rig_init(words); // watermark at 2 after first contact; 2 never arrives
        counter_reset();
        counter_start();
        for (uint32_t i = 0; i < per_fill; i++) {
            sink += update_reliable_ack(&node, &sub, 3 + i, REMOTE_NODE_ID, 0, SENDER_IP, SENDER_PORT) ? 1U : 0U;
        }
        insn += counter_stop();
        struct tt_WriterProxy* proxy = find_writer_proxy(&sub, REMOTE_NODE_ID, 0);
        if (proxy->acknack_scheduled) {
            (void)tt_Context_unschedule(&node, acknack_retry, proxy);
        }
    }
    printf("gap_open w=%u insn_per_call=%.2f calls=%u\n", (unsigned)words,
           (double)insn / ((double)per_fill * GAP_FILLS), per_fill * GAP_FILLS);
}

static void control(uint16_t words) {
    rig_init(words);
    counter_reset();
    counter_start();
    for (uint32_t i = 0; i < CONTROL_CALLS; i++) {
        sink += find_writer_proxy(&sub, REMOTE_NODE_ID, i & 1U) != NULL ? 1U : 0U;
    }
    uint64_t insn = counter_stop();
    printf("control w=%u insn_per_call=%.2f calls=%u\n", (unsigned)words, (double)insn / CONTROL_CALLS, CONTROL_CALLS);
}

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    counter_open();
    if (counter < 0) {
        return 1;
    }
    static const uint16_t widths[] = {tt_RELIABLE_BITMAP_WORDS, 16, tt_RELIABLE_BITMAP_MAX_WORDS};
    for (size_t width = 0; width < sizeof(widths) / sizeof(widths[0]); width++) {
        in_order(widths[width]);
        gap_open(widths[width]);
        control(widths[width]);
    }
    return 0;
}
