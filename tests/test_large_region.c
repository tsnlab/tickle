/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Large-message step B (DESIGN.md section 8, "Step B", pre-registration B1): a same-host large sample goes into a chunk
// of its Publisher's shared region, and the reader's ring gets one LARGE_REF descriptor whose seq_span is the sample's
// datagram count. Each test names the B1 item it checks; tests/mutants_large_region.py breaks each rule and names the
// test that must fail.
//
// Two contexts in one process, as test_large_shm.c has them: the owner has a ring and the Subscribers, the sender a
// Publisher whose one peer is the owner. The mock's segments are heap memory found by name, so the region the sender
// creates is the region the owner maps - the same bytes, as two real mappings of one file would be.
#define tt_MAX_BUFFER_LENGTH 65507 // as rmw_tickle builds core
#define tt_LARGE_SAMPLES 1

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

_Static_assert(tt_LARGE_SHM, "step B needs the segment and large samples both");

#define ENDPOINT_ID 0xaabbccddU
#define OWNER_IP 0x0a4d0002
#define OWNER_PORT 8282
#define OWNER_ID 7
#define SENDER_IP 0x0a4d0003
#define SENDER_PORT 7000
#define SENDER_ID 1
#define ONE_MB 1048576U
#define FOUR_MB (4U * ONE_MB)
#define EIGHT_MIB (8U * ONE_MB)
#define SMALLEST_LARGE ((uint32_t)tt_MAX_SAMPLE_LENGTH + 1U)
// A sample whose fragments fit the 512-slot ring (207 of them), for the tests where it gets fragments.
#define FRAGMENTED 300000U

// --- the caller's buffers -----------------------------------------------------------------------------------------

struct allocator {
    uint32_t acquires;
    uint32_t live;
};
static struct allocator sender_alloc;
static struct allocator owner_alloc;

static void* test_acquire(void* user, uint32_t bytes) {
    struct allocator* alloc = (struct allocator*)user;
    alloc->acquires++;
    uint8_t* block = (uint8_t*)malloc((size_t)bytes + 16U);
    if (block == NULL) {
        return NULL;
    }
    alloc->live++;
    return block + 16;
}
static void test_release(void* user, void* buffer) {
    struct allocator* alloc = (struct allocator*)user;
    alloc->live--;
    free((uint8_t*)buffer - 16);
}

// --- the sample ---------------------------------------------------------------------------------------------------

struct sample {
    uint32_t id;
    uint32_t size;
};
static uint8_t pattern(uint32_t i, uint32_t id) {
    return (uint8_t)((i * 7U) + (id * 31U) + (i >> 8U));
}
static int32_t sample_size(struct tt_Data* data) {
    return (int32_t)((const struct sample*)data)->size;
}
static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    const struct sample* sample = (const struct sample*)data;
    if (len < sample->size) {
        return -1;
    }
    for (uint32_t i = 0; i < sample->size; i++) {
        payload[i] = pattern(i, sample->id);
    }
    return (int32_t)sample->size;
}
static bool intact(const uint8_t* payload, uint32_t id, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        if (payload[i] != pattern(i, id)) {
            return false;
        }
    }
    return true;
}

// --- the Subscribers ----------------------------------------------------------------------------------------------

#define SEEN_MAX 32
struct seen {
    uint32_t count;
    uint32_t ids[SEEN_MAX];
    uint32_t torn;
    bool retain;
    struct tt_Sample held;
    tt_ret_t retain_result;
};
static struct seen seen_reliable;
static struct seen seen_best_effort;
static const uint8_t* decoded_payload;
static uint32_t decoded_length;

// The sample's id is in its bytes: pattern(0, id) = id * 31, and the size is the length.
static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)data;
    (void)native;
    decoded_payload = payload;
    decoded_length = len;
    return 0;
}
static void sample_free(struct tt_Data* data) {
    (void)data;
}

static uint32_t id_of(const uint8_t* payload) {
    for (uint32_t id = 0; id < 256; id++) {
        if (payload[0] == pattern(0, id) && payload[1] == pattern(1, id)) {
            return id;
        }
    }
    return UINT32_MAX;
}

