/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A RELIABLE Subscriber's reorder buffer against its ACK window (ROADMAP "Other open items", the user's item 4(a) of
// 2026-10-20), through a simulated link that loses a share of every datagram both ways.
//
// The storm: a sample that arrives ahead of a gap and finds its reorder slot taken is un-received - its bit cleared, so
// it is asked for again (hold_for_reorder()). The next arrival then reports that cleared position as a new gap and
// NACKs it at once (update_reliable_ack()); the retransmission finds the slot still taken and is un-received again. A
// writer's held samples lie 1 to window - 1 past its watermark, so a buffer of window - 1 slots or more never collides
// with itself; fewer do, and at 20% loss 16, 64 or 128 slots against the default 256-sample window delivered 65, 340
// and 244 of 2,000 samples after 21, 22 and 9 million DATA (main 1511981c, before the check). No buffer at all
// (tt_QOS_REORDER_NONE) sets no bit past the gap and does not storm.
//
// The check (tt_QosProfile_resolve()): a RELIABLE Subscriber's reorder_slots must be tt_QOS_REORDER_NONE or at least
// its window less one, or the profile is refused - creation fails and tt_Subscriber_storage_size() is 0. A zero
// reorder_slots is the window. Pre-registered, and enforced below rather than stated:
//   - every profile the check accepts delivers all samples, in order, at under 3 DATA a sample through 20% loss,
//     window - 1 slots included; a storm there falsifies the bound;
//   - the profiles it refuses are ones that storm without it: with the check removed,
//     test_every_accepted_profile_survives_loss() goes red on 16, 64 and 128 slots and on 1024 of 4096;
//   - a buffer sized as before (255, 256 or 512 of 256, 4096 of 4096, NONE) sends exactly the DATA and ACKNACKs it
//     sent on main before the check - the control the check cannot touch, since no receive path changed.
//
// Several writers on one Subscriber share its slots, so their held samples collide whatever the bound; the same storm
// followed until a collision with another writer's sample made new gaps NACK from the watermark up
// (extend_gap_to_watermark()). test_several_writers_share_the_buffer() has the counts, its rules and its control.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define RECEIVER_ID 2
#define RECEIVER_IP 0x0a000002
#define PORT 8282
#define ENDPOINT_NAME "reorder_window"
#define SAMPLE_LEN 64U
#define MAX_WRITERS 4U

// Writer w's node id and address: 1 for the first, as the single-writer runs always had, then 3, 4, 5.
static uint8_t writer_id(uint32_t w) {
    return (uint8_t)(w == 0 ? 1U : w + 2U);
}

static uint32_t writer_ip(uint32_t w) {
    return 0x0a000000U + writer_id(w);
}

// The sample carries its writer's index in its first byte, so the receiver can check order per writer.
static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)SAMPLE_LEN;
}

static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    memset(payload, 0x5a, len);
    payload[0] = (uint8_t)*(const uint64_t*)data;
    return (int32_t)len;
}

static uint8_t decoded_writer;
static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    decoded_writer = len != 0 ? payload[0] : 0xffU;
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static struct tt_Context senders[MAX_WRITERS];
static struct tt_Context receiver;
static struct tt_TypeSupport sender_topic;
static struct tt_TypeSupport receiver_topic;
static struct tt_Publisher pubs[MAX_WRITERS];
static struct tt_Subscriber sub;
static uint32_t sim_writers = 1;

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

static void match_by_broadcast(struct tt_Publisher* publisher) {
    for (uint8_t peer = 0; peer <= tt_UNICAST_PEER_THRESHOLD; peer++) {
        (void)upsert_peer(publisher->peers, (uint8_t)(100 + peer), 0x0a000100U + peer, PORT);
    }
}

// --- the simulated link ---------------------------------------------------------------------------

