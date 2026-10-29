/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions bundle 8 (ROADMAP "Context options"): the segment options, struct tt_SegmentOptions.
//   - the defaults are config.h's tt_SEGMENT_* constants: tt_InitOptions_default() resolves the six counts and times,
//     and leaves bytes, slot_bytes and dead_reader_ns at 0; a created context derives its dead-reader time from its
//     discovery timing, which at the defaults is tt_SEGMENT_DEAD_READER_NS, and builds tt_SEGMENT_SLOTS slots of
//     tt_SEGMENT_SLOT_BYTES - the ring it built before the options;
//   - every field, set to a value other than its default, changes what the context does (no field written and never
//     read): the ring it builds, how long a fence is held, when a writer gives a reader up, how much one poll takes,
//     how far the reader's re-measures spread, when a stalled head is reported, when a missing or attached segment is
//     looked for again. Each runs beside the same arm at the defaults, which must do what the build's constants say;
//   - each check refuses a bad value with tt_RET_INVALID_ARGUMENT and accepts the boundary beside it
//     (tests/mutants_init_options_segment.sh removes each check, and each reader, and expects a failure here).
// The behaviour runs on whitebox contexts (built by hand, as tests/test_transport_seam.c builds them) holding the
// options a created context resolved.

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_segment.sh passes a mutant

#define AUTO_ID 7
#define MS 1000000ULL
#define OWNER_ID 7
#define OWNER_IP 0x0a4d0002U
#define OWNER_PORT 8282
#define OWNER_INCARNATION 0x12345678U
#define WRITER_ID 2
#define WRITER_IP 0x0a000002U
#define WRITER_PORT 7000

#if tt_SEGMENT_ENABLED

static struct tt_Context ctx;

static void reset_mock(void) {
    test_mock_reset();
    test_mock_segments_free();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = AUTO_ID;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
#if tt_CONTEXT_ID_CLAIM
    memset(test_mock_ids_held, 0, sizeof(test_mock_ids_held));
#endif
}

static tt_ret_t create_with(const struct tt_InitOptions* opts) {
    memset(&ctx, 0, sizeof(ctx));
    return tt_Context_create_with(&ctx, opts);
}

static void destroy(void) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
}

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts;
}

static void expect_refused(const struct tt_InitOptions* opts, const char* what) {
    reset_mock();
    tt_ret_t ret = create_with(opts);
    if (ret != tt_RET_INVALID_ARGUMENT) {
        fprintf(stderr, "  expected %s to be refused, got %d\n", what, ret);
    }
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, ret);
}

static void expect_accepted(const struct tt_InitOptions* opts, const char* what) {
    reset_mock();
    tt_ret_t ret = create_with(opts);
    if (ret != tt_RET_OK) {
        fprintf(stderr, "  expected %s to be accepted, got %d\n", what, ret);
    }
    EXPECT_EQ_INT(tt_RET_OK, ret);
    if (ret == tt_RET_OK) {
        destroy();
    }
}

// The options a context created with `opts` holds: validated and resolved as tt_Context_create_with() does both.
static struct tt_InitOptions resolved(const struct tt_InitOptions* opts) {
    reset_mock();
    struct tt_InitOptions out;
    memset(&out, 0, sizeof(out));
    tt_ret_t ret = create_with(opts);
    EXPECT_EQ_INT(tt_RET_OK, ret);
    if (ret == tt_RET_OK) {
        out = ctx.opts;
        destroy();
    }
    return out;
}

// A whitebox context holding `opts` (resolved): what the drain and the send paths are driven on.
static void whitebox(struct tt_Context* node, const struct tt_InitOptions* opts, uint8_t id, uint32_t ip,
                     uint16_t port) {
    memset(node, 0, sizeof(*node));
    node->opts = *opts;
    node_init_locks(node);
    node->id = id;
    node->entity_id_base = OWNER_INCARNATION;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node->hal.own_ip = ip;
    node->hal.own_port = port;
    segment_vouch_note(node, OWNER_ID, OWNER_INCARNATION,
                       OWNER_PORT); // discovery has heard the owner (segment_vouched())
}

