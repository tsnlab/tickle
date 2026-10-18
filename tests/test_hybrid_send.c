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

// tests/wire_identity_hybrid.sh builds this source against cores that predate the QoS creation API (DESIGN.md section
// 9), with their own test_common.h: there it creates as they did. tt_QOS_REORDER_NONE is a macro of the profile's.
#ifdef tt_QOS_REORDER_NONE
#define HYBRID_QOS_AT_CREATION 1
#else
#define HYBRID_QOS_AT_CREATION 0
#define tt_TypeSupport tt_Topic
#endif

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

// --- the ranged encode (DESIGN.md section 8, "Encoding while sending") -----------------------------------------------
// On the writer's topic while `ranged` is set: sized_encode's bytes, a range at a time. Each call is logged with the
// units the writer had put on the wire by then and whether its send buffer was full.

static bool ranged;
// The seq_no the next set's writer gives its first unit (init_set()): 1 as a writer's own, or one near 2^32.
static uint32_t first_seq_no = 1;
struct range_call {
    uint32_t offset;
    uint32_t length;
    uint32_t units_before; // units sent (first transmissions, in runs and alone) when the range was asked for
    bool buffer_full;
};
#define RANGE_CALLS_MAX 256U
static struct range_call range_calls[RANGE_CALLS_MAX];
static uint32_t range_call_count;
static uint32_t units_sent(void);

static int32_t sized_encode_range(struct tt_Data* data, uint8_t* payload, uint32_t offset, uint32_t len) {
    (void)data;
    if (range_call_count < RANGE_CALLS_MAX) {
        range_calls[range_call_count] =
            (struct range_call) {offset, len, units_sent(), test_mock_nonblocking_room == 0};
    }
    range_call_count++;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = pattern(offset + i, sample_seed);
    }
    return (int32_t)len;
}

// --- the readers -----------------------------------------------------------------------------------------------------

struct reader {
    struct tt_Context ctx;
    struct tt_TypeSupport topic;
    struct tt_Subscriber sub;
    uint32_t ip;
    int delivered;
    int torn;
    uint32_t last_seq;
    bool in_order;
    bool present;
    bool retain_next;          // the next delivery is kept with tt_Sample_retain()
    struct tt_Sample held;     // what it kept
    tt_ret_t retain_result;    // and what tt_Sample_retain() answered
    uint8_t lend_kind;         // the context's lend kind while that delivery's callback ran
    const uint8_t* decoded_at; // the payload the decode was handed, last delivery
    uint8_t* granted[8];       // the large buffers this reader's context was handed, most recent first
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
    decoding->decoded_at = payload;
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
            if (readers[i].retain_next) {
                readers[i].retain_next = false;
#if tt_SAMPLE_LENDING
                readers[i].lend_kind = readers[i].ctx.lend.rx_kind;
#endif
                readers[i].retain_result = tt_Sample_retain(s, &readers[i].held);
            }
        }
    }
}

// `user` is the reader whose context asks (NULL: the writer's): each reader keeps the buffers it was handed, so a lent
// sample can be traced to the assembly it was put together in.
static void* test_acquire(void* user, uint32_t bytes) {
    uint8_t* buffer = (uint8_t*)malloc(bytes);
    struct reader* r = (struct reader*)user;
    if (r != NULL && buffer != NULL) {
        memmove((void*)&r->granted[1], (const void*)&r->granted[0], sizeof(r->granted) - sizeof(r->granted[0]));
        r->granted[0] = buffer;
    }
    return buffer;
}

static void test_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}

// --- the writer ------------------------------------------------------------------------------------------------------

static struct tt_Context sender;
static struct tt_TypeSupport sender_topic;
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
    sender_topic.data_encode_range = ranged ? sized_encode_range : NULL;
    range_call_count = 0;
#if HYBRID_QOS_AT_CREATION
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&sender), &pub, &sender_topic,
                                                      ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
#else
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&sender, &pub, &sender_topic, ENDPOINT_NAME));
#endif
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&sender, test_acquire, test_release, NULL));
    pub.seq_no = first_seq_no - 1U; // before any reader is matched: each one's baseline is where the writer is
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
        r->retain_next = false;
        memset(&r->held, 0, sizeof(r->held));
        r->retain_result = tt_RET_OK;
        r->lend_kind = 0;
        r->decoded_at = NULL;
        memset((void*)r->granted, 0, sizeof(r->granted));
        init_bare_node(&r->ctx, reader_id(i));
        memset(&r->topic, 0, sizeof(r->topic));
        r->topic.name = TOPIC_NAME;
        r->topic.data_size = 8;
        r->topic.data_decode = checking_decode;
        r->topic.data_free = free_nothing;
#if HYBRID_QOS_AT_CREATION
        EXPECT_EQ_INT(tt_RET_OK,
                      tt_Node_create_subscriber(tt_Context_default_node(&r->ctx), &r->sub, &r->topic, ENDPOINT_NAME,
                                                &TEST_QOS_UNRELIABLE,
                                                &(struct tt_SubscriberOptions) {.callback = on_data}, NULL, 0));
#else
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&r->ctx, &r->sub, &r->topic, ENDPOINT_NAME, on_data));
#endif
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&r->ctx, test_acquire, test_release, r));
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
// A receiver that falls behind: every sim_receiver_drop_every-th run the sender's link delivers is dropped at the
// reader's socket, as a full receive buffer drops the largest datagrams first. Counted where the HAL counts it
// (test_mock_receiver_drops) unless sim_receiver_drops_uncounted: a HAL that cannot say.
static uint32_t sim_receiver_drop_every;
static uint32_t sim_receiver_runs;
static uint32_t sim_receiver_dropped;
static bool sim_receiver_drops_uncounted;
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
// What the link did to the sender's first transmissions on the fragment path, by kind: the count a reader's p_mtu and
// q(n) estimate. A repair - addressed to one reader, a first transmission to everyone - is not in it.
struct link_count {
    uint32_t mtu;      // MTU units put on the link
    uint32_t mtu_lost; // and lost there
    uint32_t runs;
    uint32_t runs_lost;
};
static struct link_count link;

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
// Every datagram the writer sent, in order, folded into one FNV-1a hash with their count and bytes: two arms that sent
// the same datagrams byte for byte agree on all three.
struct wire_digest {
    uint64_t hash;
    uint32_t datagrams;
    uint64_t bytes;
};
static struct wire_digest wire;
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
        for (size_t i = 0; i < len; i++) {
            wire.hash = (wire.hash ^ d->bytes[i]) * 0x100000001b3ULL;
        }
        wire.hash = (wire.hash ^ d->to_ip) * 0x100000001b3ULL;
        wire.datagrams++;
        wire.bytes += len;
    }
}

static uint32_t units_sent(void) {
    return sent.run_units + sent.mtu;
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

static bool (*sim_drop_hook)(const struct sim_datagram* d); // a datagram a test drops by what it is, or NULL

static bool sim_lost(const struct sim_datagram* d) {
    if (sim_drop_hook != NULL && sim_drop_hook(d)) {
        return true;
    }
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

// Whether the reader's socket drops this datagram of the sender's (sim_receiver_drop_every, above).
static bool sim_receiver_drops(const struct sim_datagram* d) {
    if (sim_receiver_drop_every == 0 || d->len <= FRAME_PAYLOAD - 8U) {
        return false;
    }
    if (++sim_receiver_runs % sim_receiver_drop_every != 0) {
        return false;
    }
    sim_receiver_dropped++;
    if (!sim_receiver_drops_uncounted) {
        test_mock_receiver_drops++;
    }
    return true;
}

// One of the sender's datagrams the link carried, `lost` or not, in its count (link, above).
static void link_note(const struct sim_datagram* d, bool lost) {
    uint32_t first = 0;
    uint32_t units = 0;
    bool run = false;
    bool addressed =
        d->bytes[0] != tt_SINGLE_MARKER_LE && d->bytes[0] != tt_SINGLE_MARKER_BE &&
        ((const struct tt_SubmessageHeader*)(d->bytes + sizeof(struct tt_Header)))->receiver != tt_SUBMESSAGE_ID_ALL;
    if (d->from != SENDER_ID || addressed || !datagram_units(d->bytes, &first, &units, &run)) {
        return;
    }
    if (run) {
        link.runs++;
        link.runs_lost += lost ? 1U : 0U;
    } else {
        link.mtu++;
        link.mtu_lost += lost ? 1U : 0U;
    }
}

static void sim_deliver(struct sim_datagram* d) {
    bool lost = sim_lost(d);
    link_note(d, lost);
    if (!lost) {
        if (d->from == SENDER_ID) {
            for (int i = 0; i < READERS; i++) {
                if (readers[i].present && (d->to_ip == 0 || d->to_ip == readers[i].ip) && !sim_receiver_drops(d)) {
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

#define SIM_PAUSE_HEARTBEAT_NS (100U * tt_MILLISECOND)
static uint64_t sim_pause_heartbeat_ns; // sim_settle(): 0, or the periodic Heartbeat it arms while a sample is owed
static uint32_t sim_pause_heartbeats;   // settles that armed it, every arm: the treatment, printed at the end

static void sim_start(uint32_t frame_loss_ppm) {
    if (sim_queue == NULL) {
        sim_queue = (struct sim_datagram*)calloc(SIM_QUEUE, sizeof(struct sim_datagram));
    }
    sim_head = sim_tail = 0;
    sim_frame_loss_ppm = frame_loss_ppm;
    sim_rng = 11;
    sim_drop_fragmented = false;
    sim_drop_hook = NULL;
    sim_pause_heartbeat_ns = 0;
    sim_receiver_drop_every = 0;
    sim_receiver_runs = 0;
    sim_receiver_dropped = 0;
    sim_receiver_drops_uncounted = false;
    test_mock_receiver_drops = 0;
    sim_reasm_threshold = 0;
    sim_reasm_lifetime = 30U * tt_SECOND;
    sim_reasm_head = sim_reasm_tail = 0;
    sim_reasm_held = 0;
    sim_reasm_refused = 0;
    memset(&link, 0, sizeof(link));
    memset(&sent, 0, sizeof(sent));
    wire = (struct wire_digest) {0xcbf29ce484222325ULL, 0, 0};
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

// A RELIABLE sample on 8/9 can still be owed when the link goes quiet: its tail was lost (with the run before it, or
// alone), and nothing the writer has sent shows the reader a gap. Core chases the end-of-sample HEARTBEAT for large
// samples (11/12) only; a sample on 8/9 is asked for when the next sample's traffic shows the gap, or at the periodic
// Heartbeat (tt_Publisher_set_heartbeat_period()), what production offers a publisher that has stopped. An arm that
// settles each sample before the next sets sim_pause_heartbeat_ns, and the settle arms that Heartbeat while a sample is
// owed and nothing is due. Without it the owed sample arrived during the next publish and was checked against that
// sample's pattern (torn), and the arm's last one never arrived: 9 of 12 seeds failed so (2026-10-14).
static void sim_settle(int want, uint64_t budget) {
    uint64_t since = test_mock_now;
    bool armed = false;
    while (total_delivered() < want && test_mock_now - since < budget) {
        if (sim_step()) {
            continue;
        }
        if (armed || sim_pause_heartbeat_ns == 0) {
            break;
        }
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, sim_pause_heartbeat_ns));
        armed = true;
        sim_pause_heartbeats++;
    }
    if (armed) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, 0));
    }
}

// init_set() matches each reader on the writer as its announce would, but the announce itself still arrives at the
// first step, and the writer takes it as a new peer: it re-registers the reader and sends the initial HEARTBEAT, which
// names what is already published. Production matches a reader at that announce, before any sample owed to it; an arm
// that publishes first has sent the reader its first sample by then, and when the match Heartbeat and that sample's
// datagrams naming the endpoint are all lost, the initial HEARTBEAT is the reader's first contact and puts a VOLATILE
// reader's baseline past the sample - never asked for (seeds 33 and 98 of 100, 2026-10-14). So an arm that settles a
// RELIABLE stream under loss lets the announces go first, both ways. These bare contexts announce once, at their
// endpoint's creation, and run no node_update(): one lost under the arm's loss is sent again a
// tt_CONTEXT_UPDATE_INTERVAL on, as the next summary would have it pulled.
static bool sim_announced(void) {
    for (int i = 0; i < READERS; i++) {
        if (readers[i].present &&
            (sender.update_last_seen[reader_id(i)] == 0 || readers[i].ctx.update_last_seen[SENDER_ID] == 0)) {
            return false;
        }
    }
    return true;
}

#define SIM_ANNOUNCE_TRIES 16U
static struct tt_Discovery sim_discovery[READERS];

// What a lost announce gets a tt_CONTEXT_UPDATE_INTERVAL on: each side that was not heard announces again.
static void sim_announce_again(void) {
    test_mock_now += tt_CONTEXT_UPDATE_INTERVAL;
    bool writer_unheard = false;
    for (int i = 0; i < READERS; i++) {
        writer_unheard = writer_unheard || (readers[i].present && readers[i].ctx.update_last_seen[SENDER_ID] == 0);
    }
    if (writer_unheard) {
        sim_acting = SENDER_ID;
        (void)build_and_send_update(&sender, NULL, 0);
    }
    for (int i = 0; i < READERS; i++) {
        if (readers[i].present && sender.update_last_seen[reader_id(i)] == 0) {
            sim_acting = readers[i].ctx.id;
            (void)build_and_send_update(&readers[i].ctx, NULL, 0);
        }
    }
    sim_drain();
}

static bool sim_no_discovery_table; // sim_announce(): the readers get no discovery table (a core-only reader)

static void sim_announce(void) {
    // A writer proxy made after the writer's announce takes its KEEP_ALL policy from the reader's discovery table
    // (writer_announced_keep_all()), as an rmw_tickle reader has one. A reader without one learns it only when the
    // writer's next summary has the announce pulled again, which these bare contexts never send on their own
    // (test_a_reader_without_a_discovery_table_learns_keep_last()).
    for (int i = 0; i < READERS; i++) {
        if (readers[i].present && !sim_no_discovery_table) {
            memset(&sim_discovery[i], 0, sizeof(sim_discovery[i]));
            EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&readers[i].ctx, &sim_discovery[i], NULL, NULL));
        }
    }
    for (uint32_t tries = 0; !sim_announced() && tries <= SIM_ANNOUNCE_TRIES; tries++) {
        if (tries > 0) {
            sim_announce_again();
        }
        uint64_t since = test_mock_now;
        while (!sim_announced() && test_mock_now - since < tt_SECOND && sim_step()) {
        }
    }
    EXPECT_TRUE(sim_announced());
}

static void expect_intact(int reader, int want) {
    EXPECT_EQ_INT(want, readers[reader].delivered);
    EXPECT_EQ_INT(0, readers[reader].torn);
}

// Until 2026-10-15 a KEEP_LAST RELIABLE reader gave a unit up after tt_RELIABLE_RETRY requests for it in a row went
// unanswered (gap_abandoned), and the sample it belonged to with it: at 5% frame loss each request and each repair is
// lost one time in twenty, so over 40 samples of 1 MB that happened to some unit in about one stream in seven (14 seeds
// of 100, 2026-10-14). It no longer gives up on a count; a unit can still be given up by a window jump, or skipped on
// the writer's eviction HEARTBEAT. Every sample is delivered whole unless a unit was, and only as many fewer as were.
static void expect_intact_or_given_up(int reader, int want) {
    int abandoned = (int)(readers[reader].sub.gap_abandoned + readers[reader].sub.gap_evicted);
    int delivered = readers[reader].delivered;
    EXPECT_EQ_INT(0, readers[reader].torn);
    EXPECT_TRUE(delivered <= want && delivered + abandoned >= want);
    EXPECT_TRUE((delivered == want) == (abandoned == 0));
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

// The receive drain core asks for while a large sample is delivered (tt_rx_drain_start(), tickle.c large_rx_drain())
// is sized for the longest datagram that may come meanwhile: a run up to the ceiling this reader advertises - not a
// fragment, which would cut every run (drain_truncated) - and a fragment for a reader that takes no run.
static void test_the_drain_is_sized_for_runs(void) {
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(0);
    test_mock_drain_bytes = 0;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    expect_intact(0, 1);
    EXPECT_EQ_U32(FULL_CEILING, test_mock_drain_bytes);
    init_one(ONE_MB, 0, false);
    readers[0].ctx.ip_reassembly_max = 0; // a reader that advertises no ceiling: MTU fragments only
    sim_start(0);
    test_mock_drain_bytes = 0;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
    expect_intact(0, 1);
    EXPECT_EQ_U32(tt_CONTROL_MAX_LENGTH, test_mock_drain_bytes);
}

static void test_large_samples_go_in_runs_of_n_max(void) {
    large_be_round_trip(LARGE_LEN, 3);
    large_be_round_trip(ONE_MB, 18);
    large_be_round_trip(FOUR_MB, 66);
    // A send call carries runs until it holds LARGE_BATCH (64) units, two runs of 45, and the tail goes in a call of
    // its own: 1 MB's 17 runs (16 of 45 and one of 2) in 9 calls, its tail in a tenth.
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(0);
    int calls = test_mock_send_batch_call_count;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(4));
    EXPECT_EQ_INT(10, test_mock_send_batch_call_count - calls);
    expect_intact(0, 1);
    // Runs of 8 (the Publisher's own bound): eight a call, 64 units, as a batch of MTU fragments.
    init_one(LARGE_LEN, FULL_CEILING, false);
    pub.ip_datagram_max = (uint16_t)(28U + (8U * 1452U)); // exactly eight large units
    sim_start(0);
    calls = test_mock_send_batch_call_count;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(5));
    EXPECT_EQ_U32(8, sent.max_n);
    EXPECT_EQ_U32(9, sent.runs);                               // 68 units before the tail: eight runs of 8 and one of 4
    EXPECT_EQ_INT(3, test_mock_send_batch_call_count - calls); // 8 runs, the run of 4, the tail
    expect_intact(0, 1);
}

