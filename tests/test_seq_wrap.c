/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// seq_no past 2^32. A writer's seq_no is a 32-bit counter that wraps to 0 and goes on; every comparison of two seq_no
// is serial (tickle.c's seq_delta()), nothing takes seq_no 0 for "none", and the rings indexed by seq_no stay
// contiguous across the wrap. These tests start writers just short of 2^32 so the wrap falls inside the run.
//
// Each wrap test has a control that cannot see the wrap: the same run started at seq_no 1 (or a ring whose depth
// divides 2^32), and a wrap run is judged against its control, not against expectations of its own.

// Room for 10 fragments, so the end-to-end runs cover fragmented samples too (as test_data_frag.c builds).
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

// --- the writer's cache ring ----------------------------------------------------------------------

static struct tt_Context cache_node;

// Caches a `len`-byte DATA record for seq_no whose bytes after the submessage header are all (uint8_t)seq_no
// (test_reliable_pubsub.c's own helper, with a real type: a fragmenting build evicts a FRAG_CONT with its sample).
static void cache_write(struct tt_ReliableCache* cache, uint32_t seq_no, uint32_t len) {
    struct tt_SubmessageHeader* header = (struct tt_SubmessageHeader*)cache_node.tx_buffer;
    memset(cache_node.tx_buffer, (int)(seq_no & 0xFFU), ROUNDUP(len));
    header->type = tt_SUBMESSAGE_TYPE_DATA;
    cache_node.tx_tail = len;
    cache_reliable_sample(&cache_node, header, cache, seq_no, len);
}

// Whether seq_no is retained with the bytes it was written with, found where every lookup finds it.
static bool cache_holds(const struct tt_ReliableCache* cache, uint32_t seq_no) {
    uint16_t depth = reliable_cache_depth(cache);
    const struct tt_ReliableCacheIndex* entry = reliable_cache_slot(cache, depth, seq_no);
    if (cache->slot_origin == 0 || entry->seq_no != seq_no || entry->len == 0) {
        return false;
    }
    for (uint32_t i = sizeof(struct tt_SubmessageHeader); i < entry->len; i++) {
        if (cache->arena[entry->offset + i] != (uint8_t)(seq_no & 0xFFU)) {
            return false;
        }
    }
    return true;
}

// Writes seq_no first .. first + count - 1 into a cache of `depth` slots, with room for every record, and after each
// write checks the last `depth` written are all held, intact - KEEP_LAST by count - and that oldest/newest name them.
// Returns how many checks failed.
static uint32_t cache_run(uint16_t depth, uint32_t first, uint32_t count) {
    static struct tt_ReliableCacheIndex index[64];
    static uint8_t arena[tt_RELIABLE_CACHE_ARENA_BYTES(64, tt_RELIABLE_RECORD_BYTES(512))];
    struct tt_ReliableCache cache;
    memset(index, 0, sizeof(index));
    memset(&cache, 0, sizeof(cache));
    cache.index = index;
    cache.capacity = depth;
    cache.depth = depth;
    cache.arena = arena;
    cache.arena_size = (uint32_t)sizeof(arena);
    uint32_t failures = 0;
    for (uint32_t n = 0; n < count; n++) {
        uint32_t seq_no = first + n;
        cache_write(&cache, seq_no, 16 + ((seq_no * 7U) % 40U));
        uint32_t held = n + 1 < depth ? n + 1 : depth;
        for (uint32_t back = 0; back < held; back++) {
            failures += cache_holds(&cache, seq_no - back) ? 0U : 1U;
        }
        failures += cache.oldest_seq_no == seq_no - (held - 1U) && cache.newest_seq_no == seq_no ? 0U : 1U;
    }
    return failures;
}

static void test_cache_ring_stays_contiguous_across_the_wrap(void) {
    // 13 slots: 2^32 % 13 = 9, so (seq_no - 1) % 13 jumps by 9 slots where seq_no passes 2^32, and seq_no 1 would
    // land on a slot still holding a sample from before the wrap. 16 divides 2^32: the control, which no rebase is
    // needed for. Both run from 40 short of 2^32 to 40 past it, and from 1 as the stream that never wraps.
    EXPECT_EQ_U32(0, cache_run(13, 0xFFFFFFFFU - 39U, 80));
    EXPECT_EQ_U32(0, cache_run(16, 0xFFFFFFFFU - 39U, 80));
    EXPECT_EQ_U32(0, cache_run(13, 1, 80));
    // seq_no 0 is a sample like any other: retained, found, and the cache not read as empty while it is the oldest.
    EXPECT_EQ_U32(0, cache_run(3, 0xFFFFFFFEU, 6));
}

static void test_cache_ring_rebases_without_moving_a_sample(void) {
    // From seq_no 1 the slots are (seq_no - 1) % depth (slot_origin 1). When seq_no - 1 reaches 2^31 the base moves on
    // by a multiple of depth while samples are retained, and every one of them must still be found where it was:
    // a run across 2^31, at a depth that divides 2^32 and one that does not.
    EXPECT_EQ_U32(0, cache_run(13, 0x80000000U - 20U, 40));
    EXPECT_EQ_U32(0, cache_run(16, 0x80000000U - 20U, 40));
    static struct tt_ReliableCacheIndex index[13];
    static uint8_t arena[tt_RELIABLE_CACHE_ARENA_BYTES(13, tt_RELIABLE_RECORD_BYTES(512))];
    struct tt_ReliableCache cache;
    memset(index, 0, sizeof(index));
    memset(&cache, 0, sizeof(cache));
    cache.index = index;
    cache.capacity = 13;
    cache.depth = 13;
    cache.arena = arena;
    cache.arena_size = (uint32_t)sizeof(arena);
    for (uint32_t seq_no = 1; seq_no < 30; seq_no++) {
        cache_write(&cache, seq_no, 24);
    }
    EXPECT_EQ_U32(1, cache.slot_origin); // control: never wrapped, slots as they always were
    EXPECT_TRUE(&index[(29U - 1U) % 13U] == reliable_cache_slot(&cache, 13, 29));
    for (uint32_t seq_no = 0x80000000U - 5U; seq_no != 0x80000000U + 5U; seq_no++) {
        cache_write(&cache, seq_no, 24);
    }
    EXPECT_TRUE(cache.slot_origin != 1 && (cache.slot_origin - 1U) % 13U == 0); // treatment: moved, by whole rings
}