static void record_seen(struct tt_Subscriber* sub, struct seen* seen) {
    uint32_t id = id_of(decoded_payload);
    if (seen->count < SEEN_MAX) {
        seen->ids[seen->count] = id;
    }
    seen->count++;
    if (id == UINT32_MAX || !intact(decoded_payload, id, decoded_length)) {
        seen->torn++;
    }
    if (seen->retain) {
        seen->retain = false;
        seen->retain_result = tt_Sample_retain(sub, &seen->held);
    }
}
static void on_reliable(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)seq_no;
    (void)data;
    record_seen(sub, &seen_reliable);
}
static void on_best_effort(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)seq_no;
    (void)data;
    record_seen(sub, &seen_best_effort);
}

// --- the rig ------------------------------------------------------------------------------------------------------

static struct tt_Context owner;
static struct tt_Context sender;
static struct tt_Topic sender_topic;
static struct tt_Topic owner_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub_reliable;
static struct tt_Subscriber sub_best_effort;

#define CACHE_DEPTH 64
static struct tt_ReliableCacheIndex cache_index[CACHE_DEPTH];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache cache;
#define WIDE_WORDS (tt_RELIABLE_BITMAP_MAX_BITS / 64)
static uint64_t tracking[tt_MAX_PEER_COUNT * WIDE_WORDS];

// The owner's UDP sends (its ACKNACKs), delivered to the sender by pump().
#define BACK_MAX 64
static uint8_t back_bytes[BACK_MAX][1500];
static uint32_t back_len[BACK_MAX];
static uint32_t back_count;
static bool capturing_owner;
static void capture_send(const void* buf, size_t len) {
    if (capturing_owner && back_count < BACK_MAX && len <= sizeof(back_bytes[0])) {
        memcpy(back_bytes[back_count], buf, len);
        back_len[back_count++] = (uint32_t)len;
    }
}

static void init_subscriber(struct tt_Subscriber* sub, bool reliable, tt_SUBSCRIBER_CALLBACK callback) {
    memset(sub, 0, sizeof(*sub));
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = ENDPOINT_ID;
    sub->node = &owner;
    sub->topic = &owner_topic;
    sub->callback = callback;
    sub->reliable = reliable;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    if (reliable) {
        memset(tracking, 0, sizeof(tracking));
        sub->tracking_bitmaps = tracking;
        sub->tracking_words = WIDE_WORDS;
    }
    owner.endpoints[owner.endpoint_count++] = (struct tt_Endpoint*)sub;
    owner.endpoint_index_valid = false;
}

enum rig_subs { RIG_BEST_EFFORT = 1, RIG_RELIABLE = 2, RIG_BOTH = 3 };

