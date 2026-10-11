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
    if (sender.large.release != NULL) {
        large_release_publisher(&sender,
                                &pub); // what the last set's writer still holds goes back (sanitize's leak check)
    }
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
// The reader host's IP reassembly, modelled as the kernel's (the PC experiment's reading of it): a fragmented datagram
// that lost a frame holds its arrived frames' truesize for the queue lifetime; while what is held exceeds the
// threshold, every fragmented datagram is dropped whole on arrival. 0 bytes: not modelled (no squeeze).
#define SIM_REASM_HELD 262144U
#define SIM_TRUESIZE 2048U
static uint64_t sim_reasm_threshold;
static uint64_t sim_reasm_lifetime;
static uint64_t sim_reasm_until[SIM_REASM_HELD];
static uint32_t sim_reasm_bytes[SIM_REASM_HELD];
static uint32_t sim_reasm_head;
static uint32_t sim_reasm_tail;
static uint64_t sim_reasm_held;
static uint32_t sim_reasm_refused; // fragmented datagrams dropped for want of memory

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

// The modelled reassembly memory at the current time: what has expired is let go.
static void sim_reasm_expire(void) {
    while (sim_reasm_head != sim_reasm_tail && sim_reasm_until[sim_reasm_head % SIM_REASM_HELD] <= test_mock_now) {
        sim_reasm_held -= sim_reasm_bytes[sim_reasm_head % SIM_REASM_HELD];
        sim_reasm_head++;
    }
}

static bool sim_lost(const struct sim_datagram* d) {
    uint32_t frames = (d->len + 8U + FRAME_PAYLOAD - 1U) / FRAME_PAYLOAD;
    if (sim_drop_fragmented && frames > 1U && d->from == SENDER_ID) {
        sent.frames_dropped += frames;
        return true;
    }
    if (sim_reasm_threshold != 0 && frames > 1U) {
        sim_reasm_expire();
        if (sim_reasm_held > sim_reasm_threshold) {
            sim_reasm_refused++;
            return true;
        }
    }
    uint32_t arrived = 0;
    for (uint32_t i = 0; i < frames; i++) {
        arrived += sim_frame_lost() ? 0U : 1U; // every frame drawn, so the stream of draws does not depend on n
    }
    if (sim_reasm_threshold != 0 && frames > 1U && arrived < frames && arrived > 0 &&
        sim_reasm_tail - sim_reasm_head < SIM_REASM_HELD) {
        sim_reasm_until[sim_reasm_tail % SIM_REASM_HELD] = test_mock_now + sim_reasm_lifetime;
        sim_reasm_bytes[sim_reasm_tail % SIM_REASM_HELD] = arrived * SIM_TRUESIZE;
        sim_reasm_held += (uint64_t)arrived * SIM_TRUESIZE;
        sim_reasm_tail++;
    }
    return arrived < frames;
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
    sim_reasm_threshold = 0;
    sim_reasm_lifetime = 30U * tt_SECOND;
    sim_reasm_head = sim_reasm_tail = 0;
    sim_reasm_held = 0;
    sim_reasm_refused = 0;
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
    // The tail in a send call of its own: 1 MB's 17 runs in one call (up to 64 a call), its tail in a second.
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(0);
    int calls = test_mock_send_batch_call_count;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(4));
    EXPECT_EQ_INT(2, test_mock_send_batch_call_count - calls);
    expect_intact(0, 1);
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

// --- the reader's cap: the control law (DESIGN.md section 8, "The objective and the control law", decision 3) -------

#define ONE_MB_UNITS 723U
#define R_30MBS (ONE_MB_UNITS * 30.0) // 1 MB at 30 Hz, in units a second

// A reader for the control law on its own (n_max 45): its MTU baseline `events` lost of `frames`.
static struct tt_HybridState* law_reader(uint32_t events, uint32_t frames) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    hybrid->mtu_loss = (struct tt_LossEstimate) {events, frames};
    return hybrid;
}

// One decision at `now` over a window of `sent` runs of n, `lost` of them lost, R units a second.
static void law_window(struct tt_HybridState* hybrid, uint64_t now, uint32_t run_n, uint32_t sent, uint32_t lost,
                       double units_per_s) {
    hybrid->window = (struct tt_RunWindow) {sent, lost, sent * run_n};
    hybrid->window_n = run_n;
    hybrid_control(hybrid, now, units_per_s, 45);
}

// The test as pre-defined: frame loss as (1 - p)^n predicts is no collapse; the PC's collapse (30 MB/s, 0.1%, n = 8:
// q 0.46) is one; against L0 at p_mtu's upper bound (0.136%: L0 1.08% at n = 8, 10.8 runs of 1,000) 25 lost is
// significant and 20 is not; a significant excess under 2 L0 is not a collapse; and nothing is decided without enough
// runs, losses, or an MTU baseline. A clean window loses no more than R L0.
static void test_the_collapse_test_is_the_designs(void) {
    struct tt_LossEstimate mtu = {30, 30000}; // p_mtu 0.1%
    struct tt_RunWindow consistent = {1000, 8, 8000};
    EXPECT_TRUE(!hybrid_collapsed(&consistent, &mtu));
    EXPECT_TRUE(hybrid_clean(&consistent, &mtu));
    struct tt_RunWindow pc = {1000, 540, 8000};
    EXPECT_TRUE(hybrid_collapsed(&pc, &mtu));
    EXPECT_TRUE(!hybrid_clean(&pc, &mtu));
    struct tt_RunWindow excess = {1000, 25, 8000};
    EXPECT_TRUE(hybrid_collapsed(&excess, &mtu));
    EXPECT_TRUE(!hybrid_clean(&excess, &mtu));
    struct tt_RunWindow within = {1000, 20, 8000};
    EXPECT_TRUE(!hybrid_collapsed(&within, &mtu));
    EXPECT_TRUE(!hybrid_clean(&within, &mtu)); // neither: the window gathers more
    // Significant (z near 6) but only 1.2x L0 on a vast exposure: not far below (1 - p)^n.
    struct tt_LossEstimate vast = {10000, 10000000};
    struct tt_RunWindow slight = {125000, 1200, 1000000};
    EXPECT_TRUE(!hybrid_collapsed(&slight, &vast));
    struct tt_RunWindow few_runs = {15, 15, 120};
    EXPECT_TRUE(!hybrid_collapsed(&few_runs, &mtu));
    EXPECT_TRUE(!hybrid_clean(&few_runs, &mtu));
    struct tt_RunWindow few_lost = {100, 3, 800};
    struct tt_LossEstimate none_lost = {0, 30000};
    EXPECT_TRUE(!hybrid_collapsed(&few_lost, &none_lost));
    struct tt_LossEstimate no_baseline = {0, 0};
    EXPECT_TRUE(!hybrid_collapsed(&pc, &no_baseline));
    // A fragment path refusing every run on a young baseline (200 tails, none lost: p_hi 1.5%, L0 49% at n = 45) is a
    // collapse all the same - the run-level test does not need the many MTU units a per-frame comparison would.
    struct tt_LossEstimate young = {0, 200};
    struct tt_RunWindow refused = {510, 510, 510U * 45U};
    EXPECT_TRUE(hybrid_collapsed(&refused, &young));
    // Twice L0 on that baseline (L0 11.4% at n = 8) but on 20 runs: not significant (z 1.6).
    struct tt_RunWindow small = {20, 5, 160};
    EXPECT_TRUE(!hybrid_collapsed(&small, &young));
}

