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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define RECEIVER_IP 0x0a000002
#define PORT 8282
#define ENDPOINT_NAME "reorder_window"
#define SAMPLE_LEN 64U

static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)SAMPLE_LEN;
}

static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0x5a, len);
    return (int32_t)len;
}

static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)len;
    (void)native;
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_TypeSupport sender_topic;
static struct tt_TypeSupport receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;

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
    uint32_t len;
    uint8_t bytes[tt_MAX_BUFFER_LENGTH];
};
static struct sim_datagram sim_queue[SIM_QUEUE];
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;
static uint32_t sim_loss_per_mille;
static uint32_t sim_rng;
static uint64_t sim_sent[3];
static uint64_t sim_dropped[3];
static uint64_t sim_data_sent;     // DATA submessages the sender put on the wire, originals and resends
static uint64_t sim_acknacks_sent; // ACKNACK submessages the receiver put on the wire
static bool sim_overran;

static void sim_count_submessages(const void* buf, size_t len) {
    uint8_t classic[tt_MAX_BUFFER_LENGTH + sizeof(struct tt_Header)];
    size_t n = test_classic_form(buf, len, classic);
    size_t at = sizeof(struct tt_Header);
    while (at + sizeof(struct tt_SubmessageHeader) <= n) {
        const struct tt_SubmessageHeader* sh = (const struct tt_SubmessageHeader*)(classic + at);
        if (sh->type == tt_SUBMESSAGE_TYPE_DATA && sim_acting == SENDER_ID) {
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
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
    sim_sent[sim_acting]++;
    sim_count_submessages(buf, len);
}

static bool sim_lost(void) {
    sim_rng = (sim_rng * 1103515245U) + 12345U;
    return ((sim_rng >> 16) % 1000U) < sim_loss_per_mille;
}

// A run that has overflowed this often is a storm already. A zero-latency link ping-pongs one for minutes inside a
// single drain (each un-received retransmission NACKed at once by the next arrival), so the drain stops there too.
#define SIM_OVERFLOW_CAP 200000U

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        if (sub.reorder_overflow >= SIM_OVERFLOW_CAP) {
            sim_head = sim_tail;
            return;
        }
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        if (sim_lost()) {
            sim_dropped[d->from]++;
            continue;
        }
        struct tt_Context* to = d->from == SENDER_ID ? &receiver : &sender;
        sim_acting = to->id;
        process_packet(to, d->bytes, 0, d->len, d->from == SENDER_ID ? SENDER_IP : RECEIVER_IP, PORT, tt_TRANSPORT_UDP);
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
    uint64_t a = UINT64_MAX;
    uint64_t b = UINT64_MAX;
    if (!sched_next_time(&sender, &a)) {
        a = UINT64_MAX;
    }
    if (!sched_next_time(&receiver, &b)) {
        b = UINT64_MAX;
    }
    return a < b ? a : b;
}

static bool sim_step(void) {
    uint64_t next = sim_next_due();
    if (next == UINT64_MAX) {
        return false;
    }
    if (next > test_mock_now) {
        test_mock_now = next;
    }
    sim_run_due(&sender);
    sim_run_due(&receiver);
    return true;
}

static uint32_t delivered;
static uint16_t last_seq;
static bool in_order;
static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)data;
    in_order = in_order && (delivered == 0 || (int16_t)(seq_no - last_seq) > 0);
    last_seq = seq_no;
    delivered++;
}

#define SIM_CACHE_DEPTH 4096
static struct tt_ReliableCacheIndex sim_cache_index[SIM_CACHE_DEPTH];
static uint8_t sim_cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(SIM_CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(SAMPLE_LEN))];
static struct tt_ReliableCache sim_cache;
static uint64_t sub_storage[(size_t)2U * 1024U * 1024U / sizeof(uint64_t)];

struct run_result {
    tt_ret_t created;
    uint32_t samples;
    uint32_t delivered;
    bool in_order;
    uint64_t data_sent;
    uint64_t acknacks;
    uint64_t sender_datagrams;
    uint32_t overflow;
    uint64_t took_ns;
};