#define SIM_QUEUE 16384
struct sim_datagram {
    uint8_t from;
    uint32_t to_ip; // 0: broadcast
    uint32_t len;
    uint8_t bytes[tt_MAX_BUFFER_LENGTH];
};
static struct sim_datagram sim_queue[SIM_QUEUE];
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;
static uint32_t sim_loss_per_mille;
static uint32_t sim_rng;
static uint64_t sim_data_sent;     // DATA submessages the writers put on the wire, originals and resends
static uint64_t sim_acknacks_sent; // ACKNACK submessages the receiver put on the wire
static bool sim_overran;

static void sim_count_submessages(const void* buf, size_t len) {
    uint8_t classic[tt_MAX_BUFFER_LENGTH + sizeof(struct tt_Header)];
    size_t n = test_classic_form(buf, len, classic);
    size_t at = sizeof(struct tt_Header);
    while (at + sizeof(struct tt_SubmessageHeader) <= n) {
        const struct tt_SubmessageHeader* sh = (const struct tt_SubmessageHeader*)(classic + at);
        if (sh->type == tt_SUBMESSAGE_TYPE_DATA && sim_acting != RECEIVER_ID) {
            sim_data_sent++;
        } else if (sh->type == tt_SUBMESSAGE_TYPE_ACKNACK && sim_acting == RECEIVER_ID) {
            sim_acknacks_sent++;
        }
        if (sh->length < sizeof(struct tt_SubmessageHeader)) {
            break;
        }
        at += sh->length;
    }
}

static void sim_capture(const void* buf, size_t len) {
    if (sim_tail - sim_head >= SIM_QUEUE || len > tt_MAX_BUFFER_LENGTH) {
        sim_overran = true;
        return;
    }
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->to_ip = test_mock_send_hook_ip;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
    sim_count_submessages(buf, len);
}

static bool sim_lost(void) {
    sim_rng = (sim_rng * 1103515245U) + 12345U;
    return ((sim_rng >> 16) % 1000U) < sim_loss_per_mille;
}

// A run that has overflowed this often is a storm already. A zero-latency link ping-pongs one for minutes inside a
// single drain (each un-received retransmission NACKed at once by the next arrival), so the drain stops there too.
#define SIM_OVERFLOW_CAP 200000U

// A writer's datagram goes to the receiver; the receiver's to the writer it is addressed to, or to every writer when
// broadcast. One loss draw a datagram either way, so a single-writer run draws exactly as it always did.
static void sim_drain(void) {
    while (sim_head != sim_tail) {
        if (sub.reorder_overflow >= SIM_OVERFLOW_CAP) {
            sim_head = sim_tail;
            return;
        }
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        if (sim_lost()) {
            continue;
        }
        if (d->from != RECEIVER_ID) {
            sim_acting = RECEIVER_ID;
            process_packet(&receiver, d->bytes, 0, d->len, 0x0a000000U + d->from, PORT, tt_TRANSPORT_UDP);
            continue;
        }
        for (uint32_t w = 0; w < sim_writers; w++) {
            if (d->to_ip == 0 || d->to_ip == writer_ip(w)) {
                sim_acting = senders[w].id;
                process_packet(&senders[w], d->bytes, 0, d->len, RECEIVER_IP, PORT, tt_TRANSPORT_UDP);
            }
        }
    }
}

static void sim_run_due(struct tt_Context* node) {
    bool has_next = false;
    uint64_t next = 0;
    sim_acting = node->id;
    while (run_due_entry(node, test_mock_now, &has_next, &next)) {
    }
    sim_drain();
}

static uint64_t sim_next_due(void) {
    uint64_t earliest = UINT64_MAX;
    for (uint32_t w = 0; w <= sim_writers; w++) {
        uint64_t t = UINT64_MAX;
        if (sched_next_time(w < sim_writers ? &senders[w] : &receiver, &t) && t < earliest) {
            earliest = t;
        }
    }
    return earliest;
}

static bool sim_step(void) {
    uint64_t next = sim_next_due();
    if (next == UINT64_MAX) {
        return false;
    }
    if (next > test_mock_now) {
        test_mock_now = next;
    }
    for (uint32_t w = 0; w < sim_writers; w++) {
        sim_run_due(&senders[w]);
    }
    sim_run_due(&receiver);
    return true;
}

