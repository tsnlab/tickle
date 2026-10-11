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
#define TT_HYBRID_TEST_SEAM 1 // loss_estimate_p() answers hybrid_test_forced_p when it is set

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
// A RELIABLE reader of a sample on 8/9 holds fragments that come out of order in its reorder buffer.
#define REORDER_SLOTS 128
#define REORDER_SLOT_BYTES tt_REORDER_SLOT_SIZE(1472)
static uint64_t reorder[READERS][REORDER_SLOTS * REORDER_SLOT_BYTES / sizeof(uint64_t)];

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
    reset_node_state(node); // as creation does: the hybrid state reads the mock's limits
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
    test_mock_hybrid_reset();
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
            memset(reorder[i], 0, sizeof(reorder[i]));
            r->sub.reorder_storage = reorder[i];
            r->sub.reorder_slots = REORDER_SLOTS;
            r->sub.reorder_slot_bytes = REORDER_SLOT_BYTES;
            struct tt_PeerAck* ack = claim_peer_ack(&pub, reader_id(i), r->sub.endpoint.entity_id);
            EXPECT_TRUE(ack != NULL);
            if (ack != NULL) {
                ack->tracking_words = WIDE_WORDS;
                arm_match_heartbeat(&pub, ack); // as its announce would: the first sample's baseline (match point)
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
#define SEEN_MAX 65536U
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
                    readers[i].ctx.rx_tail = d->len; // as process_datagram_in() sets it
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
    EXPECT_EQ_U32(6, readers[0].ctx.hybrid.rx_loss.frames);          // the run's units and the tail, no gap
    EXPECT_EQ_U32(0, readers[0].ctx.hybrid.rx_loss.events);
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
    // The reader saw every unit, runs included, and no gap.
    EXPECT_EQ_U32(tt_sample_datagrams(len), readers[0].ctx.hybrid.rx_loss.frames);
    EXPECT_EQ_U32(0, readers[0].ctx.hybrid.rx_loss.events);
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

// --- the reader's cap (DESIGN.md section 8, "BEST_EFFORT") -----------------------------------------------------------

#define ONE_MB_UNITS 723U
#define F16_UNITS 12U

// The cap at U units a second (one second's worth received) and a forced p.
static uint32_t cap_at(struct tt_Context* node, uint32_t units_per_s, double p) {
    node->hybrid.tick_units = units_per_s;
    hybrid_test_forced_p = p;
    uint32_t cap = hybrid_reader_cap(node, tt_SECOND, 45);
    hybrid_test_forced_p = -1.0;
    return cap;
}

// DESIGN.md's own examples, at the kernel's defaults (4 MB, 30 s, 2 KiB a fragment): 1 MB at 30 Hz gives 31 at 0.01%,
// 3 at 0.1%, 1 at 1%; 16 KB at 100 Hz gives 5 at 1% and 1 at 5%. Nothing received: n_max.
static void test_the_reader_cap_is_the_designs(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    EXPECT_TRUE(node->hybrid.rx_capable);
    EXPECT_EQ_U32(4194304, node->hybrid.frag_high_thresh);
    EXPECT_EQ_U32(30, node->hybrid.frag_time_s);
    EXPECT_EQ_U32(31, cap_at(node, ONE_MB_UNITS * 30U, 0.0001));
    EXPECT_EQ_U32(3, cap_at(node, ONE_MB_UNITS * 30U, 0.001));
    EXPECT_EQ_U32(1, cap_at(node, ONE_MB_UNITS * 30U, 0.01));
    EXPECT_EQ_U32(5, cap_at(node, F16_UNITS * 100U, 0.01));
    EXPECT_EQ_U32(1, cap_at(node, F16_UNITS * 100U, 0.05));
    EXPECT_EQ_U32(45, cap_at(node, 0, 0.5));
    // A smaller threshold caps lower: a quarter of it at 0.01% gives 7.
    node->hybrid.frag_high_thresh = 4194304U / 4U;
    EXPECT_EQ_U32(7, cap_at(node, ONE_MB_UNITS * 30U, 0.0001));
    // Quantised as advertised: 31 -> 16 units, 3 -> 2, 1 -> 1, n_max -> the configured ceiling.
    EXPECT_EQ_U32(28U + (16U * 1454U), hybrid_cap_bytes(node, 31, 45));
    EXPECT_EQ_U32(28U + (2U * 1454U), hybrid_cap_bytes(node, 3, 45));
    EXPECT_EQ_U32(28U + 1454U, hybrid_cap_bytes(node, 1, 45));
    EXPECT_EQ_U32(FULL_CEILING, hybrid_cap_bytes(node, 45, 45));
    // ... and each reads back as exactly that many units of either type pair.
    EXPECT_EQ_U32(16, hybrid_units_within(hybrid_cap_bytes(node, 31, 45), false));
    EXPECT_EQ_U32(16, hybrid_units_within(hybrid_cap_bytes(node, 31, 45), true));
    EXPECT_EQ_U32(1, hybrid_units_within(hybrid_cap_bytes(node, 1, 45), true));
}

// The limits come from the HAL at create, the kernel's defaults where they cannot be read; a context whose ceiling
// holds one unit (a core-default build) keeps no cap at all.
static void test_the_reader_reads_the_kernels_limits(void) {
    test_mock_reset();
    test_mock_hybrid_reset();
    static struct tt_Context node;
    test_mock_reasm_high_thresh = 1000000;
    test_mock_reasm_time_s = 7;
    init_bare_node(&node, 9);
    EXPECT_EQ_U32(1000000, node.hybrid.frag_high_thresh);
    EXPECT_EQ_U32(7, node.hybrid.frag_time_s);
    test_mock_reasm_limits_readable = false;
    init_bare_node(&node, 9);
    EXPECT_EQ_U32(tt_HYBRID_IPFRAG_HIGH_THRESH, node.hybrid.frag_high_thresh);
    EXPECT_EQ_U32(tt_HYBRID_IPFRAG_TIME_S, node.hybrid.frag_time_s);
    uint16_t setting = _tt_CONFIG.ip_reassembly_max;
    _tt_CONFIG.ip_reassembly_max = 1472;
    init_bare_node(&node, 9);
    EXPECT_TRUE(!node.hybrid.rx_capable);
    node.hybrid.tick_units = 100000;
    hybrid_reader_tick(&node, tt_SECOND);
    hybrid_reader_tick(&node, 3 * tt_SECOND);
    EXPECT_EQ_U32(1472, node.hybrid.advertised); // unchanged: nothing to cap
    EXPECT_EQ_INT(0, test_mock_reasm_failure_reads);
    _tt_CONFIG.ip_reassembly_max = setting;
}

// The pooled estimate: a unit beyond the next expected is one event whatever it skipped, every unit is exposure once,
// a sample left short is one event when the writer's next begins, and a repair is neither. U is the exposure too.
static void test_the_reader_counts_gaps_over_exposure(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    struct tt_LossEstimate* loss = &node->hybrid.rx_loss;
    hybrid_rx_note(node, SENDER_ID, 7, 100, 0, 100, 45); // units 0-44
    EXPECT_EQ_U32(45, loss->frames);
    EXPECT_EQ_U32(0, loss->events);
    hybrid_rx_note(node, SENDER_ID, 7, 100, 90, 100, 9); // 45-89 lost as one run
    EXPECT_EQ_U32(99, loss->frames);
    EXPECT_EQ_U32(1, loss->events);
    hybrid_rx_note(node, SENDER_ID, 7, 100, 50, 100, 1); // a repair
    EXPECT_EQ_U32(99, loss->frames);
    EXPECT_EQ_U32(1, loss->events);
    hybrid_rx_note(node, SENDER_ID, 7, 200, 0, 100, 1); // the next sample: the tail of the last never came
    EXPECT_EQ_U32(101, loss->frames);
    EXPECT_EQ_U32(2, loss->events);
    hybrid_rx_note(node, SENDER_ID, 8, 50, 3, 10, 1); // another writer, first met mid-sample: no gap
    EXPECT_EQ_U32(102, loss->frames);
    EXPECT_EQ_U32(2, loss->events);
    hybrid_rx_note(node, SENDER_ID, 7, 100, 99, 100, 1); // an older sample's unit
    EXPECT_EQ_U32(102, loss->frames);
    EXPECT_EQ_U32(102, node->hybrid.tick_units); // U counts the exposure: units sent to it, arrived or not
    // The window halves both at 2^17 frames.
    loss->frames = (uint32_t)tt_HYBRID_WINDOW_FRAMES - 1U;
    loss->events = 40;
    loss_estimate_observe(loss, 1, 0);
    EXPECT_EQ_U32((uint32_t)tt_HYBRID_WINDOW_FRAMES / 2U, loss->frames);
    EXPECT_EQ_U32(20, loss->events);
}

// The bound: 3 / N at no events (the rule of three), the exact Poisson limits up to 10, Wilson-Hilferty above; 1 with
// no exposure.
static void test_the_loss_bound_is_the_poisson_upper_limit(void) {
    struct tt_LossEstimate estimate = {0, 0};
    EXPECT_TRUE(loss_estimate_p(&estimate) == 1.0);
    estimate.frames = 10000;
    EXPECT_TRUE(loss_estimate_p(&estimate) > 0.000299 && loss_estimate_p(&estimate) < 0.000300);
    estimate.events = 10;
    EXPECT_TRUE(loss_estimate_p(&estimate) > 0.001696 && loss_estimate_p(&estimate) < 0.001697);
    estimate.events = 100; // exactly 118.08
    EXPECT_TRUE(loss_estimate_p(&estimate) > 0.01178 && loss_estimate_p(&estimate) < 0.01183);
    estimate.events = 1000; // exactly 1053.60
    EXPECT_TRUE(loss_estimate_p(&estimate) > 0.10530 && loss_estimate_p(&estimate) < 0.10542);
    estimate.frames = 10;
    EXPECT_TRUE(loss_estimate_p(&estimate) == 1.0);
}

// Routes everything a reader sent (its announces) to the writer.
static void deliver_reader_sends(void) {
    sim_drain();
}

// Ticks every present reader at `now`, and lets its batched announce go to the writer at the next flush.
static void tick_readers(uint64_t now) {
    test_mock_now = now;
    for (int i = 0; i < READERS; i++) {
        if (readers[i].present) {
            sim_acting = readers[i].ctx.id;
            hybrid_reader_tick(&readers[i].ctx, now);
        }
    }
    test_mock_now = now + tt_CONTEXT_TX_INTERVAL;
    for (int i = 0; i < READERS; i++) {
        if (readers[i].present) {
            sim_run_due(&readers[i].ctx);
        }
    }
    deliver_reader_sends();
}

// A reader whose cap is switched off (a benchmark arm, _tt_CONFIG.reassembly_cap_off) advertises its configured ceiling
// whatever its loss and load, and is not withdrawn by the kernel's failures either.
static void test_a_reader_with_the_cap_off_advertises_its_ceiling(void) {
    _tt_CONFIG.reassembly_cap_off = 1;
    init_one(LARGE_LEN, FULL_CEILING, false);
    _tt_CONFIG.reassembly_cap_off = 0;
    struct tt_Context* node = &readers[0].ctx;
    EXPECT_TRUE(node->hybrid.cap_off);
    sim_start(0);
    uint64_t t = test_mock_now;
    tick_readers(t);
    hybrid_test_forced_p = 0.05;
    node->hybrid.tick_units = 100000;
    tick_readers(t += tt_SECOND);
    EXPECT_EQ_U32(FULL_CEILING, node->hybrid.advertised); // capped, it would be one unit
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    test_mock_reasm_failures = 3;
    tick_readers(t += tt_SECOND);
    EXPECT_EQ_U32(FULL_CEILING, node->hybrid.advertised);
    EXPECT_TRUE(!node->hybrid.withdrawn);
    hybrid_test_forced_p = -1.0;
    init_one(LARGE_LEN, FULL_CEILING, false);
    EXPECT_TRUE(!readers[0].ctx.hybrid.cap_off); // the default
}

// Best effort through a path that drops every IP fragment (the tc filter): the runs are lost, only the tails arrive;
// the reader's gaps raise its p, its cap falls to one unit at the next tick, the writer learns it from the announce
// and sends MTU units - and samples arrive again. Then the filter goes and the cap comes back as clean frames build up.
static void test_a_path_that_drops_fragments_brings_n_to_one(void) {
    init_one(LARGE_LEN, FULL_CEILING, false);
    readers[0].ctx.ip_reassembly_max = FULL_CEILING;
    sim_start(0);
    sim_drop_fragmented = true;
    uint64_t start = test_mock_now;
    tick_readers(start);
    uint8_t seed = 1;
    for (; seed <= 30; seed++) { // one second at 30 Hz
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(seed));
        test_mock_now += 33 * tt_MILLISECOND;
    }
    EXPECT_EQ_INT(0, readers[0].delivered);
    EXPECT_TRUE(sent.runs > 0);
    uint64_t generation = readers[0].ctx.last_modified;
    tick_readers(start + (tt_SECOND / 2U)); // within the announce interval: nothing yet
    EXPECT_EQ_U32(FULL_CEILING, readers[0].ctx.hybrid.advertised);
    tick_readers(start + tt_SECOND);
    EXPECT_TRUE(readers[0].ctx.last_modified != generation); // a new generation: a peer that missed it pulls it
    EXPECT_EQ_U32(28U + 1454U, readers[0].ctx.hybrid.advertised);
    EXPECT_EQ_U64(1, readers[0].ctx.hybrid.cap_changes);
    EXPECT_EQ_U32(28U + 1454U, sender.peer_ip_reassembly_max[reader_id(0)]); // learnt from the ordinary announce
    uint32_t runs_before = sent.runs;
    for (; seed <= 40; seed++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(seed));
        test_mock_now += 33 * tt_MILLISECOND;
    }
    EXPECT_EQ_U32(runs_before, sent.runs);
    EXPECT_EQ_INT(10, readers[0].delivered);
    EXPECT_EQ_INT(0, readers[0].torn);
}

