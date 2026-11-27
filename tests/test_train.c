/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The packet train (tt_HEARTBEAT_FLAG_TRAIN; adaptive_pacing/DESIGN.md 28), through the mock HAL, built as rmw_tickle
// builds core:
//   - the estimator (ap_train_sample()): a first sample, an immediate drop, a bounded rise and a rise the median
//   confirms, the
//     recovery after a one-sample dip, an outlier too high, implausible samples, the least over one round's readers,
//     the guards that block rises when trains read too high, a believed prior;
//   - the reader (train_rx_note(), train_rx_close(), train_rx_take()): the span of a complete train in 8 us units, none
//   for a
//     lost, reordered, too long or stale one, a sequence wrap, one slot a writer;
//   - the writer on an ADAPTIVE link: the first 20 datagrams go back to back, the TRAIN HEARTBEAT names the 20th, the
//   rest is
//     paced after the train's debt; an answer sets the rate; an answer of 0 (an older reader), or one with no train
//     outstanding, changes nothing; three unanswered trains stop them for TRAIN_SKIP_SAMPLES samples; no train goes on
//     a link that is not ADAPTIVE.

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

#define MBIT 1000000ULL
#define LINK_BROADCAST "10.0.0.255"
#define PEER_ID 9
#define PEER_IP 0x0a000009U
#define PEER_ENTITY 0x22220001U
#define MS_NS 1000000ULL

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

// ---- The estimator

static void new_pacer(struct ap_pacer* ap) {
    ap_init(ap, 100000ULL, 1000000000ULL, 0, 20U * MS_NS, 1538U);
}

// A sample of `bps` read over a span of 1 ms (long enough to be believed).
static bool sample(struct ap_pacer* ap, uint32_t round, uint64_t bps) {
    return ap_train_sample(ap, 10U * tt_SECOND, round, bps, 1U * MS_NS);
}

static void test_first_sample_sets_the_rate(void) {
    struct ap_pacer ap;
    new_pacer(&ap);
    EXPECT_TRUE(ap.state == AP_START);
    EXPECT_TRUE(sample(&ap, 1, 30U * MBIT));
    EXPECT_TRUE(ap.state == AP_CRUISE);
    EXPECT_EQ_U64(30U * MBIT, ap.train_est_bps);
    EXPECT_EQ_U64(28500000ULL, ap.rate_bps); // 95 %
    EXPECT_EQ_U64(28500000ULL, ap_btlbw_bps(&ap));
    EXPECT_TRUE(ap.train_fresh_until_ns > 10U * tt_SECOND); // probes wait
}

static void test_a_drop_is_taken_at_once_and_a_rise_is_bounded(void) {
    struct ap_pacer ap;
    new_pacer(&ap);
    EXPECT_TRUE(sample(&ap, 1, 30U * MBIT));
    EXPECT_TRUE(sample(&ap, 2, 4U * MBIT)); // one train says the link fell: believed at once
    EXPECT_EQ_U64(4U * MBIT, ap.train_est_bps);
    EXPECT_EQ_U64(3800000ULL, ap.rate_bps);
    EXPECT_TRUE(sample(&ap, 3, 4U * MBIT));
    // A real step up: 4 -> 30. Median of (4, 4, 30) is 4: one train raises it by 1.5x only; the next agrees, and it is
    // taken.
    EXPECT_TRUE(sample(&ap, 4, 30U * MBIT));
    EXPECT_EQ_U64(6U * MBIT, ap.train_est_bps);
    EXPECT_TRUE(sample(&ap, 5, 30U * MBIT));
    EXPECT_EQ_U64(30U * MBIT, ap.train_est_bps);
    EXPECT_EQ_U64(28500000ULL, ap.rate_bps);
}

static void test_a_one_train_dip_recovers_at_the_next(void) {
    struct ap_pacer ap;
    new_pacer(&ap);
    EXPECT_TRUE(sample(&ap, 1, 30U * MBIT));
    EXPECT_TRUE(sample(&ap, 2, 30U * MBIT));
    EXPECT_TRUE(sample(&ap, 3, 4U * MBIT)); // the outlier, low: taken (an over-estimate is the dangerous one)
    EXPECT_EQ_U64(4U * MBIT, ap.train_est_bps);
    EXPECT_TRUE(sample(&ap, 4, 30U * MBIT)); // median of (30, 4, 30) is 30: back at once
    EXPECT_EQ_U64(30U * MBIT, ap.train_est_bps);
}

