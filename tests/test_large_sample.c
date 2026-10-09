/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Large-message stage 2 (DESIGN.md section 8, "Stage 2"), step A - the socket path: samples above tt_MAX_SAMPLE_LENGTH
// go as FRAG_FIRST_L/FRAG_CONT_L from a caller-acquired buffer and are put back together in another one. These are
// the pre-registration's L1 correctness checks; tests/mutants_large_sample.py makes the mutant each one names and
// requires it to fail here.
//
// Built as rmw_tickle builds core - a 65507-byte datagram, so tt_MAX_SAMPLE_LENGTH is 65507 and 65,508 bytes is the
// first large sample. Two contexts in one process over the mock HAL: every datagram the sender's HAL is handed is
// queued and fed to the receiver's process_packet() (and back), with loss when a test asks for it, on the mock clock.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1 // as rmw_tickle builds core

#include <pthread.h>
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
#define TOPIC_NAME "large_topic"
#define ENDPOINT_NAME "large_endpoint"

#define ONE_MB 1048576U
#define FOUR_MB 4194304U
#define EIGHT_MIB 8388608U
// The two sizes either side of the 8-bit index a small fragment carries: 255 and 256 datagrams.
// A sample goes padded to 4, so these are the largest multiple of 4 that 255 datagrams carry and the next one.
#define SIZE_255_FRAGMENTS ((1446U + (1452U * 254U)) & ~3U)
#define SIZE_256_FRAGMENTS (SIZE_255_FRAGMENTS + 4U)

// --- the caller's buffers: one allocator per context, with a failure switch and a reuse list -------------------------

struct test_allocator {
    uint32_t acquires;
    uint32_t releases;
    uint32_t live;
    uint32_t fail_every; // 0: never; N: every Nth acquire answers NULL
    // Released buffers, reused last-in first-out by the next acquire that fits: a buffer handed back while a sample in
    // it is still in use is then overwritten by the next one, which is what the lending test looks for.
    uint8_t* reuse[8];
    uint32_t reuse_count;
};

static struct test_allocator sender_alloc;
static struct test_allocator receiver_alloc;

static uint32_t stored_size(const uint8_t* buffer) {
    uint32_t size = 0;
    memcpy(&size, buffer - 16, sizeof(size));
    return size;
}

static void* test_acquire(void* user, uint32_t bytes) {
    struct test_allocator* alloc = (struct test_allocator*)user;
    alloc->acquires++;
    if (alloc->fail_every != 0 && alloc->acquires % alloc->fail_every == 0) {
        return NULL;
    }
    for (uint32_t i = alloc->reuse_count; i-- > 0;) {
        uint8_t* buffer = alloc->reuse[i];
        if (stored_size(buffer) >= bytes) {
            alloc->reuse[i] = alloc->reuse[--alloc->reuse_count];
            memset(buffer, 0xA5, bytes); // whatever was there before is gone
            alloc->live++;
            return buffer;
        }
    }
    uint8_t* block = (uint8_t*)malloc((size_t)bytes + 16U);
    if (block == NULL) {
        return NULL;
    }
    memcpy(block, &bytes, sizeof(bytes));
    memset(block + 16, 0xA5, bytes);
    alloc->live++;
    return block + 16;
}

static void test_release(void* user, void* buffer) {
    struct test_allocator* alloc = (struct test_allocator*)user;
    alloc->releases++;
    alloc->live--;
    uint8_t* bytes = (uint8_t*)buffer;
    memset(bytes, 0x5A, stored_size(bytes)); // a sample read after its release reads this, never its own bytes
    if (alloc->reuse_count == sizeof(alloc->reuse) / sizeof(alloc->reuse[0])) {
        free(alloc->reuse[0] - 16);
        alloc->reuse[0] = alloc->reuse[--alloc->reuse_count];
    }
    alloc->reuse[alloc->reuse_count++] = bytes;
}

static void allocator_reset(struct test_allocator* alloc) {
    for (uint32_t i = 0; i < alloc->reuse_count; i++) {
        free(alloc->reuse[i] - 16);
    }
    memset(alloc, 0, sizeof(*alloc));
}

// --- the sample: a sized, patterned payload whose first byte is its seed
// ----------------------------------------------

static uint32_t sample_len;
static uint8_t sample_seed;

