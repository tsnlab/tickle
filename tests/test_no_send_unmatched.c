/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A Publisher with no matched reader sends nothing on the wire (the user's decision, 2026-10-15; DESIGN.md section 4),
// as a DDS writer does. Until then "no known peer" was a broadcast to the whole segment. What is held here:
// - a writer nobody reads puts no datagram on the wire for any sample size or path - small, fragmented, a large sample
//   above 64 KB (ranged encode included), the zero-copy path, a batching publisher, the periodic Heartbeat, a match
//   Heartbeat still owed - BEST_EFFORT counting the sample as dropped, RELIABLE and durable as kept, seq_nos taken
//   either way. Each arm has a control: the same publish with matched readers does send, so a count of 0 can fail;
// - a VOLATILE reader that joins later gets the samples from its match point on, and none from before;
// - a TRANSIENT_LOCAL reader that joins later gets the retained history (KEEP_LAST), small and large, then the rest;
// - a reader that knows the writer before the writer knows it gets data from the moment the writer learns it: its
//   first sample is the first published after its announce arrived. Measured here in the mock's time: one flush tick
//   (tt_CONTEXT_TX_INTERVAL) after the Subscriber is created when its announce gets through; with that announce lost,
//   the writer learns it from the reader's next summary and the request it answers - within one
//   tt_CONTEXT_UPDATE_INTERVAL - and from then on the same;
// - the risk-check cases (DESIGN.md section 4): neither context keeps a discovery table (core's default) and the reader
//   is still learned; a reader that does not fit a full peers[] still gets the broadcast; a live reader presumed gone
//   (its traffic lost past the liveliness limit) gets data again once its next summary arrives.
// Mutants, each killed here (tests/mutants_no_send_unmatched.py): publisher_unmatched() always false; the staging
// path's unmatched return removed; the zero-copy path's; the large path's empty destination set; the periodic
// Heartbeat's gate; the match Heartbeat's gate; the large ranged-encode refusal; the dropped/kept counters swapped.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1 // as rmw_tickle builds core: the large path is compiled in

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

#define WRITER_ID 1
#define READER_ID 2
#define WRITER_IP 0x0a000001U
#define READER_IP 0x0a000002U
#define PORT 8282
#define QUEUE 512
#define DEPTH 64
#define REORDER_SLOTS 64
#define REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + 64)
#define MAX_DELIVERED 256

#define SMALL 16U
#define FRAGMENTED 8000U // above one datagram, below 64 KB: DATA_FRAG
#define LARGE 100000U    // above tt_MAX_SAMPLE_LENGTH: the large path
#define ZEROCOPY 65500U  // a 4-aligned in-place body that fills a datagram: publish_zerocopy()

// --- samples: an id, then bytes derived from it, so a torn or misplaced byte shows -----------------------------

static uint32_t sample_len = SMALL;
static uint32_t sample_id;

static uint8_t sample_byte(uint32_t id, uint32_t index) {
    if (index < sizeof(id)) {
        return (uint8_t)(id >> (8U * index));
    }
    return (uint8_t)(id + (index * 7U));
}

static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}
static int32_t sample_encode_range(struct tt_Data* data, uint8_t* payload, uint32_t offset, uint32_t len) {
    (void)data;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = sample_byte(sample_id, offset + i);
    }
    return (int32_t)len;
}
static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    return sample_encode_range(data, payload, 0, len);
}
static uint8_t zerocopy_body[ZEROCOPY];
static int32_t sample_encode_inplace(struct tt_Data* data, const uint8_t** body) {
    (void)data;
    *body = zerocopy_body;
    return (int32_t)ZEROCOPY;
}

static uint32_t delivered[MAX_DELIVERED];
static int delivered_count;
static int delivered_torn;
static uint64_t first_delivered_at;

static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native) {
    (void)data;
    (void)is_native;
    uint32_t id = 0;
    if (len < sizeof(id)) {
        delivered_torn++;
        return 0;
    }
    memcpy(&id, payload, sizeof(id));
    for (uint32_t i = 0; i < len; i++) {
        if (payload[i] != sample_byte(id, i)) {
            delivered_torn++;
            break;
        }
    }
    if (delivered_count == 0) {
        first_delivered_at = test_mock_now;
    }
    if (delivered_count < MAX_DELIVERED) {
        delivered[delivered_count] = id;
    }
    delivered_count++;
    return 0;
}
static void free_nothing(struct tt_Data* data) {
    (void)data;
}
static void on_data(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
}