// A reader starts at two units and doubles per clean window (slow start) to n_max. A collapse at 45 with the PC's
// survival there (30 MB/s, 0.1%: q 0.08 against 0.96) remembers the collapse point the survival shows, 45 q / q0 = 3.8
// -> 4, not the 45 the growth overshot to, and drops the cap at once to drop(4) = 3. A mild collapse (q 0.62 at 45:
// the PC's 0.01%) remembers 30 and drops to 0.7 x 30 = 21.
static void test_slow_start_and_the_drop_on_a_collapse(void) {
    struct tt_HybridState* hybrid = law_reader(30, 30000);
    EXPECT_EQ_U32(2, hybrid->cap);
    EXPECT_EQ_U32(28U + (2U * 1454U), hybrid->advertised);
    EXPECT_EQ_U32(30 * tt_SECOND, hybrid->lifetime_ns);
    uint64_t t = 10 * tt_SECOND;
    static const uint32_t caps[] = {4, 8, 16, 32, 45, 45};
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        law_window(hybrid, t += tt_SECOND, hybrid->cap, 100, 0, R_30MBS);
        EXPECT_EQ_U32(caps[i], hybrid->cap);
    }
    law_window(hybrid, t += tt_SECOND, 45, 1000, 920, R_30MBS);
    EXPECT_EQ_U64(1, hybrid->collapses);
    EXPECT_EQ_U32(4, hybrid->n_fail);
    EXPECT_EQ_U32(3, hybrid->cap);
    EXPECT_EQ_U32(0, hybrid->window.sent); // decided: the window starts again
    // The drop rule on its own: 0.7 n, n - 1 when that is less of a drop.
    EXPECT_EQ_U32(21, hybrid_drop(30));
    EXPECT_EQ_U32(3, hybrid_drop(4));
    EXPECT_EQ_U32(2, hybrid_drop(3));
    EXPECT_EQ_U32(1, hybrid_drop(2));
    hybrid = law_reader(30, 30000);
    hybrid->cap = 45;
    law_window(hybrid, t, 45, 1000, 380, R_30MBS);
    EXPECT_EQ_U32(30, hybrid->n_fail);
    EXPECT_EQ_U32(21, hybrid->cap);
}

// After a collapse (ceiling 30, cap 21) the reader holds for tau - windows then are about the full memory draining,
// not the cap, and are dropped whatever they show - then grows fast, half the way a tick, to the plateau 29, and
// stays there: no probe before 2 tau. At 2 tau it probes above the ceiling (1.25 x 30 -> 38) and judges the probe over
// a whole tau; a collapse during it keeps the ceiling, drops again, and doubles the wait.
static void test_the_hold_and_the_timed_probe(void) {
    struct tt_HybridState* hybrid = law_reader(30, 30000);
    hybrid->cap = 45;
    uint64_t t0 = 100 * tt_SECOND;
    law_window(hybrid, t0, 45, 1000, 380, R_30MBS);
    EXPECT_EQ_U32(21, hybrid->cap);
    EXPECT_EQ_U64(t0 + (30 * tt_SECOND), hybrid->hold_until_ns);
    EXPECT_EQ_U64(t0 + (60 * tt_SECOND), hybrid->probe_at_ns);
    uint64_t t = t0;
    for (int i = 0; i < 29; i++) {
        law_window(hybrid, t += tt_SECOND, 21, 1000, 900, R_30MBS); // the memory still full: not judged
    }
    EXPECT_EQ_U64(1, hybrid->collapses);
    EXPECT_EQ_U32(21, hybrid->cap);
    hybrid_control(hybrid, t0 + (30 * tt_SECOND), R_30MBS, 45); // and not kept: the hold's last window is gone
    EXPECT_EQ_U64(1, hybrid->collapses);
    t = t0 + (29 * tt_SECOND);
    static const uint32_t caps[] = {25, 27, 28, 29, 29};
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        law_window(hybrid, t += tt_SECOND, hybrid->cap, 100, 0, R_30MBS);
        EXPECT_EQ_U32(caps[i], hybrid->cap);
    }
    while (t < t0 + (59 * tt_SECOND)) {
        law_window(hybrid, t += tt_SECOND, hybrid->cap, 100, 0, R_30MBS);
        EXPECT_EQ_U32(29, hybrid->cap); // no probe before 2 tau
    }
    EXPECT_EQ_U64(0, hybrid->probes);
    law_window(hybrid, t = t0 + (60 * tt_SECOND), 29, 100, 0, R_30MBS);
    EXPECT_EQ_U64(1, hybrid->probes);
    EXPECT_EQ_U32(38, hybrid->cap);
    law_window(hybrid, t += 10 * tt_SECOND, 38, 100, 0, R_30MBS); // clean, but not a whole tau yet
    EXPECT_EQ_U32(38, hybrid->cap);
    EXPECT_EQ_U32(30, hybrid->n_fail);
    law_window(hybrid, t += tt_SECOND, 38, 1000, 150, R_30MBS); // collapses: survival says 34, the ceiling stands
    EXPECT_EQ_U64(1, hybrid->probe_failures);
    EXPECT_EQ_U32(30, hybrid->n_fail);
    EXPECT_EQ_U32(21, hybrid->cap);
    EXPECT_EQ_U32(1, hybrid->backoff);
    EXPECT_EQ_U64(t + (120 * tt_SECOND), hybrid->probe_at_ns); // 2 tau, doubled
}

// A probe that stays clean for a whole tau moves the ceiling up a step (1.25 x the probed n); past n_max there is no
// ceiling left, and the cap grows as at the start.
static void test_a_probe_that_holds_moves_the_ceiling_up(void) {
    struct tt_HybridState* hybrid = law_reader(30, 30000);
    hybrid->cap = 16;
    uint64_t t0 = 100 * tt_SECOND;
    law_window(hybrid, t0, 16, 1000, 390, R_30MBS); // q 0.61 at 16: ceiling 10, cap 7
    EXPECT_EQ_U32(10, hybrid->n_fail);
    EXPECT_EQ_U32(7, hybrid->cap);
    uint64_t t = t0 + (30 * tt_SECOND);
    while (t < t0 + (60 * tt_SECOND)) {
        law_window(hybrid, t += tt_SECOND, hybrid->cap, 100, 0, R_30MBS);
    }
    EXPECT_EQ_U32(13, hybrid->cap); // the probe: 1.25 x 10 -> 13
    while (t < t0 + (90 * tt_SECOND)) {
        law_window(hybrid, t += tt_SECOND, hybrid->cap, 100, 0, R_30MBS);
    }
    EXPECT_EQ_U32(17, hybrid->n_fail); // held for tau: the ceiling moves up to 1.25 x 13 -> 17
    EXPECT_EQ_U32(0, hybrid->backoff);
    law_window(hybrid, t += tt_SECOND, hybrid->cap, 100, 0, R_30MBS);
    EXPECT_EQ_U32(15, hybrid->cap); // and the cap grows towards its plateau, 16
    hybrid->n_fail = 40;
    hybrid->cap = 39;
    hybrid->probe_at_ns = 0;
    law_window(hybrid, t += tt_SECOND, 39, 100, 0, R_30MBS);
    EXPECT_EQ_U32(45, hybrid->cap); // a probe at 1.25 x 40 is n_max
    law_window(hybrid, t += 31 * tt_SECOND, 45, 100, 0, R_30MBS);
    EXPECT_EQ_U32(0, hybrid->n_fail); // no ceiling left
}