static uint8_t pattern(uint32_t i, uint8_t seed) {
    return (uint8_t)((i * 7U) + seed + (i >> 12U)); // the high bits too: a whole fragment shifted is not the same bytes
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

// --- delivery ----------------------------------------------------------------------------------------------------

static int delivered_count;
static int delivered_torn; // deliveries whose bytes were not the sample's
static uint32_t delivered_len;
static uint8_t delivered_seeds[64];
static uint32_t last_seq;
static bool in_order;
static uint64_t delivered_at[64];
static bool retain_next;
static struct tt_Sample held;
static tt_ret_t retain_result;
static struct tt_Subscriber sub;

static bool payload_intact(const uint8_t* payload, uint32_t len, uint32_t expected_len) {
    if (len != ROUNDUP(expected_len)) {
        return false;
    }
    uint8_t seed = payload[0];
    for (uint32_t i = 0; i < expected_len; i++) {
        if (payload[i] != pattern(i, seed)) {
            return false;
        }
    }
    for (uint32_t i = expected_len; i < len; i++) {
        if (payload[i] != 0) {
            return false; // padding goes zeroed
        }
    }
    return true;
}

static int32_t checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    delivered_len = len;
    if (!payload_intact(payload, len, sample_len)) {
        delivered_torn++;
    }
    if (delivered_count < 64) {
        delivered_seeds[delivered_count] = payload[0];
    }
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)data;
    in_order = in_order && (delivered_count == 0 || (uint16_t)(seq_no - (uint16_t)last_seq) < 0x8000U);
    last_seq = seq_no;
    if (delivered_count < 64) {
        delivered_at[delivered_count] = test_mock_now;
    }
    delivered_count++;
    if (retain_next) {
        retain_next = false;
        retain_result = tt_Sample_retain(s, &held);
    }
}

// --- nodes -------------------------------------------------------------------------------------------------------

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_Topic sender_topic;
static struct tt_Topic receiver_topic;
static struct tt_Publisher pub;

#define CACHE_DEPTH 256
static struct tt_ReliableCacheIndex cache_index[CACHE_DEPTH];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache cache;
// The 8192-bit window rmw_tickle gives a subscription whose type can be large (DESIGN.md section 8).
#define WIDE_WORDS (tt_RELIABLE_BITMAP_MAX_BITS / 64)
static uint64_t tracking[tt_MAX_PEER_COUNT * WIDE_WORDS];

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

enum pair_mode { BEST_EFFORT, RELIABLE_KEEP_LAST, RELIABLE_KEEP_ALL };

// A Publisher and a Subscriber of one topic, each in its own context with its own large buffers. RELIABLE modes give
// the Publisher a cache and the Subscriber the wide window, and pre-match the reader on the writer with that window,
// as its announce would.
static void init_pair(uint32_t len, enum pair_mode mode) {
    test_mock_reset();
    sample_len = len;
    sample_seed = 1;
    delivered_count = 0;
    delivered_torn = 0;
    delivered_len = 0;
    last_seq = 0;
    in_order = true;
    retain_next = false;
    retain_result = tt_RET_OK;
    memset(&held, 0, sizeof(held));
    allocator_reset(&sender_alloc);
    allocator_reset(&receiver_alloc);

    init_bare_node(&sender, SENDER_ID);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = 8;
    sender_topic.data_encode_size = sized_encode_size;
    sender_topic.data_encode = sized_encode;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&sender, &pub, &sender_topic, ENDPOINT_NAME));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&sender, test_acquire, test_release, &sender_alloc));

    init_bare_node(&receiver, RECEIVER_ID);
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = 8;
    receiver_topic.data_decode = checking_decode;
    receiver_topic.data_free = free_nothing;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&receiver, &sub, &receiver_topic, ENDPOINT_NAME, on_data));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&receiver, test_acquire, test_release, &receiver_alloc));

    if (mode != BEST_EFFORT) {
        memset(cache_index, 0, sizeof(cache_index));
        memset(&cache, 0, sizeof(cache));
        cache.index = cache_index;
        cache.capacity = CACHE_DEPTH;
        cache.depth = CACHE_DEPTH;
        cache.arena = cache_arena;
        cache.arena_size = (uint32_t)sizeof(cache_arena);
        pub.reliable_cache = &cache;
        pub.reliable = true;
        pub.keep_all = mode == RELIABLE_KEEP_ALL;
        sub.reliable = true;
        memset(tracking, 0, sizeof(tracking));
        sub.tracking_bitmaps = tracking;
        sub.tracking_words = WIDE_WORDS;
        struct tt_PeerAck* ack = claim_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id);
        EXPECT_TRUE(ack != NULL);
        if (ack != NULL) {
            ack->tracking_words = WIDE_WORDS;
        }
    }
}

// --- the link: a queue of datagrams both ways, with loss
// --------------------------------------------------------------

#define SIM_QUEUE 32768U
#define SIM_DATAGRAM_MAX 1500U