// --- large buffers -------------------------------------------------------------------------------------------------

static int large_live;
static void* acquire(void* user, uint32_t bytes) {
    (void)user;
    large_live++;
    return malloc(bytes);
}
static void release(void* user, void* buffer) {
    (void)user;
    large_live--;
    free(buffer);
}

// --- two contexts and the link between them ------------------------------------------------------------------------

struct datagram {
    uint8_t from;
    bool unicast;
    uint32_t len;
};
static struct datagram queue[QUEUE];
static uint8_t queue_bytes[QUEUE][tt_MAX_BUFFER_LENGTH];
static int queued;
static uint8_t acting;
static int seen_send_to;
static bool (*drop)(const struct datagram* datagram, const uint8_t* bytes);

// Whether a datagram carries discovery alone - announces, summaries, list requests, all on tt_DISCOVERY_ENDPOINT_ID.
// Anything else the writer sends is its Publisher's, and counted in topic_sent.
static bool discovery_only(const void* buf, size_t len) {
    static uint8_t classic[tt_MAX_BUFFER_LENGTH + sizeof(struct tt_Header)];
    size_t classic_len = test_classic_form(buf, len, classic);
    size_t head = sizeof(struct tt_Header);
    bool any = false;
    while (head + sizeof(struct tt_SubmessageHeader) + sizeof(uint32_t) <= classic_len) {
        struct tt_SubmessageHeader submessage;
        memcpy(&submessage, &classic[head], sizeof(submessage));
        uint32_t endpoint_id = 0;
        memcpy(&endpoint_id, &classic[head + sizeof(submessage)], sizeof(endpoint_id));
        bool discovery =
            (submessage.type == tt_SUBMESSAGE_TYPE_DATA || submessage.type == tt_SUBMESSAGE_TYPE_HEARTBEAT ||
             submessage.type == tt_SUBMESSAGE_TYPE_ACKNACK) &&
            endpoint_id == tt_DISCOVERY_ENDPOINT_ID;
        if (!discovery || submessage.length < sizeof(submessage)) {
            return false;
        }
        any = true;
        head += submessage.length;
    }
    return any;
}

static int topic_sent; // datagrams the writer handed the HAL that were not discovery

static void capture(const void* buf, size_t len) {
    if (acting == WRITER_ID && !discovery_only(buf, len)) {
        topic_sent++;
    }
    EXPECT_TRUE(queued < QUEUE && len <= tt_MAX_BUFFER_LENGTH);
    if (queued >= QUEUE || len > tt_MAX_BUFFER_LENGTH) {
        return;
    }
    struct datagram* datagram = &queue[queued];
    datagram->from = acting;
    datagram->unicast = test_mock_send_to_call_count != seen_send_to;
    seen_send_to = test_mock_send_to_call_count;
    datagram->len = (uint32_t)len;
    memcpy(queue_bytes[queued], buf, len);
    queued++;
}

static struct tt_Context writer;
static struct tt_Context reader;
static bool reader_up; // the reader context exists and takes datagrams

static void run_due(struct tt_Context* node) {
    bool has_next = false;
    uint64_t next = 0;
    acting = node->id;
    while (true) {
        uint64_t head = UINT64_MAX;
        if (!sched_next_time(node, &head) || head > test_mock_now) {
            return;
        }
        (void)run_due_entry(node, test_mock_now, &has_next, &next);
    }
}

static void deliver(void) {
    for (int i = 0; i < queued; i++) { // grows while delivering: a reply is delivered in the same pass
        struct datagram* datagram = &queue[i];
        if (drop != NULL && drop(datagram, queue_bytes[i])) {
            continue;
        }
        struct tt_Context* to = datagram->from == WRITER_ID ? &reader : &writer;
        if (to == &reader && !reader_up) {
            continue;
        }
        acting = to->id;
        memcpy(to->rx_buffer, queue_bytes[i], datagram->len);
        to->rx_via_data_port = datagram->unicast;
        (void)process_packet(to, to->rx_buffer, 0, datagram->len, datagram->from == WRITER_ID ? WRITER_IP : READER_IP,
                             PORT, tt_TRANSPORT_UDP);
    }
    queued = 0;
}

