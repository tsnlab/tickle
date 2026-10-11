/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Hybrid transport, sending (DESIGN.md section 8, "Hybrid transport", steps 3 to 5): a Publisher puts the units of a
// multi-datagram sample in runs of n per datagram to each peer that allows it, the last unit always alone as an MTU
// fragment, and every repair is an MTU unit. One writer context and up to three reader contexts in this process, joined
// by a simulated link that delivers each datagram by its destination address (0: every reader), loses whole datagrams
// as IP does - one lost frame of a run loses the run - and can drop every IP-fragmented datagram (the tc filter's
// case). Every delivered sample is checked byte for byte.
//
// Built as rmw_tickle builds core: a 65507-byte datagram, large samples.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: the send path is static
#include "test_mock.h"

#define SENDER_ID 1
#define SENDER_IP 0x0a000001
#define PORT 8282
#define TOPIC_NAME "hybrid_send_topic"
#define ENDPOINT_NAME "hybrid_send_endpoint"
#define READERS 3

#define ONE_MB 1048576U
#define FOUR_MB 4194304U
#define SMALL_LEN 8000U   // 6 units on 8/9
#define LARGE_LEN 100000U // 69 units on 11/12
#define FULL_CEILING 65507U

// --- the sample ------------------------------------------------------------------------------------------------------

static uint32_t sample_len;
static uint8_t sample_seed;

static uint8_t pattern(uint32_t i, uint8_t seed) {
    return (uint8_t)((i * 13U) + seed + (i >> 11U));
}

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = pattern(i, sample_seed);
    }
    return (int32_t)len;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

// --- the readers -----------------------------------------------------------------------------------------------------

struct reader {
    struct tt_Context ctx;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    uint32_t ip;
    int delivered;
    int torn;
    uint32_t last_seq;
    bool in_order;
    bool present;
};

static struct reader readers[READERS];
static struct reader* decoding; // the reader whose callback is running

#define WIDE_WORDS (tt_RELIABLE_BITMAP_MAX_BITS / 64)
static uint64_t tracking[READERS][tt_MAX_PEER_COUNT * WIDE_WORDS];

static int32_t checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    bool intact = len == ROUNDUP(sample_len);
    for (uint32_t i = 0; intact && i < sample_len; i++) {
        intact = payload[i] == pattern(i, sample_seed);
    }
    decoding->torn += intact ? 0 : 1;
    return 0;
}

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)data;
    for (int i = 0; i < READERS; i++) {
        if (&readers[i].sub == s) {
            readers[i].delivered++;
            readers[i].in_order = readers[i].in_order && seq_no != readers[i].last_seq;
            readers[i].last_seq = seq_no;
        }
    }
}

static void* test_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void test_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}

// --- the writer ------------------------------------------------------------------------------------------------------

static struct tt_Context sender;
static struct tt_Topic sender_topic;
static struct tt_Publisher pub;
#define CACHE_DEPTH 1024
static struct tt_ReliableCacheIndex cache_index[CACHE_DEPTH];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache cache;

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

static uint8_t reader_id(int i) {
    return (uint8_t)(2 + i);
}

