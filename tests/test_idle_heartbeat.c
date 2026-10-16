/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The idle HEARTBEAT (config.h's tt_IDLE_HEARTBEAT_PERIOD, DESIGN.md section 6; the user's decision 2026-10-16): a
// RELIABLE writer with something unacknowledged sends a non-FINAL HEARTBEAT once no HEARTBEAT has gone for a period P,
// timed from the last one of any kind. tests/mutants_idle_heartbeat.py breaks each condition and requires a failure
// here.
//
// Two contexts in one process over the mock HAL, as test_large_sample.c: every datagram either one hands its HAL is
// queued and fed to the other's process_packet(), on the mock clock, with whatever loss a test asks for. Each case has
// its control - the same run with the idle HEARTBEAT off, or without the HEARTBEAT that should push it back - so a
// case that passes for a reason other than the mechanism shows up as a control that passes too.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#define TOPIC_NAME "idle_topic"
#define ENDPOINT_NAME "idle_endpoint"
#define PERIOD (100U * tt_MILLISECOND) // the period these cases are written against (set on the Publisher)
#define SLACK (20U * tt_MILLISECOND)   // a round trip and the reader's own retry interval, with room

// --- the topic: a uint32_t per sample
// ---------------------------------------------------------------------------------

static int32_t u32_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}

static int32_t u32_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static int32_t u32_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

#define MAX_VALUES 4096U
static uint64_t delivered_at[MAX_VALUES + 1U]; // by value: 0 = not delivered
static int delivered_count;

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)seq_no;
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    if (value <= MAX_VALUES && delivered_at[value] == 0) {
        delivered_at[value] = test_mock_now;
    }
    delivered_count++;
}

// --- the two contexts
// -------------------------------------------------------------------------------------------------

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_Topic sender_topic;
static struct tt_Topic receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;

#define CACHE_DEPTH 256
static struct tt_ReliableCacheIndex cache_index[CACHE_DEPTH];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(64))];
static struct tt_ReliableCache cache;
#define WORDS 16
static uint64_t tracking[tt_MAX_PEER_COUNT * WORDS];
static bool pair_up;

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

static void teardown_pair(void) {
    if (!pair_up) {
        return;
    }
    pair_up = false;
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&sender));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&receiver));
}

// A RELIABLE KEEP_LAST writer and a RELIABLE reader, the reader matched on the writer as its announce would match it.
// reliable_reader false matches it as a BEST_EFFORT reader's announce does (register_subscriber_peer_on_publisher()).
static void init_pair(bool reliable_reader) {
    teardown_pair();
    test_mock_reset();
    memset(delivered_at, 0, sizeof(delivered_at));
    delivered_count = 0;

    init_bare_node(&sender, SENDER_ID);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = sizeof(uint32_t);
    sender_topic.data_encode_size = u32_encode_size;
    sender_topic.data_encode = u32_encode;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&sender, &pub, &sender_topic, ENDPOINT_NAME));
    EXPECT_EQ_U64(tt_IDLE_HEARTBEAT_PERIOD, pub.idle_heartbeat_ns); // on by default
    EXPECT_TRUE(tt_IDLE_HEARTBEAT_PERIOD != 0);
    pub.idle_heartbeat_ns = PERIOD; // the cases' own period, whatever the default

    init_bare_node(&receiver, RECEIVER_ID);
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = sizeof(uint32_t);
    receiver_topic.data_decode = u32_decode;
    receiver_topic.data_free = free_nothing;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&receiver, &sub, &receiver_topic, ENDPOINT_NAME, on_data));

    memset(cache_index, 0, sizeof(cache_index));
    memset(&cache, 0, sizeof(cache));
    cache.index = cache_index;
    cache.capacity = CACHE_DEPTH;
    cache.depth = CACHE_DEPTH;
    cache.arena = cache_arena;
    cache.arena_size = (uint32_t)sizeof(cache_arena);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    sub.reliable = reliable_reader;
    memset(tracking, 0, sizeof(tracking));
    sub.tracking_bitmaps = tracking;
    sub.tracking_words = WORDS;
    struct tt_PeerAck* ack = claim_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id);
    EXPECT_TRUE(ack != NULL);
    if (ack != NULL) {
        ack->tracking_words = WORDS;
        ack->reliable_reader = reliable_reader;
    }
    (void)upsert_peer(pub.peers, RECEIVER_ID, RECEIVER_IP, PORT);
    pair_up = true;
}