// The ceiling moves with R p (decision 3c): with R halved for three ticks in a row the remembered ceiling doubles and a
// probe is allowed at once; two such ticks and a normal one do nothing; R doubled for three ticks halves it and the cap
// with it. p_mtu here rests on 30 events, so it is read as measured.
static void test_the_ceiling_scales_with_r_p(void) {
    struct tt_HybridState* hybrid = law_reader(30, 30000);
    hybrid->cap = 16;
    uint64_t t = 100 * tt_SECOND;
    law_window(hybrid, t, 16, 1000, 390, R_30MBS); // ceiling 10, cap 7, anchored at R
    EXPECT_EQ_U32(10, hybrid->n_fail);
    t += 31 * tt_SECOND; // past the hold
    law_window(hybrid, t += tt_SECOND, 7, 10, 0, R_30MBS / 2.0);
    law_window(hybrid, t += tt_SECOND, 7, 10, 0, R_30MBS / 2.0);
    law_window(hybrid, t += tt_SECOND, 7, 10, 0, R_30MBS); // the streak broken
    law_window(hybrid, t += tt_SECOND, 7, 10, 0, R_30MBS / 2.0);
    law_window(hybrid, t += tt_SECOND, 7, 10, 0, R_30MBS / 2.0);
    EXPECT_EQ_U64(0, hybrid->rescales);
    EXPECT_EQ_U32(10, hybrid->n_fail);
    law_window(hybrid, t += tt_SECOND, 7, 10, 0, R_30MBS / 2.0); // the third in a row
    EXPECT_EQ_U64(1, hybrid->rescales);
    EXPECT_EQ_U32(20, hybrid->n_fail);
    EXPECT_EQ_U64(t, hybrid->probe_at_ns); // a probe allowed at once
    law_window(hybrid, t += tt_SECOND, 7, 100, 0, R_30MBS / 2.0);
    EXPECT_EQ_U32(13, hybrid->cap); // and the cap grows towards the new plateau, 19
    for (int i = 0; i < 3; i++) {
        law_window(hybrid, t += tt_SECOND, 13, 10, 0, R_30MBS); // R back up: R p twice the new anchor
    }
    EXPECT_EQ_U64(2, hybrid->rescales);
    EXPECT_EQ_U32(10, hybrid->n_fail);
    EXPECT_EQ_U32(9, hybrid->cap); // held under the plateau at once
}

// The law runs once per announce interval: a tick sooner decides nothing, the next one a full interval on does.
static void test_the_law_runs_once_an_interval(void) {
    struct tt_HybridState* hybrid = law_reader(30, 30000);
    struct tt_Context* node = &readers[0].ctx;
    hybrid_reader_tick(node, 10 * tt_SECOND);
    hybrid->window = (struct tt_RunWindow) {100, 0, 200};
    hybrid->window_n = 2;
    hybrid_reader_tick(node, (10 * tt_SECOND) + (tt_SECOND / 2U));
    EXPECT_EQ_U32(2, hybrid->cap);
    hybrid_reader_tick(node, 11 * tt_SECOND);
    EXPECT_EQ_U32(4, hybrid->cap);
}

// tau = min(30 s, 65536 / D_s), D_s a source's runs a second over every writer of it, the longest over the sources;
// unchanged by a tick without runs.
static void test_tau_from_the_writers_run_rates(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    hybrid->rx_writers[0] = (struct tt_HybridRxWriter) {.entity_id = 1, .sample = 5, .tick_runs = 4096, .source = 7};
    hybrid->rx_writers[1] = (struct tt_HybridRxWriter) {.entity_id = 2, .sample = 5, .tick_runs = 4096, .source = 7};
    hybrid_measure_lifetime(hybrid, tt_SECOND);
    EXPECT_EQ_U64(8 * tt_SECOND, hybrid->lifetime_ns); // 8,192 a second from source 7: its IP ID wraps in 8 s
    hybrid->rx_writers[2] = (struct tt_HybridRxWriter) {.entity_id = 3, .sample = 5, .tick_runs = 100, .source = 9};
    hybrid->rx_writers[0].tick_runs = 4096;
    hybrid->rx_writers[1].tick_runs = 4096;
    hybrid_measure_lifetime(hybrid, tt_SECOND);
    EXPECT_EQ_U64(30 * tt_SECOND, hybrid->lifetime_ns); // source 9 sends 100 a second: its queues live the full 30 s
    EXPECT_EQ_U32(0, hybrid->rx_writers[0].tick_runs);
    hybrid->lifetime_ns = 5 * tt_SECOND;
    hybrid_measure_lifetime(hybrid, tt_SECOND);
    EXPECT_EQ_U64(5 * tt_SECOND, hybrid->lifetime_ns); // no runs: unchanged
}