// The writer, and `count` readers whose announces advertised `ceilings`; RELIABLE gives the writer a cache and each
// reader the wide window, pre-matched on the writer as its announce would.
static void init_set(uint32_t len, int count, const uint32_t* ceilings, bool reliable) {
    test_mock_reset();
    sample_len = len;
    sample_seed = 1;
    for (int i = 0; i < READERS; i++) {
        readers[i].present = false;
    }

    init_bare_node(&sender, SENDER_ID);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = 8;
    sender_topic.data_encode_size = sized_encode_size;
    sender_topic.data_encode = sized_encode;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&sender, &pub, &sender_topic, ENDPOINT_NAME));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&sender, test_acquire, test_release, NULL));
    if (reliable) {
        memset(cache_index, 0, sizeof(cache_index));
        memset(&cache, 0, sizeof(cache));
        cache.index = cache_index;
        cache.capacity = CACHE_DEPTH;
        cache.depth = CACHE_DEPTH;
        cache.arena = cache_arena;
        cache.arena_size = (uint32_t)sizeof(cache_arena);
        pub.reliable_cache = &cache;
        pub.reliable = true;
    }

    for (int i = 0; i < count; i++) {
        struct reader* r = &readers[i];
        r->present = true;
        r->ip = SENDER_IP + 1U + (uint32_t)i;
        r->delivered = 0;
        r->torn = 0;
        r->last_seq = 0;
        r->in_order = true;
        init_bare_node(&r->ctx, reader_id(i));
        memset(&r->topic, 0, sizeof(r->topic));
        r->topic.name = TOPIC_NAME;
        r->topic.data_size = 8;
        r->topic.data_decode = checking_decode;
        r->topic.data_free = free_nothing;
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&r->ctx, &r->sub, &r->topic, ENDPOINT_NAME, on_data));
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&r->ctx, test_acquire, test_release, NULL));
        pub.peers[i] = (struct tt_Peer) {reader_id(i), r->ip, PORT};
        sender.peer_ip_reassembly_max[reader_id(i)] = (uint16_t)ceilings[i];
        if (reliable) {
            r->sub.reliable = true;
            memset(tracking[i], 0, sizeof(tracking[i]));
            r->sub.tracking_bitmaps = tracking[i];
            r->sub.tracking_words = WIDE_WORDS;
            struct tt_PeerAck* ack = claim_peer_ack(&pub, reader_id(i), r->sub.endpoint.entity_id);
            EXPECT_TRUE(ack != NULL);
            if (ack != NULL) {
                ack->tracking_words = WIDE_WORDS;
            }
        }
    }
}

static void init_one(uint32_t len, uint32_t ceiling, bool reliable) {
    init_set(len, 1, &ceiling, reliable);
}

// --- the link --------------------------------------------------------------------------------------------------------

#define SIM_QUEUE 65536U
#define FRAME_PAYLOAD 1480U // an Ethernet frame's IP payload: 1472 + the UDP header

struct sim_datagram {
    uint8_t from;
    uint32_t to_ip;
    uint32_t len;
    uint8_t* bytes;
};
static struct sim_datagram* sim_queue;
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;
static uint32_t sim_frame_loss_ppm; // per frame: a datagram of f frames is lost when any of them is
static uint32_t sim_rng;
static bool sim_drop_fragmented; // the tc filter: every datagram IP had to fragment is dropped

// What the writer sent, by kind: first transmissions and repairs, and the largest repair.
struct sent_stats {
    uint32_t runs;        // FRAG_RUN / FRAG_RUN_L
    uint32_t run_units;   // units in them
    uint32_t mtu;         // FRAG_FIRST/CONT and their large pair
    uint32_t largest;     // largest fragment-path datagram
    uint32_t repair_runs; // runs carrying a unit already sent once
    uint32_t repairs;     // MTU fragments of a unit already sent once
    uint32_t repair_largest;
    uint32_t to_reader[READERS]; // datagrams addressed to each reader
    uint32_t broadcast;
    uint32_t max_n;
    uint32_t min_n;
    uint32_t frames_dropped;
};
static struct sent_stats sent;
#define SEEN_MAX 8192U
static uint8_t seen_unit[SEEN_MAX]; // by seq_no: a unit already sent once (to anyone)

static uint8_t datagram_type(const uint8_t* bytes, const uint8_t** body) {
    if (bytes[0] == tt_SINGLE_MARKER_LE || bytes[0] == tt_SINGLE_MARKER_BE) {
        *body = bytes + sizeof(struct tt_SingleHeader);
        return ((const struct tt_SingleHeader*)bytes)->type;
    }
    *body = bytes + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader);
    return ((const struct tt_SubmessageHeader*)(bytes + sizeof(struct tt_Header)))->type;
}