// The pair, with a region of `chunks` chunks for samples up to `max_cdr`. RELIABLE: a KEEP_LAST writer with a cache,
// pre-matched on the reader with the wide window, as its announce would.
static void rig_up(enum rig_subs subs, uint32_t chunks, uint32_t max_cdr) {
    test_mock_reset();
    test_mock_now = 100ULL * tt_SECOND;
    test_mock_send_hook = capture_send;
    back_count = 0;
    capturing_owner = false;
    memset(&seen_reliable, 0, sizeof(seen_reliable));
    memset(&seen_best_effort, 0, sizeof(seen_best_effort));
    memset(&sender_alloc, 0, sizeof(sender_alloc));
    memset(&owner_alloc, 0, sizeof(owner_alloc));
    memset(&owner, 0, sizeof(owner));
    memset(&sender, 0, sizeof(sender));
    memset(&pub, 0, sizeof(pub));

    node_init_locks(&owner);
    owner.id = OWNER_ID;
    owner.entity_id_base = 0x12345678U;
    owner.hal.own_ip = OWNER_IP;
    owner.hal.own_port = OWNER_PORT;
    owner.tx_tail = sizeof(struct tt_Header);
    owner.tx_size = sizeof(owner.tx_buffer);
    owner.rx_seq_span = 1;
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    owner.same_host_peer[SENDER_ID] = true;
    owner.same_host_peer_count = 1;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Context_set_large_buffers(&owner, test_acquire, test_release, &owner_alloc));
    memset(&owner_topic, 0, sizeof(owner_topic));
    owner_topic.name = "large_region";
    owner_topic.data_size = sizeof(struct sample);
    owner_topic.data_decode = sample_decode;
    owner_topic.data_free = sample_free;
    if ((subs & RIG_RELIABLE) != 0) {
        init_subscriber(&sub_reliable, true, on_reliable);
    }
    if ((subs & RIG_BEST_EFFORT) != 0) {
        init_subscriber(&sub_best_effort, false, on_best_effort);
    }

    node_init_locks(&sender);
    sender.id = SENDER_ID;
    sender.tx_tail = sizeof(struct tt_Header);
    sender.tx_size = sizeof(sender.tx_buffer);
    sender.hal.own_ip = SENDER_IP;
    sender.hal.own_port = SENDER_PORT;
    EXPECT_EQ_INT((int)tt_RET_OK,
                  (int)tt_Context_set_large_buffers(&sender, test_acquire, test_release, &sender_alloc));
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = "large_region";
    sender_topic.data_size = sizeof(struct sample);
    sender_topic.data_encode_size = sample_size;
    sender_topic.data_encode = sample_encode;
    sender_topic.data_free = sample_free;
    pub.endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub.endpoint.id = ENDPOINT_ID;
    pub.endpoint.entity_id = 0x5a5a5a5aU;
    pub.endpoint.name = "large_region_pub";
    pub.node = &sender;
    pub.topic = &sender_topic;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub.peers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    pub.peers[0].context_id = OWNER_ID;
    pub.peers[0].ip = OWNER_IP;
    pub.peers[0].port = OWNER_PORT;
    sender.endpoints[sender.endpoint_count++] = (struct tt_Endpoint*)&pub;
    if ((subs & RIG_RELIABLE) != 0) {
        memset(cache_index, 0, sizeof(cache_index));
        memset(&cache, 0, sizeof(cache));
        cache.index = cache_index;
        cache.capacity = CACHE_DEPTH;
        cache.depth = CACHE_DEPTH;
        cache.arena = cache_arena;
        cache.arena_size = (uint32_t)sizeof(cache_arena);
        pub.reliable_cache = &cache;
        pub.reliable = true;
        struct tt_PeerAck* ack = claim_peer_ack(&pub, OWNER_ID, sub_reliable.endpoint.entity_id);
        EXPECT_TRUE(ack != NULL);
        if (ack != NULL) {
            ack->tracking_words = WIDE_WORDS;
        }
    }
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Publisher_set_large_region(&pub, chunks, tt_LARGE_CHUNK_BYTES(max_cdr)));
    EXPECT_TRUE(peer_segment(&sender, OWNER_ID, OWNER_IP, OWNER_PORT) != NULL);
}

static void rig_down(void) {
    large_release_publisher(&sender, &pub);
    large_release_assemblies(&owner);
    large_peer_regions_release(&owner);
    release_segments(&sender);
    release_own_segment(&owner);
    test_mock_segments_free();
    test_mock_send_hook = NULL;
}

static struct tt_SegmentHeader* ring(void) {
    return owner.own_segment;
}

static tt_ret_t publish(uint32_t id, uint32_t size) {
    test_mock_now += tt_MILLISECOND; // each newer than the last, or a best-effort reader discards it
    struct sample sample = {id, size};
    return tt_Publisher_publish(&pub, (struct tt_Data*)&sample);
}

static void drain(void) {
    bool emptied = false;
    for (int pass = 0; pass < 64 && owner.own_segment != NULL; pass++) {
        (void)drain_own_segment(&owner, &emptied);
        if (emptied) {
            return;
        }
    }
}

// Both contexts' due timers, the owner's ACKNACKs to the sender and the ring drained, for `ms` milliseconds.
static void pump(uint32_t ms) {
    for (uint32_t step = 0; step < ms * 4U; step++) {
        test_mock_now += tt_MILLISECOND / 4U;
        bool has_next = false;
        uint64_t next = 0;
        while (run_due_entry(&sender, test_mock_now, &has_next, &next)) {
        }
        drain();
        capturing_owner = true;
        while (run_due_entry(&owner, test_mock_now, &has_next, &next)) {
        }
        (void)flush_tx(&owner, owner.tx_tail, NULL, 0);
        capturing_owner = false;
        for (uint32_t i = 0; i < back_count; i++) {
            process_packet(&sender, back_bytes[i], 0, back_len[i], OWNER_IP, OWNER_PORT, tt_TRANSPORT_UDP);
        }
        back_count = 0;
        drain();
    }
}

