/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A DURABLE (TRANSIENT_LOCAL) reader that joins late gets the writer's whole retained history, then everything after.
//
// The reader's first contact with the writer is the backlog the writer unicasts on the match
// (deliver_durability_backlog(), then send_initial_heartbeat() - a match's order), and its writer proxy starts at the
// default baseline of seq_no 1. A writer that has sent more than a tracking window of datagrams before the reader came
// is far ahead of that default. Until 2026-10-15 the reader then lost the backlog's first sample, and with samples of
// more than one datagram the orphaned fragments filled its reorder ring until the stream stalled: 127 of 200
// two-datagram samples delivered from a writer at seq_no 1000, 24 of 200 ten-datagram ones, against 200 of 200 from
// seq_no 1.
//
// What counts as loss, decided before the first run and enforced in expect_case(): with K samples published before
// the reader joins and the writer keeping the last N (KEEP_LAST depth N; KEEP_ALL keeps them all), the reader must end
// up holding a contiguous, intact, in-order run of sample indices that ends at the last one published and starts no
// later than max(1, K - N + 1) - the last min(N, K) of the backlog with no gap, then every sample published after the
// join. Anything else - a missing first sample, a hole, a stall, a sample out of order or torn - is loss.
//
// Each case runs twice from the same first seq_no. The treatment joins in a match's order, backlog first, so the
// reader's first contact is the backlog's oldest DATA, and it counts only if that seq_no lay at least a tracking window
// past 1 (both read back from what ran). The control joins Heartbeat first, which gives the reader the writer's oldest
// as its baseline before any DATA (inform_subscriber_of_heartbeat()) - a path a DATA first contact never takes - and
// holds the history before the fix as after it. Over the mock HAL with test_data_frag.c's simulated
// link: what a node's HAL is handed is queued and fed to the other node's process_packet(), timers run in simulated
// time.

// Room for 10 fragments a sample, so the runs cover DATA_FRAG samples too (as test_seq_wrap.c builds).
#define tt_MAX_SAMPLE_LENGTH 16000

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
#define TOPIC_NAME "late_topic"
#define ENDPOINT_NAME "late_endpoint"
#define SIM_QUEUE 8192
#define SAMPLES 200
#define PUBLISH_SPACING (100 * tt_MICROSECOND)
#define MAX_DELIVERIES 4096

// --- the samples: each carries its index, and bytes derived from it ----------------------------------------------

static uint32_t sample_len;
static uint32_t sample_index; // the index the next published sample carries, from 1

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = (uint8_t)((i * 7U) + sample_index);
    }
    memcpy(payload, &sample_index, sizeof(sample_index)); // len >= 4
    return (int32_t)len;
}

// Every index the reader was handed, in the order it was handed them.
static uint32_t delivered[MAX_DELIVERIES];
static int delivered_count;
static uint32_t decoded_index;
static bool decoded_intact;
static bool all_intact;

static int32_t checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    memcpy(&decoded_index, payload, sizeof(decoded_index));
    decoded_intact = len == sample_len;
    for (uint32_t i = sizeof(decoded_index); i < len && decoded_intact; i++) {
        decoded_intact = payload[i] == (uint8_t)((i * 7U) + decoded_index);
    }
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)seq_no;
    (void)data;
    all_intact = all_intact && decoded_intact;
    if (delivered_count < MAX_DELIVERIES) {
        delivered[delivered_count] = decoded_index;
    }
    delivered_count++;
}

// --- the pair and the link -------------------------------------------------------------------------------------

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_TypeSupport sender_topic;
static struct tt_TypeSupport receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;

#define CACHE_DEPTH 1285 // datagrams: KEEP_ALL's 120 samples of 10 fragments fit
#define REORDER_SLOTS 257
#define REORDER_SLOT_BYTES tt_REORDER_SLOT_SIZE(1472)
static struct tt_ReliableCacheIndex cache_index[CACHE_DEPTH];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache cache;
static uint64_t reorder[REORDER_SLOTS * REORDER_SLOT_BYTES / sizeof(uint64_t)];

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

struct sim_datagram {
    uint8_t from;
    bool muted;
    uint32_t len;
    uint8_t bytes[tt_MAX_BUFFER_LENGTH];
};
static struct sim_datagram sim_queue[SIM_QUEUE];
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;
static uint32_t sim_loss_per_mille;
static uint32_t sim_rng;
static bool sim_mute; // nobody is listening yet: every datagram is lost, both ways

static void sim_capture(const void* buf, size_t len) {
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->muted = sim_mute;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
}

