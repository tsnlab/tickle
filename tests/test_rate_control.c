/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Rate control (tt_LinkOptions.rate_control, rate_min_rate, rate_queue_ns; docs/DESIGN.md "Send pacing"), through the
// mock HAL and tt_Context_create_with(), built as rmw_tickle builds core (large samples):
//   - the zero value keeps what pacing_rate always meant: a program that sets only pacing_rate is FIXED at it, T1S's
//     profile still paces at 75 % of its link_rate, the other profiles still do not pace, tt_PACING_OFF still turns
//     pacing off - and rate_control holds the concrete mode after resolution;
//   - FIXED without a pacing_rate, a value past tt_RATE_CONTROL_ADAPTIVE, the adaptive tunables on a link that is not
//     ADAPTIVE, ADAPTIVE with pacing off, a floor above the cap or below tt_LINK_RATE_MIN and a queue target below
//     tt_RATE_QUEUE_MIN_NS are refused with tt_RET_INVALID_ARGUMENT, and each bound itself is accepted;
//   - ADAPTIVE's defaults: the cap is link_rate, the floor tt_LINK_RATE_MIN, the burst four frames.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1 // as rmw_tickle builds core

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox

#define LINK_BROADCAST "10.0.0.255"
#define T1S_RATE 10000000ULL
#define T1S_PACED (T1S_RATE / 100U * tt_LINK_T1S_PACING_PCT)

static struct tt_Context node;
static struct tt_TypeSupport topic;
static struct tt_Publisher pub;
static uint64_t pub_storage[(size_t)256U * 1024U / sizeof(uint64_t)];
static uint32_t sample_len;
static uint32_t buffers_out;

static bool within_pct(uint64_t value, uint64_t expected, uint64_t pct) {
    uint64_t slack = expected * pct / 100U;
    return value + slack >= expected && value <= expected + slack;
}

static struct tt_InitOptions one_link(uint8_t profile) {
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = LINK_BROADCAST, .profile = profile};
    opts.net.link_count = 1;
    return opts;
}

static void mock_setup(void) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    test_mock_link_mtu = -1;
}

static void create(const struct tt_InitOptions* opts) {
    mock_setup();
    memset(&node, 0, sizeof(node));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&node, opts));
}

static void destroy(void) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
}

static void expect_created(const struct tt_InitOptions* opts, tt_ret_t expected, const char* what) {
    mock_setup();
    struct tt_Context created;
    memset(&created, 0, sizeof(created));
    tt_ret_t ret = tt_Context_create_with(&created, opts);
    if (ret != expected) {
        printf("test_rate_control: %s: expected %d, got %d\n", what, (int)expected, (int)ret);
    }
    EXPECT_EQ_INT(expected, ret);
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));
    }
}

// ---- The send path on an adaptive link: a large sample's units leave at the estimator's start rate, are counted into
// it, and ask for the feedback it needs.

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0x5A, len);
    return (int32_t)len;
}

static void* test_acquire(void* user, uint32_t bytes) {
    (void)user;
    buffers_out++;
    return malloc(bytes);
}

static void test_release(void* user, void* buffer) {
    (void)user;
    buffers_out--;
    free(buffer);
}

static bool large_unsent(void) {
    for (uint32_t i = 0; i < pub.large_count; i++) {
        if (!large_record_at(&pub, i)->sent) {
            return true;
        }
    }
    return false;
}

// Runs the scheduler on the mock clock until the large sample has gone (or two minutes of mock time).
static void run_until_sent(void) {
    uint64_t give_up = test_mock_now + (120ULL * tt_SECOND);
    uint32_t entries = 0;
    while (large_unsent() && test_mock_now < give_up && entries < 1000000U) {
        uint64_t next = 0;
        bool has_next = false;
        if (!sched_next_time(&node, &next)) {
            break;
        }
        test_mock_now = next > test_mock_now ? next : test_mock_now;
        while (entries < 1000000U && run_due_entry(&node, test_mock_now, &has_next, &next)) {
            entries++;
        }
    }
}