// What one send call of runs carries (large_build_runs()): runs until the call holds LARGE_BATCH (64) units, so a
// ranged encode runs a call - about a batch of MTU fragments - ahead of the wire. Of 1 MB (723 units): two runs of 45
// at n = 45, eight of 8 at n = 8; a re-probe's one run of 45 and then 19 MTU units, 64 in all; the last run, of 2,
// stops short of the tail, and the tail goes alone.
static void expect_call(uint32_t run_n, uint32_t run_until, uint32_t next, uint32_t want_datagrams,
                        uint32_t want_units) {
    static uint8_t framings[LARGE_BATCH][HYBRID_RUN_L_FRAMING];
    struct tt_OutDatagram batch[LARGE_BATCH];
    uint32_t units[LARGE_BATCH];
    const struct tt_LargeRecord* record = large_record_at(&pub, 0);
    struct tt_Peer dest = {0};
    uint32_t k = large_build_runs(&sender, record, run_n, run_until, next, &dest, framings, batch, units);
    uint32_t total = 0;
    for (uint32_t i = 0; i < k; i++) {
        total += units[i];
    }
    EXPECT_EQ_U32(want_datagrams, k);
    EXPECT_EQ_U32(want_units, total);
}

static void test_a_send_call_carries_about_large_batch_units(void) {
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(0);
    test_mock_nonblocking_room = 0; // the record stays at the cursor's first unit, unsent
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    test_mock_nonblocking_room = -1;
    EXPECT_EQ_U32(723, large_record_at(&pub, 0)->count);
    expect_call(45, 722, 0, 2, 90);
    expect_call(8, 722, 0, 8, 64);
    expect_call(45, 45, 0, 20, 64); // a re-probe: one run, then MTU units, each counted
    expect_call(45, 722, 630, 2, 90);
    expect_call(45, 722, 720, 1, 2); // the last run, of 2: never the tail
    expect_call(45, 722, 722, 1, 1); // the tail, alone
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
    hybrid->mtu_loss = (struct tt_LossEstimate) {events, frames, 0, 0};
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
// q 0.46) is one. Significance is judged against L0 at p_mtu's 99.9% bound (0.170%: L0 1.35% at n = 8, 13.5 runs of
// 1,000), the size of the excess against L0 at the 95% bound (0.136%: 1.08%, 10.8 runs): 26 lost is both (z 3.27), 25
// is twice L0 but not significant (z 3.00 - it was, z 4.19, at the 95% bound), 20 is neither and not clean either; a
// significant excess under 2 L0 is not a collapse; and nothing is decided without enough runs, losses, or an MTU
// baseline. A clean window loses no more than R L0 at the 95% bound.
static void test_the_collapse_test_is_the_designs(void) {
    struct tt_LossEstimate mtu = {30, 30000, 0, 0}; // p_mtu 0.1%
    struct tt_RunWindow consistent = {1000, 8, 8000};
    EXPECT_TRUE(!hybrid_collapsed(&consistent, &mtu));
    EXPECT_TRUE(hybrid_clean(&consistent, &mtu));
    struct tt_RunWindow pc = {1000, 540, 8000};
    EXPECT_TRUE(hybrid_collapsed(&pc, &mtu));
    EXPECT_TRUE(!hybrid_clean(&pc, &mtu));
    struct tt_RunWindow excess = {1000, 26, 8000};
    EXPECT_TRUE(hybrid_collapsed(&excess, &mtu));
    EXPECT_TRUE(!hybrid_clean(&excess, &mtu));
    struct tt_RunWindow not_significant = {1000, 25, 8000};
    EXPECT_TRUE(!hybrid_collapsed(&not_significant, &mtu));
    struct tt_RunWindow within = {1000, 20, 8000};
    EXPECT_TRUE(!hybrid_collapsed(&within, &mtu));
    EXPECT_TRUE(!hybrid_clean(&within, &mtu));       // neither: the window gathers more
    struct tt_RunWindow above_l0 = {1000, 12, 8000}; // above R L0 at the 95% bound (10.8), under it at 99.9% (13.5)
    EXPECT_TRUE(!hybrid_clean(&above_l0, &mtu));
    // Seed 68 of test_frame_loss_never_moves_the_cap (2026-10-17): a 1% path whose first 1,599 tails lost 3. At the 95%
    // bound (0.485%) runs of 11 had L0 5.2% and 22 of 200 lost read as a collapse (z 3.53); at the 99.9% bound (0.817%,
    // L0 8.6%) they are z 1.07 - what a 1% path does (10.5%).
    struct tt_LossEstimate few_tails = {3, 1599, 0, 0};
    struct tt_RunWindow plain_loss = {200, 22, 2200};
    EXPECT_TRUE(!hybrid_collapsed(&plain_loss, &few_tails));
    // Every run of 45 refused on a baseline of 90 tails, none lost: significant at the 99.9% bound (L0 97%, z 3.66),
    // and at the 95% one L0 is 78%, so twice L0 is past 1 and the survival side decides - none survived, against at
    // most 10.9% allowed: a collapse. Read as twice L0 alone it never was, and a squeeze waited for more tails.
    struct tt_LossEstimate ninety_tails = {0, 90, 0, 0};
    struct tt_RunWindow all_refused = {510, 510, 510U * 45U};
    EXPECT_TRUE(hybrid_collapsed(&all_refused, &ninety_tails));
    struct tt_RunWindow most_refused = {510, 500, 510U * 45U}; // 2% survived: z 0.95 at the 99.9% bound
    EXPECT_TRUE(!hybrid_collapsed(&most_refused, &ninety_tails));
    // Significant (z near 6) but only 1.2x L0 on a vast exposure: not far below (1 - p)^n.
    struct tt_LossEstimate vast = {10000, 10000000, 0, 0};
    struct tt_RunWindow slight = {125000, 1200, 1000000};
    EXPECT_TRUE(!hybrid_collapsed(&slight, &vast));
    struct tt_RunWindow few_runs = {15, 15, 120};
    EXPECT_TRUE(!hybrid_collapsed(&few_runs, &mtu));
    EXPECT_TRUE(!hybrid_clean(&few_runs, &mtu));
    struct tt_RunWindow few_lost = {100, 3, 800};
    struct tt_LossEstimate none_lost = {0, 30000, 0, 0};
    EXPECT_TRUE(!hybrid_collapsed(&few_lost, &none_lost));
    struct tt_LossEstimate no_baseline = {0, 0, 0, 0};
    EXPECT_TRUE(!hybrid_collapsed(&pc, &no_baseline));
    // A fragment path refusing every run on a young baseline (200 tails, none lost: p_hi 1.5%, L0 49% at n = 45) is a
    // collapse all the same - the run-level test does not need the many MTU units a per-frame comparison would.
    struct tt_LossEstimate young = {0, 200, 0, 0};
    struct tt_RunWindow refused = {510, 510, 510U * 45U};
    EXPECT_TRUE(hybrid_collapsed(&refused, &young));
    // Twice L0 on that baseline (L0 11.4% at n = 8) but on 20 runs: not significant (z 1.6).
    struct tt_RunWindow small = {20, 5, 160};
    EXPECT_TRUE(!hybrid_collapsed(&small, &young));
}

// A reader starts at n_max (no slow start by default, 2026-10-13); a cap below n_max before any collapse - two units, a
// build's tt_HYBRID_SLOW_START - doubles per clean window to n_max. A collapse at 45 with the PC's
// survival there (30 MB/s, 0.1%: q 0.08 against 0.96) remembers the collapse point the survival shows, 45 q / q0 = 3.8
// -> 4, not the 45 the growth overshot to, and drops the cap at once to drop(4) = 3. A mild collapse (q 0.62 at 45:
// the PC's 0.01%) remembers 30 and drops to 0.7 x 30 = 21.
static void test_slow_start_and_the_drop_on_a_collapse(void) {
    struct tt_HybridState* hybrid = law_reader(30, 30000);
    EXPECT_EQ_U32(45, hybrid->cap);
    EXPECT_EQ_U32(FULL_CEILING, hybrid->advertised);
    EXPECT_EQ_U32(30 * tt_SECOND, hybrid->lifetime_ns);
    hybrid->cap = 2;
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
    hybrid->cap = 2; // below n_max before any collapse: the next clean window doubles it
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

// A sample's gap goes by that sample's layout, not the one before (hybrid_rx_layout()). After an MTU sample (a
// RELIABLE AUTO stream's first), the next sample's first run is lost and its second is the first datagram seen of it:
// one run lost - one event on 45 frames, in p only, one lost run in the window - not 45 MTU units. A receiver drop on
// hand covers it whole. The control: the same gap in front of an MTU unit, which says the sample is MTU, is 45 MTU
// units lost.
static void test_a_gap_goes_by_its_own_samples_layout(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    struct tt_HybridState* hybrid = &node->hybrid;
    test_mock_receiver_drops = 0;
    for (uint32_t i = 0; i < 100; i++) {
        hybrid_rx_note(node, SENDER_ID, 7, 100, i, 100, 1, false); // sample 100 whole, as MTU units
    }
    EXPECT_EQ_U32(1, hybrid->rx_writers[0].layout_n);
    hybrid_rx_note(node, SENDER_ID, 7, 200, 45, 100, 45, true); // sample 200: run 0-44 lost, 45-89 the first seen
    EXPECT_EQ_U32(45, hybrid->rx_writers[0].layout_n);
    EXPECT_EQ_U32(1, hybrid->rx_loss.events);
    EXPECT_EQ_U32(0, hybrid->mtu_loss.events);
    EXPECT_EQ_U32(1, hybrid->window.lost);
    EXPECT_EQ_U32(45, hybrid->rx_writers[0].body_lost);
    hybrid_rx_note(node, SENDER_ID, 7, 200, 90, 100, 9, true); // the rest of sample 200 arrives
    hybrid_rx_note(node, SENDER_ID, 7, 200, 99, 100, 1, false);
    // Its last run alone after lost ones, the layout before it MTU: runs, of an n not seen - one event, in p only.
    for (uint32_t i = 0; i < 100; i++) {
        hybrid_rx_note(node, SENDER_ID, 7, 300, i, 100, 1, false); // sample 300 whole, as MTU units
    }
    hybrid_rx_note(node, SENDER_ID, 7, 400, 90, 100, 9, true); // sample 400's 0-89 lost, its short last run seen
    EXPECT_EQ_U32(0, hybrid->rx_writers[0].layout_n);
    EXPECT_EQ_U32(0, hybrid->mtu_loss.events);
    EXPECT_EQ_U32(2, hybrid->rx_loss.events);
    hybrid_rx_note(node, SENDER_ID, 7, 400, 99, 100, 1, false);
    // A receiver drop on hand covers the lost first run whole: one datagram.
    for (uint32_t i = 0; i < 100; i++) {
        hybrid_rx_note(node, SENDER_ID, 7, 500, i, 100, 1, false);
    }
    uint32_t events = hybrid->rx_loss.events;
    test_mock_receiver_drops = 1;
    hybrid_rx_note(node, SENDER_ID, 7, 600, 45, 100, 45, true);
    EXPECT_EQ_U32(events, hybrid->rx_loss.events);
    EXPECT_EQ_U64(1, hybrid->receiver_lost);
    test_mock_receiver_drops = 0;
    // The control: the same gap in front of an MTU unit - the sample is MTU, and 45 MTU units were lost.
    init_one(SMALL_LEN, FULL_CEILING, false);
    node = &readers[0].ctx;
    hybrid = &node->hybrid;
    for (uint32_t i = 0; i < 100; i++) {
        hybrid_rx_note(node, SENDER_ID, 7, 100, i, 100, 1, false);
    }
    hybrid_rx_note(node, SENDER_ID, 7, 200, 45, 100, 1, false);
    EXPECT_EQ_U32(45, hybrid->rx_loss.events);
    EXPECT_EQ_U32(45, hybrid->mtu_loss.events);
    EXPECT_EQ_U32(0, hybrid->window.lost);
}

// One 12-unit sample of writer 7's, sent as one run of 11 and the tail, arrived whole (first seq_no `first`).
static void whole_sample(struct tt_Context* node, uint32_t first) {
    hybrid_rx_note(node, SENDER_ID, 7, first, 0, 12, 11, true);
    hybrid_rx_note(node, SENDER_ID, 7, first, 11, 12, 1, false);
}

// A sample the reader never saw - its run and its tail both lost - is counted from the jump in the writer's seq_no
// (hybrid_rx_missed()): a jump of whole samples of the size on both sides is that many samples lost, each laid out as
// the last one was - one lost run and one lost tail each, in p, p_mtu and the run window, or MTU units for a writer
// sending MTU, receiver drops on hand put against its datagrams as against any gap. Anything else is left alone: a
// jump that is not whole samples (small samples between), a size that changed, more than eight samples, a seq_no that
// went back, and a repair (addressed to this reader: rx_targeted), which is never a first transmission. Across 2^32
// as anywhere.
static void test_a_sample_lost_whole_is_counted(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    struct tt_HybridState* hybrid = &node->hybrid;
    test_mock_receiver_drops = 0;
    whole_sample(node, 100);
    hybrid_rx_note(node, SENDER_ID, 7, 124, 0, 12, 11, true); // 112-123 never came
    EXPECT_EQ_U64(1, hybrid->missed_samples);
    EXPECT_EQ_U32(1, hybrid->mtu_loss.events); // its tail
    EXPECT_EQ_U32(2, hybrid->mtu_loss.frames);
    EXPECT_EQ_U32(1, hybrid->window.lost); // its run
    EXPECT_EQ_U32(3, hybrid->window.sent);
    EXPECT_EQ_U32(2, hybrid->rx_loss.events);
    EXPECT_EQ_U32(12 + 12 + 11, hybrid->rx_loss.frames);
    EXPECT_EQ_U32(12 + 12 + 11, hybrid->tick_units); // R: every unit sent to it, arrived or not
    hybrid_rx_note(node, SENDER_ID, 7, 124, 11, 12, 1, false);
    hybrid_rx_note(node, SENDER_ID, 7, 160, 0, 12, 11, true); // 136-159: two samples
    EXPECT_EQ_U64(3, hybrid->missed_samples);
    EXPECT_EQ_U32(3, hybrid->mtu_loss.events);
    EXPECT_EQ_U32(3, hybrid->window.lost);
    EXPECT_EQ_U32(0, hybrid->rx_writers[0].body_lost); // the sample now is 160's, nothing of it lost yet
    hybrid_rx_note(node, SENDER_ID, 7, 160, 11, 12, 1, false);
    // Not whole samples of its size: a small sample between (one seq_no), a sample and a small one (13), or a size
    // that changed (13 seq_nos before a sample of 13 units, after one of 12).
    whole_sample(node, 173);
    whole_sample(node, 198);                                  // 185-197
    hybrid_rx_note(node, SENDER_ID, 7, 223, 0, 13, 12, true); // 210-222
    hybrid_rx_note(node, SENDER_ID, 7, 223, 12, 13, 1, false);
    EXPECT_EQ_U64(3, hybrid->missed_samples);
    EXPECT_EQ_U32(3, hybrid->mtu_loss.events);
    // More than eight samples: a reader cut off a while, or a writer that restarted. Eight are counted.
    whole_sample(node, 236);
    whole_sample(node, 248 + (9 * 12)); // 248-355: nine
    EXPECT_EQ_U64(3, hybrid->missed_samples);
    whole_sample(node, 368 + (8 * 12)); // 368-463: eight
    EXPECT_EQ_U64(11, hybrid->missed_samples);
    EXPECT_EQ_U32(11, hybrid->mtu_loss.events);
    // A seq_no that went back: nothing (an older sample's unit).
    whole_sample(node, 100);
    EXPECT_EQ_U64(11, hybrid->missed_samples);
    // A repair, addressed to this reader: never a first transmission, whatever its seq_no.
    node->rx_targeted = true;
    hybrid_rx_note(node, SENDER_ID, 7, 500, 0, 12, 1, false);
    node->rx_targeted = false;
    EXPECT_EQ_U32(464, hybrid->rx_writers[0].sample);
    EXPECT_EQ_U64(11, hybrid->missed_samples);
    // A sample lost whole whose datagrams the HAL saw dropped at the socket: put down to them, one a datagram.
    uint32_t events = hybrid->mtu_loss.events;
    uint32_t rx_events = hybrid->rx_loss.events;
    test_mock_receiver_drops = 2;
    whole_sample(node, 488); // 476-487 never came, both its datagrams dropped at the socket
    EXPECT_EQ_U64(12, hybrid->missed_samples);
    EXPECT_EQ_U64(2, hybrid->receiver_lost);
    EXPECT_EQ_U32(events, hybrid->mtu_loss.events);
    EXPECT_EQ_U32(rx_events, hybrid->rx_loss.events);
    // A writer sending MTU units: a sample lost whole is 12 MTU units lost.
    for (uint32_t i = 0; i < 12; i++) {
        hybrid_rx_note(node, SENDER_ID, 7, 500, i, 12, 1, false);
    }
    EXPECT_EQ_U32(1, hybrid->rx_writers[0].layout_n);
    events = hybrid->mtu_loss.events;
    hybrid_rx_note(node, SENDER_ID, 7, 524, 0, 12, 1, false); // 512-523 never came
    EXPECT_EQ_U32(events + 12U, hybrid->mtu_loss.events);
    EXPECT_EQ_U64(13, hybrid->missed_samples);
    // Across 2^32: 0xFFFFFFF0 whole, 0xFFFFFFFC-7 never came, 8 the next.
    init_one(SMALL_LEN, FULL_CEILING, false);
    node = &readers[0].ctx;
    hybrid = &node->hybrid;
    whole_sample(node, 0xFFFFFFF0U);
    whole_sample(node, 8);
    EXPECT_EQ_U64(1, hybrid->missed_samples);
    EXPECT_EQ_U32(1, hybrid->mtu_loss.events);
    EXPECT_EQ_U32(1, hybrid->window.lost);
}

// Receiver drops are not path loss (tickle.c, hybrid_rx_receiver_lost()): a gap the HAL's count of receiver drops
// covers reaches neither p, p_mtu, the run window nor rule (e) - one drop for one lost datagram, a run or an MTU unit -
// and is counted as receiver_lost. With no drop on hand the same gap is path loss (the control). Drops not matched
// within two announce intervals are let go. A reader that has announced a p (before it, an excused gap restarts the
// estimate: test_a_doubtful_gap_is_not_clean()).
static void test_receiver_drops_are_not_path_loss(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    struct tt_HybridState* hybrid = &node->hybrid;
    hybrid->advertised_loss = hybrid_loss_code(0.01);
    test_mock_receiver_drops = 0;
    hybrid_rx_note(node, SENDER_ID, 7, 100, 0, 100, 45, true); // units 0-44: a full run of 45
    test_mock_receiver_drops = 1;                              // the next run dropped at the socket
    hybrid_rx_note(node, SENDER_ID, 7, 100, 90, 100, 9, true); // 45-89 missing: one run, the receiver's
    EXPECT_EQ_U32(0, hybrid->rx_loss.events);
    EXPECT_EQ_U32(54, hybrid->rx_loss.frames); // the lost run's frames are not observed at all
    EXPECT_EQ_U32(0, hybrid->window.lost);
    EXPECT_EQ_U32(2, hybrid->window.sent);
    EXPECT_EQ_U32(0, hybrid->rx_writers[0].body_lost);
    EXPECT_EQ_U64(1, hybrid->receiver_drops);
    EXPECT_EQ_U64(1, hybrid->receiver_lost);
    EXPECT_EQ_U32(99, hybrid->tick_units); // R still counts every unit sent to it, arrived or not
    // The control: the same gap with no drop on hand is path loss.
    hybrid_rx_note(node, SENDER_ID, 7, 200, 0, 100, 45, true); // the tail of sample 100 missing: path loss
    EXPECT_EQ_U32(1, hybrid->mtu_loss.events);
    hybrid_rx_note(node, SENDER_ID, 7, 200, 90, 100, 9, true); // 45-89 missing, no drop counted
    EXPECT_EQ_U32(2, hybrid->rx_loss.events);
    EXPECT_EQ_U32(1, hybrid->window.lost);
    EXPECT_EQ_U64(1, hybrid->receiver_lost);
    // MTU units: two missing, one drop on hand - one is the receiver's, one is path loss.
    hybrid_rx_note(node, SENDER_ID, 8, 50, 3, 10, 1, false);
    test_mock_receiver_drops = 2;
    hybrid_rx_note(node, SENDER_ID, 8, 50, 6, 10, 1, false); // 4 and 5 missing
    EXPECT_EQ_U32(2, hybrid->mtu_loss.events);
    EXPECT_EQ_U64(2, hybrid->receiver_lost);
    // A missing tail, alone, covered by a drop: no MTU loss.
    hybrid_rx_note(node, SENDER_ID, 8, 50, 7, 10, 1, false);
    hybrid_rx_note(node, SENDER_ID, 8, 50, 8, 10, 1, false);
    test_mock_receiver_drops = 3;
    hybrid_rx_note(node, SENDER_ID, 8, 60, 0, 10, 1, false); // sample 50 ended short of its tail, 9
    EXPECT_EQ_U64(3, hybrid->receiver_lost);
    EXPECT_EQ_U32(2, hybrid->mtu_loss.events);
    // Drops age out: seen at one tick, kept through the next interval, gone at the one after.
    test_mock_receiver_drops = 8;
    hybrid_rx_receiver_age(node);
    EXPECT_EQ_U32(5, hybrid->rx_drop_credit[1]);
    hybrid_rx_receiver_age(node);
    EXPECT_EQ_U32(0, hybrid->rx_drop_credit[0] + hybrid->rx_drop_credit[1]);
    uint32_t events = hybrid->mtu_loss.events;
    hybrid_rx_note(node, SENDER_ID, 8, 60, 2, 10, 1, false); // 1 missing: path loss, the old drops let go
    EXPECT_EQ_U32(events + 1U, hybrid->mtu_loss.events);
    EXPECT_EQ_U64(8, hybrid->receiver_drops);
    test_mock_receiver_drops = 0;
}

// Rule (e) on the reader's side needs frame loss to be no excuse: a writer none of whose runs ever arrived loses the
// one run of a 3-unit sample with its tail through - at p_mtu near 50% that is what frame loss does (L0 0.79): nothing;
// at p_mtu near 0, the same sample twice is (L0 2e-6 a sample): one unit at once.
static void test_the_path_rule_needs_frame_loss_to_be_no_excuse(void) {
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    struct tt_HybridState* hybrid = &node->hybrid;
    hybrid->mtu_loss = (struct tt_LossEstimate) {500, 1000, 0};
    for (uint32_t s = 0; s < 8; s++) {
        hybrid_rx_note(node, SENDER_ID, 7, 100U + (3U * s), 2, 3, 1, false); // the tail of a sample whose run was lost
    }
    EXPECT_EQ_U64(0, hybrid->path_failures);
    EXPECT_EQ_U32(45, hybrid->cap);
    hybrid->mtu_loss = (struct tt_LossEstimate) {0, 3000000, 0};
    hybrid_rx_note(node, SENDER_ID, 8, 200, 2, 3, 1, false); // another writer, met at a tail: no gap yet
    hybrid_rx_note(node, SENDER_ID, 8, 203, 2, 3, 1, false);
    EXPECT_EQ_U64(0, hybrid->path_failures); // once is still 2e-6
    hybrid_rx_note(node, SENDER_ID, 8, 206, 2, 3, 1, false);
    EXPECT_EQ_U64(1, hybrid->path_failures);
    EXPECT_EQ_U32(1, hybrid->cap);
    EXPECT_EQ_U32(45, hybrid->path_cap);
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

// "None seen" (hybrid_rx_loss_code()) takes tt_HYBRID_NONE_SEEN_FRAMES clean frames, not one frame fewer; before them,
// and after any event, a reader announces its posterior mean. Its mutant ("none seen" after any frame) used to be
// caught only by a late sample the old fixture counted as torn (2026-10-14).
static void test_none_seen_takes_its_frames(void) {
    struct tt_LossEstimate estimate = {0, 0, 0, 0};
    loss_estimate_observe(&estimate, (uint32_t)tt_HYBRID_NONE_SEEN_FRAMES - 1U, 0);
    EXPECT_EQ_U32(hybrid_loss_code(loss_estimate_mean(&estimate)), hybrid_rx_loss_code(&estimate));
    EXPECT_TRUE(hybrid_rx_loss_code(&estimate) != HYBRID_LOSS_NONE_SEEN);
    loss_estimate_observe(&estimate, 1, 0);
    EXPECT_EQ_U32(HYBRID_LOSS_NONE_SEEN, hybrid_rx_loss_code(&estimate));
    loss_estimate_observe(&estimate, 1000, 1); // an event: the mean again, whatever the frames
    EXPECT_EQ_U32(hybrid_loss_code(loss_estimate_mean(&estimate)), hybrid_rx_loss_code(&estimate));
    EXPECT_TRUE(hybrid_rx_loss_code(&estimate) != HYBRID_LOSS_NONE_SEEN);
}

// The bound: 3 / N at no events (the rule of three), the exact Poisson limits up to 10, Wilson-Hilferty above; 1 with
// no exposure.
static void test_the_loss_bound_is_the_poisson_upper_limit(void) {
    struct tt_LossEstimate estimate = {0, 0, 0, 0};
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
    // The collapse test's, at 99.9%: 6.908 / N at no events, the exact limits up to 10, Wilson-Hilferty above.
    estimate = (struct tt_LossEstimate) {0, 10000, 0, 0};
    EXPECT_TRUE(loss_estimate_bound(&estimate, true) > 0.00069075 && loss_estimate_bound(&estimate, true) < 0.00069085);
    estimate.events = 10;
    EXPECT_TRUE(loss_estimate_bound(&estimate, true) > 0.002413 && loss_estimate_bound(&estimate, true) < 0.002414);
    estimate.events = 100; // exactly 134.92
    EXPECT_TRUE(loss_estimate_bound(&estimate, true) > 0.01348 && loss_estimate_bound(&estimate, true) < 0.01351);
    estimate.events = 1000; // exactly 1101.63
    EXPECT_TRUE(loss_estimate_bound(&estimate, true) > 0.11010 && loss_estimate_bound(&estimate, true) < 0.11022);
    estimate.frames = 0;
    EXPECT_TRUE(loss_estimate_bound(&estimate, true) == 1.0);
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
    node->hybrid.mtu_loss = (struct tt_LossEstimate) {0, 30000, 0};
    node->hybrid.window = (struct tt_RunWindow) {1000, 1000, 45000}; // a collapse, were it judged
    node->hybrid.window_n = 45;
    tick_readers(t += tt_SECOND);
    EXPECT_EQ_U32(FULL_CEILING, node->hybrid.advertised);
    EXPECT_EQ_U64(0, node->hybrid.collapses);
    init_one(LARGE_LEN, FULL_CEILING, false);
    EXPECT_TRUE(!readers[0].ctx.hybrid.cap_off); // the default: capped, from n_max
    EXPECT_EQ_U32(45, readers[0].ctx.hybrid.cap);
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

// p_mtu is p (DESIGN.md section 8, "What the reader measures"): over a stream, the MTU units the reader counts sent and
// lost are the ones the link carried and lost, and so are its runs - a sample that lost every datagram, its runs and
// its tail, included. The reader never sees such a sample; until 2026-10-17 it counted none of it, so p_mtu read
// p (1 - L(n)) for a sample of one run (0.88 p at 1%, 0.56 p at 5%: the remaining false collapses) and the run loss
// a little low. The writer in forced IP and the reader's cap off, so the layout is fixed at the arm's n: best effort,
// one run of 11 and the tail (16 KB), two runs and the tail (16 KB at n = 6), two runs of 45 and 23 and the tail
// (100 KB), at 1% and 5% frame loss; and RELIABLE, one run of 11 and the tail at 5%, where a repair - addressed to the
// reader - is no first transmission (it passed for one: a repaired tail as a tail through, a sample lost whole and
// repaired as a sample sent in MTU units, 721 MTU units counted of 600 sent). Exact up to the stream's first sample and
// its last (its losses not seen yet).
struct pmtu_arm {
    uint32_t len;
    uint32_t bound; // the Publisher's n at most (its ip_datagram_max), 0: the ceiling's
    uint32_t ppm;
    uint32_t run_n;   // the n the arm is meant to send at
    uint32_t seconds; // at 100 Hz
    bool reliable;    // each sample settled (its repairs in) before the next
};

struct pmtu_result {
    uint32_t frames; // the reader's p_mtu: MTU units
    uint32_t events; // and lost ones
    uint32_t runs;   // its q(n): runs of the writer's
    uint32_t runs_lost;
    uint32_t largest; // the largest n the writer sent
};

static struct pmtu_result pmtu_stream(const struct pmtu_arm* arm) {
    _tt_CONFIG.reassembly_cap_off = 1;
    init_one(arm->len, FULL_CEILING, arm->reliable);
    _tt_CONFIG.reassembly_cap_off = 0;
    pub.large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    if (arm->bound != 0) {
        pub.ip_datagram_max = (uint16_t)(28U + (arm->bound * 1452U));
    }
    sim_start(arm->ppm);
    if (arm->reliable) {
        sim_announce();
    }
    tick_readers(test_mock_now);
    uint8_t seed = 1;
    uint32_t largest = 0;
    if (!arm->reliable) {
        largest = sim_stream(arm->seconds, 100, &seed);
    } else {
        sim_pause_heartbeat_ns = SIM_PAUSE_HEARTBEAT_NS;
        uint64_t t = test_mock_now;
        for (uint32_t s = 1; s <= arm->seconds * 100U; s++) {
            memset(seen_unit, 0, sizeof(seen_unit));
            uint64_t at = t + ((uint64_t)s * 10U * tt_MILLISECOND);
            test_mock_now = at > test_mock_now ? at : test_mock_now; // a settle that took longer delays the next
            sent.max_n = 0;
            EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)s));
            sim_settle((int)s, 10U * tt_SECOND); // whole before the next: a late one is checked against its pattern
            largest = sent.max_n > largest ? sent.max_n : largest;
            if (s % 100U == 0) {
                tick_readers(test_mock_now);
            }
        }
        expect_intact_or_given_up(0, (int)(arm->seconds * 100U));
    }
    const struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    struct pmtu_result result = {hybrid->mtu_loss.frames, hybrid->mtu_loss.events, hybrid->rx_writers[0].runs.sent,
                                 hybrid->rx_writers[0].runs.lost, largest};
    large_release_assemblies(&readers[0].ctx); // the last sample, if it lost a datagram: its buffer back (sanitize)
    return result;
}

static uint32_t distance(uint32_t a, uint32_t b) {
    return a > b ? a - b : b - a;
}

static void test_p_mtu_counts_samples_lost_whole(void) {
    static const struct pmtu_arm arms[] = {
        {16384, 0, 10000, 11, 60}, {16384, 0, 50000, 11, 20},     {16384, 7, 10000, 6, 60},
        {16384, 7, 50000, 6, 20},  {LARGE_LEN, 0, 10000, 45, 60}, {16384, 0, 50000, 11, 12, true},
    };
    // Each arm expects 60 or more lost MTU units, so the treatment check (> 20) holds for any loss draw, not only
    // the default seed's: at 20 s arm 4 expected 20 and failed about half of 100 seeds (2026-10-17).
    for (size_t i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
        struct pmtu_result got = pmtu_stream(&arms[i]);
        EXPECT_EQ_U32(arms[i].run_n, got.largest); // the treatment: the arm's layout
        printf(
            "test_hybrid_send: p_mtu arm %zu (%u B, n %u, %.1f%%): reader %u of %u MTU units lost (%.4f), link %u of "
            "%u (%.4f); runs %u of %u, link %u of %u\n",
            i, arms[i].len, arms[i].run_n, arms[i].ppm / 1e4, got.events, got.frames,
            got.frames != 0 ? (double)got.events / got.frames : 0.0, link.mtu_lost, link.mtu,
            link.mtu != 0 ? (double)link.mtu_lost / link.mtu : 0.0, got.runs_lost, got.runs, link.runs_lost, link.runs);
        EXPECT_TRUE(link.mtu_lost > 20U); // the arm had losses to count
        EXPECT_TRUE(distance(got.frames, link.mtu) <= 2U);
        EXPECT_TRUE(distance(got.events, link.mtu_lost) <= 2U);
        // Runs: within 4 or 1% of the link's count, whichever is larger. RELIABLE at 5% read 5 of 600 short on some
        // seeds (the default seed reads 1 or fewer in every arm).
        const uint32_t runs_slack = link.runs / 100U > 4U ? link.runs / 100U : 4U;
        EXPECT_TRUE(distance(got.runs, link.runs) <= runs_slack);
        EXPECT_TRUE(distance(got.runs_lost, link.runs_lost) <= runs_slack);
        EXPECT_EQ_INT(0, readers[0].torn);
    }
}

// The rig's 4 MB cells (2026-10-12): a receive socket that overflows drops runs, and a reader that took those drops for
// path loss advertised a p that held RELIABLE AUTO's n at 4-31 on a clean link. End to end, RELIABLE 1 MB at 30 Hz with
// no frame loss and every tenth run dropped at the reader's socket: counted by the HAL, p stays at no events and n
// leaves MTU for n_max as on a clean link (test_reliable_auto_leaves_mtu_as_clean_frames_accumulate); every sample
// arrives whole and in order, the drops repaired. The control - the same drops, uncounted, a HAL that cannot say -
// reads them as path loss and n ends below n_max. Returns the n of the last sample.
static uint32_t receiver_drop_stream(bool counted) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(0);
    sim_receiver_drop_every = 10;
    sim_receiver_drops_uncounted = !counted;
    uint64_t t = test_mock_now;
    tick_readers(t);
    uint32_t last_n = 0;
    for (uint32_t s = 1; s <= 600; s++) {
        memset(&sent, 0, sizeof(sent));
        memset(seen_unit, 0, sizeof(seen_unit));
        test_mock_now = t + ((uint64_t)s * 34U * tt_MILLISECOND);
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)s));
        sim_settle((int)s, tt_SECOND);
        last_n = sent.max_n;
        if (s % 30U == 0) {
            tick_readers(test_mock_now);
        }
    }
    struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    EXPECT_TRUE(sim_receiver_dropped > 100U); // the treatment: runs were dropped at the socket
    EXPECT_EQ_U64(counted ? sim_receiver_dropped : 0U, hybrid->receiver_lost);
    EXPECT_EQ_U32(counted ? 0U : hybrid->rx_loss.events, hybrid->rx_loss.events);
    EXPECT_TRUE(counted || hybrid->rx_loss.events > 0U);
    EXPECT_EQ_U32(0, pub.peer_watch[0].path_failures);
    expect_intact(0, 600);
    EXPECT_TRUE(readers[0].in_order);
    return last_n;
}