static void run_until(uint64_t until) {
    while (true) {
        uint64_t next_writer = UINT64_MAX;
        uint64_t next_reader = UINT64_MAX;
        (void)sched_next_time(&writer, &next_writer);
        if (reader_up) {
            (void)sched_next_time(&reader, &next_reader);
        }
        uint64_t next = next_writer < next_reader ? next_writer : next_reader;
        if (next > until) {
            test_mock_now = until;
            deliver();
            return;
        }
        if (next > test_mock_now) {
            test_mock_now = next;
        }
        run_due(&writer);
        if (reader_up) {
            run_due(&reader);
        }
        deliver();
    }
}

static void init_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
    node->last_modified = test_mock_now;
    node->entity_id_base = 0x5A000000U | ((uint32_t)id << 16); // as tt_Context_create() draws one: never 0
}

static struct tt_TypeSupport writer_topic;
static struct tt_TypeSupport reader_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static struct tt_ReliableCacheIndex cache_index[DEPTH];
static uint8_t cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(DEPTH, tt_RELIABLE_RECORD_BYTES(1500))];
static struct tt_ReliableCache cache;
static uint64_t reorder_storage[REORDER_SLOTS * REORDER_SLOT_BYTES / sizeof(uint64_t)];

enum mode { BEST_EFFORT, RELIABLE, DURABLE };

// The writer context, its Publisher in `mode` keeping `depth` samples, and the reader context, up and knowing the
// writer but with no Subscriber yet. Neither context keeps a discovery table (tt_Context_set_discovery() is opt-in):
// the writer learns its readers from their announces alone. `periodic` false leaves both contexts' periodic tasks off,
// so that every datagram the writer sends is the Publisher's.
static void start(enum mode mode, uint16_t depth, bool periodic) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    test_mock_send_hook = capture;
    queued = 0;
    seen_send_to = 0;
    topic_sent = 0;
    drop = NULL;
    delivered_count = 0;
    delivered_torn = 0;
    first_delivered_at = 0;
    memset(delivered, 0, sizeof(delivered));
    sample_len = SMALL;
    sample_id = 0;
    init_node(&writer, WRITER_ID);
    init_node(&reader, READER_ID);
    reader_up = periodic;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&writer, acquire, release, NULL));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&reader, acquire, release, NULL));
    if (periodic) {
        EXPECT_EQ_INT(tt_RET_OK, schedule_periodic_tasks(&writer));
        EXPECT_EQ_INT(tt_RET_OK, schedule_periodic_tasks(&reader));
    }
    writer_topic = (struct tt_TypeSupport) {.name = "unmatched",
                                            .data_size = 8,
                                            .data_encode_size = sample_encode_size,
                                            .data_encode = sample_encode,
                                            .data_encode_range = sample_encode_range};
    reader_topic = (struct tt_TypeSupport) {.name = "unmatched",
                                            .data_size = 8,
                                            .data_decode = sample_decode,
                                            .data_free = free_nothing};
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&writer, &pub, &writer_topic, "unmatched_ep"));
    if (mode != BEST_EFFORT) {
        memset(&cache, 0, sizeof(cache));
        EXPECT_EQ_INT(tt_RET_OK, tt_ReliableCache_init(&cache, cache_index, depth, cache_arena, sizeof(cache_arena)));
        cache.sample_depth = depth; // KEEP_LAST in samples, as rmw_tickle sets it: the large ring keeps as many
        pub.reliable_cache = &cache;
        pub.reliable = true;
        pub.durable = mode == DURABLE;
    }
    EXPECT_TRUE(writer.discovery == NULL && reader.discovery == NULL);
    if (periodic) {
        run_until(test_mock_now + (2 * tt_SECOND)); // the two contexts know each other
        EXPECT_TRUE(reader.update_seen[WRITER_ID] && writer.update_seen[READER_ID]);
    }
}