// The withdrawal (DESIGN.md section 8): the host's ReasmFails rising while runs arrive advertises one unit whatever p
// says; it is re-derived once the counters have been still for 2T. Rising without runs arriving withdraws nothing, nor
// does a counter that cannot be read.
static void test_reassembly_failures_withdraw_the_ceiling(void) {
    init_one(LARGE_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    test_mock_reasm_time_s = 30;
    sim_start(0);
    uint64_t t = test_mock_now;
    tick_readers(t);              // the baseline reading
    test_mock_reasm_failures = 5; // something else's failures, and no run arriving
    tick_readers(t += tt_SECOND);
    EXPECT_TRUE(!node->hybrid.withdrawn);
    EXPECT_EQ_U32(FULL_CEILING, node->hybrid.advertised);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1)); // runs arrive ...
    test_mock_reasm_failures = 9;             // ... and the failures rise
    tick_readers(t += tt_SECOND);
    EXPECT_TRUE(node->hybrid.withdrawn);
    EXPECT_EQ_U64(1, node->hybrid.withdrawals);
    EXPECT_EQ_U32(28U + 1454U, node->hybrid.advertised);
    EXPECT_EQ_U32(28U + 1454U, sender.peer_ip_reassembly_max[reader_id(0)]);
    tick_readers(t += 30 * tt_SECOND); // still for 30 s: not yet 2T
    EXPECT_TRUE(node->hybrid.withdrawn);
    test_mock_reasm_failures = 10; // moved again: the 2T starts over
    tick_readers(t += 20 * tt_SECOND);
    EXPECT_TRUE(node->hybrid.withdrawn);
    tick_readers(t += 59 * tt_SECOND);
    EXPECT_TRUE(node->hybrid.withdrawn);
    tick_readers(t += 2 * tt_SECOND); // 61 s still
    EXPECT_TRUE(!node->hybrid.withdrawn);
    EXPECT_EQ_U32(FULL_CEILING, node->hybrid.advertised);
    EXPECT_EQ_U32(FULL_CEILING, sender.peer_ip_reassembly_max[reader_id(0)]);
    // Unreadable: nothing withdrawn, whatever arrives - and the first reading after is a baseline, not a rise.
    test_mock_reasm_failures_readable = false;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
    tick_readers(t += tt_SECOND);
    EXPECT_TRUE(!node->hybrid.withdrawn);
    init_one(LARGE_LEN, FULL_CEILING, false);
    node = &readers[0].ctx;
    sim_start(0);
    t = test_mock_now;
    test_mock_reasm_failures_readable = false;
    tick_readers(t);
    test_mock_reasm_failures_readable = true;
    test_mock_reasm_failures = 50;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(3));
    tick_readers(t += tt_SECOND);
    EXPECT_TRUE(!node->hybrid.withdrawn);
}