// What the reader rebuilds of what was sent (DESIGN.md section 8, "What the reader measures"): a run says the writer's
// n; a gap in the body is lost runs of that n (one event and one lost run each, on their frames), or lost MTU units
// where the writer sends MTU units; a missing tail is a lost MTU unit; a repair and an older sample's unit count for
// nothing; a writer first met mid-sample has no gap behind it. R counts every unit sent, arrived or not.
static void test_the_reader_rebuilds_what_was_sent(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    struct tt_HybridState* hybrid = &node->hybrid;
    hybrid_rx_note(node, SENDER_ID, 7, 100, 0, 100, 45, true); // units 0-44: a full run of 45
    EXPECT_EQ_U32(45, hybrid->rx_loss.frames);
    EXPECT_EQ_U32(1, hybrid->window.sent);
    EXPECT_EQ_U32(45, hybrid->window_n); // the largest run in the window: the n a collapse is read at
    EXPECT_EQ_U32(0, hybrid->window.lost);
    EXPECT_EQ_U32(45, hybrid->rx_writers[0].layout_n);
    hybrid_rx_note(node, SENDER_ID, 7, 100, 90, 100, 9, true); // 45-89 lost: one run of 45
    EXPECT_EQ_U32(99, hybrid->rx_loss.frames);
    EXPECT_EQ_U32(1, hybrid->rx_loss.events);
    EXPECT_EQ_U32(3, hybrid->window.sent);
    EXPECT_EQ_U32(1, hybrid->window.lost);
    EXPECT_EQ_U32(99, hybrid->window.frames);
    EXPECT_EQ_U32(45, hybrid->rx_writers[0].layout_n);          // the short last run does not say n
    hybrid_rx_note(node, SENDER_ID, 7, 100, 50, 100, 1, false); // a repair
    EXPECT_EQ_U32(99, hybrid->rx_loss.frames);
    EXPECT_EQ_U32(0, hybrid->mtu_loss.frames);
    hybrid_rx_note(node, SENDER_ID, 7, 200, 0, 100, 45, true); // the next sample: the last one's tail never came
    EXPECT_EQ_U32(1, hybrid->mtu_loss.frames);
    EXPECT_EQ_U32(1, hybrid->mtu_loss.events);
    EXPECT_EQ_U32(2, hybrid->rx_loss.events);
    hybrid_rx_note(node, SENDER_ID, 7, 200, 99, 100, 1, false); // its tail: runs 45-89 and 90-98 lost
    EXPECT_EQ_U32(2, hybrid->mtu_loss.frames);
    EXPECT_EQ_U32(1, hybrid->mtu_loss.events);
    EXPECT_EQ_U32(6, hybrid->window.sent);
    EXPECT_EQ_U32(3, hybrid->window.lost);
    EXPECT_EQ_U32(3, hybrid->rx_writers[0].runs.lost); // the writer's own q
    EXPECT_EQ_U32(6, hybrid->rx_writers[0].runs.sent);
    EXPECT_EQ_U32(6, hybrid->rx_writers[0].tick_runs);
    hybrid_rx_note(node, SENDER_ID, 8, 50, 3, 10, 1, false); // another writer, first met mid-sample: no gap
    EXPECT_EQ_U32(1, hybrid->rx_writers[1].layout_n);        // an MTU unit in the body: it sends MTU units
    hybrid_rx_note(node, SENDER_ID, 8, 50, 6, 10, 1, false); // 4 and 5 lost, as MTU units
    EXPECT_EQ_U32(6, hybrid->mtu_loss.frames);
    EXPECT_EQ_U32(3, hybrid->mtu_loss.events);
    hybrid_rx_note(node, SENDER_ID, 7, 100, 98, 100, 1, false); // an older sample's unit
    EXPECT_EQ_U32(6, hybrid->mtu_loss.frames);
    EXPECT_EQ_U32(200 + 4, hybrid->tick_units); // R: every unit sent to it, arrived or not (writer 8 from unit 3)
    // A writer whose layout is not known yet: a gap is one event on its units, in p only.
    hybrid_rx_note(node, SENDER_ID, 9, 300, 0, 10, 1, false); // unit 0 in MTU: known as MTU
    hybrid->rx_writers[2].layout_n = 0;
    uint32_t events = hybrid->rx_loss.events;
    hybrid_rx_note(node, SENDER_ID, 9, 300, 9, 10, 1, false); // 1-8 lost, sent as what is not known
    EXPECT_EQ_U32(events + 1U, hybrid->rx_loss.events);
    EXPECT_EQ_U32(3, hybrid->mtu_loss.events);
    // Each window halves at 2^20 frames.
    struct tt_LossEstimate* loss = &hybrid->rx_loss;
    loss->frames = (uint32_t)tt_HYBRID_WINDOW_FRAMES - 1U;
    loss->events = 40;
    loss_estimate_observe(loss, 1, 0);
    EXPECT_EQ_U32((uint32_t)tt_HYBRID_WINDOW_FRAMES / 2U, loss->frames);
    EXPECT_EQ_U32(20, loss->events);
}

// Rule (e) on the reader's side needs frame loss to be no excuse: a writer none of whose runs ever arrived loses the
// one run of a 3-unit sample with its tail through - at p_mtu near 50% that is what frame loss does (L0 0.79): nothing;
// at p_mtu near 0, the same sample twice is (L0 2e-6 a sample): one unit at once.
static void test_the_path_rule_needs_frame_loss_to_be_no_excuse(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    struct tt_HybridState* hybrid = &node->hybrid;
    hybrid->mtu_loss = (struct tt_LossEstimate) {500, 1000};
    for (uint32_t s = 0; s < 8; s++) {
        hybrid_rx_note(node, SENDER_ID, 7, 100U + (3U * s), 2, 3, 1, false); // the tail of a sample whose run was lost
    }
    EXPECT_EQ_U64(0, hybrid->path_failures);
    EXPECT_EQ_U32(2, hybrid->cap);
    hybrid->mtu_loss = (struct tt_LossEstimate) {0, 3000000};
    hybrid_rx_note(node, SENDER_ID, 8, 200, 2, 3, 1, false); // another writer, met at a tail: no gap yet
    hybrid_rx_note(node, SENDER_ID, 8, 203, 2, 3, 1, false);
    EXPECT_EQ_U64(0, hybrid->path_failures); // once is still 2e-6
    hybrid_rx_note(node, SENDER_ID, 8, 206, 2, 3, 1, false);
    EXPECT_EQ_U64(1, hybrid->path_failures);
    EXPECT_EQ_U32(1, hybrid->cap);
    EXPECT_EQ_U32(2, hybrid->path_cap);
    // At its time the re-probe restores the cap the path had (say 16 had it grown), not the law's next step from one.
    hybrid->path_cap = 16;
    hybrid->tick_ns = hybrid->path_until_ns - tt_SECOND;
    hybrid_reader_tick(node, hybrid->path_until_ns);
    EXPECT_EQ_U32(16, hybrid->cap);
    EXPECT_TRUE(hybrid->path_probe);
}