static uint32_t sent_count;
static uint64_t sent_first_ns;
static uint64_t sent_last_ns;
static uint64_t sent_wire_bytes;

static void capture(const void* buf, size_t len) {
    (void)buf;
    if (sent_count == 0) {
        sent_first_ns = test_mock_now;
    }
    sent_count++;
    sent_last_ns = test_mock_now;
    sent_wire_bytes += (uint64_t)len + 8U + 20U + 38U;
}

static void test_a_large_send_on_an_adaptive_link(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    create(&opts);
    memset(&topic, 0, sizeof(topic));
    topic.name = "rate_topic";
    topic.data_size = 8;
    topic.data_encode_size = sized_encode_size;
    topic.data_encode = sized_encode;
    struct tt_QosProfile reliable = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 2},
                                     .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    size_t storage = tt_Publisher_storage_size(&topic, &reliable);
    EXPECT_TRUE(storage <= sizeof(pub_storage));
    struct tt_PublisherOptions unbatched = {.batching = tt_SWITCH_OFF};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node), &pub, &topic, "rate", &reliable,
                                                      &unbatched, storage != 0 ? pub_storage : NULL, storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&node, test_acquire, test_release, NULL));
    (void)upsert_peer(pub.peers, 9, 0x0a000009U, 8282);

    sample_len = 256U * 1024U;
    sent_count = 0;
    sent_wire_bytes = 0;
    test_mock_send_hook = capture;
    uint8_t data[8] = {0};
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    run_until_sent();
    EXPECT_TRUE(!large_unsent());
    // No feedback came back, so the rate never left its start: the units went at it, not in a burst.
    EXPECT_EQ_U64(AP_START_RATE_BPS, node.rate[0].ap.rate_bps);
    EXPECT_TRUE(sent_last_ns > sent_first_ns);
    uint64_t measured = sent_wire_bytes * 8U * tt_SECOND / (sent_last_ns - sent_first_ns);
    EXPECT_TRUE(within_pct(measured, AP_START_RATE_BPS, 25));
    // Every datagram left through the seam was counted into the estimator, and the writer asked for its feedback.
    EXPECT_TRUE(node.rate[0].ap.last_active_ns != 0);
    EXPECT_TRUE(node.rate[0].ap.round_sent > 0 || node.rate[0].ap.samples > 0);
    EXPECT_TRUE(pub.ack_solicit_outstanding);
    EXPECT_TRUE(pub.last_ack_solicit_ns != 0);
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
    EXPECT_EQ_U32(0, buffers_out);
}

// ---- The zero value is what pacing_rate says.

static void test_zero_keeps_what_pacing_rate_meant(void) {
    static const uint8_t unpaced[] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_TSN, tt_LINK_PROFILE_WIFI};
    for (size_t i = 0; i < sizeof(unpaced); i++) {
        struct tt_InitOptions opts = one_link(unpaced[i]);
        create(&opts);
        EXPECT_EQ_U32(tt_RATE_CONTROL_OFF, node.opts.net.links[0].rate_control);
        EXPECT_EQ_U64(tt_PACING_OFF, node.opts.net.links[0].pacing_rate);
        EXPECT_TRUE(!node.pacing);
        destroy();
    }
    struct tt_InitOptions t1s = one_link(tt_LINK_PROFILE_T1S);
    create(&t1s);
    EXPECT_EQ_U32(tt_RATE_CONTROL_FIXED, node.opts.net.links[0].rate_control);
    EXPECT_EQ_U64(T1S_PACED, node.opts.net.links[0].pacing_rate);
    EXPECT_TRUE(node.pacing);
    destroy();

    // Only pacing_rate set, on a profile that does not pace by default: FIXED at it, as before.
    struct tt_InitOptions eth = one_link(tt_LINK_PROFILE_WIFI);
    eth.net.links[0].pacing_rate = 5000000ULL;
    create(&eth);
    EXPECT_EQ_U32(tt_RATE_CONTROL_FIXED, node.opts.net.links[0].rate_control);
    EXPECT_EQ_U64(5000000ULL, node.opts.net.links[0].pacing_rate);
    EXPECT_TRUE(node.pacing);
    destroy();

    // tt_PACING_OFF still turns T1S's pacing off.
    t1s.net.links[0].pacing_rate = tt_PACING_OFF;
    create(&t1s);
    EXPECT_EQ_U32(tt_RATE_CONTROL_OFF, node.opts.net.links[0].rate_control);
    EXPECT_TRUE(!node.pacing);
    destroy();

    // An explicit FIXED with a rate, or with T1S's own, is the same as leaving it zero.
    struct tt_InitOptions fixed = one_link(tt_LINK_PROFILE_T1S);
    fixed.net.links[0].rate_control = tt_RATE_CONTROL_FIXED;
    create(&fixed);
    EXPECT_EQ_U32(tt_RATE_CONTROL_FIXED, node.opts.net.links[0].rate_control);
    EXPECT_EQ_U64(T1S_PACED, node.opts.net.links[0].pacing_rate);
    destroy();
}