static void test_receiver_drops_do_not_lower_n(void) {
    EXPECT_EQ_U32(45, receiver_drop_stream(true));
    EXPECT_TRUE(receiver_drop_stream(false) < 45U); // the control: a HAL that cannot say
    test_mock_receiver_drops = 0;
}

// Best effort through a path that drops every IP fragment from the start (the tc filter; rule (e) on the reader's
// side): no run of the writer's ever gets through, only the tails. Within a few seconds the reader judges the path -
// its cap starts at n_max (2026-10-13), so each sample is a few runs of 45, which p_mtu's bound from a handful of tails
// cannot yet call a failed path; with the slow start's cap of 2 it took a few samples -
// advertises one unit at once, the writer follows and samples arrive again. After 2 tau the cap it had comes back as a
// re-probe: the filter still there, the next sample fails it at once and the wait doubles; the filter gone, runs arrive
// and it stays. A collapse it is not: the control law's ceiling is untouched.
#define PATH_JUDGED_WITHIN_S 3U
static void test_a_path_that_drops_fragments_brings_n_to_one(void) {
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(0);
    sim_drop_fragmented = true;
    tick_readers(test_mock_now);
    struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    uint8_t seed = 1;
    uint32_t seconds = 0;
    while (hybrid->path_failures == 0 && seconds < 10) {
        (void)sim_stream(1, 30, &seed);
        seconds++;
    }
    EXPECT_EQ_U64(1, hybrid->path_failures);
    EXPECT_TRUE(seconds <= PATH_JUDGED_WITHIN_S);
    EXPECT_EQ_U32(1, hybrid->cap);
    EXPECT_EQ_U32(1, hybrid_units_within(sender.peer_ip_reassembly_max[reader_id(0)], true)); // announced at once
    EXPECT_TRUE(readers[0].delivered > 0 && readers[0].delivered < (int)(30U * seconds));     // all but the first ones
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
    uint64_t generation = 0;
    while (hybrid->collapses == 0 && seconds < 40) {
        generation = readers[0].ctx.last_modified;
        uint32_t n = sim_stream(1, 30, &seed);
        largest = n > largest ? n : largest;
        seconds++;
    }
    EXPECT_EQ_U64(1, hybrid->collapses);
    EXPECT_TRUE(seconds < 30);
    // The memory was full when the collapse was called: the refusals began in the second its tick closed, or before -
    // not frame loss. Since the size rule reads the survival side (2026-10-17) it is often that same second.
    EXPECT_TRUE(sim_reasm_refused > 0);
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

// AUTO from nothing measured at 0% loss, end to end: the first 1 MB sample goes as MTU units - the frames the reader's
// p rests on - and at its tail the reader, 723 clean frames in (tt_HYBRID_NONE_SEEN_FRAMES), announces "none seen" at
// once; from the second sample on every sample goes at n_max, the reader's cap starting there (no slow start). Until
// 2026-10-13 the samples went as MTU units until the reader's first tick (sample 30) and n reached 45 only after about
// 115,000 clean frames, ~6 s at 30 Hz.
// The reader's announce interval falling inside the first sample, after `MID_SAMPLE_FRAMES` of its frames: the reader
// then announces 1 / N of those frames, before "none seen" can be (at the tail).
#define MID_SAMPLE_FRAMES 100U
static bool mid_first_sample_ticked;
static bool tick_mid_first_sample(const struct sim_datagram* d) {
    (void)d;
    struct tt_Context* reader = &readers[0].ctx;
    if (!mid_first_sample_ticked && reader->hybrid.rx_loss.frames >= MID_SAMPLE_FRAMES) {
        mid_first_sample_ticked = true;
        uint8_t acting = sim_acting;
        sim_acting = reader->id;
        hybrid_reader_tick(reader, test_mock_now);
        sim_acting = acting;
    }
    return false;
}

static void test_reliable_auto_leaves_mtu_after_one_clean_sample(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(0);
    uint64_t t = test_mock_now;
    tick_readers(t);
    uint32_t first_runs_at = 0;
    uint32_t last_n = 0;
    uint32_t below_n_max = 0;
    mid_first_sample_ticked = false;
    for (uint32_t s = 1; s <= 300; s++) {
        memset(&sent, 0, sizeof(sent));
        memset(seen_unit, 0, sizeof(seen_unit)); // seq_nos pass SEEN_MAX here: a unit is new to its own sample
        // A second on from the first tick: the interval falls due inside the first sample. 30 a tick after that.
        test_mock_now = t + tt_SECOND + ((uint64_t)s * 34U * tt_MILLISECOND);
        sim_drop_hook = s == 1 ? tick_mid_first_sample : NULL;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)s));
        sim_settle((int)s, tt_SECOND);
        sim_drop_hook = NULL;
        first_runs_at = first_runs_at == 0 && sent.runs != 0 ? s : first_runs_at;
        EXPECT_TRUE(sent.max_n >= last_n || sent.max_n == 0); // n never falls with nothing lost
        last_n = sent.max_n;
        below_n_max += sent.max_n < 45U ? 1U : 0U;
        if (s % 30U == 0) {
            tick_readers(test_mock_now);
        }
    }
    EXPECT_TRUE(mid_first_sample_ticked);
    EXPECT_EQ_U32(2, first_runs_at); // after the first sample's "none seen"
    EXPECT_EQ_U32(1, below_n_max);   // that first sample alone
    EXPECT_EQ_U32(45, last_n);
    EXPECT_TRUE(readers[0].ctx.hybrid.advertised_loss != 0);
    EXPECT_EQ_U32(readers[0].ctx.hybrid.advertised_loss, sender.hybrid.peer_loss_p[reader_id(0)]);
    expect_intact(0, 300);
    EXPECT_TRUE(readers[0].in_order);
}