// p on the wire (tickle.h's loss_p): rounded up, never below what was measured and within 1/256 of it above; 0 for no
// estimate and anything from 1/2, read back as 1.
static void test_the_loss_code_rounds_up(void) {
    static const double ps[] = {1e-7, 1e-5, 0.0001, 0.00123, 0.01, 0.25, 0.49};
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
        uint16_t code = hybrid_loss_code(ps[i]);
        double back = hybrid_loss_decode(code);
        EXPECT_TRUE(code != 0);
        EXPECT_TRUE(back >= ps[i] && back <= ps[i] * (1.0 + (1.0 / 256.0)));
    }
    EXPECT_EQ_U32(0, hybrid_loss_code(0.5));
    EXPECT_EQ_U32(0, hybrid_loss_code(0.4999)); // rounded up to 1/2: read as 1
    EXPECT_EQ_U32(0, hybrid_loss_code(1.0));
    EXPECT_EQ_U32(0, hybrid_loss_code(0.0));
    EXPECT_TRUE(hybrid_loss_decode(0) == 1.0);
    EXPECT_EQ_U32((1U << 8U) | 0U, hybrid_loss_code(0.25)); // exactly a power of two: no rounding up past it
    // Re-announced on a factor of 2 (an exponent), or to and from none.
    EXPECT_TRUE(!hybrid_loss_moved(hybrid_loss_code(0.001), hybrid_loss_code(0.0015)));
    EXPECT_TRUE(hybrid_loss_moved(hybrid_loss_code(0.001), hybrid_loss_code(0.0021)));
    EXPECT_TRUE(hybrid_loss_moved(hybrid_loss_code(0.001), 0));
    EXPECT_TRUE(hybrid_loss_moved(0, hybrid_loss_code(0.001)));
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
// whatever its windows show, and the control law never runs on it.
static void test_a_reader_with_the_cap_off_advertises_its_ceiling(void) {
    _tt_CONFIG.reassembly_cap_off = 1;
    init_one(LARGE_LEN, FULL_CEILING, false);
    _tt_CONFIG.reassembly_cap_off = 0;
    struct tt_Context* node = &readers[0].ctx;
    EXPECT_TRUE(node->hybrid.cap_off);
    EXPECT_EQ_U32(45, node->hybrid.cap);
    EXPECT_EQ_U32(FULL_CEILING, node->hybrid.advertised);
    sim_start(0);
    uint64_t t = test_mock_now;
    tick_readers(t);
    node->hybrid.mtu_loss = (struct tt_LossEstimate) {0, 30000};
    node->hybrid.window = (struct tt_RunWindow) {1000, 1000, 45000}; // a collapse, were it judged
    node->hybrid.window_n = 45;
    tick_readers(t += tt_SECOND);
    EXPECT_EQ_U32(FULL_CEILING, node->hybrid.advertised);
    EXPECT_EQ_U64(0, node->hybrid.collapses);
    init_one(LARGE_LEN, FULL_CEILING, false);
    EXPECT_TRUE(!readers[0].ctx.hybrid.cap_off); // the default
    EXPECT_EQ_U32(2, readers[0].ctx.hybrid.cap);
}

// Publishes for `seconds` at `hz` - `hz` samples a second, evenly spaced - and ticks the readers on every second
// boundary, so that each tick sees exactly one second of traffic. Returns the largest n the writer sent.
static uint32_t sim_stream(uint32_t seconds, uint32_t hz, uint8_t* seed) {
    uint32_t largest = 0;
    for (uint32_t second = 0; second < seconds; second++) {
        uint64_t start = test_mock_now;
        for (uint32_t s = 0; s < hz; s++) {
            memset(seen_unit, 0, sizeof(seen_unit)); // seq_nos pass SEEN_MAX: a unit is new to its own sample
            test_mock_now = start + ((uint64_t)s * (tt_SECOND / hz));
            sent.max_n = 0;
            EXPECT_EQ_INT(tt_RET_OK, sim_publish((*seed)++));
            largest = sent.max_n > largest ? sent.max_n : largest;
            for (int i = 0; i < READERS; i++) { // what a reader sent at once (an announce) goes at its next flush
                if (readers[i].present) {
                    sim_run_due(&readers[i].ctx);
                }
            }
        }
        tick_readers(start + tt_SECOND);
        test_mock_now = start + tt_SECOND;
    }
    return largest;
}

// Plain frame loss never moves the cap (decision 3d): best effort 16 KB samples (12 units) at 100 Hz through 1% frame
// loss for 30 s - a run of 11 is lost a tenth of the time, as (1 - p)^n says - grow the cap from 2 to n_max with no
// collapse, and every sample that arrives is whole.
static void test_frame_loss_never_moves_the_cap(void) {
    init_one(16384, FULL_CEILING, false);
    sim_start(10000);
    tick_readers(test_mock_now);
    uint8_t seed = 1;
    (void)sim_stream(30, 100, &seed);
    struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    EXPECT_EQ_U64(0, hybrid->collapses);
    EXPECT_EQ_U32(45, hybrid->cap);
    EXPECT_EQ_U32(11, sent.max_n); // the writer follows: one run of 11 and the tail
    EXPECT_TRUE(hybrid->rx_writers[0].runs.lost > 0);
    EXPECT_EQ_INT(0, readers[0].torn);
    EXPECT_TRUE(readers[0].delivered > 2400 && readers[0].delivered < 3000);
}

// Best effort through a path that drops every IP fragment from the start (the tc filter; rule (e) on the reader's
// side): no run of the writer's ever gets through, only the tails. Within a few samples the reader judges the path,
// advertises one unit at once, the writer follows and samples arrive again. After 2 tau the cap it had comes back as a
// re-probe: the filter still there, the next sample fails it at once and the wait doubles; the filter gone, runs arrive
// and it stays. A collapse it is not: the control law's ceiling is untouched.
static void test_a_path_that_drops_fragments_brings_n_to_one(void) {
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(0);
    sim_drop_fragmented = true;
    tick_readers(test_mock_now);
    struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    uint8_t seed = 1;
    (void)sim_stream(1, 30, &seed);
    EXPECT_EQ_U64(1, hybrid->path_failures);
    EXPECT_EQ_U32(1, hybrid->cap);
    EXPECT_EQ_U32(1, hybrid_units_within(sender.peer_ip_reassembly_max[reader_id(0)], true)); // announced at once
    EXPECT_TRUE(readers[0].delivered >= 20 && readers[0].delivered < 30);                     // all but the first few
    EXPECT_EQ_U64(0, hybrid->collapses);
    int delivered = readers[0].delivered;
    uint32_t runs = sent.runs;
    (void)sim_stream(58, 30, &seed);
    EXPECT_EQ_U32(runs, sent.runs);                             // MTU units only until the re-probe
    EXPECT_EQ_INT(delivered + (58 * 30), readers[0].delivered); // every sample, as MTU units
    (void)sim_stream(3, 30, &seed);                             // 2 tau: the re-probe, failed by its first sample
    EXPECT_EQ_U64(2, hybrid->path_failures);
    EXPECT_EQ_U32(1, hybrid->cap);
    EXPECT_EQ_U32(1, hybrid->path_backoff);
    sim_drop_fragmented = false;
    (void)sim_stream(122, 30, &seed); // 4 tau: the next re-probe gets through
    EXPECT_EQ_U64(2, hybrid->path_failures);
    EXPECT_TRUE(hybrid->cap >= 2U);
    EXPECT_EQ_U32(0, hybrid->path_backoff);
    EXPECT_TRUE(sent.runs > runs);
    EXPECT_EQ_U64(0, hybrid->collapses);
    EXPECT_EQ_INT(0, readers[0].torn);
}