static uint32_t delivered;
static uint32_t delivered_from[MAX_WRITERS];
static uint16_t first_seq[MAX_WRITERS];
static uint16_t last_seq[MAX_WRITERS];
static bool in_order;   // every writer's samples in increasing order
static bool contiguous; // ...and none skipped after the first
static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)data;
    uint32_t w = decoded_writer;
    delivered++;
    if (w >= sim_writers) {
        in_order = false; // a sample from no writer of this run
        contiguous = false;
        return;
    }
    if (delivered_from[w] == 0) {
        first_seq[w] = seq_no;
    } else {
        in_order = in_order && (int16_t)(seq_no - last_seq[w]) > 0;
        contiguous = contiguous && (uint16_t)(seq_no - last_seq[w]) == 1U;
    }
    last_seq[w] = seq_no;
    delivered_from[w]++;
}

// Each writer's samples from the first its reader heard, or all `samples` until it has heard one.
static uint32_t owed_samples(uint32_t samples) {
    uint32_t owed = 0;
    for (uint32_t w = 0; w < sim_writers; w++) {
        owed += delivered_from[w] == 0 ? samples : samples - first_seq[w] + 1U;
    }
    return owed;
}

#define SIM_CACHE_DEPTH 4096
static struct tt_ReliableCacheIndex sim_cache_index[MAX_WRITERS][SIM_CACHE_DEPTH];
static uint8_t sim_cache_arena[MAX_WRITERS]
                              [tt_RELIABLE_CACHE_ARENA_BYTES(SIM_CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(SAMPLE_LEN))];
static struct tt_ReliableCache sim_cache[MAX_WRITERS];
static uint64_t sub_storage[(size_t)2U * 1024U * 1024U / sizeof(uint64_t)];

struct run_result {
    tt_ret_t created;
    uint32_t writers;
    uint32_t samples; // every writer's together
    uint32_t delivered;
    uint32_t delivered_min; // the fewest any one writer had delivered
    uint32_t owed;          // every writer's samples from the first its reader heard (a VOLATILE reader is owed no
                            // sample before its first contact with the writer) to the last published
    bool in_order;          // per writer
    bool contiguous;        // per writer, nothing skipped after its first
    uint64_t data_sent;
    uint64_t acknacks;
    uint32_t overflow;
    uint32_t held_peak;
    uint64_t took_ns;
};

// Writer w publishes one sample, waiting out a full history up to `budget`: false when it could not.
static bool sim_publish(uint32_t w, uint64_t budget) {
    uint64_t sample = w;
    sim_acting = senders[w].id;
    tt_ret_t result = tt_Publisher_publish(&pubs[w], (struct tt_Data*)&sample);
    uint64_t blocked_since = test_mock_now;
    while (result == tt_RET_WOULD_BLOCK && test_mock_now - blocked_since < budget &&
           sub.reorder_overflow < SIM_OVERFLOW_CAP) {
        sim_drain();
        if (!sim_step()) {
            break;
        }
        sim_acting = senders[w].id;
        result = tt_Publisher_publish(&pubs[w], (struct tt_Data*)&sample);
    }
    if (result != tt_RET_OK) {
        return false;
    }
    sim_drain();
    return true;
}