// A few events early in a stream, then none (the rig's 1 MB RELIABLE cell, 2026-10-13, 0% loss and no socket drop: one
// run in five spent ~393 samples below n_max). Here two runs of the second sample are lost on the path and the stream
// is clean after. The reader reads p ~ 3 / 800 then; with the events kept, p falls only as 1 / frames and n reaches
// n_max only after ~300,000 frames (~400 samples). Forgotten once the clean frames since have outlived them
// (loss_estimate_forget_stale(): two events at F = 813 frames, forgotten at the floor of 9,210 clean frames - 9,309 at
// the tail of sample 14, 12 samples below n_max; the ratio alone waits for 99 F, 80,886 frames, sample 113, and the
// rule before 2026-10-15 took 2,802, sample 5, at a false-forget rate of 6% at two events), n is at n_max at once,
// announced at a tail. The control is the same stream with the rule off
// (hybrid_test_stale_off); the loss draws are identical in both.
#define EARLY_EVENT_SAMPLE 2U
#define EARLY_EVENT_RUNS 2U
#define EARLY_STREAM_SAMPLES 450U
#define EARLY_FORGOTTEN_WITHIN 13U // samples below n_max after the events, the rule on (12; 28 at the next interval)
#define EARLY_HELD_AT_LEAST 200U   // and at least this many with the events kept (the control)
static uint32_t early_sample;
static uint32_t early_dropped;
static bool drop_early_runs(const struct sim_datagram* d) {
    if (d->from != SENDER_ID || d->len <= FRAME_PAYLOAD || early_sample != EARLY_EVENT_SAMPLE ||
        early_dropped >= EARLY_EVENT_RUNS) {
        return false;
    }
    early_dropped++;
    return true;
}

static uint32_t early_events_stream(bool stale_off, uint64_t* forgets) {
    hybrid_test_stale_off = stale_off;
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(0);
    uint64_t t = test_mock_now;
    tick_readers(t);
    uint32_t below_n_max = 0;
    early_dropped = 0;
    for (uint32_t s = 1; s <= EARLY_STREAM_SAMPLES; s++) {
        memset(&sent, 0, sizeof(sent));
        memset(seen_unit, 0, sizeof(seen_unit));
        test_mock_now = t + tt_SECOND + ((uint64_t)s * 34U * tt_MILLISECOND);
        early_sample = s;
        sim_drop_hook = drop_early_runs;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)s));
        sim_settle((int)s, tt_SECOND);
        sim_drop_hook = NULL;
        below_n_max += s > EARLY_EVENT_SAMPLE && sent.max_n < 45U ? 1U : 0U;
        if (s % 30U == 0 || s == EARLY_EVENT_SAMPLE) { // the interval falls just after the events: n drops
            tick_readers(test_mock_now);
        }
    }
    EXPECT_EQ_U32(EARLY_EVENT_RUNS, early_dropped);
    expect_intact(0, (int)EARLY_STREAM_SAMPLES);
    *forgets = readers[0].ctx.hybrid.stale_forgets;
    hybrid_test_stale_off = false;
    return below_n_max;
}

// The rule itself: k events, at most tt_HYBRID_STALE_EVENTS, are forgotten once the clean frames C since the last,
// against the frames F up to it, have (1 + C / F)^k at least 1 / tt_HYBRID_STALE_FALSE_FORGET (1e4), not one frame
// before - or, whatever F, once C reaches the floor tt_HYBRID_STALE_EXPOSURE / tt_HYBRID_STALE_P_MIN, 9,210 frames, not
// one frame before; more events never are, however long the stretch; none, nothing to forget. Forgetting keeps the
// clean stretch as the estimate's frames. The rule before 2026-10-15 (C >= 9.21 F / (k + 1)) forgot one event at F = 50
// after 231 clean frames, which a steady path gives 18% of the time; the ratio alone takes 499,950, the floor 9,210 -
// which a steady path at p >= 1e-3 gives at most 1e-4 of the time.
#define STALE_THREE_NEEDED 4109U // F = 200, k = 3: C >= 200 (10^(4/3) - 1) = 4,108.9, below the floor
#define STALE_ONE_AT 50U
#define STALE_FLOOR 9210U  // 9.21 / 1e-3; F = 50, k = 1: the ratio wants C >= 50 (10^4 - 1) = 499,950
#define STALE_OLD_ONE 231U // F = 50, k = 1: 9.21 * 50 / 2, rounded up
static void test_the_stale_rule_is_the_designs(void) {
    struct tt_LossEstimate estimate = {0, 0, 0, 0};
    loss_estimate_observe(&estimate, 197, 0);
    loss_estimate_observe(&estimate, 3, 3); // three events at 200 frames
    loss_estimate_observe(&estimate, STALE_THREE_NEEDED - 1U, 0);
    EXPECT_TRUE(!loss_estimate_forget_stale(&estimate)); // one frame short
    loss_estimate_observe(&estimate, 1, 0);
    EXPECT_TRUE(loss_estimate_forget_stale(&estimate));
    EXPECT_EQ_U32(0, estimate.events);
    EXPECT_EQ_U32(STALE_THREE_NEEDED, estimate.frames);
    EXPECT_TRUE(!loss_estimate_forget_stale(&estimate)); // no events: nothing to forget
    // One event at 50 frames: the old rule's stretch is not enough, nor one frame short of the floor; the floor is.
    estimate = (struct tt_LossEstimate) {0, 0, 0, 0};
    loss_estimate_observe(&estimate, STALE_ONE_AT, 1);
    loss_estimate_observe(&estimate, STALE_OLD_ONE, 0);
    EXPECT_TRUE(!loss_estimate_forget_stale(&estimate));
    loss_estimate_observe(&estimate, STALE_FLOOR - STALE_OLD_ONE - 1U, 0);
    EXPECT_TRUE(!loss_estimate_forget_stale(&estimate));
    loss_estimate_observe(&estimate, 1, 0);
    EXPECT_TRUE(loss_estimate_forget_stale(&estimate));
    EXPECT_EQ_U32(STALE_FLOOR, estimate.frames);
    // tt_HYBRID_STALE_EVENTS events and one more, over a long clean stretch.
    for (uint32_t events = tt_HYBRID_STALE_EVENTS; events <= tt_HYBRID_STALE_EVENTS + 1U; events++) {
        estimate = (struct tt_LossEstimate) {0, 0, 0, 0};
        loss_estimate_observe(&estimate, 20000, events);
        loss_estimate_observe(&estimate, 400000, 0);
        EXPECT_TRUE(loss_estimate_forget_stale(&estimate) == (events <= tt_HYBRID_STALE_EVENTS));
    }
}

// A gap put down to a receiver drop is not clean (loss_estimate_doubt()): "none seen" takes its
// tt_HYBRID_NONE_SEEN_FRAMES after it, and the stale rule measures both its stretch and F from it - three events at
// 200 frames, a doubt at 400: (1 + C / 400)^3 >= 1e4 at C = 8,218, not (1 + C / 200)^3 at 4,109. The doubt halves with
// the window, and a forget clears it: left at 20,000 after a forget at the floor, it would put the stretch's start past
// the estimate's frames and forget the next events at once.
#define DOUBT_AT 400U
#define DOUBT_THREE_NEEDED 8218U // 400 (10^(4/3) - 1) = 8,217.7
static void test_a_doubtful_gap_is_not_clean(void) {
    struct tt_LossEstimate estimate = {0, 0, 0, 0};
    loss_estimate_observe(&estimate, 300, 0);
    loss_estimate_doubt(&estimate);
    loss_estimate_observe(&estimate, (uint32_t)tt_HYBRID_NONE_SEEN_FRAMES - 1U, 0);
    EXPECT_TRUE(hybrid_rx_loss_code(&estimate) != HYBRID_LOSS_NONE_SEEN);
    loss_estimate_observe(&estimate, 1, 0);
    EXPECT_EQ_U32(HYBRID_LOSS_NONE_SEEN, hybrid_rx_loss_code(&estimate));
    // The stale rule from the doubt.
    estimate = (struct tt_LossEstimate) {0, 0, 0, 0};
    loss_estimate_observe(&estimate, 197, 0);
    loss_estimate_observe(&estimate, 3, 3); // three events at 200 frames
    loss_estimate_observe(&estimate, DOUBT_AT - 200U, 0);
    loss_estimate_doubt(&estimate);
    loss_estimate_observe(&estimate, DOUBT_THREE_NEEDED - 1U, 0);
    EXPECT_TRUE(!loss_estimate_forget_stale(&estimate)); // one frame short
    loss_estimate_observe(&estimate, 1, 0);
    EXPECT_TRUE(loss_estimate_forget_stale(&estimate));
    EXPECT_EQ_U32(DOUBT_THREE_NEEDED, estimate.frames);
    EXPECT_EQ_U32(0, estimate.frames_at_doubt);
    // Halved with the window: ten frames seen since the doubt, before and after.
    estimate = (struct tt_LossEstimate) {0, 0, 0, 0};
    loss_estimate_observe(&estimate, (uint32_t)tt_HYBRID_WINDOW_FRAMES - 10U, 0);
    loss_estimate_doubt(&estimate);
    loss_estimate_observe(&estimate, 20, 0);
    EXPECT_TRUE(estimate.frames - estimate.frames_at_doubt == 10U);
    EXPECT_TRUE(hybrid_rx_loss_code(&estimate) != HYBRID_LOSS_NONE_SEEN);
    // Cleared by a forget.
    estimate = (struct tt_LossEstimate) {0, 0, 0, 0};
    loss_estimate_observe(&estimate, 9999, 0);
    loss_estimate_observe(&estimate, 1, 1); // one event at 10,000 frames
    loss_estimate_observe(&estimate, 10000, 0);
    loss_estimate_doubt(&estimate);                      // a doubt at 20,000
    loss_estimate_observe(&estimate, STALE_FLOOR, 0);    // the floor's stretch after it
    EXPECT_TRUE(loss_estimate_forget_stale(&estimate));  // forgotten, the estimate now the 9,210 frames
    loss_estimate_observe(&estimate, 1, 2);              // two events
    loss_estimate_observe(&estimate, 10, 0);             // and ten clean frames
    EXPECT_TRUE(!loss_estimate_forget_stale(&estimate)); // are not forgotten
    // A reader that has excused no gap announces its first p at the first tail, however few its frames (a small
    // sample's: 2 events in 100).
    init_one(SMALL_LEN, FULL_CEILING, false);
    struct tt_Context* node = &readers[0].ctx;
    struct tt_HybridState* hybrid = &node->hybrid;
    test_mock_receiver_drops = 0;
    loss_estimate_observe(&hybrid->rx_loss, 100, 2);
    hybrid_rx_first_estimate(node);
    EXPECT_EQ_U32(hybrid_loss_code(3.0 / 100.0), hybrid->advertised_loss);
    // One that has announced no p yet: an excused gap restarts its estimate, and its first p waits for
    // tt_HYBRID_NONE_SEEN_FRAMES frames after it - at the tail and at the interval alike.
    init_one(SMALL_LEN, FULL_CEILING, false);
    test_mock_receiver_drops = 0;
    loss_estimate_observe(&hybrid->rx_loss, 300, 2);
    test_mock_receiver_drops = 1;
    EXPECT_EQ_U32(1, hybrid_rx_receiver_lost(node, 1));
    EXPECT_EQ_U32(0, hybrid->rx_loss.events);
    EXPECT_EQ_U32(0, hybrid->rx_loss.frames);
    loss_estimate_observe(&hybrid->rx_loss, (uint32_t)tt_HYBRID_NONE_SEEN_FRAMES - 1U, 1);
    EXPECT_EQ_U32(0, hybrid_rx_announce_code(hybrid));
    hybrid_rx_first_estimate(node);
    EXPECT_EQ_U32(0, hybrid->advertised_loss);
    hybrid_announce(node, test_mock_now); // the interval
    EXPECT_EQ_U32(0, hybrid->advertised_loss);
    loss_estimate_observe(&hybrid->rx_loss, 1, 0);
    EXPECT_TRUE(hybrid_rx_announce_code(hybrid) != 0);
    hybrid_rx_first_estimate(node);
    EXPECT_TRUE(hybrid->advertised_loss != 0 && hybrid->advertised_loss != HYBRID_LOSS_NONE_SEEN);
    // Once announced, an excused gap is a doubt: the estimate stays.
    uint32_t frames = hybrid->rx_loss.frames;
    test_mock_receiver_drops = 2;
    EXPECT_EQ_U32(1, hybrid_rx_receiver_lost(node, 1));
    EXPECT_EQ_U32(frames, hybrid->rx_loss.frames);
    EXPECT_EQ_U32(frames, hybrid->rx_loss.frames_at_doubt);
    EXPECT_EQ_U32(1, hybrid->rx_loss.events);
    test_mock_receiver_drops = 0;
}

