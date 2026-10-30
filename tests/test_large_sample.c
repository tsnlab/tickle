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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_capacity.sh passes a mutant
#include "test_mock.h"

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define RECEIVER_IP 0x0a000002
// A second reader on a node of its own (PAIR_SECOND_READER): the writer's repair slots are one per seq_no, whichever
// reader they went to.
#define READER_B_ID 3
#define READER_B_IP 0x0a000003
#define PORT 8282
#define TOPIC_NAME "large_topic"
#define ENDPOINT_NAME "large_endpoint"

// The resend order (DESIGN.md section 4) is this tree's: tests/wire_identity_resend_order.sh builds this file against
// the parent's headers too, which have no tt_LARGE_REPAIR_SLOTS, to run its sim on the parent.
#ifdef tt_LARGE_REPAIR_SLOTS
#define HELD_REQUESTS(node) ((unsigned long long)(node).large.resend_held)
#else
#define HELD_REQUESTS(node) 0ULL
#endif

#define ONE_MB 1048576U
#define FOUR_MB 4194304U
#define EIGHT_MIB 8388608U
// The two sizes either side of the 8-bit index a small fragment carries: 255 and 256 datagrams.
// A sample goes padded to 4, so these are the largest multiple of 4 that 255 datagrams carry and the next one.
#define SIZE_255_FRAGMENTS ((1446U + (1452U * 254U)) & ~3U)
#define SIZE_256_FRAGMENTS (SIZE_255_FRAGMENTS + 4U)

// --- the caller's buffers: one allocator per context, with a failure switch and a reuse list -------------------------

struct test_allocator {
    uint32_t acquires; // every call, including the ones answered NULL
    uint32_t granted;  // the calls that handed a buffer out: at teardown, exactly as many as `releases`
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
static struct test_allocator reader_b_alloc;

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
            alloc->granted++;
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
    alloc->granted++;
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

// Every buffer a context was handed must come back by the time it is destroyed: the core's release paths - a sample
// delivered, an assembly abandoned, a Publisher or context destroyed - account for every one, and LeakSanitizer
// reports whatever does not. Checked per allocator so a failure names the side that kept one.
static void expect_books_balance(const char* side, const struct test_allocator* alloc) {
    if (alloc->granted != alloc->releases) {
        printf("test_large_sample: %s allocator granted %u buffers and got %u back\n", side, (unsigned)alloc->granted,
               (unsigned)alloc->releases);
    }
    EXPECT_EQ_U32(alloc->granted, alloc->releases);
    EXPECT_EQ_U32(0, alloc->live);
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

// data_encode fails while this is set (the ranged-encode failure tests): what a type that cannot encode does.
static bool whole_encode_fails;

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    if (whole_encode_fails) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = pattern(i, sample_seed);
    }
    return (int32_t)len;
}

// --- the ranged encode (DESIGN.md section 8, "Encoding while sending")
// ------------------------------------------------ On for the sender's topic while `ranged` is: sized_encode's bytes, a
// range at a time. Each call is logged with how many datagrams the sender had sent by then and whether the send buffer
// was full. Every range reaching past `range_decline_at` is declined.

static bool ranged;
static uint32_t range_decline_at = UINT32_MAX;
struct range_call {
    uint32_t offset;
    uint32_t length;
    uint64_t sent_before;
    bool buffer_full;
};
#define RANGE_CALLS_MAX 4096U
static struct range_call range_calls[RANGE_CALLS_MAX];
static uint32_t range_call_count;
static uint64_t sender_datagrams(void);

static int32_t sized_encode_range(struct tt_Data* data, uint8_t* payload, uint32_t offset, uint32_t len) {
    (void)data;
    if (range_call_count < RANGE_CALLS_MAX) {
        range_calls[range_call_count] =
            (struct range_call) {offset, len, sender_datagrams(), test_mock_nonblocking_room == 0};
    }
    range_call_count++;
    if (offset + len > range_decline_at) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = pattern(offset + i, sample_seed);
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

// A sample's length by its seed, for a run whose samples differ in size (the mixed durable backlog): 0 means
// sample_len, as every other test has it.
static uint32_t len_of_seed[256];

static int32_t checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    delivered_len = len;
    uint32_t expected_len = len_of_seed[payload[0]] != 0 ? len_of_seed[payload[0]] : sample_len;
    if (!payload_intact(payload, len, expected_len)) {
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

// The in-place decode rmw_tickle gives every subscription: the payload itself is the sample.
static struct tt_Data* checking_decode_inplace(const uint8_t* payload, uint32_t len, bool native) {
    (void)checking_decode(NULL, payload, len, native);
    return (struct tt_Data*)payload;
}

// The topics' C struct size: 8 (a size, as most tests use), or a fixed-size type above tt_MAX_SAMPLE_LENGTH -
// perf_test's Array1m, 1 MB and its header - whose Subscriber then decodes in place, as rmw_tickle's does.
#define ARRAY1M_STRUCT (1048576U + 16U)
static uint32_t pair_struct_size = 8;

static struct tt_Subscriber sub_b;
static int delivered_b; // samples the second reader (PAIR_SECOND_READER) got; delivered_count is the first one's

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)data;
    if (s == &sub_b) {
        delivered_b++;
        return;
    }
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
static struct tt_Context reader_b;
static bool second_reader; // init_pair_with() made reader_b
static struct tt_TypeSupport sender_topic;
static struct tt_TypeSupport receiver_topic;
static struct tt_Publisher pub;

#define CACHE_DEPTH 256
static struct tt_ReliableCacheIndex cache_index[CACHE_DEPTH];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache cache;
// The 8192-bit window rmw_tickle gives a subscription whose type can be large (DESIGN.md section 8).
#define WIDE_WORDS (tt_RELIABLE_BITMAP_MAX_BITS / 64)
// A durable backlog's reader holds each fragment of a small sample that fragments in a slot until its sample is in
// order (tickle.h, reorder_storage): PAIR_REORDER gives the reader MIXED_REORDER_SLOTS of them.
#define MIXED_REORDER_SLOTS tt_RELIABLE_BITMAP_MAX_BITS // the wide window: fewer are refused (test_reorder_window.c)
#define MIXED_REORDER_SLOT_BYTES tt_REORDER_SLOT_SIZE(tt_CONTROL_MAX_LENGTH)
// The RELIABLE reader's storage: its wide window, then its reorder buffer when it has one, else its own reassembly
// slot (tt_Subscriber_storage_size(); docs/DESIGN.md section 8, "Reassembly slots").
static uint64_t sub_storage[((size_t)tt_MAX_PEER_COUNT * WIDE_WORDS) +
                            (MIXED_REORDER_SLOTS * MIXED_REORDER_SLOT_BYTES / sizeof(uint64_t))];
static uint64_t sub_b_storage[((size_t)tt_MAX_PEER_COUNT * WIDE_WORDS) + (tt_FRAG_SLOT_BYTES / sizeof(uint64_t))];
// What the RELIABLE writer's creation carves; the pair's cache (above) replaces it.
static uint64_t created_storage[(size_t)256U * 1024U / sizeof(uint64_t)];

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

enum pair_mode { BEST_EFFORT, RELIABLE_KEEP_LAST, RELIABLE_KEEP_ALL };

static bool pair_up; // init_pair() made the two contexts and teardown_pair() has not yet destroyed them

// Destroys both contexts as a user would, then checks that every large buffer either one acquired was released: an
// assembly still being put together and a sample a Publisher still holds go back at the context's destruction
// (node_destroy_locked()). Each test's pair is torn down before the next is made, and the last one in main().
static void teardown_pair(void) {
    if (!pair_up) {
        return;
    }
    pair_up = false;
    test_mock_send_hook = NULL; // the farewell announces go nowhere: no test reads them
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&sender));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&receiver));
    expect_books_balance("sender", &sender_alloc);
    expect_books_balance("receiver", &receiver_alloc);
    if (second_reader) {
        second_reader = false;
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&reader_b));
        expect_books_balance("second reader", &reader_b_alloc);
    }
}

// A Publisher and a Subscriber of one topic, each in its own context with its own large buffers. RELIABLE modes give
// the Publisher a cache and the Subscriber the wide window, and pre-match the reader on the writer with that window,
// as its announce would.
// init_pair_with()'s flags: a TRANSIENT_LOCAL writer, a TRANSIENT_LOCAL reader, a reader with a reorder buffer.
#define PAIR_DURABLE_WRITER 1U
#define PAIR_DURABLE_READER 2U
#define PAIR_REORDER 4U
// The reader is the writer's one known peer, so the writer unicasts to it (tt_UNICAST_PEER_THRESHOLD) - without it
// two more peers keep the broadcast most cases were written against.
#define PAIR_UNICAST 8U
// A second RELIABLE reader, reader_b, on a node of its own (READER_B_ID): the writer's broadcast reaches both, each
// datagram lost to both or to neither (sim_deliver_now()), its unicast reaches the one it names.
#define PAIR_SECOND_READER 16U

// `options`: the writer's (NULL: none).
static void init_pair_with(uint32_t len, enum pair_mode mode, unsigned flags,
                           const struct tt_PublisherOptions* options) {
    teardown_pair();
    test_mock_reset();
    sample_len = len;
    sample_seed = 1;
    delivered_count = 0;
    delivered_b = 0;
    delivered_torn = 0;
    delivered_len = 0;
    last_seq = 0;
    in_order = true;
    retain_next = false;
    retain_result = tt_RET_OK;
    memset(&held, 0, sizeof(held));
    memset(len_of_seed, 0, sizeof(len_of_seed));
    allocator_reset(&sender_alloc);
    allocator_reset(&receiver_alloc);
    allocator_reset(&reader_b_alloc);

    init_bare_node(&sender, SENDER_ID);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = pair_struct_size;
    sender_topic.data_encode_size = sized_encode_size;
    sender_topic.data_encode = sized_encode;
    sender_topic.data_encode_range = ranged ? sized_encode_range : NULL;
    range_call_count = 0;
    range_decline_at = UINT32_MAX;
    whole_encode_fails = false;
    // RELIABLE: KEEP_LAST or KEEP_ALL from the profile, and TRANSIENT_LOCAL when asked. White-box: the samples it
    // retains are the pair's own cache, a ring of CACHE_DEPTH datagrams with no sample depth - which no history.depth
    // gives (a KEEP_LAST depth would also bound the large ring) - so the created cache is replaced with it below.
    bool reliable = mode != BEST_EFFORT;
    struct tt_QosProfile pub_qos = TEST_QOS_UNRELIABLE;
    if (reliable) {
        pub_qos.reliability.kind = tt_QOS_RELIABILITY_RELIABLE;
        pub_qos.history.kind = mode == RELIABLE_KEEP_ALL ? tt_QOS_HISTORY_KEEP_ALL : tt_QOS_HISTORY_KEEP_LAST;
        pub_qos.history.depth = 1;
    }
    if ((flags & PAIR_DURABLE_WRITER) != 0) {
        pub_qos.durability.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL;
    }
    size_t pub_storage_size = tt_Publisher_storage_size(&sender_topic, &pub_qos);
    EXPECT_TRUE(pub_storage_size <= sizeof(created_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(
                                 tt_Context_default_node(&sender), &pub, &sender_topic, ENDPOINT_NAME, &pub_qos,
                                 options, pub_storage_size != 0 ? created_storage : NULL, pub_storage_size));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&sender, test_acquire, test_release, &sender_alloc));

    init_bare_node(&receiver, RECEIVER_ID);
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = pair_struct_size;
    receiver_topic.data_decode = checking_decode;
    receiver_topic.data_decode_inplace = pair_struct_size > tt_MAX_SAMPLE_LENGTH ? checking_decode_inplace : NULL;
    receiver_topic.data_free = free_nothing;
    // RELIABLE: the wide window, and the reorder buffer when asked; TRANSIENT_LOCAL when asked.
    struct tt_QosProfile sub_qos = TEST_QOS_UNRELIABLE;
    if (reliable) {
        sub_qos = TEST_QOS_RELIABLE_READER;
        sub_qos.reliability.tracking_words = WIDE_WORDS;
        if ((flags & PAIR_REORDER) != 0) {
            sub_qos.reliability.reorder_slots = MIXED_REORDER_SLOTS;
        }
    }
    if ((flags & PAIR_DURABLE_READER) != 0) {
        sub_qos.durability.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL;
    }
    size_t sub_storage_size = tt_Subscriber_storage_size(&receiver_topic, &sub_qos);
    EXPECT_TRUE(sub_storage_size <= sizeof(sub_storage));
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub, &receiver_topic, ENDPOINT_NAME,
                                            &sub_qos, &(struct tt_SubscriberOptions) {.callback = on_data},
                                            sub_storage_size != 0 ? sub_storage : NULL, sub_storage_size));
    EXPECT_TRUE((flags & PAIR_REORDER) == 0 || sub.reorder_slot_bytes == MIXED_REORDER_SLOT_BYTES);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&receiver, test_acquire, test_release, &receiver_alloc));
    second_reader = reliable && (flags & PAIR_SECOND_READER) != 0;
    if (second_reader) {
        init_bare_node(&reader_b, READER_B_ID);
        struct tt_QosProfile b_qos = TEST_QOS_RELIABLE_READER;
        b_qos.reliability.tracking_words = WIDE_WORDS;
        size_t b_size = tt_Subscriber_storage_size(&receiver_topic, &b_qos);
        EXPECT_TRUE(b_size <= sizeof(sub_b_storage));
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&reader_b), &sub_b, &receiver_topic,
                                                           ENDPOINT_NAME, &b_qos,
                                                           &(struct tt_SubscriberOptions) {.callback = on_data},
                                                           b_size != 0 ? sub_b_storage : NULL, b_size));
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&reader_b, test_acquire, test_release, &reader_b_alloc));
    }

    if (mode != BEST_EFFORT) {
        memset(cache_index, 0, sizeof(cache_index));
        memset(&cache, 0, sizeof(cache));
        cache.index = cache_index;
        cache.capacity = CACHE_DEPTH;
        cache.depth = CACHE_DEPTH;
        cache.arena = cache_arena;
        cache.arena_size = (uint32_t)sizeof(cache_arena);
        pub.reliable_cache = &cache;
        struct tt_PeerAck* ack = claim_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id);
        EXPECT_TRUE(ack != NULL);
        if (ack != NULL) {
            ack->tracking_words = WIDE_WORDS;
        }
        if (second_reader) {
            ack = claim_peer_ack(&pub, READER_B_ID, sub_b.endpoint.entity_id);
            EXPECT_TRUE(ack != NULL);
            if (ack != NULL) {
                ack->tracking_words = WIDE_WORDS;
            }
            (void)upsert_peer(pub.peers, READER_B_ID, READER_B_IP, PORT);
        }
    }
    // A Publisher with no matched reader sends nothing (2026-10-15): the reader is matched as its announce would match
    // it, and two more known peers keep the broadcast these cases were written against.
    (void)upsert_peer(pub.peers, RECEIVER_ID, RECEIVER_IP, PORT);
    for (uint8_t i = 1; (flags & PAIR_UNICAST) == 0 && i <= tt_UNICAST_PEER_THRESHOLD; i++) {
        (void)upsert_peer(pub.peers, (uint8_t)(100U + i), 0x0a000100U + i, PORT);
    }
    pair_up = true;
}

static void init_pair(uint32_t len, enum pair_mode mode) {
    init_pair_with(len, mode, 0, NULL);
}

// --- the link: a queue of datagrams both ways, with loss
// --------------------------------------------------------------

#define SIM_QUEUE 32768U
#define SIM_DATAGRAM_MAX 1500U

struct sim_datagram {
    uint8_t from;
    uint32_t to_ip; // 0: broadcast (test_mock_send_hook_ip)
    uint32_t len;
    uint8_t bytes[SIM_DATAGRAM_MAX];
};
static struct sim_datagram* sim_queue;
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting;
static uint32_t sim_loss_per_mille;
static uint32_t sim_rng;
static uint64_t sim_sent[4];       // by node id
static uint64_t sim_dropped[4];    // by the sending node's id
static uint32_t sim_room_per_step; // 0: unlimited; else the send buffer's room, in datagrams, refilled each step
static bool sim_reverse;           // deliver the sender's datagrams newest first, a burst at a time
// Drops the last fragment of each large sample once (the tail test).
static bool sim_drop_tails;
// ... and the end-of-sample HEARTBEAT that follows each dropped tail, once.
static bool sim_drop_tail_heartbeats;
static bool sim_heartbeat_owed;
static uint32_t sim_tails_dropped[64];
static uint32_t sim_tail_count;
// Drops the first sim_unit_drops[i].copies copies of the writer's continuation fragment sim_unit_drops[i].index -
// its original, then its repairs - of whichever large sample it is in.
static struct {
    uint16_t index;
    uint8_t copies;
} sim_unit_drops[4];
static uint32_t sim_oversize;
// A writer busy sending a large sample reads nothing until the send ends, as one blocked in a long sendmmsg() loop
// does: the reader's datagrams to it wait here meanwhile.
static bool sim_busy_writer;
static struct sim_datagram* sim_held;
static uint32_t sim_held_count;
#define SIM_HELD 4096U
// The reader's datagrams take this long to reach the writer (0: none), so a repair comes back that long after it was
// asked for - a path whose round trip the reader's retry interval can be seen to track.
static uint64_t sim_reverse_delay;
static struct sim_datagram* sim_late;
static uint64_t sim_late_due[SIM_HELD];
static uint32_t sim_late_head;
static uint32_t sim_late_tail;
// The writer's unicast datagrams take this long to reach the reader (0: none), its broadcast ones none: a queue the
// broadcast path does not cross - a netem band of its own, a Wi-Fi AP's multicast queue, another NIC queue. Not with
// sim_reverse_delay, which shares the late queue and would leave it out of due order.
static uint64_t sim_unicast_delay;
// A slow reader (0: none): it takes sim_reader_service_ns to read each datagram the writer sent it, and its socket
// holds sim_reader_capacity of them waiting; one arriving at a full socket is dropped there (sim_socket_drops) - loss
// at the reader, not on the path. Its timers run meanwhile, as a reader's own do while it is behind. Not with the two
// delays above, which share the late queue.
static uint64_t sim_reader_service_ns;
static uint32_t sim_reader_capacity;
static uint64_t sim_reader_free_ns;
static uint64_t sim_socket_drops;
// With a second reader: the writer's broadcast is lost to the first reader alone, never to reader_b.
static bool sim_loss_first_reader_only;
// The first send of every sim_whole_loss_every-th sample sim_publish() publishes (by its seed; 0: none) goes nowhere,
// its HEARTBEAT through: the reader asks for the whole sample at once, as after its receive drain overran (RESULTS
// `EWS`). sim_whole_lost counts the samples so lost.
static uint32_t sim_whole_loss_every;
static uint32_t sim_whole_lost;
static bool sim_whole_losing;

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

static bool sim_unit_drop(const uint8_t* bytes) {
    const uint8_t* body = NULL;
    if (datagram_type(bytes, &body) != tt_SUBMESSAGE_TYPE_FRAG_CONT_L) {
        return false;
    }
    struct tt_FragContLHeader cont;
    memcpy(&cont, body, sizeof(cont));
    for (size_t i = 0; i < sizeof(sim_unit_drops) / sizeof(sim_unit_drops[0]); i++) {
        if (sim_unit_drops[i].copies != 0 && sim_unit_drops[i].index == cont.frag_index) {
            sim_unit_drops[i].copies--;
            return true;
        }
    }
    return false;
}

static void sim_capture(const void* buf, size_t len) {
    if (len > SIM_DATAGRAM_MAX) {
        sim_oversize++;
        return;
    }
    const uint8_t* body = NULL;
    uint8_t type = sim_whole_losing && sim_acting == SENDER_ID ? datagram_type((const uint8_t*)buf, &body) : 0;
    if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L || type == tt_SUBMESSAGE_TYPE_FRAG_CONT_L ||
        type == tt_SUBMESSAGE_TYPE_FRAG_RUN_L) {
        sim_sent[sim_acting]++;
        sim_dropped[sim_acting]++;
        return;
    }
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->to_ip = test_mock_send_hook_ip;
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
    sim_sent[sim_acting]++;
}