// The region chunk of the newest record the Publisher holds (1 + its index), 0 for none.
static uint8_t newest_chunk(void) {
    return pub.large_count == 0 ? 0 : large_record_at(&pub, pub.large_count - 1U)->chunk;
}

static bool owner_bit_set(uint32_t chunk) {
    uint64_t word = large_chunk_entry(pub.large_region.mapping, chunk)->readers[OWNER_ID / 64];
    return (word & large_reader_bit(OWNER_ID)) != 0;
}

static uint8_t record_type(uint32_t index) {
    const struct tt_SegmentSlot* slot = (const struct tt_SegmentSlot*)segment_slot(ring(), index);
    const uint8_t* record = (const uint8_t*)(slot + 1);
    return record[0] == tt_SINGLE_MARKER_LE || record[0] == tt_SINGLE_MARKER_BE
               ? ((const struct tt_SingleHeader*)record)->type
               : ((const struct tt_SubmessageHeader*)(record + sizeof(struct tt_Header)))->type;
}

// Records the ring took between write indices `from` and `to` that are descriptors.
static uint32_t ref_records(uint32_t from, uint32_t to) {
    uint32_t refs = 0;
    for (uint32_t i = from; i < to; i++) {
        refs += record_type(i) == tt_SUBMESSAGE_TYPE_LARGE_REF ? 1U : 0U;
    }
    return refs;
}

// Records the ring took that are fragments, between write indices `from` and `to`.
static uint32_t fragment_records(uint32_t from, uint32_t to) {
    uint32_t fragments = 0;
    for (uint32_t i = from; i < to; i++) {
        const struct tt_SegmentSlot* slot = (const struct tt_SegmentSlot*)segment_slot(ring(), i);
        const uint8_t* record = (const uint8_t*)(slot + 1);
        uint8_t type = record[0] == tt_SINGLE_MARKER_LE || record[0] == tt_SINGLE_MARKER_BE
                           ? ((const struct tt_SingleHeader*)record)->type
                           : ((const struct tt_SubmessageHeader*)(record + sizeof(struct tt_Header)))->type;
        if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L || type == tt_SUBMESSAGE_TYPE_FRAG_CONT_L) {
            fragments++;
        }
    }
    return fragments;
}

// ---- B1.1: one record per sample, the socket path's seq space, byte-exact ----

static void round_trip(enum rig_subs subs, uint32_t size) {
    rig_up(subs, 3, EIGHT_MIB);
    uint32_t count = large_count_for(ROUNDUP(size));
    for (uint32_t id = 1; id <= 3; id++) {
        uint32_t before = ring()->write_index;
        EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(id, size));
        EXPECT_TRUE(newest_chunk() != 0 || subs == RIG_BEST_EFFORT);
        // One descriptor and no fragments (RELIABLE also has its end-of-sample HEARTBEAT in the ring).
        EXPECT_EQ_U32(1, ref_records(before, ring()->write_index));
        EXPECT_EQ_U32(0, fragment_records(before, ring()->write_index));
        const struct tt_SegmentSlot* slot = (const struct tt_SegmentSlot*)segment_slot(ring(), before);
        EXPECT_EQ_U32(count, slot->seq_span);
        drain();
        pump(2);
    }
    struct seen* seen = subs == RIG_RELIABLE ? &seen_reliable : &seen_best_effort;
    EXPECT_EQ_U32(3, seen->count);
    EXPECT_EQ_U32(0, seen->torn);
    EXPECT_EQ_U32(size, decoded_length);
    EXPECT_EQ_U32(1, seen->ids[0]);
    EXPECT_EQ_U32(3, seen->ids[2]);
    EXPECT_EQ_U64(3, sender.large.ref_sent);
    EXPECT_EQ_U64(3, owner.large.ref_received);
    EXPECT_EQ_U64(0, owner.large.ref_refused);
    EXPECT_EQ_U32(0, owner_alloc.acquires); // the reader acquired nothing: it read the chunks
    EXPECT_EQ_U32(0, sender_alloc.acquires);
    EXPECT_EQ_U32(3U * count, pub.seq_no); // the socket path's numbering
    if (subs == RIG_RELIABLE) {
        struct tt_WriterProxy* proxy = find_writer_proxy(&sub_reliable, SENDER_ID, pub.endpoint.entity_id);
        EXPECT_TRUE(proxy != NULL);
        if (proxy != NULL) {
            EXPECT_EQ_U32(pub.seq_no + 1U, proxy->ack_seq_no);
        }
    }
    for (uint32_t chunk = 0; chunk < pub.large_region.chunks; chunk++) {
        EXPECT_TRUE(!(owner_bit_set(chunk))); // every chunk given back
    }
    printf("test_large_region: %s %u B: 3 samples, 1 record each (span %u), 0 fragments, intact\n",
           subs == RIG_RELIABLE ? "RELIABLE" : "BEST_EFFORT", (unsigned)size, (unsigned)count);
    rig_down();
}

