/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// hybrid_options_insn.c - user-space instructions per 1 MB sample in the hybrid transport's estimator, reader side and
// writer side (2026-10-21, tt_InitOptions bundle 7, ab/initoptions-7-hybrid). The change under test reads the
// estimator's and the control law's settings from the context's options (node->opts.hybrid_transport) where it read
// config.h constants. Instructions by perf_event_open() around the measured calls alone, on the mock HAL, so the count
// is the core's own. Only functions whose signature both trees share are called.
//
// Both contexts hold tt_InitOptions_default() (both trees have it), so the change reads resolved options, as a created
// context does. Counted per sample (723 units, as 1 MB on 8/9):
//   rx      - the reader's hybrid_rx_note() for each datagram of the sample - 16 runs of 45, one of 2, the tail - and
//             hybrid_reader_tick() at 30 Hz (a decision once a second): the per-run hot path, where the window, "none
//             seen" and the stale rule read the options;
//   tx      - the writer's hybrid_destination_n() by broadcast over three RELIABLE AUTO readers at p 1e-3: the per-
//             sample choice of n, where n_max and the failed-path time read the options;
//   control - hybrid_n_star() at the same p, which reads no option. It must be the same in both builds; if it moves,
//             they differ in something besides the change.
// Cases: clean (no loss), loss1 (each run lost with 1 - 0.99^n, a fixed sequence: gaps, events, the stale rule).
// Every case checks its own treatment: the reader's frames grew by the sample's units, and the writer chose n >= 2, or
// the line says VOID. Prints "<case> rx=<x> tx=<y> control=<z> samples=<n>" per case.
//
// Build: hybrid_options_insn.sh (A: the base tickle.c, B: the change), each from a copy of its own tree.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h> // tt_log_set_level, TT_LOG_NONE
#include <tickle/tickle.h>

#define TEST_MOCK_DEFINE_STORAGE
#include "../../../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: the estimator is static
#include "../../../tests/test_mock.h"

#define CLOCK_NS (100ULL * tt_SECOND)
#define SAMPLE_NS (tt_SECOND / 30U)
#define UNITS 723U
#define RUN_N 45U
#define WRITER_ID 9
#define READERS 3
#define SAMPLES 3000U
#define WARMUP 300U
#define P_READER 1e-3
#define LOSS_1 0.01 // the lossy case's frame loss
#define LCG_SEED 12345U
#define LCG_MUL 1103515245U
#define LCG_ADD 12345U
#define LCG_SHIFT 8U
#define LCG_SPAN 16777216.0 // 2^24: the draw's 24 bits in [0, 1)
#define FIRST_SEQ 1000U
#define READER_ID 2
#define PEER_ID_BASE 20
#define PEER_IP_BASE 0x0a000014U
#define PEER_PORT 40000
#define PEER_CEILING 65507U

static struct tt_Context reader;
static struct tt_Context writer;
static struct tt_Publisher pub;
static uint32_t lcg = LCG_SEED;
static uint32_t seq = FIRST_SEQ;
static volatile uint32_t sink; // keeps the control's result alive

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
        exit(1);
    }
}
static uint64_t counter_read(void) {
    uint64_t value = 0;
    if (read(counter, &value, sizeof(value)) != (ssize_t)sizeof(value)) {
        return 0;
    }
    return value;
}

static double draw(void) {
    lcg = (lcg * LCG_MUL) + LCG_ADD;
    return (double)(lcg >> LCG_SHIFT) / LCG_SPAN;
}

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    tt_InitOptions_default(&context->opts);
    node_init_locks(context);
    reset_node_state(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = sizeof(context->tx_buffer);
    context->last_modified = CLOCK_NS;
}