// --- step 4: RELIABLE runs with MTU repairs -------------------------------------------------------------------------

// n follows p (forced through the test seam), by the cost model at the platform default costs - DESIGN.md's table
// (45, 25, 11, 8, 4 at 0, 0.1, 0.5, 1 and 5% for a share of 0.6; the default share, 0.574, gives 24 at 0.1%) - and is
// never above what the reader allows.
static void test_n_follows_p(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    static const double ps[] = {0.0, 0.0001, 0.001, 0.005, 0.01, 0.05, 0.2, 0.5, 1.0};
    static const uint32_t ns[] = {45, 45, 24, 11, 8, 4, 2, 1, 1};
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
        hybrid_test_forced_p = ps[i];
        EXPECT_EQ_U32(ns[i], hybrid_reliable_n(&sender, &pub, reader_id(0), 45));
        EXPECT_EQ_U32(ns[i] < 2U ? ns[i] : 2U, hybrid_reliable_n(&sender, &pub, reader_id(0), 2));
    }
    // A node with no reader of this Publisher: 1.
    hybrid_test_forced_p = 0.0;
    EXPECT_EQ_U32(1, hybrid_reliable_n(&sender, &pub, 77, 45));
    // Two readers on one node: the most pessimistic one's p decides - 10 events in 1,000 frames (bound 1.7%: 6), not
    // none in 100,000 (45).
    hybrid_test_forced_p = -1.0;
    struct tt_PeerAck* second = claim_peer_ack(&pub, reader_id(0), 0x51515151U);
    EXPECT_TRUE(second != NULL);
    if (second != NULL) {
        pub.peer_acks[0].loss = (struct tt_LossEstimate) {0, 100000};
        second->loss = (struct tt_LossEstimate) {10, 1000};
        EXPECT_EQ_U32(6, hybrid_reliable_n(&sender, &pub, reader_id(0), 45));
        second->loss = (struct tt_LossEstimate) {0, 100000};
        pub.peer_acks[0].loss = (struct tt_LossEstimate) {10, 1000};
        EXPECT_EQ_U32(6, hybrid_reliable_n(&sender, &pub, reader_id(0), 45));
        forget_peer_ack(&pub, reader_id(0), 0x51515151U, false);
        pub.peer_acks[0].loss = (struct tt_LossEstimate) {0, 0};
    }
    // And the sample goes in runs of that n: 1% -> 8.
    hybrid_test_forced_p = 0.01;
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_U32(8, sent.max_n);
    EXPECT_EQ_U32(91, sent.runs); // 722 units in runs of 8: 90 and one of 2
    EXPECT_EQ_U32(8, pub.peer_acks[0].run_n);
    expect_intact(0, 1);
    hybrid_test_forced_p = -1.0;
}