static void test_one_train_too_high_is_bounded(void) {
    struct ap_pacer ap;
    new_pacer(&ap);
    EXPECT_TRUE(sample(&ap, 1, 30U * MBIT));
    EXPECT_TRUE(sample(&ap, 2, 30U * MBIT));
    EXPECT_TRUE(sample(&ap, 3, 300U * MBIT)); // 10x: plausible, but alone it moves the estimate 1.5x at most
    EXPECT_EQ_U64(45U * MBIT, ap.train_est_bps);
    EXPECT_TRUE(sample(&ap, 4, 30U * MBIT));
    EXPECT_EQ_U64(30U * MBIT, ap.train_est_bps);
}

static void test_implausible_samples_are_not_read(void) {
    struct ap_pacer ap;
    new_pacer(&ap);
    EXPECT_TRUE(!ap_train_sample(&ap, 5U * tt_SECOND, 1, 30U * MBIT, 0));              // no span
    EXPECT_TRUE(!ap_train_sample(&ap, 5U * tt_SECOND, 1, 30U * MBIT, 8000U));          // under the clock's resolution
    EXPECT_TRUE(!ap_train_sample(&ap, 5U * tt_SECOND, 1, 30U * MBIT, 3U * tt_SECOND)); // a train that took 3 s
    EXPECT_TRUE(!ap_train_sample(&ap, 5U * tt_SECOND, 1, 0, 1U * MS_NS));              // no rate
    EXPECT_TRUE(!ap_train_sample(&ap, 5U * tt_SECOND, 1, 2000U * MBIT, 1U * MS_NS));   // above the line rate
    EXPECT_EQ_U64(0, ap.train_applied);
    EXPECT_TRUE(ap.state == AP_START);
    EXPECT_TRUE(sample(&ap, 1, 30U * MBIT));
    EXPECT_TRUE(!sample(&ap, 2, 600U * MBIT)); // more than 16x the last estimate
    EXPECT_EQ_U64(30U * MBIT, ap.train_est_bps);
    EXPECT_EQ_U64(6, ap.train_rejected);
}

static void test_the_least_over_a_rounds_readers(void) {
    struct ap_pacer ap;
    new_pacer(&ap);
    EXPECT_TRUE(sample(&ap, 1, 30U * MBIT));
    EXPECT_TRUE(sample(&ap, 2, 30U * MBIT));
    EXPECT_TRUE(sample(&ap, 2, 20U * MBIT)); // a second reader of the same sample reads less: the link's is the least
    EXPECT_EQ_U64(20U * MBIT, ap.train_est_bps);
    EXPECT_TRUE(sample(&ap, 2, 25U * MBIT)); // and a third that reads more does not raise it
    EXPECT_EQ_U64(20U * MBIT, ap.train_est_bps);
    EXPECT_EQ_U32(2, ap.train_hist_n);
}

static void test_trains_that_read_too_high_are_blocked(void) {
    struct ap_pacer ap;
    new_pacer(&ap);
    // A burst-forming link: every train reads the burst's line rate, and what is sent at it trips the guards.
    EXPECT_TRUE(sample(&ap, 1, 900U * MBIT));
    EXPECT_EQ_U64(900U * MBIT, ap.train_est_bps);
    ap.queue_trips += 3;
    EXPECT_TRUE(sample(&ap, 2, 900U * MBIT)); // one sample after the guards tripped
    ap.queue_trips += 3;
    EXPECT_TRUE(sample(&ap, 3, 900U * MBIT)); // two in a row: rises are blocked from now
    EXPECT_EQ_U32(AP_TRAIN_BLOCK_SAMPLES, ap.train_block);
    // The estimator finds the real rate by itself (the drop guard's reset); the train's reading no longer pushes it up.
    ap_filter_reset(&ap.bw, 28U * MBIT, ap.rounds);
    EXPECT_TRUE(sample(&ap, 4, 900U * MBIT));
    EXPECT_EQ_U64(28U * MBIT, ap_btlbw_bps(&ap)); // the train's 900 does not raise it
    // But a train that reads lower than that is still believed.
    EXPECT_TRUE(sample(&ap, 5, 10U * MBIT));
    EXPECT_EQ_U64(9500000ULL, ap_btlbw_bps(&ap));
    // And the block ends: AP_TRAIN_BLOCK_SAMPLES rounds on, a train rises again (by the bounded step).
    for (uint32_t round = 6; round < 6U + AP_TRAIN_BLOCK_SAMPLES; round++) {
        EXPECT_TRUE(sample(&ap, round, 10U * MBIT));
    }
    EXPECT_EQ_U32(0, ap.train_block);
}