// ---- ADAPTIVE's defaults.

static void test_adaptive_defaults(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    create(&opts);
    const struct tt_LinkOptions* link = &node.opts.net.links[0];
    EXPECT_EQ_U32(tt_RATE_CONTROL_ADAPTIVE, link->rate_control);
    EXPECT_EQ_U64(link->link_rate, link->pacing_rate); // the cap
    EXPECT_EQ_U64(tt_RATE_MIN_DEFAULT, link->rate_min_rate);
    EXPECT_EQ_U32(tt_PACING_BURST_FRAMES * (1500U + 38U), link->pacing_burst);
    EXPECT_TRUE(node.pacing);
    destroy();

    // The cap, the floor and the queue target as given.
    opts.net.links[0].link_rate = 100000000ULL;
    opts.net.links[0].pacing_rate = 50000000ULL;
    opts.net.links[0].rate_min_rate = 2000000ULL;
    opts.net.links[0].rate_queue_ns = 4000000U;
    create(&opts);
    EXPECT_EQ_U64(50000000ULL, node.opts.net.links[0].pacing_rate);
    EXPECT_EQ_U64(2000000ULL, node.opts.net.links[0].rate_min_rate);
    EXPECT_EQ_U32(4000000U, node.opts.net.links[0].rate_queue_ns);
    destroy();
}

// ---- Each check refuses the value past its bound and accepts the bound itself.

static void test_validation(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "rate_control past ADAPTIVE");
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    expect_created(&opts, tt_RET_OK, "rate_control ADAPTIVE");

    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_FIXED;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "FIXED on a link with no pacing_rate");
    opts.net.links[0].pacing_rate = 1000000ULL;
    expect_created(&opts, tt_RET_OK, "FIXED with a pacing_rate");
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_FIXED;
    expect_created(&opts, tt_RET_OK, "FIXED on T1S, which paces by its profile");
    opts.net.links[0].pacing_rate = tt_PACING_OFF;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "FIXED with pacing_rate tt_PACING_OFF");

    // The adaptive tunables only on an ADAPTIVE link.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_min_rate = 1000000ULL;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "rate_min_rate on a link that is not ADAPTIVE");
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_queue_ns = 1000000U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "rate_queue_ns on a link that is not ADAPTIVE");
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].rate_min_rate = 1000000ULL;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "rate_min_rate on a FIXED link");

    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].pacing_rate = tt_PACING_OFF;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "ADAPTIVE with pacing off");

    // The cap: at least tt_LINK_RATE_MIN, at most link_rate.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].link_rate = 100000000ULL;
    opts.net.links[0].pacing_rate = 100000000ULL;
    expect_created(&opts, tt_RET_OK, "cap at link_rate");
    opts.net.links[0].pacing_rate = 100000001ULL;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "cap above link_rate");
    opts.net.links[0].pacing_rate = tt_LINK_RATE_MIN;
    expect_created(&opts, tt_RET_OK, "cap at tt_LINK_RATE_MIN");
    opts.net.links[0].pacing_rate = tt_LINK_RATE_MIN - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "cap below tt_LINK_RATE_MIN");

    // The floor: at least tt_LINK_RATE_MIN, at most the cap.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].link_rate = 100000000ULL;
    opts.net.links[0].pacing_rate = 20000000ULL;
    opts.net.links[0].rate_min_rate = 20000000ULL;
    expect_created(&opts, tt_RET_OK, "floor at the cap");
    opts.net.links[0].rate_min_rate = 20000001ULL;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "floor above the cap");
    opts.net.links[0].rate_min_rate = tt_LINK_RATE_MIN;
    expect_created(&opts, tt_RET_OK, "floor at tt_LINK_RATE_MIN");

    // The queue target: at least tt_RATE_QUEUE_MIN_NS.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].rate_queue_ns = tt_RATE_QUEUE_MIN_NS;
    expect_created(&opts, tt_RET_OK, "queue target at tt_RATE_QUEUE_MIN_NS");
    opts.net.links[0].rate_queue_ns = tt_RATE_QUEUE_MIN_NS - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "queue target below tt_RATE_QUEUE_MIN_NS");

    // The burst keeps its bound on an ADAPTIVE link.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    opts.net.links[0].pacing_burst = tt_PACING_BURST_MIN - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "pacing_burst below tt_PACING_BURST_MIN on ADAPTIVE");
}