static void drain_pass(struct tt_Context* node) {
    bool emptied = false;
    (void)drain_own_segment(node, &emptied);
}

// ---- The defaults.

static void test_defaults_are_the_compiled_settings(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32(0, opts.segment.bytes);
    EXPECT_EQ_U32(0, opts.segment.slot_bytes);
    EXPECT_EQ_U64(0, opts.segment.dead_reader_ns); // derived when a context is created
    EXPECT_EQ_U64(tt_SEGMENT_FENCE_HOLD_NS, opts.segment.fence_hold_ns);
    EXPECT_EQ_U32(tt_SEGMENT_DRAIN_PER_POLL, opts.segment.drain_per_poll);
    EXPECT_EQ_U32(tt_SEGMENT_PROBE_EVERY_MAX, opts.segment.probe_every_max);
    EXPECT_EQ_U32(tt_SEGMENT_STALL_PASSES, opts.segment.stall_passes);
    EXPECT_EQ_U64(tt_SEGMENT_ATTACH_RETRY_NS, opts.segment.attach_retry_ns);
    EXPECT_EQ_U64(tt_SEGMENT_REVALIDATE_NS, opts.segment.revalidate_ns);

    // A created context: the dead-reader time the build's constant, and the ring the build always built.
    struct tt_InitOptions created = resolved(&opts);
    EXPECT_EQ_U64(tt_SEGMENT_DEAD_READER_NS, created.segment.dead_reader_ns);
    struct tt_Context owner;
    whitebox(&owner, &created, OWNER_ID, OWNER_IP, OWNER_PORT);
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    if (owner.own_segment != NULL) {
        EXPECT_EQ_U32(tt_SEGMENT_SLOTS, owner.own_segment->slots);
        EXPECT_EQ_U32(tt_SEGMENT_SLOT_BYTES, owner.own_segment->slot_bytes);
    }
    release_own_segment(&owner);
    test_mock_segments_free();

    // A whitebox context built from nothing reads the same: node_init_locks() resolves the group.
    struct tt_Context bare;
    memset(&bare, 0, sizeof(bare));
    node_init_locks(&bare);
    EXPECT_EQ_U64(tt_SEGMENT_DEAD_READER_NS, bare.opts.segment.dead_reader_ns);
    EXPECT_EQ_U32(tt_SEGMENT_DRAIN_PER_POLL, bare.opts.segment.drain_per_poll);
}

// ---- Every field changes behaviour.

// The slot count of the ring a context built from `opts` creates.
static uint32_t ring_slots(const struct tt_InitOptions* opts, uint32_t* slot_bytes) {
    struct tt_InitOptions created = resolved(opts);
    struct tt_Context owner;
    whitebox(&owner, &created, OWNER_ID, OWNER_IP, OWNER_PORT);
    create_own_segment(&owner);
    uint32_t slots = 0;
    EXPECT_TRUE(owner.own_segment != NULL);
    if (owner.own_segment != NULL) {
        slots = owner.own_segment->slots;
        *slot_bytes = owner.own_segment->slot_bytes;
    }
    release_own_segment(&owner);
    test_mock_segments_free();
    return slots;
}

static void test_bytes_and_slot_bytes_are_the_ring_built(void) {
    const uint32_t stride = (uint32_t)(sizeof(struct tt_SegmentSlot) + tt_SEGMENT_SLOT_BYTES);
    uint32_t slot_bytes = 0;
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32(tt_SEGMENT_SLOTS, ring_slots(&opts, &slot_bytes)); // control
    // 64 slots' bytes, and a byte short of 64: 32 (rounded down to a power of two).
    opts.segment.bytes = 64U * stride;
    EXPECT_EQ_U32(64, ring_slots(&opts, &slot_bytes));
    opts.segment.bytes = (64U * stride) - 1U;
    EXPECT_EQ_U32(32, ring_slots(&opts, &slot_bytes));
    // The slot size: the ring's slots carry it; a zero bytes keeps tt_SEGMENT_SLOTS slots whatever it is, as the
    // net.segment_slot_bytes it replaces did, and a set bytes holds fewer of the larger slots.
    opts = defaults();
    opts.segment.slot_bytes = 512;
    EXPECT_EQ_U32(tt_SEGMENT_SLOTS, ring_slots(&opts, &slot_bytes));
    EXPECT_EQ_U32(512, slot_bytes);
    opts.segment.bytes = 64U * stride;
    EXPECT_EQ_U32(128, ring_slots(&opts, &slot_bytes)); // 64 x 1,488 / 528 = 180 -> 128
    EXPECT_EQ_U32(512, slot_bytes);
}