static void test_cache_ring_origin_is_never_0(void) {
    // slot_origin 0 means "nothing retained" (reliable_cache_retains()), so a rebase must never land there. With 255
    // slots (2^32 - 1 = 255 x 16843009) one can: from origin 2^32 - 255q, seq_no 2^30 is 2^31 + 191 on and the move of
    // 255q whole rings would reach exactly 2^32. The origin must go one ring further instead - still by whole rings,
    // still within 2^31 of seq_no. The control: from origin 1, where the same move lands nowhere near 0.
    const uint32_t rings = 4210753U; // 255 x rings = 2^30 + 191
    const uint32_t from[] = {0U - (255U * rings), 1U};
    const uint32_t seq_no[] = {0x40000000U, 0x80000000U + 1U};
    for (int c = 0; c < 2; c++) {
        uint32_t origin = reliable_cache_place_origin(from[c], 255, seq_no[c]);
        EXPECT_TRUE(origin != 0);
        EXPECT_EQ_U32(0, (origin - from[c]) % 255U);             // whole rings
        EXPECT_TRUE(seq_delta(seq_no[c], origin) >= 0x3FFF0000); // about 2^30 behind seq_no, as any rebase leaves it
    }
}

// --- end to end: a writer and readers over a simulated lossy link -----------------------------------
//
// test_data_frag.c's simulation: what each node's HAL is handed goes into a queue, dropped loss_per_mille of the time
// by a seeded generator - and, for the cases that need a loss at a given place, whichever of the writer's datagrams
// fall in a range of send order - and is fed to the other node's process_packet(); timers run in simulated time. The
// writer's first seq_no is set before it publishes anything. Every sample carries its index, and each reader checks
// it gets each once, in order.

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define RECEIVER_IP 0x0a000002
#define PORT 8282
#define TOPIC_NAME "wrap_topic"
#define ENDPOINT_NAME "wrap_endpoint"
#define SIM_QUEUE 8192
#define SIM_SAMPLES 200
#define SIM_PUBLISH_SPACING (100 * tt_MICROSECOND)
#define READERS 2

static uint32_t sample_len;
static uint32_t sample_index; // the index the next published sample carries

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

// What one reader saw. decode runs before the callback, and only the callback knows which reader it is.
struct reader_seen {
    int delivered;
    bool in_order; // every delivery's index above the one before, its bytes intact
    uint32_t last_index;
};
static struct reader_seen seen[READERS];
static uint32_t decoded_index;
static bool decoded_intact;
static int delivered_count; // every reader together

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

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_Topic sender_topic;
static struct tt_Topic receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;  // the reader every case has
static struct tt_Subscriber sub2; // a second reader on the same node, for the cases that ask for one

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)seq_no;
    (void)data;
    struct reader_seen* r = &seen[s == &sub ? 0 : 1];
    r->in_order = r->in_order && decoded_intact && (r->delivered == 0 || decoded_index > r->last_index);
    r->last_index = decoded_index;
    r->delivered++;
    delivered_count++;
}

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