static void test_a_few_early_events_are_forgotten(void) {
    uint64_t forgets = 0;
    uint32_t below = early_events_stream(false, &forgets);
    EXPECT_TRUE(below <= EARLY_FORGOTTEN_WITHIN);
    EXPECT_EQ_U64(1, forgets);
    EXPECT_EQ_U64(EARLY_EVENT_RUNS, readers[0].ctx.hybrid.stale_events);
    EXPECT_EQ_U32(0, readers[0].ctx.hybrid.rx_loss.events);
    uint64_t control_forgets = 0;
    uint32_t control = early_events_stream(true, &control_forgets);
    EXPECT_TRUE(control >= EARLY_HELD_AT_LEAST);
    EXPECT_EQ_U64(0, control_forgets);
    printf("test_hybrid_send: early events: %u samples below n_max forgotten, %u kept (the control)\n", below, control);
}

// Loss all along is not a burst: at 1% and 0.1% frame loss a RELIABLE AUTO stream's events are never forgotten - they
// come too often and soon too many - and the stream sends exactly what it sends with the rule off. The draws depend
// only on the frames, so the two arms see the same loss.
#define STEADY_SAMPLES 120U
static void steady_loss_stream(uint32_t loss_ppm, bool stale_off, uint64_t* forgets, uint64_t* digest) {
    hybrid_test_stale_off = stale_off;
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(loss_ppm);
    uint64_t t = test_mock_now;
    tick_readers(t);
    for (uint32_t s = 1; s <= STEADY_SAMPLES; s++) {
        test_mock_now = t + tt_SECOND + ((uint64_t)s * 34U * tt_MILLISECOND);
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)s));
        sim_settle((int)s, 5 * tt_SECOND);
        if (s % 30U == 0) {
            tick_readers(test_mock_now);
        }
    }
    expect_intact(0, (int)STEADY_SAMPLES);
    *forgets = readers[0].ctx.hybrid.stale_forgets;
    *digest = wire.hash;
    hybrid_test_stale_off = false;
}

static void test_steady_loss_is_never_forgotten(void) {
    static const uint32_t losses[] = {10000U, 1000U};
    for (size_t i = 0; i < sizeof(losses) / sizeof(losses[0]); i++) {
        uint64_t forgets = 0;
        uint64_t digest = 0;
        uint64_t control_forgets = 0;
        uint64_t control_digest = 0;
        steady_loss_stream(losses[i], false, &forgets, &digest);
        steady_loss_stream(losses[i], true, &control_forgets, &control_digest);
        EXPECT_EQ_U64(0, forgets);
        EXPECT_EQ_U64(0, control_forgets);
        EXPECT_TRUE(digest == control_digest); // the same wire, byte for byte
    }
}

// Under frame loss, RELIABLE delivers every sample whole and in order, or as KEEP_LAST gives it up (expect_intact_or_
// given_up()), and every repair is an MTU unit - no run is ever resent, no repair datagram is above one frame. Forced
// IP (n = 45 throughout) and AUTO, 1% and 5% frame loss, small and large samples.
static void reliable_under_loss(uint32_t len, uint8_t mode, uint32_t loss_ppm, uint8_t samples) {
    _tt_CONFIG.reassembly_cap_off = mode == tt_LARGE_FRAGMENTATION_IP ? 1 : 0; // a forced arm's reader, as on the rig
    init_one(len, FULL_CEILING, true);
    _tt_CONFIG.reassembly_cap_off = 0;

    pub.large_fragmentation = mode;
    sim_start(loss_ppm);
    sim_pause_heartbeat_ns = SIM_PAUSE_HEARTBEAT_NS; // a sample still owed when the link is quiet (sim_settle())
    sim_announce();
    for (uint8_t s = 1; s <= samples; s++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, 5 * tt_SECOND);
    }
    expect_intact_or_given_up(0, samples);
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
    struct tt_PeerWatch* watch = &pub.peer_watch[0];
    uint32_t entity = readers[0].sub.endpoint.entity_id;
    hybrid_note_nacks(&pub, reader_id(0), entity, 100, (const uint64_t[]) {1}, 1);
    EXPECT_EQ_U32(0, watch->nack_high); // nothing watched
    hybrid_note_exposure(&pub, reader_id(0), 45, 300, 100, 299);
    uint64_t bitmap[2] = {(1ULL << 45U) - 1U, 0}; // 100..144: one run of 45
    hybrid_note_nacks(&pub, reader_id(0), entity, 100, bitmap, 2);
    EXPECT_EQ_U32(144, watch->nack_high);
    EXPECT_EQ_U32(45, watch->watch_nacked);
    hybrid_note_nacks(&pub, reader_id(0), entity, 100, bitmap, 2); // again: re-NACKs
    EXPECT_EQ_U32(45, watch->watch_nacked);
    EXPECT_EQ_U32(144, watch->nack_high);
}

// seq_no wrap safety (seq_delta()): a writer whose units pass 2^32 sends and delivers in runs as one that starts at 1.
// The first sample starts short of 2^32, so its runs cross it, and two sets start at seq_no 0 itself; 1 MB samples go
// in large runs (14), 23-unit ones in small runs (15). Mutants: a run's seq_no range-checked as if it could not wrap
// (such runs refused, the sample torn or repaired as MTU units), the reader's writer slot taken as empty at sample 0 (a
// gap in that sample not seen), the reader's newer-sample test or the watch's first-NACK mark compared unsigned.
static void runs_across_the_wrap(uint32_t len, uint32_t first, bool reliable, uint8_t mode, uint32_t loss_ppm) {
    first_seq_no = first;
    _tt_CONFIG.reassembly_cap_off = mode == tt_LARGE_FRAGMENTATION_IP ? 1 : 0;
    init_one(len, FULL_CEILING, reliable);
    _tt_CONFIG.reassembly_cap_off = 0;
    first_seq_no = 1;
    readers[0].last_seq = (uint16_t)(first - 1U); // on_data's order check: the sample before the first
    pub.large_fragmentation = mode;
    hybrid_test_forced_p = 0.0; // n = n_max from the first sample: what crosses 2^32 is runs
    sim_start(loss_ppm);
    sim_pause_heartbeat_ns = reliable ? SIM_PAUSE_HEARTBEAT_NS : 0U;
    if (reliable) {
        sim_announce();
    }
    memset(&sent, 0, sizeof(sent));
    for (uint8_t s = 1; s <= 4; s++) {
        memset(seen_unit, 0, sizeof(seen_unit));
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        sim_settle(s, 5 * tt_SECOND);
    }
    hybrid_test_forced_p = -1.0;
    EXPECT_TRUE(sent.runs > 0);
    EXPECT_EQ_U32(0, sent.repair_runs);
    if (loss_ppm == 0) {
        expect_intact(0, 4);
    } else {
        expect_intact_or_given_up(0, 4); // at 5% a unit given up now and then (seeds 47 and 50 of 100)
    }
    EXPECT_TRUE(readers[0].in_order);
    EXPECT_EQ_U64(0, readers[0].ctx.frag_dropped); // no run refused
    EXPECT_EQ_U64(0, readers[0].ctx.large.dropped);
    if (loss_ppm == 0) {
        EXPECT_EQ_U32(0, sent.repairs);
        EXPECT_EQ_U32(4U * tt_sample_datagrams(len), readers[0].ctx.hybrid.rx_loss.frames); // every unit once
        EXPECT_EQ_U32(0, readers[0].ctx.hybrid.rx_loss.events);
    }
}

static void test_runs_cross_2_32_as_if_they_had_not(void) {
    runs_across_the_wrap(ONE_MB, 0U - 300U, false, tt_LARGE_FRAGMENTATION_AUTO, 0);
    runs_across_the_wrap(ONE_MB, 0U - 300U, true, tt_LARGE_FRAGMENTATION_AUTO, 0);
    runs_across_the_wrap(ONE_MB, 0U, false, tt_LARGE_FRAGMENTATION_AUTO, 0); // the first sample's own seq_no is 0
    runs_across_the_wrap(ONE_MB, 0U - 300U, true, tt_LARGE_FRAGMENTATION_IP, 10000);
    runs_across_the_wrap(SMALL_LEN * 4U, 0U - 10U, false, tt_LARGE_FRAGMENTATION_AUTO, 0);
    runs_across_the_wrap(SMALL_LEN * 4U, 0U, true, tt_LARGE_FRAGMENTATION_AUTO, 0);
    runs_across_the_wrap(SMALL_LEN * 4U, 0U - 10U, true, tt_LARGE_FRAGMENTATION_IP, 50000);
    // The failed-path watch, with a sample sent in runs from 2^32 - 16: its first NACKs past 2^32 are first ones.
    init_one(ONE_MB, FULL_CEILING, true);
    struct tt_PeerWatch* watch = &pub.peer_watch[0];
    uint32_t entity = readers[0].sub.endpoint.entity_id;
    uint32_t base = 0U - 16U;
    hybrid_note_exposure(&pub, reader_id(0), 45, 300, base, 299);
    uint64_t bitmap[2] = {(1ULL << 45U) - 1U, 0}; // base .. base + 44: one run of 45, 29 of them past 2^32
    hybrid_note_nacks(&pub, reader_id(0), entity, base, bitmap, 2);
    EXPECT_EQ_U32(base + 44U, watch->nack_high);
    EXPECT_EQ_U32(45, watch->watch_nacked);
    hybrid_note_nacks(&pub, reader_id(0), entity, base, bitmap, 2); // again: re-NACKs
    EXPECT_EQ_U32(45, watch->watch_nacked);
    // The reader's side of a sample whose first seq_no is 0: a run of it missing is a run lost.
    struct tt_Context* rx = &readers[0].ctx;
    uint32_t events = rx->hybrid.rx_loss.events;
    hybrid_rx_note(rx, SENDER_ID, pub.endpoint.entity_id, 0U, 0, 100, 45, true);
    hybrid_rx_note(rx, SENDER_ID, pub.endpoint.entity_id, 0U, 90, 100, 9, true); // units 45..89 never came
    EXPECT_EQ_U32(events + 1U, rx->hybrid.rx_loss.events);
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
    EXPECT_EQ_U32(723, pub.peer_watch[0].watch_count);
    EXPECT_EQ_U32(0, pub.peer_watch[1].watch_count);
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
    EXPECT_TRUE(pub.peer_watch[0].path_failed_ns != 0);
    EXPECT_EQ_U32(1, pub.peer_watch[0].path_failures);
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
    EXPECT_EQ_U32(2, pub.peer_watch[0].path_failures);
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
    EXPECT_EQ_U64(0, pub.peer_watch[0].path_failed_ns);
    expect_intact(0, s - 1);
    hybrid_test_forced_p = -1.0;
    EXPECT_TRUE(readers[0].in_order);
}

// The judgement on its own: a sample of 100 units sent as two runs (0-44, 45-89 ... here 0-98 in runs of 45: three)
// and its tail. Every run unit NACKed with the tail too is loss, not a failed path; without the tail it is.
static void test_a_failed_path_needs_the_tail_through(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    struct tt_PeerWatch* watch = &pub.peer_watch[0];
    uint32_t entity = readers[0].sub.endpoint.entity_id;
    hybrid_note_exposure(&pub, reader_id(0), 45, 100, 1000, 99);
    EXPECT_EQ_U32(3, watch->watch_runs);
    uint64_t bitmap[2] = {~0ULL, (1ULL << 36U) - 1U}; // 1000..1099: every run unit and the tail
    hybrid_note_nacks(&pub, reader_id(0), entity, 1000, bitmap, 2);
    EXPECT_EQ_U32(0, watch->path_failures);
    hybrid_note_exposure(&pub, reader_id(0), 45, 100, 2000, 99);
    bitmap[1] = (1ULL << 35U) - 1U; // 2000..2098: the runs, not the tail
    hybrid_note_nacks(&pub, reader_id(0), entity, 2000, bitmap, 2);
    EXPECT_EQ_U32(1, watch->path_failures);
    EXPECT_TRUE(watch->path_failed_ns != 0);
    // A sample one of whose runs got through: no judgement.
    watch->path_failed_ns = 0;
    watch->all_lost_streak = 0;
    hybrid_note_exposure(&pub, reader_id(0), 45, 100, 3000, 99);
    bitmap[0] = 0;
    bitmap[1] = (1ULL << 35U) - 1U; // 3064..3098 only
    hybrid_note_nacks(&pub, reader_id(0), entity, 3000, bitmap, 2);
    EXPECT_EQ_U32(1, watch->path_failures);
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
    EXPECT_EQ_U32(0, pub.peer_watch[0].path_failures);
    expect_intact_or_given_up(0, 40);
    init_one(3500, FULL_CEILING, true);
    pub.large_fragmentation = tt_LARGE_FRAGMENTATION_AUTO;
    hybrid_test_forced_p = 0.0; // n = n_max: one run of two units and the tail
    sim_start(0);
    sim_drop_fragmented = true;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, 5 * tt_SECOND);
    EXPECT_EQ_U32(0, pub.peer_watch[0].path_failures); // one sample, one run: not yet
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
    sim_settle(2, 5 * tt_SECOND);
    EXPECT_EQ_U32(1, pub.peer_watch[0].path_failures); // the second in a row
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
    struct tt_LossEstimate estimate = {0, 0, 0, 0};
    EXPECT_TRUE(loss_estimate_mean(&estimate) == 1.0);
    estimate.frames = 100000;
    EXPECT_TRUE(loss_estimate_mean(&estimate) > 0.0000099 && loss_estimate_mean(&estimate) < 0.0000101);
    estimate.events = 99;
    EXPECT_TRUE(loss_estimate_mean(&estimate) > 0.00099 && loss_estimate_mean(&estimate) < 0.00101);
    // n_max at 0% from about 115,000 clean frames, and kept there: the window halves at 2^20, not below it.
    estimate = (struct tt_LossEstimate) {0, 115000, 0};
    EXPECT_EQ_U32(45, hybrid_n_star(loss_estimate_mean(&estimate), HYBRID_UNIT, HYBRID_UNIT_HEADER, 45));
    estimate.frames = (uint32_t)tt_HYBRID_WINDOW_FRAMES / 2U;
    EXPECT_EQ_U32(45, hybrid_n_star(loss_estimate_mean(&estimate), HYBRID_UNIT, HYBRID_UNIT_HEADER, 45));
}

// --- a full send buffer: resumed when the socket has room (DESIGN.md section 8, "Send cursor") ---------------------

// When the next resume of the sender's large send is due, or 0 for none.
static uint64_t resume_due(void) {
    for (int32_t i = 0; i < sender.scheduler_tail; i++) {
        if (sender.scheduler[i].function == large_send_resume) {
            return sender.scheduler[i].time;
        }
    }
    return 0;
}