// The reassembly squeeze, end to end (decision 3): best effort 1 MB at 30 Hz through 0.1% frame loss, the reader host's
// reassembly modelled as the kernel's (4 MB, 30 s). The cap grows from 2 by slow start; the incomplete reassemblies of
// the runs frame loss breaks fill the memory and every run is refused - the collapse is detected (after the refusals
// began, never before), the cap drops at once to drop(n_fail), and the writer follows at its next sample. No probe
// before 2 tau; at 2 tau one probe. Then the rate halves, and within the scaling's three ticks the remembered ceiling
// doubles and a probe is allowed at once.
static void test_a_reassembly_squeeze_end_to_end(void) {
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(1000);
    sim_reasm_threshold = 4194304U;
    sim_reasm_lifetime = 30U * tt_SECOND;
    struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    tick_readers(test_mock_now);
    uint8_t seed = 1;
    uint32_t seconds = 0;
    uint32_t largest = 0;
    uint32_t refused_before = 0;
    uint64_t generation = 0;
    while (hybrid->collapses == 0 && seconds < 40) {
        refused_before = sim_reasm_refused;
        generation = readers[0].ctx.last_modified;
        uint32_t n = sim_stream(1, 30, &seed);
        largest = n > largest ? n : largest;
        seconds++;
    }
    EXPECT_EQ_U64(1, hybrid->collapses);
    EXPECT_TRUE(seconds < 30);
    EXPECT_TRUE(refused_before > 0);         // the memory was full before the collapse was called: not frame loss
    EXPECT_EQ_U64(0, hybrid->path_failures); // runs had got through: the memory's, not the path's
    EXPECT_EQ_U32(45, largest);              // slow start reached n_max first
    uint64_t collapsed_at = test_mock_now;
    uint32_t n_fail = hybrid->n_fail;
    EXPECT_TRUE(n_fail >= 2 && n_fail < 45);
    EXPECT_EQ_U32(hybrid_drop(n_fail), hybrid->cap);
    EXPECT_TRUE(readers[0].ctx.last_modified != generation); // a new generation: a peer that missed it pulls it
    EXPECT_EQ_U32(hybrid->cap, hybrid_units_within(sender.peer_ip_reassembly_max[reader_id(0)], true));
    uint32_t after = sim_stream(1, 30, &seed);
    EXPECT_TRUE(after <= hybrid->cap); // the writer follows at once
    // No probe before 2 tau: the cap stays at or under the plateau, n_fail - 1.
    uint32_t plateau = n_fail - 1U > 1U ? n_fail - 1U : 1U;
    while (test_mock_now < collapsed_at + (59 * tt_SECOND)) {
        (void)sim_stream(1, 30, &seed);
        EXPECT_TRUE(hybrid->cap <= plateau);
    }
    EXPECT_EQ_U64(0, hybrid->probes);
    (void)sim_stream(2, 30, &seed);
    EXPECT_EQ_U64(1, hybrid->probes);
    // The rate halves: R p halves, and within three ticks the ceiling doubles and a probe is allowed at once.
    uint64_t rescales = hybrid->rescales;
    uint32_t ceiling = hybrid->n_fail;
    (void)sim_stream(4, 15, &seed);
    EXPECT_EQ_U64(rescales + 1U, hybrid->rescales);
    EXPECT_TRUE(hybrid->n_fail == 0 || hybrid->n_fail == 2U * ceiling);
    EXPECT_EQ_INT(0, readers[0].torn);
}

// --- step 4: RELIABLE runs with MTU repairs -------------------------------------------------------------------------

// RELIABLE AUTO's n for a node, as a send asks for it.
static uint32_t reliable_n(uint8_t context_id, uint32_t n_max) {
    bool one_run = false;
    return hybrid_reliable_n(&sender, &pub, context_id, n_max, &one_run);
}

// n follows p (forced through the test seam), by the objective - DESIGN.md's table (45, 13, 4, 2, 1 at 0, 0.01, 0.1,
// 0.5 and 1%) - and is never above what the reader allows.
static void test_n_follows_p(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    static const double ps[] = {0.0, 0.0001, 0.001, 0.005, 0.01, 0.05, 0.2, 0.5, 1.0};
    static const uint32_t ns[] = {45, 13, 4, 2, 1, 1, 1, 1, 1};
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
        hybrid_test_forced_p = ps[i];
        EXPECT_EQ_U32(ns[i], reliable_n(reader_id(0), 45));
        EXPECT_EQ_U32(ns[i] < 2U ? ns[i] : 2U, reliable_n(reader_id(0), 2));
    }
    // A node with no reader of this Publisher: 1.
    hybrid_test_forced_p = 0.0;
    EXPECT_EQ_U32(1, reliable_n(77, 45));
    // Unforced, the p is the one the reader node reported in its announce: none yet is 1, 0.001% is 42, 0.02% is 9.
    hybrid_test_forced_p = -1.0;
    EXPECT_EQ_U32(1, reliable_n(reader_id(0), 45));
    sender.hybrid.peer_loss_p[reader_id(0)] = hybrid_loss_code(0.00001);
    EXPECT_EQ_U32(42, reliable_n(reader_id(0), 45));
    sender.hybrid.peer_loss_p[reader_id(0)] = hybrid_loss_code(0.0002);
    EXPECT_EQ_U32(9, reliable_n(reader_id(0), 45));
    sender.hybrid.peer_loss_p[reader_id(0)] = 0;
    // And the sample goes in runs of that n: 0.1% -> 4.
    hybrid_test_forced_p = 0.001;
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_U32(4, sent.max_n);
    EXPECT_EQ_U32(181, sent.runs); // 722 units in runs of 4: 180 and one of 2
    expect_intact(0, 1);
    hybrid_test_forced_p = -1.0;
}

// AUTO from nothing measured, end to end: the first 1 MB samples go as MTU units until the reader's first tick
// (sample 30), whose announce carries the p it measured (its posterior mean, 1 / N at no loss); the writer's n follows
// each re-announce (a factor of 2 in p) under the reader's cap, which doubles per clean tick from 2 (slow start), and
// reaches 45 once the reader has seen about 115,000 clean frames, ~6 s at 30 Hz.
static void test_reliable_auto_leaves_mtu_as_clean_frames_accumulate(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(0);
    uint64_t t = test_mock_now;
    tick_readers(t);
    uint32_t first_runs_at = 0;
    uint32_t last_n = 0;
    for (uint32_t s = 1; s <= 300; s++) {
        memset(&sent, 0, sizeof(sent));
        memset(seen_unit, 0, sizeof(seen_unit)); // seq_nos pass SEEN_MAX here: a unit is new to its own sample
        test_mock_now = t + ((uint64_t)s * 34U * tt_MILLISECOND); // 30 a tick, just over a second
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)s));
        sim_settle((int)s, tt_SECOND);
        first_runs_at = first_runs_at == 0 && sent.runs != 0 ? s : first_runs_at;
        EXPECT_TRUE(sent.max_n >= last_n || sent.max_n == 0); // n never falls with nothing lost
        last_n = sent.max_n;
        if (s % 30U == 0) {
            tick_readers(test_mock_now);
        }
    }
    EXPECT_EQ_U32(31, first_runs_at); // after the first tick's announce
    EXPECT_EQ_U32(45, last_n);
    EXPECT_TRUE(readers[0].ctx.hybrid.advertised_loss != 0);
    EXPECT_EQ_U32(readers[0].ctx.hybrid.advertised_loss, sender.hybrid.peer_loss_p[reader_id(0)]);
    expect_intact(0, 300);
    EXPECT_TRUE(readers[0].in_order);
}