static uint64_t sender_datagrams(void) {
    return sim_sent[SENDER_ID];
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

static void sim_deliver_now(struct sim_datagram* d);

static void sim_deliver(struct sim_datagram* d) {
    if (sim_reader_service_ns != 0 && d->from == SENDER_ID) {
        if (sim_late_tail - sim_late_head >= sim_reader_capacity) {
            sim_socket_drops++;
            return;
        }
        uint64_t start = sim_reader_free_ns > test_mock_now ? sim_reader_free_ns : test_mock_now;
        sim_reader_free_ns = start + sim_reader_service_ns;
        sim_late[sim_late_tail % SIM_HELD] = *d;
        sim_late_due[sim_late_tail % SIM_HELD] = sim_reader_free_ns;
        sim_late_tail++;
        return;
    }
    if ((sim_reverse_delay != 0 && d->from != SENDER_ID) ||
        (sim_unicast_delay != 0 && d->from == SENDER_ID && d->to_ip != 0)) {
        EXPECT_TRUE(sim_late_tail - sim_late_head < SIM_HELD);
        if (sim_late_tail - sim_late_head < SIM_HELD) {
            sim_late[sim_late_tail % SIM_HELD] = *d;
            sim_late_due[sim_late_tail % SIM_HELD] =
                test_mock_now + (d->from != SENDER_ID ? sim_reverse_delay : sim_unicast_delay);
            sim_late_tail++;
        }
        return;
    }
    sim_deliver_now(d);
}

static void sim_deliver_now(struct sim_datagram* d) {
    const uint8_t* body = NULL;
    if (sim_busy_writer && d->from != SENDER_ID && sim_writer_sending()) {
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
    bool b_spared = second_reader && sim_loss_first_reader_only && d->from == SENDER_ID && d->to_ip == 0;
    if (b_spared) {
        uint8_t copy[SIM_DATAGRAM_MAX];
        memcpy(copy, d->bytes, d->len);
        sim_acting = READER_B_ID;
        process_packet(&reader_b, copy, 0, d->len, SENDER_IP, PORT, tt_TRANSPORT_UDP);
    }
    if (sim_lost() || (d->from == SENDER_ID && (sim_tail_drop(d->bytes) || sim_unit_drop(d->bytes)))) {
        sim_dropped[d->from]++;
        return;
    }
    if (d->from != SENDER_ID) {
        sim_acting = SENDER_ID;
        process_packet(&sender, d->bytes, 0, d->len, d->from == READER_B_ID ? READER_B_IP : RECEIVER_IP, PORT,
                       tt_TRANSPORT_UDP);
        return;
    }
    if (second_reader && ((d->to_ip == 0 && !b_spared) || d->to_ip == READER_B_IP)) {
        uint8_t copy[SIM_DATAGRAM_MAX];
        memcpy(copy, d->bytes, d->len); // reader_b gets its own copy of a broadcast, as from the wire
        sim_acting = READER_B_ID;
        process_packet(&reader_b, copy, 0, d->len, SENDER_IP, PORT, tt_TRANSPORT_UDP);
    }
    if (!second_reader || d->to_ip == 0 || d->to_ip == RECEIVER_IP) {
        sim_acting = RECEIVER_ID;
        process_packet(&receiver, d->bytes, 0, d->len, SENDER_IP, PORT, tt_TRANSPORT_UDP);
    }
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
    uint64_t next = a < b ? a : b;
    if (second_reader && sched_next_time(&reader_b, &b) && b < next) {
        next = b;
    }
    if (sim_late_head != sim_late_tail && sim_late_due[sim_late_head % SIM_HELD] < next) {
        next = sim_late_due[sim_late_head % SIM_HELD];
    }
    return next;
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
    while (sim_late_head != sim_late_tail && sim_late_due[sim_late_head % SIM_HELD] <= test_mock_now) {
        sim_deliver_now(&sim_late[sim_late_head % SIM_HELD]);
        sim_late_head++;
        sim_drain();
    }
    sim_run_due(&sender);
    sim_run_due(&receiver);
    if (second_reader) {
        sim_run_due(&reader_b);
    }
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
    memset(sim_unit_drops, 0, sizeof(sim_unit_drops));
    sim_drop_tail_heartbeats = false;
    sim_heartbeat_owed = false;
    sim_oversize = 0;
    if (sim_held == NULL) {
        sim_held = (struct sim_datagram*)calloc(SIM_HELD, sizeof(struct sim_datagram));
    }
    sim_busy_writer = false;
    sim_held_count = 0;
    if (sim_late == NULL) {
        sim_late = (struct sim_datagram*)calloc(SIM_HELD, sizeof(struct sim_datagram));
    }
    sim_reverse_delay = 0;
    sim_unicast_delay = 0;
    sim_reader_service_ns = 0;
    sim_reader_capacity = 0;
    sim_reader_free_ns = 0;
    sim_socket_drops = 0;
    sim_loss_first_reader_only = false;
    sim_whole_loss_every = 0;
    sim_whole_lost = 0;
    sim_whole_losing = false;
    sim_late_head = sim_late_tail = 0;
    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
}

// Publishes one sample (seed `seed`), retrying while KEEP_ALL says WOULD_BLOCK, and runs the link until it is quiet.
static tt_ret_t sim_publish(uint8_t seed, uint64_t budget) {
    sample_seed = seed;
    sim_run_due(&sender);
    sim_run_due(&receiver);
    if (second_reader) {
        sim_run_due(&reader_b);
    }
    sim_acting = SENDER_ID;
    if (sim_room_per_step != 0) {
        test_mock_nonblocking_room = (int32_t)sim_room_per_step;
    }
    sim_whole_losing = sim_whole_loss_every != 0 && seed % sim_whole_loss_every == 0;
    sim_whole_lost += sim_whole_losing ? 1U : 0U;
    tt_ret_t result = tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len);
    sim_whole_losing = false;
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
    while ((delivered_count < want || (second_reader && delivered_b < want)) && test_mock_now - since < budget) {
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

static void test_a_fixed_size_type_above_a_sample_is_a_large_topic(void) {
    // A type whose C struct is above tt_MAX_SAMPLE_LENGTH - a fixed 1 MB array, perf_test's Array1m - is a topic like
    // any other with large samples: its Publisher and in-place Subscriber are created and a sample goes through.
    // Refused until 2026-10-09 (by core's topic check and rmw's), which voided every rig run of Array1m.
    pair_struct_size = ARRAY1M_STRUCT;
    round_trip(ONE_MB, BEST_EFFORT, false);
    round_trip(ONE_MB, RELIABLE_KEEP_LAST, false);
    // A Subscriber of it without an in-place decode is refused: the copying decode puts the struct on the stack.
    struct tt_TypeSupport copying = receiver_topic;
    copying.data_decode_inplace = NULL;
    static struct tt_Subscriber refused;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_subscriber(tt_Context_default_node(&receiver), &refused, &copying, "copying",
                                            &TEST_QOS_UNRELIABLE, &(struct tt_SubscriberOptions) {.callback = on_data},
                                            NULL, 0));
    // Nor is any struct above the largest sample core can carry.
    struct tt_TypeSupport too_large = sender_topic;
    too_large.data_size = 1446U + (1452U * (tt_LARGE_MAX_FRAGMENTS - 1U)) + 1U;
    static struct tt_Publisher refused_pub;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_publisher(tt_Context_default_node(&sender), &refused_pub, &too_large, "too_large",
                                           &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    too_large.data_size--;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&sender), &refused_pub, &too_large,
                                                      "largest", &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    pair_struct_size = 8;
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

// One run of `samples` 1 MB samples at `loss` per mille, from a writer whose first seq_no is `first`.
struct wrap_run {
    int delivered;
    bool in_order;
    uint64_t sent[3];
    uint32_t sender_live; // large buffers the writer still holds once settled
    uint32_t abandoned;   // what the reader gave up: gap_abandoned + gap_evicted, and assemblies abandoned
};

static struct wrap_run large_run_from(uint32_t first, int samples, enum pair_mode mode, uint32_t loss) {
    init_pair(ONE_MB, mode);
    sim_start(loss);
    pub.seq_no = first - 1U;
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        pub.peer_acks[i].ack_seq_no = first; // matched before the move: where its history now begins
    }
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), 10 * tt_SECOND));
    }
    sim_settle(samples, 10 * tt_SECOND);
    uint64_t quiet_by = test_mock_now + tt_SECOND; // the last acknowledgements, and what they hand back
    while (test_mock_now < quiet_by && sim_step()) {
    }
    struct wrap_run run = {delivered_count,
                           in_order && delivered_torn == 0,
                           {0, 0, 0},
                           sender_alloc.live,
                           sub.gap_abandoned + sub.gap_evicted + (uint32_t)receiver.large.abandoned};
    memcpy(run.sent, sim_sent, sizeof(run.sent));
    for (int i = 0; i < samples && i < 64; i++) {
        run.in_order = run.in_order && (i >= delivered_count || delivered_seeds[i] == i + 1 || mode == BEST_EFFORT);
    }
    return run;
}

static void expect_large_wrap(const char* name, int samples, enum pair_mode mode, uint32_t loss, bool all_delivered) {
    // Starting 2 x 723 datagrams short of 2^32, the third 1 MB sample's first seq_no is 0 - once both "no send in
    // progress" (large_cursor.seq_no) and "no large record". Against the same run from seq_no 1: as many samples, in
    // order, the same datagrams each way, the same given up, and the acknowledged buffers handed back alike.
    struct wrap_run control = large_run_from(1, samples, mode, loss);
    struct wrap_run wrap = large_run_from(0U - (2U * tt_sample_datagrams(ONE_MB)), samples, mode, loss);
    EXPECT_TRUE(pub.seq_no < 0x80000000U); // treatment: the writer's seq_no passed 2^32
    EXPECT_EQ_INT(control.delivered, wrap.delivered);
    EXPECT_TRUE(control.in_order && wrap.in_order);
    EXPECT_EQ_U64(control.sent[SENDER_ID], wrap.sent[SENDER_ID]);
    EXPECT_EQ_U64(control.sent[RECEIVER_ID], wrap.sent[RECEIVER_ID]);
    EXPECT_EQ_U32(control.sender_live, wrap.sender_live);
    EXPECT_EQ_U32(control.abandoned, wrap.abandoned);
    if (all_delivered) {
        EXPECT_EQ_INT(samples, wrap.delivered);
    }
    printf("test_large_sample: %s, %d x 1 MB across 2^32 - %d delivered, %lu/%lu datagrams (control %d, %lu/%lu), "
           "%u buffers held, %u given up (control %u, %u)\n",
           name, samples, wrap.delivered, (unsigned long)wrap.sent[SENDER_ID], (unsigned long)wrap.sent[RECEIVER_ID],
           control.delivered, (unsigned long)control.sent[SENDER_ID], (unsigned long)control.sent[RECEIVER_ID],
           wrap.sender_live, wrap.abandoned, control.sender_live, control.abandoned);
}

static void test_large_samples_cross_2_32_as_if_they_had_not(void) {
    expect_large_wrap("RELIABLE KEEP_ALL 5% loss", 5, RELIABLE_KEEP_ALL, 50, true);
    expect_large_wrap("RELIABLE KEEP_LAST 5% loss", 5, RELIABLE_KEEP_LAST, 50, false);
    expect_large_wrap("BEST_EFFORT lossless", 5, BEST_EFFORT, 0, true);
}

static void test_best_effort_abandons_older_samples_across_2_32(void) {
    // BEST_EFFORT: the second sample's last fragment is lost, so it never completes, and the third - starting at
    // seq_no 0, past 2^32 - abandons it when it does (large_accept_best_effort()): best-effort order never delivers the
    // older one now. Against the same from seq_no 1: as many delivered and abandoned, the receiver holding as many
    // buffers.
    uint64_t abandoned[2];
    uint32_t live[2];
    for (int control = 0; control < 2; control++) {
        init_pair(ONE_MB, BEST_EFFORT);
        sim_start(0);
        pub.seq_no = (control != 0 ? 1U : 0U - (2U * tt_sample_datagrams(ONE_MB))) - 1U;
        for (int i = 0; i < 4; i++) {
            sim_drop_tails = i == 1;
            EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
        }
        sim_drop_tails = false;
        sim_settle(3, tt_SECOND);
        abandoned[control] = receiver.large.abandoned;
        live[control] = receiver_alloc.live;
        EXPECT_EQ_INT(3, delivered_count);
    }
    EXPECT_TRUE(abandoned[1] > 0); // control: the older ones were abandoned at all
    EXPECT_EQ_U64(abandoned[1], abandoned[0]);
    EXPECT_EQ_U32(live[1], live[0]);
}

static void test_a_durable_backlog_of_large_samples_across_2_32(void) {
    // Three 1 MB samples nobody heard, the third starting at seq_no 0, then a DURABLE late joiner: the backlog walks
    // the retained run - small cache and large records - through 2^32 (large_send_backlog_before()). The control is
    // the same from seq_no 1.
    for (int control = 0; control < 2; control++) {
        init_pair_with(ONE_MB, RELIABLE_KEEP_LAST, PAIR_DURABLE_WRITER | PAIR_DURABLE_READER, NULL);
        sim_start(0);
        uint32_t first = control != 0 ? 1U : 0U - (2U * tt_sample_datagrams(ONE_MB));
        pub.seq_no = first - 1U;
        sim_acting = SENDER_ID;
        for (int i = 0; i < 3; i++) {
            sample_seed = (uint8_t)(i + 1);
            EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
            sim_head = sim_tail; // nobody heard it
        }
        struct tt_Peer late = {RECEIVER_ID, RECEIVER_IP, PORT};
        // The initial Heartbeat first, so the reader's baseline is the writer's oldest before any backlog arrives. A
        // match sends it after the backlog, which test_a_late_joiner_gets_a_large_backlog_from_a_writer_far_past_1()
        // covers.
        send_initial_heartbeat(&sender, &pub, &late);
        sim_drain();
        sim_head = sim_tail;
        deliver_durability_backlog(&sender, &pub, &late);
        EXPECT_EQ_U32(3U * tt_sample_datagrams(ONE_MB), sim_tail - sim_head);
        sim_drain();
        sim_settle(3, tt_SECOND);
        printf("test_large_sample: durable backlog of 3 x 1 MB from %u: %d delivered, %d torn\n", first,
               delivered_count, delivered_torn);
        EXPECT_EQ_INT(3, delivered_count);
        EXPECT_EQ_INT(0, delivered_torn);
    }
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
           (double)both / 1e6, (double)reliable_retry_interval_publisher(NULL) / 1e6);
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

// Runs the protocol until `want` samples have been delivered, as sim_settle() does, and answers how long after its
// send ended - its last datagram gone the first time - the last of them was: 0 when it was whole by then.
static uint64_t sim_settle_after_send(int want, uint64_t budget) {
    uint64_t since = test_mock_now;
    uint64_t send_end = 0;
    bool ended = false;
    while (delivered_count < want && test_mock_now - since < budget) {
        if (!ended && !sim_writer_sending()) {
            ended = true;
            send_end = test_mock_now;
        }
        if (!sim_step()) {
            break;
        }
    }
    if (delivered_count < want || want < 1 || want > 64 || !ended) {
        return 0; // not delivered (the caller's count says so), or whole before its send was seen to end
    }
    return delivered_at[want - 1] - send_end;
}