// AUTO from nothing measured: the first 1 MB sample goes as MTU units, the second in runs of 12 (723 clean frames: p's
// bound 3/723), and n reaches 45 near 11,000 clean frames - the rule of three leaving MTU on its own.
static void test_reliable_auto_leaves_mtu_as_clean_frames_accumulate(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_U32(0, sent.runs);
    EXPECT_EQ_U32(723, pub.peer_acks[0].loss.frames);
    memset(&sent, 0, sizeof(sent));
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_U32(12, sent.max_n);
    for (uint8_t s = 3; s <= 17; s++) {
        memset(&sent, 0, sizeof(sent));
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, tt_SECOND);
    }
    EXPECT_EQ_U32(45, sent.max_n);
    expect_intact(0, 17);
    EXPECT_TRUE(readers[0].in_order);
}

// Under frame loss, RELIABLE delivers every sample whole and in order, and every repair is an MTU unit - no run is ever
// resent, no repair datagram is above one frame. Forced IP (n = 45 throughout) and AUTO, 1% and 5% frame loss, small
// and large samples.
static void reliable_under_loss(uint32_t len, uint8_t mode, uint32_t loss_ppm, uint8_t samples) {
    init_one(len, FULL_CEILING, true);
    pub.large_fragmentation = mode;
    sim_start(loss_ppm);
    for (uint8_t s = 1; s <= samples; s++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, 5 * tt_SECOND);
    }
    expect_intact(0, samples);
    EXPECT_TRUE(readers[0].in_order);
    EXPECT_EQ_U32(0, sent.repair_runs);
    EXPECT_TRUE(sent.repairs > 0);
    EXPECT_TRUE(sent.repair_largest <= tt_CONTROL_MAX_LENGTH);
    if (mode == tt_LARGE_FRAGMENTATION_IP) {
        EXPECT_TRUE(sent.runs > 0);
    }
}