// `writers` RELIABLE KEEP_ALL writers, each on its own node with its own cache of SIM_CACHE_DEPTH entries, and one
// RELIABLE reader with `reorder_slots` and `tracking_words`. Each writer publishes `samples`, the writers in turn and
// every round 100 us after the last, through a link losing loss_per_mille both ways; then the protocol runs until
// everything is delivered or `budget` has passed. One writer is the harness as it always was, draw for draw.
static struct run_result sim_run_writers(uint32_t writers, uint16_t reorder_slots, uint16_t tracking_words,
                                         uint32_t samples, uint32_t loss_per_mille, uint64_t budget) {
    struct run_result r;
    memset(&r, 0, sizeof(r));
    r.writers = writers;
    r.samples = writers * samples;
    sim_writers = writers;
    static uint64_t pub_storage[MAX_WRITERS][(size_t)64U * 1024U / sizeof(uint64_t)];
    const struct tt_QosProfile pub_qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL, .depth = 1},
                                          .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    struct tt_QosProfile sub_qos = TEST_QOS_RELIABLE_READER;
    sub_qos.reliability.reorder_slots = reorder_slots;
    sub_qos.reliability.tracking_words = tracking_words;

    test_mock_reset();
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = ENDPOINT_NAME;
    sender_topic.data_size = 8;
    sender_topic.data_encode_size = sample_encode_size;
    sender_topic.data_encode = sample_encode;
    for (uint32_t w = 0; w < writers; w++) {
        init_bare_node(&senders[w], writer_id(w));
        size_t pub_size = tt_Publisher_storage_size(&sender_topic, &pub_qos);
        EXPECT_TRUE(pub_size <= sizeof(pub_storage[w]));
        EXPECT_EQ_INT(tt_RET_OK,
                      tt_Node_create_publisher(
                          tt_Context_default_node(&senders[w]), &pubs[w], &sender_topic, ENDPOINT_NAME, &pub_qos,
                          &(struct tt_PublisherOptions) {.heartbeat_period_ns = 10 * tt_MILLISECOND}, pub_storage[w],
                          pub_size));
        match_by_broadcast(&pubs[w]);
    }

    init_bare_node(&receiver, RECEIVER_ID);
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = ENDPOINT_NAME;
    receiver_topic.data_size = 8;
    receiver_topic.max_encoded_size = SAMPLE_LEN;
    receiver_topic.data_decode = sample_decode;
    receiver_topic.data_free = free_nothing;
    size_t sub_size = tt_Subscriber_storage_size(&receiver_topic, &sub_qos);
    EXPECT_TRUE(sub_size <= sizeof(sub_storage));
    const struct tt_SubscriberOptions sub_options = {.callback = on_data};
    r.created = tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub, &receiver_topic, ENDPOINT_NAME,
                                          &sub_qos, &sub_options, sub_size != 0 ? sub_storage : NULL, sub_size);
    if (r.created != tt_RET_OK) {
        return r;
    }

    sim_head = sim_tail = 0;
    sim_loss_per_mille = loss_per_mille;
    sim_rng = 1;
    sim_data_sent = 0;
    sim_acknacks_sent = 0;
    sim_overran = false;
    delivered = 0;
    memset(delivered_from, 0, sizeof(delivered_from));
    memset(first_seq, 0, sizeof(first_seq));
    memset(last_seq, 0, sizeof(last_seq));
    in_order = true;
    contiguous = true;

    for (uint32_t w = 0; w < writers; w++) {
        memset(sim_cache_index[w], 0, sizeof(sim_cache_index[w]));
        memset(&sim_cache[w], 0, sizeof(sim_cache[w]));
        sim_cache[w].index = sim_cache_index[w];
        sim_cache[w].capacity = SIM_CACHE_DEPTH;
        sim_cache[w].depth = SIM_CACHE_DEPTH;
        sim_cache[w].arena = sim_cache_arena[w];
        sim_cache[w].arena_size = (uint32_t)sizeof(sim_cache_arena[w]);
        pubs[w].reliable_cache = &sim_cache[w];
    }

    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
    bool stopped = false;
    for (uint32_t i = 0; i < samples && !stopped; i++) {
        for (uint32_t w = 0; w < writers; w++) {
            sim_run_due(&senders[w]);
        }
        sim_run_due(&receiver);
        // A writer blocked past the budget, or the run stopped at the overflow cap: what was delivered says which.
        for (uint32_t w = 0; w < writers && !stopped; w++) {
            stopped = !sim_publish(w, budget);
        }
        if (!stopped) {
            test_mock_now += 100 * tt_MICROSECOND;
        }
    }
    uint64_t published = test_mock_now;
    while (delivered < owed_samples(samples) && test_mock_now - published < budget &&
           sub.reorder_overflow < SIM_OVERFLOW_CAP) {
        if (!sim_step()) {
            break;
        }
    }
    test_mock_send_hook = NULL;
    EXPECT_TRUE(!sim_overran);
    r.delivered = delivered;
    r.delivered_min = UINT32_MAX;
    for (uint32_t w = 0; w < writers; w++) {
        r.delivered_min = delivered_from[w] < r.delivered_min ? delivered_from[w] : r.delivered_min;
    }
    r.owed = owed_samples(samples);
    r.in_order = in_order;
    r.contiguous = contiguous;
    for (uint32_t w = 0; w < writers; w++) {
        r.contiguous = r.contiguous && last_seq[w] == (uint16_t)samples; // ...up to the last published
    }
    r.data_sent = sim_data_sent;
    r.acknacks = sim_acknacks_sent;
    r.overflow = sub.reorder_overflow;
    r.held_peak = sub.reorder_held_peak;
    r.took_ns = test_mock_now - published;
    return r;
}