struct sim_datagram {
    uint8_t from;
    uint32_t len;
    uint8_t bytes[SIM_DATAGRAM_MAX];
};
static struct sim_datagram* sim_queue;
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;
static uint32_t sim_loss_per_mille;
static uint32_t sim_rng;
static uint64_t sim_sent[3];
static uint64_t sim_dropped[3];
static uint32_t sim_room_per_step; // 0: unlimited; else the send buffer's room, in datagrams, refilled each step
static bool sim_reverse;           // deliver the sender's datagrams newest first, a burst at a time
// Drops the last fragment of each large sample once (the tail test).
static bool sim_drop_tails;
// ... and the end-of-sample HEARTBEAT that follows each dropped tail, once.
static bool sim_drop_tail_heartbeats;
static bool sim_heartbeat_owed;
static uint32_t sim_tails_dropped[64];
static uint32_t sim_tail_count;
static uint32_t sim_oversize;
// A writer busy sending a large sample reads nothing until the send ends, as one blocked in a long sendmmsg() loop
// does: the reader's datagrams to it wait here meanwhile.
static bool sim_busy_writer;
static struct sim_datagram* sim_held;
static uint32_t sim_held_count;
#define SIM_HELD 4096U

// The fragment header of a captured datagram, whichever form it went in: its type, and for FRAG_CONT_L its index,
// count and seq_no.
static uint8_t datagram_type(const uint8_t* bytes, const uint8_t** body) {
    if (bytes[0] == tt_SINGLE_MARKER_LE || bytes[0] == tt_SINGLE_MARKER_BE) {
        *body = bytes + sizeof(struct tt_SingleHeader);
        return ((const struct tt_SingleHeader*)bytes)->type;
    }
    *body = bytes + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader);
    return ((const struct tt_SubmessageHeader*)(bytes + sizeof(struct tt_Header)))->type;
}

static bool sim_tail_drop(const uint8_t* bytes) {
    const uint8_t* body = NULL;
    if (!sim_drop_tails || datagram_type(bytes, &body) != tt_SUBMESSAGE_TYPE_FRAG_CONT_L) {
        return false;
    }
    struct tt_FragContLHeader cont;
    memcpy(&cont, body, sizeof(cont));
    if (cont.frag_index + 1U != cont.frag_count) {
        return false;
    }
    for (uint32_t i = 0; i < sim_tail_count; i++) {
        if (sim_tails_dropped[i] == cont.seq_no) {
            return false; // once: its retransmission goes through
        }
    }
    if (sim_tail_count < 64) {
        sim_tails_dropped[sim_tail_count++] = cont.seq_no;
        sim_heartbeat_owed = sim_drop_tail_heartbeats;
    }
    return true;
}

static void sim_capture(const void* buf, size_t len) {
    if (len > SIM_DATAGRAM_MAX) {
        sim_oversize++;
        return;
    }
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
    sim_sent[sim_acting]++;
}

static bool sim_lost(void) {
    sim_rng = (sim_rng * 1103515245U) + 12345U;
    return ((sim_rng >> 16) % 1000U) < sim_loss_per_mille;
}

// Whether the Publisher still has a large sample, or part of one, to send.
static bool sim_writer_sending(void) {
    for (uint32_t i = 0; i < pub.large_count; i++) {
        if (!pub.large[(pub.large_head + i) % tt_LARGE_RETAINED].sent) {
            return true;
        }
    }
    return false;
}

static void sim_deliver(struct sim_datagram* d) {
    const uint8_t* body = NULL;
    if (sim_busy_writer && d->from == RECEIVER_ID && sim_writer_sending()) {
        EXPECT_TRUE(sim_held_count < SIM_HELD);
        if (sim_held_count < SIM_HELD) {
            sim_held[sim_held_count++] = *d;
        }
        return;
    }
    if (d->from == SENDER_ID && sim_heartbeat_owed && datagram_type(d->bytes, &body) == tt_SUBMESSAGE_TYPE_HEARTBEAT) {
        sim_heartbeat_owed = false;
        sim_dropped[d->from]++;
        return;
    }
    if (sim_lost() || (d->from == SENDER_ID && sim_tail_drop(d->bytes))) {
        sim_dropped[d->from]++;
        return;
    }
    struct tt_Context* to = d->from == SENDER_ID ? &receiver : &sender;
    sim_acting = to->id;
    process_packet(to, d->bytes, 0, d->len, d->from == SENDER_ID ? SENDER_IP : RECEIVER_IP, PORT, tt_TRANSPORT_UDP);
}

// Everything in flight, and whatever it provokes, until the link is empty.
static void sim_drain(void) {
    if (sim_reverse) {
        // One burst, newest first: what was queued when the drain began, delivered backwards.
        uint32_t end = sim_tail;
        for (uint32_t i = end; i-- > sim_head;) {
            sim_deliver(&sim_queue[i % SIM_QUEUE]);
        }
        sim_head = end;
    }
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
    if (sim_room_per_step != 0) {
        test_mock_nonblocking_room = (int32_t)sim_room_per_step; // the socket drained since the last step
    }
    if (sim_held_count != 0 && !sim_writer_sending()) {
        uint32_t held = sim_held_count;
        sim_held_count = 0;
        for (uint32_t i = 0; i < held; i++) {
            sim_deliver(&sim_held[i]);
        }
        sim_drain();
    }
    sim_run_due(&sender);
    sim_run_due(&receiver);
    return true;
}