// The units a fragment-path datagram carries: its first unit's seq_no and how many. False for anything else.
static bool datagram_units(const uint8_t* bytes, uint32_t* seq_no, uint32_t* units, bool* run) {
    const uint8_t* body = NULL;
    uint8_t type = datagram_type(bytes, &body);
    *run = false;
    *units = 1;
    switch (type) {
    case tt_SUBMESSAGE_TYPE_FRAG_FIRST:
    case tt_SUBMESSAGE_TYPE_FRAG_FIRST_L: {
        struct tt_DataHeader data;
        memcpy(&data, body, sizeof(data));
        *seq_no = data.seq_no;
        return true;
    }
    case tt_SUBMESSAGE_TYPE_FRAG_CONT: {
        struct tt_FragContHeader cont;
        memcpy(&cont, body, sizeof(cont));
        *seq_no = cont.seq_no;
        return true;
    }
    case tt_SUBMESSAGE_TYPE_FRAG_CONT_L: {
        struct tt_FragContLHeader cont;
        memcpy(&cont, body, sizeof(cont));
        *seq_no = cont.seq_no;
        return true;
    }
    case tt_SUBMESSAGE_TYPE_FRAG_RUN: {
        struct tt_FragRunHeader header;
        memcpy(&header, body, sizeof(header));
        *seq_no = header.data.seq_no;
        *units = header.run_count;
        *run = true;
        return true;
    }
    case tt_SUBMESSAGE_TYPE_FRAG_RUN_L: {
        struct tt_FragRunLHeader header;
        memcpy(&header, body, sizeof(header));
        *seq_no = header.data.seq_no;
        *units = header.run_count;
        *run = true;
        return true;
    }
    default:
        return false;
    }
}

static void note_sent(const uint8_t* bytes, uint32_t len, uint32_t to_ip) {
    uint32_t seq_no = 0;
    uint32_t units = 0;
    bool run = false;
    if (!datagram_units(bytes, &seq_no, &units, &run)) {
        return;
    }
    bool repeat = false;
    for (uint32_t i = 0; i < units; i++) {
        uint32_t slot = (seq_no + i) % SEEN_MAX;
        repeat = repeat || seen_unit[slot] != 0;
        seen_unit[slot] = 1;
    }
    if (repeat) {
        sent.repair_runs += run ? 1U : 0U;
        sent.repairs += run ? 0U : 1U;
        sent.repair_largest = len > sent.repair_largest ? len : sent.repair_largest;
    } else if (run) {
        sent.runs++;
        sent.run_units += units;
        sent.max_n = units > sent.max_n ? units : sent.max_n;
        sent.min_n = sent.min_n == 0 || units < sent.min_n ? units : sent.min_n;
    } else {
        sent.mtu++;
    }
    sent.largest = len > sent.largest ? len : sent.largest;
    if (to_ip == 0) {
        sent.broadcast++;
    }
    for (int i = 0; i < READERS; i++) {
        if (readers[i].present && readers[i].ip == to_ip) {
            sent.to_reader[i]++;
        }
    }
}

static void sim_capture(const void* buf, size_t len) {
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->to_ip = test_mock_send_hook_ip;
    d->len = (uint32_t)len;
    d->bytes = (uint8_t*)malloc(len);
    memcpy(d->bytes, buf, len);
    sim_tail++;
    if (sim_acting == SENDER_ID) {
        note_sent(d->bytes, d->len, d->to_ip);
    }
}

static bool sim_frame_lost(void) {
    sim_rng = (sim_rng * 1103515245U) + 12345U;
    uint32_t draw = ((sim_rng >> 8) & 0xFFFFFFU) % 1000000U;
    return draw < sim_frame_loss_ppm;
}

static bool sim_lost(const struct sim_datagram* d) {
    uint32_t frames = (d->len + 8U + FRAME_PAYLOAD - 1U) / FRAME_PAYLOAD;
    if (sim_drop_fragmented && frames > 1U && d->from == SENDER_ID) {
        sent.frames_dropped += frames;
        return true;
    }
    bool lost = false;
    for (uint32_t i = 0; i < frames; i++) {
        lost = sim_frame_lost() || lost; // every frame drawn, so the stream of draws does not depend on n
    }
    return lost;
}