static struct run_result sim_run(uint16_t reorder_slots, uint16_t tracking_words, uint32_t samples,
                                 uint32_t loss_per_mille, uint64_t budget) {
    return sim_run_writers(1, reorder_slots, tracking_words, samples, loss_per_mille, budget);
}

static void print_run(const char* label, uint16_t slots, uint16_t words, uint32_t loss, const struct run_result* r) {
    printf("test_reorder_window: %-10s slots=%-5u window=%-4u loss=%2u%% created=%d delivered=%u/%u in_order=%d "
           "DATA sent=%lu (resends %lu, %.2f a sample) ACKNACKs=%lu overflow=%u recovery=%.1f ms\n",
           label, (unsigned)slots, (unsigned)words * 64U, (unsigned)(loss / 10U), (int)r->created, r->delivered,
           r->samples, (int)r->in_order, (unsigned long)r->data_sent,
           (unsigned long)(r->data_sent > r->samples ? r->data_sent - r->samples : 0),
           r->samples != 0 ? (double)r->data_sent / r->samples : 0.0, (unsigned long)r->acknacks, r->overflow,
           (double)r->took_ns / 1e6);
}

#define SAMPLES 2000U
#define WIDE_WORDS 64U // a 4096-sample window, the widest of a build without large samples

static uint32_t window_of(uint16_t words) {
    return (uint32_t)(words != 0 ? words : tt_RELIABLE_BITMAP_WORDS) * tt_RELIABLE_BITMAP_WORD_BITS;
}

static bool accepted(uint16_t slots, uint16_t words) {
    return slots == tt_QOS_REORDER_NONE || slots == 0 || (uint32_t)slots + 1U >= window_of(words);
}

static void test_zero_slots_resolve_to_the_window(void) {
    static const uint16_t words[] = {0, 1, 4, WIDE_WORDS};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        struct tt_QosProfile request = TEST_QOS_RELIABLE_READER;
        request.reliability.reorder_slots = 0;
        request.reliability.tracking_words = words[i];
        struct tt_QosProfile actual;
        EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(&request, tt_KIND_TOPIC_SUBSCRIBER, NULL, &actual));
        EXPECT_EQ_U32(window_of(words[i]), actual.reliability.reorder_slots);
        struct tt_QosProfile again; // an actual profile resolves to itself
        EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(&actual, tt_KIND_TOPIC_SUBSCRIBER, NULL, &again));
        EXPECT_TRUE(tt_QosProfile_equal(&actual, &again));
    }
    struct tt_QosProfile actual;
    EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(NULL, tt_KIND_TOPIC_SUBSCRIBER, NULL, &actual));
    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_BITS, actual.reliability.reorder_slots);
}