static void sim_start(uint32_t loss_per_mille) {
    if (sim_queue == NULL) {
        sim_queue = (struct sim_datagram*)calloc(SIM_QUEUE, sizeof(struct sim_datagram));
    }
    sim_head = sim_tail = 0;
    sim_loss_per_mille = loss_per_mille;
    sim_rng = 7;
    memset(sim_sent, 0, sizeof(sim_sent));
    memset(sim_dropped, 0, sizeof(sim_dropped));
    sim_room_per_step = 0;
    sim_reverse = false;
    sim_drop_tails = false;
    sim_tail_count = 0;
    sim_drop_tail_heartbeats = false;
    sim_heartbeat_owed = false;
    sim_oversize = 0;
    if (sim_held == NULL) {
        sim_held = (struct sim_datagram*)calloc(SIM_HELD, sizeof(struct sim_datagram));
    }
    sim_busy_writer = false;
    sim_held_count = 0;
    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
}

// Publishes one sample (seed `seed`), retrying while KEEP_ALL says WOULD_BLOCK, and runs the link until it is quiet.
static tt_ret_t sim_publish(uint8_t seed, uint64_t budget) {
    sample_seed = seed;
    sim_run_due(&sender);
    sim_run_due(&receiver);
    sim_acting = SENDER_ID;
    if (sim_room_per_step != 0) {
        test_mock_nonblocking_room = (int32_t)sim_room_per_step;
    }
    tt_ret_t result = tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len);
    uint64_t since = test_mock_now;
    while (result == tt_RET_WOULD_BLOCK && test_mock_now - since < budget) {
        sim_drain();
        if (!sim_step()) {
            test_mock_now += tt_MILLISECOND;
        }
        sim_acting = SENDER_ID;
        result = tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len);
    }
    sim_drain();
    return result;
}

// Runs the protocol until `want` samples have been delivered or `budget` of simulated time has passed.
static void sim_settle(int want, uint64_t budget) {
    uint64_t since = test_mock_now;
    while (delivered_count < want && test_mock_now - since < budget) {
        if (!sim_step()) {
            break;
        }
    }
}

// --- L1.1: byte-exact round trips ---------------------------------------------------------------------------------

static void expect_wire_shape(uint32_t size) {
    // Every datagram fits the control datagram, fragment 0 is FRAG_FIRST_L, the rest FRAG_CONT_L, and there are as many
    // as tt_sample_datagrams() - what rmw_tickle sizes with - says.
    // A RELIABLE writer's end-of-sample HEARTBEAT follows the last of them.
    uint32_t count = tt_sample_datagrams(size);
    EXPECT_TRUE(sim_tail - sim_head == count || sim_tail - sim_head == count + 1);
    EXPECT_EQ_U32(0, sim_oversize);
    for (uint32_t i = sim_head; i < sim_tail; i++) {
        const uint8_t* body = NULL;
        uint8_t type = datagram_type(sim_queue[i % SIM_QUEUE].bytes, &body);
        uint8_t want = tt_SUBMESSAGE_TYPE_HEARTBEAT;
        if (i == sim_head) {
            want = tt_SUBMESSAGE_TYPE_FRAG_FIRST_L;
        } else if (i - sim_head < count) {
            want = tt_SUBMESSAGE_TYPE_FRAG_CONT_L;
        }
        EXPECT_EQ_INT(want, type);
        EXPECT_TRUE(sim_queue[i % SIM_QUEUE].len <= tt_CONTROL_MAX_LENGTH);
    }
}

static void round_trip(uint32_t size, enum pair_mode mode, bool reverse) {
    init_pair(size, mode);
    sim_start(0);
    sim_reverse = reverse;
    sample_seed = 9;
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    expect_wire_shape(size);
    EXPECT_EQ_U32(tt_sample_datagrams(size), pub.seq_no);
    sim_drain();
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_U32(ROUNDUP(size), delivered_len);
    EXPECT_EQ_U64(1, receiver.large.reassembled);
    EXPECT_EQ_U64(0, receiver.large.dropped);
    EXPECT_EQ_U32(0, receiver_alloc.live); // delivered and not kept: the receiver's buffer went back
}