// The fence at the head of `owner`'s ring: one written, then held and polled at `start` + `after`.
static bool fence_passed_after(struct tt_Context* owner, uint64_t after) {
    const uint8_t fence[tt_SEGMENT_FENCE_LENGTH] = {(uint8_t)tt_SEGMENT_FENCE_MARKER, WRITER_ID, 1};
    struct tt_SegmentHeader* ring = owner->own_segment;
    const uint32_t index = __atomic_load_n(&ring->read_index, __ATOMIC_RELAXED);
    EXPECT_TRUE(segment_write(ring, fence, sizeof(fence), NULL, 0, WRITER_IP, WRITER_PORT, 1));
    const uint64_t start = test_mock_now;
    EXPECT_TRUE(!segment_fence_at_head(owner, ring, index)); // held: the deadline is set
    test_mock_now = start + after;
    bool passed = segment_fence_at_head(owner, ring, index);
    if (!passed) {
        test_mock_now = start + (10U * owner->opts.segment.fence_hold_ns); // let it go, so the next arm starts clean
        EXPECT_TRUE(segment_fence_at_head(owner, ring, index));
    }
    test_mock_now = start;
    return passed;
}

static void test_fence_hold_ns_is_how_long_a_fence_is_held(void) {
    struct tt_InitOptions opts = defaults();
    const uint64_t hold = 5 * MS;
    for (int arm = 0; arm < 2; arm++) {
        const uint64_t expected = arm == 0 ? (uint64_t)tt_SEGMENT_FENCE_HOLD_NS : hold;
        if (arm == 1) {
            opts.segment.fence_hold_ns = hold;
        }
        struct tt_InitOptions created = resolved(&opts);
        struct tt_Context owner;
        whitebox(&owner, &created, OWNER_ID, OWNER_IP, OWNER_PORT);
        create_own_segment(&owner);
        EXPECT_TRUE(owner.own_segment != NULL);
        if (owner.own_segment == NULL) {
            continue;
        }
        EXPECT_TRUE(!fence_passed_after(&owner, expected - 1U));
        EXPECT_TRUE(fence_passed_after(&owner, expected));
        EXPECT_EQ_U64(2, owner.segment_fences_expired);
        release_own_segment(&owner);
        test_mock_segments_free();
    }
}

// Sends to a full ring nobody drains, the clock moving `step` a send, until the writer gives the reader up; the time
// that took since the ring first refused.
static uint64_t time_to_give_up(const struct tt_InitOptions* opts, uint64_t step) {
    struct tt_InitOptions created = resolved(opts);
    struct tt_Context owner;
    struct tt_Context writer;
    whitebox(&owner, &created, OWNER_ID, OWNER_IP, OWNER_PORT);
    whitebox(&writer, &created, WRITER_ID, WRITER_IP, WRITER_PORT);
    create_own_segment(&owner);
    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = WRITER_ID;
    uint64_t first_refused = 0;
    uint64_t gave_up = 0;
    for (uint32_t i = 0; i < 100000U && gave_up == 0; i++) {
        test_mock_now += step;
        enum udp_reason reason = UDP_BECAUSE_UNATTACHED;
        (void)segment_deliver(&writer, OWNER_ID, OWNER_IP, OWNER_PORT, &header, sizeof(header), NULL, 0, &reason);
        if (first_refused == 0 && writer.segment_full_dropped > 0) {
            first_refused = test_mock_now;
        }
        if (writer.segment_attach[tt_SEGMENT_REFUSED] > 0) {
            gave_up = test_mock_now;
        }
    }
    EXPECT_TRUE(first_refused != 0 && gave_up != 0);
    release_segments(&writer);
    release_own_segment(&owner);
    test_mock_segments_free();
    return gave_up - first_refused;
}