static void test_a_believed_prior_is_corrected_only_when_wrong(void) {
    struct ap_pacer ap;
    ap_init(&ap, 100000ULL, 30U * MBIT, 0, 20U * MS_NS, 1538U); // a prior: 30 Mbit/s
    EXPECT_TRUE(ap.state == AP_ANCHOR);
    uint64_t rate = ap.rate_bps;
    EXPECT_TRUE(sample(&ap, 1, 29U * MBIT)); // agrees: nothing changes
    EXPECT_TRUE(ap.state == AP_ANCHOR);
    EXPECT_EQ_U64(rate, ap.rate_bps);
    EXPECT_TRUE(sample(&ap, 2, 10U * MBIT)); // a link a third of it: the prior is wrong
    EXPECT_TRUE(ap.state == AP_CRUISE);
    EXPECT_TRUE(within_pct(ap.rate_bps, 10500000ULL, 2));
}

static void test_no_train_is_no_change(void) {
    struct ap_pacer ap;
    new_pacer(&ap);
    EXPECT_EQ_U64(0, ap.train_est_bps);
    EXPECT_EQ_U64(0, ap.train_fresh_until_ns);
    EXPECT_EQ_U64(0, ap.train_samples);
}

// ---- The reader

static struct tt_Context rx;

static void rx_reset(void) {
    memset(&rx, 0, sizeof(rx));
    test_mock_reset();
    test_mock_now = 100U * tt_SECOND;
}

// Fragments 0 .. count-1 of a sample whose first fragment has seq_no `base`, `gap_us` apart.
static void rx_train(uint8_t source, uint32_t entity, uint32_t base, uint32_t count, uint64_t gap_ns) {
    uint64_t t0 = test_mock_now;
    for (uint32_t i = 0; i < count; i++) {
        test_mock_now = t0 + (i * gap_ns);
        train_rx_note(&rx, source, entity, base + i, i);
    }
}

static void test_reader_span_of_a_complete_train(void) {
    rx_reset();
    rx_train(5, 0x77, 100, 20, 50000ULL); // 19 gaps of 50 us: 950 us = 118.75 units of 8 us
    train_rx_close(&rx, 5, 0x77, 119);
    EXPECT_EQ_U32(119, train_rx_take(&rx, 5, 0x77));
    EXPECT_EQ_U32(0, train_rx_take(&rx, 5, 0x77)); // once
}