static void test_round_trip_at_every_boundary(void) {
    static const uint32_t sizes[] = {65508, SIZE_255_FRAGMENTS, SIZE_256_FRAGMENTS, ONE_MB, FOUR_MB, EIGHT_MIB};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        round_trip(sizes[i], BEST_EFFORT, false);
        round_trip(sizes[i], RELIABLE_KEEP_LAST, false);
    }
    // 255 and 256 are the counts either side of an 8-bit field.
    EXPECT_EQ_U32(255, tt_sample_datagrams(SIZE_255_FRAGMENTS));
    EXPECT_EQ_U32(256, tt_sample_datagrams(SIZE_256_FRAGMENTS));
    EXPECT_EQ_U32(5778, tt_sample_datagrams(EIGHT_MIB)); // DESIGN.md section 8's figure
    EXPECT_EQ_U32(723, tt_sample_datagrams(ONE_MB));
    EXPECT_EQ_U32(2889, tt_sample_datagrams(FOUR_MB));
    // Control: the largest small sample is not a large one.
    EXPECT_TRUE(tt_sample_datagrams(tt_MAX_SAMPLE_LENGTH) <= tt_FRAG_MAX_COUNT);
}

static void test_any_arrival_order_reassembles(void) {
    // Newest first: the last fragment, shorter than the rest, lands before every other.
    round_trip(SIZE_256_FRAGMENTS, BEST_EFFORT, true);
    // RELIABLE: a writer is first met by a fragment 0 (as for a small sample), so the reversed sample is the second -
    // its fragments are recorded and placed in reverse, its end-of-sample HEARTBEAT arriving first.
    init_pair(ONE_MB + 3, RELIABLE_KEEP_LAST);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_reverse = true;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2, tt_SECOND));
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_INT(2, delivered_seeds[1]);
    EXPECT_EQ_U64(0, receiver.large.dropped);
}

static void test_an_inconsistent_fragment_is_dropped_and_counted(void) {
    // A continuation shorter than every non-last fragment of its sample cannot be one of them: placed, it would leave
    // the bytes it lacks unwritten and the sample torn. Dropped and counted; the genuine fragment then completes it.
    init_pair(ONE_MB, BEST_EFFORT);
    sim_start(0);
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    struct sim_datagram* fragment = &sim_queue[(sim_head + 100) % SIM_QUEUE];
    struct sim_datagram original = *fragment;
    fragment->len -= 100; // the single-submessage form: the datagram's length is the fragment's
    sim_drain();
    EXPECT_EQ_U64(1, receiver.large.dropped);
    EXPECT_EQ_INT(0, delivered_count);
    sim_queue[sim_tail % SIM_QUEUE] = original;
    sim_tail++;
    sim_drain();
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
}

static void test_small_samples_keep_the_small_path(void) {
    // Control: at tt_MAX_SAMPLE_LENGTH a sample is the fragmented DATA it always was, through tx_buffer, with no large
    // buffer acquired on either side.
    init_pair(tt_MAX_SAMPLE_LENGTH, BEST_EFFORT);
    sim_start(0);
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    const uint8_t* body = NULL;
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_FIRST, datagram_type(sim_queue[sim_head % SIM_QUEUE].bytes, &body));
    sim_drain();
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_U32(0, sender_alloc.acquires);
    EXPECT_EQ_U32(0, receiver_alloc.acquires);
}

static void test_without_large_buffers_a_large_sample_is_refused(void) {
    init_pair(65508, BEST_EFFORT);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&sender, NULL, NULL, NULL));
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_TOO_LARGE, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_U32(0, sim_tail - sim_head);
    EXPECT_EQ_U32(0, pub.seq_no);
    // A half-set pair is refused, and so is a change while a buffer is out.
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Context_set_large_buffers(&sender, test_acquire, NULL, NULL));
}

// --- L1.3: RELIABLE KEEP_ALL at 5% loss, and a lost tail -------------------------------------------------------------

static void reliable_under_loss(uint32_t size, int samples) {
    init_pair(size, RELIABLE_KEEP_ALL);
    sim_start(50); // 5% of every datagram, both ways
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), 10 * tt_SECOND));
    }
    sim_settle(samples, 10 * tt_SECOND);
    EXPECT_EQ_INT(samples, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_TRUE(in_order);
    for (int i = 0; i < samples && i < 64; i++) {
        EXPECT_EQ_INT(i + 1, delivered_seeds[i]); // every sample, once, in publish order
    }
    // The link really lost ~5%.
    EXPECT_TRUE(sim_dropped[SENDER_ID] * 100 > sim_sent[SENDER_ID] * 3);
    EXPECT_TRUE(sim_dropped[SENDER_ID] * 100 < sim_sent[SENDER_ID] * 7);
    printf("test_large_sample: %u B x %d RELIABLE KEEP_ALL at 5%% loss - %d delivered, %.3f datagrams sent per "
           "datagram of payload (%lu of %lu lost)\n",
           size, samples, delivered_count, (double)sim_sent[SENDER_ID] / ((double)tt_sample_datagrams(size) * samples),
           (unsigned long)sim_dropped[SENDER_ID], (unsigned long)sim_sent[SENDER_ID]);
}