struct sim_datagram {
    uint8_t from;
    bool dropped; // picked to be lost (sim_drop_from, sim_drop_count)
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
static uint32_t sim_drop_from; // the writer's datagrams sim_drop_from .. + sim_drop_count - 1, by send order, are lost
static uint32_t sim_drop_count;
static bool sim_mute; // every datagram is lost, both ways (nobody is listening yet)

static void sim_capture(const void* buf, size_t len) {
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    uint64_t ordinal = sim_sent[sim_acting];
    d->from = sim_acting;
    d->dropped =
        sim_mute || (sim_acting == SENDER_ID && ordinal >= sim_drop_from && ordinal - sim_drop_from < sim_drop_count);
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
    sim_sent[sim_acting]++;
}

static bool sim_lost(void) {
    sim_rng = (sim_rng * 1103515245U) + 12345U;
    return ((sim_rng >> 16) % 1000U) < sim_loss_per_mille;
}

static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        bool lost =
            sim_lost(); // drawn for every datagram alike, so a picked loss leaves the rest of the draw as it was
        if (lost || d->dropped) {
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

// Neither depth nor slots divide 2^32, and both leave the smallest remainder there is: 1285 = 5 x 257 and 257 divide
// 2^32 - 1, so 2^32 % each is 1, and were the rings not kept contiguous, seq_no 0 and 1 (or the two keys either side
// of the reorder ring's own wrap) would share a slot - next to each other, so any run that holds both sees it.
#define SIM_CACHE_DEPTH 1285
#define SIM_REORDER_SLOTS 257
_Static_assert(SIM_REORDER_SLOTS >= tt_RELIABLE_BITMAP_BITS, "the reorder ring must be at least a window wide");
#define SIM_REORDER_SLOT_BYTES tt_REORDER_SLOT_SIZE(1472)
static struct tt_ReliableCacheIndex sim_cache_index[SIM_CACHE_DEPTH];
static uint8_t sim_cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(SIM_CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache sim_cache;
static uint64_t sim_reorder[READERS][SIM_REORDER_SLOTS * SIM_REORDER_SLOT_BYTES / sizeof(uint64_t)];

enum wrap_mode { MODE_KEEP_ALL, MODE_KEEP_LAST, MODE_BEST_EFFORT, MODE_DURABLE };

// One case: what the run does, whatever its first seq_no.
struct wrap_case {
    const char* name;
    uint32_t size; // bytes a sample: 1000 is one datagram, 2800 two, 14000 ten
    uint32_t loss; // per mille of every datagram, both ways
    enum wrap_mode mode;
    uint32_t drop_from, drop_count; // the writer's datagrams lost by send order (sim_drop_from)
    uint16_t cache_depth;           // the writer's cache depth; 0: SIM_CACHE_DEPTH
    uint32_t silent;                // the first `silent` samples reach nobody; then the backlog goes to the reader
    bool decline;                   // the reader declines each of samples 95-104 the first time it arrives
    bool second_reader;             // a second RELIABLE reader, which declines samples 90-109 for 30 ms
};

static uint32_t run_first; // the run's first seq_no, so the accept callbacks decide by index, never by seq_no
static uint32_t declined_once[READERS][4];
static uint64_t second_reader_declining_until;

// Samples 95-104 declined once each (the reader's queue was full, as a bounded KEEP_ALL rmw queue can be).
static bool decline_once(struct tt_Subscriber* s, uint32_t seq_no, void* param) {
    (void)s;
    (void)param;
    uint32_t index = seq_no - run_first;
    if (index < 95U || index >= 105U) {
        return true;
    }
    uint32_t bit = index - 95U;
    if ((declined_once[0][bit / 32U] >> (bit % 32U)) & 1U) {
        return true;
    }
    declined_once[0][bit / 32U] |= 1U << (bit % 32U);
    return false;
}

// The second reader declines samples 90-109 for its first 30 ms of declining: its acknowledgement stays behind the
// first reader's, either side of the wrap, while the writer has to wait for both.
static bool decline_for_a_while(struct tt_Subscriber* s, uint32_t seq_no, void* param) {
    (void)s;
    (void)param;
    uint32_t index = seq_no - run_first;
    if (index < 90U || index >= 110U) {
        return true;
    }
    if (second_reader_declining_until == 0) {
        second_reader_declining_until = test_mock_now + (30 * tt_MILLISECOND);
    }
    return test_mock_now >= second_reader_declining_until;
}

static void make_reader_reliable(struct tt_Subscriber* s, int r, bool durable) {
    s->reliable = true;
    s->durable = durable;
    memset(sim_reorder[r], 0, sizeof(sim_reorder[r]));
    s->reorder_storage = sim_reorder[r];
    s->reorder_slots = SIM_REORDER_SLOTS;
    s->reorder_slot_bytes = SIM_REORDER_SLOT_BYTES;
}

// What one run did, for comparing a wrap run with its control.
struct wrap_outcome {
    int delivered[READERS];
    bool in_order[READERS];
    uint64_t sent[3];
    uint64_t dropped[3];
    uint32_t abandoned[READERS];    // gap_abandoned + gap_evicted: samples given up on
    uint32_t out_of_order[READERS]; // record_delivery_order()'s count of steps backwards, and those discarded
    uint32_t held_peak[READERS];
    uint32_t declines[READERS];
    uint32_t resent;
    uint32_t first_seq_no, last_seq_no;
};

static void reader_outcome(struct wrap_outcome* out, int r, const struct tt_Subscriber* s) {
    out->delivered[r] = seen[r].delivered;
    out->in_order[r] = seen[r].in_order;
    out->abandoned[r] = s->gap_abandoned + s->gap_evicted;
    out->out_of_order[r] = (uint32_t)s->out_of_order + s->out_of_order_discarded;
    out->held_peak[r] = s->reorder_held_peak;
    out->declines[r] = s->accept_declines;
}

// SIM_SAMPLES samples from a writer whose first seq_no is `first`, as `c` says; runs until every reader has them all
// or `budget` has passed.
static struct wrap_outcome sim_run_case(uint32_t first, const struct wrap_case* c, uint64_t budget) {
    test_mock_reset();
    sample_len = c->size;
    sample_index = 1;
    delivered_count = 0;
    memset(seen, 0, sizeof(seen));
    seen[0].in_order = seen[1].in_order = true;
    memset(declined_once, 0, sizeof(declined_once));
    second_reader_declining_until = 0;
    run_first = first;
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
    int readers = c->second_reader ? 2 : 1;
    if (c->second_reader) {
        EXPECT_EQ_INT(tt_RET_OK,
                      tt_Context_create_subscriber(&receiver, &sub2, &receiver_topic, ENDPOINT_NAME, on_data));
    }

    sim_head = sim_tail = 0;
    sim_loss_per_mille = c->loss;
    sim_rng = 1;
    sim_drop_from = c->drop_from;
    sim_drop_count = c->drop_count;
    sim_mute = false;
    memset(sim_sent, 0, sizeof(sim_sent));
    memset(sim_dropped, 0, sizeof(sim_dropped));
    memset(sim_cache_index, 0, sizeof(sim_cache_index));
    memset(&sim_cache, 0, sizeof(sim_cache));
    sim_cache.index = sim_cache_index;
    sim_cache.capacity = SIM_CACHE_DEPTH;
    sim_cache.depth = c->cache_depth != 0 ? c->cache_depth : SIM_CACHE_DEPTH;
    sim_cache.arena = sim_cache_arena;
    sim_cache.arena_size = (uint32_t)sizeof(sim_cache_arena);
    if (c->mode != MODE_BEST_EFFORT) {
        pub.reliable_cache = &sim_cache;
        pub.reliable = true;
        pub.keep_all = c->mode != MODE_KEEP_LAST;
        pub.durable = c->mode == MODE_DURABLE;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, 10 * tt_MILLISECOND));
        make_reader_reliable(&sub, 0, c->mode == MODE_DURABLE);
        if (c->decline) {
            sub.accept_callback = decline_once;
        }
        if (c->second_reader) {
            make_reader_reliable(&sub2, 1, c->mode == MODE_DURABLE);
            sub2.accept_callback = decline_for_a_while;
        }
    }
    pub.seq_no = first - 1U; // the next sample takes seq_no `first`

    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
    for (int i = 0; i < SIM_SAMPLES; i++) {
        sim_mute = (uint32_t)i < c->silent;
        if ((uint32_t)i == c->silent && c->silent != 0) {
            // The reader arrives: what the writer retains goes to it, then the initial Heartbeat - a match's order
            // (register_subscriber_peer_on_publisher()).
            struct tt_Peer reader = {RECEIVER_ID, RECEIVER_IP, PORT};
            sim_acting = SENDER_ID;
            deliver_durability_backlog(&sender, &pub, &reader);
            send_initial_heartbeat(&sender, &pub, &reader);
            sim_drain();
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
        test_mock_now += SIM_PUBLISH_SPACING;
    }
    sim_mute = false;
    uint64_t published = test_mock_now;
    while (delivered_count < readers * SIM_SAMPLES && test_mock_now - published < budget) {
        if (!sim_step()) {
            break;
        }
    }
    test_mock_send_hook = NULL;

    struct wrap_outcome out;
    memset(&out, 0, sizeof(out));
    reader_outcome(&out, 0, &sub);
    out.in_order[1] = true; // no second reader: nothing out of order there
    if (c->second_reader) {
        reader_outcome(&out, 1, &sub2);
    }
    memcpy(out.sent, sim_sent, sizeof(out.sent));
    memcpy(out.dropped, sim_dropped, sizeof(out.dropped));
    out.resent = pub.retransmitted;
    out.first_seq_no = first;
    out.last_seq_no = pub.seq_no;
    return out;
}

// The plain case most tests below vary: SIM_SAMPLES samples of `size` bytes at `loss` per mille.
static struct wrap_outcome sim_run(uint32_t first, uint32_t size, uint32_t loss, enum wrap_mode mode, uint64_t budget) {
    struct wrap_case c = {"", size, loss, mode, 0, 0, 0, 0, false, false};
    return sim_run_case(first, &c, budget);
}

// The salt reorder_writer_slot() adds to this writer's seq_no: its ring key passes 2^32 where seq_no passes -salt.
static uint32_t writer_salt(void) {
    return (pub.endpoint.entity_id * 2654435761U) ^ ((uint32_t)SENDER_ID * 40503U);
}

static bool same_outcome(const struct wrap_outcome* a, const struct wrap_outcome* b) {
    bool same = a->resent == b->resent && a->sent[SENDER_ID] == b->sent[SENDER_ID] &&
                a->sent[RECEIVER_ID] == b->sent[RECEIVER_ID];
    for (int r = 0; r < READERS; r++) {
        same = same && a->delivered[r] == b->delivered[r] && a->in_order[r] == b->in_order[r] &&
               a->abandoned[r] == b->abandoned[r] && a->out_of_order[r] == b->out_of_order[r] &&
               a->held_peak[r] == b->held_peak[r] && a->declines[r] == b->declines[r];
    }
    return same;
}

// A wrap run against its control: the same case from seq_no 1, which never wraps. Translating seq_no changes nothing
// the protocol does - the loss pattern is drawn per datagram and picked by send order, never by seq_no - so the two
// must match exactly: every sample delivered once and in order as often, the same datagrams sent each way and resent,
// the same samples given up on, stepped back over and declined, the same peak held. The treatment is checked from what
// ran: the span of seq_no the writer gave out must contain `across` (2^32 itself, or the reorder ring's own wrap
// point); a lossy reliable run must have held and resent something, and a dropping one lost what it picked.
static void expect_case(const struct wrap_case* c, uint32_t first, uint32_t across, bool all_delivered) {
    struct wrap_outcome control = sim_run_case(1, c, 5 * tt_SECOND);
    struct wrap_outcome wrap = sim_run_case(first, c, 5 * tt_SECOND);
    bool same = same_outcome(&control, &wrap);
    printf("test_seq_wrap: %-34s first %10u last %10u: delivered %d/%d in order %d, sent %lu/%lu, resent %u, held "
           "%u, given up %u, out of order %u%s\n",
           c->name, wrap.first_seq_no, wrap.last_seq_no, wrap.delivered[0], wrap.delivered[1], wrap.in_order[0],
           (unsigned long)wrap.sent[SENDER_ID], (unsigned long)wrap.sent[RECEIVER_ID], wrap.resent, wrap.held_peak[0],
           wrap.abandoned[0], wrap.out_of_order[0], same ? "" : "  <- DIFFERS from the control");
    if (!same) {
        printf(
            "test_seq_wrap:   control: delivered %d/%d in order %d/%d, sent %lu/%lu, resent %u, held %u/%u, given up "
            "%u/%u, out of order %u/%u, declined %u/%u\n",
            control.delivered[0], control.delivered[1], control.in_order[0], control.in_order[1],
            (unsigned long)control.sent[SENDER_ID], (unsigned long)control.sent[RECEIVER_ID], control.resent,
            control.held_peak[0], control.held_peak[1], control.abandoned[0], control.abandoned[1],
            control.out_of_order[0], control.out_of_order[1], control.declines[0], control.declines[1]);
    }
    EXPECT_TRUE(same);
    EXPECT_TRUE(wrap.in_order[0] && wrap.in_order[1]);
    if (all_delivered) {
        EXPECT_EQ_INT(SIM_SAMPLES, wrap.delivered[0]);
        if (c->second_reader) {
            EXPECT_EQ_INT(SIM_SAMPLES, wrap.delivered[1]);
        }
    }
    EXPECT_TRUE(across - wrap.first_seq_no <= wrap.last_seq_no - wrap.first_seq_no); // the run crossed it
    if (c->loss != 0 && c->mode != MODE_BEST_EFFORT) {
        EXPECT_TRUE(wrap.held_peak[0] > 0 && wrap.resent > 0);
    }
    if (c->drop_count != 0) {
        EXPECT_TRUE(wrap.dropped[SENDER_ID] >= c->drop_count);
    }
    if (c->decline) {
        EXPECT_TRUE(wrap.declines[0] > 0);
    }
    if (c->second_reader) {
        EXPECT_TRUE(wrap.declines[1] > 0);
    }
}

// The same, for the plain cases: SIM_SAMPLES of `size` bytes at `loss`.
static void expect_same_as_control(const char* name, uint32_t first, uint32_t size, uint32_t loss, enum wrap_mode mode,
                                   uint32_t across, bool all_delivered) {
    struct wrap_case c = {name, size, loss, mode, 0, 0, 0, 0, false, false};
    expect_case(&c, first, across, all_delivered);
}

static void test_reliable_stream_crosses_2_32_as_if_it_had_not(void) {
    // Starts 100 datagrams' worth of seq_no short of 2^32, so the wrap falls mid-run: one datagram a sample (1000
    // bytes), two (2800) and ten (14000, one seq_no a datagram), KEEP_ALL and KEEP_LAST, lossless and at 5% both ways.
    static const uint32_t sizes[] = {1000, 2800, 14000};
    static const uint32_t datagrams[] = {1, 2, 10};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint32_t first = 0U - (100U * datagrams[i]);
        expect_same_as_control("KEEP_ALL lossless", first, sizes[i], 0, MODE_KEEP_ALL, 0, true);
        expect_same_as_control("KEEP_ALL 5% loss", first, sizes[i], 50, MODE_KEEP_ALL, 0, true);
        expect_same_as_control("KEEP_LAST 5% loss", first, sizes[i], 50, MODE_KEEP_LAST, 0, false);
    }
}

static void test_losses_at_the_wrap_itself(void) {
    // Random loss lands where it lands; these put it at 2^32. The writer starts 100 samples short (one datagram each,
    // or ten), and the datagrams carrying the last samples before 2^32 and the first after it are lost, so the reader
    // meets the wrap with a gap open below it: DATA past 2^32 with the watermark short of it, repairs and ACKNACKs that
    // name both sides, and - with a small cache (KEEP_LAST, depth 13) - samples gone, an eviction Heartbeat whose first
    // available is past 2^32, and give-ups across it. A burst wider than the window makes the reader jump across 2^32.
    // BEST_EFFORT with the same holes orders past them.
    const struct wrap_case cases[] = {
        {"KEEP_ALL, hole at 2^32", 1000, 0, MODE_KEEP_ALL, 97, 6, 0, 0, false, false},
        {"KEEP_ALL 2% loss, hole at 2^32", 1000, 20, MODE_KEEP_ALL, 97, 6, 0, 0, false, false},
        {"KEEP_LAST depth 13, hole at 2^32", 1000, 20, MODE_KEEP_LAST, 95, 10, 13, 0, false, false},
        {"KEEP_LAST, burst over the window", 1000, 0, MODE_KEEP_LAST, 60, 300, 0, 0, false, false},
        {"BEST_EFFORT, hole at 2^32", 1000, 0, MODE_BEST_EFFORT, 97, 6, 0, 0, false, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        expect_case(&cases[i], 0U - 100U, 0, cases[i].mode == MODE_KEEP_ALL);
    }
    const struct wrap_case fragmented[] = {
        {"10 fragments, hole at 2^32", 14000, 0, MODE_KEEP_ALL, 985, 30, 0, 0, false, false},
        {"10 fragments KEEP_LAST depth 13", 14000, 0, MODE_KEEP_LAST, 985, 30, 13, 0, false, false},
        {"10 fragments BEST_EFFORT, hole", 14000, 0, MODE_BEST_EFFORT, 985, 30, 0, 0, false, false},
        {"10 fragments, burst over the window", 14000, 0, MODE_KEEP_LAST, 900, 300, 0, 0, false, false},
    };
    for (size_t i = 0; i < sizeof(fragmented) / sizeof(fragmented[0]); i++) {
        expect_case(&fragmented[i], 0U - 1000U, 0, fragmented[i].mode == MODE_KEEP_ALL);
    }
}

static void test_declines_and_a_second_reader_across_the_wrap(void) {
    // A reader that declines the samples either side of 2^32 once (accept_callback: its queue full) asks for them
    // again; a second reader that declines them for 30 ms holds its acknowledgement short of 2^32 while the first
    // passes it, and the KEEP_ALL writer must wait for the slower - the lowest acknowledgement of the two, serially.
    const struct wrap_case cases[] = {
        {"KEEP_ALL, declined at 2^32", 1000, 0, MODE_KEEP_ALL, 0, 0, 0, 0, true, false},
        {"KEEP_ALL, a slow second reader", 1000, 0, MODE_KEEP_ALL, 0, 0, 0, 0, false, true},
        {"KEEP_ALL depth 13, slow second", 1000, 0, MODE_KEEP_ALL, 0, 0, 13, 0, false, true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        expect_case(&cases[i], 0U - 100U, 0, true);
    }
}

static void test_durable_backlog_across_the_wrap(void) {
    // The first 120 samples reach nobody; then the writer hands its DURABLE reader everything it retains (the backlog,
    // as on a match) and goes on. Starting 100 short of 2^32, the retained run straddles 2^32 - the backlog walk from
    // its oldest to its newest has to go through it. One datagram a sample, and ten: a fragmented backlog from a writer
    // more than a window past seq_no 1 stalled, wrap or not, until 2026-10-15 (tests/test_durable_late_joiner.c).
    const struct wrap_case c = {"DURABLE backlog across 2^32", 1000, 0, MODE_DURABLE, 0, 0, 0, 120, false, false};
    expect_case(&c, 0U - 100U, 0, true);
    const struct wrap_case fragmented = {
        "DURABLE 10-fragment backlog, 2^32", 14000, 0, MODE_DURABLE, 0, 0, 0, 120, false, false};
    expect_case(&fragmented, 0U - 1000U, 0, true);
}

// --- the writer's bookkeeping across the wrap, piece by piece ----------------------------------------
//
// What the end-to-end runs above cannot place at 2^32 on demand. Each check has a control the wrap cannot touch: the
// same question asked with every seq_no moved 2^31 away from the wrap.

#define SHIFTED(seq_no) ((seq_no) + (uint32_t)0x80000000U) // the control's seq_no, far from 2^32

// A cache of 16 slots holding seq_no first .. first + count - 1, one DATA record each.
static struct tt_ReliableCacheIndex piece_index[16];
static uint8_t piece_arena[tt_RELIABLE_CACHE_ARENA_BYTES(16, tt_RELIABLE_RECORD_BYTES(512))];
static uint8_t piece_arena_grown[sizeof(piece_arena) * 2];

static void piece_cache(struct tt_ReliableCache* cache, uint32_t first, uint32_t count) {
    memset(piece_index, 0, sizeof(piece_index));
    memset(cache, 0, sizeof(*cache));
    cache->index = piece_index;
    cache->capacity = 16;
    cache->depth = 16;
    cache->arena = piece_arena;
    cache->arena_size = (uint32_t)sizeof(piece_arena);
    for (uint32_t n = 0; n < count; n++) {
        cache_write(cache, first + n, 40);
    }
}

static void test_cache_walks_cross_the_wrap(void) {
    // A retained run from 5 short of 2^32 to 5 past it. Every walk from oldest to newest - growing the arena, the
    // oldest resendable seq_no - has to go through 2^32 rather than stop at it.
    for (int control = 0; control < 2; control++) {
        uint32_t first = control != 0 ? SHIFTED(0U - 5U) : 0U - 5U;
        struct tt_ReliableCache cache;
        piece_cache(&cache, first, 10);
        // tt_ReliableCache_grow() repacks every retained record into the new arena, in seq_no order.
        EXPECT_EQ_INT(tt_RET_OK, tt_ReliableCache_grow(&cache, piece_arena_grown, (uint32_t)sizeof(piece_arena_grown)));
        uint32_t held = 0;
        for (uint32_t n = 0; n < 10; n++) {
            held += cache_holds(&cache, first + n) ? 1U : 0U;
        }
        EXPECT_EQ_U32(10, held);
        EXPECT_TRUE(cache.slot_origin != 0 && cache.oldest_seq_no == first);
        // With nothing expired, the oldest resendable is the oldest retained.
        (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND); // a Publisher to ask on behalf of
        pub.seq_no = first + 9U;
        EXPECT_EQ_U32(first, reliable_cache_first_resendable_seq_no(&pub, &cache, 16));
    }
}

static void test_cache_admits_by_acknowledgement_across_the_wrap(void) {
    // KEEP_ALL may evict for bytes only what every reader has acknowledged (reliable_cache_admits()): a record that
    // needs the whole arena admits when everything retained - which straddles 2^32 - is acknowledged, serially, and
    // not when the acknowledgement stops short of the newest.
    for (int control = 0; control < 2; control++) {
        uint32_t first = control != 0 ? SHIFTED(0U - 5U) : 0U - 5U;
        struct tt_ReliableCache cache;
        piece_cache(&cache, first, 10);
        uint32_t whole = cache.arena_size; // fits only once everything retained has gone
        EXPECT_TRUE(reliable_cache_admits(&cache, 16, whole, first + 9U));  // all acknowledged
        EXPECT_TRUE(!reliable_cache_admits(&cache, 16, whole, first + 3U)); // stops before 2^32: may not evict past
    }
}

static void test_acknowledgements_compare_serially(void) {
    // Two readers, one acknowledged up to just before 2^32 and one past it: the slower is the one before (serially
    // lowest), "acknowledged by all" holds below it and not above, and a reader whose acknowledgement passes a match
    // point on the other side of 2^32 has that match Heartbeat no longer owed. The control: the same, 2^31 away.
    for (int control = 0; control < 2; control++) {
        uint32_t base = control != 0 ? SHIFTED(0) : 0;
        (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND);
        memset(pub.peer_acks, 0, sizeof(pub.peer_acks));
        pub.peer_acks[0] =
            (struct tt_PeerAck) {.context_id = 7, .entity_id = 70, .ack_seq_no = base - 3U, .acked = true};
        pub.peer_acks[1] =
            (struct tt_PeerAck) {.context_id = 8, .entity_id = 80, .ack_seq_no = base + 4U, .acked = true};
        EXPECT_EQ_U32(base - 3U, min_peer_ack_seq_no(&pub));
        EXPECT_EQ_U32(base - 3U, tt_Publisher_min_acked_seq_no(&pub));
        EXPECT_TRUE(tt_Publisher_is_acked_by_all_peers(&pub, base - 4U));
        EXPECT_TRUE(!tt_Publisher_is_acked_by_all_peers(&pub, base + 1U));
        // Match Heartbeats: the lowest owed is the one before 2^32; an acknowledgement past an owed point clears it.
        pub.peer_acks[0].first_owed_seq_no = base - 2U;
        pub.peer_acks[0].match_heartbeats_left = 2;
        pub.peer_acks[1].first_owed_seq_no = base + 2U;
        pub.peer_acks[1].match_heartbeats_left = 2;
        uint32_t owed = 0;
        EXPECT_TRUE(take_match_heartbeat(&pub, &owed));
        EXPECT_EQ_U32(base - 2U, owed);
        record_peer_ack(&pub, 7, 70, base + 1U); // past its owed point, across 2^32
        EXPECT_EQ_U32(0, pub.peer_acks[0].match_heartbeats_left);
        EXPECT_EQ_U32(base + 1U, pub.peer_acks[0].ack_seq_no);
        record_peer_ack(&pub, 7, 70, base - 1U); // a stale one, from before 2^32, does not move it back
        EXPECT_EQ_U32(base + 1U, pub.peer_acks[0].ack_seq_no);
    }
}

static void test_fragmented_acknowledgement_rounds_down_across_the_wrap(void) {
    // KEEP_ALL rounds "acknowledged through" down to a whole sample (keep_all_acked_through()): an acknowledgement
    // inside a fragmented sample leaves all of it unacknowledged. Here the sample (FRAG_FIRST and three continuations)
    // straddles 2^32 and the reader has acknowledged into its last continuation: everything through the DATA before
    // the sample is acknowledged, and nothing of the sample - the walk back goes through 2^32.
    for (int control = 0; control < 2; control++) {
        uint32_t base = control != 0 ? SHIFTED(0) : 0;
        struct tt_ReliableCache cache;
        piece_cache(&cache, base - 4U, 0);
        static const uint8_t types[] = {tt_SUBMESSAGE_TYPE_DATA,      tt_SUBMESSAGE_TYPE_FRAG_FIRST,
                                        tt_SUBMESSAGE_TYPE_FRAG_CONT, tt_SUBMESSAGE_TYPE_FRAG_CONT,
                                        tt_SUBMESSAGE_TYPE_FRAG_CONT, tt_SUBMESSAGE_TYPE_DATA};
        for (uint32_t n = 0; n < 6; n++) {
            cache_write(&cache, base - 4U + n, 40);
            const struct tt_ReliableCacheIndex* entry = reliable_cache_slot(&cache, 16, base - 4U + n);
            ((struct tt_SubmessageHeader*)(cache.arena + entry->offset))->type = types[n];
        }
        (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND);
        pub.reliable_cache = &cache;
        memset(pub.peer_acks, 0, sizeof(pub.peer_acks));
        pub.peer_acks[0] = (struct tt_PeerAck) {.context_id = 7, .entity_id = 70, .ack_seq_no = base, .acked = true};
        EXPECT_EQ_U32(base - 4U, keep_all_acked_through(&pub)); // the DATA before the sample, not into it
    }
}

static void test_repair_requests_name_seq_no_across_the_wrap(void) {
    // An ACKNACK's request is remembered as a range (tt_WriterProxy.requests), and its range can straddle 2^32: a
    // repair of a seq_no past 2^32 is timed from it (note_repair_arrival()), a decline forgets it
    // (forget_requests_answered_by()), and a Heartbeat asking for an answer while it is still in flight leaves what it
    // named out (answer_ack_request()): a pure acknowledgement, as long as the request is younger than the transit.
    for (int control = 0; control < 2; control++) {
        uint32_t base = control != 0 ? SHIFTED(0) : 0;
        (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND);
        struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
        EXPECT_TRUE(proxy != NULL);
        if (proxy == NULL) {
            return;
        }
        proxy->ack_seq_no = base - 2U;
        proxy_clear_received(proxy);
        proxy->heartbeat_last_seq_no = base + 3U; // announced: base - 2 .. base + 3 open, a gap across 2^32
        for (int slot = 0; slot < tt_RELIABLE_REQUEST_HISTORY; slot++) {
            proxy->requests[slot].sent_ns = 0;
        }
        proxy->transit_srtt_ns = 0;
        proxy->transit_rttvar_ns = 0;
        uint64_t asked = test_mock_now;
        note_requested(proxy, base - 2U, base + 3U, asked);
        // A Heartbeat asking for an answer now: everything open was named a moment ago, so a pure acknowledgement.
        test_mock_send_hook = sim_capture;
        uint32_t before = sim_tail;
        sim_acting = RECEIVER_ID;
        answer_ack_request(&receiver, proxy);
        test_mock_send_hook = NULL;
        EXPECT_EQ_U32(before + 1U, sim_tail);
        const struct sim_datagram* d = &sim_queue[before % SIM_QUEUE];
        const struct tt_AckNackHeader* ack = NULL;
        for (uint32_t at = 0; at + sizeof(struct tt_AckNackHeader) <= d->len; at++) { // its one submessage, wherever
            const struct tt_AckNackHeader* candidate = (const struct tt_AckNackHeader*)(d->bytes + at);
            if (candidate->seq_no == base - 2U && candidate->endpoint_id == sub.endpoint.id) {
                ack = candidate;
                break;
            }
        }
        EXPECT_TRUE(ack != NULL && ack->bitmap_words == 0); // names nothing: all of it is on its way
        sim_head = sim_tail;
        // The repairs of base + 1 and of base - 1 arrive, one either side of 2^32: each is timed from the request.
        test_mock_now = asked + (50 * tt_MICROSECOND);
        note_repair_arrival(proxy, base + 1U, test_mock_now);
        EXPECT_TRUE(proxy->transit_srtt_ns != 0);
        proxy->transit_srtt_ns = 0;
        proxy->transit_rttvar_ns = 0;
        note_repair_arrival(proxy, base - 1U, test_mock_now);
        EXPECT_TRUE(proxy->transit_srtt_ns != 0);
        // A copy declined, past 2^32 and then before it: either way the request that named it is forgotten.
        const uint32_t declined[] = {base + 2U, base - 1U};
        for (size_t d = 0; d < sizeof(declined) / sizeof(declined[0]); d++) {
            note_requested(proxy, base - 2U, base + 3U, asked);
            forget_requests_answered_by(&sub, SENDER_ID, pub.endpoint.entity_id, declined[d]);
            uint32_t remembered = 0;
            for (int slot = 0; slot < tt_RELIABLE_REQUEST_HISTORY; slot++) {
                remembered += proxy->requests[slot].sent_ns != 0 ? 1U : 0U;
            }
            EXPECT_EQ_U32(0, remembered);
        }
    }
}

static void test_heartbeat_jump_across_the_wrap(void) {
    // A Heartbeat announcing a last seq_no more than a window past the watermark, on the other side of 2^32 from it,
    // jumps the reader across (inform_subscriber_of_heartbeat(), jump_ack_baseline()) and counts what it gave up. The
    // control: the same, 2^31 away.
    for (int control = 0; control < 2; control++) {
        uint32_t base = control != 0 ? SHIFTED(0) : 0;
        (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND);
        struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
        EXPECT_TRUE(proxy != NULL);
        if (proxy == NULL) {
            return;
        }
        proxy->ack_seq_no = base - 5U;
        proxy->reorder_cursor = base - 5U;
        reorder_place_pivot(&sub, proxy, base - 5U);
        proxy->heartbeat_last_seq_no = base - 6U;
        proxy_clear_received(proxy);
        uint32_t abandoned = sub.gap_abandoned;
        struct tt_Header header;
        memset(&header, 0, sizeof(header));
        header.source = SENDER_ID;
        struct heartbeat_ctx ctx = {
            &header, SENDER_IP, PORT, pub.endpoint.entity_id, base - 5U, base + 300U, tt_HEARTBEAT_FLAG_FINAL};
        inform_subscriber_of_heartbeat(&receiver, &sub.endpoint, &ctx);
        EXPECT_EQ_U32(base + 301U, proxy->ack_seq_no); // past the last announced: jumped
        EXPECT_EQ_U32(abandoned + 305U, sub.gap_abandoned);
    }
}

static void test_reorder_ring_crosses_its_own_wrap(void) {
    // The reorder ring's key is seq_no plus the writer's salt, which passes 2^32 where seq_no passes -salt - nowhere
    // near seq_no's own wrap. Start 100 datagrams before that point, with loss so samples are held across it.
    (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND); // creates pub, whose entity_id the salt hashes
    uint32_t salt = writer_salt();
    expect_same_as_control("reorder key wrap, 1 datagram", 0U - salt - 100U, 1000, 50, MODE_KEEP_ALL, 0U - salt, true);
    expect_same_as_control("reorder key wrap, 10 datagrams", 0U - salt - 1000U, 14000, 50, MODE_KEEP_ALL, 0U - salt,
                           true);
}

static void test_reorder_ring_slots_stay_distinct_across_its_wrap(void) {
    // The slots of the seq_no either side of the point where this writer's key (seq_no + salt) passes 2^32: a window's
    // worth of consecutive seq_no must take as many slots. 257 slots leave 2^32 % 257 = 1, so the two keys either side
    // of the wrap would share slot 0 if the ring jumped there. The pivot is placed as a first contact places it, at the
    // walk's first seq_no, and as the store path moves it, from 2^30 behind; the walk starts either side of the wrap in
    // turn. The control is the same walk far from it.
    (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND); // a reliable pair, and the writer's proxy
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy == NULL) {
        return;
    }
    uint32_t wrap_at = 0U - writer_salt(); // the seq_no whose key is 0
    const uint32_t starts[] = {wrap_at - 100U, wrap_at - 300U, wrap_at + 50U, 0U - 100U,
                               1000U}; // 0 - 100: seq_no's own
    for (size_t t = 0; t < 2 * (sizeof(starts) / sizeof(starts[0])); t++) {
        size_t s = t / 2;
        proxy->ack_seq_no = starts[s];
        reorder_place_pivot(&sub, proxy, (t % 2) != 0 ? starts[s] - 0x40000000U : starts[s]);
        reorder_follow_pivot(&sub, proxy, starts[s]); // moves the second kind on, to 2^29 behind the walk
        EXPECT_EQ_U32((t % 2) != 0 ? starts[s] - 0x20000000U : starts[s], proxy->reorder_pivot);
        static struct tt_ReorderSlot* seen[SIM_REORDER_SLOTS];
        uint32_t shared = 0;
        for (uint32_t i = 0; i < SIM_REORDER_SLOTS; i++) {
            seen[i] = reorder_writer_slot(&sub, proxy, SENDER_ID, pub.endpoint.entity_id, starts[s] + i);
            for (uint32_t j = 0; j < i; j++) {
                shared += seen[j] == seen[i] ? 1U : 0U;
            }
        }
        EXPECT_EQ_U32(0, shared);
    }
}

static void test_reorder_ring_repivots_before_2_31(void) {
    // The ring's pivot (tt_WriterProxy.reorder_pivot) follows what is stored once that lies 2^30 from it, so no held
    // seq_no lies 2^32 from it, where the distance would wrap. A seq_no is held 2^30 - 10 past a pivot whose key is far
    // from 2^32 (the control: the pivot stays), then one 20 further on is stored (the treatment: it moves), then one
    // behind the pivot and the first again: each time the one held keeps its slot, and the seq_no around it still take
    // distinct slots.
    (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND); // a reliable pair, and the writer's proxy
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy == NULL) {
        return;
    }
    uint32_t pivot = 0x10000000U - writer_salt(); // its key is 0x10000000: nothing near it wraps
    reorder_place_pivot(&sub, proxy, pivot);
    uint32_t held = pivot + 0x40000000U - 10U;
    proxy->ack_seq_no = held;
    reorder_follow_pivot(&sub, proxy, held); // 2^30 - 10 out: stays
    EXPECT_EQ_U32(pivot, proxy->reorder_pivot);
    struct tt_ReorderSlot* held_slot = reorder_writer_slot(&sub, proxy, SENDER_ID, pub.endpoint.entity_id, held);
    reorder_follow_pivot(&sub, proxy, held + 20U); // 2^30 + 10 out: moves to 2^29 behind it
    EXPECT_EQ_U32(held + 20U - 0x20000000U, proxy->reorder_pivot);
    EXPECT_TRUE(held_slot == reorder_writer_slot(&sub, proxy, SENDER_ID, pub.endpoint.entity_id, held));
    reorder_follow_pivot(&sub, proxy, pivot - 7U); // behind the pivot: moves back
    EXPECT_EQ_U32(pivot - 7U - 0x20000000U, proxy->reorder_pivot);
    EXPECT_TRUE(held_slot == reorder_writer_slot(&sub, proxy, SENDER_ID, pub.endpoint.entity_id, held));
    struct tt_ReorderSlot* slots[21];
    for (uint32_t i = 0; i < 21; i++) {
        slots[i] = reorder_writer_slot(&sub, proxy, SENDER_ID, pub.endpoint.entity_id, held - 5U + i);
    }
    uint32_t shared = 0;
    for (uint32_t i = 0; i < 21; i++) {
        for (uint32_t j = i + 1; j < 21; j++) {
            shared += slots[i] == slots[j] ? 1U : 0U;
        }
    }
    EXPECT_EQ_U32(0, shared);
}

static void test_stale_announcement_is_not_read_as_ahead(void) {
    // heartbeat_last_seq_no is pinned just behind a watermark that has passed it by HEARTBEAT_STALE_LAG
    // (maybe_arm_acknack_retry()). Left where it was, on a stream no Heartbeat refreshes, it would fall 2^31 behind -
    // indistinguishable, serially, from 2^31 ahead - and every in-order sample would ask for a window nobody announced.
    // 2^31 samples cannot be sent here, so this checks the invariant that rules it out: after a sample it lags the
    // watermark by at most HEARTBEAT_STALE_LAG. The control: 10 behind, which is left as it is - and asks for nothing
    // either.
    const uint32_t lags[] = {10U, (uint32_t)HEARTBEAT_STALE_LAG + 10U};
    for (size_t l = 0; l < sizeof(lags) / sizeof(lags[0]); l++) {
        (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND);
        struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
        EXPECT_TRUE(proxy != NULL);
        if (proxy == NULL) {
            return;
        }
        proxy->heartbeat_last_seq_no = proxy->ack_seq_no - lags[l]; // behind: says nothing any more
        uint64_t acknacks = sim_sent[RECEIVER_ID];
        test_mock_send_hook = sim_capture;
        sim_acting = SENDER_ID;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
        sim_drain();
        test_mock_send_hook = NULL;
        EXPECT_EQ_INT(SIM_SAMPLES + 1, delivered_count); // the sample arrived, in order
        EXPECT_EQ_U64(acknacks, sim_sent[RECEIVER_ID]);  // and asked for nothing
        int32_t lag = seq_delta(proxy->heartbeat_last_seq_no, proxy->ack_seq_no);
        EXPECT_EQ_INT(l == 0 ? -11 : -1, lag); // the control kept, the stale one pinned
    }
}

static void test_seq_no_0_delivered_still_orders(void) {
    // Strict order keeps the highest seq_no delivered (tt_WriterProxy.highest_delivered); past 2^32 that can be 0,
    // which must not read as "nothing delivered yet": a late sample from before it is still a step backwards. The
    // control: the same with 1000 for 0.
    static uint8_t payload[1000];
    memset(payload, 0, sizeof(payload));
    const uint32_t latest[] = {0, 1000};
    for (size_t c = 0; c < sizeof(latest) / sizeof(latest[0]); c++) {
        (void)sim_run(1, 1000, 0, MODE_KEEP_ALL, tt_SECOND);
        struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
        EXPECT_TRUE(proxy != NULL);
        if (proxy == NULL) {
            return;
        }
        proxy->highest_delivered = latest[c] - 1U; // the stream so far reached the one before
        int before = delivered_count;
        uint32_t discarded = sub.out_of_order_discarded;
        deliver_in_order(&receiver, &sub, proxy, latest[c], 0, payload, sizeof(payload), true, false, NULL);
        deliver_in_order(&receiver, &sub, proxy, latest[c] - 2U, 0, payload, sizeof(payload), true, false, NULL);
        EXPECT_EQ_INT(before + 1, delivered_count); // the first delivered, the late one not
        EXPECT_EQ_U32(discarded + 1U, sub.out_of_order_discarded);
    }
}

static void test_durable_reader_meets_a_writer_near_2_32(void) {
    // A DURABLE reader's first DATA leaves its watermark at the proxy's default of 1 (update_reliable_ack()). From a
    // writer 100 short of 2^32 - serially before 1 - that sample is far ahead, not late, and the baseline jumps to
    // it: nothing is asked for that was announced by nobody.
    expect_same_as_control("DURABLE lossless", 0U - 100U, 1000, 0, MODE_DURABLE, 0, true);
    expect_same_as_control("DURABLE 5% loss", 0U - 100U, 1000, 50, MODE_DURABLE, 0, true);
}

static void test_best_effort_reader_takes_a_writer_past_2_31(void) {
    // A BEST_EFFORT reader orders by its writer proxy, whose default watermark is 1; a writer whose first seq_no lies
    // 2^31 or more past it (serially before 1) must not have its first sample read as late.
    expect_same_as_control("BEST_EFFORT lossless", 0U - 100U, 1000, 0, MODE_BEST_EFFORT, 0, true);
    expect_same_as_control("BEST_EFFORT lossless, 2^31 + 5", 0x80000005U, 1000, 0, MODE_BEST_EFFORT, 0x80000005U, true);
    expect_same_as_control("BEST_EFFORT 5% loss", 0U - 100U, 1000, 50, MODE_BEST_EFFORT, 0, false);
}

int main(void) {
    tt_log_set_level(TT_LOG_WARNING); // every run creates its pair: the INFO lines would be noise
    memset(&cache_node, 0, sizeof(cache_node));
    node_init_locks(&cache_node);
    cache_node.tx_size = sizeof(cache_node.tx_buffer);

    test_cache_ring_stays_contiguous_across_the_wrap();
    test_cache_ring_rebases_without_moving_a_sample();
    test_cache_ring_origin_is_never_0();
    test_reliable_stream_crosses_2_32_as_if_it_had_not();
    test_losses_at_the_wrap_itself();
    test_declines_and_a_second_reader_across_the_wrap();
    test_durable_backlog_across_the_wrap();
    test_cache_walks_cross_the_wrap();
    test_cache_admits_by_acknowledgement_across_the_wrap();
    test_acknowledgements_compare_serially();
    test_fragmented_acknowledgement_rounds_down_across_the_wrap();
    test_repair_requests_name_seq_no_across_the_wrap();
    test_heartbeat_jump_across_the_wrap();
    test_reorder_ring_crosses_its_own_wrap();
    test_reorder_ring_slots_stay_distinct_across_its_wrap();
    test_reorder_ring_repivots_before_2_31();
    test_stale_announcement_is_not_read_as_ahead();
    test_seq_no_0_delivered_still_orders();
    test_durable_reader_meets_a_writer_near_2_32();
    test_best_effort_reader_takes_a_writer_past_2_31();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_seq_wrap: all tests passed\n");
    return 0;
}