static void sim_deliver(struct sim_datagram* d) {
    if (!sim_lost(d)) {
        if (d->from == SENDER_ID) {
            for (int i = 0; i < READERS; i++) {
                if (readers[i].present && (d->to_ip == 0 || d->to_ip == readers[i].ip)) {
                    static uint8_t copy[70000];
                    memcpy(copy, d->bytes, d->len);
                    sim_acting = readers[i].ctx.id;
                    decoding = &readers[i];
                    process_packet(&readers[i].ctx, copy, 0, d->len, SENDER_IP, PORT, tt_TRANSPORT_UDP);
                }
            }
        } else if (d->to_ip == 0 || d->to_ip == SENDER_IP) {
            sim_acting = SENDER_ID;
            process_packet(&sender, d->bytes, 0, d->len, readers[d->from - 2].ip, PORT, tt_TRANSPORT_UDP);
        }
    }
    free(d->bytes);
    d->bytes = NULL;
}

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        sim_deliver(d);
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
    uint64_t soonest = UINT64_MAX;
    uint64_t at = 0;
    if (sched_next_time(&sender, &at) && at < soonest) {
        soonest = at;
    }
    for (int i = 0; i < READERS; i++) {
        if (readers[i].present && sched_next_time(&readers[i].ctx, &at) && at < soonest) {
            soonest = at;
        }
    }
    return soonest;
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
    for (int i = 0; i < READERS; i++) {
        if (readers[i].present) {
            sim_run_due(&readers[i].ctx);
        }
    }
    return true;
}

static void sim_start(uint32_t frame_loss_ppm) {
    if (sim_queue == NULL) {
        sim_queue = (struct sim_datagram*)calloc(SIM_QUEUE, sizeof(struct sim_datagram));
    }
    sim_head = sim_tail = 0;
    sim_frame_loss_ppm = frame_loss_ppm;
    sim_rng = 11;
    sim_drop_fragmented = false;
    memset(&sent, 0, sizeof(sent));
    memset(seen_unit, 0, sizeof(seen_unit));
    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
}

static int total_delivered(void) {
    int total = 0;
    for (int i = 0; i < READERS; i++) {
        total += readers[i].present ? readers[i].delivered : 0;
    }
    return total;
}

// Publishes one sample with `seed`, retrying while KEEP_ALL says WOULD_BLOCK, and runs the link until it is quiet.
static tt_ret_t sim_publish(uint8_t seed) {
    sample_seed = seed;
    sim_acting = SENDER_ID;
    tt_ret_t result = tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len);
    sim_drain();
    return result;
}

static void sim_settle(int want, uint64_t budget) {
    uint64_t since = test_mock_now;
    while (total_delivered() < want && test_mock_now - since < budget) {
        if (!sim_step()) {
            break;
        }
    }
}

static void expect_intact(int reader, int want) {
    EXPECT_EQ_INT(want, readers[reader].delivered);
    EXPECT_EQ_INT(0, readers[reader].torn);
}

// --- step 3: best-effort runs ----------------------------------------------------------------------------------------

// 8,000 B is 6 units on 8/9: one run of 5 and the tail, in two send calls; delivered whole.
static void test_small_sample_goes_as_one_run_and_its_tail(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    sim_start(0);
    int calls = test_mock_send_batch_call_count;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    EXPECT_EQ_U32(1, sent.runs);
    EXPECT_EQ_U32(5, sent.run_units);
    EXPECT_EQ_U32(1, sent.mtu);
    EXPECT_EQ_INT(2, test_mock_send_batch_call_count - calls); // the runs, then the tail in its own call
    EXPECT_EQ_U32(6, pub.seq_no);                              // still one seq_no per unit
    expect_intact(0, 1);
    EXPECT_EQ_U64(1, sender.hybrid.runs);
    EXPECT_EQ_U64(5, sender.hybrid.run_units);
    EXPECT_EQ_U64(1, sender.hybrid.mtu_units);
    EXPECT_EQ_U64(1, sender.hybrid.n_hist[tt_HYBRID_N_BUCKETS - 1]); // n as chosen: n_max, whatever the sample holds
}

// 100,000 B is 69 units on 11/12: runs of 45 and 23, then the tail; 1 MB 18 datagrams, 4 MB 66 (DESIGN.md section 8).
static void large_be_round_trip(uint32_t len, uint32_t datagrams) {
    init_one(len, FULL_CEILING, false);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(3));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_U32(datagrams, sent.runs + sent.mtu);
    EXPECT_EQ_U32(1, sent.mtu);
    EXPECT_EQ_U32(45, sent.max_n);
    EXPECT_TRUE(sent.largest <= FULL_CEILING);
    EXPECT_EQ_U32(tt_sample_datagrams(len) - 1U, sent.run_units);
    expect_intact(0, 1);
    EXPECT_EQ_U64(1, readers[0].ctx.large.reassembled);
    EXPECT_EQ_U64(0, readers[0].ctx.large.dropped);
}