static void test_a_lost_datagram_is_resent_while_its_sample_is_still_going_out(void) {
    // A send buffer of 8 datagrams: a 4 MB sample takes hundreds of retry intervals to go out, and a fragment lost on
    // the way is asked for long before the sample's last one leaves. It is resent at once, so the sample is whole when
    // its send ends, but for what was lost in the last retry interval or two. Left to the end of the send, every loss
    // is recovered after it: delivered and not given up - the reader does not count its retries while fragments keep
    // arriving (test_a_busy_writer_is_waited_for_not_given_up) - but 15 and 227 ms after the send ended rather than at
    // it (measured 2026-10-11). Mutant: no resend before the send ends - fails the bound.
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
    uint64_t slowest = 0;
    for (int i = 1; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
        uint64_t after = sim_settle_after_send(i + 1, 2 * tt_SECOND);
        slowest = after > slowest ? after : slowest;
    }
    EXPECT_EQ_INT(samples, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_U32(0, sub.gap_abandoned);
    printf("test_large_sample: 4 MB at 5%% loss through 8 datagrams of send buffer - whole %.2f ms after its send "
           "ended\n",
           (double)slowest / 1e6);
    EXPECT_TRUE(slowest <= 2 * TEST_RETRY_INTERVAL);
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

// What the writer's HEARTBEATs announced while a large sample of it was still going out
// (test_a_heartbeat_announces_no_sample_still_going_out).
static uint32_t hb_during;      // HEARTBEATs sent while a large record was unsent
static uint32_t hb_announcing;  // ... of them, those whose last_seq_no reached that record
static uint32_t hb_after;       // HEARTBEATs sent with nothing left to send
static uint32_t hb_after_whole; // ... of them, those that announced everything given out

static void heartbeat_watch(const void* buf, size_t len) {
    const uint8_t* body = NULL;
    if (sim_acting == SENDER_ID && len >= sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) &&
        datagram_type((const uint8_t*)buf, &body) == tt_SUBMESSAGE_TYPE_HEARTBEAT) {
        struct tt_HeartbeatHeader heartbeat;
        memcpy(&heartbeat, body, sizeof(heartbeat));
        const struct tt_LargeRecord* unsent = NULL;
        for (uint32_t i = 0; i < pub.large_count && unsent == NULL; i++) {
            const struct tt_LargeRecord* record = &pub.large[(pub.large_head + i) % tt_LARGE_RETAINED];
            unsent = record->sent ? NULL : record;
        }
        if (unsent != NULL) {
            hb_during++;
            hb_announcing += seq_delta(heartbeat.last_seq_no, unsent->seq_no) >= 0 ? 1U : 0U;
        } else {
            hb_after++;
            hb_after_whole += heartbeat.last_seq_no == pub.seq_no ? 1U : 0U;
        }
    }
    sim_capture(buf, len);
}

static void test_a_heartbeat_announces_no_sample_still_going_out(void) {
    // A HEARTBEAT sent while a large sample is still going out announces up to the seq_no before it
    // (publisher_announced_last_seq_no()): announced, the unsent rest would read as a gap and be asked for, over and
    // over until the send ended - at 0% loss, requests for datagrams nobody lost. The delivery tests cannot see this:
    // the requests are ignored (a datagram not yet sent is not resent) and the sample arrives as before
    // (mutants_large_sample.py, "a HEARTBEAT announces a large sample still going out", 2026-10-16). Controls: some
    // HEARTBEATs did go out during the send, and those after it announce everything given out - what is read here is
    // the field the mutant would change.
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sample_len = FOUR_MB;
    sim_room_per_step = 8; // hundreds of retry intervals to go out
    hb_during = hb_announcing = hb_after = hb_after_whole = 0;
    test_mock_send_hook = heartbeat_watch;
    const int samples = 3; // the small one and two large ones
    for (int i = 1; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
        sim_settle(i + 1, 2 * tt_SECOND);
    }
    test_mock_send_hook = sim_capture;
    EXPECT_EQ_INT(samples, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_TRUE(hb_during > 0);
    EXPECT_EQ_U32(0, hb_announcing);
    EXPECT_TRUE(hb_after > 0);
    EXPECT_EQ_U32(hb_after, hb_after_whole);
    printf("test_large_sample: HEARTBEATs during a 4 MB send %u (announcing it: %u), after %u\n", hb_during,
           hb_announcing, hb_after);
}

static int log_occurrences(const char* text, const char* needle) {
    int count = 0;
    for (const char* at = strstr(text, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

// Runs `scenario` with the log captured, and answers how many "still waiting" warnings the reader logged.
static int stuck_warnings_in(void (*scenario)(void)) {
    char* text = NULL;
    size_t size = 0;
    FILE* log = open_memstream(&text, &size);
    tt_LogLevel level = tt_current_log_level;
    tt_current_log_level = TT_LOG_WARNING;
    tt_log_set_output(log);
    scenario();
    (void)fclose(log);
    tt_log_set_output(stderr);
    tt_current_log_level = level;
    int count = log_occurrences(text != NULL ? text : "", "Still waiting on reliable seq_no");
    free(text);
    return count;
}

// The busy writer of test_a_busy_writer_is_waited_for_not_given_up, its clock past the warning's first rate limit.
static void busy_writer_scenario(void) {
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    test_mock_now = 10 * tt_SECOND; // tt_WriterProxy.stuck_warned_ns starts at 0: a warning may go at once
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
}

// A writer gone silent with a gap open: sample 2 lost, sample 3 through, then nothing either way.
static void silent_writer_scenario(void) {
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    test_mock_now = 10 * tt_SECOND;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_loss_per_mille = 1000;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2, tt_SECOND));
    sim_loss_per_mille = 0;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(3, tt_SECOND));
    sim_loss_per_mille = 1000;
    uint64_t since = test_mock_now;
    while (test_mock_now - since < 100 * TEST_RETRY_INTERVAL && sim_step()) {
    }
    EXPECT_TRUE(delivered_count < 3); // sample 2 never came
}

static void test_a_busy_writer_is_not_warned_about(void) {
    // While a large sample's fragments keep arriving, a retry is not counted against the writer: it is still sending
    // and answers when the send ends. Counted, the busy writer's 4 MB send ran the reader's retry count to 18, past
    // tt_RELIABLE_RETRY, and a reader then warns that the writer "neither repairs it nor says it is gone" - about a
    // writer doing neither only because it is busy. Delivery cannot show this: since 2026-10-15 no gap is given up on a
    // retry count. The control: a writer that has really gone silent with a gap open is warned about, on the same
    // clock. Since 9d716d60 the uncounting is not what keeps this at 0: a retry to a sending writer that has answered
    // nothing within the interval is not sent, so the busy writer's retries never reach the count (4 counted in the
    // whole send, traced 2026-10-26). The uncounting's guard is test_a_repairing_writer_is_not_warned_about.
    int busy = stuck_warnings_in(busy_writer_scenario);
    int silent = stuck_warnings_in(silent_writer_scenario);
    printf("test_large_sample: \"still waiting\" warnings - busy writer %d, silent writer %d\n", busy, silent);
    EXPECT_EQ_INT(0, busy);
    EXPECT_TRUE(silent >= 1);
}

// The busy writer of test_a_busy_writer_is_waited_for_not_given_up, sending `samples` 4 MB samples one after another:
// fills `after` with how long after each send ended its sample was whole, and `interval` with the reader's retry
// interval once it was.
static void busy_writer_run(int samples, uint64_t* after, uint64_t* interval) {
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy == NULL) {
        return;
    }
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    sample_len = FOUR_MB;
    sim_loss_per_mille = 50;
    sim_room_per_step = 8;
    sim_busy_writer = true;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        after[i] = sim_settle_after_send(i + 2, 2 * tt_SECOND);
        interval[i] = reliable_retry_interval(&receiver, proxy);
    }
    EXPECT_EQ_INT(samples + 1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
}

static void test_a_busy_writers_send_is_not_timed_as_the_path(void) {
    // A writer busy sending a large sample reads nothing until the send ends, so a gap the reader asks for during it is
    // answered only then: request to repair spans the rest of the send. Such a recovery says how long the writer was
    // busy, not how long the path takes, and folded into the estimate it lengthened the retry interval with each
    // sample - 8 x 4 MB at 5% loss: from 43 to 61 ms, and every sample after the first whole ~15 ms after its send
    // ended against 0.95 ms for the first (2026-10-16). Karn's rule in spirit: a recovery whose wait saw the writer's
    // large sample still arriving is not timed. Mutants: the busy recovery timed (the interval climbs and the samples
    // are late again); the busy writer asked again at the plain interval (the wire bound below).
    enum { SAMPLES = 8 };
    uint64_t after[SAMPLES] = {0};
    uint64_t interval[SAMPLES] = {0};
    busy_writer_run(SAMPLES, after, interval);
    uint64_t slowest = 0;
    for (int i = 0; i < SAMPLES; i++) {
        printf("test_large_sample: busy writer, 4 MB sample %d whole %.2f ms after its send ended, retry interval "
               "%.2f ms\n",
               i + 1, (double)after[i] / 1e6, (double)interval[i] / 1e6);
        slowest = i > 0 && after[i] > slowest ? after[i] : slowest;
    }
    // The first sample within four intervals, the rest within two. Its last lost repair, with nothing sent after it to
    // show it lost, waits the writer's repair timeout (the resend order, DESIGN.md section 4): twice the srtt of its
    // repairs, which the reader's untimed 1 ms retries set - 2.95 ms against the parent's 1.95 (2026-10-23).
    EXPECT_TRUE(after[0] <= 4 * TEST_RETRY_INTERVAL);
    printf("test_large_sample: busy writer, %d x 4 MB - datagrams sent by the writer %llu (%llu lost), by the reader "
           "%llu\n",
           SAMPLES, (unsigned long long)sim_sent[SENDER_ID], (unsigned long long)sim_dropped[SENDER_ID],
           (unsigned long long)sim_sent[RECEIVER_ID]);
    EXPECT_TRUE(slowest <= 2 * TEST_RETRY_INTERVAL);
    EXPECT_TRUE(interval[SAMPLES - 1] <= 2 * TEST_RETRY_INTERVAL);
    // Nor is the busy writer asked again while it sends: at the path's interval its queued requests named each lost
    // datagram over and over, and it resent each for each - 91,786 datagrams against 26,226 with the inflated
    // interval and 25,503 now.
    EXPECT_TRUE(sim_sent[SENDER_ID] * 100 <= (uint64_t)SAMPLES * tt_sample_datagrams(FOUR_MB) * 120);
}

// The reader's recovery estimate after `samples` 1 MB samples at 5% loss over a path whose reader-to-writer leg takes
// `delay`, each sent whole at once - no sample still arriving while a repair is awaited - continuing the pair as it is.
static uint32_t quiet_path_srtt(int first, int samples, uint64_t delay) {
    sim_reverse_delay = delay;
    for (int i = first; i < first + samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
        sim_settle(i + 1, 2 * tt_SECOND);
    }
    EXPECT_EQ_INT(first + samples, delivered_count);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    return proxy != NULL ? proxy->recovery_srtt_ns : 0;
}

static void test_a_quiet_paths_round_trip_is_still_timed(void) {
    // The control of test_a_busy_writers_send_is_not_timed_as_the_path: the same large samples and loss, the writer
    // never busy while a repair is awaited, and a path whose round trip changes from 2 to 8 ms part-way. The estimate
    // learns the first and then follows the change - what leaving the busy writer's recoveries out must not stop. It
    // starts after one busy 4 MB send, so a recovery left out then does not leave the next ones out too. Mutants: no
    // recovery timed at all (srtt stays 0); the busy mark kept past its probe; originals that came with the datagram
    // revealing the gap taken for a send still going on.
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy == NULL) {
        return;
    }
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    sample_len = FOUR_MB;
    sim_loss_per_mille = 50;
    sim_room_per_step = 8;
    sim_busy_writer = true;
    // On the 2 ms path already: on the zero-delay link a repair comes back in the instant it is asked for, srtt falls
    // to ~0, and a path then 2 ms long is never learned - each recovery takes more than the plain retries, and a
    // backed-off one is not timed (a limit of the estimator itself, on the parent as well).
    sim_reverse_delay = 2 * tt_MILLISECOND;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2, tt_SECOND));
    sim_settle(2, 2 * tt_SECOND);
    EXPECT_EQ_INT(2, delivered_count);
    sim_busy_writer = false;
    sim_room_per_step = 0;
    test_mock_nonblocking_room = -1; // each sample goes out whole at once
    sample_len = ONE_MB;
    const int samples = 6;
    uint32_t short_path = quiet_path_srtt(2, samples, 2 * tt_MILLISECOND);
    uint32_t long_path = quiet_path_srtt(2 + samples, samples, 8 * tt_MILLISECOND);
    printf("test_large_sample: quiet path at 5%% loss - recovery srtt %.2f ms over a 2 ms path, %.2f ms after it "
           "became 8 ms\n",
           (double)short_path / 1e6, (double)long_path / 1e6);
    // Within a quarter of the path below (a probe started when the watermark reaches a gap asked for earlier times a
    // repair already on its way) and half above it. The parent: 4.97 ms over the 2 ms path - the busy send's 18 ms
    // recoveries still in it - and 7.39 ms over the 8 ms one; this change 1.80 and 6.37 (2026-10-16).
    EXPECT_TRUE(short_path >= 1500 * tt_MICROSECOND && short_path < 3 * tt_MILLISECOND);
    EXPECT_TRUE(long_path >= 6 * tt_MILLISECOND && long_path < 12 * tt_MILLISECOND);
}

// One 1 MB sample, sent whole at once over a 2 ms path, losing units 100 and 200, and the first repair of 100 too:
// the request for the watermark (100) is answered by 200's repair alone, and 100 comes with the retry. Answers the
// reader's recovery estimate after it, and fills `copies` with the duplicates the reader got.
static uint32_t srtt_after_a_repair_overtook_the_watermark(uint64_t* copies) {
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy == NULL) {
        return 0;
    }
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    EXPECT_EQ_U32(0, proxy->recovery_srtt_ns); // nothing timed yet: the sample's is the first recovery
    sim_reverse_delay = 2 * tt_MILLISECOND;
    test_mock_nonblocking_room = -1; // the sample goes out whole at once: no original arrives while a repair is awaited
    sample_len = ONE_MB;
    sim_unit_drops[0].index = 100;
    sim_unit_drops[0].copies = 2;
    sim_unit_drops[1].index = 200;
    sim_unit_drops[1].copies = 1;
    uint64_t duplicates = receiver.large.duplicate;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2, tt_SECOND));
    sim_settle(2, 2 * tt_SECOND);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_U32(0, sim_unit_drops[0].copies); // the treatment applied: both copies of 100 lost
    EXPECT_EQ_U32(0, sim_unit_drops[1].copies);
    *copies = receiver.large.duplicate - duplicates;
    return proxy->recovery_srtt_ns;
}

static void test_a_repair_arriving_first_is_not_a_writer_still_sending(void) {
    // Only an original arriving while a repair is awaited says the writer was still sending (tt_WriterProxy.
    // probe_busy); another unit's repair says the writer answered. Here the watermark's own repair is lost and the
    // other unit's arrives first: the writer is quiet, and the recovery - one retry, within the plain ones - is timed
    // into the path's estimate. test_a_quiet_paths_round_trip_is_still_timed() meets this case in 2 or 3 of its 27
    // recoveries, too few to move its estimate out of its bounds (2026-10-26); here it is the one recovery. Mutant
    // (mutants_large_sample.py, "a repair marks the probe busy"): the recovery is not timed and the estimate stays 0.
    uint64_t copies = 0;
    uint32_t srtt = srtt_after_a_repair_overtook_the_watermark(&copies);
    printf("test_large_sample: a repair ahead of the watermark's lost one - recovery srtt %.2f ms over a 2 ms path, "
           "%llu duplicates\n",
           (double)srtt / 1e6, (unsigned long long)copies);
    EXPECT_TRUE(srtt >= 2 * tt_MILLISECOND); // a request, a lost repair, a retry and its repair: at least the path
}

// srtt_after_a_repair_overtook_the_watermark()'s sample on a WIFI reader (repair_timing, and repair_reveal `mode`)
// whose retry interval is long - a path estimate of 60 ms, as a reader's grows behind a queued Wi-Fi path (RESULTS
// `R4K`: 37 ms at the p50) - over a path of 10 ms each way. Units 100 and 200 lost, and the first repair of 100 too.
// Answers the sample's latency and fills `copies` with the duplicates the reader got.
static uint64_t latency_after_a_repair_overtook_the_watermark(uint8_t mode, uint64_t* copies) {
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    receiver.repair_timing = true;
    receiver.repair_reveal = mode;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy == NULL) {
        return 0;
    }
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    proxy->recovery_srtt_ns = 60 * tt_MILLISECOND;
    proxy->recovery_rttvar_ns = 5 * tt_MILLISECOND;
    // And a repair transit of 30 ms (a queued path's, as the retry estimate): the reader's answers to the writer's
    // HEARTBEAT chase leave a unit out for 50 ms after it was asked for, as on its way (repair_in_flight_ns()).
    proxy->transit_srtt_ns = 30 * tt_MILLISECOND;
    proxy->transit_rttvar_ns = 5 * tt_MILLISECOND;
    proxy->transit_min_ns = 20 * tt_MILLISECOND;
    sim_reverse_delay = 10 * tt_MILLISECOND;
    sim_unicast_delay = 10 * tt_MILLISECOND;
    test_mock_nonblocking_room = -1; // the sample goes out whole at once
    sample_len = ONE_MB;
    sim_unit_drops[0].index = 100;
    sim_unit_drops[0].copies = 2;
    sim_unit_drops[1].index = 200;
    sim_unit_drops[1].copies = 1;
    uint64_t duplicates = receiver.large.duplicate;
    uint64_t published = test_mock_now;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2, tt_SECOND));
    sim_settle(2, 2 * tt_SECOND);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_U32(0, sim_unit_drops[0].copies); // the treatment applied: both copies of 100 lost
    EXPECT_EQ_U32(0, sim_unit_drops[1].copies);
    *copies = receiver.large.duplicate - duplicates;
    return delivered_count == 2 ? delivered_at[1] - published : 0;
}

static void test_a_repair_shown_lost_by_a_later_one_is_asked_for_at_once(void) {
    // 2026-10-30 (C11, maybe_reveal_lost_repairs()): 200's repair, sent after 100's, arrives and 100 is still missing,
    // so 100's repair is lost on a path that keeps order; with repair_reveal REVEALED or ALL_MISSING the reader asks
    // again then, and the writer, shown that a later resend got through, resends 100 at once: two round trips of 20 ms
    // (the requests for 100 and 200 answered together, then 200's repair revealing 100's lost) - 40 ms. OFF (the
    // default) waits out the transit window its answers to the writer's HEARTBEAT chase leave 100 out for, or its 60 ms
    // estimate: 83 ms, the parent's (2026-10-30). Mutant (mutants_large_sample.py, "no reveal"): over 50 ms.
    static const uint8_t modes[] = {tt_REPAIR_REVEAL_OFF, tt_REPAIR_REVEAL_REVEALED, tt_REPAIR_REVEAL_ALL_MISSING};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        uint64_t copies = 0;
        uint64_t took = latency_after_a_repair_overtook_the_watermark(modes[i], &copies);
        printf(
            "test_large_sample: a WIFI reader (repair_reveal %u) whose repair of the watermark was lost, a later one "
            "arriving - sample in %.2f ms over a 10 ms path (60 ms retry estimate), %llu duplicates\n",
            (unsigned)modes[i], (double)took / 1e6, (unsigned long long)copies);
        if (modes[i] == tt_REPAIR_REVEAL_OFF) {
            EXPECT_TRUE(took > 80 * tt_MILLISECOND);
        } else {
            EXPECT_TRUE(took > 0 && took <= 50 * tt_MILLISECOND);
        }
        EXPECT_EQ_U64(0, copies); // asking sooner sent nothing twice: the writer resent 100 once more, as it had to
    }
}

// The state test_a_reveal_names_only_what_it_reveals() reads, a reveal at B+5 in `mode`: answers the request it sent
// (proxy->requests[2]), or NULL if it sent none.
static const struct tt_RepairRequest* reveal_at_b5(uint8_t mode, uint32_t* base_out) {
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy == NULL) {
        return NULL;
    }
    receiver.repair_timing = true;
    receiver.repair_reveal = mode;
    test_mock_now += tt_SECOND;
    uint64_t now = test_mock_now;
    uint32_t base = proxy->ack_seq_no;
    memset(proxy->received_bitmap, 0, (size_t)proxy_words(proxy) * sizeof(uint64_t));
    proxy->received_bitmap[0] = 0x3FEULL & ~((1ULL << 3) | (1ULL << 7)); // 1..9 received but 3 and 7; 0 missing
    proxy->received_words = 1; // the words in use, as proxy_*_received() keep it
    proxy->heartbeat_last_seq_no = base - 1U;
    proxy->large_arrival_ns = now;
    proxy->transit_min_ns = (uint32_t)tt_MILLISECOND;
    proxy->transit_srtt_ns = (uint32_t)(20 * tt_MILLISECOND);
    proxy->transit_rttvar_ns = (uint32_t)tt_MILLISECOND;
    proxy->reveal_ns = 0;
    proxy->acknack_scheduled = true;
    memset(proxy->requests, 0, sizeof(proxy->requests));
    proxy->requests[0] = (struct tt_RepairRequest) {now - (5 * tt_MILLISECOND), base, base + 5U};
    proxy->requests[1] = (struct tt_RepairRequest) {now - (2 * tt_MILLISECOND), base + 7U, base + 7U};
    proxy->request_next = 2;
    uint32_t queued = sim_tail;
    sim_acting = RECEIVER_ID;
    maybe_reveal_lost_repairs(&receiver, proxy, base + 5U);
    bool sent = sim_tail == queued + 1U; // one ACKNACK went
    sim_head = sim_tail;                 // nothing of it is delivered
    proxy->acknack_scheduled = false;
    *base_out = base;
    // What it named, as the reader remembers its own requests (note_requested(): the first and last unit named).
    return sent ? &proxy->requests[2] : NULL;
}

static void test_a_reveal_names_only_what_it_reveals(void) {
    // O2 (C11, maybe_reveal_lost_repairs()): the watermark B and B+3 were asked for 5 ms ago together with B+5, whose
    // repair just arrived; B+7 was asked for 2 ms ago, inside the 24 ms repair transit window. B+5's repair went after
    // any repair of B or B+3 - they are lost - but B+7's may still be on its way. REVEALED names B and B+3 and leaves
    // B+7 out; ALL_MISSING (the prototype O) names B+7 too, and on the PC's queued Wi-Fi path the writer's RACK time
    // rule then resent such repairs early: duplicates 909-2,474 a rep against 171-446. Mutants
    // (mutants_large_sample.py, "reveal names everything", "ALL_MISSING named as REVEALED").
    static const uint8_t modes[] = {tt_REPAIR_REVEAL_REVEALED, tt_REPAIR_REVEAL_ALL_MISSING};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        uint32_t base = 0;
        const struct tt_RepairRequest* named = reveal_at_b5(modes[i], &base);
        EXPECT_TRUE(named != NULL);
        if (named == NULL) {
            continue;
        }
        printf("test_large_sample: a reveal (mode %u) at B+5 named B+%u..B+%u\n", (unsigned)modes[i],
               named->first_seq_no - base, named->last_seq_no - base);
        EXPECT_EQ_U64(test_mock_now, named->sent_ns);
        EXPECT_EQ_U32(base, named->first_seq_no);
        EXPECT_EQ_U32(base + (modes[i] == tt_REPAIR_REVEAL_REVEALED ? 3U : 7U), named->last_seq_no);
    }
}

static void test_reveals_are_paced_by_the_least_transit(void) {
    // The reveal asks again on a repair's arrival, and repairs arrive in runs: one ACKNACK each would be one per
    // repair. At most one per least repair transit (nothing it caused can have come back sooner). 4 x 1 MiB at 10% loss
    // over a 10 ms path each way, a WIFI reader: every sample delivered, and the reader's datagrams - its ACKNACKs -
    // counted. Mutant (mutants_large_sample.py, "reveal unpaced"): one ACKNACK per revealing repair, over the bound.
    // Unicast originals, so they share the repairs' 10 ms and the path keeps the writer's order, as one netem band
    // does.
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    receiver.repair_timing = true;
    receiver.repair_reveal = tt_REPAIR_REVEAL_ALL_MISSING; // the mode that names the most
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy == NULL) {
        return;
    }
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    sim_reverse_delay = 10 * tt_MILLISECOND;
    sim_unicast_delay = 10 * tt_MILLISECOND;
    test_mock_nonblocking_room = -1;
    sample_len = ONE_MB;
    sim_loss_per_mille = 100;
    uint64_t reader_sent = sim_sent[RECEIVER_ID];
    uint32_t resent = pub.retransmitted;
    enum { PACED_SAMPLES = 4 };
    for (int i = 0; i < PACED_SAMPLES; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        sim_settle(i + 2, 2 * tt_SECOND);
    }
    EXPECT_EQ_INT(PACED_SAMPLES + 1, delivered_count);
    uint64_t acks = sim_sent[RECEIVER_ID] - reader_sent;
    printf("test_large_sample: %d x 1 MiB at 10%% loss over a 10 ms path, WIFI reader - %llu reader datagrams, %u "
           "resent, %llu duplicates\n",
           PACED_SAMPLES, (unsigned long long)acks, pub.retransmitted - resent,
           (unsigned long long)receiver.large.duplicate);
    EXPECT_TRUE(acks < 400);
}