static void test_dead_reader_ns_is_when_a_writer_gives_up(void) {
    struct tt_InitOptions opts = defaults();
    const uint64_t step = MS;
    EXPECT_EQ_U64(tt_SEGMENT_DEAD_READER_NS, time_to_give_up(&opts, step)); // control
    opts.segment.dead_reader_ns = 50 * MS;
    EXPECT_EQ_U64(50 * MS, time_to_give_up(&opts, step));

    // Derived: a context whose discovery interval is not the default gives a reader up after one of its intervals -
    // (2 x 3 + 1) x 100 ms / 2 = 350 ms of silence, x 2 / 7 = 100 ms.
    opts = defaults();
    opts.discovery.update_interval_ns = 100 * MS;
    struct tt_InitOptions created = resolved(&opts);
    EXPECT_EQ_U64(100 * MS, created.segment.dead_reader_ns);
    EXPECT_EQ_U64(100 * MS, time_to_give_up(&opts, step));
    opts.discovery.miss_threshold = 1; // (2 + 1) x 100 ms / 2 = 150 ms, x 2 / 3 = 100 ms: still one interval
    created = resolved(&opts);
    EXPECT_EQ_U64(100 * MS, created.segment.dead_reader_ns);
    // At the default discovery timing, the build's constant (which a -D may have set apart from the derivation).
    opts = defaults();
    created = resolved(&opts);
    EXPECT_EQ_U64(tt_SEGMENT_DEAD_READER_NS, created.segment.dead_reader_ns);
}

// Records in `owner`'s ring that one drain pass takes, of `written`.
static uint32_t taken_in_one_pass(const struct tt_InitOptions* opts, uint32_t written) {
    struct tt_InitOptions created = resolved(opts);
    struct tt_Context owner;
    whitebox(&owner, &created, OWNER_ID, OWNER_IP, OWNER_PORT);
    create_own_segment(&owner);
    struct tt_SegmentHeader* ring = owner.own_segment;
    if (ring == NULL) {
        EXPECT_TRUE(ring != NULL);
        return 0;
    }
    for (uint32_t i = 0; i < written; i++) {
        EXPECT_TRUE(segment_write(ring, "hello", 6, NULL, 0, WRITER_IP, WRITER_PORT, 1));
    }
    drain_pass(&owner);
    uint32_t taken = __atomic_load_n(&ring->read_index, __ATOMIC_RELAXED);
    release_own_segment(&owner);
    test_mock_segments_free();
    return taken;
}

static void test_drain_per_poll_is_how_much_one_poll_takes(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32(40, taken_in_one_pass(&opts, 40)); // control: the default takes them all
    opts.segment.drain_per_poll = 3;
    EXPECT_EQ_U32(3, taken_in_one_pass(&opts, 40));
}

#if tt_SEGMENT_READER_WAITS
// segment_next_mode() asked again and again with the choice unchanged: the interval between re-measures doubles to the
// bound. Two equal modes of two epochs each, so the comparison never decides.
static uint32_t probe_interval_reached(const struct tt_InitOptions* opts) {
    struct tt_InitOptions created = resolved(opts);
    struct tt_Context node;
    whitebox(&node, &created, OWNER_ID, OWNER_IP, OWNER_PORT);
    node.segment_preferred = 0;
    node.segment_probe_every = 1;
    for (uint8_t mode = 0; mode < 2; mode++) {
        node.segment_cost_n[mode] = 2;
        node.segment_cost_mean_ns[mode] = 100;
        node.segment_cost_m2[mode] = 0;
    }
    for (int i = 0; i < 40; i++) {
        EXPECT_EQ_U32(0, segment_next_mode(&node, 1)); // a re-measure of mode 1 that did not change the choice
    }
    return node.segment_probe_every;
}