static void test_round_trip_one_record_per_sample(void) {
    uint32_t sizes[] = {SMALLEST_LARGE, ONE_MB, FOUR_MB, EIGHT_MIB};
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        round_trip(RIG_BEST_EFFORT, sizes[i]);
        round_trip(RIG_RELIABLE, sizes[i]);
    }
}

// ---- B1.2: a chunk a reader holds is not reused ----

static void test_a_retained_chunk_is_not_reused(void) {
    rig_up(RIG_BEST_EFFORT, 3, FOUR_MB);
    seen_best_effort.retain = true;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, FOUR_MB));
    drain();
    EXPECT_EQ_INT((int)tt_RET_OK, (int)seen_best_effort.retain_result);
    EXPECT_TRUE(owner_bit_set(0));
    for (uint32_t id = 2; id <= 6; id++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(id, FOUR_MB));
        drain();
    }
    EXPECT_EQ_U32(6, seen_best_effort.count);
    EXPECT_EQ_U32(0, seen_best_effort.torn);
    EXPECT_TRUE(intact(seen_best_effort.held.payload, 1, FOUR_MB)); // nothing was encoded over it
    EXPECT_TRUE(owner_bit_set(0));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&owner, &seen_best_effort.held));
    EXPECT_TRUE(!(owner_bit_set(0)));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(7, FOUR_MB));
    EXPECT_TRUE(pub.large_region.held == 0); // best effort: the writer keeps nothing once sent
    drain();
    EXPECT_EQ_U32(7, seen_best_effort.count);
    rig_down();
}

// ---- B1.3: two Subscribers of one context share the bit ----

static void test_the_bit_waits_for_every_subscriber(void) {
    rig_up(RIG_BOTH, 3, ONE_MB);
    seen_reliable.retain = true; // the RELIABLE one keeps it; the best-effort one is done at once
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, ONE_MB));
    drain();
    EXPECT_EQ_U32(1, seen_reliable.count);
    EXPECT_EQ_U32(1, seen_best_effort.count);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)seen_reliable.retain_result);
    EXPECT_TRUE(owner_bit_set(0));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&owner, &seen_reliable.held));
    EXPECT_TRUE(!(owner_bit_set(0)));

    seen_best_effort.retain = true; // and the other way round
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(2, ONE_MB));
    uint8_t chunk = newest_chunk();
    EXPECT_TRUE(chunk != 0);
    drain();
    EXPECT_EQ_U32(2, seen_reliable.count);
    EXPECT_EQ_U32(2, seen_best_effort.count);
    EXPECT_TRUE(chunk != 0 && owner_bit_set(chunk - 1U));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&owner, &seen_best_effort.held));
    EXPECT_TRUE(chunk == 0 || !owner_bit_set(chunk - 1U));
    rig_down();
}

// ---- B1.4: a dropped descriptor ----

// Fills the owner's ring with harmless empty records, leaving `room` slots.
static void fill_ring(uint32_t room) {
    while (ring()->write_index - ring()->read_index < ring()->slots - room) {
        EXPECT_TRUE(segment_write(ring(), NULL, 0, NULL, 0, SENDER_IP, SENDER_PORT, 1));
    }
}