static void test_reliable_repairs_are_mtu_units_only(void) {
    reliable_under_loss(ONE_MB, tt_LARGE_FRAGMENTATION_IP, 10000, 8);
    reliable_under_loss(ONE_MB, tt_LARGE_FRAGMENTATION_IP, 50000, 4);
    reliable_under_loss(ONE_MB, tt_LARGE_FRAGMENTATION_AUTO, 10000, 8);
    reliable_under_loss(SMALL_LEN * 4U, tt_LARGE_FRAGMENTATION_IP, 50000, 40); // 23 units on 8/9
    reliable_under_loss(SMALL_LEN * 4U, tt_LARGE_FRAGMENTATION_AUTO, 10000, 40);
}

// The writer's count of a reader's loss: a contiguous range of first NACKs is ceil(length / n) lost runs; a re-NACK is
// no event and a repair no exposure (both measured too high when repairs queue behind a large sample's runs).
static void test_nacks_count_lost_runs(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    struct tt_PeerAck* ack = &pub.peer_acks[0];
    uint32_t entity = readers[0].sub.endpoint.entity_id;
    ack->loss = (struct tt_LossEstimate) {0, 0};
    hybrid_note_nacks(&pub, reader_id(0), entity, 100, (const uint64_t[]) {1}, 1);
    EXPECT_EQ_U32(0, ack->loss.events); // nothing sent to it yet: nothing it says is counted
    EXPECT_EQ_U32(0, ack->nack_high);
    ack->loss.frames = 1000;
    ack->run_n = 45;
    uint64_t bitmap[2] = {0, 0};
    bitmap[0] = (1ULL << 45U) - 1U; // 100..144: one run of 45
    hybrid_note_nacks(&pub, reader_id(0), entity, 100, bitmap, 2);
    EXPECT_EQ_U32(1, ack->loss.events);
    EXPECT_EQ_U32(1000, ack->loss.frames);
    EXPECT_EQ_U32(144, ack->nack_high);
    bitmap[0] = ~0ULL;       // 200..263: 64 in a row - two runs
    bitmap[1] = 1ULL << 10U; // and 274 alone - a third
    hybrid_note_nacks(&pub, reader_id(0), entity, 200, bitmap, 2);
    EXPECT_EQ_U32(4, ack->loss.events);
    EXPECT_EQ_U32(274, ack->nack_high);
    bitmap[0] = 0x7; // 200..202 again: re-NACKs, at once or a retry interval on
    bitmap[1] = 0;
    hybrid_note_nacks(&pub, reader_id(0), entity, 200, bitmap, 1);
    test_mock_now += 10 * reliable_retry_interval_publisher();
    hybrid_note_nacks(&pub, reader_id(0), entity, 200, bitmap, 1);
    EXPECT_EQ_U32(4, ack->loss.events);
    EXPECT_EQ_U32(274, ack->nack_high);
    // A range that starts among re-NACKs: only its new part counts. MTU (n = 1): each unit of a range is its own event.
    ack->run_n = 1;
    bitmap[0] = 0xFULL << 2U; // 272..275: 272-274 re-NACKs, 275 new
    hybrid_note_nacks(&pub, reader_id(0), entity, 270, bitmap, 1);
    EXPECT_EQ_U32(5, ack->loss.events);
    bitmap[0] = 0xF; // 300..303
    hybrid_note_nacks(&pub, reader_id(0), entity, 300, bitmap, 1);
    EXPECT_EQ_U32(9, ack->loss.events);
}

// Readers of one RELIABLE writer by unicast, one advertising 0: it gets MTU units, the other the cost model's n.
static void test_mixed_reliable_readers_each_get_their_own_n(void) {
    static const uint32_t ceilings[] = {FULL_CEILING, 0};
    init_set(ONE_MB, 2, ceilings, true);
    hybrid_test_forced_p = 0.01;
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_U32(92, sent.to_reader[0]); // 91 runs of up to 8, and the tail
    EXPECT_EQ_U32(723, sent.to_reader[1]);
    EXPECT_EQ_U32(723, pub.peer_acks[0].loss.frames); // exposure for each reader, whatever its n
    EXPECT_EQ_U32(723, pub.peer_acks[1].loss.frames);
    expect_intact(0, 1);
    expect_intact(1, 1);
    hybrid_test_forced_p = -1.0;
}

// KEEP_ALL's bound is in units whatever goes in runs: a reader whose window is narrower than the sample still makes it
// tt_RET_TOO_LARGE, as with MTU fragments.
static void test_keep_all_bounds_are_unchanged(void) {
    init_one(4U * ONE_MB, FULL_CEILING, true);
    pub.keep_all = true;
    pub.large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    pub.peer_acks[0].tracking_words = 32; // 2048 bits: narrower than 4 MB's 2,889 units
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_TOO_LARGE, sim_publish(1));
    EXPECT_EQ_U32(0, sent.runs);
    pub.peer_acks[0].tracking_words = WIDE_WORDS;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
    sim_settle(1, tt_SECOND);
    expect_intact(0, 1);
    EXPECT_EQ_U32(66, sent.runs + sent.mtu);
}