static void create_reader(enum mode mode) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&reader, &sub, &reader_topic, "unmatched_ep", on_data));
    if (mode != BEST_EFFORT) {
        sub.reliable = true;
        sub.durable = mode == DURABLE;
        memset(reorder_storage, 0, sizeof(reorder_storage));
        sub.reorder_storage = reorder_storage;
        sub.reorder_slots = REORDER_SLOTS;
        sub.reorder_slot_bytes = REORDER_SLOT_BYTES;
    }
}

static void stop(void) {
    drop = NULL;
    test_mock_send_hook = NULL;
    large_release_publisher(&writer, &pub);
    large_release_assemblies(&reader);
}

// One publish of `len` bytes with id `id`; returns how many datagrams the writer handed the HAL meanwhile.
static int publish_one(uint32_t id, uint32_t len) {
    sample_id = id;
    sample_len = len;
    int before = topic_sent;
    acting = WRITER_ID;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_id));
    return topic_sent - before;
}

static void expect_delivered(uint32_t first, uint32_t last) {
    EXPECT_EQ_INT((int)(last - first + 1), delivered_count);
    for (uint32_t id = first; id <= last && (int)(id - first) < delivered_count; id++) {
        EXPECT_EQ_U32(id, delivered[id - first]);
    }
    EXPECT_EQ_INT(0, delivered_torn);
}

// Gives the Publisher `count` known readers that are not the reader context: tt_UNICAST_PEER_THRESHOLD + 1 makes it
// broadcast, as with a segment full of readers. The controls use it.
static void fake_readers(uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        (void)upsert_peer(pub.peers, (uint8_t)(100U + i), 0x0a000100U + i, PORT);
    }
}

// --- 1. nothing on the wire ----------------------------------------------------------------------------------------

// Publishes `len`-byte samples with no reader and returns what reached the HAL: during the publishes and over the next
// 50 ms of the writer's own timers (a batch's flush, a large send's resume, the periodic Heartbeat).
static int sent_with_no_reader(enum mode mode, uint32_t len, bool batch) {
    start(mode, DEPTH, false);
    pub.batch = batch;
    if (mode != BEST_EFFORT) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, 5 * tt_MILLISECOND));
    }
    int sent = 0;
    for (uint32_t id = 1; id <= 3; id++) {
        sent += publish_one(id, len);
    }
    int before = topic_sent;
    run_until(test_mock_now + (50 * tt_MILLISECOND));
    sent += topic_sent - before;
    EXPECT_TRUE(writer.tx_tail == sizeof(struct tt_Header)); // nothing left waiting to go either
    EXPECT_TRUE(!pub.large_cursor.active);
    return sent;
}

static void expect_silent_and_counted(enum mode mode, uint32_t len, bool batch) {
    int sent = sent_with_no_reader(mode, len, batch);
    EXPECT_EQ_INT(0, sent);
    EXPECT_EQ_U64(mode == BEST_EFFORT ? 3U : 0U, writer.tx_unmatched_dropped);
    EXPECT_EQ_U64(mode == BEST_EFFORT ? 0U : 3U, writer.tx_unmatched_kept);
    EXPECT_TRUE(pub.seq_no >= 3); // the seq_nos were taken as a sent sample's would be
    if (sent != 0) {
        printf("test_no_send_unmatched: mode %d, %u bytes, batch %d: %d datagram(s) with no reader\n", (int)mode, len,
               batch, sent);
    }
    stop();

    // Control: the same with readers known, so a count of 0 above is a count that could have been more.
    start(mode, DEPTH, false);
    pub.batch = batch;
    fake_readers(tt_UNICAST_PEER_THRESHOLD + 1);
    int control = 0;
    for (uint32_t id = 1; id <= 3; id++) {
        control += publish_one(id, len);
    }
    int before = topic_sent;
    run_until(test_mock_now + (50 * tt_MILLISECOND));
    control += topic_sent - before;
    EXPECT_TRUE(control >= 1);
    EXPECT_EQ_U64(0, writer.tx_unmatched_dropped + writer.tx_unmatched_kept);
    stop();
}

static void test_a_writer_nobody_reads_sends_nothing(void) {
    const uint32_t sizes[] = {SMALL, FRAGMENTED, LARGE};
    for (int mode = BEST_EFFORT; mode <= DURABLE; mode++) {
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            expect_silent_and_counted((enum mode)mode, sizes[i], false);
        }
        expect_silent_and_counted((enum mode)mode, SMALL, true); // a batching publisher: node_flush() finds nothing
    }
}