static void test_probe_every_max_bounds_the_reader_re_measures(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32(tt_SEGMENT_PROBE_EVERY_MAX, probe_interval_reached(&opts)); // control
    opts.segment.probe_every_max = 8;
    EXPECT_EQ_U32(8, probe_interval_reached(&opts));
}
#endif

// Drain passes over a head claimed and never published until the stall is reported.
static uint32_t passes_to_report_a_stall(const struct tt_InitOptions* opts) {
    struct tt_InitOptions created = resolved(opts);
    struct tt_Context owner;
    whitebox(&owner, &created, OWNER_ID, OWNER_IP, OWNER_PORT);
    create_own_segment(&owner);
    struct tt_SegmentHeader* ring = owner.own_segment;
    if (ring == NULL) {
        EXPECT_TRUE(ring != NULL);
        return 0;
    }
    uint32_t claimed = __atomic_load_n(&ring->write_index, __ATOMIC_RELAXED);
    __atomic_store_n(&ring->write_index, claimed + 1U, __ATOMIC_RELEASE); // claimed, never published
    uint32_t passes = 0;
    while (owner.segment_stall_warnings == 0 && passes < 100000U) {
        drain_pass(&owner);
        passes++;
    }
    release_own_segment(&owner);
    test_mock_segments_free();
    return passes;
}

static void test_stall_passes_is_when_a_stall_is_reported(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32(tt_SEGMENT_STALL_PASSES, passes_to_report_a_stall(&opts)); // control
    opts.segment.stall_passes = 10;
    EXPECT_EQ_U32(10, passes_to_report_a_stall(&opts));
}

// How long after the summary tick that first sees a "no" the writer asks about the peer again: summary ticks every
// 2^20 ns (the deadline's own unit), a send after each, until the second attach attempt.
#define AGE_STEP_NS (1ULL << 20U)
static uint64_t ns_between_attach_attempts(const struct tt_InitOptions* opts) {
    struct tt_InitOptions created = resolved(opts);
    struct tt_Context writer;
    whitebox(&writer, &created, WRITER_ID, WRITER_IP, WRITER_PORT);
    test_mock_segments_free();
    EXPECT_TRUE(peer_segment(&writer, OWNER_ID, OWNER_IP, OWNER_PORT) == NULL); // asked, and answered "no"
    const int asked = test_mock_segment_attach_calls;
    const uint64_t stamp = (5U * tt_SECOND) + 12345U; // any phase within a unit
    segment_recheck_sweep(&writer, stamp);
    uint64_t now = stamp;
    while (test_mock_segment_attach_calls == asked && now - stamp < 100U * tt_SECOND) {
        now += AGE_STEP_NS;
        segment_recheck_sweep(&writer, now);
        EXPECT_TRUE(peer_segment(&writer, OWNER_ID, OWNER_IP, OWNER_PORT) == NULL);
    }
    return now - stamp;
}

// The deadline is the option's, to within the unit it is kept in (rounded down) and the step of the ticks above.
static void expect_about(uint64_t want, uint64_t got) {
    EXPECT_TRUE(got <= want + AGE_STEP_NS && got + 2U * AGE_STEP_NS > want);
    if (got > want + AGE_STEP_NS || got + 2U * AGE_STEP_NS <= want) {
        printf("  want about %llu ns, got %llu ns\n", (unsigned long long)want, (unsigned long long)got);
    }
}

static void test_attach_retry_ns_is_when_a_missing_segment_is_asked_again(void) {
    struct tt_InitOptions opts = defaults();
    expect_about(tt_SEGMENT_ATTACH_RETRY_NS, ns_between_attach_attempts(&opts)); // control
    opts.segment.attach_retry_ns = 50U * tt_MILLISECOND;
    expect_about(50U * tt_MILLISECOND, ns_between_attach_attempts(&opts));
}