static void test_reliable_keep_all_at_5_percent_loss(void) {
    reliable_under_loss(ONE_MB, 12);
    reliable_under_loss(FOUR_MB, 4);
}

// The tail case for each sample: its last fragment dropped once, and - with `heartbeat_too` - the end-of-sample
// HEARTBEAT after it as well. Nothing after it reveals the gap: the next sample is far off and no periodic Heartbeat is
// set. Returns the slowest sample's publish-to-delivery time.
static uint64_t lost_tail_case(bool heartbeat_too) {
    init_pair(ONE_MB, RELIABLE_KEEP_LAST);
    sim_start(0);
    sim_drop_tails = true;
    sim_drop_tail_heartbeats = heartbeat_too;
    const int samples = 4;
    uint64_t published_at[4];
    for (int i = 0; i < samples; i++) {
        published_at[i] = test_mock_now;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
        sim_settle(i + 1, 100 * tt_MILLISECOND);
        test_mock_now += 100 * tt_MILLISECOND; // the next sample is far off
    }
    EXPECT_EQ_INT(samples, delivered_count);
    EXPECT_EQ_U32((uint32_t)samples, sim_tail_count);
    uint64_t slowest = 0;
    for (int i = 0; i < samples && i < delivered_count; i++) {
        uint64_t took = delivered_at[i] - published_at[i];
        slowest = took > slowest ? took : slowest;
    }
    return slowest;
}

static void test_a_lost_tail_is_recovered_within_two_retry_intervals(void) {
    // With the end-of-sample HEARTBEAT the tail is asked for at once. Mutant: no end-of-sample HEARTBEAT - fails.
    EXPECT_TRUE(lost_tail_case(false) <= 2 * TEST_RETRY_INTERVAL);
    // And when that HEARTBEAT is lost too, it is asked again one retry interval on (large_ack_chase()): the tail is
    // back within a few. Without the second ask the last sample of a burst would wait for whatever came next.
    uint64_t both = lost_tail_case(true);
    printf("test_large_sample: tail and its HEARTBEAT lost - recovered in %.2f ms (retry interval %.2f ms)\n",
           (double)both / 1e6, (double)reliable_retry_interval_publisher() / 1e6);
    EXPECT_TRUE(both <= 4 * TEST_RETRY_INTERVAL);
}

// --- L1.4: BEST_EFFORT with no buffer every third acquire: counted, never torn
// -----------------------------------------

static void test_best_effort_without_a_buffer_is_counted_never_torn(void) {
    init_pair(ONE_MB, BEST_EFFORT);
    receiver_alloc.fail_every = 3;
    sim_start(1); // and 0.1% loss: a sample can also miss a fragment in its middle, which a torn delivery would show
    const int samples = 30;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
        sim_settle(samples, tt_MILLISECOND);
    }
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_TRUE(receiver.large.no_buffer >= 1);
    EXPECT_TRUE(delivered_count >= 1 && delivered_count < samples);
    EXPECT_TRUE(in_order);
    // Every sample either arrived whole, or is accounted for: no buffer, or abandoned incomplete.
    EXPECT_EQ_U64((uint64_t)delivered_count, receiver.large.reassembled);
    printf("test_large_sample: BEST_EFFORT, acquire NULL every 3rd - %d of %d delivered whole, no_buffer=%lu "
           "abandoned=%lu, 0 torn\n",
           delivered_count, samples, (unsigned long)receiver.large.no_buffer, (unsigned long)receiver.large.abandoned);
}

// --- L1.5: a reader whose window is narrower than the sample -------------------------------------------------------