// ---- The estimator on a link: the start, the pooled feedback, and a link whose rate steps.

// The bucket's rate in bits per second, from what it holds.
static uint64_t pacer_rate_bps(void) {
    return ((8ULL * tt_SECOND) << 16U) / node.pacers[0].ns_per_byte_q16;
}

static void test_adaptive_starts_between_floor_and_cap(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    create(&opts);
    EXPECT_TRUE(node.rate[0].adaptive);
    EXPECT_EQ_U64(AP_START_RATE_BPS, node.rate[0].ap.rate_bps);
    EXPECT_TRUE(within_pct(pacer_rate_bps(), AP_START_RATE_BPS, 1));
    EXPECT_EQ_U64(node.pacers[0].burst_bytes, node.opts.net.links[0].pacing_burst);
    destroy();

    // A floor above the start rate, a cap below it.
    opts.net.links[0].rate_min_rate = 4000000ULL;
    create(&opts);
    EXPECT_EQ_U64(4000000ULL, node.rate[0].ap.rate_bps);
    destroy();
    opts.net.links[0].rate_min_rate = 0;
    opts.net.links[0].pacing_rate = 500000ULL;
    create(&opts);
    EXPECT_EQ_U64(500000ULL, node.rate[0].ap.rate_bps);
    destroy();

    // FIXED and OFF links have no estimator and keep their bucket.
    struct tt_InitOptions fixed = one_link(tt_LINK_PROFILE_T1S);
    create(&fixed);
    EXPECT_TRUE(!node.rate[0].adaptive);
    uint64_t rate = pacer_rate_bps();
    struct tt_PeerAck ack = {0};
    rate_acknack(&node, 0, &ack, 100, true, test_mock_now - 1000);
    rate_acknack(&node, 0, &ack, 900, true, test_mock_now - 1000);
    EXPECT_EQ_U64(rate, pacer_rate_bps());
    EXPECT_EQ_U64(0, node.rate[0].delivered);
    EXPECT_EQ_U32(0, ack.rate_units_valid ? 1U : 0U);
    destroy();
}