// RELIABLE KEEP_ALL writer (its cache the sim's own SIM_CACHE_DEPTH entries), RELIABLE reader with `reorder_slots`
// and `tracking_words`, `samples` published 100 us apart through a link losing loss_per_mille both ways, then the
// protocol runs until everything is delivered or `budget` has passed.
static struct run_result sim_run(uint16_t reorder_slots, uint16_t tracking_words, uint32_t samples,
                                 uint32_t loss_per_mille, uint64_t budget) {
    struct run_result r;
    memset(&r, 0, sizeof(r));
    r.samples = samples;
    static uint64_t pub_storage[(size_t)64U * 1024U / sizeof(uint64_t)];
    const struct tt_QosProfile pub_qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL, .depth = 1},
                                          .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    struct tt_QosProfile sub_qos = TEST_QOS_RELIABLE_READER;
    sub_qos.reliability.reorder_slots = reorder_slots;
    sub_qos.reliability.tracking_words = tracking_words;

    test_mock_reset();
    init_bare_node(&sender, SENDER_ID);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = ENDPOINT_NAME;
    sender_topic.data_size = 8;
    sender_topic.data_encode_size = sample_encode_size;
    sender_topic.data_encode = sample_encode;
    size_t pub_size = tt_Publisher_storage_size(&sender_topic, &pub_qos);
    EXPECT_TRUE(pub_size <= sizeof(pub_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(
                                 tt_Context_default_node(&sender), &pub, &sender_topic, ENDPOINT_NAME, &pub_qos,
                                 &(struct tt_PublisherOptions) {.heartbeat_period_ns = 10 * tt_MILLISECOND},
                                 pub_storage, pub_size));
    match_by_broadcast(&pub);

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
    memset(sim_sent, 0, sizeof(sim_sent));
    memset(sim_dropped, 0, sizeof(sim_dropped));
    sim_data_sent = 0;
    sim_acknacks_sent = 0;
    sim_overran = false;
    delivered = 0;
    last_seq = 0;
    in_order = true;

    memset(sim_cache_index, 0, sizeof(sim_cache_index));
    memset(&sim_cache, 0, sizeof(sim_cache));
    sim_cache.index = sim_cache_index;
    sim_cache.capacity = SIM_CACHE_DEPTH;
    sim_cache.depth = SIM_CACHE_DEPTH;
    sim_cache.arena = sim_cache_arena;
    sim_cache.arena_size = (uint32_t)sizeof(sim_cache_arena);
    pub.reliable_cache = &sim_cache;

    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
    uint32_t dummy = 0;
    for (uint32_t i = 0; i < samples; i++) {
        sim_run_due(&sender);
        sim_run_due(&receiver);
        sim_acting = SENDER_ID;
        tt_ret_t result = tt_Publisher_publish(&pub, (struct tt_Data*)&dummy);
        uint64_t blocked_since = test_mock_now;
        while (result == tt_RET_WOULD_BLOCK && test_mock_now - blocked_since < budget &&
               sub.reorder_overflow < SIM_OVERFLOW_CAP) {
            sim_drain();
            if (!sim_step()) {
                break;
            }
            sim_acting = SENDER_ID;
            result = tt_Publisher_publish(&pub, (struct tt_Data*)&dummy);
        }
        if (result != tt_RET_OK) {
            break; // blocked past the budget, or the run stopped at the overflow cap: what was delivered says which
        }
        sim_drain();
        test_mock_now += 100 * tt_MICROSECOND;
    }
    uint64_t published = test_mock_now;
    while (delivered < samples && test_mock_now - published < budget && sub.reorder_overflow < SIM_OVERFLOW_CAP) {
        if (!sim_step()) {
            break;
        }
    }
    test_mock_send_hook = NULL;
    EXPECT_TRUE(!sim_overran);
    r.delivered = delivered;
    r.in_order = in_order;
    r.data_sent = sim_data_sent;
    r.acknacks = sim_acknacks_sent;
    r.sender_datagrams = sim_sent[SENDER_ID];
    r.overflow = sub.reorder_overflow;
    r.took_ns = test_mock_now - published;
    return r;
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
// is deterministic (a fixed seed and the mock clock), so equality is the claim.
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
        {256, 0, 200, SAMPLES, 2509, 552, 0},
        {255, 0, 200, SAMPLES, 2509, 552, 0},
        {512, 0, 200, SAMPLES, 2509, 552, 0},
        {tt_QOS_REORDER_NONE, 0, 50, SAMPLES, 3814, 1622, 1604},
        {tt_QOS_REORDER_NONE, 0, 200, SAMPLES, 4902, 1856, 1841},
        {4096, WIDE_WORDS, 50, 3U * SAMPLES, 6328, 325, 0},
        {4096, WIDE_WORDS, 200, 3U * SAMPLES, 7522, 994, 0},
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

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_zero_slots_resolve_to_the_window();
    test_too_few_slots_are_refused_at_the_boundary();
    test_a_refused_profile_creates_nothing();
    test_every_accepted_profile_survives_loss();
    test_a_window_sized_buffer_behaves_as_before();
    return test_result();
}