// The zero-copy path (publish_zerocopy()), BEST_EFFORT only: it sends from the caller's bytes.
static void test_the_zero_copy_path_sends_nothing_either(void) {
    for (int arm = 0; arm < 2; arm++) {
        start(BEST_EFFORT, DEPTH, false);
        writer_topic.data_encode_inplace = sample_encode_inplace;
        if (arm == 1) {
            fake_readers(tt_UNICAST_PEER_THRESHOLD + 1); // control
        }
        int sent = publish_one(1, ZEROCOPY);
        EXPECT_EQ_INT(arm == 0 ? 0 : 1, sent > 0 ? 1 : 0);
        EXPECT_EQ_U64(arm == 0 ? 1U : 0U, writer.tx_unmatched_dropped);
        EXPECT_EQ_U32(tt_sample_datagrams(ZEROCOPY), pub.seq_no); // its seq_nos taken all the same
        stop();
    }
}

// A match Heartbeat owed to a reader that has since gone is not sent to nobody either.
static void test_an_owed_match_heartbeat_waits_for_a_reader(void) {
    for (int arm = 0; arm < 2; arm++) {
        start(RELIABLE, DEPTH, true);
        create_reader(RELIABLE);
        run_until(test_mock_now + (100 * tt_MILLISECOND));
        EXPECT_EQ_INT(1, (int)count_peers(pub.peers));
        EXPECT_TRUE(pub.match_heartbeat_pending || match_point_owed(&pub, &(uint32_t) {0}));
        reader_up = false; // the reader is gone from the link
        if (arm == 0) {
            forget_peer(pub.peers, READER_ID); // and from the Publisher's peers, its ack entry left behind
        }
        pub.match_heartbeat_pending = true;
        int sent = publish_one(1, SMALL);
        EXPECT_EQ_INT(arm == 0 ? 0 : 1, sent > 0 ? 1 : 0); // control: with the reader still a peer, it goes
        stop();
    }
}

// --- 2. a VOLATILE reader that joins later ------------------------------------------------------------------------

static void test_a_volatile_reader_gets_samples_from_its_match_on(void) {
    start(RELIABLE, DEPTH, true);
    int sent = 0;
    for (uint32_t id = 1; id <= 3; id++) {
        sent += publish_one(id, SMALL);
        run_until(test_mock_now + tt_MILLISECOND);
    }
    EXPECT_EQ_INT(0, sent);                // nobody to send to yet
    EXPECT_EQ_U32(1, cache.oldest_seq_no); // control: they are retained, so they could have been resent
    EXPECT_EQ_INT(0, (int)tt_Publisher_matched_count(&pub)); // the public accessor says what peers[] says
    EXPECT_EQ_INT(0, (int)tt_Publisher_matched_count(NULL));
    create_reader(RELIABLE);
    run_until(test_mock_now + (100 * tt_MILLISECOND));
    EXPECT_EQ_INT(1, (int)count_peers(pub.peers));
    EXPECT_EQ_INT(1, (int)tt_Publisher_matched_count(&pub));
    for (uint32_t id = 4; id <= 6; id++) {
        (void)publish_one(id, SMALL);
        run_until(test_mock_now + tt_MILLISECOND);
    }
    run_until(test_mock_now + (200 * tt_MILLISECOND));
    expect_delivered(4, 6);
    stop();
}

// --- 3. a TRANSIENT_LOCAL reader that joins later ------------------------------------------------------------------

static void transient_local_history(uint32_t len, uint16_t depth) {
    start(DURABLE, depth, true);
    int sent = 0;
    for (uint32_t id = 1; id <= 6; id++) {
        sent += publish_one(id, len);
        run_until(test_mock_now + tt_MILLISECOND);
    }
    EXPECT_EQ_INT(0, sent);
    EXPECT_EQ_U64(6, writer.tx_unmatched_kept);
    create_reader(DURABLE);
    run_until(test_mock_now + (200 * tt_MILLISECOND));
    for (uint32_t id = 7; id <= 8; id++) {
        (void)publish_one(id, len);
        run_until(test_mock_now + tt_MILLISECOND);
    }
    run_until(test_mock_now + (500 * tt_MILLISECOND));
}

