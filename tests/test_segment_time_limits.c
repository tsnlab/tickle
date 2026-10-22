/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROADMAP 5a: the counts that stood in for times, as times kept off the hot path (DESIGN.md section 10, "Lifecycle").
//
// A writer's cached answer about a peer's segment - "no segment", or an attached one - is asked again after a time,
// not after a count of sends: tt_SEGMENT_ATTACH_RETRY_SENDS (256) bounded the delay in datagrams, which is the
// sender's rate, and held a late binder off shared memory for four minutes at 1 Hz; tt_SEGMENT_REVALIDATE_SENDS
// (4096) wrote a dead owner's ring for over an hour at 1 Hz and cost every send a decrement and a store. The time is
// kept by the context's summary tick (node_update() -> segment_recheck_sweep()) at the poll's own reading, so a send
// reads one byte (recheck_due) and no clock: the version on ab/hotpath-split checked the deadline on the send path
// against a 64-bit clock every poll and every publish wrote, cost the rig's P1 cross-host latency +2.7% and did not
// link on RV32 FreeRTOS.
//
// Each test drives the clock by hand (the sweep takes its `now`), and each condition it decides is removed by one
// mutant in tests/mutants_segment_time_limits.py, which must make this file fail:
//   - at a standing clock any number of sends ask once (the send path asks only when due);
//   - the tick that first sees an answer stamps it and does not make it due; a tick one unit short of the deadline
//     does not; the tick at the deadline does (the stamp, the comparison, the due flag);
//   - the summary tick is what ages the answers (node_update() calls the sweep);
//   - an answer asked again and refused again stands again, from a new stamp (remember_absent() resets both flags);
//   - an announce from a peer at our own address makes a "no" due at once (note_same_host_peer());
//   - the same for a "yes": ten thousand sends at a standing clock re-attach nothing, the deadline tick re-attaches,
//     at segment.revalidate_ns and not at the "no"'s interval, and the sweep ages attached entries at all;
//   - an attach that follows a "no" is a new answer, stamped by the next tick, not the "no"'s due flag or deadline.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: the sweep and the cache are static
#include "test_mock.h"

#define OWNER_ID 7
#define OWNER_IP 0x0a4d0002U
#define OWNER_PORT 8282
#define WRITER_ID 2
#define WRITER_IP 0x0a000002U
#define WRITER_PORT 7000
#define UNIT_NS (1ULL << 20U) // the deadline's unit (SEGMENT_RECHECK_SHIFT)

#if tt_SEGMENT_ENABLED

static struct tt_Context ctx;

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts;
}

// A whitebox writer holding `opts` (tt_InitOptions_default() resolves the segment times), at its own address.
static void writer_with(struct tt_Context* node, const struct tt_InitOptions* opts, uint32_t ip) {
    test_mock_reset();
    test_mock_segments_free();
    memset(node, 0, sizeof(*node));
    node->opts = *opts;
    node_init_locks(node);
    node->id = WRITER_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node->hal.own_ip = ip;
    node->hal.own_port = WRITER_PORT;
}

static int asks(void) {
    return test_mock_segment_attach_calls;
}

// An owner whose segment the writer can attach, at OWNER_IP (whitebox, as tests/test_transport_seam.c builds one).
static void make_owner(struct tt_Context* owner, const struct tt_InitOptions* opts) {
    memset(owner, 0, sizeof(*owner));
    owner->opts = *opts;
    node_init_locks(owner);
    owner->id = OWNER_ID;
    owner->entity_id_base = 0x12345678U;
    owner->hal.own_ip = OWNER_IP;
    owner->hal.own_port = OWNER_PORT;
    create_own_segment(owner);
    EXPECT_TRUE(owner->own_segment != NULL);
}

static uint32_t attaches(const struct tt_Context* writer) {
    return writer->segment_attach[tt_SEGMENT_ATTACHED];
}

static void send_to_attached(struct tt_Context* writer, const struct tt_Context* owner) {
    EXPECT_TRUE(peer_segment(writer, OWNER_ID, OWNER_IP, OWNER_PORT) == owner->own_segment);
}

static void send_to_owner(struct tt_Context* node, uint32_t owner_ip) {
    EXPECT_TRUE(peer_segment(node, OWNER_ID, owner_ip, OWNER_PORT) == NULL); // no owner exists: always "no"
}

// ---- A "no" (a peer with no segment for us).

