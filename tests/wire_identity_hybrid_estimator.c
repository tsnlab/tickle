/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// tt_InitOptions bundle 7 (tests/wire_identity_hybrid_estimator.sh): the hybrid transport's reader - its loss
// estimates, the control law, rule (e) - fed a fixed stream of 1 MB samples, and a RELIABLE writer's choice of n for a
// failed fragment path. The trace - every datagram the reader sends (its announces: the advertised ceiling and loss_p),
// and at every tick its state (cap, ceiling, tau, the estimates, the decisions' counters) - is written to
// WIRE_CAPTURE_OUT. Built against two trees, the traces are compared byte for byte: at the default options the
// estimator must decide exactly as it did on constants.
//
// One whitebox reader on the mock HAL, holding tt_InitOptions_default() (which both trees have). Only functions whose
// signature both trees share are called: hybrid_rx_note(), hybrid_reader_tick(), hybrid_reliable_n(). A writer (source
// 9) sends 723-unit samples at 30 Hz in runs of the n the reader's announce allows, the tail alone; the clock steps a
// sample at a time and the reader ticks as it falls due. Phases:
//   0. one sample in MTU units, then 60 with no loss - "none seen";
//   1. 2 samples losing about one run each, then 40 clean - the stale rule forgets them;
//   2. 300 samples at 1% frame loss (a run lost with 1 - 0.99^n);
//   3. 300 samples whose runs are lost 60% of the time while MTU units arrive - a reassembly squeeze: collapse, drop,
//      hold, probes; then 300 at the squeeze's rate halved - R falls, the ceiling scales;
//   4. 200 samples at 1% with the receiver dropping a datagram now and then - excused gaps; 900 clean;
//   5. 3 samples of a second writer losing every run with the tail through - rule (e), one unit; then 2,100 clean of
//      the first - its re-probe after 2 tau;
//   6. a writer whose reader's path failed 59 s, 61 s and 121 s ago: MTU, re-probe, re-probe.
// It prints what the phases reached (collapses, probes, rescales, path failures, stale forgets, receiver drops).
// Not a unit test (no test_ prefix): `make test` does not build it; the script builds it against both trees.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; the script passes each tree's tickle.c

#ifndef WIRE_CAPTURE_OUT
#define WIRE_CAPTURE_OUT "/dev/null" // the script names a file; built bare (lint), the trace goes nowhere
#endif

#define CLOCK_NS (100ULL * tt_SECOND)
#define SAMPLE_NS (tt_SECOND / 30U)
#define UNITS 723U // a 1 MB sample
#define WRITER 9
#define ENTITY 1
#define PEER 5
#define BUILD_N_MAX 45U // tt_HYBRID_N_MAX at the defaults: the writer's own bound

static struct tt_Context reader;
static struct tt_Publisher pub;
static FILE* trace;
static uint32_t lcg = 12345U;
static uint32_t seq = 1000U; // the next sample's first seq_no
static uint32_t ticks_seen;
static uint32_t entity = ENTITY; // the writer's endpoint

// A uniform draw in [0, 1).
static double draw(void) {
    lcg = (lcg * 1103515245U) + 12345U;
    return (double)(lcg >> 8) / 16777216.0;
}

// One record: kind, the time since CLOCK_NS, then `len` bytes.
static void record(char kind, const void* bytes, uint32_t len) {
    uint64_t at = test_mock_now - CLOCK_NS;
    fputc(kind, trace);
    fwrite(&at, sizeof(at), 1, trace);
    fwrite(&len, sizeof(len), 1, trace);
    fwrite(bytes, 1, len, trace);
}

static void hook(const void* buf, size_t len) {
    record('D', buf, (uint32_t)len);
}

// What the reader queued to send since the last look - its announces, which a flush timer would send - taken out of
// its transmit buffer as one record (the whitebox reader has no timers running).
static void flush(void) {
    if (reader.tx_tail > sizeof(struct tt_Header)) {
        record('D', reader.tx_buffer, reader.tx_tail);
        reader.tx_tail = sizeof(struct tt_Header);
    }
}

// The reader's state after a tick, field by field (not the struct: padding and fields only one tree has).
static void snapshot(void) {
    const struct tt_HybridState* h = &reader.hybrid;
    uint64_t v[] = {h->cap,
                    h->n_fail,
                    h->advertised,
                    h->advertised_loss,
                    h->lifetime_ns,
                    h->hold_until_ns,
                    h->probe_at_ns,
                    h->probe_since_ns,
                    h->path_until_ns,
                    h->path_cap,
                    h->path_backoff,
                    h->backoff,
                    (uint64_t)(int64_t)h->scale_streak,
                    h->collapses,
                    h->probes,
                    h->probe_failures,
                    h->rescales,
                    h->cap_changes,
                    h->path_failures,
                    h->stale_forgets,
                    h->stale_events,
                    h->receiver_lost,
                    h->missed_samples,
                    h->rx_loss.events,
                    h->rx_loss.frames,
                    h->mtu_loss.events,
                    h->mtu_loss.frames,
                    h->window.sent,
                    h->window.lost,
                    h->window.frames};
    record('S', v, (uint32_t)sizeof(v));
}