// A writer that reads requests while it sends - repairs queued behind the rest of its sample - on a path of `delay`:
// `samples` 4 MB samples at 5% loss through 8 datagrams of send buffer. Answers the copies the reader got twice.
static uint64_t answering_writer_duplicates(int samples, uint64_t delay) {
    init_pair(1000, RELIABLE_KEEP_LAST);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL);
    if (proxy == NULL) {
        return 0;
    }
    proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    sim_reverse_delay = delay;
    sample_len = FOUR_MB;
    sim_loss_per_mille = 50;
    sim_room_per_step = 8;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        sim_settle(i + 2, 2 * tt_SECOND);
    }
    EXPECT_EQ_INT(samples + 1, delivered_count);
    return receiver.large.duplicate;
}

// answering_writer_duplicates() with a second reader on a node of its own, each original lost to both or to neither:
// every unit one reader asks for, the other asks for too, and the writer's one repair slot for it holds the resend to
// whichever asked last. Answers the copies both readers got twice; *resent the writer's resends.
static uint64_t two_readers_duplicates(int samples, uint64_t delay, bool shared, uint32_t seed, uint32_t* resent) {
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_SECOND_READER, NULL);
    sim_start(0);
    sim_rng = seed;
    sim_loss_first_reader_only = !shared;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxies[2] = {find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id),
                                         find_writer_proxy(&sub_b, SENDER_ID, pub.endpoint.entity_id)};
    for (int i = 0; i < 2; i++) {
        EXPECT_TRUE(proxies[i] != NULL);
        if (proxies[i] == NULL) {
            return 0;
        }
        proxies[i]->keep_all = tt_WRITER_KEEP_ALL_NO;
    }
    sim_reverse_delay = delay;
    sample_len = FOUR_MB;
    sim_loss_per_mille = 50;
    sim_room_per_step = 8;
    uint32_t before = pub.retransmitted;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        sim_settle(i + 2, 2 * tt_SECOND);
    }
    EXPECT_EQ_INT(samples + 1, delivered_count);
    EXPECT_EQ_INT(samples + 1, delivered_b);
    *resent = pub.retransmitted - before;
    return receiver.large.duplicate + reader_b.large.duplicate;
}

static void test_two_readers_are_not_resent_repairs_on_their_way(void) {
    // Since the resend order (DESIGN.md section 4) the writer holds a unit whose resend is on its way, so with one
    // reader a retry naming it costs nothing (test_a_sending_writer_is_not_asked_for_repairs_on_their_way(): 3
    // duplicates with the reader's skip, 2 without). The hold lives in one repair slot a seq_no, which names one
    // reader: two readers on two nodes that lost the same unit take it from each other, and each one's next request for
    // it finds the slot the other's and is answered at once. So here the reader's own skip - a retry while the writer
    // sends names only what was asked for longer ago than the repair transit time (repairs_in_flight()) - is what keeps
    // the copies out. Read 2026-10-26, seeds 7 13 23: 35 duplicates with the skip (2 9 24), 523 without it (139 183
    // 201). Control: the same readers with every original lost to the first alone, which shares no slot - the second
    // reader's own requests are the treatment, so the shared runs resend about twice the control's (1,297 against
    // 628 at seed 7). Mutant (mutants_large_sample.py, "(srtt) a retry while the writer sends names repairs in
    // flight"): 0.5% of the samples' datagrams is the bound.
    enum { SAMPLES = 4 };
    static const uint32_t seeds[] = {7, 13, 23};
    uint64_t total = 0;
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        uint32_t control = 0;
        (void)two_readers_duplicates(SAMPLES, 2 * tt_MILLISECOND, false, seeds[i], &control);
        uint32_t resent = 0;
        uint64_t duplicates = two_readers_duplicates(SAMPLES, 2 * tt_MILLISECOND, true, seeds[i], &resent);
        printf("test_large_sample: answering writer, two readers, seed %u, %d x 4 MB at 5%% over a 2 ms path - lost to "
               "the first only: resent %u; lost to both: resent %u, duplicates at the readers %llu (%llu + %llu), "
               "requests held %llu\n",
               (unsigned)seeds[i], SAMPLES, control, resent, (unsigned long long)duplicates,
               (unsigned long long)receiver.large.duplicate, (unsigned long long)reader_b.large.duplicate,
               HELD_REQUESTS(sender));
        EXPECT_TRUE((uint64_t)resent * 2 >= (uint64_t)control * 3); // the treatment: both readers asked for repairs
        total += duplicates;
    }
    EXPECT_TRUE(total * 200 <=
                (uint64_t)SAMPLES * tt_sample_datagrams(FOUR_MB) * 3); // 174: 35 with the skip, 523 without
}

static void test_a_sending_writer_is_not_asked_for_repairs_on_their_way(void) {
    // The other writer: it reads requests while it sends, so its repairs come - queued behind the rest of the sample -
    // and the reader's retry interval, its busy recoveries left out, is far shorter than that queue. Asked again at
    // that interval it resent repairs already on their way: on the PC (srtt_busy_writer_pc_ab.sh, 2026-10-16) +37%
    // writer datagrams at 0% netem; here 3,907 copies the reader got twice. While the sample still arrives a retry
    // names only what was asked for longer ago than the repair transit time, and asks no oftener than the interval
    // over every recovery (sending_srtt_ns): 6. The parent, its one interval inflated by the queue: 245. Mutants: the
    // retry names repairs in flight; it asks at the path's interval.
    enum { SAMPLES = 4 };
    uint64_t duplicates = answering_writer_duplicates(SAMPLES, 2 * tt_MILLISECOND);
    printf("test_large_sample: answering writer, %d x 4 MB at 5%% over a 2 ms path - writer datagrams %llu, "
           "duplicates at the reader %llu\n",
           SAMPLES, (unsigned long long)sim_sent[SENDER_ID], (unsigned long long)duplicates);
    EXPECT_TRUE(duplicates * 400 <= (uint64_t)SAMPLES * tt_sample_datagrams(FOUR_MB)); // 0.25%: 28; no transit rule: 80
}

// `samples` 1 MiB samples at 0% loss from a writer whose one reader it unicasts to over a path that holds its unicast
// datagrams 40 ms - a 1 MiB sample draining through a ~200 Mbit/s link - and its broadcast ones not at all; with
// `summary_batched`, a discovery summary is batched in tx_buffer, waiting for its flush tick, just after each sample
// went out (send_discovery_summary()), as the polling thread leaves it when the summary's timer comes first - here
// before the sample's first ack chase (large_ack_chase()). Answers the datagrams the writer resent.
static uint32_t resent_on_a_split_path(int samples, bool summary_batched) {
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_unicast_delay = 40 * tt_MILLISECOND;
    sample_len = ONE_MB;
    test_mock_nonblocking_room = -1; // each sample goes out whole at once, into the path's queue
    uint32_t before = pub.retransmitted;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        if (summary_batched) {
            send_discovery_summary(&sender); // batched after the sample went out, before its first ack chase
        }
        sim_settle(i + 2, 2 * tt_SECOND);
    }
    EXPECT_EQ_INT(samples + 1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    return pub.retransmitted - before;
}

static void test_a_heartbeat_takes_the_path_of_its_data(void) {
    // A RELIABLE writer's HEARTBEAT announcing what it unicast must go to the same peers. Sent from a tx_buffer that
    // held a batched discovery summary - broadcast-only content - it went broadcast with it (unicast only from an empty
    // buffer), took a path the data's queue was not on, and reached the reader while the sample was still draining:
    // the reader asked for every datagram not yet arrived, the writer resent them, and the reader asked again each
    // retry interval while the originals drained. The 1 MiB control of the WB netns baseline (RESULTS.md) resent 4,995
    // to 127,131 units a 30 s rep with nothing lost; traced, one broadcast HEARTBEAT, one revealed gap, 7,404 resends
    // (2026-10-23). The batched content now goes first, as its flush tick would send it, and the HEARTBEAT to the
    // peers. Control: the same path with no summary waiting resends nothing - the summary's company, not the delay,
    // sent the HEARTBEAT ahead. Mutant (mutants_large_sample.py): the batched content not flushed first.
    enum { SAMPLES = 3 };
    uint32_t control = resent_on_a_split_path(SAMPLES, false);
    uint32_t batched = resent_on_a_split_path(SAMPLES, true);
    printf("test_large_sample: %d x 1 MiB at 0%% loss, unicast path 40 ms, broadcast 0 - resent %u without a batched "
           "summary, %u with one\n",
           SAMPLES, control, batched);
    EXPECT_EQ_U32(0, control);
    EXPECT_EQ_U32(0, batched);
}

// `samples` 1 MiB samples at 30 Hz over a lossless path to a reader that reads a datagram every 20 us (~73 MB/s, faster
// than the 31 MB/s offered, slower than a sample's burst) with room for 400 waiting in its socket: each sample's burst
// overflows the socket and the reader loses its tail there, as a CPU-starved reader did on the PC (the LRF netns runs
// under other sessions' load, 2026-10-23: socket drops 222-6,658 a rep, resends 8,179-532,481, duplicates up to 97%).
// Answers the writer's resends, the socket drops and the reader's duplicate datagrams.
// `service_ns` a datagram, `capacity` waiting, `loss_per_mille` on the path after the queue: with no loss it is the
// slow reader above; with loss and a deep queue it is a rate-limited path (12 us a datagram ~ 1 Gbit/s at the MTU).
// `seed` seeds the path's loss.
// The clock starts at `queue_start_ns` (0 unless a caller sets it).
static uint64_t queue_start_ns;
static void resent_through_a_queue(int samples, uint64_t service_ns, uint32_t capacity, uint32_t loss_per_mille,
                                   uint32_t seed, uint64_t* resent, uint64_t* drops, uint64_t* duplicates) {
    EXPECT_TRUE(capacity <= SIM_HELD);
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    test_mock_now = queue_start_ns;
    sim_rng = seed;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_reader_service_ns = service_ns;
    sim_reader_capacity = capacity;
    sim_loss_per_mille = loss_per_mille;
    sample_len = ONE_MB;
    test_mock_nonblocking_room = -1; // each sample goes out whole at once
    uint32_t before = pub.retransmitted;
    uint64_t duplicates_before = receiver.large.duplicate;
    uint64_t t0 = test_mock_now;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        uint64_t next = t0 + ((uint64_t)(i + 1) * tt_SECOND / 30U);
        while (test_mock_now < next && sim_step()) {
        }
        if (test_mock_now < next) {
            test_mock_now = next;
        }
    }
    sim_settle(samples + 1, 2 * tt_SECOND);
    *resent = pub.retransmitted - before;
    *drops = sim_socket_drops;
    *duplicates = receiver.large.duplicate - duplicates_before;
}

static void test_a_slow_readers_repairs_are_measured(void) {
    // Loss at the reader, not on the path: what the large-sample repair amplifies when the first retry is 1 ms (nothing
    // timed yet) and the writer resends a unit each time it is named. Registered before the first run (2026-10-23, the
    // follow-ups of the reliable HEARTBEAT fix, item (a), with large_repair_followups_netns.sh's slow phase): REAL when
    // the writer resends more than 1.25 per unit dropped in the socket, or the reader's duplicate datagrams exceed 20%
    // of the resends; the ideal resends each dropped unit once, the path losing nothing.
    // Read on the parent (RESULTS `LRF`): NOT REAL as registered - 1.01 resends per drop, duplicates 1.0% of resends.
    // But the criterion cannot decide here: with every unit resent twice (a mutant) it read 1.09 and 8.4%, because a
    // resend into a full socket is dropped again and counted as loss. So it is a measurement, not a guard; the
    // per-unit guard is test_a_sending_writer_is_not_asked_for_repairs_on_their_way() (path loss: duplicates bounded),
    // which kills that mutant. What the reader pays is something else: 564,402 resends for 10 samples of 723
    // datagrams, nearly all dropped again. Each of its ~200 ACKNACKs names up to thousands of units and the writer
    // resends them in one burst, which overflows the socket it was asked to fill; a later first retry (5 or 20 ms,
    // explored after the reading) changed that little. Repair flow control against the reader's capacity - no item of
    // this change, a lead (ROADMAP). With the resend order (DESIGN.md section 4) a unit whose resend is on its way is
    // not resent again: 36,999 resends (2026-10-23), still 5 per sample datagram - what is left is the bursts.
    enum { SAMPLES = 10 };
    uint64_t resent = 0;
    uint64_t drops = 0;
    uint64_t duplicates = 0;
    resent_through_a_queue(SAMPLES, 20 * tt_MICROSECOND, 400, 0, 7, &resent, &drops, &duplicates);
    printf("test_large_sample: %d x 1 MiB at 30 Hz to a slow reader (20 us a datagram, 400 waiting) - %llu dropped in "
           "its socket, %llu resent (%.2f per drop), %llu duplicates at the reader, %d delivered\n",
           SAMPLES, (unsigned long long)drops, (unsigned long long)resent,
           drops != 0 ? (double)resent / (double)drops : 0.0, (unsigned long long)duplicates, delivered_count);
    EXPECT_TRUE(drops > 0); // the treatment applied
    EXPECT_EQ_INT(SAMPLES + 1, delivered_count);
}

// The slow reader of test_a_slow_readers_repairs_are_measured, its clock past the warning's first rate limit. Answers
// the units the writer resent.
static uint64_t repairing_writer_resent;
static void repairing_writer_scenario(void) {
    enum { SAMPLES = 10 };
    uint64_t drops = 0;
    uint64_t duplicates = 0;
    queue_start_ns = 10 * tt_SECOND; // tt_WriterProxy.stuck_warned_ns starts at 0: a warning may go at once
    resent_through_a_queue(SAMPLES, 20 * tt_MICROSECOND, 400, 0, 7, &repairing_writer_resent, &drops, &duplicates);
    queue_start_ns = 0;
    EXPECT_TRUE(drops > 0);
    EXPECT_EQ_INT(SAMPLES + 1, delivered_count);
}

static void test_repairs_passed_by_an_acknack_naming_nothing_are_freed(void) {
    // A reader's last ACKNACK of a sample names nothing - it has every unit - and process_acknack() answers it without
    // the resend order, which frees a pending repair's slot only when an ACKNACK it reads passes the unit. The next
    // ACKNACK it reads starts from that last watermark, so the repairs between were never freed: their slots stayed
    // live until another unit took them, and repairs_live, whose 0 is the order's "nothing pending" fast path, did not
    // return to 0 between samples (C7, found with the slow reader of resent_through_a_queue(): 9 slots of every
    // sample). Here: three 1 MiB samples at a true 30 Hz to a reader taking 20 us a datagram with 400 waiting, each
    // sample's end dropped at its socket and repaired; once all are delivered, no slot may still hold a unit of the
    // first two. Control: their units were resent at all.
    enum { SAMPLES = 3 };
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_reader_service_ns = 20 * tt_MICROSECOND;
    sim_reader_capacity = 400;
    sample_len = ONE_MB;
    test_mock_nonblocking_room = -1; // each sample goes out whole at once
    uint32_t before = pub.retransmitted;
    uint32_t last_first = 0;
    uint64_t t0 = test_mock_now;
    for (int i = 0; i < SAMPLES; i++) {
        last_first = pub.seq_no + 1U;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        uint64_t next = t0 + ((uint64_t)(i + 1) * tt_SECOND / 30U);
        while (sim_next_due() <= next && sim_step()) { // the clock never passes the next publish
        }
        test_mock_now = test_mock_now < next ? next : test_mock_now;
    }
    sim_settle(SAMPLES + 1, tt_SECOND);
    uint32_t stale = 0;
    for (uint32_t i = 0; i < tt_LARGE_REPAIR_SLOTS; i++) {
        const struct tt_LargeRepair* slot = &sender.large.repairs[i];
        stale += slot->context_id != tt_CONTEXT_ID_INVALID && seq_delta(slot->seq_no, last_first) < 0 ? 1U : 0U;
    }
    printf(
        "test_large_sample: %d x 1 MiB to a reader taking 20 us a datagram, 400 waiting - %u resent, %u repair slots "
        "of the delivered samples before the last still live, %u live in all\n",
        SAMPLES, pub.retransmitted - before, stale, sender.large.repairs_live);
    EXPECT_EQ_INT(SAMPLES + 1, delivered_count);
    EXPECT_TRUE(pub.retransmitted - before >= (uint32_t)SAMPLES); // the treatment: every sample was repaired
    EXPECT_EQ_U32(0, stale);
}

static void test_a_repairing_writer_is_not_warned_about(void) {
    // A writer whose repairs of a large sample keep arriving is answering, though the gap stays open: the slow reader's
    // socket drops most of each repair burst again, and the gap takes many retry intervals to close. A retry within
    // the interval of a large-sample arrival - original or repair - is not counted against the writer; counted, the
    // reader's retries ran to 11 here, past tt_RELIABLE_RETRY, and it warned that the writer "neither repairs it nor
    // says it is gone" while it was doing the first (mutants_large_sample.py, "a retry while a large sample still
    // arrives counts against the writer", 2026-10-26). test_a_busy_writer_is_not_warned_about cannot show this since
    // 9d716d60: a writer busy in its send answers nothing, and a retry to one that has answered nothing within the
    // interval is not sent at all, so none of its retries reaches the count. Controls: the writer did resend, and a
    // writer really gone silent is warned about on the same clock.
    int repairing = stuck_warnings_in(repairing_writer_scenario);
    int silent = stuck_warnings_in(silent_writer_scenario);
    printf("test_large_sample: \"still waiting\" warnings - repairing writer %d (%llu resent), silent writer %d\n",
           repairing, (unsigned long long)repairing_writer_resent, silent);
    EXPECT_TRUE(repairing_writer_resent > 0);
    EXPECT_EQ_INT(0, repairing);
    EXPECT_TRUE(silent >= 1);
}

// `samples` 1 MiB samples at 30 Hz through the queued path of resent_through_a_queue() (`service_ns` a datagram,
// `capacity` waiting), each sample's tail lost once (so the repairs the timeout is timed on are a few units behind an
// empty queue), and every `every`-th sample's first send lost whole. Answers the units resent; *lost the whole-sample
// losses, *delivered the samples delivered within `settle` after the last.
static uint64_t resent_after_whole_losses(int samples, uint32_t every, uint64_t service_ns, uint32_t capacity,
                                          uint32_t loss_per_mille, uint64_t settle, uint32_t* lost, int* delivered) {
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_reader_service_ns = service_ns;
    sim_reader_capacity = capacity;
    sim_loss_per_mille = loss_per_mille;
    sim_drop_tails = true;
    sim_whole_loss_every = every;
    sample_len = ONE_MB;
    test_mock_nonblocking_room = -1; // each sample goes out whole at once
    uint32_t before = pub.retransmitted;
    uint64_t t0 = test_mock_now;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        uint64_t next = t0 + ((uint64_t)(i + 1) * tt_SECOND / 30U);
        while (test_mock_now < next && sim_step()) {
        }
        if (test_mock_now < next) {
            test_mock_now = next;
        }
    }
    sim_settle(samples + 1, settle);
    *lost = sim_whole_lost;
    *delivered = delivered_count;
    return pub.retransmitted - before;
}