// A HAL that reports room (tt_tx_await_writable() true) has a send that filled the buffer resume the moment the wait
// says so - through tt_Context_poll()'s wait, at the same instant - with the timer only as a fallback at
// tt_LARGE_SEND_WRITABLE_FALLBACK_NS; each wait re-arms the report. One that cannot (the control, and FreeRTOS) retries
// on the timer from tt_LARGE_SEND_RETRY_NS as before, and so does a context last polled with timeout 0, which never
// waits and so never sees the report. 1 MB in runs, a send buffer of three datagrams a turn.
static void test_a_full_send_buffer_resumes_when_the_socket_has_room(void) {
    for (int arm = 0; arm < 3; arm++) { // 0: no report, 1: the report, 2: the report but a poll that never waits
        int reports = arm == 1 ? 1 : 0;
        init_one(ONE_MB, FULL_CEILING, false);
        sim_start(0);
        test_mock_tx_writable_supported = arm != 0;
        if (arm == 2) {
            sim_acting = SENDER_ID;
            (void)tt_Context_poll(&sender, 0);
        }
        test_mock_nonblocking_room = 3;
        uint64_t published_at = test_mock_now;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
        EXPECT_EQ_U64(1, sender.large.send_waits);
        EXPECT_EQ_INT(arm != 0 ? 1 : 0, test_mock_tx_writable_arms);
        EXPECT_EQ_U64(published_at + (reports != 0 ? tt_LARGE_SEND_WRITABLE_FALLBACK_NS : tt_LARGE_SEND_RETRY_NS),
                      resume_due());
        if (reports == 0) {
            test_mock_nonblocking_room = -1;
            sim_settle(1, tt_SECOND); // the timer alone takes it the rest of the way
            expect_intact(0, 1);
            EXPECT_EQ_U64(0, sender.large.send_writable);
            test_mock_tx_writable_supported = false;
            continue;
        }
        // Each wait reports room: the send goes on at once, three datagrams a turn, and arms the report again.
        int turns = 0;
        while (readers[0].delivered == 0 && turns < 20) {
            test_mock_nonblocking_room = 3;
            test_mock_receive_reports_writable = true;
            sim_acting = SENDER_ID;
            (void)tt_Context_poll(&sender, (int64_t)tt_MILLISECOND);
            sim_drain();
            turns++;
        }
        test_mock_nonblocking_room = -1;
        expect_intact(0, 1);
        EXPECT_EQ_INT(5, turns);                    // 18 datagrams, 3 a turn: the first 3 went at the publish
        EXPECT_EQ_U64(published_at, test_mock_now); // no timer was waited for
        EXPECT_EQ_U64((uint64_t)turns, sender.large.send_writable);
        EXPECT_EQ_U64((uint64_t)turns, sender.large.send_waits); // one wait per full buffer, the last one ending it
        EXPECT_EQ_INT(turns, test_mock_tx_writable_arms);
        EXPECT_EQ_U64(0, resume_due()); // and no fallback timer left behind
        test_mock_tx_writable_supported = false;
    }
}

// --- the match point under loss (REL_1MB_IP_5's sample 0, 2026-10-11)
// -------------------------------------------------

// The writer's datagrams a test drops: its first `drop_heartbeats` datagrams that begin with a HEARTBEAT (the match
// Heartbeat, alone ahead of a large sample; then the end-of-sample one) and, when drop_first_unit is set, the first
// transmission of the datagram carrying unit 0 of the first sample.
static uint32_t drop_heartbeats;
static bool drop_first_unit;
static uint32_t dropped_heartbeats;
static bool dropped_unit;

static bool drop_the_match_point(const struct sim_datagram* d) {
    if (d->from != SENDER_ID) {
        return false;
    }
    const uint8_t* body = NULL;
    if (datagram_type(d->bytes, &body) == tt_SUBMESSAGE_TYPE_HEARTBEAT) {
        if (dropped_heartbeats < drop_heartbeats) {
            dropped_heartbeats++;
            return true;
        }
        return false;
    }
    uint32_t seq_no = 0;
    uint32_t units = 0;
    bool run = false;
    if (drop_first_unit && !dropped_unit && datagram_units(d->bytes, &seq_no, &units, &run) && seq_no == first_seq_no) {
        dropped_unit = true;
        return true;
    }
    return false;
}

// A RELIABLE KEEP_ALL sample published right after the match is delivered, whatever is lost. Its match Heartbeat goes
// alone ahead of a large sample, and only the datagram holding unit 0 or a HEARTBEAT can be a reader's first contact
// (a continuation names no endpoint). With the match Heartbeat and unit 0 both lost, the end-of-sample HEARTBEAT
// (last = 723) was the first contact and the baseline 724: sample 0 was never asked for, and the reader's ack released
// it at the writer (the netns suite's REL_1MB_IP_5, 2 runs in 10, and REL_4MB_IP_5's one). Arms: the match Heartbeat
// lost (0), unit 0 lost (1) - the controls, which held before the fix; both (2); both and the end-of-sample HEARTBEAT,
// so its chase's resend is the first contact (3). Forced runs (cap off) and forced MTU, 1 MB, three samples, every
// other datagram delivered.
static void test_a_sample_published_at_the_match_is_delivered(void) {
    static const uint8_t modes[] = {tt_LARGE_FRAGMENTATION_IP, tt_LARGE_FRAGMENTATION_MTU};
    static const uint32_t heartbeats[] = {1, 0, 1, 2};
    for (size_t m = 0; m < 2; m++) {
        for (int arm = 0; arm < 4; arm++) {
            _tt_CONFIG.reassembly_cap_off = 1;
            init_one(ONE_MB, FULL_CEILING, true);
            _tt_CONFIG.reassembly_cap_off = 0;
            pub.large_fragmentation = modes[m];
            sim_start(0);
            drop_heartbeats = heartbeats[arm];
            drop_first_unit = arm != 0;
            dropped_heartbeats = 0;
            dropped_unit = false;
            sim_drop_hook = drop_the_match_point;
            for (int s = 0; s < 3; s++) {
                EXPECT_EQ_INT(tt_RET_OK, sim_publish(1)); // one pattern: a torn or misplaced unit shows
            }
            sim_settle(3, 5 * tt_SECOND);
            EXPECT_EQ_U32(drop_heartbeats, dropped_heartbeats); // the treatment was applied
            EXPECT_TRUE(dropped_unit == drop_first_unit);
            EXPECT_EQ_INT(3, readers[0].delivered);
            EXPECT_EQ_INT(0, readers[0].torn);
            EXPECT_TRUE(readers[0].in_order);
            uint32_t owed = 0;
            EXPECT_TRUE(!match_point_owed(&pub, &owed)); // acknowledged: later HEARTBEATs carry no match point
            if (readers[0].delivered != 3) {
                printf("test_hybrid_send: match point, mode %u arm %d: %d of 3 delivered\n", modes[m], arm,
                       readers[0].delivered);
            }
            sim_drop_hook = NULL;
        }
    }
}

// The periodic Heartbeat (tt_Publisher_set_heartbeat_period()) carries the match point too. A 23-unit sample goes as a
// run of 22 and its tail; with its match Heartbeat and the run lost, the tail names no endpoint, and the reader's
// first contact is the periodic Heartbeat (last = 23). Without the match point ahead of it the VOLATILE reader's
// baseline was 24 and the sample was never asked for (sim_settle()'s pause Heartbeat, 2026-10-14). The control arm
// loses the run alone: the match Heartbeat is the first contact, which held before the fix.
static bool first_contact_dropping;
static bool first_contact_match_too;
static uint32_t first_contact_dropped;

static bool drop_the_run_and_the_match_point(const struct sim_datagram* d) {
    if (!first_contact_dropping || d->from != SENDER_ID) {
        return false;
    }
    const uint8_t* body = NULL;
    uint32_t seq_no = 0;
    uint32_t units = 0;
    bool run = false;
    bool drop = datagram_type(d->bytes, &body) == tt_SUBMESSAGE_TYPE_HEARTBEAT
                    ? first_contact_match_too
                    : datagram_units(d->bytes, &seq_no, &units, &run) && run;
    first_contact_dropped += drop ? 1U : 0U;
    return drop;
}

static void test_a_periodic_heartbeat_carries_the_match_point(void) {
    for (int arm = 0; arm < 2; arm++) {
        init_one(SMALL_LEN * 4U, FULL_CEILING, true);
        hybrid_test_forced_p = 0.0; // n = n_max: one run of 22 and the tail
        sim_start(0);
        sim_announce();
        first_contact_dropping = true;
        first_contact_match_too = arm == 0;
        first_contact_dropped = 0;
        sim_drop_hook = drop_the_run_and_the_match_point;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
        first_contact_dropping = false;
        EXPECT_EQ_U32(1, sent.runs); // the treatment: the run lost, and the match Heartbeat with it in arm 0
        EXPECT_EQ_U32(arm == 0 ? 2U : 1U, first_contact_dropped);
        if (arm == 0) {
            EXPECT_EQ_U32(tt_CONTEXT_ID_INVALID, readers[0].sub.writers[0].context_id); // no first contact yet
        }
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, SIM_PAUSE_HEARTBEAT_NS));
        sim_settle(1, 5 * tt_SECOND);
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, 0));
        hybrid_test_forced_p = -1.0;
        sim_drop_hook = NULL;
        expect_intact(0, 1);
        EXPECT_TRUE(sent.repairs > 0); // asked for and repaired: the run never arrived
        if (readers[0].delivered != 1) {
            printf("test_hybrid_send: periodic Heartbeat, arm %d: sample not delivered\n", arm);
        }
    }
}

// The initial HEARTBEAT a re-registration draws carries the match point too. Every new generation of a reader's
// announce re-adds its peer slot on the writer (process_announce()'s forget-then-re-add), which sends that HEARTBEAT
// (send_initial_heartbeat()); a hybrid reader announces again whenever its advertised ceiling or loss moves
// (hybrid_reader_tick()). The same 23-unit sample, its run and match Heartbeat lost, and then the reader announces
// a new generation: the HEARTBEAT it draws (last = 23) is the reader's first contact. Without the match point ahead of
// it the VOLATILE baseline was 24 and the sample was never asked for; nothing else is sent here (no periodic
// Heartbeat). The control arm loses the run alone, as above.
static uint32_t reannounce_heartbeats;

static bool count_writer_heartbeats(const struct sim_datagram* d) {
    const uint8_t* body = NULL;
    if (d->from == SENDER_ID && datagram_type(d->bytes, &body) == tt_SUBMESSAGE_TYPE_HEARTBEAT) {
        reannounce_heartbeats++;
    }
    return false;
}

// A new generation of reader `i`'s announce, as hybrid_reader_tick() makes one when what it advertises moves.
static void sim_reannounce(int i) {
    struct tt_Context* node = &readers[i].ctx;
    node->last_modified = test_mock_now > node->last_modified ? test_mock_now : node->last_modified + 1U;
    sim_acting = node->id;
    (void)build_and_send_update(node, NULL, 0);
    sim_drain();
}

static void test_a_re_registration_carries_the_match_point(void) {
    for (int arm = 0; arm < 2; arm++) {
        init_one(SMALL_LEN * 4U, FULL_CEILING, true);
        hybrid_test_forced_p = 0.0; // n = n_max: one run of 22 and the tail
        sim_start(0);
        sim_announce();
        first_contact_dropping = true;
        first_contact_match_too = arm == 0;
        first_contact_dropped = 0;
        sim_drop_hook = drop_the_run_and_the_match_point;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
        first_contact_dropping = false;
        EXPECT_EQ_U32(1, sent.runs); // the treatment: the run lost, and the match Heartbeat with it in arm 0
        EXPECT_EQ_U32(arm == 0 ? 2U : 1U, first_contact_dropped);
        if (arm == 0) {
            EXPECT_EQ_U32(tt_CONTEXT_ID_INVALID, readers[0].sub.writers[0].context_id); // no first contact yet
        }
        reannounce_heartbeats = 0;
        sim_drop_hook = count_writer_heartbeats;
        sim_reannounce(0);
        sim_settle(1, 5 * tt_SECOND);
        // The treatment: in arm 0 the re-registration drew its HEARTBEAT, the only one this writer sends here (no
        // periodic Heartbeat, no end-of-sample one on 8/9, nothing evicted). In the control arm the sample was repaired
        // and acknowledged before the re-announce, and a writer that retains nothing sends no initial HEARTBEAT.
        EXPECT_TRUE((reannounce_heartbeats > 0) == (arm == 0));
        hybrid_test_forced_p = -1.0;
        sim_drop_hook = NULL;
        expect_intact(0, 1);
        EXPECT_TRUE(sent.repairs > 0); // asked for and repaired: the run never arrived
        if (readers[0].delivered != 1) {
            printf("test_hybrid_send: re-registration, arm %d: sample not delivered\n", arm);
        }
    }
}

// A reader with no discovery table - a core-only one: tt_Context_set_discovery() is opt-in - learns that its writer
// is KEEP_LAST from the writer's next summary. Its writer proxy is made at the first DATA, after the writer's announce
// was read, so nothing it holds says KEEP_LAST and the policy was UNKNOWN for good. That once mattered for delivery:
// UNKNOWN never gave a gap up, and a KEEP_LAST writer stopped resending a sample after tt_RELIABLE_RETRY resends
// without saying it was gone, so a unit whose every resend was lost held the stream behind it for ever (seed 34: unit
// 355 of a 23-unit sample, its run and its three resends lost, then refused ~50 times a second for the rest of the
// run). Since 2026-10-15 neither side gives up on a count; the policy still decides the back-off and the warnings.
// Two 8,000 B samples (a run of 5 and the tail) from a writer that keeps one sample (KEEP_LAST 1), the first one's tail
// lost every time it is sent, the writer's summary between them: the second publish evicts the first, whose tail the
// reader then skips on the writer's eviction HEARTBEAT (gap_evicted, never gap_abandoned), and the second is
// delivered. The control arm gives the reader a table, as rmw_tickle does.
static uint32_t lost_unit;
static uint32_t lost_unit_sends;
static uint32_t lost_unit_drops; // how many of its sends lose_one_unit() drops; UINT32_MAX: every one

static bool lose_one_unit(const struct sim_datagram* d) {
    uint32_t seq_no = 0;
    uint32_t units = 0;
    bool run = false;
    if (d->from == SENDER_ID && datagram_units(d->bytes, &seq_no, &units, &run) && lost_unit - seq_no < units) {
        lost_unit_sends++;
        return lost_unit_sends <= lost_unit_drops;
    }
    return false;
}

// The tail of a sample on 8/9 lost alone leaves the reader nothing showing the gap until the writer says more; the
// periodic Heartbeat says it (sim_settle()'s reasoning). Armed until the reader has asked for the tail again on its
// timer, or has it, then off, so what follows is the reader's own requests and the writer's answers to them.
static void reveal_lost_tail(struct tt_WriterProxy* proxy) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, 10U * tt_MILLISECOND));
    uint64_t since = test_mock_now;
    while (proxy->retry == 0 && seq_delta(proxy->ack_seq_no, lost_unit) <= 0 && test_mock_now - since < tt_SECOND &&
           sim_step()) {
    }
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, 0));
    EXPECT_TRUE(proxy->retry != 0 || seq_delta(proxy->ack_seq_no, lost_unit) > 0);
}

static void test_a_reader_without_a_discovery_table_learns_keep_last(void) {
    for (int arm = 0; arm < 2; arm++) {
        init_one(SMALL_LEN, FULL_CEILING, true);
        cache.sample_depth = 1;     // KEEP_LAST 1: the second sample evicts the first
        hybrid_test_forced_p = 0.0; // n = n_max: one run of 5 and the tail
        sim_start(0);
        sim_no_discovery_table = arm == 0;
        sim_announce();
        sim_no_discovery_table = false;
        lost_unit = first_seq_no + 5U; // the first sample's tail
        lost_unit_sends = 0;
        lost_unit_drops = UINT32_MAX;
        sim_drop_hook = lose_one_unit;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
        struct tt_WriterProxy* proxy = &readers[0].sub.writers[0];
        EXPECT_EQ_U32(SENDER_ID, proxy->context_id); // first contact made, by the run
        // The treatment: a proxy without a table does not know the policy; one with a table does, and has nothing
        // pulled again.
        EXPECT_EQ_INT(arm == 0 ? tt_WRITER_KEEP_ALL_UNKNOWN : tt_WRITER_KEEP_ALL_NO, (int)proxy->keep_all);
        EXPECT_TRUE(readers[0].ctx.update_reprocess[SENDER_ID] == (arm == 0));
        reveal_lost_tail(proxy);
        sim_acting = SENDER_ID;
        send_discovery_summary(&sender); // node_update()'s, every tt_CONTEXT_UPDATE_INTERVAL
        uint64_t since = test_mock_now;
        while (test_mock_now - since < tt_SECOND && sim_step()) {
        }
        EXPECT_EQ_INT(tt_WRITER_KEEP_ALL_NO, (int)proxy->keep_all);
        EXPECT_EQ_U32(0, readers[0].sub.gap_abandoned); // a second of asking, nothing given up
        EXPECT_EQ_INT(0, readers[0].delivered);
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(2));
        sim_settle(1, 5 * tt_SECOND);
        hybrid_test_forced_p = -1.0;
        sim_drop_hook = NULL;
        expect_intact(0, 1); // the second sample
        EXPECT_EQ_U32(0, readers[0].sub.gap_abandoned);
        EXPECT_EQ_U32(1, readers[0].sub.gap_evicted); // the first sample's tail, on the writer's word
        // Resent well past the old cap of tt_RELIABLE_RETRY while the writer held it.
        EXPECT_TRUE(lost_unit_sends > 2U + tt_RELIABLE_RETRY);
        if (readers[0].delivered != 1) {
            printf("test_hybrid_send: no discovery table, arm %d: %d delivered, policy %d\n", arm, readers[0].delivered,
                   (int)proxy->keep_all);
        }
    }
}