static void test_reader_gives_none_for_a_bad_train(void) {
    rx_reset();
    // a fragment missing
    uint64_t t0 = test_mock_now;
    for (uint32_t i = 0; i < 20; i++) {
        if (i != 7U) {
            test_mock_now = t0 + (i * 50000ULL);
            train_rx_note(&rx, 5, 0x77, 100 + i, i);
        }
    }
    train_rx_close(&rx, 5, 0x77, 119);
    EXPECT_EQ_U32(0, train_rx_take(&rx, 5, 0x77));
    // reordered: fragment 9 stamped before fragment 8
    rx_reset();
    t0 = test_mock_now;
    for (uint32_t i = 0; i < 20; i++) {
        test_mock_now = t0 + (i * 50000ULL) - (i == 9U ? 100000ULL : 0U);
        train_rx_note(&rx, 5, 0x77, 100 + i, i);
    }
    train_rx_close(&rx, 5, 0x77, 119);
    EXPECT_EQ_U32(0, train_rx_take(&rx, 5, 0x77));
    // a train longer than the stamps kept, and one of a single datagram
    rx_reset();
    rx_train(5, 0x77, 100, tt_TRAIN_MAX_K, 50000ULL);
    train_rx_close(&rx, 5, 0x77, 100 + tt_TRAIN_MAX_K); // names one past the last kept
    EXPECT_EQ_U32(0, train_rx_take(&rx, 5, 0x77));
    train_rx_close(&rx, 5, 0x77, 100); // K = 1
    EXPECT_EQ_U32(0, train_rx_take(&rx, 5, 0x77));
    train_rx_close(&rx, 5, 0x77, 100 + tt_TRAIN_MAX_K - 1U); // the longest kept: fine
    EXPECT_TRUE(train_rx_take(&rx, 5, 0x77) != 0);
    // never heard of this writer
    train_rx_close(&rx, 6, 0x77, 119);
    EXPECT_EQ_U32(0, train_rx_take(&rx, 6, 0x77));
}

static void test_reader_span_is_stale_after_a_second_and_saturates(void) {
    rx_reset();
    rx_train(5, 0x77, 100, 20, 50000ULL);
    train_rx_close(&rx, 5, 0x77, 119);
    test_mock_now += 2U * tt_SECOND;
    EXPECT_EQ_U32(0, train_rx_take(&rx, 5, 0x77));
    rx_reset();
    rx_train(5, 0x77, 100, 20, 40000000ULL); // 19 x 40 ms = 760 ms = 95,000 units: saturates
    train_rx_close(&rx, 5, 0x77, 119);
    EXPECT_EQ_U32(UINT16_MAX, train_rx_take(&rx, 5, 0x77));
}

static void test_reader_sequence_wrap_and_new_samples(void) {
    rx_reset();
    rx_train(5, 0x77, 0xFFFFFFF5U, 20, 50000ULL); // seq_no wraps inside the train
    train_rx_close(&rx, 5, 0x77, 0xFFFFFFF5U + 19U);
    EXPECT_EQ_U32(119, train_rx_take(&rx, 5, 0x77));
    // A new sample's fragment 0 starts over: the HEARTBEAT of the old train, arriving late, finds none.
    rx_train(5, 0x77, 500, 3, 50000ULL);
    train_rx_close(&rx, 5, 0x77, 0xFFFFFFF5U + 19U);
    EXPECT_EQ_U32(0, train_rx_take(&rx, 5, 0x77));
    // Fragments of a sample whose fragment 0 was not seen are not stamped.
    train_rx_note(&rx, 5, 0x77, 901, 1);
    train_rx_close(&rx, 5, 0x77, 901);
    EXPECT_EQ_U32(0, train_rx_take(&rx, 5, 0x77));
}

static void test_reader_keeps_a_slot_a_writer(void) {
    rx_reset();
    for (uint32_t w = 0; w < tt_TRAIN_RX_WRITERS; w++) {
        rx_train((uint8_t)(10 + w), 0x100 + w, 100, 20, 50000ULL);
    }
    for (uint32_t w = 0; w < tt_TRAIN_RX_WRITERS; w++) {
        train_rx_close(&rx, (uint8_t)(10 + w), 0x100 + w, 119);
    }
    for (uint32_t w = 0; w < tt_TRAIN_RX_WRITERS; w++) {
        EXPECT_EQ_U32(119, train_rx_take(&rx, (uint8_t)(10 + w), 0x100 + w));
    }
    rx_train(99, 0x999, 100, 20, 50000ULL); // a fifth writer takes a slot (round robin), the table stays consistent
    train_rx_close(&rx, 99, 0x999, 119);
    EXPECT_EQ_U32(119, train_rx_take(&rx, 99, 0x999));
}

// ---- The writer on an ADAPTIVE link

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