static void test_large_samples_go_in_runs_of_n_max(void) {
    large_be_round_trip(LARGE_LEN, 3);
    large_be_round_trip(ONE_MB, 18);
    large_be_round_trip(FOUR_MB, 66);
}

// Runs only from three units: a two-unit sample is its two MTU fragments, as before; three are a run of two and the
// tail.
static void test_runs_start_at_three_units(void) {
    init_one(2000, FULL_CEILING, false);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    EXPECT_EQ_U32(0, sent.runs);
    EXPECT_EQ_U32(2, sent.mtu);
    expect_intact(0, 1);
    init_one(3500, FULL_CEILING, false);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    EXPECT_EQ_U32(1, sent.runs);
    EXPECT_EQ_U32(2, sent.run_units);
    EXPECT_EQ_U32(1, sent.mtu);
    expect_intact(0, 1);
}

// What a peer advertises bounds n: 0 (FreeRTOS) and one unit (a core-default build, 1472) get MTU; 3,000 B two units.
// And the Publisher's own ip_datagram_max: 8 large units.
static void test_the_peers_ceiling_and_the_publishers_bound_n(void) {
    static const uint32_t mtu_ceilings[] = {0, 1472, 1500};
    for (size_t i = 0; i < sizeof(mtu_ceilings) / sizeof(mtu_ceilings[0]); i++) {
        init_one(LARGE_LEN, mtu_ceilings[i], false);
        sim_start(0);
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
        EXPECT_EQ_U32(0, sent.runs);
        EXPECT_EQ_U32(69, sent.mtu);
        EXPECT_TRUE(sent.largest <= tt_CONTROL_MAX_LENGTH);
        expect_intact(0, 1);
    }
    init_one(SMALL_LEN, 3000, false);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    EXPECT_EQ_U32(2, sent.max_n); // runs 0-1, 2-3, 4 and the tail
    EXPECT_EQ_U32(3, sent.runs);
    EXPECT_TRUE(sent.largest <= 3000);
    expect_intact(0, 1);

    init_one(LARGE_LEN, FULL_CEILING, false);
    pub.ip_datagram_max = (uint16_t)(28U + (8U * 1452U)); // exactly eight large units
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_U32(8, sent.max_n);
    EXPECT_TRUE(sent.largest <= 28U + (8U * 1452U));
    expect_intact(0, 1);
    pub.ip_datagram_max = (uint16_t)(28U + (8U * 1452U) - 1U); // one byte short of eight
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_U32(7, sent.max_n);
}

// MTU, from the context or the Publisher, sends no run; IP on a Publisher overrides a context's MTU.
static void test_mtu_mode_sends_no_run(void) {
    init_one(LARGE_LEN, FULL_CEILING, false);
    sender.large_fragmentation = tt_LARGE_FRAGMENTATION_MTU;
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_U32(0, sent.runs);
    EXPECT_EQ_U32(69, sent.mtu);
    expect_intact(0, 1);
    pub.large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_U32(2, sent.runs);
    sender.large_fragmentation = tt_LARGE_FRAGMENTATION_AUTO;
    pub.large_fragmentation = tt_LARGE_FRAGMENTATION_MTU;
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(3));
    sim_settle(3, tt_SECOND);
    EXPECT_EQ_U32(0, sent.runs);
    expect_intact(0, 3);
}

// Two readers by unicast, one advertising 0: each its own n - runs to one, MTU fragments to the other - from the one
// sample, and both deliver it whole. Small and large.
static void test_mixed_peers_by_unicast_each_get_their_own_n(void) {
    static const uint32_t ceilings[] = {FULL_CEILING, 0};
    static const uint32_t lens[] = {SMALL_LEN, LARGE_LEN};
    for (size_t i = 0; i < 2; i++) {
        init_set(lens[i], 2, ceilings, false);
        sim_start(0);
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
        sim_settle(2, tt_SECOND);
        uint32_t units = tt_sample_datagrams(lens[i]);
        EXPECT_EQ_U32(units == 6 ? 2U : 3U, sent.to_reader[0]);
        EXPECT_EQ_U32(units, sent.to_reader[1]);
        EXPECT_EQ_U32(0, sent.broadcast);
        expect_intact(0, 1);
        expect_intact(1, 1);
    }
}