// The seed-34 shape, and the user's 2026-10-15 decision: a KEEP_LAST writer repairs a unit for as long as its history
// holds it. The 8,000 B sample's tail is lost on its first `drops` sends - the original and drops - 1 resends - and
// gets through on the next. Arms: the reader knows the writer is KEEP_LAST (a table) or does not (none, and no summary:
// the policy stays UNKNOWN). Treatment: drops 1 + tt_RELIABLE_RETRY + 1, past what the writer used to resend; control:
// drops 2, inside it. Before 2026-10-15 the treatment delivered nothing in either arm: the writer stopped after
// tt_RELIABLE_RETRY resends, the KEEP_LAST reader gave the tail up, the UNKNOWN reader asked for good.
static void test_a_keep_last_writer_repairs_a_unit_while_it_holds_it(void) {
    for (int arm = 0; arm < 4; arm++) {
        bool no_table = (arm & 1) != 0;
        uint32_t drops = (arm & 2) != 0 ? 2U : 2U + (uint32_t)tt_RELIABLE_RETRY;
        init_one(SMALL_LEN, FULL_CEILING, true);
        hybrid_test_forced_p = 0.0; // one run of 5 and the tail
        sim_start(0);
        sim_no_discovery_table = no_table;
        sim_announce();
        sim_no_discovery_table = false;
        lost_unit = first_seq_no + 5U; // the tail
        lost_unit_sends = 0;
        lost_unit_drops = drops;
        sim_drop_hook = lose_one_unit;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
        EXPECT_EQ_INT(no_table ? tt_WRITER_KEEP_ALL_UNKNOWN : tt_WRITER_KEEP_ALL_NO,
                      (int)readers[0].sub.writers[0].keep_all);
        reveal_lost_tail(&readers[0].sub.writers[0]);
        sim_settle(1, 5 * tt_SECOND);
        hybrid_test_forced_p = -1.0;
        sim_drop_hook = NULL;
        expect_intact(0, 1);
        EXPECT_EQ_U32(drops + 1U, lost_unit_sends); // sent until it got through, and not again
        EXPECT_EQ_U32(0, readers[0].sub.gap_abandoned);
        EXPECT_EQ_U32(0, readers[0].sub.gap_evicted);
        if (readers[0].delivered != 1) {
            printf("test_hybrid_send: repaired while held, %s, %u drops: %d delivered, %u sends\n",
                   no_table ? "no table" : "table", drops, readers[0].delivered, lost_unit_sends);
        }
    }
}

// Once the writer no longer holds a unit, the reader hears so at its next request and skips exactly that: KEEP_LAST 1,
// the first sample's tail lost every time, the second sample published (evicting the first) after the reader's
// requests have backed off. The first is skipped on the eviction HEARTBEAT (gap_evicted 1, gap_abandoned 0) within
// one backed-off retry interval of the eviction - not after a count of timeouts - and the second is delivered.
static void test_an_evicted_unit_is_skipped_on_the_heartbeat(void) {
    init_one(SMALL_LEN, FULL_CEILING, true);
    cache.sample_depth = 1;
    // The reader's own back-off is what is counted below; the writer's idle HEARTBEAT (tt_IDLE_HEARTBEAT_PERIOD)
    // prompts one more request per period on top of it, and is tested on its own (test_idle_heartbeat.c).
    pub.idle_heartbeat_ns = 0;
    hybrid_test_forced_p = 0.0;
    sim_start(0);
    sim_announce();
    lost_unit = first_seq_no + 5U;
    lost_unit_sends = 0;
    lost_unit_drops = UINT32_MAX;
    sim_drop_hook = lose_one_unit;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    struct tt_WriterProxy* proxy = &readers[0].sub.writers[0];
    EXPECT_EQ_INT(tt_WRITER_KEEP_ALL_NO, (int)proxy->keep_all);
    reveal_lost_tail(proxy);
    uint64_t since = test_mock_now;
    while (test_mock_now - since < 200U * tt_MILLISECOND && sim_step()) {
    }
    EXPECT_TRUE(proxy->retry > tt_RELIABLE_RETRY + tt_RELIABLE_RETRY_BACKOFF_SHIFT); // backed off to the ceiling
    EXPECT_EQ_U32(lost_unit, proxy->ack_seq_no);                                     // still asking for the tail
    EXPECT_EQ_U32(0, readers[0].sub.gap_abandoned);
    // Requests at the ceiling: about one per 2^shift intervals, not one per interval.
    uint32_t sends_before = lost_unit_sends;
    since = test_mock_now;
    while (test_mock_now - since < 100U * tt_MILLISECOND && sim_step()) {
    }
    uint64_t ceiling = reliable_retry_interval(&readers[0].ctx, proxy) << tt_RELIABLE_RETRY_BACKOFF_SHIFT;
    EXPECT_TRUE(lost_unit_sends - sends_before <= (uint32_t)((100U * tt_MILLISECOND) / ceiling) + 1U);

    uint64_t evicted_at = test_mock_now;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2)); // evicts the first sample
    // Its DATA can show the reader a gap and draw a request at once, answered in the same exchange.
    uint64_t skipped_at = readers[0].sub.gap_evicted != 0 ? test_mock_now : 0;
    since = test_mock_now;
    while (test_mock_now - since < tt_SECOND && sim_step()) {
        if (skipped_at == 0 && readers[0].sub.gap_evicted != 0) {
            skipped_at = test_mock_now;
        }
    }
    hybrid_test_forced_p = -1.0;
    sim_drop_hook = NULL;
    expect_intact(0, 1);
    EXPECT_EQ_U32(1, readers[0].sub.gap_evicted);
    EXPECT_EQ_U32(0, readers[0].sub.gap_abandoned);
    EXPECT_TRUE(skipped_at != 0 && skipped_at - evicted_at <= ceiling);
    printf("test_hybrid_send: evicted unit skipped %llu ns after the eviction (%s; back-off ceiling %llu ns)\n",
           (unsigned long long)(skipped_at - evicted_at), skipped_at != 0 ? "skipped" : "never",
           (unsigned long long)ceiling);
}

// --- runs meet encoding while sending and the lent large sample (DESIGN.md section 8) -------------------------------

// One arm of a writer and its readers: `samples` samples of `len`, each run until every reader has it, the send buffer
// refilled to `room` datagrams before each publish and step (-1: unlimited).
struct arm {
    uint32_t len;
    int readers;
    const uint32_t* ceilings;
    bool reliable;
    uint8_t mode;
    uint32_t loss_ppm;
    int32_t room;
    uint8_t samples;
};

static struct wire_digest run_arm(const struct arm* arm, bool use_ranges) {
    ranged = use_ranges;
    _tt_CONFIG.reassembly_cap_off = arm->mode == tt_LARGE_FRAGMENTATION_IP ? 1 : 0; // a forced arm's reader
    init_set(arm->len, arm->readers, arm->ceilings, arm->reliable);
    _tt_CONFIG.reassembly_cap_off = 0;
    ranged = false;
    pub.large_fragmentation = arm->mode;
    sim_start(arm->loss_ppm);
    for (uint8_t s = 1; s <= arm->samples; s++) {
        test_mock_nonblocking_room = arm->room;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish(s));
        uint64_t since = test_mock_now;
        while (total_delivered() < (int)s * arm->readers && test_mock_now - since < 5U * tt_SECOND) {
            test_mock_nonblocking_room = arm->room;
            if (!sim_step()) {
                test_mock_now += tt_MILLISECOND;
            }
        }
    }
    test_mock_nonblocking_room = -1;
    for (int i = 0; i < arm->readers; i++) {
        expect_intact(i, arm->samples);
    }
    return wire;
}

// Every datagram the writer sends - runs, the MTU tail, HEARTBEATs and MTU repairs - is the same byte for byte, to the
// same address, in the same order, whether the sample was encoded whole first or a send call at a time: in runs of 45
// to one reader (4 MB, 64 runs a call), with a send buffer of three datagrams, RELIABLE in forced IP at 1% frame loss
// (the repairs come from the encoded record), and to two readers by unicast where one takes MTU units and the other
// runs, in both orders (the first destination's send calls encode). The control is the arm without a ranged encode;
// time fields come from the mock clock, so they match only if the two arms behave alike. And the ranged arm really was
// one: each sample encoded by range, and the arm sent runs.
static void test_runs_with_a_ranged_encode_leave_the_wire_byte_identical(void) {
    static const uint32_t one_full[] = {FULL_CEILING};
    static const uint32_t mtu_then_runs[] = {0, FULL_CEILING};
    static const uint32_t runs_then_mtu[] = {FULL_CEILING, 0};
    const struct arm arms[] = {
        {FOUR_MB, 1, one_full, false, tt_LARGE_FRAGMENTATION_AUTO, 0, -1, 2},
        {ONE_MB, 1, one_full, false, tt_LARGE_FRAGMENTATION_AUTO, 0, 3, 2},
        {ONE_MB, 1, one_full, true, tt_LARGE_FRAGMENTATION_IP, 10000, -1, 3},
        {ONE_MB, 2, mtu_then_runs, false, tt_LARGE_FRAGMENTATION_AUTO, 0, -1, 2},
        {ONE_MB, 2, runs_then_mtu, false, tt_LARGE_FRAGMENTATION_AUTO, 0, -1, 2},
    };
    for (size_t i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
        struct wire_digest whole = run_arm(&arms[i], false);
        EXPECT_EQ_U64(0, sender.large.encoded_by_range); // the control has no ranged encode
        struct wire_digest by_range = run_arm(&arms[i], true);
#ifdef HYBRID_SEND_WIRE_PRINT // tests/wire_identity_hybrid.sh: the digests, to compare across builds
        printf("WIRE arm=%zu whole=%016llx/%u/%llu by_range=%016llx/%u/%llu\n", i, (unsigned long long)whole.hash,
               whole.datagrams, (unsigned long long)whole.bytes, (unsigned long long)by_range.hash, by_range.datagrams,
               (unsigned long long)by_range.bytes);
#endif
        EXPECT_EQ_U64(arms[i].samples, sender.large.encoded_by_range);
        EXPECT_TRUE(range_call_count >= 2U * arms[i].samples);
        EXPECT_TRUE(sent.runs + sent.repair_runs > 0); // to a second reader, a run of units already sent counts so
        EXPECT_EQ_U32(whole.datagrams, by_range.datagrams);
        EXPECT_EQ_U64(whole.bytes, by_range.bytes);
        EXPECT_EQ_U64(whole.hash, by_range.hash);
        if (whole.hash != by_range.hash) {
            printf("test_hybrid_send: arm %zu wire differs: %u vs %u datagrams\n", i, whole.datagrams,
                   by_range.datagrams);
        }
        if (arms[i].reliable) {
            EXPECT_TRUE(sent.repairs > 0); // there were repairs, and none was a run
            EXPECT_EQ_U32(0, sent.repair_runs);
        }
    }
}

// The point of encoding while sending, with runs: each send call's bytes are encoded just before that call - through
// the last unit of its last run, no further - and the tail, alone in a call of its own, just before it. 4 MB to one
// reader is fragment 0 at the publish, then 64 runs of 45 two a call (32 calls of 90 units, the call stops once it
// holds LARGE_BATCH units), a run of 8 (a 33rd) and the tail (a 34th). Mutants: runs sent without encoding (torn
// delivery), the whole sample encoded at the first call (the second range ends at 4 MB), or a call that packs runs
// past LARGE_BATCH units (the second range ends past unit 90).
static void test_each_run_call_is_encoded_just_before_it_goes(void) {
    ranged = true;
    init_one(FOUR_MB, FULL_CEILING, false);
    ranged = false;
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    sim_settle(1, tt_SECOND);
    expect_intact(0, 1);
    EXPECT_EQ_U32(66, sent.runs + sent.mtu);
    EXPECT_EQ_U64(1, sender.large.encoded_by_range);
    EXPECT_EQ_U32(35, range_call_count);
    if (range_call_count != 35) {
        return;
    }
    uint32_t end = 0;
    for (uint32_t i = 0; i < 35; i++) {
        uint32_t before = 0; // units on the wire when range i was asked for
        if (i >= 2U) {
            before = i <= 33U ? (i - 1U) * 90U : (32U * 90U) + 8U;
        }
        EXPECT_EQ_U32(end, range_calls[i].offset); // contiguous, once each
        EXPECT_EQ_U32(before, range_calls[i].units_before);
        end = range_calls[i].offset + range_calls[i].length;
        uint32_t want = FOUR_MB;
        if (i == 0) {
            want = LARGE_FIRST_PAYLOAD;
        } else if (i < 33U) {
            want = large_offset(i * 90U);
        } else if (i == 33U) {
            want = large_offset((32U * 90U) + 8U);
        }
        EXPECT_EQ_U32(want, end); // through the last unit the call carries
    }
    // A send buffer of three datagrams: the first call sends its two runs, the second one of its two, and the rest is
    // encoded at once, while the buffer drains - so when the publish returns its record is the whole sample, and
    // nothing is encoded after it.
    ranged = true;
    init_one(FOUR_MB, FULL_CEILING, false);
    ranged = false;
    sim_start(0);
    test_mock_nonblocking_room = 3;
    sim_acting = SENDER_ID;
    sample_seed = 2;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_U32(4, range_call_count);
    EXPECT_TRUE(!range_calls[2].buffer_full);
    EXPECT_EQ_U32(large_offset(180), range_calls[2].offset + range_calls[2].length);
    EXPECT_TRUE(range_calls[3].buffer_full);
    EXPECT_EQ_U32(FOUR_MB, range_calls[3].offset + range_calls[3].length);
    const struct tt_LargeRecord* record = large_record_at(&pub, 0);
    EXPECT_TRUE(!record->sent);
    EXPECT_EQ_U32(record->cdr_len, record->encoded);
    sim_drain();
    for (int step = 0; step < 2000 && total_delivered() < 1; step++) {
        test_mock_nonblocking_room = 3;
        if (!sim_step()) {
            test_mock_now += tt_MILLISECOND;
        }
    }
    test_mock_nonblocking_room = -1;
    expect_intact(0, 1);
    EXPECT_EQ_U32(4, range_call_count);
}

// A sample that came in runs is lent where it was put together (the rmw subscription's 4 MB lend): the callback runs
// with the large lend kind, tt_Sample_retain() keeps the very bytes the decode was handed, and they are the reader's
// own assembly buffer, at the CDR offset - no copy between the runs and the lent sample. The next sample does not
// touch it, and it is released. RELIABLE at 1% frame loss too, so units arrive both in runs and as MTU repairs into the
// one buffer. The control is the forced-MTU arm (n = 1), which is lent the same way.
static void test_a_sample_received_in_runs_is_lent_in_place(void) {
#if tt_SAMPLE_LENDING
    static const uint8_t modes[] = {tt_LARGE_FRAGMENTATION_IP, tt_LARGE_FRAGMENTATION_MTU};
    for (size_t m = 0; m < 2; m++) {
        for (int reliable = 0; reliable < 2; reliable++) {
            _tt_CONFIG.reassembly_cap_off = 1;
            init_one(ONE_MB, FULL_CEILING, reliable != 0);
            _tt_CONFIG.reassembly_cap_off = 0;
            pub.large_fragmentation = modes[m];
            sim_start(reliable != 0 ? 10000 : 0);
            struct reader* r = &readers[0];
            r->retain_next = true;
            EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
            sim_settle(1, 5 * tt_SECOND);
            expect_intact(0, 1);
            EXPECT_EQ_U32(modes[m] == tt_LARGE_FRAGMENTATION_IP ? 1U : 0U, sent.runs > 0 ? 1U : 0U);
            if (reliable != 0) {
                EXPECT_TRUE(sent.repairs > 0);
            }
            EXPECT_EQ_INT(tt_RET_OK, r->retain_result);
            EXPECT_EQ_INT(tt_LEND_LARGE, r->lend_kind);
            EXPECT_TRUE(r->held.payload != NULL && r->held.payload == r->decoded_at);
            EXPECT_EQ_U32(ROUNDUP(ONE_MB), r->held.length);
            bool in_assembly = false;
            for (size_t k = 0; k < sizeof(r->granted) / sizeof(r->granted[0]); k++) {
                in_assembly =
                    in_assembly || (r->granted[k] != NULL && r->held.payload == r->granted[k] + tt_LARGE_CDR_OFFSET);
            }
            EXPECT_TRUE(in_assembly);
            EXPECT_EQ_INT(tt_RET_OK, sim_publish(2)); // a second sample, assembled while the first is held
            sim_settle(2, 5 * tt_SECOND);
            expect_intact(0, 2);
            bool kept = r->held.payload != NULL;
            for (uint32_t i = 0; kept && i < ONE_MB; i++) {
                kept = r->held.payload[i] == pattern(i, 1);
            }
            EXPECT_TRUE(kept);
            EXPECT_EQ_INT(tt_RET_OK, tt_Sample_release(&r->ctx, &r->held));
        }
    }
#endif
}

// --- the first p on a path that loses whole send calls (2026-10-15)
// ---------------------------------------------------
//
// The rig's I1L cell (1 MB RELIABLE KEEP_LAST 10 at 30 Hz, netem 5% on the writer's egress): netem decides its loss
// per packet as the packet is queued, above the segmentation, so a UDP_SEGMENT send of up to 44 MTU datagrams is lost
// or kept whole. The first sample's 723 MTU units - what the reader's first p rests on - were ~22 chances to lose, not
// 723, came through clean in 2 runs of 5, and the reader announced "none seen": n = 45 on a path where a run of 45
// IP fragments, each its own packet, arrives one time in ten. The link here: the writer's MTU datagrams are lost by the
// send call that carried them (a train), every TRAIN_LOSS_EVERY-th train - 5%, deterministically, and none of the
// first sample's trains of 64 - and a fragmented datagram frame by frame at 5%. A datagram sent alone is a train of
// its own.
#define TRAIN_LOSS_EVERY 20U
#define TRAIN_LOST_AT (TRAIN_LOSS_EVERY - 1U)
#define TRAIN_FRAME_LOSS_PPM 50000U
#define TRAIN_SAMPLES 30U
static uint32_t train_call[SIM_QUEUE]; // by queue slot: the send call that carried the datagram
static uint32_t train_last_call;
static uint32_t train_count;
static bool train_lost_now;
static uint32_t train_rng;
static uint32_t train_call_now;
static uint32_t train_call_firsts; // first transmissions of MTU units in the current send call
static uint32_t train_max_firsts;  // the most in one call since the last reset