// Under frame loss, RELIABLE delivers every sample whole and in order, and every repair is an MTU unit - no run is ever
// resent, no repair datagram is above one frame. Forced IP (n = 45 throughout) and AUTO, 1% and 5% frame loss, small
// and large samples.
static void reliable_under_loss(uint32_t len, uint8_t mode, uint32_t loss_ppm, uint8_t samples) {
    _tt_CONFIG.reassembly_cap_off = mode == tt_LARGE_FRAGMENTATION_IP ? 1 : 0; // a forced arm's reader, as on the rig
    init_one(len, FULL_CEILING, true);
    _tt_CONFIG.reassembly_cap_off = 0;

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

// NACKs are read for the failed-path watch only: a first NACK moves nack_high, a re-NACK does not, and with no sample
// watched nothing is read.
static void test_nacks_are_read_for_the_watch(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    struct tt_PeerAck* ack = &pub.peer_acks[0];
    uint32_t entity = readers[0].sub.endpoint.entity_id;
    hybrid_note_nacks(&pub, reader_id(0), entity, 100, (const uint64_t[]) {1}, 1);
    EXPECT_EQ_U32(0, ack->nack_high); // nothing watched
    hybrid_note_exposure(&pub, reader_id(0), 45, 300, 100, 299);
    uint64_t bitmap[2] = {(1ULL << 45U) - 1U, 0}; // 100..144: one run of 45
    hybrid_note_nacks(&pub, reader_id(0), entity, 100, bitmap, 2);
    EXPECT_EQ_U32(144, ack->nack_high);
    EXPECT_EQ_U32(45, ack->watch_nacked);
    hybrid_note_nacks(&pub, reader_id(0), entity, 100, bitmap, 2); // again: re-NACKs
    EXPECT_EQ_U32(45, ack->watch_nacked);
    EXPECT_EQ_U32(144, ack->nack_high);
}

// Readers of one RELIABLE writer by unicast, one advertising 0: it gets MTU units, the other the cost model's n.
static void test_mixed_reliable_readers_each_get_their_own_n(void) {
    static const uint32_t ceilings[] = {FULL_CEILING, 0};
    init_set(ONE_MB, 2, ceilings, true);
    hybrid_test_forced_p = 0.001;
    sim_start(0);
    sample_seed = 1;
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    // Before any ACKNACK: the reader sent runs watches the sample for a failed path; the one sent MTU units does not.
    EXPECT_EQ_U32(723, pub.peer_acks[0].watch_count);
    EXPECT_EQ_U32(0, pub.peer_acks[1].watch_count);
    sim_drain();
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_U32(182, sent.to_reader[0]); // 181 runs of up to 4, and the tail
    EXPECT_EQ_U32(723, sent.to_reader[1]);
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

// A path that drops every IP fragment, met after n has reached 45: the next sample's runs are all lost whole while its
// tail arrives - the path is judged failed, not lossy - and every sample after it goes as MTU units, needing no repair,
// as the forced-MTU arm's would. After tt_HYBRID_PATH_CLEAN_NS one sample re-probes with a single run: lost, and the
// path is failed again; with the filter gone, it gets through and n returns. Every sample whole and in order.
static void test_reliable_through_a_path_that_drops_fragments(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(0);
    hybrid_test_forced_p = 0.0; // n = 45: the path's judgement does not depend on p
    uint8_t s = 1;
    for (; s <= 17; s++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, tt_SECOND);
    }
    EXPECT_EQ_U32(45, sent.max_n);
    sim_drop_fragmented = true;
    memset(&sent, 0, sizeof(sent));
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
    sim_settle(s, 5 * tt_SECOND);
    s++;
    EXPECT_EQ_U32(45, sent.max_n); // the sample that meets the filter
    EXPECT_TRUE(pub.peer_acks[0].path_failed_ns != 0);
    EXPECT_EQ_U32(1, pub.peer_acks[0].path_failures);
    for (int k = 0; k < 5; k++, s++) {
        memset(&sent, 0, sizeof(sent));
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, 5 * tt_SECOND);
        EXPECT_EQ_U32(0, sent.runs); // the second sample on: MTU units, as the forced-MTU arm
        EXPECT_EQ_U32(0, sent.repairs);
    }
    // The clean period passes: one sample re-probes with one run, the rest MTU; it is lost, so the path fails again.
    test_mock_now += tt_HYBRID_PATH_CLEAN_NS;
    memset(&sent, 0, sizeof(sent));
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
    sim_settle(s, 5 * tt_SECOND);
    s++;
    EXPECT_EQ_U32(1, sent.runs);
    EXPECT_EQ_U32(723 - sent.run_units, sent.mtu); // the run of n units, the rest MTU
    EXPECT_EQ_U32(2, pub.peer_acks[0].path_failures);
    memset(&sent, 0, sizeof(sent));
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
    sim_settle(s, 5 * tt_SECOND);
    s++;
    EXPECT_EQ_U32(0, sent.runs);
    // The filter goes; after the clean period the re-probe gets through and the next sample is in runs again.
    sim_drop_fragmented = false;
    test_mock_now += tt_HYBRID_PATH_CLEAN_NS;
    memset(&sent, 0, sizeof(sent));
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
    sim_settle(s, 5 * tt_SECOND);
    s++;
    EXPECT_EQ_U32(1, sent.runs);
    memset(&sent, 0, sizeof(sent));
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
    sim_settle(s, 5 * tt_SECOND);
    s++;
    EXPECT_TRUE(sent.runs > 1);
    EXPECT_EQ_U64(0, pub.peer_acks[0].path_failed_ns);
    expect_intact(0, s - 1);
    hybrid_test_forced_p = -1.0;
    EXPECT_TRUE(readers[0].in_order);
}

// The judgement on its own: a sample of 100 units sent as two runs (0-44, 45-89 ... here 0-98 in runs of 45: three)
// and its tail. Every run unit NACKed with the tail too is loss, not a failed path; without the tail it is.
static void test_a_failed_path_needs_the_tail_through(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    struct tt_PeerAck* ack = &pub.peer_acks[0];
    uint32_t entity = readers[0].sub.endpoint.entity_id;
    hybrid_note_exposure(&pub, reader_id(0), 45, 100, 1000, 99);
    EXPECT_EQ_U32(3, ack->watch_runs);
    uint64_t bitmap[2] = {~0ULL, (1ULL << 36U) - 1U}; // 1000..1099: every run unit and the tail
    hybrid_note_nacks(&pub, reader_id(0), entity, 1000, bitmap, 2);
    EXPECT_EQ_U32(0, ack->path_failures);
    hybrid_note_exposure(&pub, reader_id(0), 45, 100, 2000, 99);
    bitmap[1] = (1ULL << 35U) - 1U; // 2000..2098: the runs, not the tail
    hybrid_note_nacks(&pub, reader_id(0), entity, 2000, bitmap, 2);
    EXPECT_EQ_U32(1, ack->path_failures);
    EXPECT_TRUE(ack->path_failed_ns != 0);
    // A sample one of whose runs got through: no judgement.
    ack->path_failed_ns = 0;
    ack->all_lost_streak = 0;
    hybrid_note_exposure(&pub, reader_id(0), 45, 100, 3000, 99);
    bitmap[0] = 0;
    bitmap[1] = (1ULL << 35U) - 1U; // 3064..3098 only
    hybrid_note_nacks(&pub, reader_id(0), entity, 3000, bitmap, 2);
    EXPECT_EQ_U32(1, ack->path_failures);
}