static void test_too_few_slots_are_refused_at_the_boundary(void) {
    // Per window: one slot and one below the bound refused; the bound, the window and NONE accepted.
    static const uint16_t words[] = {1, 4, WIDE_WORDS};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        const uint32_t window = window_of(words[i]);
        const struct {
            uint32_t slots;
            tt_ret_t expected;
        } cases[] = {{1, tt_RET_INVALID_ARGUMENT},
                     {window - 2U, tt_RET_INVALID_ARGUMENT},
                     {window - 1U, tt_RET_OK},
                     {window, tt_RET_OK},
                     {tt_QOS_REORDER_NONE, tt_RET_OK}};
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            struct tt_QosProfile request = TEST_QOS_RELIABLE_READER;
            request.reliability.reorder_slots = (uint16_t)cases[c].slots;
            request.reliability.tracking_words = words[i];
            struct tt_QosProfile actual;
            memset(&actual, 0x7f, sizeof(actual));
            EXPECT_EQ_INT(cases[c].expected, tt_QosProfile_resolve(&request, tt_KIND_TOPIC_SUBSCRIBER, NULL, &actual));
            // The slots asked for on an acceptance; `actual` untouched on a refusal.
            EXPECT_EQ_U32(cases[c].expected == tt_RET_OK ? cases[c].slots : 0x7f7fU, actual.reliability.reorder_slots);
        }
    }
    // Only a RELIABLE Subscriber holds samples: a BEST_EFFORT one and a Publisher ignore the field, as before.
    struct tt_QosProfile request = {.reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT, .reorder_slots = 16}};
    struct tt_QosProfile actual;
    EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(&request, tt_KIND_TOPIC_SUBSCRIBER, NULL, &actual));
    EXPECT_EQ_U32(0, actual.reliability.reorder_slots);
    request.reliability.kind = tt_QOS_RELIABILITY_RELIABLE;
    EXPECT_EQ_INT(tt_RET_OK, tt_QosProfile_resolve(&request, tt_KIND_TOPIC_PUBLISHER, NULL, &actual));
    EXPECT_EQ_U32(0, actual.reliability.reorder_slots);
}

static void test_a_refused_profile_creates_nothing(void) {
    struct run_result r = sim_run(16, 0, 1, 0, tt_SECOND);
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, r.created);
    struct tt_QosProfile request = TEST_QOS_RELIABLE_READER;
    request.reliability.reorder_slots = 16;
    EXPECT_EQ_U64(0, tt_Subscriber_storage_size(&receiver_topic, &request)); // nothing to size: the profile is refused
}

// The storm test: every reorder_slots asked for, the default and NONE included, is refused or survives 20% loss.
static void test_every_accepted_profile_survives_loss(void) {
    static const uint16_t slots[] = {0, 16, 64, 128, 254, 255, 256, 512, tt_QOS_REORDER_NONE};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        struct run_result r = sim_run(slots[i], 0, SAMPLES, 200, 10 * tt_SECOND);
        print_run("storm", slots[i], tt_RELIABLE_BITMAP_WORDS, 200, &r);
        EXPECT_EQ_INT(accepted(slots[i], 0) ? tt_RET_OK : tt_RET_INVALID_ARGUMENT, r.created);
        if (r.created == tt_RET_OK) {
            EXPECT_EQ_U32(SAMPLES, r.delivered);
            EXPECT_TRUE(r.in_order);
            EXPECT_TRUE(r.data_sent < (uint64_t)3U * SAMPLES);
        }
    }
    // rmw_tickle's shape for an unbounded type until this check: 1024 slots against the widest window.
    struct run_result r = sim_run(1024, WIDE_WORDS, 3U * SAMPLES, 200, 10 * tt_SECOND);
    print_run("storm", 1024, WIDE_WORDS, 200, &r);
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, r.created);
    if (r.created == tt_RET_OK) {
        EXPECT_EQ_U32(3U * SAMPLES, r.delivered);
        EXPECT_TRUE(r.data_sent < (uint64_t)9U * SAMPLES);
    }
}