// At a standing clock, with no summary tick, ten thousand sends ask once: a count of 256 would have asked 40 times.
static void test_a_no_stands_however_many_sends(void) {
    struct tt_InitOptions opts = defaults();
    struct tt_Context writer;
    writer_with(&writer, &opts, WRITER_IP);
    for (int i = 0; i < 10000; i++) {
        send_to_owner(&writer, OWNER_IP);
    }
    EXPECT_EQ_INT(1, asks());
    EXPECT_EQ_U32(1, writer.segment_attach[tt_SEGMENT_ABSENT]);
}

// The stamp and the deadline, on the unit grid so the boundary is exact: retry = 64 units. The stamp tick does not make
// the answer due, nor a tick one unit short of the deadline, and the tick at the deadline does - and then only one
// send asks. The control is the first arm: the same sends with no tick at all never ask again.
static void test_a_no_falls_due_on_the_tick_at_its_deadline(void) {
    struct tt_InitOptions opts = defaults();
    opts.segment.attach_retry_ns = 64U * UNIT_NS;
    struct tt_Context writer;
    writer_with(&writer, &opts, WRITER_IP);
    send_to_owner(&writer, OWNER_IP);
    EXPECT_EQ_INT(1, asks());

    const uint64_t stamp = 4096U * UNIT_NS;
    segment_recheck_sweep(&writer, stamp);
    send_to_owner(&writer, OWNER_IP);
    EXPECT_EQ_INT(1, asks()); // stamped, not due
    segment_recheck_sweep(&writer, stamp + (63U * UNIT_NS));
    send_to_owner(&writer, OWNER_IP);
    EXPECT_EQ_INT(1, asks()); // one unit short
    segment_recheck_sweep(&writer, stamp + (64U * UNIT_NS));
    send_to_owner(&writer, OWNER_IP);
    EXPECT_EQ_INT(2, asks()); // at the deadline: asked again
    send_to_owner(&writer, OWNER_IP);
    EXPECT_EQ_INT(2, asks()); // refused again, so it stands again: one ask, not one per send

    // And from a new stamp: the tick at the old deadline, seen again, stamps the new answer and leaves it standing.
    segment_recheck_sweep(&writer, stamp + (64U * UNIT_NS));
    send_to_owner(&writer, OWNER_IP);
    EXPECT_EQ_INT(2, asks());
    segment_recheck_sweep(&writer, stamp + (128U * UNIT_NS));
    send_to_owner(&writer, OWNER_IP);
    EXPECT_EQ_INT(3, asks());
}

// The summary tick is what ages the answers: node_update() at two times a retry interval apart, on a created context.
static void test_the_summary_tick_ages_the_answers(void) {
    test_mock_reset();
    test_mock_segments_free();
    test_mock_now = 5U * tt_SECOND;
    test_mock_node_id = 1;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    struct tt_InitOptions opts = defaults();
    memset(&ctx, 0, sizeof(ctx));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&ctx, &opts));
    const int before = asks();
    send_to_owner(&ctx, OWNER_IP);
    EXPECT_EQ_INT(before + 1, asks());
    node_update(&ctx, test_mock_now, NULL);
    send_to_owner(&ctx, OWNER_IP);
    EXPECT_EQ_INT(before + 1, asks()); // stamped
    node_update(&ctx, test_mock_now + tt_SEGMENT_ATTACH_RETRY_NS, NULL);
    send_to_owner(&ctx, OWNER_IP);
    EXPECT_EQ_INT(before + 2, asks()); // due, and asked
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
}

// An announce from a peer at our own address clears its "no" at once: that peer is about to have a segment (it builds
// one when it hears of us). The control: a send before the announce does not ask.
static void test_a_same_host_announce_makes_a_no_due(void) {
    struct tt_InitOptions opts = defaults();
    struct tt_Context writer;
    writer_with(&writer, &opts, OWNER_IP); // on the owner's host
    send_to_owner(&writer, OWNER_IP);
    send_to_owner(&writer, OWNER_IP);
    EXPECT_EQ_INT(1, asks());
    note_same_host_peer(&writer, OWNER_ID, OWNER_IP);
    const int after_announce = asks(); // building our own segment asks nothing of the mock's attach
    send_to_owner(&writer, OWNER_IP);
    EXPECT_EQ_INT(after_announce + 1, asks());
    release_segments(&writer);
    test_mock_segments_free();
}

// ---- A "yes" (an attached segment).