static void test_control_datagrams_are_not_units(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    create(&opts);
    uint32_t unit = node.rate[0].unit_wire;
    EXPECT_EQ_U32(1500U + 38U, unit);
    // A flood of HEARTBEAT-sized datagrams (a solicitation per round trip) leaves what a unit counts as alone: it once
    // fell to the control datagrams' size and the estimator read every delivery as a fraction of what it was.
    for (uint32_t i = 0; i < 1000; i++) {
        pace_charge(&node, 0, 90U, test_mock_now);
    }
    EXPECT_EQ_U32(unit, node.rate[0].avg_wire);
    // Data frames of another size move it, within what is a data frame.
    for (uint32_t i = 0; i < 1000; i++) {
        pace_charge(&node, 0, 1000U, test_mock_now);
    }
    EXPECT_TRUE(node.rate[0].avg_wire < unit);
    EXPECT_TRUE(node.rate[0].avg_wire >= unit / 2U);
    destroy();
}

static void test_held_units_from_an_acknack(void) {
    uint64_t none[2] = {0, 0};
    EXPECT_EQ_U32(1000, rate_units_held(1000, none, 0)); // nothing missing: the watermark
    uint64_t gaps[2] = {0x5ULL, 0};                      // units 1000 and 1002 missing: held 1000 + (3 - 2)
    EXPECT_EQ_U32(1001, rate_units_held(1000, gaps, 1));
    uint64_t wide[2] = {0x1ULL, 0x2ULL}; // units 1000 and 1065 missing: window 66, 2 missing
    EXPECT_EQ_U32(1000 + 64, rate_units_held(1000, wide, 2));
    EXPECT_EQ_U32(0, rate_units_held(0xFFFFFFFFU, gaps, 1)); // the watermark wraps, the window follows
}

static void test_readers_pool_into_one_link_count(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    create(&opts);
    uint32_t wire = node.rate[0].avg_wire;
    struct tt_PeerAck a = {0};
    struct tt_PeerAck b = {0};
    // First ACKNACKs are baselines: nothing counted.
    rate_acknack(&node, 0, &a, 500, false, 0);
    rate_acknack(&node, 0, &b, 9000, false, 0);
    EXPECT_EQ_U64(0, node.rate[0].delivered);
    // Each reader's advance adds to the one count.
    rate_acknack(&node, 0, &a, 510, false, 0);
    EXPECT_EQ_U64(10ULL * wire, node.rate[0].delivered);
    rate_acknack(&node, 0, &b, 9020, false, 0);
    EXPECT_EQ_U64(30ULL * wire, node.rate[0].delivered);
    // A reordered, older ACKNACK moves nothing, and does not lower the baseline.
    rate_acknack(&node, 0, &a, 505, false, 0);
    EXPECT_EQ_U64(30ULL * wire, node.rate[0].delivered);
    rate_acknack(&node, 0, &a, 512, false, 0);
    EXPECT_EQ_U64(32ULL * wire, node.rate[0].delivered);
    EXPECT_EQ_U32(6U, node.rate[0].acks_seen);
    destroy();
}