// A path that drops every IP fragment, met after n has reached 45: every run is lost and repaired as MTU units, so
// delivery stays whole and in order, and n falls as the NACKed runs raise p - 45, 17, 11, 8 on the first four samples.
// It does not reach 1 within one sample, as DESIGN.md expects, nor within 25: a lost run is one event on n frames,
// which reads a path that loses every run as a frame loss near 1/n, and the clean history holds the bound down. The
// trajectory is printed; the time-varying estimator (ROADMAP, Later) is where this belongs.
static void test_reliable_through_a_path_that_drops_fragments(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(0);
    uint8_t s = 1;
    for (; s <= 17; s++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, tt_SECOND);
    }
    EXPECT_EQ_U32(45, sent.max_n);
    sim_drop_fragmented = true;
    static const uint32_t expected_n[] = {45, 17, 11, 8};
    printf("test_hybrid_send: fragments dropped after n = 45; n per sample:");
    for (uint32_t k = 0; k < 25; k++, s++) {
        memset(&sent, 0, sizeof(sent));
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, 5 * tt_SECOND);
        printf(" %u", sent.max_n);
        if (k < sizeof(expected_n) / sizeof(expected_n[0])) {
            EXPECT_EQ_U32(expected_n[k], sent.max_n);
        }
        EXPECT_EQ_U32(0, sent.repair_runs);
        EXPECT_TRUE(sent.repair_largest <= tt_CONTROL_MAX_LENGTH);
    }
    printf("\n");
    EXPECT_TRUE(sent.max_n >= 2U); // still not 1 after 25 samples: the finding, kept visible
    expect_intact(0, s - 1);
    EXPECT_TRUE(readers[0].in_order);
}

// --- step 5: measuring c_d and c_f
// ------------------------------------------------------------------------------------

static uint32_t noise_state = 1;

// A deterministic +-5% factor.
static double noise(void) {
    noise_state = (noise_state * 1103515245U) + 12345U;
    return 0.95 + (0.1 * (double)((noise_state >> 8) % 1000U) / 1000.0);
}

// Feeds `fit` calls alternating between runs (17 datagrams, 723 frames) and tails (1, 1) costing c_d and c_f ns, with
// the process's CPU clock moving by `cpu_factor` times what the calls took.
static void feed_fit(struct tt_CostFit* fit, double c_d, double c_f, uint32_t calls, double cpu_factor) {
    for (uint32_t i = 0; i < calls; i++) {
        uint32_t d = (i % 2U) == 0 ? 17U : 1U;
        uint32_t f = (i % 2U) == 0 ? 723U : 1U;
        double t = ((c_d * d) + (c_f * f)) * noise();
        test_mock_process_cpu_ns += (uint64_t)(t * cpu_factor);
        cost_fit_observe(fit, d, f, (uint64_t)t, "test");
    }
}

static bool near(uint32_t value, double expected) {
    return (double)value > expected * 0.97 && (double)value < expected * 1.03;
}

// The fit recovers the costs it is fed, within noise, and is used once its checks pass.
static void test_the_cost_fit_recovers_the_costs(void) {
    test_mock_hybrid_reset();
    test_mock_process_cpu_ns = tt_SECOND;
    struct tt_CostFit fit;
    memset(&fit, 0, sizeof(fit));
    feed_fit(&fit, 5000.0, 2000.0, 400, 1.0);
    EXPECT_TRUE(fit.accepted > 0);
    EXPECT_EQ_U32(2, fit.rejected); // the first two checks: fewer than 64 calls
    EXPECT_TRUE(near(fit.datagram_ns, 5000.0));
    EXPECT_TRUE(near(fit.frame_ns, 2000.0));
    // The process spending 1.9 times what the calls took still agrees; 2.5 times does not, nor 0.4.
    feed_fit(&fit, 5000.0, 2000.0, 100, 1.9);
    EXPECT_TRUE(fit.datagram_ns != 0);
    uint32_t rejected = fit.rejected;
    feed_fit(&fit, 5000.0, 2000.0, 100, 2.5);
    EXPECT_TRUE(fit.rejected > rejected);
    EXPECT_EQ_U32(0, fit.datagram_ns);
    EXPECT_TRUE(fit.cpu_share_pct >= 38U && fit.cpu_share_pct <= 42U); // the prediction is 40% of the CPU time
    EXPECT_TRUE(fit.logged);
    feed_fit(&fit, 5000.0, 2000.0, 100, 1.0);
    EXPECT_TRUE(fit.datagram_ns != 0);
    feed_fit(&fit, 5000.0, 2000.0, 100, 0.4);
    EXPECT_EQ_U32(0, fit.datagram_ns);
    // The first window opens at the CPU clock as it reads at the first call: a fit whose first check already has enough
    // calls (small ones: 2 datagrams of 40 frames, and tails) is used at once.
    memset(&fit, 0, sizeof(fit));
    test_mock_process_cpu_ns = 50ULL * tt_SECOND;
    for (uint32_t i = 0; i < 420; i++) { // 8,610 frames: one check, at 8,192
        uint32_t d = (i % 2U) == 0 ? 2U : 1U;
        uint32_t f = (i % 2U) == 0 ? 40U : 1U;
        double t = ((5000.0 * d) + (2000.0 * f)) * noise();
        test_mock_process_cpu_ns += (uint64_t)t;
        cost_fit_observe(&fit, d, f, (uint64_t)t, "test");
    }
    EXPECT_EQ_U32(1, fit.accepted);
    EXPECT_EQ_U32(0, fit.rejected);
    // A negative cost, however well fitted, is not used.
    memset(&fit, 0, sizeof(fit));
    feed_fit(&fit, -200.0, 2000.0, 400, 1.0);
    EXPECT_EQ_U32(0, fit.accepted);
}