static void test_a_dropped_descriptor_frees_its_bit_and_is_resent_whole(void) {
    rig_up(RIG_RELIABLE, 3, FOUR_MB);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, FOUR_MB)); // the reader knows the writer
    drain();
    pump(5);
    EXPECT_EQ_U32(1, seen_reliable.count);
    fill_ring(0);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(2, FOUR_MB));
    uint8_t chunk = newest_chunk();
    EXPECT_EQ_U64(1, sender.large.ref_dropped);
    EXPECT_TRUE(chunk != 0 && !owner_bit_set(chunk - 1U)); // nobody will read it for that descriptor
    drain();                                               // the empty records go; nothing of the sample was there
    EXPECT_EQ_U32(1, seen_reliable.count);
    uint64_t sent = sender.large.ref_sent;
    uint32_t before = ring()->write_index;
    pump(400); // the end-of-sample HEARTBEAT is chased, the reader asks, the writer answers
    EXPECT_EQ_U32(2, seen_reliable.count);
    EXPECT_EQ_U32(2, seen_reliable.ids[1]);
    EXPECT_TRUE(sender.large.ref_sent > sent);
    EXPECT_EQ_U32(0, seen_reliable.torn);
    EXPECT_EQ_U32(0, fragment_records(before, ring()->write_index)); // the answer was a descriptor, not 2,889 records
    EXPECT_TRUE(sender.large.ref_sent >= 1);
    printf("test_large_region: a dropped 4 MB descriptor recovered by %lu resent descriptor(s), %u ring records\n",
           (unsigned long)(sender.large.ref_sent - sent), (unsigned)(ring()->write_index - before));
    rig_down();
}

// ---- B1.5: stale descriptors are refused ----

static void test_a_stale_descriptor_is_refused(void) {
    rig_up(RIG_BEST_EFFORT, 3, ONE_MB);
    // Generation changed after the descriptor went: the chunk was reused under it.
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, ONE_MB));
    struct tt_LargeChunkEntry* entry = large_chunk_entry(pub.large_region.mapping, 0);
    entry->generation++;
    drain();
    EXPECT_EQ_U32(0, seen_best_effort.count);
    EXPECT_EQ_U64(1, owner.large.ref_refused);
    entry->generation--;
    __atomic_fetch_and(&entry->readers[OWNER_ID / 64], ~large_reader_bit(OWNER_ID), __ATOMIC_RELEASE);

    // Its bit not set: the writer gave it up for this reader.
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(2, ONE_MB));
    entry = large_chunk_entry(pub.large_region.mapping, 0);
    __atomic_fetch_and(&entry->readers[OWNER_ID / 64], ~large_reader_bit(OWNER_ID), __ATOMIC_RELEASE);
    drain();
    EXPECT_EQ_U32(0, seen_best_effort.count);
    EXPECT_EQ_U64(2, owner.large.ref_refused);

    // A region recreated under the same name: its incarnation is not the descriptor's.
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(3, ONE_MB));
    ((struct tt_LargeRegionHeader*)pub.large_region.mapping)->incarnation ^= 0x100U;
    large_peer_regions_release(&owner); // not mapped yet with the old incarnation
    drain();
    EXPECT_EQ_U32(0, seen_best_effort.count);
    EXPECT_EQ_U64(3, owner.large.ref_refused);
    ((struct tt_LargeRegionHeader*)pub.large_region.mapping)->incarnation ^= 0x100U;

    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(4, ONE_MB)); // the control: an ordinary one is taken
    drain();
    EXPECT_EQ_U32(1, seen_best_effort.count);
    EXPECT_EQ_U32(4, seen_best_effort.ids[0]);
    rig_down();
}

// ---- B1.6: a dead reader's chunks ----

static void test_a_dead_readers_chunk_is_freed_by_liveliness_not_before(void) {
    rig_up(RIG_BEST_EFFORT, 2, ONE_MB);
    seen_best_effort.retain = true; // the reader holds chunk 0 and then dies holding it
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, ONE_MB));
    drain();
    EXPECT_TRUE(owner_bit_set(0));
    for (uint32_t id = 2; id <= 4; id++) {
        EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(id, ONE_MB));
        drain();
    }
    EXPECT_EQ_U32(4, seen_best_effort.count);
    EXPECT_EQ_U64(3, sender.large.ref_sent - 1U); // chunk 1 every time
    EXPECT_TRUE(owner_bit_set(0));                // not before: only slow, as far as the writer can tell
    EXPECT_TRUE(intact(seen_best_effort.held.payload, 1, ONE_MB));
    presume_node_dead(&sender, OWNER_ID, 5ULL * tt_SECOND);
    EXPECT_TRUE(!(owner_bit_set(0))); // after: free again
    EXPECT_TRUE(large_chunk_unread(pub.large_region.mapping, 0));
    lend_forget_all(&owner);
    rig_down();
}