// Real frame loss is not a failed path: at 5% frame loss AUTO runs n by the cost model, its runs are lost now and then
// but never every run of a sample with the tail through, and the path is never judged failed (40 samples of 1 MB).
// A run of one unit lost twice in a row (a 3-unit sample, one run) is enough on its own - two samples, not one.
static void test_frame_loss_is_not_a_failed_path(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(50000);
    for (uint8_t s = 1; s <= 40; s++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, 5 * tt_SECOND);
    }
    EXPECT_EQ_U32(0, pub.peer_acks[0].path_failures);
    expect_intact(0, 40);
    init_one(3500, FULL_CEILING, true);
    pub.large_fragmentation = tt_LARGE_FRAGMENTATION_AUTO;
    hybrid_test_forced_p = 0.0; // n = n_max: one run of two units and the tail
    sim_start(0);
    sim_drop_fragmented = true;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, 5 * tt_SECOND);
    EXPECT_EQ_U32(0, pub.peer_acks[0].path_failures); // one sample, one run: not yet
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
    sim_settle(2, 5 * tt_SECOND);
    EXPECT_EQ_U32(1, pub.peer_acks[0].path_failures); // the second in a row
    memset(&sent, 0, sizeof(sent));
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(3));
    sim_settle(3, 5 * tt_SECOND);
    EXPECT_EQ_U32(0, sent.runs);
    hybrid_test_forced_p = -1.0;
    expect_intact(0, 3);
}

// --- the objective: wire bytes per delivered unit (DESIGN.md section 8, "The objective and the control law") --------

// Cost(n) = (u + h / n) + (1 - (1 - p)^n) (u + h), u = 1,454 B (a unit on 8/9) and h = 26 B (its framing and UDP):
// DESIGN.md's table - 45, 42, 13, 4, 2, 1 at 0, 0.001, 0.01, 0.1, 0.5 and 1% - and never above n_max.
static void test_the_objective_is_wire_bytes(void) {
    EXPECT_EQ_U32(1454, HYBRID_UNIT);
    EXPECT_EQ_U32(26, HYBRID_UNIT_HEADER);
    static const double ps[] = {0.0, 0.00001, 0.0001, 0.001, 0.005, 0.01, 0.05, 1.0};
    static const uint32_t ns[] = {45, 42, 13, 4, 2, 1, 1, 1};
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
        EXPECT_EQ_U32(ns[i], hybrid_n_star(ps[i], HYBRID_UNIT, HYBRID_UNIT_HEADER, 45));
        EXPECT_EQ_U32(ns[i] < 8U ? ns[i] : 8U, hybrid_n_star(ps[i], HYBRID_UNIT, HYBRID_UNIT_HEADER, 8));
    }
    // h is what n trades against: no header to share and runs only lose; a header the size of a unit and n grows.
    EXPECT_EQ_U32(1, hybrid_n_star(0.0001, HYBRID_UNIT, 0.0, 45));
    EXPECT_EQ_U32(45, hybrid_n_star(0.0001, HYBRID_UNIT, 100.0 * HYBRID_UNIT, 45));
    // A repair costs the unit and its header (u + h): with h = u the optimum is 23, with only u it would be 32.
    EXPECT_EQ_U32(23, hybrid_n_star(0.001, 100.0, 100.0, 45));
}

// p for the objective is the posterior mean (events + 1) / frames, 1 with no exposure; the bound stays 3 / N.
static void test_the_objective_reads_the_posterior_mean(void) {
    struct tt_LossEstimate estimate = {0, 0};
    EXPECT_TRUE(loss_estimate_mean(&estimate) == 1.0);
    estimate.frames = 100000;
    EXPECT_TRUE(loss_estimate_mean(&estimate) > 0.0000099 && loss_estimate_mean(&estimate) < 0.0000101);
    estimate.events = 99;
    EXPECT_TRUE(loss_estimate_mean(&estimate) > 0.00099 && loss_estimate_mean(&estimate) < 0.00101);
    // n_max at 0% from about 115,000 clean frames, and kept there: the window halves at 2^20, not below it.
    estimate = (struct tt_LossEstimate) {0, 115000};
    EXPECT_EQ_U32(45, hybrid_n_star(loss_estimate_mean(&estimate), HYBRID_UNIT, HYBRID_UNIT_HEADER, 45));
    estimate.frames = (uint32_t)tt_HYBRID_WINDOW_FRAMES / 2U;
    EXPECT_EQ_U32(45, hybrid_n_star(loss_estimate_mean(&estimate), HYBRID_UNIT, HYBRID_UNIT_HEADER, 45));
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
    test_the_collapse_test_is_the_designs();
    test_slow_start_and_the_drop_on_a_collapse();
    test_the_hold_and_the_timed_probe();
    test_a_probe_that_holds_moves_the_ceiling_up();
    test_the_ceiling_scales_with_r_p();
    test_tau_from_the_writers_run_rates();
    test_the_law_runs_once_an_interval();
    test_the_reader_rebuilds_what_was_sent();
    test_the_loss_code_rounds_up();
    test_the_path_rule_needs_frame_loss_to_be_no_excuse();
    test_a_reader_with_the_cap_off_advertises_its_ceiling();
    test_the_loss_bound_is_the_poisson_upper_limit();
    test_frame_loss_never_moves_the_cap();
    test_a_path_that_drops_fragments_brings_n_to_one();
    test_a_reassembly_squeeze_end_to_end();
    test_n_follows_p();
    test_reliable_auto_leaves_mtu_as_clean_frames_accumulate();
    test_reliable_repairs_are_mtu_units_only();
    test_nacks_are_read_for_the_watch();
    test_mixed_reliable_readers_each_get_their_own_n();
    test_keep_all_bounds_are_unchanged();
    test_reliable_through_a_path_that_drops_fragments();
    test_frame_loss_is_not_a_failed_path();
    test_a_failed_path_needs_the_tail_through();
    test_the_objective_is_wire_bytes();
    test_the_objective_reads_the_posterior_mean();
    free(sim_queue);
    if (test_result() != 0) {
        return 1;
    }
    printf("test_hybrid_send: all tests passed\n");
    return 0;
}