// The control: a buffer sized as before sends exactly what it sent on main 1511981c, before the check. The simulation
// is deterministic (a fixed seed and the mock clock), so equality is the claim. Re-pinned 2026-10-29 (C8, here and in
// test_several_writers_share_the_buffer()): the last match Heartbeat now asks for an ACKNACK and stays until the
// reader's acknowledgement passes the match point, which moves the first rounds of every run; every rule held in every
// row; the counts moved by -770..+35 DATA and -727..+34 ACKNACKs a run, 8 of 22 rows by more than 1% (4 writers at 5%
// loss, no reorder buffer: 15,731 -> 14,961 DATA; with one: 8,740 -> 8,469 and overflow 259 -> 0).
static void test_a_window_sized_buffer_behaves_as_before(void) {
    const struct {
        uint16_t slots;
        uint16_t words;
        uint32_t loss;
        uint32_t samples;
        uint64_t data_sent;
        uint64_t acknacks;
        uint32_t overflow;
    } before[] = {
        {256, 0, 50, SAMPLES, 2117, 118, 0},
        {256, 0, 200, SAMPLES, 2516, 501, 0},
        {255, 0, 200, SAMPLES, 2516, 501, 0},
        {512, 0, 200, SAMPLES, 2516, 501, 0},
        {tt_QOS_REORDER_NONE, 0, 50, SAMPLES, 3814, 1622, 1604},
        {tt_QOS_REORDER_NONE, 0, 200, SAMPLES, 4902, 1856, 1841},
        {4096, WIDE_WORDS, 50, 3U * SAMPLES, 6328, 325, 0},
        {4096, WIDE_WORDS, 200, 3U * SAMPLES, 7520, 997, 0},
    };
    for (size_t i = 0; i < sizeof(before) / sizeof(before[0]); i++) {
        struct run_result r =
            sim_run(before[i].slots, before[i].words, before[i].samples, before[i].loss, 10 * tt_SECOND);
        print_run("control", before[i].slots, before[i].words != 0 ? before[i].words : tt_RELIABLE_BITMAP_WORDS,
                  before[i].loss, &r);
        EXPECT_EQ_INT(tt_RET_OK, r.created);
        EXPECT_EQ_U32(before[i].samples, r.delivered);
        EXPECT_TRUE(r.in_order);
        EXPECT_EQ_U64(before[i].data_sent, r.data_sent);
        EXPECT_EQ_U64(before[i].acknacks, r.acknacks);
        EXPECT_EQ_U32(before[i].overflow, r.overflow);
    }
}

static void print_writers_run(const char* label, uint16_t slots, uint32_t loss, const struct run_result* r) {
    printf("test_reorder_window: %-10s writers=%u slots=%-5u loss=%2u%% created=%d delivered=%u/%u owed (fewest "
           "%u) in_order=%d contiguous=%d DATA sent=%lu (%.2f a sample) ACKNACKs=%lu overflow=%u held_peak=%u "
           "recovery=%.1f ms\n",
           label, r->writers, (unsigned)slots, (unsigned)(loss / 10U), (int)r->created, r->delivered, r->owed,
           r->delivered_min, (int)r->in_order, (int)r->contiguous, (unsigned long)r->data_sent,
           r->samples != 0 ? (double)r->data_sent / r->samples : 0.0, (unsigned long)r->acknacks, r->overflow,
           r->held_peak, (double)r->took_ns / 1e6);
}