static void test_a_transient_local_reader_gets_the_history(void) {
    transient_local_history(SMALL, 4); // KEEP_LAST 4: 3..6 kept
    expect_delivered(3, 8);
    stop();
    // Large samples: each its own record in the large ring, KEEP_LAST 4 - and encoded whole although nothing was sent,
    // which the ranged encode would have left to a send.
    transient_local_history(LARGE, 4);
    expect_delivered(3, 8);
    stop();
    EXPECT_EQ_INT(0, large_live);
}

// --- 4. a reader that knows the writer before the writer knows it --------------------------------------------------

// The reader's announces and lists - its endpoint list, which is what matches it - while `lose_reader_lists` is set.
static bool lose_reader_lists;
static bool is_reader_list(const struct datagram* datagram, const uint8_t* bytes) {
    if (!lose_reader_lists || datagram->from != READER_ID) {
        return false;
    }
    static uint8_t classic[tt_MAX_BUFFER_LENGTH + sizeof(struct tt_Header)];
    size_t len = test_classic_form(bytes, datagram->len, classic);
    if (len < sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader)) {
        return false;
    }
    struct tt_SubmessageHeader submessage;
    memcpy(&submessage, &classic[sizeof(struct tt_Header)], sizeof(submessage));
    struct tt_DataHeader data;
    memcpy(&data, &classic[sizeof(struct tt_Header) + sizeof(submessage)], sizeof(data));
    return submessage.type == tt_SUBMESSAGE_TYPE_DATA && data.endpoint_id == tt_DISCOVERY_ENDPOINT_ID;
}

// The writer publishes every millisecond from 100 ms before the Subscriber is created; returns how long after the
// Subscriber's creation the first sample was delivered, in ns. lose_ns: the reader's lists are lost for this long.
static uint64_t first_sample_after(enum mode mode, uint64_t lose_ns) {
    start(mode, DEPTH, true);
    drop = is_reader_list;
    lose_reader_lists = lose_ns != 0;
    uint32_t id = 1;
    for (int i = 0; i < 100; i++) {
        (void)publish_one(id++, SMALL);
        run_until(test_mock_now + tt_MILLISECOND);
    }
    uint64_t created = test_mock_now;
    create_reader(mode);
    EXPECT_TRUE(reader.update_seen[WRITER_ID]); // the reader knows the writer already; the writer cannot know it
    uint32_t first_after_match = 0;
    while (test_mock_now - created < 3 * tt_SECOND) {
        if (lose_reader_lists && test_mock_now - created >= lose_ns) {
            lose_reader_lists = false;
        }
        bool matched_before = count_peers(pub.peers) != 0;
        (void)publish_one(id, SMALL);
        if (matched_before && first_after_match == 0) {
            first_after_match = id;
        }
        id++;
        run_until(test_mock_now + tt_MILLISECOND);
    }
    EXPECT_TRUE(delivered_count > 0);
    // The first sample delivered is the first published once the writer had matched the reader - none sooner, none
    // later.
    EXPECT_EQ_U32(first_after_match, delivered[0]);
    EXPECT_EQ_INT(0, delivered_torn);
    uint64_t delay = first_delivered_at - created;
    stop();
    return delay;
}

static void test_a_reader_known_first_gets_data_once_the_writer_learns_it(void) {
    for (int mode = BEST_EFFORT; mode <= RELIABLE; mode++) {
        // Its changed announce gets through: one flush tick (announce_soon()), then the next sample.
        uint64_t prompt = first_sample_after((enum mode)mode, 0);
        printf("test_no_send_unmatched: mode %d, the reader's announce arrives: first sample %.1f ms after the "
               "Subscriber was created\n",
               mode, (double)prompt / (double)tt_MILLISECOND);
        EXPECT_TRUE(prompt <= (2 * tt_CONTEXT_TX_INTERVAL) + tt_MILLISECOND);
        // Its lists are lost for 50 ms: the writer learns it from its next summary, which it answers with a request.
        uint64_t late = first_sample_after((enum mode)mode, 50 * tt_MILLISECOND);
        printf("test_no_send_unmatched: mode %d, the reader's announce lost: first sample %.1f ms after the Subscriber "
               "was created\n",
               mode, (double)late / (double)tt_MILLISECOND);
        EXPECT_TRUE(late >= 50 * tt_MILLISECOND && late <= tt_CONTEXT_UPDATE_INTERVAL + (10 * tt_MILLISECOND));
    }
}