// Rejected: the process's CPU time unreadable; no variation in d / f; fewer than 64 calls; a share it cannot pin down.
static void test_the_cost_fit_is_used_only_when_it_holds(void) {
    test_mock_hybrid_reset();
    struct tt_CostFit fit;
    memset(&fit, 0, sizeof(fit));
    test_mock_process_cpu_ns = 0;
    feed_fit(&fit, 5000.0, 2000.0, 400, 0.0); // the CPU clock reads 0 throughout: no window ever closes
    EXPECT_EQ_U32(0, fit.accepted);
    EXPECT_EQ_U32(0, fit.datagram_ns);
    memset(&fit, 0, sizeof(fit));
    test_mock_process_cpu_ns = tt_SECOND;
    for (uint32_t i = 0; i < 20000; i++) { // every call one MTU datagram: c_d and c_f cannot be told apart
        test_mock_process_cpu_ns += 7000;
        cost_fit_observe(&fit, 1, 1, 7000, "test");
    }
    EXPECT_TRUE(fit.rejected > 0);
    EXPECT_EQ_U32(0, fit.accepted);
    memset(&fit, 0, sizeof(fit));
    for (uint32_t i = 0; i < 30; i++) { // 30 calls of 723 frames: checked at 8192 frames with too few calls
        uint32_t d = (i % 2U) == 0 ? 17U : 16U;
        test_mock_process_cpu_ns += (5000U * d) + (2000U * 723U);
        cost_fit_observe(&fit, d, 723, (5000U * d) + (2000U * 723U), "test");
    }
    EXPECT_TRUE(fit.rejected > 0);
    EXPECT_EQ_U32(0, fit.accepted);
    memset(&fit, 0, sizeof(fit));
    for (uint32_t i = 0; i < 2000; i++) { // times unrelated to d and f
        uint32_t d = (i % 2U) == 0 ? 17U : 1U;
        uint32_t f = (i % 2U) == 0 ? 723U : 1U;
        uint64_t t = (uint64_t)(400000.0 * noise() * noise() * noise() * noise() * noise() * noise());
        t = (i % 3U) == 0 ? t * 20U : t;
        test_mock_process_cpu_ns += t;
        cost_fit_observe(&fit, d, f, t, "test");
    }
    EXPECT_EQ_U32(0, fit.accepted);
    // Positive costs, the CPU time agreeing - but calls of nearly one d / f (17 / 723 and 16 / 700, no tails) cannot
    // tell c_d from c_f: the share's error rejects.
    memset(&fit, 0, sizeof(fit));
    noise_state = 3;
    for (uint32_t i = 0; i < 400; i++) {
        uint32_t d = (i % 2U) == 0 ? 17U : 16U;
        uint32_t f = (i % 2U) == 0 ? 723U : 700U;
        double t = ((5000.0 * d) + (2000.0 * f)) * noise();
        test_mock_process_cpu_ns += (uint64_t)t;
        cost_fit_observe(&fit, d, f, (uint64_t)t, "test");
    }
    {
        double c_d = 0.0;
        double c_f = 0.0;
        cost_fit_solve(&fit, &c_d, &c_f);
        EXPECT_TRUE(c_d > 0.0 && c_f > 0.0);
        EXPECT_TRUE(cost_fit_share_se(&fit, c_d, c_f) >= tt_HYBRID_FIT_MAX_SHARE_SE);
    }
    EXPECT_EQ_U32(0, fit.accepted);
    // The sums halve at the window: calls stay bounded.
    EXPECT_TRUE(fit.calls < 2000.0);
    EXPECT_TRUE(fit.frames < (double)tt_HYBRID_WINDOW_FRAMES);
}

// The costs a writer uses: the platform defaults, its own fit's once accepted, a reader's advertised ones - and n
// follows them: a reader whose frames cost a hundred times its datagrams wants no runs at 1% loss.
static void test_the_writer_uses_the_measured_costs(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    double c_d = 0.0;
    double c_f = 0.0;
    hybrid_costs(&sender, reader_id(0), &c_d, &c_f);
    EXPECT_TRUE(c_d == 11850.0 && c_f == 8800.0);
    sender.hybrid.tx_fit.datagram_ns = 1000;
    sender.hybrid.tx_fit.frame_ns = 3000;
    hybrid_costs(&sender, reader_id(0), &c_d, &c_f);
    EXPECT_TRUE(c_d == 6600.0 && c_f == 6200.0);
    sender.hybrid.peer_cost_datagram_ns[reader_id(0)] = 2000;
    sender.hybrid.peer_cost_frame_ns[reader_id(0)] = 500;
    hybrid_costs(&sender, reader_id(0), &c_d, &c_f);
    EXPECT_TRUE(c_d == 3000.0 && c_f == 3500.0);
    hybrid_test_forced_p = 0.01;
    sender.hybrid.tx_fit.datagram_ns = 100;
    sender.hybrid.tx_fit.frame_ns = 10000;
    sender.hybrid.peer_cost_datagram_ns[reader_id(0)] = 100;
    sender.hybrid.peer_cost_frame_ns[reader_id(0)] = 10000;
    EXPECT_EQ_U32(1, hybrid_reliable_n(&sender, &pub, reader_id(0), 45));
    hybrid_test_forced_p = -1.0;
}