// Three readers (above the unicast threshold of 2): one broadcast with the smallest n - MTU when any advertises 0, two
// units when the smallest allows two, n_max when all allow it.
static void test_broadcast_takes_the_smallest_n(void) {
    static const uint32_t with_zero[] = {FULL_CEILING, 0, FULL_CEILING};
    static const uint32_t with_two[] = {FULL_CEILING, 3000, FULL_CEILING};
    static const uint32_t all_full[] = {FULL_CEILING, FULL_CEILING, FULL_CEILING};
    init_set(LARGE_LEN, 3, with_zero, false);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(3, tt_SECOND);
    EXPECT_EQ_U32(0, sent.runs);
    EXPECT_EQ_U32(69, sent.broadcast);
    for (int i = 0; i < 3; i++) {
        expect_intact(i, 1);
    }
    init_set(LARGE_LEN, 3, with_two, false);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(3, tt_SECOND);
    EXPECT_EQ_U32(2, sent.max_n);
    EXPECT_EQ_U32(2, sent.min_n);
    init_set(LARGE_LEN, 3, all_full, false);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(3, tt_SECOND);
    EXPECT_EQ_U32(45, sent.max_n);
    EXPECT_EQ_U32(3, sent.broadcast);
    for (int i = 0; i < 3; i++) {
        expect_intact(i, 1);
    }
}

// A send buffer with room for three datagrams at a time: the cursor moves by the units of the runs that went, and every
// sample arrives whole.
static void test_a_full_send_buffer_resumes_runs_where_they_stopped(void) {
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(0);
    test_mock_nonblocking_room = 3;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    for (int step = 0; step < 100 && total_delivered() < 1; step++) {
        test_mock_nonblocking_room = 3;
        if (!sim_step()) {
            test_mock_now += tt_MILLISECOND;
        }
    }
    EXPECT_EQ_U32(18, sent.runs + sent.mtu);
    expect_intact(0, 1);
    EXPECT_TRUE(sender.large.send_waits > 0);
}

// Best effort at 1% frame loss: a sample whose run lost a frame is not delivered, never torn.
static void test_best_effort_loss_never_tears(void) {
    init_one(LARGE_LEN, FULL_CEILING, false);
    sim_start(10000);
    for (uint8_t s = 1; s <= 40; s++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, 10 * tt_MILLISECOND);
    }
    EXPECT_EQ_INT(0, readers[0].torn);
    EXPECT_TRUE(readers[0].delivered > 0 && readers[0].delivered < 40);
}

// RELIABLE AUTO sends MTU units to a reader it has measured nothing of: its loss bound starts at 1 (step 4's cost
// model, which needs exposure before n leaves 1). Forced IP sends runs all the same.
static void test_reliable_auto_starts_at_mtu(void) {
    init_one(LARGE_LEN, FULL_CEILING, true);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_U32(0, sent.runs);
    expect_intact(0, 1);
    pub.large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_U32(2, sent.runs);
    expect_intact(0, 2);
}

int main(void) {
    test_small_sample_goes_as_one_run_and_its_tail();
    test_large_samples_go_in_runs_of_n_max();
    test_runs_start_at_three_units();
    test_the_peers_ceiling_and_the_publishers_bound_n();
    test_mtu_mode_sends_no_run();
    test_mixed_peers_by_unicast_each_get_their_own_n();
    test_broadcast_takes_the_smallest_n();
    test_a_full_send_buffer_resumes_runs_where_they_stopped();
    test_best_effort_loss_never_tears();
    test_reliable_auto_starts_at_mtu();
    free(sim_queue);
    if (test_result() != 0) {
        return 1;
    }
    printf("test_hybrid_send: all tests passed\n");
    return 0;
}