// A fluid link behind the real bucket: every step the writer sends what the bucket's rate lets it, the link carries at
// most `link_bps` of it behind a queue of `queue_bytes`, and every 20 ms one solicited ACKNACK reports what arrived.
// The round trip is 20 ms plus the queue. Returns the bucket's rate at the end of each second in rates[].
static void fluid_link(uint64_t (*link_bps)(uint64_t second), uint32_t queue_bytes, uint32_t seconds, uint64_t* rates,
                       uint64_t* queue_peak) {
    struct tt_PeerAck ack = {0};
    uint64_t queue = 0;
    uint64_t delivered_bytes = 0;
    uint64_t sent_credit = 0;
    uint64_t snapshot_at = 0; // the solicitation reaches the reader: what it holds then is what it reports
    uint64_t reply_at = 0;
    uint64_t solicit_ns = 0;
    uint64_t held_at_solicit = 0;
    bool outstanding = false;
    bool snapped = false;
    const uint64_t base_rtt = 20ULL * 1000000ULL;
    const uint64_t step = 1000000ULL; // 1 ms
    uint64_t start = test_mock_now;
    *queue_peak = 0;
    rate_acknack(&node, 0, &ack, 0, false, 0); // the baseline
    for (uint64_t t = 0; t < (uint64_t)seconds * tt_SECOND; t += step) {
        test_mock_now = start + t;
        uint64_t bps = link_bps(t / tt_SECOND);
        sent_credit += pacer_rate_bps() / 8U * step / tt_SECOND;
        while (sent_credit >= 1538U) { // a unit leaves: the seam charges it into the bucket
            sent_credit -= 1538U;
            pace_charge(&node, 0, 1472U, test_mock_now);
            queue += 1538U;
        }
        uint64_t carried = bps / 8U * step / tt_SECOND;
        carried = carried < queue ? carried : queue;
        queue -= carried;
        delivered_bytes += carried;
        if (queue > queue_bytes) {
            queue = queue_bytes; // tail drop: a repair would follow, not modelled here
        }
        if (queue > *queue_peak) {
            *queue_peak = queue;
        }
        if (!outstanding) {
            rate_note_solicit(&node);
            solicit_ns = test_mock_now;
            snapshot_at = test_mock_now + (base_rtt / 2U) + (queue * 8U * tt_SECOND / bps); // out and through the queue
            outstanding = true;
            snapped = false;
        } else if (!snapped && test_mock_now >= snapshot_at) {
            held_at_solicit = delivered_bytes / 1538U;
            reply_at = test_mock_now + (base_rtt / 2U); // and back
            snapped = true;
        } else if (snapped && test_mock_now >= reply_at) {
            rate_acknack(&node, 0, &ack, (uint32_t)held_at_solicit, true, solicit_ns);
            outstanding = false;
        }
        if ((t + step) % tt_SECOND == 0) {
            rates[t / tt_SECOND] = pacer_rate_bps();
        }
    }
}

static uint64_t steady_4m(uint64_t second) {
    (void)second;
    return 4000000ULL;
}

static uint64_t step_30_4_30(uint64_t second) {
    return (second >= 8 && second < 16) ? 4000000ULL : 30000000ULL;
}

static void test_the_bucket_follows_the_link(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = tt_RATE_CONTROL_ADAPTIVE;
    create(&opts);
    uint64_t rates[32];
    uint64_t queue_peak = 0;
    fluid_link(steady_4m, 40U * 1538U, 12, rates, &queue_peak);
    // After the start the bucket is near the link's rate, never far above it, and the queue stays well under its limit.
    for (uint32_t s = 4; s < 12; s++) {
        EXPECT_TRUE(rates[s] >= 4000000ULL * 6U / 10U);
        EXPECT_TRUE(rates[s] <= 4000000ULL * 14U / 10U);
    }
    EXPECT_TRUE(node.rate[0].ap.state == AP_CRUISE);
    destroy();

    create(&opts);
    fluid_link(step_30_4_30, 60U * 1538U, 24, rates, &queue_peak);
    printf("test_rate_control: 30 -> 4 -> 30 Mbit/s fluid link, bucket rate by second (Mbit/s):");
    for (uint32_t s = 0; s < 24; s++) {
        printf(" %.1f", (double)rates[s] / 1e6);
    }
    printf("\n");
    EXPECT_TRUE(rates[7] >= 30000000ULL * 6U / 10U);  // before the drop
    EXPECT_TRUE(rates[10] <= 4000000ULL * 14U / 10U); // followed the drop
    EXPECT_TRUE(rates[10] >= 4000000ULL * 6U / 10U);
    EXPECT_TRUE(rates[14] <= 4000000ULL * 14U / 10U);
    EXPECT_TRUE(rates[20] >= 30000000ULL * 6U / 10U); // and the rise
    EXPECT_TRUE(rates[22] >= 30000000ULL * 6U / 10U);
    destroy();
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_zero_keeps_what_pacing_rate_meant();
    test_adaptive_defaults();
    test_validation();
    test_adaptive_starts_between_floor_and_cap();
    test_control_datagrams_are_not_units();
    test_held_units_from_an_acknack();
    test_readers_pool_into_one_link_count();
    test_the_bucket_follows_the_link();
    test_a_large_send_on_an_adaptive_link();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_rate_control: all tests passed\n");
    return 0;
}