// --- the link
// ---------------------------------------------------------------------------------------------------------

#define SIM_QUEUE 65536U
#define SIM_DATAGRAM_MAX 1600U

struct sim_datagram {
    uint8_t from;
    uint32_t len;
    uint8_t bytes[SIM_DATAGRAM_MAX];
};
static struct sim_datagram* sim_queue;
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;

// What the sender put on the wire for this Publisher, by datagram.
struct sent_shape {
    bool data;        // a DATA of this endpoint
    bool heartbeat;   // a HEARTBEAT of this endpoint
    bool final;       // ... with tt_HEARTBEAT_FLAG_FINAL
    uint32_t value;   // the DATA's sample, when it has one
    uint32_t hb_last; // the HEARTBEAT's last_seq_no
};

static struct sent_shape shape_of(const uint8_t* bytes, uint32_t len) {
    struct sent_shape shape = {false, false, false, 0, 0};
    static uint8_t classic[SIM_DATAGRAM_MAX + sizeof(struct tt_Header)];
    size_t classic_len = test_classic_form(bytes, len, classic);
    uint32_t endpoint = ((struct tt_Endpoint*)&pub)->id;
    size_t at = sizeof(struct tt_Header);
    while (at + sizeof(struct tt_SubmessageHeader) <= classic_len) {
        struct tt_SubmessageHeader submessage;
        memcpy(&submessage, classic + at, sizeof(submessage));
        const uint8_t* body = classic + at + sizeof(submessage);
        if (submessage.type == tt_SUBMESSAGE_TYPE_DATA) {
            struct tt_DataHeader data;
            memcpy(&data, body, sizeof(data));
            if (data.endpoint_id == endpoint) {
                shape.data = true;
                memcpy(&shape.value, body + sizeof(data), sizeof(uint32_t));
            }
        } else if (submessage.type == tt_SUBMESSAGE_TYPE_HEARTBEAT) {
            struct tt_HeartbeatHeader heartbeat;
            memcpy(&heartbeat, body, sizeof(heartbeat));
            if (heartbeat.endpoint_id == endpoint) {
                shape.heartbeat = true;
                shape.final = (heartbeat.flags & tt_HEARTBEAT_FLAG_FINAL) != 0;
                shape.hb_last = heartbeat.last_seq_no;
            }
        }
        if (submessage.length < sizeof(submessage)) {
            break;
        }
        at += submessage.length;
    }
    return shape;
}

// Counters over the sender's datagrams of this Publisher, from the last sim_count_reset().
static uint32_t lone_heartbeats;   // a HEARTBEAT on a datagram of its own: what the idle HEARTBEAT costs in datagrams
static uint32_t lone_nonfinal;     // ... asking for an answer
static uint32_t riding_heartbeats; // a HEARTBEAT on a DATA's datagram
static uint32_t data_datagrams;
#define MAX_TIMES 64U
static uint64_t lone_at[MAX_TIMES]; // when each lone HEARTBEAT was sent

// Loss: the DATA of these sample values, once each; every lone HEARTBEAT until `drop_lone_heartbeats` is spent; every
// datagram from the reader while `drop_reader` is set.
#define MAX_DROPS 8U
static uint32_t drop_values[MAX_DROPS];
static uint32_t drop_count;
static uint32_t drop_lone_heartbeats;
static bool drop_reader;

static void sim_count_reset(void) {
    lone_heartbeats = 0;
    lone_nonfinal = 0;
    riding_heartbeats = 0;
    data_datagrams = 0;
}