static void test_a_queued_path_does_not_storm(void) {
    // A rate-limited path (12 us a datagram, 4,000 queued) at 1% loss after it: item (a)'s storm (RESULTS `LRF`; the
    // WB-harness netns path at 1% stormed one rep in three). The sim's clock jumps to its next timer after the first
    // sample, so the next several go back to back - 4,344 datagrams into a queue of 4,000, the burst a queued path
    // meets. Each ACKNACK then named the units whose repairs still queued behind the drain, and the writer resent every
    // one each time it was named: the repairs became the queue, and its drops more repairs.
    // The criterion (DESIGN.md section 4, "Large-sample resend order", its falsifier): STORM when the writer resends
    // more datagrams than its samples carry (30 x 724). The parent, seeds 7 13 23: 259,042, 29,331 and 27,024 resent -
    // a storm at every seed. The resend order: 9,541, 8,235 and 6,049 (2026-10-24). Mutants (mutants_large_sample.py,
    // "resend order"): every request answered at once; no seed for the repair timeout before the first timing.
    enum { QUEUED_SAMPLES = 30 };
    static const uint32_t seeds[] = {7, 13, 23};
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        uint64_t resent = 0;
        uint64_t drops = 0;
        uint64_t duplicates = 0;
        resent_through_a_queue(QUEUED_SAMPLES, 12 * tt_MICROSECOND, 4000, 10, seeds[i], &resent, &drops, &duplicates);
        printf("test_large_sample: seed %u, %d x 1 MiB at 30 Hz through a 1 Gbit/s-like queue of 4000 at 1%% loss - "
               "%llu lost on the path, %llu dropped at the queue, %llu resent, %llu duplicates at the reader, %d "
               "delivered, %llu requests held\n",
               (unsigned)seeds[i], QUEUED_SAMPLES, (unsigned long long)sim_dropped[SENDER_ID],
               (unsigned long long)drops, (unsigned long long)resent, (unsigned long long)duplicates, delivered_count,
               HELD_REQUESTS(sender));
        EXPECT_TRUE(drops > 0); // the treatment applied: the burst met a full queue
        EXPECT_EQ_INT(QUEUED_SAMPLES + 1, delivered_count);
        EXPECT_TRUE(resent < (uint64_t)QUEUED_SAMPLES * tt_sample_datagrams(ONE_MB));
    }
}

// --- The resend order (DESIGN.md section 4, "Large-sample resend order") ---------------------------------------------

// `samples` 1 MiB samples, each losing its last fragment once, over a path that holds the writer's unicast datagrams
// `delay` - the reader's requests come back at once - so each tail's repair is on its way for `delay` while the reader,
// its interval untimed (1 ms), asks for it again and again. Answers the datagrams the writer resent.
static uint32_t resent_for_lost_tails(int samples, uint64_t delay) {
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_unicast_delay = delay;
    sim_drop_tails = true;
    sample_len = ONE_MB;
    test_mock_nonblocking_room = -1; // each sample goes out whole at once
    uint32_t before = pub.retransmitted;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        sim_settle(i + 2, 2 * tt_SECOND);
    }
    EXPECT_EQ_INT(samples + 1, delivered_count);
    EXPECT_EQ_U32((uint32_t)samples, sim_tail_count); // the treatment: one tail lost a sample
    return pub.retransmitted - before;
}

static void test_a_whole_sample_lost_does_not_storm(void) {
    // The ETHERNET amp writer storm (RESULTS `EWS`): when a reader's receive drain overran, it asked for a whole
    // sample (722 units) at once, and the loaded link took ~9.5 ms to carry the resend batch - longer than the repair
    // timeout, a fixed 2 x srtt from each resend. Every ACKNACK read meanwhile released every unit it named again; once
    // all had gone twice Karn took no sample, the timeout could not grow, and the copies became the queue. Here: the
    // queued path of test_a_queued_path_does_not_storm (12 us a datagram, 4,000 waiting), each sample's tail lost once,
    // and every third sample's first send lost whole (10 of 30). The criterion (set after the runs below, between
    // them, so it is a guard, not a measurement): STORM when the writer resends more than 5 x the units those losses
    // need (10 whole samples and 30 tails). The parent: 167,013 resent (131,414 dropped at the queue, 23 x the need);
    // with the timeout held while the reader shows progress through what went before a resend: 18,429 (2.5 x, 1,290
    // dropped); a per-unit back-off of the timeout instead (x2 a resend, at most x64; not taken - it slowed the slow
    // reader's repairs above from 2 s to 8 s): 58,065 (8 x) (2026-10-26). Control: the same path with no whole sample
    // lost, 7,271 resent on the parent, 5,096 here. Mutants (mutants_large_sample.py, "progress"): the timeout run
    // from the resend alone.
    enum { SAMPLES = 30, EVERY = 3 };
    uint32_t lost = 0;
    int delivered = 0;
    uint64_t control =
        resent_after_whole_losses(SAMPLES, 0, 12 * tt_MICROSECOND, 4000, 0, 2 * tt_SECOND, &lost, &delivered);
    EXPECT_EQ_U32(0, lost);
    EXPECT_EQ_INT(SAMPLES + 1, delivered);
    uint64_t resent =
        resent_after_whole_losses(SAMPLES, EVERY, 12 * tt_MICROSECOND, 4000, 0, 2 * tt_SECOND, &lost, &delivered);
    uint64_t need = ((uint64_t)lost * tt_sample_datagrams(ONE_MB)) + SAMPLES;
    printf("test_large_sample: %d x 1 MiB at 30 Hz through a queue of 4000, %u lost whole - %llu resent (%.1f x the "
           "%llu units lost; %llu with none lost whole), %llu dropped at the queue, %d delivered\n",
           SAMPLES, lost, (unsigned long long)resent, (double)resent / (double)need, (unsigned long long)need,
           (unsigned long long)control, (unsigned long long)sim_socket_drops, delivered);
    EXPECT_EQ_U32((uint32_t)(SAMPLES / EVERY), lost); // the treatment applied
    EXPECT_EQ_INT(SAMPLES + 1, delivered);
    EXPECT_TRUE(resent <= 5U * need);
}

#ifdef tt_LARGE_REPAIR_SLOTS
// The reader stops reading for `ns`: what waits in its socket waits that much longer, and what arrives meanwhile queues
// behind it, dropped once the socket is full. Its timers run on, so it still asks for what it misses.
static void sim_reader_stall(uint64_t ns) {
    for (uint32_t i = sim_late_head; i != sim_late_tail; i++) {
        sim_late_due[i % SIM_HELD] += ns;
    }
    sim_reader_free_ns = (sim_reader_free_ns > test_mock_now ? sim_reader_free_ns : test_mock_now) + ns;
}

// One run of resent_through_stalls(): the sample lost whole that the next stall waits on, and what the stalls showed.
struct stall_run {
    uint64_t stall;      // how long the reader stops reading
    uint32_t srtt;       // the writer's repair srtt to the reader, set as the stall begins (0: as measured)
    bool armed;          // a sample was lost whole, and the reader has not yet stalled on its resend
    uint32_t lost_first; // its first seq_no
    bool stalled;        // the reader is stalled until `end`
    uint64_t end;
    uint32_t stalls;  // stalls applied
    uint32_t copies;  // the most copies of one unit of a sample lost whole sent at the end of a stall
    double mean;      // ... and the mean over that sample's units still pending then, in the stall that sent most
    uint32_t pending; // ... how many those were
};

// A stall's end: how often each unit of the sample lost whole still pending went to the reader meanwhile, which could
// not read any of them.
static void stall_count_copies(struct stall_run* run) {
    const uint32_t units = tt_sample_datagrams(ONE_MB);
    run->stalled = false;
    uint32_t pending = 0;
    uint32_t sum = 0;
    uint32_t most = 0;
    for (uint32_t unit = run->lost_first; unit != run->lost_first + units; unit++) {
        const struct tt_LargeRepair* slot = large_repair_slot(&sender, unit);
        if (large_repair_is(slot, &pub, unit, RECEIVER_ID)) {
            pending++;
            sum += slot->resends;
            most = slot->resends > most ? slot->resends : most;
        }
    }
    double mean = pending != 0 ? (double)sum / (double)pending : 0.0;
    if (mean > run->mean) {
        run->mean = mean;
        run->pending = pending;
    }
    run->copies = most > run->copies ? most : run->copies;
}

// The resend of half of the sample lost whole pending: the reader stalls (stall_count_copies() at its end).
static void stall_watch(struct stall_run* run) {
    const uint32_t units = tt_sample_datagrams(ONE_MB);
    if (run->stalled && test_mock_now >= run->end) {
        stall_count_copies(run);
    }
    if (!run->armed) {
        return;
    }
    uint32_t pending = 0;
    for (uint32_t unit = run->lost_first; unit != run->lost_first + units; unit++) {
        pending += large_repair_is(large_repair_slot(&sender, unit), &pub, unit, RECEIVER_ID) ? 1U : 0U;
    }
    if (pending >= units / 2U) {
        run->armed = false;
        if (run->stall != 0) {
            sim_reader_stall(run->stall);
            run->stalled = true;
            run->end = test_mock_now + run->stall;
            run->stalls++;
        }
        struct tt_PeerAck* ack = find_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id);
        if (ack != NULL && run->srtt != 0) {
            ack->repair_srtt_ns = run->srtt;
            ack->repair_rttvar_ns = 0;
        }
    }
}

// The queued path of resent_after_whole_losses() (12 us a datagram, 4,000 waiting, each tail lost once, every
// `every`-th sample's first send lost whole), and the reader stalled for run->stall once the resend of half of each
// sample lost whole is pending. Answers the units resent; *delivered the samples delivered within 2 s after the last.
static uint64_t resent_through_stalls(int samples, uint32_t every, struct stall_run* run, int* delivered) {
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_reader_service_ns = 12 * tt_MICROSECOND;
    sim_reader_capacity = 4000;
    sim_drop_tails = true;
    sim_whole_loss_every = every;
    sample_len = ONE_MB;
    test_mock_nonblocking_room = -1; // each sample goes out whole at once
    const uint32_t units = tt_sample_datagrams(ONE_MB);
    uint32_t before = pub.retransmitted;
    uint64_t t0 = test_mock_now;
    for (int i = 0; i < samples; i++) {
        bool losing = every != 0 && (uint32_t)(i + 2) % every == 0; // sim_publish()'s rule, by the seed
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        if (losing) {
            run->armed = true;
            run->lost_first = pub.seq_no - units + 1U;
        }
        uint64_t next = t0 + ((uint64_t)(i + 1) * tt_SECOND / 30U);
        while (test_mock_now < next && sim_step()) {
            stall_watch(run);
        }
        if (test_mock_now < next) {
            test_mock_now = next;
        }
    }
    uint64_t since = test_mock_now;
    while (delivered_count < samples + 1 && test_mock_now - since < 2 * tt_SECOND && sim_step()) {
        stall_watch(run);
    }
    *delivered = delivered_count;
    return pub.retransmitted - before;
}

static void test_a_stalled_reader_does_not_storm(void) {
    // The stall the progress-held timeout leaves open (RESULTS `EWF`: one rep where every bench on the PC stormed at
    // once, 8.5 M resent, 8.47 M of them released by the plain timeout). A reader that stops reading while the resend
    // of a whole sample is pending shows no progress, so the timeout runs from each resend; its ACKNACKs go on naming
    // the sample, and each timeout releases every unit again into a socket nobody reads. Here: the queued path of
    // test_a_whole_sample_lost_does_not_storm (12 us a datagram, 4,000 waiting), each tail lost once, every tenth
    // sample's first send lost whole (3 of 30), and the reader stopped for 80 ms once half of each such sample's resend
    // is pending. The writer's srtt is set to 750 us as the stall begins - the calm srtt the census storms began on
    // (EWS: 1.5-3.3 ms timeouts), which Karn then froze; the sim's own tail repairs, timed through the queue, give it
    // 15-27 ms, and a stall of a few timeouts sends 2-3 copies whatever the rule.
    // The criterion (set between the parent's run and this change's, 2026-10-27; a guard, not a measurement): STORM
    // when, at a stall's end, the lost sample's units still pending went to the stalled reader more than 8 times each
    // on average. An 80 ms stall at a 1.5 ms timeout: doubling at each resend sends 6, a fixed timeout up to ~53. The
    // parent (45129b71): 30 copies a unit, 99,996 resent in all (90,508 dropped at the socket); this change: 6, 36,473
    // (24,331). Control: the same run with no stall - the parent 65,451 resent, this change 20,770. Mutants
    // (mutants_large_sample.py, "stall"): no back-off.
    enum { SAMPLES = 30, EVERY = 10 };
    int delivered = 0;
    struct stall_run control = {.stall = 0, .srtt = 750 * tt_MICROSECOND};
    uint64_t unstalled = resent_through_stalls(SAMPLES, EVERY, &control, &delivered);
    EXPECT_EQ_INT(SAMPLES + 1, delivered);
    struct stall_run run = {.stall = 80 * tt_MILLISECOND, .srtt = 750 * tt_MICROSECOND};
    uint64_t resent = resent_through_stalls(SAMPLES, EVERY, &run, &delivered);
    printf("test_large_sample: %d x 1 MiB at 30 Hz through a queue of 4000, %u lost whole, the reader stalled 80 ms %u "
           "times on their resends - %.1f copies of each pending unit at a stall's end (%u units, at most %u), %llu "
           "resent (%llu with no stall), %llu dropped at the queue, %d delivered\n",
           SAMPLES, sim_whole_lost, run.stalls, run.mean, run.pending, run.copies, (unsigned long long)resent,
           (unsigned long long)unstalled, (unsigned long long)sim_socket_drops, delivered);
    EXPECT_EQ_U32((uint32_t)(SAMPLES / EVERY), sim_whole_lost); // the treatment applied
    EXPECT_TRUE(run.stalls >= 2);
    EXPECT_TRUE(run.pending >= tt_sample_datagrams(ONE_MB) / 2U);
    EXPECT_EQ_INT(SAMPLES + 1, delivered);
    EXPECT_TRUE(run.mean <= 8.0);
}
#endif

static void test_a_repair_on_its_way_is_not_resent(void) {
    // A tail lost on a 40 ms path: its repair takes 40 ms to arrive, and a reader that has timed nothing asks again and
    // again meanwhile. Resent at each request, it went ~10 times. Resent only once shown lost - something sent
    // after it arrived, or a repair timeout passed - it goes once, or twice when the timeout, seeded with how long the
    // reader took to ask for it (40 ms), runs out as its repair arrives. Control: the same path with no delay. The
    // parent: 4 samples, 41 resent at 40 ms, 4 at 0; this change 4 and 4 (2026-10-23). Mutant (mutants_large_sample.py,
    // "resend order"): every request answered at once.
    enum { SAMPLES = 4 };
    uint32_t control = resent_for_lost_tails(SAMPLES, 0);
    uint32_t delayed = resent_for_lost_tails(SAMPLES, 40 * tt_MILLISECOND);
    printf("test_large_sample: %d x 1 MiB, each tail lost once - resent %u over a 0 ms path, %u over a 40 ms one "
           "(%llu requests held)\n",
           SAMPLES, control, delayed, HELD_REQUESTS(sender));
    EXPECT_EQ_U32((uint32_t)SAMPLES, control);
    EXPECT_TRUE(delayed >= (uint32_t)SAMPLES && delayed <= 2U * SAMPLES);
}

#ifdef tt_LARGE_REPAIR_SLOTS
// A sent 1 MiB sample to work the rule's parts on directly: its record, and its reader as the writer addresses it.
static struct tt_LargeRecord* order_fixture(struct tt_Peer* reader) {
    init_pair_with(ONE_MB, RELIABLE_KEEP_LAST, PAIR_UNICAST | PAIR_DURABLE_WRITER, NULL); // durable: kept after its ack
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    *reader = (struct tt_Peer) {RECEIVER_ID, RECEIVER_IP, PORT};
    struct tt_LargeRecord* record = pub.large_count != 0 ? large_record_at(&pub, 0) : NULL;
    EXPECT_TRUE(record != NULL && record->first_serial != 0);
    return record;
}

// One ACKNACK from the reader read for the order: its watermark `base`, its previous one `prev`, and the units it names
// (`count` of them, as seq_nos).
static void order_read(const struct tt_Peer* reader, uint32_t prev, uint32_t base, const uint32_t* named, int count,
                       struct large_order* order) {
    uint64_t bitmap[tt_RELIABLE_BITMAP_MAX_WORDS] = {0};
    uint16_t words = 0;
    for (int i = 0; i < count; i++) {
        uint32_t bit = named[i] - base;
        bitmap[bit / 64U] |= 1ULL << (bit % 64U);
        uint16_t need = (uint16_t)((bit / 64U) + 1U);
        words = need > words ? need : words;
    }
    large_order_begin(&sender, &pub, reader, sub.endpoint.entity_id, prev, base, bitmap, words, order);
}