// Ten thousand sends at a standing clock re-attach nothing: a count of 4096 would have re-attached twice.
static void test_a_yes_stands_however_many_sends(void) {
    struct tt_InitOptions opts = defaults();
    struct tt_Context writer;
    struct tt_Context owner;
    writer_with(&writer, &opts, WRITER_IP);
    make_owner(&owner, &opts);
    for (int i = 0; i < 10000; i++) {
        send_to_attached(&writer, &owner);
    }
    EXPECT_EQ_U32(1, attaches(&writer));
    release_segments(&writer);
    release_own_segment(&owner);
    test_mock_segments_free();
}

// The deadline of a "yes" is segment.revalidate_ns (32 units here), not the "no"'s (64): the stamp tick and the tick a
// unit short re-attach nothing, the tick at 32 units does, and once.
static void test_a_yes_falls_due_on_the_tick_at_its_deadline(void) {
    struct tt_InitOptions opts = defaults();
    opts.segment.attach_retry_ns = 64U * UNIT_NS;
    opts.segment.revalidate_ns = 32U * UNIT_NS;
    struct tt_Context writer;
    struct tt_Context owner;
    writer_with(&writer, &opts, WRITER_IP);
    make_owner(&owner, &opts);
    send_to_attached(&writer, &owner);
    const uint64_t stamp = 8192U * UNIT_NS;
    segment_recheck_sweep(&writer, stamp);
    send_to_attached(&writer, &owner);
    EXPECT_EQ_U32(1, attaches(&writer)); // stamped
    segment_recheck_sweep(&writer, stamp + (31U * UNIT_NS));
    send_to_attached(&writer, &owner);
    EXPECT_EQ_U32(1, attaches(&writer)); // a unit short
    segment_recheck_sweep(&writer, stamp + (32U * UNIT_NS));
    send_to_attached(&writer, &owner);
    EXPECT_EQ_U32(2, attaches(&writer)); // revalidated
    send_to_attached(&writer, &owner);
    EXPECT_EQ_U32(2, attaches(&writer)); // and the new answer stands
    release_segments(&writer);
    release_own_segment(&owner);
    test_mock_segments_free();
}

// A "no" that falls due and is then answered "yes" - the owner appeared meanwhile - is a new answer: not due (no
// re-attach on the next send), and stamped by the next tick rather than holding the "no"'s passed deadline.
static void test_an_attach_after_a_no_is_a_new_answer(void) {
    struct tt_InitOptions opts = defaults();
    opts.segment.attach_retry_ns = 16U * UNIT_NS;
    opts.segment.revalidate_ns = 32U * UNIT_NS;
    struct tt_Context writer;
    writer_with(&writer, &opts, WRITER_IP);
    send_to_owner(&writer, OWNER_IP);
    const uint64_t stamp = 1024U * UNIT_NS;
    segment_recheck_sweep(&writer, stamp);
    segment_recheck_sweep(&writer, stamp + (16U * UNIT_NS)); // the "no" is due
    struct tt_Context owner;
    make_owner(&owner, &opts);
    send_to_attached(&writer, &owner);
    EXPECT_EQ_U32(1, attaches(&writer));
    send_to_attached(&writer, &owner);
    EXPECT_EQ_U32(1, attaches(&writer));                     // not re-attached at once
    segment_recheck_sweep(&writer, stamp + (16U * UNIT_NS)); // the tick at the "no"'s deadline, again: only a stamp
    send_to_attached(&writer, &owner);
    EXPECT_EQ_U32(1, attaches(&writer));
    segment_recheck_sweep(&writer, stamp + (48U * UNIT_NS)); // the "yes" deadline, from that stamp
    send_to_attached(&writer, &owner);
    EXPECT_EQ_U32(2, attaches(&writer));
    release_segments(&writer);
    release_own_segment(&owner);
    test_mock_segments_free();
}

int main(void) {
    test_a_no_stands_however_many_sends();
    test_a_no_falls_due_on_the_tick_at_its_deadline();
    test_the_summary_tick_ages_the_answers();
    test_a_same_host_announce_makes_a_no_due();
    test_a_yes_stands_however_many_sends();
    test_a_yes_falls_due_on_the_tick_at_its_deadline();
    test_an_attach_after_a_no_is_a_new_answer();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_segment_time_limits: all tests passed\n");
    return 0;
}

#else
int main(void) {
    printf("test_segment_time_limits: tt_SEGMENT_ENABLED is 0, nothing to test\n");
    return 0;
}
#endif