static void sim_capture(const void* buf, size_t len) {
    if (len > SIM_DATAGRAM_MAX || len == 0) {
        return;
    }
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
    if (sim_acting != SENDER_ID) {
        return;
    }
    struct sent_shape shape = shape_of(d->bytes, d->len);
    if (shape.data) {
        data_datagrams++;
        riding_heartbeats += shape.heartbeat ? 1U : 0U;
    } else if (shape.heartbeat) {
        if (lone_heartbeats < MAX_TIMES) {
            lone_at[lone_heartbeats] = test_mock_now;
        }
        lone_heartbeats++;
        lone_nonfinal += shape.final ? 0U : 1U;
    }
}

// Whether a datagram from the reader carries an ACKNACK. Nothing else of the two contexts' traffic is delivered: their
// announces would run discovery and context-id claiming between two bare contexts that share a mock address, which is
// not what is tested here - the reader is matched on the writer by init_pair(), and its writer proxy is made by the
// first DATA, as it is when the announce is slower than the data.
static bool carries_acknack(const uint8_t* bytes, uint32_t len) {
    static uint8_t classic[SIM_DATAGRAM_MAX + sizeof(struct tt_Header)];
    size_t classic_len = test_classic_form(bytes, len, classic);
    size_t at = sizeof(struct tt_Header);
    while (at + sizeof(struct tt_SubmessageHeader) <= classic_len) {
        struct tt_SubmessageHeader submessage;
        memcpy(&submessage, classic + at, sizeof(submessage));
        if (submessage.type == tt_SUBMESSAGE_TYPE_ACKNACK) {
            return true;
        }
        if (submessage.length < sizeof(submessage)) {
            break;
        }
        at += submessage.length;
    }
    return false;
}