static void test_the_resend_order_rules(void) {
    // Each condition of large_resend_due() and large_order_begin(), on one sent sample. Units are its seq_nos from S.
    struct tt_Peer reader;
    struct tt_LargeRecord* record = order_fixture(&reader);
    if (record == NULL) {
        return;
    }
    const uint32_t S = record->seq_no;
    struct large_order order;
    uint64_t held_before = sender.large.resend_held;

    // Never resent: answered at once, before and after anything is pending.
    order_read(&reader, S, S, (uint32_t[]) {S + 10}, 1, &order);
    EXPECT_TRUE(!order.active);
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 10, &reader, &order));
    test_mock_now += 5 * tt_MILLISECOND; // the reader asked 5 ms after the sample went: the timeout's seed
    large_note_resent(&sender, &pub, record, S + 10, &reader);
    EXPECT_EQ_U32(1, sender.large.repairs_live);

    // Resent, asked again at once with nothing shown: held, and counted.
    order_read(&reader, S, S, (uint32_t[]) {S + 10}, 1, &order);
    EXPECT_TRUE(order.active);
    EXPECT_TRUE(!large_resend_due(&sender, &pub, S + 10, &reader, &order));
    EXPECT_EQ_U64(held_before + 1U, sender.large.resend_held);

    // Another reader's context asking for the same unit is answered: the pending resend went to the first one only.
    struct tt_Peer other = {RECEIVER_ID + 1U, RECEIVER_IP + 1U, PORT};
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 10, &other, &order));

    // Shown lost: a unit resent after it (S + 20) arrived - left out between two requests - so it goes at once.
    large_note_resent(&sender, &pub, record, S + 20, &reader);
    large_note_resent(&sender, &pub, record, S + 30, &reader);
    order_read(&reader, S, S, (uint32_t[]) {S + 10, S + 30}, 2, &order);
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 10, &reader, &order));
    // ... but not the unit resent after the one that arrived: nothing sent after it has been shown.
    EXPECT_TRUE(!large_resend_due(&sender, &pub, S + 30, &reader, &order));
    // The one left out is taken as arrived but not timed into the repair timeout (the reader may have left it out as
    // a repair on its way - RESULTS `TVS`): srtt has no sample, RACK's round trip has it, and its slot stays, its next
    // request answered at once. Resent so, it goes as a first resend, and stays untimed (below). Written against the
    // base's fields only, as the rest of this test.
    const struct tt_PeerAck* ack = find_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id);
    EXPECT_TRUE(ack != NULL && ack->repair_srtt_ns == 0 && ack->repair_rack_rtt_ns != 0);
    EXPECT_TRUE(large_repair_is(large_repair_slot(&sender, S + 20), &pub, S + 20, RECEIVER_ID));
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 20, &reader, &order));
    large_note_resent(&sender, &pub, record, S + 20, &reader);
    EXPECT_TRUE(large_repair_slot(&sender, S + 20)->resends == 1);

    // A unit left out but outside the requested range shows nothing: a narrow request names one new gap only, and what
    // lies below it may still be missing. S + 40 resent after S + 30, then S + 30 asked for alone.
    large_note_resent(&sender, &pub, record, S + 40, &reader);
    order_read(&reader, S, S, (uint32_t[]) {S + 30}, 1, &order);
    EXPECT_TRUE(!large_resend_due(&sender, &pub, S + 30, &reader, &order));
    EXPECT_TRUE(large_repair_is(large_repair_slot(&sender, S + 40), &pub, S + 40, RECEIVER_ID)); // still pending

    // The watermark passing a resent unit shows it: S + 30 and S + 40 below the new watermark S + 41 - and times
    // them, S + 20 (untimed) not.
    test_mock_now += 4 * tt_MILLISECOND;
    order_read(&reader, S, S + 41, (uint32_t[]) {S + 41}, 1, &order);
    EXPECT_TRUE(order.shown && large_serial_after(order.shown_serial, large_repair_slot(&sender, S + 30)->serial));
    EXPECT_TRUE(!large_repair_is(large_repair_slot(&sender, S + 40), &pub, S + 40, RECEIVER_ID));
    EXPECT_TRUE(!large_repair_is(large_repair_slot(&sender, S + 20), &pub, S + 20, RECEIVER_ID));
    EXPECT_TRUE(ack->repair_srtt_ns != 0);

    // A unit of a sample that began to go after a resend, received, shows the resend lost; a unit of the sample the
    // resend is of does not - its copy went before it (large_unit_sent_after()). S + 50 and S + 60 resent, then asked
    // for again with this sample's S + 599 left out between them and S + 600: held.
    large_note_resent(&sender, &pub, record, S + 50, &reader);
    large_note_resent(&sender, &pub, record, S + 60, &reader);
    order_read(&reader, S, S, (uint32_t[]) {S + 50, S + 60, S + 600}, 3, &order);
    EXPECT_TRUE(!large_resend_due(&sender, &pub, S + 50, &reader, &order));
    // The next sample goes after both resends; one of its units left out between requests shows them lost.
    uint32_t next = pub.seq_no + 1U;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2, tt_SECOND));
    sim_settle(2, tt_SECOND);
    EXPECT_TRUE(large_find(&pub, record->seq_no) == record && large_find(&pub, next) != NULL); // both still held
    order_read(&reader, S, S, (uint32_t[]) {S + 50, S + 60, next + 10}, 3, &order);
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 50, &reader, &order));
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 60, &reader, &order));

    // The repair timeout: S + 70 resent, nothing shown, held until twice the reader's srtt has passed. Resent twice, so
    // RACK's time rule (below), which answers a unit's first resend only, cannot release it first.
    large_note_resent(&sender, &pub, record, S + 70, &reader);
    large_note_resent(&sender, &pub, record, S + 70, &reader);
    order_read(&reader, S, S, (uint32_t[]) {S + 70}, 1, &order);
    EXPECT_TRUE(order.timeout != 0);
    EXPECT_TRUE(!large_resend_due(&sender, &pub, S + 70, &reader, &order));
    test_mock_now += ((uint64_t)order.timeout + 1U) << LARGE_REPAIR_SHIFT;
    order_read(&reader, S, S, (uint32_t[]) {S + 70}, 1, &order);
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 70, &reader, &order));

    // Karn: a unit resent twice is not timed when it arrives; once, it is.
    large_note_resent(&sender, &pub, record, S + 80, &reader);
    large_note_resent(&sender, &pub, record, S + 80, &reader);
    struct tt_PeerAck* writable = find_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id);
    uint32_t srtt = writable->repair_srtt_ns;
    test_mock_now += 100 * tt_MILLISECOND; // a sample this long would move srtt
    order_read(&reader, S + 80, S + 81, (uint32_t[]) {S + 81}, 1, &order);
    EXPECT_EQ_U32(srtt, writable->repair_srtt_ns);
    EXPECT_TRUE(!large_repair_is(large_repair_slot(&sender, S + 80), &pub, S + 80, RECEIVER_ID)); // but done with

    // A reader sends the words up to the highest unit it knows of: what it leaves out in every word but the last has
    // arrived, above its highest request too. S + 90, S + 95, S + 140 resent; asked for S + 90 alone in three words:
    // S + 95 (word 1) arrived after S + 90's resend, so S + 90 goes; S + 140 (the last word) says nothing yet.
    large_note_resent(&sender, &pub, record, S + 90, &reader);
    large_note_resent(&sender, &pub, record, S + 95, &reader);
    large_note_resent(&sender, &pub, record, S + 140, &reader);
    {
        uint64_t bitmap[tt_RELIABLE_BITMAP_MAX_WORDS] = {0};
        bitmap[1] = 1ULL << (90U - 64U); // from base S + 0: S + 90 is bit 90
        large_order_begin(&sender, &pub, &reader, sub.endpoint.entity_id, S, S, bitmap, 3, &order);
    }
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 90, &reader, &order));
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 95, &reader, &order)); // shown, taken as arrived: answered at once
    EXPECT_TRUE(large_repair_is(large_repair_slot(&sender, S + 140), &pub, S + 140, RECEIVER_ID)); // still pending

    // RACK's time rule (the tail, DESIGN.md section 4): a unit still named RACK.rtt + reo_wnd after its one resend
    // goes, with nothing sent after it and before the repair timeout. RACK.rtt follows the most recently resent repair
    // shown arrived, not the last to be shown: S + 101 resent, S + 100 8 ms later, both shown 1 ms after that - 9 ms
    // and 1 ms round trips, passed by the watermark bottom up (S + 100 first, S + 101 last), so RACK.rtt is S + 100's
    // 1 ms while srtt takes both. Written against the base's fields only, so the base's tickle.c builds against it and
    // fails (S + 120 held).
    large_note_resent(&sender, &pub, record, S + 101, &reader);
    test_mock_now += 8 * tt_MILLISECOND;
    large_note_resent(&sender, &pub, record, S + 100, &reader);
    test_mock_now += 1 * tt_MILLISECOND;
    order_read(&reader, S + 99, S + 102, (uint32_t[]) {S + 110}, 1, &order);
    // S + 120 resent once, nothing after it: held at half RACK.rtt, due at one and a half - before the timeout.
    large_note_resent(&sender, &pub, record, S + 120, &reader);
    test_mock_now += 500 * tt_MICROSECOND;
    order_read(&reader, S, S, (uint32_t[]) {S + 120}, 1, &order);
    EXPECT_TRUE(!large_resend_due(&sender, &pub, S + 120, &reader, &order));
    test_mock_now += 1 * tt_MILLISECOND;
    order_read(&reader, S, S, (uint32_t[]) {S + 120}, 1, &order);
    EXPECT_TRUE(((uint64_t)order.timeout << LARGE_REPAIR_SHIFT) > 2 * tt_MILLISECOND); // the timeout would hold it
    printf("test_large_sample: resend order - repair timeout %.2f ms, a tail resent once due 1.5 ms after it went\n",
           (double)((uint64_t)order.timeout << LARGE_REPAIR_SHIFT) / (double)tt_MILLISECOND);
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 120, &reader, &order));
    // ... once: resent again, the same wait past its second resend leaves it to the order rule and the timeout.
    large_note_resent(&sender, &pub, record, S + 120, &reader);
    test_mock_now += 1500 * tt_MICROSECOND;
    order_read(&reader, S, S, (uint32_t[]) {S + 120}, 1, &order);
    EXPECT_TRUE(!large_resend_due(&sender, &pub, S + 120, &reader, &order));

    // A unit left out stays untimed (RESULTS `TVS`): S + 200 resent, left out, asked for and resent again, then passed
    // by the watermark 50 ms later - no sample, srtt as it was. Timed, it is the copy that may have arrived as the
    // earlier one (the base frees the slot when it is left out and times the second resend: srtt moves).
    large_note_resent(&sender, &pub, record, S + 200, &reader);
    order_read(&reader, S, S, (uint32_t[]) {S + 199, S + 201}, 2, &order);
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 200, &reader, &order));
    large_note_resent(&sender, &pub, record, S + 200, &reader);
    uint32_t srtt_before = writable->repair_srtt_ns;
    test_mock_now += 50 * tt_MILLISECOND;
    order_read(&reader, S + 199, S + 201, (uint32_t[]) {S + 201}, 1, &order);
    EXPECT_TRUE(!large_repair_is(large_repair_slot(&sender, S + 200), &pub, S + 200, RECEIVER_ID)); // done with
    EXPECT_EQ_U32(srtt_before, writable->repair_srtt_ns);

    // A Publisher going away takes its pending repairs: none left for its entity.
    large_repairs_forget(&sender, &pub);
    EXPECT_EQ_U32(0, sender.large.repairs_live);
}
#endif

#ifdef tt_LARGE_REPAIR_SLOTS
static void test_a_tail_waits_rack_rtt_and_its_reordering_window(void) {
    // RACK's time rule on a reader whose timed repairs took 4 ms, then 1 ms: RACK.rtt and min_RTT 1 ms, reo_wnd a
    // quarter of it, the repair timeout twice srtt (7.25 ms). A tail resent once, nothing after it, still named: held
    // 1.1 ms after it went (inside RACK.rtt + reo_wnd), due at 1.3 ms - where the base held it to the timeout.
    struct tt_Peer reader;
    struct tt_LargeRecord* record = order_fixture(&reader);
    if (record == NULL) {
        return;
    }
    const uint32_t S = record->seq_no;
    struct large_order order;
    large_note_resent(&sender, &pub, record, S + 10, &reader);
    test_mock_now += 4 * tt_MILLISECOND;
    order_read(&reader, S + 9, S + 11, (uint32_t[]) {S + 11}, 1, &order); // the watermark passed S + 10: arrived
    large_note_resent(&sender, &pub, record, S + 12, &reader);
    test_mock_now += 1 * tt_MILLISECOND;
    order_read(&reader, S + 11, S + 13, (uint32_t[]) {S + 13}, 1, &order); // S + 12 arrived
    large_note_resent(&sender, &pub, record, S + 20, &reader);
    test_mock_now += 1100 * tt_MICROSECOND;
    order_read(&reader, S, S, (uint32_t[]) {S + 20}, 1, &order);
    EXPECT_TRUE(!large_resend_due(&sender, &pub, S + 20, &reader, &order));
    test_mock_now += 200 * tt_MICROSECOND;
    order_read(&reader, S, S, (uint32_t[]) {S + 20}, 1, &order);
    EXPECT_TRUE(((uint64_t)order.timeout << LARGE_REPAIR_SHIFT) > 7 * tt_MILLISECOND);
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 20, &reader, &order));
}
#endif

#ifdef tt_LARGE_REPAIR_SLOTS
static void test_the_repair_timeout_waits_while_the_reader_progresses(void) {
    // A resend batch the path is still carrying (RESULTS `EWS`): 100 units resent twice (so RACK's rule, which answers
    // a first resend only, stays out of it), the reader's srtt 4 ms - the timeout ~8 ms. Every 5 ms an ACKNACK passes
    // the next 5 units of the batch and names its upper half: the reader is receiving the batch in order, and the units
    // it names are behind what it received. For 50 ms the top unit is held - the base, timing from the resend alone,
    // released it from the second ACKNACK on (and then at every one). Then the path stalls - the same watermark one
    // timeout later - and it goes: what a stall shows is loss. Mutants (mutants_large_sample.py, "progress"): the
    // timeout run from the resend alone; progress taken at every ACKNACK, not only at a later send shown.
    struct tt_Peer reader;
    struct tt_LargeRecord* record = order_fixture(&reader);
    if (record == NULL) {
        return;
    }
    const uint32_t S = record->seq_no;
    struct large_order order;
    large_note_resent(&sender, &pub, record, S + 10, &reader);
    test_mock_now += 4 * tt_MILLISECOND;
    order_read(&reader, S + 9, S + 11, (uint32_t[]) {S + 11}, 1, &order); // S + 10 arrived: srtt 4 ms
    for (int copy = 0; copy < 2; copy++) {
        for (uint32_t unit = S + 100; unit < S + 200; unit++) {
            large_note_resent(&sender, &pub, record, unit, &reader);
        }
    }
    uint32_t named[50];
    for (int i = 0; i < 50; i++) {
        named[i] = S + 150U + (uint32_t)i;
    }
    int held = 0;
    for (uint32_t step = 0; step < 10; step++) {
        test_mock_now += 5 * tt_MILLISECOND;
        order_read(&reader, S + 100 + (5 * step), S + 105 + (5 * step), named, 50, &order);
        held += large_resend_due(&sender, &pub, S + 199, &reader, &order) ? 0 : 1;
    }
    uint64_t timeout_ns = (uint64_t)order.timeout << LARGE_REPAIR_SHIFT;
    printf("test_large_sample: a resend batch read in order every 5 ms for 50 ms, repair timeout %.2f ms - its top "
           "unit held at %d of 10 ACKNACKs\n",
           (double)timeout_ns / (double)tt_MILLISECOND, held);
    EXPECT_TRUE(timeout_ns > 7 * tt_MILLISECOND && timeout_ns < 10 * tt_MILLISECOND);
    EXPECT_EQ_INT(10, held);
    test_mock_now += timeout_ns + tt_MILLISECOND;
    order_read(&reader, S + 150, S + 150, named, 50, &order); // stalled: nothing more shown
    EXPECT_TRUE(large_resend_due(&sender, &pub, S + 199, &reader, &order));
}
#endif

#ifdef tt_LARGE_REPAIR_SLOTS
// Asks for `unit` in an ACKNACK of `count` units from `named` (watermark `base`, the one before `prev`), and resends it
// in that ACKNACK's answer when it is due, as large_resend() does. True when it went.
static bool order_ask_and_resend(const struct tt_Peer* reader, const struct tt_LargeRecord* record, uint32_t prev,
                                 uint32_t base, const uint32_t* named, int count, uint32_t unit,
                                 struct large_order* order) {
    order_read(reader, prev, base, named, count, order);
    if (!large_resend_due(&sender, &pub, unit, reader, order)) {
        return false;
    }
    sender.large.acknack_order = order;
    large_note_resent(&sender, &pub, record, unit, reader);
    sender.large.acknack_order = NULL;
    return true;
}

static void test_the_repair_timeout_backs_off_while_the_reader_shows_nothing(void) {
    // A stalled reader (RESULTS `EWF`; test_a_stalled_reader_does_not_storm on the queued path): 50 units resent twice,
    // the reader's srtt 4 ms - the timeout ~8 ms - and an ACKNACK every millisecond for 200 ms naming them all, with
    // nothing received. The parent, timing from each resend alone, released the top unit at every timeout: 22 copies
    // (2026-10-27). Doubling the timeout at each resend in a row with no progress between (at most x64): released at
    // ~8, 24, 56 and 120 ms - 4 copies, and the timeout still runs (a stall is not waited out for ever). Then the
    // reader shows progress - an ACKNACK passes five of the units - and the back-off goes: the top unit, resent before
    // it, is held for one plain timeout from the progress and released after it, and the resend that follows waits one
    // plain timeout again. A back-off kept through progress is EWS's candidate F, which slowed a reader dropping at its
    // socket from 2 s to 8 s. Mutants (mutants_large_sample.py, "stall"): no back-off; the back-off not cleared by
    // progress; the back-off applied after progress too.
    struct tt_Peer reader;
    struct tt_LargeRecord* record = order_fixture(&reader);
    if (record == NULL) {
        return;
    }
    const uint32_t S = record->seq_no;
    struct large_order order;
    large_note_resent(&sender, &pub, record, S + 10, &reader);
    test_mock_now += 4 * tt_MILLISECOND;
    order_read(&reader, S + 9, S + 11, (uint32_t[]) {S + 11}, 1, &order); // S + 10 arrived: srtt 4 ms
    uint32_t named[50];
    for (int i = 0; i < 50; i++) {
        named[i] = S + 100U + (uint32_t)i;
        large_note_resent(&sender, &pub, record, named[i], &reader);
        large_note_resent(&sender, &pub, record, named[i], &reader); // twice: RACK's rule stays out of it
    }
    const uint32_t top = S + 149;
    int copies = 0;
    for (int ms = 0; ms < 200; ms++) {
        test_mock_now += tt_MILLISECOND;
        copies += order_ask_and_resend(&reader, record, S + 11, S + 11, named, 50, top, &order) ? 1 : 0;
    }
    uint64_t timeout_ns = (uint64_t)order.timeout << LARGE_REPAIR_SHIFT;
    printf("test_large_sample: a reader showing nothing for 200 ms, an ACKNACK every 1 ms, repair timeout %.2f ms - "
           "its top unit resent %d times\n",
           (double)timeout_ns / (double)tt_MILLISECOND, copies);
    EXPECT_TRUE(timeout_ns > 7 * tt_MILLISECOND && timeout_ns < 10 * tt_MILLISECOND);
    EXPECT_TRUE(copies >= 3 && copies <= 5);
    // Progress: the watermark passes S + 100 .. S + 104, resent before the top unit's last resend.
    test_mock_now += tt_MILLISECOND;
    EXPECT_TRUE(!order_ask_and_resend(&reader, record, S + 100, S + 105, named + 5, 45, top, &order));
    EXPECT_TRUE(order.progressed);
    test_mock_now += timeout_ns / 2U;
    EXPECT_TRUE(!order_ask_and_resend(&reader, record, S + 105, S + 105, named + 5, 45, top, &order));
    test_mock_now += (timeout_ns / 2U) + tt_MILLISECOND; // one plain timeout past the progress, not 16 of them
    EXPECT_TRUE(order_ask_and_resend(&reader, record, S + 105, S + 105, named + 5, 45, top, &order));
    // ... and that resend, with progress since the last, waits one plain timeout again.
    test_mock_now += timeout_ns / 2U;
    EXPECT_TRUE(!order_ask_and_resend(&reader, record, S + 105, S + 105, named + 5, 45, top, &order));
    test_mock_now += (timeout_ns / 2U) + tt_MILLISECOND;
    EXPECT_TRUE(order_ask_and_resend(&reader, record, S + 105, S + 105, named + 5, 45, top, &order));
}
#endif

#ifdef tt_LARGE_REPAIR_SLOTS
static uint32_t least_srtt; // the least repair-timeout srtt the writer held to the reader during least_timed_srtt()

static void note_least_srtt(void) {
    const struct tt_PeerAck* ack = find_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id);
    if (ack != NULL && ack->repair_srtt_ns != 0 && (least_srtt == 0 || ack->repair_srtt_ns < least_srtt)) {
        least_srtt = ack->repair_srtt_ns;
    }
}

// `samples` 1 MiB samples at 5 Hz, `loss_per_mille` lost, over a path that holds the writer's unicast datagrams `delay`
// and the reader's not at all. Answers the least srtt the writer's repair timeout to this reader stood on at any step
// (0: never timed), and through *rack the least RACK.rtt (min_RTT).
static uint32_t least_timed_srtt(int samples, uint64_t delay, uint32_t loss_per_mille, uint32_t* rack) {
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_unicast_delay = delay;
    sim_loss_per_mille = loss_per_mille;
    sample_len = ONE_MB;
    test_mock_nonblocking_room = -1; // each sample goes out whole at once
    least_srtt = 0;
    uint64_t t0 = test_mock_now;
    for (int i = 0; i < samples; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        note_least_srtt();
        uint64_t next = t0 + ((uint64_t)(i + 1) * tt_SECOND / 5U);
        while (test_mock_now < next && sim_step()) {
            note_least_srtt();
        }
        if (test_mock_now < next) {
            test_mock_now = next;
        }
    }
    uint64_t since = test_mock_now;
    while (delivered_count < samples + 1 && test_mock_now - since < 2 * tt_SECOND && sim_step()) {
        note_least_srtt();
    }
    EXPECT_EQ_INT(samples + 1, delivered_count);
    const struct tt_PeerAck* ack = find_peer_ack(&pub, RECEIVER_ID, sub.endpoint.entity_id);
    *rack = ack != NULL ? ack->repair_min_rtt_ns : 0;
    return least_srtt;
}

static void test_the_repair_timeout_never_stands_on_less_than_its_path(void) {
    // The tv storm (RESULTS `TVS`, 2026-10-25): the repair timeout is 2 x srtt over this reader's repairs, each timed
    // from its resend to the first ACKNACK to show it arrived. An ACKNACK the reader sent before a resend could reach
    // it leaves the unit out when the reader reckons an earlier repair of it on its way (repairs_in_flight()); read as
    // "arrived", it timed the round trip as the microseconds between the resend and that ACKNACK. On the PC
    // (PROFILE=wifi, tv) such samples took the srtt from ~40 ms to under 128 us, the timeout released every held unit
    // at every request, and the resends filled netem's queue (525,319 and 1,829,941 resent in a 30 s rep). No repair is
    // shown arrived sooner than its path takes, so the criterion (registered before the fix, storm_tv_ab.sh): the
    // timeout's srtt, once it has a sample, never below the 20 ms unicast delay at any step. Control: no loss, nothing
    // resent, nothing timed. The parent: 0.000 ms, 2,893 resent. RACK's round trip still takes such samples (its rule
    // releases a unit's first resend only): printed, not held to the path. Here the fix times nothing - every unit
    // resent once is left out by some ACKNACK first - so its timeout stays each unit's seed (large_note_resent()).
    uint32_t rack = 0;
    EXPECT_EQ_U32(0U, least_timed_srtt(5, 20 * tt_MILLISECOND, 0, &rack));
    uint32_t before = pub.retransmitted;
    uint32_t least = least_timed_srtt(10, 20 * tt_MILLISECOND, 50, &rack);
    printf("test_large_sample: 10 x 1 MiB at 5%% over a 20 ms unicast path - the repair timeout's least srtt %.3f ms, "
           "min_RTT %.3f ms, %u resent, %llu requests held\n",
           (double)least / 1e6, (double)rack / 1e6, pub.retransmitted - before, HELD_REQUESTS(sender));
    EXPECT_TRUE(pub.retransmitted - before > 0); // the treatment: repairs went
    EXPECT_TRUE(least == 0 || least >= 20 * tt_MILLISECOND);
}
#endif

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