enum sent_kind { SENT_OTHER, SENT_FRAGMENT, SENT_HEARTBEAT };
struct sent {
    uint64_t at_ns;
    uint8_t kind;
    uint8_t flags;
    uint32_t seq_no; // a HEARTBEAT's last_seq_no
};
static struct sent sent_log[1024];
static uint32_t sent_n;

static void capture(const void* buf, size_t len) {
    const uint8_t* bytes = buf;
    if (sent_n >= sizeof(sent_log) / sizeof(sent_log[0]) || len < 8U) {
        return;
    }
    bool single = bytes[0] == tt_SINGLE_MARKER_LE || bytes[0] == tt_SINGLE_MARKER_BE;
    uint8_t type = single ? bytes[3] : bytes[4];
    struct sent* entry = &sent_log[sent_n++];
    *entry = (struct sent) {.at_ns = test_mock_now};
    if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L || type == tt_SUBMESSAGE_TYPE_FRAG_CONT_L) {
        entry->kind = SENT_FRAGMENT;
    } else if (type == tt_SUBMESSAGE_TYPE_HEARTBEAT) {
        const struct tt_HeartbeatHeader* beat =
            (const struct tt_HeartbeatHeader*)(bytes + (single ? sizeof(struct tt_SingleHeader)
                                                               : sizeof(struct tt_Header) +
                                                                     sizeof(struct tt_SubmessageHeader)));
        entry->kind = SENT_HEARTBEAT;
        entry->flags = beat->flags;
        entry->seq_no = beat->last_seq_no;
    }
}

static void create_adaptive_with(uint8_t rate_control, uint8_t fragmentation) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].rate_control = rate_control;
    opts.net.large_fragmentation = fragmentation;
    mock_setup();
    memset(&node, 0, sizeof(node));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&node, &opts));
    memset(&topic, 0, sizeof(topic));
    topic.name = "train_topic";
    topic.data_size = 8;
    topic.data_encode_size = sized_encode_size;
    topic.data_encode = sized_encode;
    struct tt_QosProfile reliable = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 2},
                                     .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    size_t storage = tt_Publisher_storage_size(&topic, &reliable);
    EXPECT_TRUE(storage <= sizeof(pub_storage));
    struct tt_PublisherOptions unbatched = {.batching = tt_SWITCH_OFF};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node), &pub, &topic, "train", &reliable,
                                                      &unbatched, storage != 0 ? pub_storage : NULL, storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&node, test_acquire, test_release, NULL));
    (void)upsert_peer(pub.peers, PEER_ID, PEER_IP, 8282);
    struct tt_PeerAck* ack = claim_peer_ack(&pub, PEER_ID, PEER_ENTITY);
    EXPECT_TRUE(ack != NULL);
    sample_len = 256U * 1024U;
    sent_n = 0;
    test_mock_send_hook = capture;
}

static void create_adaptive(uint8_t rate_control) {
    create_adaptive_with(rate_control,
                         tt_LARGE_FRAGMENTATION_MTU); // MTU units from the first sample: no hybrid measurement
}

static void destroy_adaptive(void) {
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
    EXPECT_EQ_U32(0, buffers_out);
}

static void publish_and_send(void) {
    uint8_t data[8] = {0};
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    run_until_sent();
    EXPECT_TRUE(!large_unsent());
}

static uint32_t count_trains(void) {
    uint32_t trains = 0;
    for (uint32_t i = 0; i < sent_n; i++) {
        trains += sent_log[i].kind == SENT_HEARTBEAT && (sent_log[i].flags & tt_HEARTBEAT_FLAG_TRAIN) != 0 ? 1U : 0U;
    }
    return trains;
}