static bool sim_dropped(const struct sim_datagram* d) {
    if (d->from != SENDER_ID) {
        return drop_reader || !carries_acknack(d->bytes, d->len);
    }
    struct sent_shape shape = shape_of(d->bytes, d->len);
    if (!shape.data && !shape.heartbeat) {
        return true; // not this Publisher's: see carries_acknack()
    }
    if (shape.data) {
        for (uint32_t i = 0; i < drop_count; i++) {
            if (drop_values[i] == shape.value) {
                drop_values[i] = UINT32_MAX; // once: its repair goes through
                return true;
            }
        }
        return false;
    }
    if (shape.heartbeat && drop_lone_heartbeats != 0) {
        drop_lone_heartbeats--;
        return true;
    }
    return false;
}

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        if (sim_dropped(d)) {
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

// Runs both contexts' timers, and what they send, up to `until`, and leaves the clock there.
static void sim_run_until(uint64_t until) {
    for (;;) {
        uint64_t next = sim_next_due();
        if (next > until) {
            break;
        }
        if (next > test_mock_now) {
            test_mock_now = next;
        }
        sim_run_due(&sender);
        sim_run_due(&receiver);
    }
    test_mock_now = until;
    sim_run_due(&sender);
    sim_run_due(&receiver);
}

static void sim_start(void) {
    if (sim_queue == NULL) {
        sim_queue = (struct sim_datagram*)calloc(SIM_QUEUE, sizeof(struct sim_datagram));
    }
    sim_head = sim_tail = 0;
    drop_count = 0;
    drop_lone_heartbeats = 0;
    drop_reader = false;
    sim_count_reset();
    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
    sim_run_until(test_mock_now + tt_MILLISECOND); // whatever creation scheduled
    sim_count_reset();
}

static void sim_publish(uint32_t value) {
    sim_run_until(test_mock_now);
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    sim_drain();
}

// `count` samples, values first..first+count-1, one every `gap`; the clock is left at the last one.
static void sim_burst(uint32_t first, uint32_t count, uint64_t gap) {
    for (uint32_t i = 0; i < count; i++) {
        if (i != 0) {
            sim_run_until(test_mock_now + gap);
        }
        sim_publish(first + i);
    }
}

static void drop_sample(uint32_t value) {
    if (drop_count < MAX_DROPS) {
        drop_values[drop_count++] = value;
    }
}

// How many idle HEARTBEAT entries the scheduler holds for `param`.
static int scheduled_for(const struct tt_Context* node, const void* param) {
    int count = 0;
    for (int32_t i = 0; i < node->scheduler_tail; i++) {
        if (node->scheduler[i].function == idle_heartbeat_fire && node->scheduler[i].param == param) {
            count++;
        }
    }
    return count;
}

// Whether the sender's scheduler is still a heap (each entry no earlier than its parent): the idle HEARTBEAT's entry is
// moved in place (sched_postpone_locked()), and one left out of order would delay every timer behind it.
static bool heap_ordered(const struct tt_Context* node) {
    for (int32_t i = 1; i < node->scheduler_tail; i++) {
        if (node->scheduler[(i - 1) / 2].time > node->scheduler[i].time) {
            return false;
        }
    }
    return true;
}

// --- the tail -------------------------------------------------------------------------------------------------------

// A burst of 10 whose last sample is lost: delivered within P of it (idle_heartbeat_fire()'s bound), by one lone
// non-FINAL HEARTBEAT. The control, the idle HEARTBEAT off, is the behaviour before it: nothing reveals the tail, and a
// second later it is still missing.
static uint64_t burst_with_lost_tail(uint64_t period, uint32_t piggyback_every) {
    init_pair(true);
    pub.idle_heartbeat_ns = period;
    pub.heartbeat_piggyback_every = piggyback_every;
    sim_start();
    drop_sample(10);
    sim_burst(1, 10, tt_MILLISECOND);
    uint64_t last_sent = test_mock_now;
    sim_run_until(last_sent + tt_SECOND);
    return delivered_at[10] != 0 ? delivered_at[10] - last_sent : UINT64_MAX;
}

static void test_a_lost_tail_is_delivered_within_the_period(void) {
    uint32_t piggyback[] = {0, 64};
    for (size_t i = 0; i < sizeof(piggyback) / sizeof(piggyback[0]); i++) {
        uint64_t after = burst_with_lost_tail(PERIOD, piggyback[i]);
        EXPECT_TRUE(after <= PERIOD + SLACK);
        EXPECT_EQ_INT(10, delivered_count);
        EXPECT_TRUE(lone_nonfinal >= 1);
        printf("test_idle_heartbeat: piggyback %u: lost tail delivered %llu us after it was sent, %u lone HEARTBEATs\n",
               piggyback[i], (unsigned long long)(after / tt_MICROSECOND), lone_heartbeats);

        uint64_t control = burst_with_lost_tail(0, piggyback[i]); // the control: off, as before
        EXPECT_EQ_U64(UINT64_MAX, control);
        EXPECT_EQ_INT(9, delivered_count);
        EXPECT_EQ_U32(0, lone_heartbeats);
    }
}

// A burst of one: the lone sample's HEARTBEAT comes one period after it (idle_heartbeat_on_publish()).
static void test_a_lone_lost_sample_waits_one_period(void) {
    init_pair(true);
    sim_start();
    sim_burst(1, 3, tt_MILLISECOND); // delivered; then quiet until everything is acknowledged
    sim_run_until(test_mock_now + tt_SECOND);
    EXPECT_EQ_INT(3, delivered_count);
    drop_sample(4);
    sim_publish(4);
    uint64_t sent = test_mock_now;
    sim_run_until(sent + tt_SECOND);
    EXPECT_TRUE(delivered_at[4] != 0);
    EXPECT_TRUE(delivered_at[4] - sent >= PERIOD);
    EXPECT_TRUE(delivered_at[4] - sent <= PERIOD + SLACK);
}

// The idle HEARTBEAT itself lost with the tail: another one P after it, while the tail stays unacknowledged.
static void test_a_lost_idle_heartbeat_is_sent_again(void) {
    init_pair(true);
    sim_start();
    drop_sample(5);
    drop_lone_heartbeats = 1;
    sim_burst(1, 5, tt_MILLISECOND);
    uint64_t last_sent = test_mock_now;
    sim_run_until(last_sent + tt_SECOND);
    EXPECT_TRUE(delivered_at[5] != 0);
    EXPECT_TRUE(lone_heartbeats >= 2);
    if (lone_heartbeats >= 2) {
        EXPECT_EQ_U64(PERIOD, lone_at[1] - lone_at[0]); // P after the lost one
    }
    EXPECT_TRUE(delivered_at[5] - last_sent <= 2U * PERIOD + SLACK);
}

// --- the cost
// ---------------------------------------------------------------------------------------------------------

// A steady stream - 1000 samples/s for 3 s, 30 periods - sends no HEARTBEAT datagram of its own, with or without the
// rmw default piggyback: each sample pushes it back, and it never runs. When it stops, exactly one lone HEARTBEAT, then
// nothing for as long as everything stays acknowledged. One scheduler entry for it throughout.
static void test_a_steady_stream_sends_no_heartbeat_datagram(void) {
    uint32_t piggyback[] = {0, 64};
    for (size_t i = 0; i < sizeof(piggyback) / sizeof(piggyback[0]); i++) {
        init_pair(true);
        pub.heartbeat_piggyback_every = piggyback[i];
        sim_start();
        int most = 0;
        bool ordered = true;
        for (uint32_t v = 1; v <= 3000U; v++) {
            if (v != 1) {
                sim_run_until(test_mock_now + tt_MILLISECOND);
            }
            sim_publish(v);
            int entries = scheduled_for(&sender, &pub);
            most = entries > most ? entries : most;
            ordered = ordered && heap_ordered(&sender);
        }
        EXPECT_EQ_U32(0, lone_heartbeats);
        EXPECT_EQ_U32(3000, data_datagrams);
        EXPECT_EQ_INT(1, most); // one entry, moved in place - never one per sample
        EXPECT_TRUE(ordered);
        uint32_t riding = riding_heartbeats;
        EXPECT_EQ_U32(piggyback[i] == 0 ? 0U : 3000U / piggyback[i], riding); // the piggyback's own, and no other
        sim_run_until(test_mock_now + tt_SECOND);
        EXPECT_EQ_U32(1, lone_heartbeats);
        EXPECT_EQ_U32(1, lone_nonfinal);
        EXPECT_EQ_INT(3000, delivered_count);
        sim_run_until(test_mock_now + 2U * tt_SECOND);
        EXPECT_EQ_U32(1, lone_heartbeats); // everything acknowledged: silence
        EXPECT_TRUE(!pub.idle_heartbeat_armed);
        printf("test_idle_heartbeat: steady stream, piggyback %u: %u lone HEARTBEATs, %u riding\n", piggyback[i],
               lone_heartbeats, riding);
    }
}

// A slow stream, one sample every 2 P: each sample is a burst of one, so each pays one lone HEARTBEAT - the cost the
// period is chosen against (config.h). The count is pinned so a change to it is a decision, not a drift.
static void test_a_slow_stream_pays_one_heartbeat_per_sample(void) {
    init_pair(true);
    sim_start();
    for (uint32_t v = 1; v <= 10U; v++) {
        sim_publish(v);
        sim_run_until(test_mock_now + 2U * PERIOD);
    }
    EXPECT_EQ_U32(10, lone_heartbeats);
    EXPECT_EQ_INT(10, delivered_count);
}

// The push-back's threshold: a stream with samples 0.45 P apart moves the entry on before it is due every time (a
// publish lands in each half period), so it never runs; at 1.2 P apart every sample is a burst of one and gets its own.
static void test_a_stream_within_half_a_period_never_wakes_it(void) {
    init_pair(true);
    sim_start();
    for (uint32_t v = 1; v <= 40U; v++) {
        sim_publish(v);
        sim_run_until(test_mock_now + (PERIOD * 45U / 100U));
    }
    EXPECT_EQ_U32(0, lone_heartbeats);
    init_pair(true);
    sim_start();
    for (uint32_t v = 1; v <= 10U; v++) {
        sim_publish(v);
        sim_run_until(test_mock_now + (PERIOD * 12U / 10U));
    }
    EXPECT_EQ_U32(10, lone_heartbeats);
}

// --- when it says nothing
// ---------------------------------------------------------------------------------------------

// Everything acknowledged: none. The reader answers the first one, and after that the writer is quiet however long.
static void test_an_acknowledged_writer_sends_none(void) {
    init_pair(true);
    sim_start();
    sim_burst(1, 5, tt_MILLISECOND);
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_request_ack(&pub)); // acknowledged now, before the idle HEARTBEAT is due
    sim_drain();
    sim_count_reset();
    sim_run_until(test_mock_now + 5U * tt_SECOND);
    EXPECT_EQ_U32(0, lone_heartbeats);
    EXPECT_EQ_INT(5, delivered_count);
}