static void train_capture(const void* buf, size_t len) {
    uint32_t call = (uint32_t)test_mock_send_batch_call_count;
    train_call[sim_tail % SIM_QUEUE] = call;
    uint32_t seq_no = 0;
    uint32_t units = 0;
    bool run = false;
    if (sim_acting == SENDER_ID && datagram_units((const uint8_t*)buf, &seq_no, &units, &run) && !run &&
        seen_unit[seq_no % SEEN_MAX] == 0) {
        if (call != train_call_now) {
            train_call_now = call;
            train_call_firsts = 0;
        }
        train_call_firsts++;
        train_max_firsts = train_call_firsts > train_max_firsts ? train_call_firsts : train_max_firsts;
    }
    sim_capture(buf, len);
}

static bool train_drop(const struct sim_datagram* d) {
    if (d->from != SENDER_ID) {
        return false;
    }
    uint32_t frames = (d->len + 8U + FRAME_PAYLOAD - 1U) / FRAME_PAYLOAD;
    if (frames > 1U) {
        bool lost = false;
        for (uint32_t i = 0; i < frames; i++) {
            train_rng = (train_rng * 1103515245U) + 12345U;
            lost = lost || ((train_rng >> 8) & 0xFFFFFFU) % 1000000U < TRAIN_FRAME_LOSS_PPM;
        }
        return lost;
    }
    uint32_t call = train_call[(uint32_t)(d - sim_queue) % SIM_QUEUE];
    if (call != train_last_call) {
        train_last_call = call;
        train_count++;
        train_lost_now = train_count % TRAIN_LOSS_EVERY == TRAIN_LOST_AT;
    }
    return train_lost_now;
}

// Until a reader's first p is heard, the frames it will rest on go one a send call, never as a train; the reader
// announces that first p at the first sample's tail, whatever it reads, and the writer goes back to trains. On the
// train-losing link above, before 2026-10-15: the first sample's 12 trains all arrived, "none seen", and samples 2 to
// 30 went at n = 45. Now its 723 datagrams are 723 trains, ~36 lost, p ~ 5%, and no sample goes at n > 1.
static void test_the_first_p_rests_on_frames_not_trains(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(0);
    test_mock_send_hook = train_capture;
    sim_drop_hook = train_drop;
    train_last_call = UINT32_MAX;
    train_count = 0;
    train_lost_now = false;
    train_rng = 7;
    train_call_now = UINT32_MAX;
    sim_announce();
    uint64_t t = test_mock_now;
    tick_readers(t);
    uint32_t at_runs = 0;
    uint32_t first_alone = 0;
    uint32_t second_alone = 0;
    uint16_t heard_before_second = 0;
    for (uint32_t s = 1; s <= TRAIN_SAMPLES; s++) {
        memset(&sent, 0, sizeof(sent));
        train_max_firsts = 0;
        train_call_now = UINT32_MAX;
        test_mock_now = t + (tt_SECOND / 2U) + ((uint64_t)s * 34U * tt_MILLISECOND); // no reader tick in between
        heard_before_second = s == 2 ? sender.hybrid.peer_loss_p[reader_id(0)] : heard_before_second;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)s));
        first_alone = s == 1 ? train_max_firsts : first_alone;
        second_alone = s == 2 ? train_max_firsts : second_alone;
        sim_settle((int)s, tt_SECOND);
        at_runs += sent.max_n >= 2U ? 1U : 0U;
    }
    EXPECT_EQ_U32(1, first_alone);               // the first sample: one MTU unit a send call
    EXPECT_TRUE(heard_before_second != 0);       // its tail announced the reader's first p
    EXPECT_TRUE(second_alone > 1U);              // and the second went in trains again
    EXPECT_TRUE(train_count > TRAIN_LOSS_EVERY); // the treatment: trains were lost
    EXPECT_EQ_U32(0, at_runs);                   // no sample at n > 1 on a 5% path
    EXPECT_TRUE(readers[0].ctx.hybrid.rx_loss.events > 0);
    expect_intact_or_given_up(0, (int)TRAIN_SAMPLES);
    sim_drop_hook = NULL;
    test_mock_send_hook = sim_capture;
}

// --- the first p, measured to a reader the writer has not heard announce (2026-10-15) ------------------------------
//
// The rig's I1L cell again, after the one-a-call fix: 1 run of 8 still sent 4 samples at n = 45 (head r1). The writer,
// performance_test under rmw_tickle, publishes as it starts, and may learn its reader from the reader's ACKNACK
// (claim_from_acknack()) before the reader's announce is in: it then knows no ceiling for it (n_max 1), so
// hybrid_peer_unmeasured() did not count it, the sample went as trains, the reader - which had heard the writer's
// announce - measured it, and "none seen" at its tail was the writer's first p. The link: the reader's every datagram
// to the writer is lost until the first sample is out (on the rig its announces were lost in the writer's flooded
// well-known socket), then its announce goes; the writer's MTU datagrams are lost by train as above. Before the fix:
// the first sample's 12 trains all arrive, "none seen", and samples 2 to 30 go at n = 45. (The broadcast case first-n2
// also covered - a first sample sent before any reader is matched - is gone: a Publisher with no matched reader sends
// nothing, test_no_send_unmatched.c.)
static bool readers_muted;

static bool mute_readers_or_drop_trains(const struct sim_datagram* d) {
    if (readers_muted && d->from != SENDER_ID) {
        return true;
    }
    return train_drop(d);
}

static void test_the_first_p_rests_on_frames_not_trains_to_a_reader_claimed_from_its_acknack(void) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(0);
    uint8_t rid = reader_id(0);
    sender.peer_ip_reassembly_max[rid] = 0; // no announce of the reader's has arrived; it is matched as if claimed
    memset(&sim_discovery[0], 0, sizeof(sim_discovery[0]));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&readers[0].ctx, &sim_discovery[0], NULL, NULL));
    test_mock_send_hook = train_capture;
    sim_drop_hook = mute_readers_or_drop_trains;
    train_last_call = UINT32_MAX;
    train_count = 0;
    train_lost_now = false;
    train_rng = 7;
    readers_muted = true;
    uint64_t since = test_mock_now; // the writer's announce reaches the reader; its reply is lost
    while (readers[0].ctx.update_last_seen[SENDER_ID] == 0 && test_mock_now - since < tt_SECOND && sim_step()) {
    }
    EXPECT_TRUE(readers[0].ctx.update_last_seen[SENDER_ID] != 0);
    EXPECT_EQ_U64(0, sender.update_last_seen[rid]);
    uint64_t t = test_mock_now;
    tick_readers(t);
    memset(&sent, 0, sizeof(sent));
    train_max_firsts = 0;
    train_call_now = UINT32_MAX;
    test_mock_now = t + (tt_SECOND / 2U);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1));
    uint32_t first_alone = train_max_firsts;
    // The treatment: the first sample went by unicast to the claimed reader.
    EXPECT_TRUE(sent.broadcast == 0 && sent.to_reader[0] > 0);
    EXPECT_EQ_U32(0, sender.hybrid.peer_loss_p[rid]);
    readers_muted = false;
    test_mock_now += tt_CONTEXT_UPDATE_INTERVAL;
    sim_acting = rid;
    (void)build_and_send_update(&readers[0].ctx, NULL, 0); // its announce gets through at last
    since = test_mock_now;
    while (sender.update_last_seen[rid] == 0 && test_mock_now - since < tt_SECOND && sim_step()) {
    }
    EXPECT_TRUE(sender.update_last_seen[rid] != 0);
    sim_settle(1, tt_SECOND);
    uint32_t at_runs = 0;
    for (uint32_t s = 2; s <= TRAIN_SAMPLES; s++) {
        memset(&sent, 0, sizeof(sent));
        test_mock_now = t + (tt_SECOND / 2U) + ((uint64_t)s * 34U * tt_MILLISECOND); // no reader tick in between
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)s));
        sim_settle((int)s, tt_SECOND);
        at_runs += sent.max_n >= 2U ? 1U : 0U;
    }
    EXPECT_EQ_U32(1, first_alone);               // the first sample: one MTU unit a send call
    EXPECT_TRUE(train_count > TRAIN_LOSS_EVERY); // the treatment: trains were lost
    EXPECT_EQ_U32(0, at_runs);                   // no sample at n > 1 on a 5% path
    EXPECT_TRUE(readers[0].ctx.hybrid.rx_loss.events > 0);
    expect_intact_or_given_up(0, (int)TRAIN_SAMPLES);
    sim_drop_hook = NULL;
    test_mock_send_hook = sim_capture;
}

// --- a socket that overflows during the reader's first sample (2026-10-16, rig first-n2 r11) -----------------------
//
// The reader's socket dropped 158 datagrams during its first sample (its well-known socket flooded; "receiver_drops=158
// receiver_lost=158 ... stale_forgets=1 stale_events=4"), on a 5% path. The drops were not this sample's datagrams, but
// tt_rx_receiver_drops() cannot say whose they were, so hybrid_rx_receiver_lost() put the sample's own path losses down
// to them, one each, until the 158 were used up: the reader saw a few events early in the sample and then a "clean"
// stretch made of gaps it had excused. Before 2026-10-16 that stretch was read as clean: the stale rule forgot the few
// events (flood at 150 frames: 26 samples at n = 45), or, a flood before the first event, no event was seen at all (3
// samples at n = 45) - "none seen" either way, on a 5% path. A gap put down to a receiver drop is a stretch the reader
// did not see, not a clean one: before its first p it restarts the estimate, and that p waits for 500 frames after
// it (hybrid_rx_announce_code()); after, neither "none seen" nor the stale rule may rest on frames before it
// (loss_estimate_doubt()). The drops are counted as the hook sees flood_at_frames of the reader's frames; the control
// is the same link with no flood, whose draws are identical. A writer that heard no p at all (the reader's announce
// lost on the 5% link: 7 of 100 seeds of hybrid_seed_sweep.sh, the control too) sends MTU units: that passes.
#define FLOOD_DROPS 158U
#define FLOOD_SAMPLES 60U
static uint32_t flood_at_frames;
static bool flood_counted;
static bool flood_the_socket(const struct sim_datagram* d) {
    (void)d;
    if (!flood_counted && readers[0].ctx.hybrid.rx_loss.frames >= flood_at_frames) {
        flood_counted = true;
        test_mock_receiver_drops += FLOOD_DROPS; // datagrams of someone else's, dropped at this reader's socket
    }
    return false;
}

struct flood_result {
    uint16_t first_p;       // the first p the writer heard
    uint32_t first_p_at;    // before this sample
    uint32_t second_n;      // the second sample's n
    uint32_t at_n_max;      // samples at n = 45
    uint32_t at_runs;       // samples at n > 1
    uint64_t stale_forgets; // the reader's
    uint64_t receiver_lost; // gaps put down to receiver drops: the treatment
};

static struct flood_result flood_stream(bool flood, uint32_t at_frames) {
    init_one(ONE_MB, FULL_CEILING, true);
    sim_start(50000);
    sim_pause_heartbeat_ns = SIM_PAUSE_HEARTBEAT_NS;
    sim_announce();
    flood_at_frames = at_frames;
    flood_counted = !flood;
    sim_drop_hook = flood_the_socket;
    struct flood_result result = {0, 0, 0, 0, 0, 0, 0};
    uint64_t t = test_mock_now;
    tick_readers(t);
    for (uint32_t s = 1; s <= FLOOD_SAMPLES; s++) {
        memset(&sent, 0, sizeof(sent));
        memset(seen_unit, 0, sizeof(seen_unit));
        test_mock_now = t + (tt_SECOND / 2U) + ((uint64_t)s * 34U * tt_MILLISECOND);
        if (result.first_p == 0 && sender.hybrid.peer_loss_p[reader_id(0)] != 0) {
            result.first_p = sender.hybrid.peer_loss_p[reader_id(0)];
            result.first_p_at = s;
        }
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)s));
        sim_settle((int)s, 5 * tt_SECOND);
        result.second_n = s == 2 ? sent.max_n : result.second_n;
        result.at_n_max += sent.max_n >= 45U ? 1U : 0U;
        result.at_runs += sent.max_n >= 2U ? 1U : 0U;
        if (s % 30U == 0) {
            tick_readers(test_mock_now);
        }
    }
    EXPECT_TRUE(flood_counted);
    expect_intact_or_given_up(0, (int)FLOOD_SAMPLES);
    result.stale_forgets = readers[0].ctx.hybrid.stale_forgets;
    result.receiver_lost = readers[0].ctx.hybrid.receiver_lost;
    sim_drop_hook = NULL;
    printf("test_hybrid_send: flood=%d at %u frames: first p %.4g (code 0x%04x) before sample %u, second n %u, %u "
           "samples at n > 1, "
           "%u at n_max, stale_forgets %lu, receiver_lost %lu\n",
           flood ? 1 : 0, at_frames, hybrid_loss_decode(result.first_p), result.first_p, result.first_p_at,
           result.second_n, result.at_runs, result.at_n_max, (unsigned long)result.stale_forgets,
           (unsigned long)result.receiver_lost);
    return result;
}

static void test_receiver_drops_never_make_a_lossy_path_look_clean(void) {
    struct flood_result control = flood_stream(false, 0);
    EXPECT_EQ_U64(0, control.receiver_lost);
    EXPECT_TRUE(control.first_p != HYBRID_LOSS_NONE_SEEN && hybrid_loss_decode(control.first_p) > 0.02);
    EXPECT_EQ_U32(0, control.at_runs);
    static const uint32_t ats[] = {0U, 150U}; // before the first event: "none seen"; after a few: forgotten (r11)
    for (size_t i = 0; i < sizeof(ats) / sizeof(ats[0]); i++) {
        struct flood_result flooded = flood_stream(true, ats[i]);
        EXPECT_TRUE(flooded.receiver_lost > 0); // the treatment: path losses were put down to the drops
        EXPECT_TRUE(flooded.first_p != HYBRID_LOSS_NONE_SEEN && hybrid_loss_decode(flooded.first_p) > 0.02);
        EXPECT_EQ_U64(0, flooded.stale_forgets);
        EXPECT_TRUE(flooded.second_n < 2U); // MTU units until the reader's first p rests on frames it saw
        EXPECT_EQ_U32(0, flooded.at_n_max);
        EXPECT_EQ_U32(0, flooded.at_runs); // and no sample at n > 1 on a 5% path, as the control
    }
}

int main(void) {
    test_small_sample_goes_as_one_run_and_its_tail();
    test_large_samples_go_in_runs_of_n_max();
    test_a_send_call_carries_about_large_batch_units();
    test_the_drain_is_sized_for_runs();
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
    test_a_gap_goes_by_its_own_samples_layout();
    test_a_sample_lost_whole_is_counted();
    test_receiver_drops_are_not_path_loss();
    test_receiver_drops_do_not_lower_n();
    test_the_loss_code_rounds_up();
    test_none_seen_takes_its_frames();
    test_the_path_rule_needs_frame_loss_to_be_no_excuse();
    test_a_reader_with_the_cap_off_advertises_its_ceiling();
    test_the_loss_bound_is_the_poisson_upper_limit();
    test_frame_loss_never_moves_the_cap();
    test_p_mtu_counts_samples_lost_whole();
    test_a_path_that_drops_fragments_brings_n_to_one();
    test_a_reassembly_squeeze_end_to_end();
    test_n_follows_p();
    test_reliable_auto_leaves_mtu_after_one_clean_sample();
    test_the_stale_rule_is_the_designs();
    test_a_doubtful_gap_is_not_clean();
    test_a_few_early_events_are_forgotten();
    test_steady_loss_is_never_forgotten();
    test_reliable_repairs_are_mtu_units_only();
    test_nacks_are_read_for_the_watch();
    test_mixed_reliable_readers_each_get_their_own_n();
    test_keep_all_bounds_are_unchanged();
    test_reliable_through_a_path_that_drops_fragments();
    test_runs_cross_2_32_as_if_they_had_not();
    test_frame_loss_is_not_a_failed_path();
    test_a_failed_path_needs_the_tail_through();
    test_the_objective_is_wire_bytes();
    test_the_objective_reads_the_posterior_mean();
    test_a_sample_published_at_the_match_is_delivered();
    test_a_periodic_heartbeat_carries_the_match_point();
    test_a_re_registration_carries_the_match_point();
    test_a_reader_without_a_discovery_table_learns_keep_last();
    test_a_keep_last_writer_repairs_a_unit_while_it_holds_it();
    test_an_evicted_unit_is_skipped_on_the_heartbeat();
    test_a_full_send_buffer_resumes_when_the_socket_has_room();
    test_runs_with_a_ranged_encode_leave_the_wire_byte_identical();
    test_each_run_call_is_encoded_just_before_it_goes();
    test_a_sample_received_in_runs_is_lent_in_place();
    test_the_first_p_rests_on_frames_not_trains();
    test_the_first_p_rests_on_frames_not_trains_to_a_reader_claimed_from_its_acknack();
    test_receiver_drops_never_make_a_lossy_path_look_clean();
    free(sim_queue);
    printf("test_hybrid_send: %u samples owed when the link went quiet, asked for at the periodic Heartbeat\n",
           sim_pause_heartbeats);
    if (test_result() != 0) {
        return 1;
    }
    printf("test_hybrid_send: all tests passed\n");
    return 0;
}