// The n the writer sends at: what the reader's announce allows of a 65,507-byte datagram, at most its own bound.
static uint32_t writer_n(void) {
    uint32_t advertised = reader.hybrid.advertised;
    uint32_t units = advertised > HYBRID_RUN_WIRE_FRAMING ? (advertised - HYBRID_RUN_WIRE_FRAMING) / HYBRID_UNIT : 1U;
    units = units < BUILD_N_MAX ? units : BUILD_N_MAX;
    return units < 1U ? 1U : units;
}

// One sample: units [0, UNITS - 1) in runs of n, each lost with `run_loss` (MTU units, n = 1, with `unit_loss`), the
// tail alone, lost with `unit_loss`. `mtu` sends the body as MTU units. Then the clock steps a sample and the reader
// ticks.
static void sample(double run_loss, double unit_loss, bool mtu) {
    uint32_t n = mtu ? 1U : writer_n();
    for (uint32_t start = 0; start + 1U < UNITS; start += n) {
        uint32_t span = UNITS - 1U - start < n ? UNITS - 1U - start : n;
        bool run = span >= 2U;
        if (draw() >= (run ? run_loss : unit_loss)) {
            hybrid_rx_note(&reader, WRITER, entity, seq, start, UNITS, span, run);
        }
    }
    if (draw() >= unit_loss) {
        hybrid_rx_note(&reader, WRITER, entity, seq, UNITS - 1U, UNITS, 1, false);
    }
    seq += UNITS;
    flush();
    test_mock_now += SAMPLE_NS;
    hybrid_reader_tick(&reader, test_mock_now);
    flush();
    if (reader.hybrid.ticks != ticks_seen) {
        ticks_seen = reader.hybrid.ticks;
        snapshot();
    }
}

static double run_loss_at(double p) {
    double clean = 1.0;
    for (uint32_t i = 0; i < writer_n(); i++) {
        clean *= 1.0 - p;
    }
    return 1.0 - clean;
}

static void writer_choice(uint64_t failed_ago) {
    memset(&pub, 0, sizeof(pub));
    pub.peer_acks[0].context_id = PEER;
    pub.peer_watch[0].path_failed_ns = test_mock_now - failed_ago;
    reader.hybrid.peer_loss_p[PEER] = reader.hybrid.advertised_loss;
    bool one_run = false;
    uint64_t v[2];
    v[0] = hybrid_reliable_n(&reader, &pub, PEER, BUILD_N_MAX, &one_run);
    v[1] = one_run ? 1U : 0U;
    record('W', v, (uint32_t)sizeof(v));
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    trace = fopen(WIRE_CAPTURE_OUT, "wb");
    if (trace == NULL) {
        return 2;
    }
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    memset(&reader, 0, sizeof(reader));
    tt_InitOptions_default(&reader.opts);
    node_init_locks(&reader);
    reset_node_state(&reader);
    reader.id = 2;
    reader.tx_tail = sizeof(struct tt_Header);
    reader.tx_size = sizeof(reader.tx_buffer);
    reader.last_modified = CLOCK_NS;
    test_mock_send_hook = hook;

    sample(0.0, 0.0, true); // a RELIABLE AUTO stream's first sample: MTU units
    for (int i = 0; i < 60; i++) {
        sample(0.0, 0.0, false);
    }
    for (int i = 0; i < 2; i++) {
        sample(1.0 / 16.0, 0.0, false); // about one run of each lost
    }
    for (int i = 0; i < 40; i++) {
        sample(0.0, 0.0, false);
    }
    for (int i = 0; i < 300; i++) {
        sample(run_loss_at(0.01), 0.01, false);
    }
    for (int i = 0; i < 300; i++) {
        sample(0.6, 0.001, false);
    }
    for (int i = 0; i < 300; i++) { // R halved: every other sample interval empty
        sample(0.6, 0.001, false);
        test_mock_now += SAMPLE_NS;
    }
    for (int i = 0; i < 200; i++) {
        if (i % 7 == 0) {
            test_mock_receiver_drops++;
        }
        sample(run_loss_at(0.01), 0.01, false);
    }
    for (int i = 0; i < 900; i++) {
        sample(0.0, 0.0, false);
    }
    entity = ENTITY + 1U; // a second writer of the same node, none of whose runs ever arrived
    for (int i = 0; i < 3; i++) {
        sample(1.0, 0.0, false); // every run lost, the tail through
    }
    entity = ENTITY;
    for (int i = 0; i < 2100; i++) {
        sample(0.0, 0.0, false);
    }
    writer_choice(59 * tt_SECOND);
    writer_choice(61 * tt_SECOND);
    writer_choice(121 * tt_SECOND);
    snapshot();
    fclose(trace);
    // What the scenario reached: a phase that decided nothing would leave its counter at 0, and the trace silent on it.
    const struct tt_HybridState* h = &reader.hybrid;
    printf("ticks=%u collapses=%llu probes=%llu probe_failures=%llu rescales=%llu path_failures=%llu "
           "stale_forgets=%llu receiver_lost=%llu cap_changes=%llu\n",
           (unsigned)h->ticks, (unsigned long long)h->collapses, (unsigned long long)h->probes,
           (unsigned long long)h->probe_failures, (unsigned long long)h->rescales, (unsigned long long)h->path_failures,
           (unsigned long long)h->stale_forgets, (unsigned long long)h->receiver_lost,
           (unsigned long long)h->cap_changes);
    return 0;
}