static void setup(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    lcg = LCG_SEED;
    init_context(&reader, READER_ID);
    init_context(&writer, WRITER_ID);
    memset(&pub, 0, sizeof(pub));
    pub.reliable = true;
    pub.large_fragmentation = tt_LARGE_FRAGMENTATION_INHERIT;
    writer.large_fragmentation = tt_LARGE_FRAGMENTATION_AUTO;
    for (int i = 0; i < READERS; i++) {
        uint8_t id = (uint8_t)(PEER_ID_BASE + i);
        pub.peers[i] = (struct tt_Peer) {id, PEER_IP_BASE + (uint32_t)i, PEER_PORT};
        pub.peer_acks[i].context_id = id;
        writer.peer_ip_reassembly_max[id] = PEER_CEILING;
        writer.hybrid.peer_loss_p[id] = hybrid_loss_code(P_READER);
        writer.update_seen[id] = true;
    }
}

// One sample's datagrams to the reader, each run lost with `run_loss`; then the clock steps and the reader ticks.
static void reader_sample(double run_loss) {
    for (uint32_t start = 0; start + 1U < UNITS; start += RUN_N) {
        uint32_t span = UNITS - 1U - start < RUN_N ? UNITS - 1U - start : RUN_N;
        if (run_loss == 0.0 || draw() >= run_loss) {
            hybrid_rx_note(&reader, WRITER_ID, 1, seq, start, UNITS, span, span >= 2U);
        }
    }
    hybrid_rx_note(&reader, WRITER_ID, 1, seq, UNITS - 1U, UNITS, 1, false);
    seq += UNITS;
    test_mock_now += SAMPLE_NS;
    hybrid_reader_tick(&reader, test_mock_now);
    reader.tx_tail = sizeof(struct tt_Header); // an announce queued: dropped, as no flush timer runs here
}

static void run_case(const char* name, double frame_loss) {
    setup();
    double clean = 1.0;
    for (uint32_t i = 0; i < RUN_N; i++) {
        clean *= 1.0 - frame_loss;
    }
    double run_loss = 1.0 - clean;
    for (uint32_t k = 0; k < WARMUP; k++) {
        reader_sample(run_loss);
    }
    uint64_t rx_count = 0;
    uint64_t tx_count = 0;
    uint64_t control = 0;
    bool treated = true;
    (void)ioctl(counter, PERF_EVENT_IOC_RESET, 0);
    (void)ioctl(counter, PERF_EVENT_IOC_ENABLE, 0);
    for (uint32_t k = 0; k < SAMPLES; k++) {
        uint32_t frames = reader.hybrid.rx_loss.frames;
        uint64_t at_rx = counter_read();
        reader_sample(run_loss);
        uint64_t at_tx = counter_read();
        bool one_run = false;
        uint32_t run_n = hybrid_destination_n(&writer, &pub, tt_CONTEXT_ID_INVALID, 0, 0, false, &one_run);
        uint64_t at_control = counter_read();
        sink = hybrid_n_star(P_READER, (double)HYBRID_UNIT, (double)HYBRID_UNIT_HEADER, RUN_N);
        uint64_t at_end = counter_read();
        rx_count += at_tx - at_rx;
        tx_count += at_control - at_tx;
        control += at_end - at_control;
        // Treated: the sample was counted (frames grew, unless the window just halved) and the writer chose runs.
        treated = treated && run_n >= 2U && (reader.hybrid.rx_loss.frames != frames);
    }
    (void)ioctl(counter, PERF_EVENT_IOC_DISABLE, 0);
    if (!treated) {
        printf("%s VOID untreated samples=%u\n", name, SAMPLES);
        return;
    }
    printf("%s rx=%.2f tx=%.2f control=%.2f samples=%u\n", name, (double)rx_count / SAMPLES, (double)tx_count / SAMPLES,
           (double)control / SAMPLES, SAMPLES);
}

int main(void) {
    tt_log_set_level(TT_LOG_NONE);
    counter_open();
    run_case("clean", 0.0);
    run_case("loss1", LOSS_1);
    return 0;
}