static void test_the_first_datagrams_go_back_to_back_then_a_train_heartbeat(void) {
    create_adaptive(tt_RATE_CONTROL_ADAPTIVE);
    uint32_t first_seq = pub.seq_no + 1U;
    publish_and_send();
    // The first 20 fragments carry one send time (one batch, past the bucket), the 20th is followed by the HEARTBEAT.
    uint32_t fragments = 0;
    uint32_t heartbeat_at = UINT32_MAX;
    for (uint32_t i = 0; i < sent_n && heartbeat_at == UINT32_MAX; i++) {
        if (sent_log[i].kind == SENT_FRAGMENT) {
            fragments++;
        } else if (sent_log[i].kind == SENT_HEARTBEAT && (sent_log[i].flags & tt_HEARTBEAT_FLAG_TRAIN) != 0) {
            heartbeat_at = i;
        }
    }
    EXPECT_TRUE(heartbeat_at != UINT32_MAX);
    EXPECT_EQ_U32(20, fragments);
    EXPECT_EQ_U32(0, sent_log[heartbeat_at].flags & tt_HEARTBEAT_FLAG_FINAL); // a reply is required
    EXPECT_EQ_U32(first_seq + 19U, sent_log[heartbeat_at].seq_no);
    EXPECT_EQ_U64(sent_log[0].at_ns, sent_log[heartbeat_at - 1U].at_ns); // back to back: no pacing wait between
    EXPECT_EQ_U32(1, count_trains());
    // The rest is paced after the train's debt: its first datagram waits for the 20 before it at the start rate.
    uint32_t after = heartbeat_at + 1U;
    while (after < sent_n && sent_log[after].kind != SENT_FRAGMENT) {
        after++;
    }
    EXPECT_TRUE(after < sent_n);
    EXPECT_TRUE(sent_log[after].at_ns >= sent_log[0].at_ns + (10U * MS_NS));
    // The reader's train is outstanding, with its length and the datagram's wire size.
    struct tt_PeerAck* ack = find_peer_ack(&pub, PEER_ID, PEER_ENTITY);
    EXPECT_TRUE(ack != NULL && ack->train_open);
    EXPECT_EQ_U32(20, ack->train_k);
    EXPECT_TRUE(ack->train_wire > 1400U);
    destroy_adaptive();
}

// The span (in 8 us units) a reader would report for a link of `bps`, for the outstanding train of `ack`.
static uint16_t span_for(const struct tt_PeerAck* ack, uint64_t bps) {
    uint64_t span_ns = ((uint64_t)(ack->train_k - 1U) * ack->train_wire * 8U * tt_SECOND) / bps;
    return (uint16_t)((span_ns + 4000U) / 8000U);
}

static void test_an_answer_sets_the_rate_and_zero_or_a_stray_one_does_not(void) {
    create_adaptive(tt_RATE_CONTROL_ADAPTIVE);
    publish_and_send();
    struct tt_PeerAck* ack = find_peer_ack(&pub, PEER_ID, PEER_ENTITY);
    EXPECT_TRUE(ack != NULL && ack->train_open);
    uint64_t before = node.rate[0].ap.rate_bps;
    // An ACKNACK that answers with 0: an older reader, or a train that did not arrive whole. Nothing changes, and the
    // train stays outstanding for the real answer.
    rate_acknack(&node, PEER_IP, PEER_ID, ack, 10, 10, 0);
    EXPECT_TRUE(ack->train_open);
    EXPECT_EQ_U64(0, node.rate[0].ap.train_samples);
    EXPECT_EQ_U64(before, node.rate[0].ap.rate_bps);
    // The answer: 30 Mbit/s.
    rate_acknack(&node, PEER_IP, PEER_ID, ack, 20, 20, span_for(ack, 30U * MBIT));
    EXPECT_TRUE(!ack->train_open);
    EXPECT_TRUE(within_pct(node.rate[0].ap.train_est_bps, 30U * MBIT, 2));
    EXPECT_TRUE(within_pct(node.rate[0].ap.rate_bps, 28500000ULL, 2));
    EXPECT_TRUE(node.rate[0].ap.state == AP_CRUISE);
    EXPECT_EQ_U64(node.rate[0].ap.rate_bps, node.rate[0].applied_bps); // the bucket has it
    // No train is outstanding now: a reserved field that is not zero is not read (a hand-built ACKNACK leaves it so).
    uint64_t samples = node.rate[0].ap.train_samples;
    rate_acknack(&node, PEER_IP, PEER_ID, ack, 30, 30, 0x7777);
    EXPECT_EQ_U64(samples, node.rate[0].ap.train_samples);
    destroy_adaptive();
}