static bool sim_lost(void) {
    sim_rng = (sim_rng * 1103515245U) + 12345U;
    return ((sim_rng >> 16) % 1000U) < sim_loss_per_mille;
}

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        bool lost = sim_lost(); // drawn for every datagram alike
        if (lost || d->muted) {
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

static bool sim_step(void) {
    uint64_t a = UINT64_MAX;
    uint64_t b = UINT64_MAX;
    if (!sched_next_time(&sender, &a)) {
        a = UINT64_MAX;
    }
    if (!sched_next_time(&receiver, &b)) {
        b = UINT64_MAX;
    }
    uint64_t next = a < b ? a : b;
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

// The reader arrives: what the writer retains goes to it, then the initial Heartbeat - a match's order
// (register_subscriber_peer_on_publisher()) - or, for the control, the Heartbeat first, which gives the reader the
// writer's oldest as its baseline before any DATA arrives (inform_subscriber_of_heartbeat()), a path the backlog's
// first DATA does not take. Returns the oldest seq_no the writer retained at the join: the backlog's first.
static bool heartbeat_first;

static uint32_t reader_joins(void) {
    sim_mute = false;
    struct tt_Peer reader = {RECEIVER_ID, RECEIVER_IP, PORT};
    sim_acting = SENDER_ID;
    uint32_t oldest = cache.oldest_seq_no;
    // Matched as register_subscriber_peer_on_publisher() matches it: its ack entry, then its peer slot. Without the
    // peer the writer sends nothing after the join - a Publisher with no matched reader sends nothing (2026-10-15).
    EXPECT_TRUE(claim_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id) != NULL);
    EXPECT_TRUE(upsert_peer(pub.peers, RECEIVER_ID, RECEIVER_IP, PORT));
    if (heartbeat_first) {
        send_initial_heartbeat(&sender, &pub, &reader);
        sim_drain();
        sim_acting = SENDER_ID;
    }
    deliver_durability_backlog(&sender, &pub, &reader);
    if (!heartbeat_first) {
        send_initial_heartbeat(&sender, &pub, &reader);
    }
    sim_drain();
    return oldest;
}

// --- one case ----------------------------------------------------------------------------------------------------

struct late_case {
    const char* name;
    uint32_t size;       // bytes a sample: 1000 is one datagram, 2800 two, 14000 ten
    uint32_t depth;      // KEEP_LAST depth in samples; 0: KEEP_ALL
    uint32_t join_after; // samples published before the reader joins; SAMPLES: after the writer has stopped
    uint32_t loss;       // per mille of every datagram after the join, both ways
};

struct late_outcome {
    int delivered;
    uint32_t first_index, last_index;
    bool contiguous; // each index the one after the last, every sample intact
    uint32_t window; // the reader's tracking window for this writer, read back from its proxy
    uint32_t reorder_held;
    uint32_t oldest; // the oldest seq_no the writer retained when the reader joined: the backlog's first
};

static uint32_t datagrams_per_sample(uint32_t size) {
    if (size <= 1000) {
        return 1U;
    }
    return size <= 2800 ? 2U : 10U;
}

static struct late_outcome run_case(const struct late_case* c, uint32_t first, bool control, uint64_t budget) {
    test_mock_reset();
    heartbeat_first = control;
    uint32_t oldest = 0;
    sample_len = c->size;
    sample_index = 1;
    delivered_count = 0;
    all_intact = true;
    init_bare_node(&sender, SENDER_ID);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = 8;
    sender_topic.data_encode_size = sized_encode_size;
    sender_topic.data_encode = sized_encode;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&sender, &pub, &sender_topic, ENDPOINT_NAME));
    init_bare_node(&receiver, RECEIVER_ID);
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = 8;
    receiver_topic.data_decode = checking_decode;
    receiver_topic.data_free = free_nothing;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&receiver, &sub, &receiver_topic, ENDPOINT_NAME, on_data));

    memset(cache_index, 0, sizeof(cache_index));
    memset(&cache, 0, sizeof(cache));
    cache.index = cache_index;
    cache.capacity = CACHE_DEPTH;
    cache.depth = (uint16_t)(c->depth != 0 ? c->depth * datagrams_per_sample(c->size) : CACHE_DEPTH);
    cache.arena = cache_arena;
    cache.arena_size = (uint32_t)sizeof(cache_arena);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    pub.keep_all = c->depth == 0;
    pub.durable = true;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, 10 * tt_MILLISECOND));
    sub.reliable = true;
    sub.durable = true;
    memset(reorder, 0, sizeof(reorder));
    sub.reorder_storage = reorder;
    sub.reorder_slots = REORDER_SLOTS;
    sub.reorder_slot_bytes = REORDER_SLOT_BYTES;
    pub.seq_no = first - 1U; // the next sample takes seq_no `first`

    sim_head = sim_tail = 0;
    sim_loss_per_mille = 0;
    sim_rng = 1;
    sim_mute = c->join_after != 0;
    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
    for (uint32_t i = 0; i < SAMPLES; i++) {
        if (i == c->join_after && i != 0) {
            sim_loss_per_mille = c->loss;
            oldest = reader_joins();
        }
        sim_run_due(&sender);
        sim_run_due(&receiver);
        sim_acting = SENDER_ID;
        tt_ret_t result = tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len);
        uint64_t blocked_since = test_mock_now;
        while (result == tt_RET_WOULD_BLOCK && test_mock_now - blocked_since < budget) {
            sim_drain();
            if (!sim_step()) {
                break;
            }
            sim_acting = SENDER_ID;
            result = tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len);
        }
        EXPECT_EQ_INT(tt_RET_OK, result);
        sample_index++;
        sim_drain();
        test_mock_now += PUBLISH_SPACING;
    }
    if (c->join_after == SAMPLES) {
        sim_loss_per_mille = c->loss;
        oldest = reader_joins();
    }
    // Run until the last sample is in, or the budget is spent.
    uint64_t published = test_mock_now;
    while ((delivered_count == 0 || delivered[(delivered_count - 1) % MAX_DELIVERIES] != SAMPLES) &&
           test_mock_now - published < budget) {
        if (!sim_step()) {
            break;
        }
    }
    test_mock_send_hook = NULL;

    struct late_outcome out;
    memset(&out, 0, sizeof(out));
    out.delivered = delivered_count;
    out.contiguous = all_intact && delivered_count <= MAX_DELIVERIES;
    for (int i = 1; i < delivered_count && i < MAX_DELIVERIES; i++) {
        out.contiguous = out.contiguous && delivered[i] == delivered[i - 1] + 1U;
    }
    if (delivered_count > 0) {
        out.first_index = delivered[0];
        out.last_index = delivered[(delivered_count < MAX_DELIVERIES ? delivered_count : MAX_DELIVERIES) - 1];
    }
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    out.window = proxy != NULL ? proxy_window_bits(proxy) : 0;
    out.reorder_held = sub.reorder_held;
    out.oldest = oldest;
    return out;
}