static int idle_callbacks;

static void count_idle_callback(struct tt_Publisher* p, void* param) {
    (void)p;
    (void)param;
    idle_callbacks++;
}

static void test_large_in_flight_answers_and_wakes_once(void) {
    // tt_Publisher_large_in_flight() (rmw_tickle's RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS sleeps on it): true while a large
    // sample is still going out or waits behind one, false once all have gone; and having answered true, the writable
    // callback fires exactly once, from the send's own resumption, when the last of them has gone - KEEP_LAST, which
    // never fired it before. A Publisher that did not ask is not called (the control): the callback is the answer to
    // the question, not a new event every large send raises. Mutants: the notify removed (0 calls), or not cleared (2).
    init_pair_with(ONE_MB, RELIABLE_KEEP_LAST, 0,
                   &(struct tt_PublisherOptions) {.writable_callback = count_idle_callback});
    sim_start(0);
    idle_callbacks = 0;
    sim_room_per_step = 64;
    sim_acting = SENDER_ID;
    test_mock_nonblocking_room = 64;
    EXPECT_TRUE(!tt_Publisher_large_in_flight(&pub)); // nothing published yet
    EXPECT_TRUE(!tt_Publisher_large_in_flight(NULL));
    sample_seed = 1;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    sample_seed = 2;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len)); // waits behind the first
    EXPECT_TRUE(tt_Publisher_large_in_flight(&pub));
    EXPECT_TRUE(tt_Publisher_large_in_flight(&pub)); // asked twice, still one wake
    EXPECT_EQ_INT(0, idle_callbacks);
    int steps = 0;
    while (tt_Publisher_large_in_flight(&pub) && steps < 100000 && sim_step()) {
        steps++;
        if (tt_Publisher_large_in_flight(&pub)) {
            EXPECT_EQ_INT(0, idle_callbacks); // not before the last datagram of the waiting sample has gone
        }
    }
    EXPECT_TRUE(!tt_Publisher_large_in_flight(&pub));
    EXPECT_EQ_INT(1, idle_callbacks);
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_U64(0, sender.large.tail_abandoned);

    // The control: the same send, nobody asking - no call.
    sample_seed = 3;
    test_mock_nonblocking_room = 64;
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    sim_settle(3, tt_SECOND);
    EXPECT_EQ_INT(3, delivered_count);
    EXPECT_EQ_INT(1, idle_callbacks);
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

    init_pair_with(ONE_MB, RELIABLE_KEEP_LAST, PAIR_DURABLE_WRITER, NULL);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U32(1, sender_alloc.live);
    EXPECT_EQ_U32(1, pub.large_count);
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_destroy(&pub));
    EXPECT_EQ_U32(0, sender_alloc.live); // destroying the Publisher hands every buffer back
}

// tt_InitOptions bundle 9: a context's capacity.large_retained is the most large records one of its Publishers keeps,
// large[] keeping its compiled size. The control is each writer at the default, tt_LARGE_RETAINED;
// tests/mutants_init_options_capacity.sh puts the constant back in each reader.
//   - A durable KEEP_LAST writer (the pair's cache has no sample depth, so only the capacity bounds it) keeps that
//     many, the oldest going as the next commits (large_ring_limit()).
//   - A KEEP_ALL writer holding that many, the oldest unacknowledged, refuses the next large publish
//     (large_keep_all_admits()) rather than drop an unacknowledged record. Asked directly, for one datagram: in this
//     pair the cache's 256-datagram bound (keep_all_bound()) refuses any second unacknowledged large sample first, so
//     only a writer with a deeper cache gets to ask - the limit is 1 here, one sample out and nothing acknowledged.
#define CAPACITY_LARGE 3U
static void test_capacity_large_retained_is_the_records_kept(void) {
    static_assert(CAPACITY_LARGE + 2U <= tt_LARGE_RETAINED, "the default must hold what the limit refuses");
    for (int limited = 0; limited <= 1; limited++) {
        init_pair_with(ONE_MB, RELIABLE_KEEP_LAST, PAIR_DURABLE_WRITER, NULL);
        EXPECT_EQ_U32(tt_LARGE_RETAINED, sender.opts.capacity.large_retained); // resolved by node_init_locks()
        if (limited) {
            sender.opts.capacity.large_retained = CAPACITY_LARGE;
        }
        sim_start(0);
        for (uint8_t seed = 1; seed <= CAPACITY_LARGE + 2U; seed++) {
            EXPECT_EQ_INT(tt_RET_OK, sim_publish(seed, tt_SECOND));
            sim_settle(seed, tt_SECOND);
        }
        EXPECT_EQ_INT((int)CAPACITY_LARGE + 2, delivered_count);
        EXPECT_EQ_U32(limited ? CAPACITY_LARGE : CAPACITY_LARGE + 2U, pub.large_count);

        // KEEP_ALL: the sample's datagrams all lost, so nothing is acknowledged.
        init_pair(ONE_MB, RELIABLE_KEEP_ALL);
        if (limited) {
            sender.opts.capacity.large_retained = 1;
        }
        sim_start(0);
        sim_acting = SENDER_ID;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
        sim_head = sim_tail;
        EXPECT_EQ_U32(1, pub.large_count);
        EXPECT_TRUE(!pub.large_cursor.active);
        EXPECT_EQ_INT(limited ? tt_RET_WOULD_BLOCK : tt_RET_OK, large_keep_all_admits(&pub, 1));
    }
    teardown_pair();
}

static void test_a_durable_writer_backs_a_late_joiner_with_its_large_samples(void) {
    init_pair_with(ONE_MB, RELIABLE_KEEP_LAST, PAIR_DURABLE_WRITER | PAIR_DURABLE_READER, NULL);
    sim_start(0);
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    sim_head = sim_tail; // nobody heard it the first time
    EXPECT_EQ_INT(0, delivered_count);
    struct tt_Peer late = {RECEIVER_ID, RECEIVER_IP, PORT};
    deliver_durability_backlog(&sender, &pub, &late);
    EXPECT_EQ_U32(tt_sample_datagrams(ONE_MB), sim_tail - sim_head);
    sim_drain();
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
}

static void test_a_late_joiner_gets_a_large_backlog_from_a_writer_far_past_1(void) {
    // Three samples nobody heard from a writer that had sent more than the reader's window (8,192 datagrams) before,
    // then a DURABLE reader joins in a match's order: the backlog first, so its first contact is the oldest sample's
    // FRAG_FIRST_L, far past the default baseline of 1 - then the initial Heartbeat. All three, in order, whole, none
    // given up (tests/test_durable_late_joiner.c's reading). Until 2026-10-15 the first was lost: the baseline jumped
    // to it without the reorder cursor, the drain walked past it, and its assembly was abandoned. The control is the
    // same with the Heartbeat first, whose baseline is the writer's oldest before any DATA arrives.
    static const uint32_t sizes[] = {65508, ONE_MB};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        for (int control = 1; control >= 0; control--) {
            init_pair_with(sizes[s], RELIABLE_KEEP_LAST, PAIR_DURABLE_WRITER | PAIR_DURABLE_READER, NULL);
            sim_start(0);
            pub.seq_no = 100000U - 1U;
            sim_acting = SENDER_ID;
            for (int i = 0; i < 3; i++) {
                sample_seed = (uint8_t)(i + 1);
                EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
                sim_head = sim_tail; // nobody heard it
            }
            struct tt_Peer late = {RECEIVER_ID, RECEIVER_IP, PORT};
            if (control != 0) {
                send_initial_heartbeat(&sender, &pub, &late);
                sim_drain();
            }
            deliver_durability_backlog(&sender, &pub, &late);
            if (control == 0) {
                send_initial_heartbeat(&sender, &pub, &late);
            }
            sim_drain();
            sim_settle(3, tt_SECOND);
            uint32_t abandoned = sub.gap_abandoned + sub.gap_evicted + (uint32_t)receiver.large.abandoned;
            printf("test_large_sample: %s durable backlog of 3 x %u B from 100000: %d delivered (seeds %u %u %u), %d "
                   "torn, %u given up\n",
                   control != 0 ? "Heartbeat-first" : "match-order", sizes[s], delivered_count, delivered_seeds[0],
                   delivered_seeds[1], delivered_seeds[2], delivered_torn, abandoned);
            EXPECT_EQ_INT(3, delivered_count);
            EXPECT_EQ_INT(1, delivered_seeds[0]);
            EXPECT_EQ_INT(2, delivered_seeds[1]);
            EXPECT_EQ_INT(3, delivered_seeds[2]);
            EXPECT_EQ_INT(0, delivered_torn);
            EXPECT_EQ_U32(0, abandoned);
        }
    }
}

// --- A durable backlog that interleaves large and small samples --------------------------------------------------
// A TRANSIENT_LOCAL writer publishes small and large samples, mixed, before any reader is matched - and so sends
// nothing - then a durable reader joins in a match's order (register_subscriber_peer_on_publisher(): the backlog, then
// the initial Heartbeat). deliver_durability_backlog() walks the small cache in seq_no order and sends each large
// record before the first small sample above it (large_send_backlog_before()), so the backlog leaves in publish order.
// The reader must get the writer's history - KEEP_LAST: its newest `depth` samples - in order, each whole, at 5% loss
// too, and give nothing up on its own: a RELIABLE reader drops a gap only on the writer's word (DESIGN.md section 6).

#define MIXED_RING 2048 // datagram slots: the whole of either mix, as rmw_tickle sizes a large type's ring
static struct tt_ReliableCacheIndex mixed_index[MIXED_RING];
static uint8_t mixed_arena[tt_RELIABLE_CACHE_ARENA_BYTES(32, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache mixed_cache;

#define SMALL_ONE 1000U    // one datagram, one seq_no
#define SMALL_FRAG 5000U   // a fragmented small sample: a seq_no per datagram
#define LARGE_100K 102400U // 71 datagrams

// The seq_no a captured datagram of the Publisher's samples carries - DATA, FRAG_FIRST(_L) and FRAG_CONT(_L) all have
// it 4 bytes into the first submessage's body. False for any other datagram: a HEARTBEAT, or an announce (DATA too,
// of another entity).
static bool datagram_seq_no(const uint8_t* bytes, uint32_t* seq_no) {
    const uint8_t* body = NULL;
    uint8_t type = datagram_type(bytes, &body);
    uint32_t entity_id = 0;
    if (type == tt_SUBMESSAGE_TYPE_DATA || type == tt_SUBMESSAGE_TYPE_FRAG_FIRST ||
        type == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L) {
        memcpy(&entity_id, body + offsetof(struct tt_DataHeader, entity_id), sizeof(entity_id));
    } else if (type == tt_SUBMESSAGE_TYPE_FRAG_CONT || type == tt_SUBMESSAGE_TYPE_FRAG_CONT_L) {
        memcpy(&entity_id, body, sizeof(entity_id)); // entity_id leads both continuation headers
    } else {
        return false;
    }
    memcpy(seq_no, body + sizeof(uint32_t), sizeof(*seq_no));
    return entity_id == pub.endpoint.entity_id;
}

// The writer's sample datagrams queued in [from, to) up to its first HEARTBEAT after them - for a backlog, the initial
// Heartbeat that follows it - and, in *ascending, whether each seq_no was above the one before.
static uint32_t sample_datagrams_sent(uint32_t from, uint32_t to, bool* ascending) {
    uint32_t sent = 0;
    uint32_t previous = 0;
    for (uint32_t i = from; i < to; i++) {
        const struct sim_datagram* d = &sim_queue[i % SIM_QUEUE];
        const uint8_t* body = NULL;
        uint32_t seq_no = 0;
        if (d->from != SENDER_ID) {
            continue;
        }
        if (datagram_seq_no(d->bytes, &seq_no)) {
            if (ascending != NULL && sent != 0 && seq_delta(seq_no, previous) <= 0) {
                *ascending = false;
            }
            previous = seq_no;
            sent++;
        } else if (sent != 0 && datagram_type(d->bytes, &body) == tt_SUBMESSAGE_TYPE_HEARTBEAT) {
            break;
        }
    }
    return sent;
}

// Takes the reader off the writer init_pair() matched it to, as if it had never been there.
static void unmatch_reader(void) {
    forget_peer(pub.peers, RECEIVER_ID);
    for (uint8_t i = 1; i <= tt_UNICAST_PEER_THRESHOLD; i++) {
        forget_peer(pub.peers, (uint8_t)(100U + i));
    }
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        pub.peer_acks[i].context_id = tt_CONTEXT_ID_INVALID;
        pub.peer_acks[i].entity_id = 0;
    }
}

// One run: `count` samples of `sizes` (seeds 1..count) published unmatched by a durable writer of `mode` (KEEP_LAST
// `depth`), then a durable reader matched over a link losing `loss` per mille.
// A durable writer of `mode` (KEEP_LAST `depth`) with a cache the whole of either mix fits, and a reader with a reorder
// buffer, matched as init_pair() matches it.
static void mixed_pair(uint32_t first_size, enum pair_mode mode, uint16_t depth) {
    init_pair_with(first_size, mode, PAIR_DURABLE_WRITER | PAIR_DURABLE_READER | PAIR_REORDER, NULL);
    memset(mixed_index, 0, sizeof(mixed_index));
    memset(&mixed_cache, 0, sizeof(mixed_cache));
    mixed_cache.index = mixed_index;
    mixed_cache.capacity = MIXED_RING;
    mixed_cache.depth = MIXED_RING;
    mixed_cache.arena = mixed_arena;
    mixed_cache.arena_size = (uint32_t)sizeof(mixed_arena);
    mixed_cache.sample_depth = mode == RELIABLE_KEEP_LAST ? depth : 0;
    pub.reliable_cache = &mixed_cache;
}

static void mixed_backlog(const char* mix, const uint32_t* sizes, int count, enum pair_mode mode, uint16_t depth,
                          uint32_t loss) {
    mixed_pair(sizes[0], mode, depth);
    unmatch_reader();
    sim_start(loss);

    // Unmatched: every sample is kept and none is sent - at the publish, nor while the writer's clock runs a second
    // (the no-send rule). Only the writer runs, and what it sends reaches nobody: the reader is not there yet.
    sim_acting = SENDER_ID;
    for (int i = 0; i < count; i++) {
        sample_len = sizes[i];
        sample_seed = (uint8_t)(i + 1);
        len_of_seed[i + 1] = sizes[i];
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    }
    uint64_t until = test_mock_now + tt_SECOND;
    uint64_t next = 0;
    while (sched_next_time(&sender, &next) && next < until) {
        test_mock_now = next > test_mock_now ? next : test_mock_now;
        bool has_next = false;
        while (run_due_entry(&sender, test_mock_now, &has_next, &next)) {
        }
    }
    test_mock_now = until;
    EXPECT_TRUE(publisher_unmatched(&pub));
    EXPECT_EQ_U32(0, sample_datagrams_sent(0, sim_tail, NULL));
    sim_head = sim_tail;

    // What the history holds: KEEP_LAST the newest `depth`, KEEP_ALL every one.
    int first = mode == RELIABLE_KEEP_LAST && depth < count ? count - depth : 0;
    int expected = count - first;
    uint32_t expected_datagrams = 0;
    uint32_t evicted_datagrams = 0;
    for (int i = 0; i < count; i++) {
        *(i < first ? &evicted_datagrams : &expected_datagrams) += tt_sample_datagrams(sizes[i]);
    }

    // The match: the reader's announce, as discovery delivers it. The writer sends the backlog, then its initial
    // Heartbeat, all from the announce's processing - before the reader has said anything more.
    uint32_t backlog_from = sim_tail;
    uint64_t match_by = test_mock_now + tt_SECOND;
    while (publisher_unmatched(&pub) && test_mock_now < match_by && sim_step()) {
    }
    EXPECT_TRUE(!publisher_unmatched(&pub));

    // The backlog left in publish order: every sample datagram's seq_no above the one before, all of the history's.
    bool ascending = true;
    uint32_t backlog_datagrams = sample_datagrams_sent(backlog_from, sim_tail, &ascending);
    EXPECT_TRUE(ascending);
    EXPECT_EQ_U32(expected_datagrams, backlog_datagrams);

    sim_drain();
    sim_settle(expected, 10 * tt_SECOND);
    uint64_t quiet_by = test_mock_now + tt_SECOND; // the last acknowledgements, and anything a mutant still sends
    while (test_mock_now < quiet_by && sim_step()) {
    }
    printf("test_large_sample: mixed durable backlog %s, %s depth %u, %u/1000 loss - %d of %d delivered (seeds", mix,
           mode == RELIABLE_KEEP_ALL ? "KEEP_ALL" : "KEEP_LAST", depth, loss, delivered_count, expected);
    for (int i = 0; i < delivered_count && i < 64; i++) {
        printf(" %u", delivered_seeds[i]);
    }
    printf("), backlog %u datagrams %s, %d torn, gaps abandoned %u evicted %u, large abandoned %lu\n",
           backlog_datagrams, ascending ? "in order" : "OUT OF ORDER", delivered_torn, sub.gap_abandoned,
           sub.gap_evicted, (unsigned long)receiver.large.abandoned);
    EXPECT_EQ_INT(expected, delivered_count);
    for (int i = 0; i < expected && i < delivered_count && i < 64; i++) {
        EXPECT_EQ_INT(first + i + 1, delivered_seeds[i]); // the history, once each, in publish order
    }
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_TRUE(in_order);
    // Nothing given up on the reader's own account (KEEP_LAST repair, DESIGN.md section 6): a seq_no is dropped only on
    // the writer's word that it is gone, and that is said only of the samples the depth evicted - none at all when the
    // whole mix is held.
    EXPECT_EQ_U32(0, sub.gap_abandoned);
    EXPECT_TRUE(sub.gap_evicted <= evicted_datagrams);
    EXPECT_EQ_U64(0, receiver.large.abandoned);
    if (loss != 0) {
        EXPECT_TRUE(sim_dropped[SENDER_ID] > 0); // treatment: the link lost some of it
    }
}

// KEEP_LAST holds its depth across both kinds from the moment a sample is published, not from when its send ends: with
// depth 2, L1 S2 and then L3 still going out, the history - what a HEARTBEAT announces, and what a reader joining now
// would be handed - begins at S2. Until 2026-10-16 L1 stayed beside them until L3 had gone.
static void keep_last_depth_holds_while_a_large_sample_goes_out(void) {
    mixed_pair(ONE_MB, RELIABLE_KEEP_LAST, 2);
    sim_start(0);
    static const uint32_t sizes[] = {ONE_MB, SMALL_ONE, ONE_MB};
    for (int i = 0; i < 2; i++) {
        sample_len = sizes[i];
        len_of_seed[i + 1] = sizes[i];
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), tt_SECOND));
    }
    sim_room_per_step = 64;
    test_mock_nonblocking_room = 64;
    sample_len = sizes[2];
    len_of_seed[3] = sizes[2];
    sample_seed = 3;
    sim_acting = SENDER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_TRUE(pub.large_cursor.active); // treatment: L3 is still going out
    uint32_t oldest = 0;
    EXPECT_TRUE(publisher_oldest_seq_no(&pub, &oldest));
    EXPECT_EQ_U32(tt_sample_datagrams(ONE_MB) + 1U, oldest); // S2's seq_no
    EXPECT_EQ_U32(2, mixed_cache.retained_samples + pub.large_count);
    // Control: the matched reader still gets every sample, in order.
    sim_drain();
    sim_settle(3, 10 * tt_SECOND);
    EXPECT_EQ_INT(3, delivered_count);
    EXPECT_TRUE(in_order);
    EXPECT_EQ_INT(0, delivered_torn);
}