static void test_unanswered_trains_stop_for_a_while(void) {
    create_adaptive(tt_RATE_CONTROL_ADAPTIVE);
    for (uint32_t sample_no = 0; sample_no < 3; sample_no++) {
        publish_and_send(); // each one's train goes unanswered
    }
    EXPECT_EQ_U32(3, count_trains());
    EXPECT_EQ_U32(0, node.rate[0].train_skip);
    publish_and_send(); // the fourth finds three misses: no train, and none for the next samples
    EXPECT_EQ_U32(3, count_trains());
    EXPECT_EQ_U32(TRAIN_SKIP_SAMPLES, node.rate[0].train_skip);
    publish_and_send();
    EXPECT_EQ_U32(3, count_trains());
    EXPECT_EQ_U32(TRAIN_SKIP_SAMPLES - 1U, node.rate[0].train_skip);
    destroy_adaptive();
}

static void test_no_train_off_an_adaptive_link_or_for_a_short_sample(void) {
    create_adaptive(tt_RATE_CONTROL_OFF);
    publish_and_send();
    EXPECT_EQ_U32(0, count_trains());
    destroy_adaptive();
    create_adaptive(tt_RATE_CONTROL_ADAPTIVE);
    sample_len = 40U * 1400U; // fewer than 2 x 20 fragments
    publish_and_send();
    EXPECT_EQ_U32(0, count_trains());
    destroy_adaptive();
}

static void test_no_train_while_the_hybrid_transport_measures_the_reader(void) {
    // AUTO fragmentation sends a reader's first samples one datagram a call until its loss is measured (never as a
    // train).
    create_adaptive_with(tt_RATE_CONTROL_ADAPTIVE, tt_LARGE_FRAGMENTATION_AUTO);
    publish_and_send();
    EXPECT_EQ_U32(0, count_trains());
    destroy_adaptive();
}

static void test_the_train_grows_with_the_link(void) {
    struct tt_RateControl rate;
    memset(&rate, 0, sizeof(rate));
    rate.unit_wire = 1538U;
    EXPECT_EQ_U32(20, train_length(&rate)); // the link is not known
    rate.ap.train_est_bps = 4U * MBIT;
    EXPECT_EQ_U32(20, train_length(&rate)); // 10 ms of 4 Mbit/s is 3 datagrams: the least
    rate.ap.train_est_bps = 30U * MBIT;
    EXPECT_EQ_U32(24, train_length(&rate)); // 10 ms of 30 Mbit/s: 37.5 KB
    rate.ap.train_est_bps = 100U * MBIT;
    EXPECT_EQ_U32(tt_TRAIN_MAX_K, train_length(&rate)); // 81 datagrams: the most
    rate.ap.train_est_bps = 1000U * MBIT;
    EXPECT_EQ_U32(tt_TRAIN_MAX_K, train_length(&rate));
}

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    test_first_sample_sets_the_rate();
    test_a_drop_is_taken_at_once_and_a_rise_is_bounded();
    test_a_one_train_dip_recovers_at_the_next();
    test_one_train_too_high_is_bounded();
    test_implausible_samples_are_not_read();
    test_the_least_over_a_rounds_readers();
    test_trains_that_read_too_high_are_blocked();
    test_a_believed_prior_is_corrected_only_when_wrong();
    test_no_train_is_no_change();
    test_reader_span_of_a_complete_train();
    test_reader_gives_none_for_a_bad_train();
    test_reader_span_is_stale_after_a_second_and_saturates();
    test_reader_sequence_wrap_and_new_samples();
    test_reader_keeps_a_slot_a_writer();
    test_the_first_datagrams_go_back_to_back_then_a_train_heartbeat();
    test_an_answer_sets_the_rate_and_zero_or_a_stray_one_does_not();
    test_unanswered_trains_stop_for_a_while();
    test_no_train_off_an_adaptive_link_or_for_a_short_sample();
    test_no_train_while_the_hybrid_transport_measures_the_reader();
    test_the_train_grows_with_the_link();
    if (test_failures != 0) {
        printf("test_train: %d check(s) failed\n", test_failures);
        return 1;
    }
    printf("test_train: all tests passed\n");
    return 0;
}