// The pre-registered reading (see the top of this file).
static bool holds_the_history(const struct late_case* c, const struct late_outcome* o) {
    uint32_t backlog = c->join_after;
    uint32_t kept = (c->depth != 0 && c->depth < backlog) ? c->depth : backlog;
    uint32_t latest_first = backlog > kept ? backlog - kept + 1U : 1U;
    return o->delivered > 0 && o->contiguous && o->last_index == SAMPLES && o->first_index <= latest_first;
}

static void report(const char* arm, const struct late_case* c, uint32_t first, const struct late_outcome* o,
                   bool holds) {
    printf("test_durable_late_joiner: %-9s %-41s from %6u, backlog from %6u (window %4u): delivered %3d, indices "
           "%u..%u, contiguous %d, held %u - %s\n",
           arm, c->name, first, o->oldest, o->window, o->delivered, o->first_index, o->last_index, o->contiguous,
           o->reorder_held, holds ? "holds the history" : "LOSS");
}

// The control joins Heartbeat first, the treatment in a match's order; both must hold the history and leave nothing
// stranded in the reorder ring. The treatment must have met a backlog starting at least a window past the default
// baseline of 1 - read back from what ran - or it tested nothing.
static void expect_case(const struct late_case* c, uint32_t first) {
    struct late_outcome control = run_case(c, first, true, 5 * tt_SECOND);
    bool control_holds = holds_the_history(c, &control);
    report("control", c, first, &control, control_holds);
    struct late_outcome late = run_case(c, first, false, 5 * tt_SECOND);
    bool late_holds = holds_the_history(c, &late);
    report("treatment", c, first, &late, late_holds);
    EXPECT_TRUE(control_holds);
    EXPECT_EQ_U32(0, control.reorder_held);
    EXPECT_TRUE(late_holds);
    EXPECT_EQ_U32(0, late.reorder_held);
    EXPECT_TRUE(late.window != 0 && late.oldest - 1U >= late.window);
}

static void test_a_late_durable_reader_gets_the_whole_history(void) {
    // Sizes of one, two and ten datagrams; KEEP_ALL and KEEP_LAST at depths below and above the backlog; the reader
    // joining after one sample, mid-stream and after the writer has stopped. Two first seq_no: past one window, and
    // far past it.
    const struct late_case cases[] = {
        {"1 datagram, KEEP_ALL, join at 10", 1000, 0, 10, 0},
        {"1 datagram, KEEP_LAST 8, join at 120", 1000, 8, 120, 0},
        {"1 datagram, KEEP_ALL, join after the end", 1000, 0, SAMPLES, 0},
        {"2 datagrams, KEEP_ALL, join at 1", 2800, 0, 1, 0},
        {"2 datagrams, KEEP_ALL, join at 10", 2800, 0, 10, 0},
        {"2 datagrams, KEEP_LAST 8, join at 120", 2800, 8, 120, 0},
        {"2 datagrams, KEEP_LAST 300, join at 120", 2800, 300, 120, 0},
        {"2 datagrams, KEEP_LAST 8, after the end", 2800, 8, SAMPLES, 0},
        {"10 datagrams, KEEP_ALL, join at 10", 14000, 0, 10, 0},
        {"10 datagrams, KEEP_LAST 4, join at 120", 14000, 4, 120, 0},
        {"10 datagrams, KEEP_LAST 100, join at 120", 14000, 100, 120, 0},
        {"10 datagrams, KEEP_LAST 4, after the end", 14000, 4, SAMPLES, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        expect_case(&cases[i], 1000);
        expect_case(&cases[i], 100000);
    }
}

int main(void) {
    tt_log_set_level(TT_LOG_ERROR); // the INFO and WARNING lines of every pair would be noise
    test_a_late_durable_reader_gets_the_whole_history();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_durable_late_joiner: all tests passed\n");
    return 0;
}