// How long after the summary tick that first sees an attached segment the writer asks its name again - by
// tt_segment_identify(), which keeps the mapping when the name still holds it (2026-10-23), or by an attach (two calls:
// header, then region): summary ticks every 2^20 ns, a send after each.
static uint64_t ns_before_revalidation(const struct tt_InitOptions* opts) {
    struct tt_InitOptions created = resolved(opts);
    struct tt_Context owner;
    struct tt_Context writer;
    whitebox(&owner, &created, OWNER_ID, OWNER_IP, OWNER_PORT);
    whitebox(&writer, &created, WRITER_ID, WRITER_IP, WRITER_PORT);
    create_own_segment(&owner);
    EXPECT_TRUE(peer_segment(&writer, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    const int asked = test_mock_segment_attach_calls + test_mock_segment_identifies;
    const uint64_t stamp = (5U * tt_SECOND) + 12345U;
    segment_recheck_sweep(&writer, stamp);
    uint64_t now = stamp;
    while (test_mock_segment_attach_calls + test_mock_segment_identifies == asked && now - stamp < 100U * tt_SECOND) {
        now += AGE_STEP_NS;
        segment_recheck_sweep(&writer, now);
        EXPECT_TRUE(peer_segment(&writer, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
    }
    release_segments(&writer);
    release_own_segment(&owner);
    test_mock_segments_free();
    return now - stamp;
}

static void test_revalidate_ns_is_when_an_attached_segment_is_asked_again(void) {
    struct tt_InitOptions opts = defaults();
    expect_about(tt_SEGMENT_REVALIDATE_NS, ns_before_revalidation(&opts)); // control
    opts.segment.revalidate_ns = 30U * tt_MILLISECOND;
    expect_about(30U * tt_MILLISECOND, ns_before_revalidation(&opts));
}

// ---- The checks.

static void test_validation(void) {
    const uint32_t stride = (uint32_t)(sizeof(struct tt_SegmentSlot) + tt_SEGMENT_SLOT_BYTES);
    struct tt_InitOptions opts = defaults();
    expect_accepted(&opts, "the defaults");

    // slot_bytes: a datagram header at least, a whole datagram at most.
    opts = defaults();
    opts.segment.slot_bytes = (uint32_t)sizeof(struct tt_Header) - 1U;
    expect_refused(&opts, "segment.slot_bytes below a header");
    opts.segment.slot_bytes = (uint32_t)sizeof(struct tt_Header);
    expect_accepted(&opts, "segment.slot_bytes = a header");

    // bytes: 4 slots at least, tt_SEGMENT_SLOTS at most.
    opts = defaults();
    opts.segment.bytes = (4U * stride) - 1U;
    expect_refused(&opts, "segment.bytes short of 4 slots");
    opts.segment.bytes = 4U * stride;
    expect_accepted(&opts, "segment.bytes of 4 slots");
    opts.segment.bytes = ((uint32_t)tt_SEGMENT_SLOTS * 2U * stride) - 1U;
    expect_accepted(&opts, "segment.bytes a byte short of twice tt_SEGMENT_SLOTS: tt_SEGMENT_SLOTS");
    opts.segment.bytes = (uint32_t)tt_SEGMENT_SLOTS * 2U * stride;
    expect_refused(&opts, "segment.bytes of twice tt_SEGMENT_SLOTS");
    // Judged at the slot size: the same bytes in slots of a quarter the size hold four times as many.
    opts.segment.bytes = (uint32_t)tt_SEGMENT_SLOTS * stride;
    expect_accepted(&opts, "segment.bytes of tt_SEGMENT_SLOTS slots");
    opts.segment.slot_bytes = ((uint32_t)tt_SEGMENT_SLOT_BYTES - 48U) / 4U; // stride / 4
    expect_refused(&opts, "segment.bytes of tt_SEGMENT_SLOTS slots, in quarter slots");

    // dead_reader_ns below the discovery silence (3.5 update intervals at the default miss threshold).
    opts = defaults();
    const uint64_t silence = (7U * opts.discovery.update_interval_ns) / 2U;
    opts.segment.dead_reader_ns = silence;
    expect_refused(&opts, "segment.dead_reader_ns = the silence");
    opts.segment.dead_reader_ns = silence - 1U;
    expect_accepted(&opts, "segment.dead_reader_ns just below the silence");
    // A short interval with the build's dead-reader time set by hand is refused; derived, it is accepted.
    opts = defaults();
    opts.discovery.update_interval_ns = 100 * MS;
    expect_accepted(&opts, "a short interval, the dead-reader time derived");
    opts.segment.dead_reader_ns = tt_SEGMENT_DEAD_READER_NS;
    expect_refused(&opts, "a short interval, the build's dead-reader time kept");

    // fence_hold_ns below the dead-reader time.
    opts = defaults();
    opts.segment.dead_reader_ns = 10 * MS;
    opts.segment.fence_hold_ns = 10 * MS;
    expect_refused(&opts, "segment.fence_hold_ns = segment.dead_reader_ns");
    opts.segment.fence_hold_ns = (10 * MS) - 1U;
    expect_accepted(&opts, "segment.fence_hold_ns just below segment.dead_reader_ns");
    // Against a derived dead-reader time: one interval. tx_interval_ns at most the interval, so only this check is met.
    opts = defaults();
    opts.discovery.tx_interval_ns = tt_SEGMENT_FENCE_HOLD_NS;
    opts.discovery.update_interval_ns = 2U * tt_SEGMENT_FENCE_HOLD_NS;
    expect_accepted(&opts, "the default fence hold against a derived dead-reader time of twice it");
    opts.discovery.update_interval_ns = tt_SEGMENT_FENCE_HOLD_NS;
    expect_refused(&opts, "the default fence hold against a derived dead-reader time of the same length");

    // probe_every_max: a power of two.
    opts = defaults();
    opts.segment.probe_every_max = 1;
    expect_accepted(&opts, "segment.probe_every_max = 1");
    opts.segment.probe_every_max = 1U << 31;
    expect_accepted(&opts, "segment.probe_every_max = 2^31");
    opts.segment.probe_every_max = 3;
    expect_refused(&opts, "segment.probe_every_max = 3");
    opts.segment.probe_every_max = (uint32_t)tt_SEGMENT_PROBE_EVERY_MAX + 1U;
    expect_refused(&opts, "segment.probe_every_max one above the default");
}

// The legacy path: tt_Context_create() reads _tt_CONFIG.segment_slot_bytes into the segment group.
static void test_legacy_slot_bytes_reach_the_segment_group(void) {
    reset_mock();
    _tt_CONFIG.segment_slot_bytes = 1024;
    memset(&ctx, 0, sizeof(ctx));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&ctx));
    EXPECT_EQ_U32(1024, ctx.opts.segment.slot_bytes);
    destroy();
    _tt_CONFIG.segment_slot_bytes = 0;
}

#endif // tt_SEGMENT_ENABLED

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
#if tt_SEGMENT_ENABLED
    test_defaults_are_the_compiled_settings();
    test_bytes_and_slot_bytes_are_the_ring_built();
    test_fence_hold_ns_is_how_long_a_fence_is_held();
    test_dead_reader_ns_is_when_a_writer_gives_up();
    test_drain_per_poll_is_how_much_one_poll_takes();
#if tt_SEGMENT_READER_WAITS
    test_probe_every_max_bounds_the_reader_re_measures();
#endif
    test_stall_passes_is_when_a_stall_is_reported();
    test_attach_retry_ns_is_when_a_missing_segment_is_asked_again();
    test_revalidate_ns_is_when_an_attached_segment_is_asked_again();
    test_validation();
    test_legacy_slot_bytes_reach_the_segment_group();
#else
    printf("test_init_options_segment: tt_SEGMENT_ENABLED is 0 - nothing to test\n");
#endif
    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_segment: all tests passed\n");
    return 0;
}