// No matched reader in peers[] - an ack entry kept across an announce's forget-and-re-add, with the peer slot gone -
// and a BEST_EFFORT reader of a RELIABLE writer, which never acknowledges: neither is waited on.
static void test_nobody_to_ask_sends_none(void) {
    // Published while matched, unacknowledged (the reader's answers lost), then the peer slot goes before the idle
    // HEARTBEAT is due: it finds nobody to send to, and sends nothing - not a broadcast.
    init_pair(true);
    sim_start();
    drop_reader = true;
    sim_burst(1, 5, tt_MILLISECOND);
    EXPECT_TRUE(pub.idle_heartbeat_armed);
    forget_peer(pub.peers, RECEIVER_ID); // the ack entry stays (Phase 3 prerequisite (c)'s preserve_ack case)
    sim_run_until(test_mock_now + tt_SECOND);
    EXPECT_EQ_U32(0, lone_heartbeats);
    EXPECT_TRUE(!pub.idle_heartbeat_armed);

    init_pair(false);
    sim_start();
    sim_burst(1, 5, tt_MILLISECOND);
    sim_run_until(test_mock_now + tt_SECOND);
    EXPECT_EQ_U32(0, lone_heartbeats);
    EXPECT_EQ_INT(5, delivered_count);
    // The control: the same reader counted as RELIABLE is asked, and asked again while it does not answer.
    init_pair(true);
    sim_start();
    drop_reader = true;
    sim_burst(1, 5, tt_MILLISECOND);
    sim_run_until(test_mock_now + tt_SECOND);
    EXPECT_TRUE(lone_heartbeats >= 8U);
}