// ---- B1.7: a reader without the feature, and a descriptor from a socket ----

static void test_a_reader_without_the_feature_gets_fragments(void) {
    rig_up(RIG_BEST_EFFORT, 3, ONE_MB);
    ring()->features = 0;
    uint32_t before = ring()->write_index;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, FRAGMENTED));
    uint32_t count = large_count_for(FRAGMENTED);
    EXPECT_EQ_U32(count, ring()->write_index - before);
    EXPECT_EQ_U32(count, fragment_records(before, ring()->write_index));
    EXPECT_EQ_U64(0, sender.large.ref_sent);
    drain();
    EXPECT_EQ_U32(1, seen_best_effort.count);
    EXPECT_EQ_U32(0, seen_best_effort.torn);
    rig_down();
}

static void test_a_descriptor_from_a_socket_is_refused(void) {
    rig_up(RIG_BEST_EFFORT, 3, ONE_MB);
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, ONE_MB));
    // The record the ring got, sent to the owner's socket instead.
    const struct tt_SegmentSlot* slot = (const struct tt_SegmentSlot*)segment_slot(ring(), ring()->read_index);
    uint8_t copy[256];
    uint32_t len = slot->length;
    EXPECT_TRUE(len <= sizeof(copy));
    memcpy(copy, slot + 1, len);
    uint64_t refused = owner.rx_shm_only_on_socket;
    process_packet(&owner, copy, 0, len, SENDER_IP, SENDER_PORT, tt_TRANSPORT_UDP);
    EXPECT_EQ_U64(refused + 1U, owner.rx_shm_only_on_socket);
    EXPECT_EQ_U32(0, seen_best_effort.count);
    EXPECT_EQ_U64(0, owner.large.ref_received);
    drain(); // and the same record from the ring is taken
    EXPECT_EQ_U32(1, seen_best_effort.count);
    rig_down();
}

// ---- B1.8: the region exhausted ----

static void test_an_exhausted_region_falls_back_to_fragments(void) {
    rig_up(RIG_BEST_EFFORT, 1, ONE_MB);
    seen_best_effort.retain = true; // the one chunk held by the reader
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, ONE_MB));
    drain();
    uint32_t before = ring()->write_index;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(2, FRAGMENTED));
    EXPECT_EQ_U64(1, sender.large.region_full);
    EXPECT_EQ_U32(large_count_for(FRAGMENTED), fragment_records(before, ring()->write_index));
    drain();
    EXPECT_EQ_U32(2, seen_best_effort.count);
    EXPECT_EQ_U32(0, seen_best_effort.torn);
    EXPECT_TRUE(intact(seen_best_effort.held.payload, 1, ONE_MB));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)tt_Sample_release(&owner, &seen_best_effort.held));
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(3, ONE_MB)); // the chunk again once given back
    EXPECT_EQ_U64(2, sender.large.ref_sent);
    drain();
    EXPECT_EQ_U32(3, seen_best_effort.count);
    rig_down();
}

// A sample above the chunk goes as fragments too, and is counted.
static void test_a_sample_above_the_chunk_goes_as_fragments(void) {
    rig_up(RIG_BEST_EFFORT, 2, SMALLEST_LARGE);
    uint32_t before = ring()->write_index;
    EXPECT_EQ_INT((int)tt_RET_OK, (int)publish(1, FRAGMENTED));
    EXPECT_EQ_U64(1, sender.large.region_full);
    EXPECT_EQ_U32(large_count_for(FRAGMENTED), fragment_records(before, ring()->write_index));
    drain();
    EXPECT_EQ_U32(1, seen_best_effort.count);
    rig_down();
}

int main(void) {
    test_round_trip_one_record_per_sample();
    test_a_retained_chunk_is_not_reused();
    test_the_bit_waits_for_every_subscriber();
    test_a_dropped_descriptor_frees_its_bit_and_is_resent_whole();
    test_a_stale_descriptor_is_refused();
    test_a_dead_readers_chunk_is_freed_by_liveliness_not_before();
    test_a_reader_without_the_feature_gets_fragments();
    test_a_descriptor_from_a_socket_is_refused();
    test_an_exhausted_region_falls_back_to_fragments();
    test_a_sample_above_the_chunk_goes_as_fragments();
    printf("test_large_region: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