// A reader's accepted receive costs go out in its announce, and the writer records them; a change of less than an
// eighth is not announced again, a larger one is; a peer presumed dead is forgotten.
static void test_the_reader_advertises_its_costs(void) {
    init_one(LARGE_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    sim_start(0);
    uint64_t t = test_mock_now;
    tick_readers(t);
    node->hybrid.rx_fit.datagram_ns = 4000;
    node->hybrid.rx_fit.frame_ns = 1000;
    tick_readers(t += tt_SECOND);
    EXPECT_EQ_U32(4000, sender.hybrid.peer_cost_datagram_ns[reader_id(0)]);
    EXPECT_EQ_U32(1000, sender.hybrid.peer_cost_frame_ns[reader_id(0)]);
    node->hybrid.rx_fit.datagram_ns = 4400; // +10%
    tick_readers(t += tt_SECOND);
    EXPECT_EQ_U32(4000, node->hybrid.advertised_cost_datagram_ns);
    node->hybrid.rx_fit.frame_ns = 1200; // +20%
    tick_readers(t += tt_SECOND);
    EXPECT_EQ_U32(4400, sender.hybrid.peer_cost_datagram_ns[reader_id(0)]);
    EXPECT_EQ_U32(1200, sender.hybrid.peer_cost_frame_ns[reader_id(0)]);
    node->hybrid.rx_fit.datagram_ns = 0; // rejected: "not measured" again
    node->hybrid.rx_fit.frame_ns = 0;
    tick_readers(t += tt_SECOND);
    EXPECT_EQ_U32(0, sender.hybrid.peer_cost_datagram_ns[reader_id(0)]);
    node->hybrid.rx_fit.datagram_ns = 4000;
    node->hybrid.rx_fit.frame_ns = 1000;
    tick_readers(t += tt_SECOND);
    EXPECT_EQ_U32(4000, sender.hybrid.peer_cost_datagram_ns[reader_id(0)]);
    presume_node_dead(&sender, reader_id(0), t);
    EXPECT_EQ_U32(0, sender.hybrid.peer_cost_datagram_ns[reader_id(0)]);
    EXPECT_EQ_U32(0, sender.hybrid.peer_cost_frame_ns[reader_id(0)]);
}

// The live timing: every send call of a sample's runs and its tail is one observation of the sender's fit, every
// fragment-path datagram one of the reader's but the one that completes a sample (its time is the delivery's).
static void test_send_calls_and_received_datagrams_are_timed(void) {
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(0);
    test_mock_clock_step_ns = 1000;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, tt_SECOND);
    EXPECT_TRUE(sender.hybrid.tx_fit.calls == 2.0); // 17 runs in one call, the tail in another
    EXPECT_TRUE(sender.hybrid.tx_fit.frames == 723.0);
    // The reader: every run, not the tail - the datagram that completes a sample carries its delivery.
    EXPECT_TRUE(readers[0].ctx.hybrid.rx_fit.calls == 17.0);
    EXPECT_TRUE(readers[0].ctx.hybrid.rx_fit.frames == 722.0);
    init_one(SMALL_LEN, FULL_CEILING, false);
    sim_start(0);
    test_mock_clock_step_ns = 1000;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    EXPECT_TRUE(sender.hybrid.tx_fit.calls == 2.0);
    EXPECT_TRUE(readers[0].ctx.hybrid.rx_fit.calls == 1.0);
    test_mock_clock_step_ns = 0;
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
    test_the_reader_cap_is_the_designs();
    test_the_reader_reads_the_kernels_limits();
    test_the_reader_counts_gaps_over_exposure();
    test_a_reader_with_the_cap_off_advertises_its_ceiling();
    test_the_loss_bound_is_the_poisson_upper_limit();
    test_a_path_that_drops_fragments_brings_n_to_one();
    test_reassembly_failures_withdraw_the_ceiling();
    test_n_follows_p();
    test_reliable_auto_leaves_mtu_as_clean_frames_accumulate();
    test_reliable_repairs_are_mtu_units_only();
    test_nacks_count_lost_runs();
    test_mixed_reliable_readers_each_get_their_own_n();
    test_keep_all_bounds_are_unchanged();
    test_reliable_through_a_path_that_drops_fragments();
    test_the_cost_fit_recovers_the_costs();
    test_the_cost_fit_is_used_only_when_it_holds();
    test_the_writer_uses_the_measured_costs();
    test_the_reader_advertises_its_costs();
    test_send_calls_and_received_datagrams_are_timed();
    free(sim_queue);
    if (test_result() != 0) {
        return 1;
    }
    printf("test_hybrid_send: all tests passed\n");
    return 0;
}