static void test_keep_all_sample_wider_than_the_window_is_too_large(void) {
    init_pair(EIGHT_MIB, RELIABLE_KEEP_ALL);
    pub.peer_acks[0].tracking_words = 4096 / 64; // the reader announced 4096 bits
    sim_start(0);
    sim_acting = SENDER_ID;
    // 5778 datagrams cannot fit a 4096-bit window however long the writer waits: refused for good, not WOULD_BLOCK.
    EXPECT_EQ_INT(tt_RET_TOO_LARGE, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_U64(1, sender.large.window_too_small);
    EXPECT_EQ_U32(0, pub.seq_no);
    EXPECT_EQ_U32(0, sender_alloc.live); // nothing acquired for it
    // Control: 4 MB (2889 datagrams) fits the same window and goes.
    sample_len = FOUR_MB;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    sim_drain();
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_INT(1, delivered_count);
}

// --- L1.6: a send buffer far smaller than the sample ----------------------------------------------------------------

static void test_a_full_send_buffer_delays_the_sample_never_loses_it(void) {
    // 64 KiB of send buffer: 44 datagrams of 1472 at a time, refilled as simulated time passes. Each 4 MB sample is
    // 2889 datagrams, so all but the first 44 go from the cursor. Mutant: the cursor dropped (never rescheduled) -
    // fails.
    init_pair(FOUR_MB, BEST_EFFORT);
    sim_start(0);
    sim_room_per_step = 65536 / 1472;
    const int samples = 3;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
        sim_settle(i + 1, tt_SECOND);
    }
    EXPECT_EQ_INT(samples, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_TRUE(sender.large.send_waits >= 1);
    EXPECT_EQ_U32(0, sender_alloc.live); // best effort: each buffer went back once its last fragment had gone
    EXPECT_TRUE(test_mock_nonblocking_calls > samples);
}

static void test_a_lost_datagram_is_resent_while_its_sample_is_still_going_out(void) {
    // A send buffer of 8 datagrams: a 4 MB sample takes hundreds of retry intervals to go out, and a fragment lost on
    // the way is asked for long before the sample's last one leaves. It is resent at once; left to the end of the
    // send, a KEEP_LAST writer's reader gives the sample up after its retries (the first sample on the PC, every run).
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    // The reader learns the writer is KEEP_LAST - which gives up on a gap after its retries - as an announce would tell
    // it; with no announce it would wait for ever and the give-up could not show. A small sample makes the tracking.
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy != NULL) {
        proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    }
    sample_len = FOUR_MB;
    sim_loss_per_mille = 50;
    sim_room_per_step = 8;
    const int samples = 3; // the small one and two large ones
    for (int i = 1; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
        sim_settle(i + 1, 2 * tt_SECOND);
    }
    EXPECT_EQ_INT(samples, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_U32(0, sub.gap_abandoned);
}

static void test_a_busy_writer_is_waited_for_not_given_up(void) {
    // The same, but the writer reads nothing until its send ends - one blocked in a long sendmmsg() loop, as rmw's
    // first 4 MB sample on the PC was. Its reader's 1 ms retries, the interval before any recovery has been timed, run
    // out several times over during the send; while the sample's fragments keep arriving they do not count, and the
    // lost datagrams are resent once the writer reads again.
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy != NULL) {
        proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    }
    sample_len = FOUR_MB;
    sim_loss_per_mille = 50;
    sim_room_per_step = 8;
    sim_busy_writer = true;
    const int samples = 3;
    for (int i = 1; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
        sim_settle(i + 1, 2 * tt_SECOND);
    }
    EXPECT_EQ_INT(samples, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_U32(0, sub.gap_abandoned);
}

static void test_keep_last_replaces_a_sample_waiting_behind_a_send(void) {
    // Three samples published back to back while the first is still going out: the second waits behind it and the
    // third replaces it before any of it has gone. The reader sees samples 1 and 3, in order, with no gap in between -
    // the replaced sample never took seq_nos.
    init_pair(ONE_MB, RELIABLE_KEEP_LAST);
    sim_start(0);
    sim_room_per_step = 64;
    sim_acting = SENDER_ID;
    test_mock_nonblocking_room = 64;
    for (int i = 0; i < 3; i++) {
        sample_seed = (uint8_t)(i + 1);
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    }
    sim_drain();
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_INT(1, delivered_seeds[0]);
    EXPECT_EQ_INT(3, delivered_seeds[1]);
    EXPECT_EQ_U64(1, sender.large.tail_abandoned);
    EXPECT_EQ_U32(2 * tt_sample_datagrams(ONE_MB), pub.seq_no);
    EXPECT_EQ_U32(0, sub.gap_abandoned + sub.gap_evicted);
}

static void test_a_small_sample_never_overtakes_a_waiting_large_one(void) {
    // A small sample published while a large one waits behind a send takes its seq_nos after it: order is publish
    // order.
    init_pair(ONE_MB, RELIABLE_KEEP_LAST);
    sim_start(0);
    sim_room_per_step = 64;
    sim_acting = SENDER_ID;
    test_mock_nonblocking_room = 64;
    sample_seed = 1;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    sample_seed = 2;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len)); // waits behind the first
    uint32_t before_small = pub.seq_no;
    sample_len = 1000;
    sample_seed = 3;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_U32(before_small + tt_sample_datagrams(ONE_MB) + 1, pub.seq_no); // the waiting one was given its seq_nos
    sample_len = ONE_MB; // what checking_decode compares the large ones against; the small one is checked by seed
    sim_drain();
    sim_settle(3, tt_SECOND);
    EXPECT_EQ_INT(3, delivered_count);
    EXPECT_EQ_INT(1, delivered_seeds[0]);
    EXPECT_EQ_INT(2, delivered_seeds[1]);
    EXPECT_EQ_INT(3, delivered_seeds[2]);
    EXPECT_TRUE(in_order);
}