// --- what pushes it back
// ----------------------------------------------------------------------------------------------

// Any HEARTBEAT to every matched reader restarts the clock: a solicitation 0.6 P after the sample moves the idle one
// to 0.6 P + P, and while the reader's answers are lost the next ones follow at P after each. The control has no
// solicitation, and its first idle HEARTBEAT is at P.
static void test_any_heartbeat_pushes_the_next_back(void) {
    for (int arm = 0; arm < 2; arm++) {
        bool solicit = arm == 0;
        init_pair(true);
        sim_start();
        drop_reader = true; // nothing is ever acknowledged
        sim_publish(1);
        uint64_t sent = test_mock_now;
        if (solicit) {
            sim_run_until(sent + (PERIOD * 6U / 10U));
            sim_acting = SENDER_ID;
            EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_request_ack(&pub));
            sim_drain();
        }
        sim_run_until(sent + 4U * PERIOD);
        uint64_t first_idle = solicit ? sent + (PERIOD * 6U / 10U) + PERIOD : sent + PERIOD;
        uint32_t index = solicit ? 1U : 0U; // the solicitation is lone_at[0]
        EXPECT_TRUE(lone_heartbeats > index + 1U);
        if (lone_heartbeats > index + 1U) {
            EXPECT_EQ_U64(first_idle, lone_at[index]);
            EXPECT_EQ_U64(PERIOD, lone_at[index + 1U] - lone_at[index]);
        }
    }
}

