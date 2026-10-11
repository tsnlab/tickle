/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// fence_ring_insn.c - user-space instructions per record on the segment's steady-state path, for the UDP-to-segment
// fence (tt_SEGMENT_SPAN_FENCE, 2026-10-11). The fence claims to add nothing to a record's path: the writer's side
// runs only in the attach slow path, and the reader's is tested inside the span range check's failing branch and on
// the drain's stall path. The PC's process-wide A/B (fence_samehost_insn_pc.sh) cannot resolve that - a max-rate
// cell's instructions per sample move 5-20% with how often the reader spins - so this counts the path alone, with
// perf_event_open() around the calls, on the mock HAL: no sockets, no other process, no time.
//
// Cases, per record, one ring lap (the segment's slot count) at a time:
//   write    - segment_write() of a 64-byte record, as a publisher's send does (the ring is drained, uncounted,
//              between laps).
//   drain    - drain_own_segment() over a full lap of 64-byte records the receive path rejects at once (bad magic,
//              logging off), so the drain's own machinery is most of what is counted.
//   control  - segment_has_records(), which the change does not touch: its count must not move between builds.
//
// Prints "<case> insn_per_record=<x> records=<n>". Built twice and interleaved by fence_ring_insn.sh.

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

#include "../../../src/log.h" // tt_current_log_level

#define TEST_MOCK_DEFINE_STORAGE
#include "../../../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: the ring functions are static
#include "../../../tests/test_mock.h"

#define LAPS 200U
#define RECORD_BYTES 64U
#define CONTROL_CALLS 100000U
#define SENDER_IP 0x0a000001U
#define SENDER_PORT 12345

static struct tt_Context node;
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

static uint64_t counted(int start) {
    if (start) {
        (void)ioctl(counter, PERF_EVENT_IOC_RESET, 0);
        (void)ioctl(counter, PERF_EVENT_IOC_ENABLE, 0);
        return 0;
    }
    (void)ioctl(counter, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t value = 0;
    if (read(counter, &value, sizeof(value)) != (ssize_t)sizeof(value)) {
        return 0;
    }
    return value;
}

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    test_mock_reset();
    memset(&node, 0, sizeof(node)); // as the whitebox tests set a context up (test_transport_seam.c)
    node_init_locks(&node);
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node.hal.own_ip = SENDER_IP;
    node.hal.own_port = SENDER_PORT + 1;
    create_own_segment(&node);
    struct tt_SegmentHeader* ring = node.own_segment;
    if (ring == NULL) {
        fprintf(stderr, "no segment\n");
        return 1;
    }
    counter_open();
    if (counter < 0) {
        return 1;
    }
    uint8_t record[RECORD_BYTES];
    memset(record, 'X', sizeof(record)); // not a TickLE magic: rejected at once by the receive path
    const uint32_t lap = ring->slots;
    uint64_t write_insn = 0;
    uint64_t drain_insn = 0;
    uint64_t written = 0;
    uint64_t drained = 0;
    for (uint32_t round = 0; round < LAPS; round++) {
        (void)counted(1);
        for (uint32_t i = 0; i < lap; i++) {
            written += segment_write(ring, record, RECORD_BYTES, NULL, 0, SENDER_IP, SENDER_PORT, 1) ? 1U : 0U;
        }
        write_insn += counted(0);
        bool emptied = false;
        (void)counted(1);
        uint32_t taken = 0;
        while (!emptied || taken == 0) {
            uint32_t got = drain_own_segment(&node, &emptied);
            taken += got;
            if (got == 0 && emptied) {
                break;
            }
        }
        drain_insn += counted(0);
        drained += taken;
    }
    (void)counted(1);
    uint32_t any = 0;
    for (uint32_t i = 0; i < CONTROL_CALLS; i++) {
        any += segment_has_records(&node) ? 1U : 0U;
    }
    const uint64_t control_insn = counted(0);
    if (written != (uint64_t)LAPS * lap || drained != written || any != 0) {
        fprintf(stderr, "harness: written=%lu drained=%lu any=%u (want %lu, %lu, 0)\n", (unsigned long)written,
                (unsigned long)drained, any, (unsigned long)LAPS * lap, (unsigned long)written);
        return 1;
    }
    printf("write insn_per_record=%.3f records=%lu\n", (double)write_insn / (double)written, (unsigned long)written);
    printf("drain insn_per_record=%.3f records=%lu\n", (double)drain_insn / (double)drained, (unsigned long)drained);
    printf("control insn_per_record=%.3f records=%u\n", (double)control_insn / CONTROL_CALLS, CONTROL_CALLS);
    return 0;
}