// --- 5. the risk check's other configurations ----------------------------------------------------------------------

// peers[] full (tt_MAX_PEER_COUNT): a reader whose announce finds no slot is not learned - and does not have to be,
// since a full table is more than the unicast threshold and the Publisher broadcasts. Unchanged by this rule.
static void test_a_reader_that_does_not_fit_peers_still_gets_the_broadcast(void) {
    start(BEST_EFFORT, DEPTH, true);
    fake_readers(tt_MAX_PEER_COUNT);
    create_reader(BEST_EFFORT);
    run_until(test_mock_now + (100 * tt_MILLISECOND));
    bool learned = false;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        learned = learned || pub.peers[i].context_id == READER_ID;
    }
    EXPECT_TRUE(!learned); // the premise: no slot for it
    for (uint32_t id = 1; id <= 3; id++) {
        (void)publish_one(id, SMALL);
        run_until(test_mock_now + tt_MILLISECOND);
    }
    expect_delivered(1, 3);
    stop();
}

// A live reader whose traffic is lost past the liveliness limit is presumed gone and forgotten; with no reader left
// the writer sends nothing - before 2026-10-15 it broadcast, and the reader kept receiving. Once its traffic gets
// through again its next summary is answered with a request for its list, and it is matched again.
static bool lose_everything_from_the_reader;
static bool reader_silenced(const struct datagram* datagram, const uint8_t* bytes) {
    (void)bytes;
    return lose_everything_from_the_reader && datagram->from == READER_ID;
}

static void test_a_reader_presumed_gone_gets_data_again_after_its_next_summary(void) {
    start(BEST_EFFORT, DEPTH, true);
    create_reader(BEST_EFFORT);
    run_until(test_mock_now + (100 * tt_MILLISECOND));
    EXPECT_EQ_INT(1, (int)count_peers(pub.peers));
    drop = reader_silenced;
    lose_everything_from_the_reader = true;
    uint32_t id = 1;
    while (count_peers(pub.peers) != 0 && id < 20000) {
        (void)publish_one(id++, SMALL);
        run_until(test_mock_now + tt_MILLISECOND);
    }
    EXPECT_EQ_INT(0, (int)count_peers(pub.peers)); // presumed gone
    EXPECT_EQ_INT(0, (int)tt_Publisher_matched_count(&pub));
    int before_gap = delivered_count;
    for (int i = 0; i < 100; i++) {
        (void)publish_one(id++, SMALL);
        run_until(test_mock_now + tt_MILLISECOND);
    }
    EXPECT_EQ_INT(before_gap, delivered_count); // nothing reaches it meanwhile
    lose_everything_from_the_reader = false;
    uint64_t restored = test_mock_now;
    while (delivered_count == before_gap && test_mock_now - restored < 3 * tt_SECOND) {
        (void)publish_one(id++, SMALL);
        run_until(test_mock_now + tt_MILLISECOND);
    }
    EXPECT_TRUE(delivered_count > before_gap);
    printf("test_no_send_unmatched: a reader presumed gone gets data again %.1f ms after its traffic gets through\n",
           (double)(test_mock_now - restored) / (double)tt_MILLISECOND);
    EXPECT_TRUE(test_mock_now - restored <= tt_CONTEXT_UPDATE_INTERVAL + (10 * tt_MILLISECOND));
    stop();
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    for (uint32_t i = 0; i < ZEROCOPY; i++) {
        zerocopy_body[i] = sample_byte(1, i);
    }
    test_a_writer_nobody_reads_sends_nothing();
    test_the_zero_copy_path_sends_nothing_either();
    test_an_owed_match_heartbeat_waits_for_a_reader();
    test_a_volatile_reader_gets_samples_from_its_match_on();
    test_a_transient_local_reader_gets_the_history();
    test_a_reader_known_first_gets_data_once_the_writer_learns_it();
    test_a_reader_that_does_not_fit_peers_still_gets_the_broadcast();
    test_a_reader_presumed_gone_gets_data_again_after_its_next_summary();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_no_send_unmatched: all tests passed\n");
    return 0;
}