// And the other way round: a sample published while the oldest is a large one still going out cannot evict it, so the
// history passes its depth by one until that send ends - and then comes back to it. Depth 2: L1 going out, S2 and S3
// published meanwhile; once L1 has gone the history is S2 S3.
static void keep_last_depth_comes_back_when_a_large_send_ends(void) {
    mixed_pair(ONE_MB, RELIABLE_KEEP_LAST, 2);
    sim_start(0);
    sim_room_per_step = 64;
    test_mock_nonblocking_room = 64;
    sim_acting = SENDER_ID;
    static const uint32_t sizes[] = {ONE_MB, SMALL_ONE, SMALL_ONE};
    for (int i = 0; i < 3; i++) {
        sample_len = sizes[i];
        len_of_seed[i + 1] = sizes[i];
        sample_seed = (uint8_t)(i + 1);
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    }
    EXPECT_TRUE(pub.large_cursor.active);                             // treatment: L1 still going out
    EXPECT_EQ_U32(3, mixed_cache.retained_samples + pub.large_count); // past the depth by the one being sent
    sim_drain();
    sim_settle(3, 10 * tt_SECOND);
    EXPECT_EQ_INT(3, delivered_count);
    EXPECT_TRUE(!pub.large_cursor.active);
    uint32_t oldest = 0;
    EXPECT_TRUE(publisher_oldest_seq_no(&pub, &oldest));
    EXPECT_EQ_U32(tt_sample_datagrams(ONE_MB) + 1U, oldest); // S2's seq_no
    EXPECT_EQ_U32(2, mixed_cache.retained_samples + pub.large_count);
}

static void test_a_durable_backlog_interleaving_large_and_small_samples(void) {
    keep_last_depth_holds_while_a_large_sample_goes_out();
    keep_last_depth_comes_back_when_a_large_send_ends();
    static const uint32_t small_first[] = {SMALL_ONE, LARGE_100K, SMALL_FRAG, SMALL_ONE, ONE_MB, SMALL_ONE};
    static const uint32_t large_first[] = {ONE_MB, SMALL_ONE, SMALL_FRAG, LARGE_100K};
    static const uint32_t losses[] = {0, 50};
    for (size_t l = 0; l < sizeof(losses) / sizeof(losses[0]); l++) {
        mixed_backlog("S L S S L S", small_first, 6, RELIABLE_KEEP_LAST, 16, losses[l]); // depth above the mix
        mixed_backlog("S L S S L S", small_first, 6, RELIABLE_KEEP_LAST, 3, losses[l]);  // and below it
        mixed_backlog("S L S S L S", small_first, 6, RELIABLE_KEEP_ALL, 0, losses[l]);
        mixed_backlog("L S S L", large_first, 4, RELIABLE_KEEP_LAST, 16, losses[l]);
        mixed_backlog("L S S L", large_first, 4, RELIABLE_KEEP_LAST, 2, losses[l]);
        mixed_backlog("L S S L", large_first, 4, RELIABLE_KEEP_ALL, 0, losses[l]);
    }
    len_of_seed[0] = 0;
}

// --- Encoding while sending (DESIGN.md section 8): the wire, the order, the retained copy, a range that fails -------

// Every datagram the sender sent in a run, in order: the queue keeps them after delivery.
struct sender_stream {
    uint8_t* bytes;
    uint32_t* lens;
    uint32_t count;
    size_t total;
};

static void capture_sender(struct sender_stream* out) {
    EXPECT_TRUE(sim_tail < SIM_QUEUE); // nothing overwritten
    out->count = 0;
    out->total = 0;
    out->lens = (uint32_t*)malloc(sizeof(uint32_t) * SIM_QUEUE);
    out->bytes = (uint8_t*)malloc((size_t)SIM_DATAGRAM_MAX * SIM_QUEUE);
    if (out->lens == NULL || out->bytes == NULL) {
        return;
    }
    for (uint32_t i = 0; i < sim_tail && i < SIM_QUEUE; i++) {
        if (sim_queue[i].from == SENDER_ID) {
            out->lens[out->count++] = sim_queue[i].len;
            memcpy(out->bytes + out->total, sim_queue[i].bytes, sim_queue[i].len);
            out->total += sim_queue[i].len;
        }
    }
}

static void stream_free(struct sender_stream* stream) {
    free(stream->bytes);
    free(stream->lens);
}

// Two samples of `size` (seeds 1 and 2) through the pair, with `room` datagrams of send buffer a step (0: unlimited)
// and `loss` per mille both ways, run until both are delivered; the sender's datagrams captured. `decline_at` and
// `whole_fails` set the ranged encode's failure points for the run.
static void run_and_capture(uint32_t size, enum pair_mode mode, uint32_t room, uint32_t loss, bool use_ranges,
                            uint32_t decline_at, struct sender_stream* out) {
    ranged = use_ranges;
    init_pair(size, mode);
    range_decline_at = decline_at;
    sim_start(loss);
    sim_room_per_step = room;
    for (int i = 0; i < 2; i++) {
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 1), 10 * tt_SECOND));
        sim_settle(i + 1, 10 * tt_SECOND);
    }
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    capture_sender(out);
    ranged = false;
}

static bool streams_equal(const struct sender_stream* a, const struct sender_stream* b) {
    return a->count == b->count && a->total == b->total && memcmp(a->lens, b->lens, sizeof(uint32_t) * a->count) == 0 &&
           memcmp(a->bytes, b->bytes, a->total) == 0;
}

static void expect_same_wire(uint32_t size, enum pair_mode mode, uint32_t room, uint32_t loss, uint32_t decline_at) {
    struct sender_stream whole;
    struct sender_stream by_range;
    run_and_capture(size, mode, room, loss, false, UINT32_MAX, &whole);
    uint64_t whole_waits = sender.large.send_waits;
    run_and_capture(size, mode, room, loss, true, decline_at, &by_range);
    EXPECT_TRUE(range_call_count >= 1); // the ranged encode was asked: the arm really had it
    EXPECT_TRUE(whole.count > 2U * tt_sample_datagrams(size) - 1U);
    EXPECT_TRUE(streams_equal(&whole, &by_range));
    EXPECT_EQ_U64(whole_waits, sender.large.send_waits); // and the sends were cut short where they were before
    EXPECT_EQ_U64(decline_at == 0 ? 0 : 2, sender.large.encoded_by_range);
    if (!streams_equal(&whole, &by_range)) {
        printf("test_large_sample: wire differs at %u B mode %d room %u loss %u decline %u: %u vs %u datagrams\n", size,
               (int)mode, room, loss, decline_at, whole.count, by_range.count);
    }
    stream_free(&whole);
    stream_free(&by_range);
}

static void test_encoding_while_sending_leaves_the_wire_byte_identical(void) {
    // Every datagram the sender sends - fragments, HEARTBEATs and repairs - is the same byte for byte, in the same
    // order, whether the sample was encoded whole first or a send call at a time: with the socket taking everything,
    // with a send buffer of 44 datagrams (so most of each sample goes from the cursor, from the retained buffer), and
    // RELIABLE at 5% loss (repairs from the retained buffer). The control is the arm without a ranged encode; time
    // fields come from the mock clock, so they match only if the two runs behave alike.
    static const uint32_t sizes[] = {65508, SIZE_256_FRAGMENTS, ONE_MB + 3, FOUR_MB};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        expect_same_wire(sizes[i], BEST_EFFORT, 0, 0, UINT32_MAX);
        expect_same_wire(sizes[i], RELIABLE_KEEP_LAST, 0, 0, UINT32_MAX);
        expect_same_wire(sizes[i], RELIABLE_KEEP_ALL, 65536 / 1472, 0, UINT32_MAX);
    }
    expect_same_wire(ONE_MB, RELIABLE_KEEP_LAST, 65536 / 1472, 50, UINT32_MAX);
    expect_same_wire(FOUR_MB, BEST_EFFORT, 65536 / 1472, 0, UINT32_MAX);
    // A topic that declines from the first range is encoded whole; one that declines part-way is finished by
    // data_encode - the same bytes either way.
    expect_same_wire(ONE_MB, RELIABLE_KEEP_LAST, 0, 0, 0);
    expect_same_wire(ONE_MB, RELIABLE_KEEP_LAST, 0, 0, 200000);
    expect_same_wire(ONE_MB, BEST_EFFORT, 65536 / 1472, 0, 200000);
}

static void test_each_range_is_encoded_just_before_its_datagrams_leave(void) {
    // The point of it: the first datagram leaves after one fragment's encoding, and each later range is encoded just
    // before the send call that carries it - never more than one call (LARGE_BATCH datagrams) ahead of what has gone.
    // The ranges cover the sample once, in order. Mutants: the sample encoded whole at the first range, or all of it
    // at the first send call - each fails the "just before" bound.
    ranged = true;
    init_pair(FOUR_MB, BEST_EFFORT);
    sim_start(0);
    sample_seed = 1;
    sim_acting = SENDER_ID;
    uint64_t before = sim_sent[SENDER_ID];
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    sim_drain();
    ranged = false;
    sim_settle(1, tt_SECOND);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    uint32_t count = tt_sample_datagrams(FOUR_MB);
    EXPECT_TRUE(range_call_count > count / LARGE_BATCH); // one per send call, and the first fragment's
    EXPECT_TRUE(range_call_count < RANGE_CALLS_MAX);
    if (range_call_count < 2 || range_call_count >= RANGE_CALLS_MAX) {
        return;
    }
    EXPECT_EQ_U32(0, range_calls[0].offset);
    EXPECT_EQ_U32(LARGE_FIRST_PAYLOAD, range_calls[0].length);
    EXPECT_EQ_U64(before, range_calls[0].sent_before);
    uint32_t end = 0;
    uint32_t ahead_most = 0;
    for (uint32_t i = 0; i < range_call_count; i++) {
        EXPECT_EQ_U32(end, range_calls[i].offset); // contiguous, once each
        end = range_calls[i].offset + range_calls[i].length;
        uint32_t sent = (uint32_t)(range_calls[i].sent_before - before);
        uint32_t covered = sent >= count ? FOUR_MB : large_offset(sent); // bytes already on their way
        uint32_t ahead = end - covered;
        ahead_most = ahead > ahead_most ? ahead : ahead_most;
    }
    EXPECT_EQ_U32(FOUR_MB, end);
    EXPECT_EQ_U64(1, sender.large.encoded_by_range); // the treatment check an A/B reads
    EXPECT_TRUE(ahead_most <= LARGE_BATCH * LARGE_CONT_PAYLOAD);
    EXPECT_TRUE(range_calls[range_call_count - 1].sent_before - before >=
                count - LARGE_BATCH); // the last, near the end
    printf(
        "test_large_sample: 4 MB encoded in %u ranges, at most %u B ahead of the datagrams sent; the first send call "
        "went after %u B of %u\n",
        range_call_count, ahead_most, range_calls[0].length + range_calls[1].length, FOUR_MB);
}

static void test_a_full_send_buffer_retains_the_whole_encoded_sample(void) {
    // A send buffer of 44 datagrams: the publish returns with most of a 4 MB sample unsent. Its record must already be
    // the whole encoded sample - the caller's sample is not ours after the call - so the rest is encoded the moment the
    // buffer fills (while it drains), and resends under 5% loss come from it. The caller then reuses its sample (a new
    // seed), and the reader still gets the published one, whole. Mutant: the rest left unencoded when the buffer fills
    // - the record is not whole when the publish returns, and the reader gets a torn sample.
    ranged = true;
    init_pair(FOUR_MB, RELIABLE_KEEP_LAST);
    sim_start(50);
    sim_room_per_step = 65536 / 1472;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    ranged = false;
    EXPECT_EQ_U32(1, pub.large_count);
    const struct tt_LargeRecord* record = large_record_at(&pub, 0);
    EXPECT_TRUE(!record->sent); // still going out
    EXPECT_EQ_U32(record->cdr_len, record->encoded);
    EXPECT_TRUE(payload_intact(record->buffer + tt_LARGE_CDR_OFFSET, record->cdr_len, FOUR_MB));
    bool encoded_while_full = false;
    for (uint32_t i = 0; i < range_call_count && i < RANGE_CALLS_MAX; i++) {
        encoded_while_full = encoded_while_full || range_calls[i].buffer_full;
    }
    EXPECT_TRUE(encoded_while_full);
    uint32_t calls = range_call_count;
    sample_seed = 77; // the caller's sample changes
    sim_settle(1, 10 * tt_SECOND);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_INT(1, delivered_seeds[0]);
    EXPECT_EQ_U32(calls, range_call_count);  // nothing encoded after the publish returned
    EXPECT_TRUE(sim_dropped[SENDER_ID] > 0); // and there were repairs to make
}

static void test_a_sample_waiting_behind_a_send_is_encoded_whole(void) {
    // A sample published while another is going out is not sent within its publish, so it is encoded whole there; only
    // the first was encoded by range. Both arrive whole and in order.
    ranged = true;
    init_pair(ONE_MB, RELIABLE_KEEP_LAST);
    sim_start(0);
    sim_room_per_step = 64;
    sim_acting = SENDER_ID;
    test_mock_nonblocking_room = 64;
    sample_seed = 1;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    uint32_t first_calls = range_call_count;
    sample_seed = 2;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len)); // waits behind the first
    ranged = false;
    EXPECT_TRUE(first_calls >= 2);
    EXPECT_EQ_U32(first_calls, range_call_count);
    EXPECT_EQ_U64(1, sender.large.encoded_by_range); // the waiting one was not
    sim_drain();
    sim_settle(2, tt_SECOND);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_INT(1, delivered_seeds[0]);
    EXPECT_EQ_INT(2, delivered_seeds[1]);
}

static void unfinished_sample_is_dropped(enum pair_mode mode, uint32_t room) {
    ranged = true;
    init_pair(ONE_MB, mode);
    sim_start(0);
    sim_room_per_step = room;
    range_decline_at = 200000;
    whole_encode_fails = true;
    EXPECT_EQ_INT(tt_RET_PROTOCOL_ERROR, sim_publish(1, tt_SECOND));
    EXPECT_EQ_U64(1, sender.large.encode_failed);
    EXPECT_EQ_U32(0, pub.large_count);
    EXPECT_EQ_U32(0, sender_alloc.live);  // its buffer went back
    EXPECT_TRUE(sim_sent[SENDER_ID] > 0); // part of it had gone
    range_decline_at = UINT32_MAX;
    whole_encode_fails = false;
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(2, tt_SECOND));
    ranged = false;
    sim_settle(1, 10 * tt_SECOND);
    for (int i = 0; i < 20; i++) { // and long enough for every repair of the dropped one to be asked for and refused
        if (!sim_step()) {
            break;
        }
    }
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_INT(0, delivered_torn);
    EXPECT_EQ_INT(2, delivered_seeds[0]);
}

static void test_a_sample_that_cannot_be_finished_is_dropped_never_torn(void) {
    // The ranged encode fails part-way and data_encode cannot make it up: what went cannot be recalled, so the rest is
    // not sent, the record goes (its buffer back), the publish says so, and no reader ever delivers the torn sample -
    // the next one arrives whole. Mutants: the failure ignored (the send goes on with bytes never encoded), or the
    // record kept and served to repairs - each delivers a torn sample.
    unfinished_sample_is_dropped(BEST_EFFORT, 0);
    unfinished_sample_is_dropped(RELIABLE_KEEP_LAST, 0);
    unfinished_sample_is_dropped(RELIABLE_KEEP_LAST, 65536 / 1472);
}

// Every test above, with and without the ranged encode on the sender's topic: what each checks of the large path -
// round trips, loss, the cursor, KEEP_LAST, retention, lending - holds with the sample encoded while it is sent.
static void run_path_tests(void) {
    test_large_samples_cross_2_32_as_if_they_had_not();
    test_a_durable_backlog_of_large_samples_across_2_32();
    test_best_effort_abandons_older_samples_across_2_32();
    test_round_trip_at_every_boundary();
    test_a_fixed_size_type_above_a_sample_is_a_large_topic();
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
    test_large_in_flight_answers_and_wakes_once();
    test_a_retained_large_sample_outlives_the_next_one();
    test_a_volatile_writer_hands_acknowledged_buffers_back();
    test_capacity_large_retained_is_the_records_kept();
    test_a_durable_writer_backs_a_late_joiner_with_its_large_samples();
    test_a_late_joiner_gets_a_large_backlog_from_a_writer_far_past_1();
    test_a_durable_backlog_interleaving_large_and_small_samples();
}

// WIFI repair timing (tt_LinkOptions.repair_timing, 2026-10-23) times a recovery for the path the way
// repair_timing_times() says: the seed, then Karn's rule - but not for a writer of large samples, whose recoveries
// nearly all wait out a retry behind the rest of the send; under Karn's rule its estimate stayed at the seed and the WB
// 1 MiB arms asked by timer up to 100 times as often as ETHERNET (one rep resent 208,479 units). Such a writer keeps
// the default rule past the seed. The control is the small writer on the same proxy state.
static void test_wifi_repair_timing_keeps_the_default_rule_for_a_large_writer(void) {
    static struct tt_Context ctx;
    static struct tt_WriterProxy proxy;
    memset(&ctx, 0, sizeof(ctx));
    memset(&proxy, 0, sizeof(proxy));
    ctx.opts.reliable.retry = tt_RELIABLE_RETRY;
    proxy.retry = tt_RELIABLE_RETRY + 1U;
    EXPECT_TRUE(repair_timing_times(&ctx, &proxy)); // no estimate: the seed, past the back-off
    proxy.recovery_srtt_ns = 5000000U;
    proxy.retry = 1;
    EXPECT_TRUE(!repair_timing_times(&ctx, &proxy)); // a small writer: Karn's rule, a retried recovery is not timed
    proxy.large_arrival_ns = 1;
    EXPECT_TRUE(repair_timing_times(&ctx, &proxy)); // a large writer: within the plain retries, timed
    proxy.retry = tt_RELIABLE_RETRY + 1U;
    EXPECT_TRUE(!repair_timing_times(&ctx, &proxy)); // and past the back-off, not
}

int main(void) {
    ranged = false;
    run_path_tests();
    ranged = true;
    run_path_tests();
    ranged = false;
    test_encoding_while_sending_leaves_the_wire_byte_identical();
    test_each_range_is_encoded_just_before_its_datagrams_leave();
    test_a_full_send_buffer_retains_the_whole_encoded_sample();
    test_a_sample_waiting_behind_a_send_is_encoded_whole();
    test_a_sample_that_cannot_be_finished_is_dropped_never_torn();
    test_a_heartbeat_announces_no_sample_still_going_out();
    test_a_busy_writer_is_not_warned_about();
    test_a_busy_writers_send_is_not_timed_as_the_path();
    test_wifi_repair_timing_keeps_the_default_rule_for_a_large_writer();
    test_a_quiet_paths_round_trip_is_still_timed();
    test_a_repair_arriving_first_is_not_a_writer_still_sending();
    test_a_repair_shown_lost_by_a_later_one_is_asked_for_at_once();
    test_reveals_are_paced_by_the_least_transit();
    test_a_reveal_names_only_what_it_reveals();
    test_a_sending_writer_is_not_asked_for_repairs_on_their_way();
    test_two_readers_are_not_resent_repairs_on_their_way();
    test_a_heartbeat_takes_the_path_of_its_data();
    test_a_slow_readers_repairs_are_measured();
    test_a_repairing_writer_is_not_warned_about();
    test_repairs_passed_by_an_acknack_naming_nothing_are_freed();
    test_a_queued_path_does_not_storm();
    test_a_whole_sample_lost_does_not_storm();
#ifdef tt_LARGE_REPAIR_SLOTS
    test_a_stalled_reader_does_not_storm();
#endif
    test_a_repair_on_its_way_is_not_resent();
#ifdef tt_LARGE_REPAIR_SLOTS
    test_the_resend_order_rules();
    test_a_tail_waits_rack_rtt_and_its_reordering_window();
    test_the_repair_timeout_waits_while_the_reader_progresses();
    test_the_repair_timeout_backs_off_while_the_reader_shows_nothing();
    test_the_repair_timeout_never_stands_on_less_than_its_path();
#endif
    teardown_pair();

    allocator_reset(&sender_alloc);
    allocator_reset(&receiver_alloc);
    allocator_reset(&reader_b_alloc);
    free(sim_queue);
    if (test_result() != 0) {
        return 1;
    }
    printf("test_large_sample: all tests passed\n");
    return 0;
}