// --- L1.7: lending -------------------------------------------------------------------------------------------------

static void* release_on_another_thread(void* arg) {
    (void)arg;
    static tt_ret_t result;
    result = tt_Sample_release(&receiver, &held);
    return &result;
}

static void test_a_retained_large_sample_outlives_the_next_one(void) {
    // Retain a 4 MB sample, take another 4 MB one - whose buffer the allocator would hand out from what was released -
    // and the first still holds its own bytes. Released from another thread, its buffer then goes back. Mutant: the
    // buffer handed back when the callback returns, retained or not - the second sample lands in it.
    init_pair(FOUR_MB, BEST_EFFORT);
    sim_start(0);
    retain_next = true;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_INT(tt_RET_OK, retain_result);
    EXPECT_TRUE(held.payload != NULL);
    EXPECT_EQ_U32(ROUNDUP(FOUR_MB), held.length);
    EXPECT_EQ_U32(1, receiver_alloc.live); // the retained sample's buffer, still out
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2, tt_SECOND));
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_TRUE(held.payload != NULL && payload_intact(held.payload, held.length, FOUR_MB) && held.payload[0] == 1);
    pthread_t thread;
    EXPECT_EQ_INT(0, pthread_create(&thread, NULL, release_on_another_thread, NULL));
    void* result = NULL;
    EXPECT_EQ_INT(0, pthread_join(thread, &result));
    EXPECT_EQ_INT(tt_RET_OK, result != NULL ? *(tt_ret_t*)result : tt_RET_IO_ERROR);
    EXPECT_EQ_U32(0, receiver_alloc.live); // back to the caller at the release
    EXPECT_TRUE(held.payload == NULL);
    // A second release of the same handle is refused, touching nothing.
    struct tt_Sample stale = {NULL, 0, 1, true};
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Sample_release(&receiver, &stale));
}

// --- the writer's buffers --------------------------------------------------------------------------------------------

static void test_a_volatile_writer_hands_acknowledged_buffers_back(void) {
    // VOLATILE: once every matched reader has acknowledged a large sample, its buffer goes back to the caller.
    // Control: a DURABLE writer keeps it for a late joiner.
    init_pair(ONE_MB, RELIABLE_KEEP_LAST);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U32(0, sender_alloc.live);
    EXPECT_EQ_U32(0, pub.large_count);

    init_pair(ONE_MB, RELIABLE_KEEP_LAST);
    pub.durable = true;
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U32(1, sender_alloc.live);
    EXPECT_EQ_U32(1, pub.large_count);
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_destroy(&pub));
    EXPECT_EQ_U32(0, sender_alloc.live); // destroying the Publisher hands every buffer back
}

static void test_a_durable_writer_backs_a_late_joiner_with_its_large_samples(void) {
    init_pair(ONE_MB, RELIABLE_KEEP_LAST);
    pub.durable = true;
    sim_start(0);
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    sim_head = sim_tail; // nobody heard it the first time
    EXPECT_EQ_INT(0, delivered_count);
    sub.durable = true;
    struct tt_Peer late = {RECEIVER_ID, RECEIVER_IP, PORT};
    deliver_durability_backlog(&sender, &pub, &late);
    EXPECT_EQ_U32(tt_sample_datagrams(ONE_MB), sim_tail - sim_head);
    sim_drain();
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
}

int main(void) {
    test_round_trip_at_every_boundary();
    test_any_arrival_order_reassembles();
    test_an_inconsistent_fragment_is_dropped_and_counted();
    test_small_samples_keep_the_small_path();
    test_without_large_buffers_a_large_sample_is_refused();
    test_reliable_keep_all_at_5_percent_loss();
    test_a_lost_tail_is_recovered_within_two_retry_intervals();
    test_best_effort_without_a_buffer_is_counted_never_torn();
    test_keep_all_sample_wider_than_the_window_is_too_large();
    test_a_full_send_buffer_delays_the_sample_never_loses_it();
    test_a_lost_datagram_is_resent_while_its_sample_is_still_going_out();
    test_a_busy_writer_is_waited_for_not_given_up();
    test_keep_last_replaces_a_sample_waiting_behind_a_send();
    test_a_small_sample_never_overtakes_a_waiting_large_one();
    test_a_retained_large_sample_outlives_the_next_one();
    test_a_volatile_writer_hands_acknowledged_buffers_back();
    test_a_durable_writer_backs_a_late_joiner_with_its_large_samples();

    allocator_reset(&sender_alloc);
    allocator_reset(&receiver_alloc);
    free(sim_queue);
    if (test_result() != 0) {
        return 1;
    }
    printf("test_large_sample: all tests passed\n");
    return 0;
}