// The period never goes below tt_IDLE_HEARTBEAT_RETRY_MULTIPLE retry intervals: asked for 1 ms, it is 4 ms.
static void test_the_period_has_a_floor(void) {
    init_pair(true);
    pub.idle_heartbeat_ns = tt_MILLISECOND;
    sim_start();
    drop_reader = true;
    sim_publish(1);
    sim_run_until(test_mock_now + 50U * tt_MILLISECOND);
    uint64_t floor = (uint64_t)tt_IDLE_HEARTBEAT_RETRY_MULTIPLE * tt_RELIABLE_RETRY_INITIAL;
    EXPECT_TRUE(lone_heartbeats >= 3U);
    if (lone_heartbeats >= 3U) {
        EXPECT_EQ_U64(floor, lone_at[2] - lone_at[1]);
    }
}

// sched_postpone_locked() on its own: the earliest of four entries moved past two of the others, then back before
// them, the heap still a heap and the entries due in time order.
static void dummy_timer(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
}

static void test_postpone_keeps_the_heap(void) {
    static struct tt_Context ctx;
    init_bare_node(&ctx, 9);
    int params[4];
    EXPECT_TRUE(tt_Context_schedule(&ctx, 50, dummy_timer, &params[0]));
    EXPECT_TRUE(tt_Context_schedule(&ctx, 10, dummy_timer, &params[1]));
    EXPECT_TRUE(tt_Context_schedule(&ctx, 20, dummy_timer, &params[2]));
    EXPECT_TRUE(tt_Context_schedule(&ctx, 30, dummy_timer, &params[3]));
    EXPECT_TRUE(heap_ordered(&ctx));
    EXPECT_TRUE(sched_postpone_locked(&ctx, dummy_timer, &params[1], 40)); // the root, moved down
    EXPECT_TRUE(heap_ordered(&ctx));
    uint64_t next = 0;
    EXPECT_TRUE(sched_next_time(&ctx, &next));
    EXPECT_EQ_U64(20, next);
    EXPECT_TRUE(sched_postpone_locked(&ctx, dummy_timer, &params[0], 5)); // and one moved up
    EXPECT_TRUE(heap_ordered(&ctx));
    EXPECT_TRUE(sched_next_time(&ctx, &next));
    EXPECT_EQ_U64(5, next);
    EXPECT_TRUE(!sched_postpone_locked(&ctx, idle_heartbeat_fire, &params[0], 60)); // no such entry
}

static void test_destroy_unschedules_it(void) {
    init_pair(true);
    sim_start();
    sim_publish(1);
    EXPECT_TRUE(pub.idle_heartbeat_armed);
    EXPECT_EQ_INT(1, scheduled_for(&sender, &pub));
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_destroy(&pub));
    EXPECT_EQ_INT(0, scheduled_for(&sender, &pub));
}

int main(void) {
    test_a_lost_tail_is_delivered_within_the_period();
    test_a_lone_lost_sample_waits_one_period();
    test_a_lost_idle_heartbeat_is_sent_again();
    test_a_steady_stream_sends_no_heartbeat_datagram();
    test_a_slow_stream_pays_one_heartbeat_per_sample();
    test_a_stream_within_half_a_period_never_wakes_it();
    test_an_acknowledged_writer_sends_none();
    test_nobody_to_ask_sends_none();
    test_any_heartbeat_pushes_the_next_back();
    test_the_period_has_a_floor();
    test_postpone_keeps_the_heap();
    test_destroy_unschedules_it();
    teardown_pair();
    free(sim_queue);
    if (test_result() != 0) {
        return 1;
    }
    printf("test_idle_heartbeat: all tests passed\n");
    return 0;
}