// Several writers on one Subscriber share its reorder ring, each placed at its own salted offset, so their held
// samples collide however wide the ring is (2 writers at 20% loss storm with 512 and 1024 slots too; 4096 did not).
// A sample whose slot another writer's sample holds is un-received; the next arrival NACKed it alone, it came back
// ahead of the watermark's gap into the same taken slot, and so on per arrival. Before extend_gap_to_watermark(),
// slots = the window (the default), 2,000 samples a writer, 20% loss: 2 writers 555 of 4,000 delivered at 65.0 DATA a
// sample, 3 writers 790 of 6,000 at 42.5, 4 writers 692 of 8,000 at 31.6, each stopped at the 200,000-overflow cap. At
// 5% the same three were clean (1.06, 1.06 and 1.09 DATA a sample).
//
// Pre-registered, enforced below: a run storms if it does not deliver every sample owed, in order and with none
// skipped, or sends 3 DATA a sample or more. None of 1 to 4 writers at 5% or 20% loss may storm. The reader must have
// heard each writer from (nearly) its first sample, or "every sample owed" would be a vacuous count.
//
// The control, which the change cannot touch: with no reorder buffer (tt_QOS_REORDER_NONE) no slot is ever taken, so
// no collision is ever marked, and each run sends exactly what it sent before the change.
static void test_several_writers_share_the_buffer(void) {
    // The counts this change sends, pinned as the control's are: the simulation is deterministic, and a writer proxy
    // slot reused with its last writer's collision mark (the subscriber below is re-created in the same storage for
    // each run) sends a different count while still passing the rules - 13,311 DATA for 4 writers at 20%.
    const struct {
        uint32_t writers;
        uint32_t loss;
        uint64_t data_sent;
        uint64_t acknacks;
        uint32_t overflow;
    } after[] = {
        {1, 50, 2117, 118, 0}, {1, 200, 2516, 501, 0},      {2, 50, 4230, 232, 0}, {2, 200, 5803, 1207, 571},
        {3, 50, 6344, 344, 0}, {3, 200, 10268, 2953, 2113}, {4, 50, 8469, 464, 0}, {4, 200, 14025, 4426, 2988},
    };
    for (size_t i = 0; i < sizeof(after) / sizeof(after[0]); i++) {
        struct run_result r = sim_run_writers(after[i].writers, 0, 0, SAMPLES, after[i].loss, 10 * tt_SECOND);
        print_writers_run("writers", 0, after[i].loss, &r);
        EXPECT_EQ_INT(tt_RET_OK, r.created);
        EXPECT_TRUE((uint64_t)r.owed * 100U >= (uint64_t)r.samples * 99U);
        EXPECT_EQ_U32(r.owed, r.delivered);
        EXPECT_TRUE(r.in_order);
        EXPECT_TRUE(r.contiguous);
        EXPECT_TRUE(r.data_sent < (uint64_t)3U * r.samples);
        EXPECT_EQ_U64(after[i].data_sent, r.data_sent);
        EXPECT_EQ_U64(after[i].acknacks, r.acknacks);
        EXPECT_EQ_U32(after[i].overflow, r.overflow);
    }
    const struct {
        uint32_t writers;
        uint32_t loss;
        uint64_t data_sent;
        uint64_t acknacks;
        uint32_t overflow;
    } none_before[] = {
        {2, 50, 7421, 3058, 3016},   {2, 200, 9973, 3774, 3741}, {3, 50, 11450, 4889, 4830},
        {3, 200, 15090, 5762, 5715}, {4, 50, 14961, 6259, 6180}, {4, 200, 20001, 7644, 7588},
    };
    for (size_t i = 0; i < sizeof(none_before) / sizeof(none_before[0]); i++) {
        struct run_result r = sim_run_writers(none_before[i].writers, tt_QOS_REORDER_NONE, 0, SAMPLES,
                                              none_before[i].loss, 10 * tt_SECOND);
        print_writers_run("w-control", tt_QOS_REORDER_NONE, none_before[i].loss, &r);
        EXPECT_EQ_INT(tt_RET_OK, r.created);
        EXPECT_EQ_U32(r.owed, r.delivered);
        EXPECT_EQ_U64(none_before[i].data_sent, r.data_sent);
        EXPECT_EQ_U64(none_before[i].acknacks, r.acknacks);
        EXPECT_EQ_U32(none_before[i].overflow, r.overflow);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_zero_slots_resolve_to_the_window();
    test_too_few_slots_are_refused_at_the_boundary();
    test_a_refused_profile_creates_nothing();
    test_every_accepted_profile_survives_loss();
    test_a_window_sized_buffer_behaves_as_before();
    test_several_writers_share_the_buffer();
    return test_result();
}
