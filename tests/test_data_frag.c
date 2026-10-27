/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// DATA_FRAG (rmw_tickle/DATAFRAG_PLAN.md section 6): a sample too large for one datagram goes out as
// FRAG_FIRST + FRAG_CONT datagrams and is put back together by the receiver.
//
// Built with tt_MAX_SAMPLE_LENGTH above tt_MAX_BUFFER_LENGTH, which is what compiles fragmentation in;
// every other test binary is built without it, and test_oversize_tx.c checks that such a build still
// refuses what one datagram cannot carry. Each test drives a real sender and a real receiver: what the
// sender's HAL was handed is fed, datagram by datagram, into the receiver's process_packet().

// Room for 11 fragments: the loss sweep below runs samples of 1, 2, 4 and 10.
#define tt_MAX_SAMPLE_LENGTH 16000

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: process_packet()/retransmit_one_sample()/deliver_durability_backlog() are static.
#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox: tickle.c's statics; tests/mutants_link_profiles.py passes a mutant

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define RECEIVER_IP 0x0a000002
#define PORT 8282
#define MAX_DATAGRAMS 16
#define TOPIC_NAME "frag_topic"
#define ENDPOINT_NAME "frag_endpoint" // the endpoint id hashes topic and endpoint name, so both sides share it

// The largest CDR a DATA can carry at the default datagram: 4 + ROUNDUP(4 + 16 + L) <= 1472 (the DataHeader
// is 16 bytes since tt_VERSION 10; 1444 before).
#define LARGEST_DATA_CDR 1448

// --- capture ------------------------------------------------------------------------------------

static uint8_t datagrams[MAX_DATAGRAMS][tt_MAX_BUFFER_LENGTH * 2];
static uint32_t datagram_len[MAX_DATAGRAMS];
// How long each really was on the wire: datagrams[] holds the classic form (test_classic_form(), test_mock.h),
// 4 bytes longer than a datagram sent in the single-submessage form (tt_VERSION 10), which every fragment is.
static uint32_t wire_len[MAX_DATAGRAMS];
static int datagram_count;

static void capture(const void* buf, size_t len) {
    EXPECT_TRUE(datagram_count < MAX_DATAGRAMS);
    EXPECT_TRUE(len <= tt_MAX_BUFFER_LENGTH); // every datagram, fragment or not, fits the default MTU
    if (datagram_count < MAX_DATAGRAMS && len + sizeof(struct tt_Header) <= sizeof(datagrams[0])) {
        datagram_len[datagram_count] = (uint32_t)test_classic_form(buf, len, datagrams[datagram_count]);
        wire_len[datagram_count] = (uint32_t)len;
        datagram_count++;
    }
}

static void start_capture(void) {
    datagram_count = 0;
    test_mock_send_hook = capture;
}

static const struct tt_SubmessageHeader* submessage_of(int d) {
    return (const struct tt_SubmessageHeader*)(datagrams[d] + sizeof(struct tt_Header));
}

// Bytes on the wire, all captured datagrams together.
static uint32_t total_captured_bytes(void) {
    uint32_t total = 0;
    for (int d = 0; d < datagram_count; d++) {
        total += wire_len[d];
    }
    return total;
}

// --- the sample: a sized, patterned payload -------------------------------------------------------

static uint32_t sample_len;
static uint8_t sample_seed;

static uint8_t pattern(uint32_t i, uint8_t seed) {
    return (uint8_t)((i * 7U) + seed);
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

// The zero-copy variant: the payload handed over as-is, from the publisher's own memory.
static uint8_t inplace_body[tt_MAX_SAMPLE_LENGTH];
static int32_t sized_encode_inplace(struct tt_Data* data, const uint8_t** body) {
    (void)data;
    for (uint32_t i = 0; i < sample_len; i++) {
        inplace_body[i] = pattern(i, sample_seed);
    }
    *body = inplace_body;
    return (int32_t)sample_len;
}

// --- delivery: what the receiver's Subscriber saw -------------------------------------------------

static int delivered_count;
static uint32_t delivered_len;
static bool delivered_intact;
static uint8_t expected_seed;

static int32_t checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    delivered_len = len;
    delivered_intact = true;
    for (uint32_t i = 0; i < sample_len && i < len; i++) {
        if (payload[i] != pattern(i, expected_seed)) {
            delivered_intact = false;
            break;
        }
    }
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
    delivered_count++;
}

// --- nodes ----------------------------------------------------------------------------------------

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_TypeSupport sender_topic;
static struct tt_TypeSupport receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

// A Publisher with no matched reader sends nothing (2026-10-15): more known readers than the unicast threshold, none
// of them the receiver, so a publish broadcasts as these tests expect and the receiver takes it from the broadcast.
static void match_by_broadcast(struct tt_Publisher* publisher) {
    for (uint8_t peer = 0; peer <= tt_UNICAST_PEER_THRESHOLD; peer++) {
        (void)upsert_peer(publisher->peers, (uint8_t)(100 + peer), 0x0a000100U + peer, PORT);
    }
}

// Caller storage for a Subscriber that needs only its own reassembly slot (a BEST_EFFORT one of these unbounded topics,
// tt_Subscriber_storage_size(); docs/DESIGN.md section 8, "Reassembly slots"): one per Subscriber alive at once.
#define OWN_SLOT_STORAGE (tt_FRAG_SLOT_BYTES + 64)
static uint64_t own_slot_storage[4][OWN_SLOT_STORAGE / sizeof(uint64_t)];

// How init_pair_with() creates the pair. A NULL profile is TEST_QOS_UNRELIABLE, NULL options none, a NULL callback
// on_data; storage is only used when the profile needs it, and must hold what tt_*_storage_size() says. No Subscriber
// storage is own_slot_storage[0].
struct pair_qos {
    const struct tt_QosProfile* pub_qos;
    const struct tt_PublisherOptions* pub_options;
    void* pub_storage;
    size_t pub_storage_capacity;
    const struct tt_QosProfile* sub_qos;
    tt_SUBSCRIBER_CALLBACK sub_callback;
    void* sub_storage;
    size_t sub_storage_capacity;
};

static void init_pair_with(uint32_t len, const struct pair_qos* q) {
    static const struct pair_qos none;
    if (q == NULL) {
        q = &none;
    }
    const struct tt_QosProfile* pub_qos = q->pub_qos != NULL ? q->pub_qos : &TEST_QOS_UNRELIABLE;
    const struct tt_QosProfile* sub_qos = q->sub_qos != NULL ? q->sub_qos : &TEST_QOS_UNRELIABLE;
    test_mock_reset();
    sample_len = len;
    sample_seed = 3;
    expected_seed = 3;
    delivered_count = 0;
    delivered_len = 0;
    delivered_intact = false;

    init_bare_node(&sender, SENDER_ID);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = 8;
    sender_topic.data_encode_size = sized_encode_size;
    sender_topic.data_encode = sized_encode;
    size_t pub_storage_size = tt_Publisher_storage_size(&sender_topic, pub_qos);
    EXPECT_TRUE(pub_storage_size <= q->pub_storage_capacity);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&sender), &pub, &sender_topic,
                                                      ENDPOINT_NAME, pub_qos, q->pub_options,
                                                      pub_storage_size != 0 ? q->pub_storage : NULL, pub_storage_size));
    match_by_broadcast(&pub);

    init_bare_node(&receiver, RECEIVER_ID);
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = 8;
    receiver_topic.data_decode = checking_decode;
    receiver_topic.data_free = free_nothing;
    void* sub_storage = q->sub_storage != NULL ? q->sub_storage : own_slot_storage[0];
    size_t sub_storage_capacity = q->sub_storage != NULL ? q->sub_storage_capacity : sizeof(own_slot_storage[0]);
    size_t sub_storage_size = tt_Subscriber_storage_size(&receiver_topic, sub_qos);
    EXPECT_TRUE(sub_storage_size <= sub_storage_capacity);
    const struct tt_SubscriberOptions sub_options = {.callback = q->sub_callback != NULL ? q->sub_callback : on_data};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub, &receiver_topic,
                                                       ENDPOINT_NAME, sub_qos, &sub_options,
                                                       sub_storage_size != 0 ? sub_storage : NULL, sub_storage_size));
}

static void init_pair(uint32_t len) {
    init_pair_with(len, NULL);
}

static void deliver(int d) {
    EXPECT_TRUE(process_packet(&receiver, datagrams[d], 0, datagram_len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
}

static void publish_captured(void) {
    start_capture();
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    test_mock_send_hook = NULL;
}

static void expect_delivered_once(uint32_t len) {
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U32(len, delivered_len);
    EXPECT_TRUE(delivered_intact);
}

// --- tests ----------------------------------------------------------------------------------------

static void test_topics_up_to_the_sample_limit_can_be_created(void) {
    // A topic's own C struct may be as large as the sample limit, not only the datagram: a p4 message is
    // 2800 bytes as a struct too. Found by the first run over real sockets, which failed to create
    // either endpoint - every test above uses an 8-byte struct.
    init_pair(8);
    struct tt_Publisher big_pub;
    struct tt_Subscriber big_sub;
    struct tt_TypeSupport big_topic = sender_topic;
    big_topic.name = "big";
    big_topic.data_decode = checking_decode;
    big_topic.data_free = free_nothing;
    big_topic.data_size = tt_MAX_SAMPLE_LENGTH;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&sender), &big_pub, &big_topic,
                                                      ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    EXPECT_TRUE(tt_Subscriber_storage_size(&big_topic, &TEST_QOS_UNRELIABLE) <= sizeof(own_slot_storage[1]));
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&receiver), &big_sub, &big_topic, ENDPOINT_NAME,
                                            &TEST_QOS_UNRELIABLE, &(struct tt_SubscriberOptions) {.callback = on_data},
                                            own_slot_storage[1], sizeof(own_slot_storage[1])));
    // Control: one byte more is still refused.
    struct tt_Publisher too_big_pub;
    struct tt_Subscriber too_big_sub;
    big_topic.data_size = tt_MAX_SAMPLE_LENGTH + 1;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_publisher(tt_Context_default_node(&sender), &too_big_pub, &big_topic, "other",
                                           &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_subscriber(tt_Context_default_node(&receiver), &too_big_sub, &big_topic, "other",
                                            &TEST_QOS_UNRELIABLE, &(struct tt_SubscriberOptions) {.callback = on_data},
                                            own_slot_storage[2], sizeof(own_slot_storage[2])));
}

static void test_largest_data_is_not_fragmented(void) {
    // Control for everything below: a sample one datagram can carry goes as the DATA it always was.
    // p1-p3 of the benchmark live here, and DATAFRAG_PLAN.md's acceptance bar is that they see no
    // change at all.
    init_pair(LARGEST_DATA_CDR);
    publish_captured();
    EXPECT_EQ_INT(1, datagram_count);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_DATA, submessage_of(0)->type);
    deliver(0);
    expect_delivered_once(LARGEST_DATA_CDR);
    EXPECT_EQ_U64(0, receiver.frag_reassembled);
}

static void test_one_byte_more_is_two_fragments(void) {
    init_pair(LARGEST_DATA_CDR + 1);
    publish_captured();
    EXPECT_EQ_INT(2, datagram_count);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_FIRST, submessage_of(0)->type);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_CONT, submessage_of(1)->type);
    deliver(0);
    EXPECT_EQ_INT(0, delivered_count); // nothing until the sample is whole
    deliver(1);
    // The receiver sees the sample padded to 4, exactly as it would have seen a DATA carrying it.
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U32(ROUNDUP(LARGEST_DATA_CDR + 1), delivered_len);
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_U64(1, receiver.frag_reassembled);
}

static void test_p4_is_two_datagrams_of_the_predicted_size(void) {
    // DATAFRAG_PLAN.md 6.3's arithmetic, checked against what is actually sent: 2919 B/sample on the
    // wire at p4 is these 2835 bytes of UDP payload plus 2 x 42 of Ethernet, IPv4 and UDP. Until tt_VERSION
    // 10 it was 2931 and 2847: FRAG_FIRST's DataHeader is 4 bytes shorter (W2) and each fragment goes in
    // the single-submessage form, 4 bytes less framing each (W4). The classic-form lengths below keep the
    // capacity arithmetic, which W4 does not change.
    init_pair(2800);
    publish_captured();
    EXPECT_EQ_INT(2, datagram_count);
    EXPECT_EQ_U32(tt_MAX_BUFFER_LENGTH, datagram_len[0]); // fragment 0 fills its datagram
    EXPECT_EQ_U32(4 + 4 + 17 + 1447, datagram_len[0]);
    EXPECT_EQ_U32(4 + 4 + 10 + (2800 - 1447), datagram_len[1]);
    EXPECT_EQ_U32(datagram_len[0] - 4, wire_len[0]); // single-submessage form
    EXPECT_EQ_U32(datagram_len[1] - 4, wire_len[1]);
    EXPECT_EQ_U32(2835, total_captured_bytes());
    deliver(0);
    deliver(1);
    expect_delivered_once(2800);
}

static void test_every_arrival_order_reassembles(void) {
    // Three fragments, all six orders. The ones where the last fragment arrives before any other
    // exercise parking: its length alone does not say where it goes.
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (int o = 0; o < 6; o++) {
        init_pair(4000);
        publish_captured();
        EXPECT_EQ_INT(3, datagram_count);
        for (int k = 0; k < 3; k++) {
            deliver(orders[o][k]);
        }
        expect_delivered_once(4000);
        EXPECT_EQ_U64(0, receiver.frag_dropped);
    }
}

static void test_missing_fragment_delivers_nothing(void) {
    init_pair(4000);
    publish_captured();
    deliver(0);
    deliver(2);
    EXPECT_EQ_INT(0, delivered_count);
    deliver(1); // control: the same slot completes once the gap is filled
    expect_delivered_once(4000);
}

static void test_duplicate_fragments_deliver_once(void) {
    init_pair(2800);
    publish_captured();
    deliver(0);
    deliver(0);
    deliver(1);
    expect_delivered_once(2800);
    // A duplicate of a fragment arriving after its sample completed is recognised as one (the slot is
    // marked done) and neither re-delivers nor opens a new reassembly.
    deliver(1);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U64(1, receiver.frag_duplicate);
}

// Reassemblies under way for the pair's Subscriber: in the pool, and in its own slot.
static int busy_slots(void) {
    int busy = sub.frag_own.received != 0 ? 1 : 0;
    for (int i = 0; i < tt_FRAG_REASSEMBLY_SLOTS; i++) {
        busy += receiver.frag_slots[i].received != 0 ? 1 : 0;
    }
    return busy;
}

static void test_late_fragment_of_a_completed_sample_opens_no_slot(void) {
    // The shape a retransmission takes when the original lost its first fragment: the continuation is
    // already here, the retransmitted first fragment completes the sample, and the retransmitted
    // continuation arrives after that. It must be recognised as a duplicate, not start a reassembly that
    // can never complete - which is what every such sample did before (tt_FragSlot.done), 26,711 times in
    // a 5 s veth run at 5% loss.
    init_pair(2800);
    publish_captured();
    deliver(1);
    deliver(0);
    EXPECT_EQ_INT(1, delivered_count);
    deliver(1); // the late one
    EXPECT_EQ_U64(1, receiver.frag_duplicate);
    EXPECT_EQ_INT(0, busy_slots());
    EXPECT_EQ_INT(1, delivered_count);

    // Control: a fragment of the next sample does claim a slot, so "no slot" above was the duplicate's.
    publish_captured();
    deliver(1);
    EXPECT_EQ_INT(1, busy_slots());
    EXPECT_EQ_U64(1, receiver.frag_duplicate);
}

static void test_samples_interleave_across_slots(void) {
    // Two samples whose fragments cross on the wire land in two slots and both complete.
    init_pair(2800);
    publish_captured();
    uint8_t first[2][tt_MAX_BUFFER_LENGTH];
    uint32_t first_len[2];
    for (int d = 0; d < 2; d++) {
        memcpy(first[d], datagrams[d], datagram_len[d]);
        first_len[d] = datagram_len[d];
    }
    publish_captured(); // seq_no 2, same payload
    // Each completes in publish order, so BEST_EFFORT ordering keeps both: a sample 1 completing after
    // sample 2 would rightly be discarded as out of order.
    EXPECT_TRUE(process_packet(&receiver, first[0], 0, first_len[0], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    deliver(0);
    EXPECT_TRUE(process_packet(&receiver, first[1], 0, first_len[1], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    EXPECT_EQ_INT(1, delivered_count);
    deliver(1);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_TRUE(delivered_intact);
}

// tracked: a DATA first, so the Subscriber tracks the writer and a continuation is routed to it; untracked, one it
// holds no slot for goes to a pool slot no Subscriber has taken - two paths, one outcome.
static void full_pool_case(bool tracked) {
    // Fill every slot the Subscriber may use - the pool and its own - with a sample missing its last fragment, then
    // start one more.
    init_pair(2800);
    if (tracked) {
        sample_len = 100;
        publish_captured();
        deliver(0);
        EXPECT_EQ_INT(1, delivered_count);
        delivered_count = 0;
        sample_len = 2800;
    }
    enum { SUB_SLOTS = tt_FRAG_REASSEMBLY_SLOTS + 1 };
    uint8_t lasts[SUB_SLOTS + 1][tt_MAX_BUFFER_LENGTH];
    uint32_t last_len[SUB_SLOTS + 1];
    for (int s = 0; s <= SUB_SLOTS; s++) {
        publish_captured();
        deliver(0);
        memcpy(lasts[s], datagrams[1], datagram_len[1]);
        last_len[s] = datagram_len[1];
    }
    EXPECT_EQ_U64(1, receiver.frag_abandoned); // the last took the first one's slot
    EXPECT_EQ_INT(0, delivered_count);

    // The abandoned sample's last fragment cannot complete it, and - older than every sample being put together from
    // its writer, each of which a BEST_EFFORT Subscriber would deliver first and then discard it - it takes no slot
    // from them: dropped, counted (until 2026-10-27 it started a new, incomplete one in sample 1's place).
    EXPECT_TRUE(process_packet(&receiver, lasts[0], 0, last_len[0], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_EQ_U64(1, receiver.frag_abandoned);
    EXPECT_EQ_U64(1, receiver.frag_dropped);
    // Every sample that kept its slot still completes (control: abandonment took exactly one).
    for (int s = 1; s <= SUB_SLOTS; s++) {
        EXPECT_TRUE(process_packet(&receiver, lasts[s], 0, last_len[s], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(SUB_SLOTS, delivered_count);
}

static void test_full_pool_abandons_the_oldest_and_counts_it(void) {
    full_pool_case(false);
    full_pool_case(true);
}

// Rewrites one byte of a captured fragment's header, for the malformed-input tests below.
static void set_cont_field(int d, size_t offset, uint8_t value) {
    datagrams[d][sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + offset] = value;
}

static void test_inconsistent_fragments_are_dropped_and_counted(void) {
    // A frag_count that disagrees with the slot's.
    init_pair(2800);
    publish_captured();
    deliver(0);
    set_cont_field(1, offsetof(struct tt_FragContHeader, frag_count), 3); // same sample, other count
    deliver(1);
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_EQ_U64(1, receiver.frag_dropped);

    // An index past the count, and a count of one: refused before any slot is touched.
    init_pair(2800);
    publish_captured();
    set_cont_field(1, offsetof(struct tt_FragContHeader, frag_index), 2);
    deliver(1);
    set_cont_field(1, offsetof(struct tt_FragContHeader, frag_index), 0);
    set_cont_field(1, offsetof(struct tt_FragContHeader, frag_count), 1);
    deliver(1);
    EXPECT_EQ_U64(2, receiver.frag_dropped);
    EXPECT_EQ_INT(0, delivered_count);

    // A continuation whose position would run past the end of the slot: frag_count 64 at a full
    // continuation size puts the last fragment ~92 KB in, far beyond tt_MAX_SAMPLE_LENGTH.
    init_pair(4000);
    publish_captured();
    set_cont_field(1, offsetof(struct tt_FragContHeader, frag_count), 64);
    set_cont_field(1, offsetof(struct tt_FragContHeader, frag_index), 1);
    deliver(1);
    EXPECT_EQ_U64(1, receiver.frag_dropped);
    EXPECT_EQ_INT(0, delivered_count);
}

static void test_sample_above_the_limit_is_refused(void) {
    init_pair(tt_MAX_SAMPLE_LENGTH + 1);
    start_capture();
    // Large-message stage 2 takes such a sample only from a context given large buffers
    // (tt_Context_set_large_buffers(), test_large_sample.c); without them it is refused as one no publish can carry.
    EXPECT_EQ_INT(tt_LARGE_SAMPLES ? tt_RET_TOO_LARGE : tt_RET_PROTOCOL_ERROR,
                  tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_INT(0, datagram_count);
    EXPECT_EQ_U32(0, pub.seq_no);
    EXPECT_EQ_U32(sizeof(struct tt_Header), sender.tx_tail);
    // Control: the limit itself is accepted.
    sample_len = tt_MAX_SAMPLE_LENGTH;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_INT((int)frag_count_for(&sender, tt_MAX_SAMPLE_LENGTH), datagram_count);
    for (int d = 0; d < datagram_count; d++) {
        deliver(d);
    }
    expect_delivered_once(tt_MAX_SAMPLE_LENGTH);
}

static void test_batch_ahead_of_a_fragmented_sample_goes_first(void) {
    // A batching publisher leaves a small DATA pending; the large sample behind it must not overtake
    // it, and must not stay in tx_buffer either.
    init_pair_with(100, &(struct pair_qos) {.pub_options = &(struct tt_PublisherOptions) {.batch = true}});
    start_capture();
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_INT(0, datagram_count); // batched
    sample_len = 2800;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_INT(3, datagram_count);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_DATA, submessage_of(0)->type);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_FIRST, submessage_of(1)->type);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_CONT, submessage_of(2)->type);
    EXPECT_EQ_U32(sizeof(struct tt_Header), sender.tx_tail);
    deliver(0);
    EXPECT_EQ_INT(1, delivered_count);
    deliver(1);
    deliver(2);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_U32(2800, delivered_len);
    EXPECT_TRUE(delivered_intact);
}

static void test_zero_copy_publish_fragments_too(void) {
    init_pair(2800);
    sender_topic.data_encode_inplace = sized_encode_inplace;
    publish_captured();
    EXPECT_EQ_INT(2, datagram_count);
    EXPECT_EQ_U32(2835, total_captured_bytes()); // byte-for-byte what the staging path sends
    deliver(1);
    deliver(0);
    expect_delivered_once(2800);
}

// --- system calls: the batching that makes a fragmented sample cost one send again -------------------

static void set_peers(uint8_t n) {
    while (pub.peers[0].context_id != tt_CONTEXT_ID_INVALID) {
        forget_peer(pub.peers, pub.peers[0].context_id); // init_pair()'s broadcast readers, replaced by these
    }
    for (uint8_t i = 0; i < n; i++) {
        pub.peers[i] = (struct tt_Peer) {.ip = RECEIVER_IP + i, .port = PORT, .context_id = (uint8_t)(RECEIVER_ID + i)};
    }
}

static void test_a_fragmented_sample_is_one_batch_per_destination(void) {
    // Broadcast: both fragments in one tt_send_batch() - one sendmmsg() on Linux, where each fragment used
    // to be its own sendmsg().
    init_pair(2800);
    publish_captured();
    EXPECT_EQ_INT(2, datagram_count);
    EXPECT_EQ_INT(1, test_mock_send_batch_call_count);
    EXPECT_EQ_U64(2, sender.tx_datagrams);

    // Two unicast peers: one batch each, carrying the whole sample, so each receiver gets its fragments
    // back to back.
    init_pair(2800);
    set_peers(2);
    publish_captured();
    EXPECT_EQ_INT(4, datagram_count);
    EXPECT_EQ_INT(2, test_mock_send_batch_call_count);
    EXPECT_EQ_INT(4, test_mock_send_to_call_count);
    EXPECT_EQ_U32(RECEIVER_IP, test_mock_send_to_ips[0]);
    EXPECT_EQ_U32(RECEIVER_IP, test_mock_send_to_ips[1]);
    EXPECT_EQ_U32(RECEIVER_IP + 1, test_mock_send_to_ips[2]);
    EXPECT_EQ_U32(RECEIVER_IP + 1, test_mock_send_to_ips[3]);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_FIRST, submessage_of(2)->type);
    EXPECT_EQ_U64(4, sender.tx_datagrams);
}

static void test_one_datagram_to_one_destination_is_not_batched(void) {
    // Control, and the p1-p3 promise: a sample that fits one datagram, sent to one destination, takes the
    // path it always took - no batch - whether broadcast or unicast.
    init_pair(1000);
    publish_captured();
    EXPECT_EQ_INT(1, datagram_count);
    EXPECT_EQ_INT(0, test_mock_send_batch_call_count);

    init_pair(1000);
    set_peers(1);
    publish_captured();
    EXPECT_EQ_INT(1, datagram_count);
    EXPECT_EQ_INT(0, test_mock_send_batch_call_count);
    EXPECT_EQ_INT(1, test_mock_send_to_call_count);
}

static void test_one_datagram_to_several_peers_is_one_batch(void) {
    // The flush path's own case for sendmmsg: the same bytes to each unicast peer.
    init_pair(1000);
    set_peers(2);
    publish_captured();
    EXPECT_EQ_INT(2, datagram_count);
    EXPECT_EQ_INT(1, test_mock_send_batch_call_count);
    EXPECT_EQ_INT(2, test_mock_send_to_call_count);
    EXPECT_EQ_U64(2, sender.tx_datagrams);
    deliver(0);
    expect_delivered_once(1000);
}

static void test_a_failed_batch_fails_the_publish(void) {
    init_pair(2800);
    test_mock_send_return_override = true;
    test_mock_send_return = -1;
    start_capture();
    EXPECT_EQ_INT(tt_RET_IO_ERROR, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_U32(0, pub.seq_no);
    EXPECT_EQ_U32(sizeof(struct tt_Header), sender.tx_tail); // nothing left behind in tx_buffer
}

// Rewrites captured datagram d as a node of the opposite byte order would have sent it: every framing
// field swapped and the header stamped REVERSE. The payload is the codec's business and is left alone.
static void make_reverse_endian(int d) {
    struct tt_Header* header = (struct tt_Header*)datagrams[d];
    header->magic_value = REVERSE_MAGIC_VALUE;
    struct tt_SubmessageHeader* submessage_header =
        (struct tt_SubmessageHeader*)(datagrams[d] + sizeof(struct tt_Header));
    uint8_t type = submessage_header->type;
    submessage_header->length = _tt_bswap_16(submessage_header->length);
    uint8_t* body = datagrams[d] + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader);
    if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST) {
        struct tt_DataHeader* data_header = &((struct tt_FragFirstHeader*)body)->data;
        data_header->endpoint_id = _tt_bswap_32(data_header->endpoint_id);
        data_header->seq_no = _tt_bswap_32(data_header->seq_no);
        data_header->timestamp = _tt_bswap_32(data_header->timestamp);
        data_header->entity_id = _tt_bswap_32(data_header->entity_id);
    } else {
        struct tt_FragContHeader* cont = (struct tt_FragContHeader*)body;
        cont->entity_id = _tt_bswap_32(cont->entity_id);
        cont->seq_no = _tt_bswap_32(cont->seq_no);
    }
}

static void test_reverse_endian_fragments_reassemble(void) {
    // The slot key is read through rd32() in both headers; a raw read in either would put the two
    // fragments of one sample in two slots, and neither would ever complete. A non-zero entity_id and
    // seq_no, so that a missed swap cannot pass by accident on a symmetric value.
    init_pair(2800);
    pub.endpoint.entity_id = 0x01020304;
    pub.seq_no = 0x00050000;
    publish_captured();
    EXPECT_EQ_INT(2, datagram_count);
    make_reverse_endian(0);
    make_reverse_endian(1);
    deliver(1); // last first, so parking is exercised on the swapped path too
    deliver(0);
    expect_delivered_once(2800);
    EXPECT_EQ_U64(0, receiver.frag_dropped);
}

// A reliable cache with room for sixteen whole samples of the largest size; TEST_RELIABLE_CACHE's is
// sized for small ones.
#define FRAG_CACHE_DEPTH 16
static struct tt_ReliableCacheIndex frag_cache_index[FRAG_CACHE_DEPTH];
static uint8_t frag_cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(FRAG_CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(4096))];
static struct tt_ReliableCache frag_cache;

static void make_reliable(void) {
    memset(frag_cache_index, 0, sizeof(frag_cache_index));
    memset(&frag_cache, 0, sizeof(frag_cache));
    frag_cache.index = frag_cache_index;
    frag_cache.capacity = FRAG_CACHE_DEPTH;
    frag_cache.depth = FRAG_CACHE_DEPTH;
    frag_cache.arena = frag_cache_arena;
    frag_cache.arena_size = (uint32_t)sizeof(frag_cache_arena);
    pub.reliable_cache = &frag_cache;
}

static const struct tt_Peer receiver_peer = {.ip = RECEIVER_IP, .port = PORT, .context_id = RECEIVER_ID};

static void test_sample_datagrams_matches_what_is_sent(void) {
    // tt_sample_datagrams() is what a caller sizes a cache in seq_no with (rmw_tickle does); it has to
    // agree with the publish path exactly, at every boundary.
    static const uint32_t sizes[] = {1,    1444, 1448, 1449, 1450, 2800,
                                     2901, 2902, 2904, 4000, 8000, tt_MAX_SAMPLE_LENGTH};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        init_pair(sizes[i]);
        publish_captured();
        EXPECT_EQ_INT(datagram_count, (int)tt_sample_datagrams(sizes[i]));
        EXPECT_EQ_U32(pub.seq_no, tt_sample_datagrams(sizes[i]));
    }
}

static void test_sample_cache_bytes_bounds_what_is_cached(void) {
    // tt_sample_cache_bytes() is what rmw_tickle sizes an arena in samples with: never less than what one
    // sample really takes, and not more than 4 bytes a datagram over it.
    static const uint32_t sizes[] = {1,    1444, 1448, 1449, 1450, 2800,
                                     2901, 2902, 2904, 4000, 8000, tt_MAX_SAMPLE_LENGTH};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        init_pair(sizes[i]);
        make_reliable();
        publish_captured();
        uint32_t cached = frag_cache.tail;
        uint32_t bound = tt_sample_cache_bytes(sizes[i]);
        EXPECT_TRUE(bound >= cached);
        EXPECT_TRUE(bound <= cached + (4U * tt_sample_datagrams(sizes[i])));
    }
}

static void test_every_datagram_takes_its_own_seq_no(void) {
    // DATAFRAG_PLAN.md section 13: DATA, FRAG_FIRST and each FRAG_CONT consume consecutive seq_no, and a
    // sample is named by its first datagram's.
    init_pair(4000);
    publish_captured();
    EXPECT_EQ_INT(3, datagram_count);
    const struct tt_FragFirstHeader* first = (const struct tt_FragFirstHeader*)(submessage_of(0) + 1);
    EXPECT_EQ_U32(1, first->data.seq_no);
    for (int d = 1; d < 3; d++) {
        const struct tt_FragContHeader* cont = (const struct tt_FragContHeader*)(submessage_of(d) + 1);
        EXPECT_EQ_U32((uint32_t)d + 1, cont->seq_no);
        EXPECT_EQ_INT(d, cont->frag_index);
    }
    EXPECT_EQ_U32(3, pub.seq_no);
    sample_len = 100; // control: a whole DATA takes exactly one
    publish_captured();
    EXPECT_EQ_U32(4, ((const struct tt_DataHeader*)(submessage_of(0) + 1))->seq_no);
    EXPECT_EQ_U32(4, pub.seq_no);
}

static void test_retransmission_resends_only_the_lost_datagram(void) {
    init_pair(2800);
    make_reliable();
    publish_captured();
    EXPECT_EQ_INT(2, datagram_count);
    uint32_t original_len = datagram_len[1];
    deliver(0); // the original's continuation, seq_no 2, is lost

    start_capture();
    struct tt_ReliableCache* cache = pub.reliable_cache;
    EXPECT_TRUE(!retransmit_one_sample(&sender, &pub, cache, reliable_cache_depth(cache), 2, &receiver_peer));
    EXPECT_EQ_INT(1, datagram_count);               // that datagram, not the whole sample
    EXPECT_EQ_INT(1, test_mock_send_to_call_count); // unicast, to the node that asked
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_CONT, submessage_of(0)->type);
    EXPECT_EQ_INT(RECEIVER_ID, submessage_of(0)->receiver); // Karn: addressed, so it reads as a recovery
    EXPECT_EQ_U32(2, ((const struct tt_FragContHeader*)(submessage_of(0) + 1))->seq_no);
    EXPECT_EQ_U32(original_len, datagram_len[0]); // unpadded, byte for byte what was lost
    deliver(0);
    expect_delivered_once(2800);

    // And the first fragment on its own, when that is what is asked for.
    start_capture();
    EXPECT_TRUE(!retransmit_one_sample(&sender, &pub, cache, reliable_cache_depth(cache), 1, &receiver_peer));
    EXPECT_EQ_INT(1, datagram_count);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_FIRST, submessage_of(0)->type);
}

static void test_original_and_retransmission_agree_on_fragment_count(void) {
    // 2901 bytes fill exactly two fragments; the cached record is padded to 2904, which needs three.
    // The original is padded the same way, so the two agree - otherwise a retransmission could never
    // complete a slot the original had started, since every one of its fragments would disagree
    // about frag_count and be dropped.
    init_pair(1447 + 1454);
    make_reliable();
    publish_captured();
    EXPECT_EQ_INT(3, datagram_count);
    const struct tt_FragFirstHeader* first =
        (const struct tt_FragFirstHeader*)(datagrams[0] + sizeof(struct tt_Header) +
                                           sizeof(struct tt_SubmessageHeader));
    uint8_t original_count = first->frag_count;
    deliver(0);
    deliver(2); // the middle one, seq_no 2, is lost

    start_capture();
    struct tt_ReliableCache* cache = pub.reliable_cache;
    EXPECT_TRUE(!retransmit_one_sample(&sender, &pub, cache, reliable_cache_depth(cache), 2, &receiver_peer));
    EXPECT_EQ_INT(1, datagram_count);
    const struct tt_FragContHeader* cont = (const struct tt_FragContHeader*)(submessage_of(0) + 1);
    EXPECT_EQ_INT(original_count, cont->frag_count);
    EXPECT_EQ_INT(1, cont->frag_index);
    deliver(0);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U64(0, receiver.frag_dropped);
}

static void test_durability_backlog_sends_fragmented_samples(void) {
    // A TRANSIENT_LOCAL BEST_EFFORT writer: it keeps a backlog, and only a backlog.
    static uint64_t storage[(size_t)512U * 1024U / sizeof(uint64_t)];
    const struct tt_QosProfile durable = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = FRAG_CACHE_DEPTH},
                                          .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT},
                                          .durability = {.kind = tt_QOS_DURABILITY_TRANSIENT_LOCAL}};
    init_pair_with(
        2800,
        &(struct pair_qos) {.pub_qos = &durable, .pub_storage = storage, .pub_storage_capacity = sizeof(storage)});
    publish_captured();
    publish_captured();

    start_capture();
    struct tt_Peer target = receiver_peer;
    deliver_durability_backlog(&sender, &pub, &target);
    EXPECT_EQ_INT(4, datagram_count);
    EXPECT_EQ_INT(4, test_mock_send_to_call_count);
    for (int d = 0; d < datagram_count; d++) {
        deliver(d);
    }
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_TRUE(delivered_intact);
}

// --- a lossy link, simulated: the unit-level shape of the benchmark's c6 cell -------------------
//
// Two real nodes, a RELIABLE Publisher and Subscriber, exchanging every datagram either sends - DATA
// fragments, ACKNACKs, Heartbeats, retransmissions - through a channel that drops a fixed fraction of
// them, on a simulated clock that jumps to whichever node's next scheduled entry is due. Nothing is
// timed on the wall clock, so the result is the protocol's and not the machine's.
//
// c6 (p4 at 5% loss) went VOID on 2026-09-26 because TickLE's recovery did not finish inside the
// harness's drain window. What is asserted here is the property that failed there: every sample gets
// through, exactly once and in order, within a bounded time.

#define SIM_QUEUE 4096
#define SIM_SAMPLES 200
#define SIM_PUBLISH_SPACING (100 * tt_MICROSECOND)

struct sim_datagram {
    uint8_t from;
    int16_t tag_stream; // the multi-stream simulation's: which stream's original send this is, -1 for anything else
    int16_t tag_sample; // and of which sample
    uint32_t len;
    uint8_t bytes[tt_MAX_BUFFER_LENGTH];
};
static struct sim_datagram sim_queue[SIM_QUEUE];
static uint32_t sim_head;
static uint32_t sim_tail;
static uint8_t sim_acting; // which node's code is running, so a capture knows who sent it
static uint32_t sim_loss_per_mille;
static uint32_t sim_rng;
static uint64_t sim_sent[3];    // per node id: datagrams handed to the channel
static uint64_t sim_dropped[3]; // per node id: of those, lost
// The multi-stream simulation's tags (multi_run()): set around a publish, so its original datagrams can be told apart
// from what the protocol sends; and per stream and sample, how many of them crossed the channel.
#define MS_MAX_STREAMS 16
#define MS_SAMPLES 40
static int16_t sim_tag_stream = -1;
static int16_t sim_tag_sample = -1;
static uint32_t sim_tagged; // datagrams tagged since the tag was set
static uint8_t sim_on_wire[MS_MAX_STREAMS][MS_SAMPLES];

static void sim_capture(const void* buf, size_t len) {
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    EXPECT_TRUE(len <= tt_MAX_BUFFER_LENGTH);
    struct sim_datagram* d = &sim_queue[sim_tail % SIM_QUEUE];
    d->from = sim_acting;
    d->tag_stream = -1;
    d->tag_sample = -1;
    if (sim_tag_stream >= 0) { // a fragment of the tagged sample, not what else the publish sent (a HEARTBEAT)
        static uint8_t classic[tt_MAX_BUFFER_LENGTH * 2];
        size_t classic_len = test_classic_form(buf, len, classic);
        uint8_t type = classic_len > sizeof(struct tt_Header)
                           ? ((const struct tt_SubmessageHeader*)(classic + sizeof(struct tt_Header)))->type
                           : 0;
        if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST || type == tt_SUBMESSAGE_TYPE_FRAG_CONT) {
            d->tag_stream = sim_tag_stream;
            d->tag_sample = sim_tag_sample;
            sim_tagged++;
        }
    }
    d->len = (uint32_t)len;
    memcpy(d->bytes, buf, len);
    sim_tail++;
    sim_sent[sim_acting]++;
}

static bool sim_lost(void) {
    sim_rng = (sim_rng * 1103515245U) + 12345U;
    return ((sim_rng >> 16) % 1000U) < sim_loss_per_mille;
}

// Delivers everything in flight, and whatever that provokes, until the channel is empty.
static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        if (sim_lost()) {
            sim_dropped[d->from]++;
            continue;
        }
        if (d->tag_stream >= 0) {
            sim_on_wire[d->tag_stream][d->tag_sample]++;
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

static uint32_t sim_last_seq;
static bool sim_in_order;
static void sim_on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)data;
    // Monotonic, not contiguous: a sample is named by its first datagram's seq_no, and a fragmented one
    // takes one per datagram (DATAFRAG_PLAN.md section 13).
    sim_in_order = sim_in_order && (delivered_count == 0 || seq_no > (uint16_t)sim_last_seq);
    sim_last_seq = seq_no;
    delivered_count++;
}

// As the benchmark's server sizes it: at least the tracking window, so a sample held ahead of a gap can
// never overflow into the re-request fallback. With fewer slots than the window, a gap that outlives
// the buffer turns every later sample into a re-request, and at 20% loss that alone drove this
// simulation to ~1800 datagrams a sample, fragmented or not - a real pathology, but of a
// misconfigured Subscriber, and not the one c6 measures.
#define SIM_REORDER_SLOTS tt_RELIABLE_BITMAP_BITS
#define SIM_REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + 2816)
static uint64_t sim_reorder[SIM_REORDER_SLOTS * SIM_REORDER_SLOT_BYTES / sizeof(uint64_t)];
// Records are one datagram each (DATAFRAG_PLAN.md section 13), so the cache is sized in datagrams.
#define SIM_CACHE_DEPTH 2048
static struct tt_ReliableCacheIndex sim_cache_index[SIM_CACHE_DEPTH];
static uint8_t sim_cache_arena[tt_RELIABLE_CACHE_ARENA_BYTES(SIM_CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache sim_cache;

// Runs whatever is due on both nodes, one step of simulated time on.
static bool sim_step(void) {
    uint64_t next = sim_next_due();
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

// Publishes SIM_SAMPLES samples of `size` bytes through a link losing loss_per_mille of every datagram in
// both directions, then lets the protocol run until the Subscriber has them all or `budget` has passed.
// KEEP_ALL, as c6 is: a publish that would outrun the acknowledgements waits, as a real writer blocks.
// Returns the simulated time recovery took, after the last publish.
static uint64_t sim_run(uint32_t size, uint32_t loss_per_mille, uint64_t budget) {
    // RELIABLE KEEP_ALL with a 10 ms HEARTBEAT, and a RELIABLE reader with a window-wide reorder buffer. White-box: the
    // writer's retained samples are the sim's own SIM_CACHE_DEPTH-datagram cache, swapped in below.
    static uint64_t created_storage[(size_t)64U * 1024U / sizeof(uint64_t)];
    const struct tt_QosProfile pub_qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL, .depth = 1},
                                          .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    struct tt_QosProfile sub_qos = TEST_QOS_RELIABLE_READER;
    sub_qos.reliability.reorder_slots = SIM_REORDER_SLOTS;
    init_pair_with(size, &(struct pair_qos) {
                             .pub_qos = &pub_qos,
                             .pub_options = &(struct tt_PublisherOptions) {.heartbeat_period_ns = 10 * tt_MILLISECOND},
                             .pub_storage = created_storage,
                             .pub_storage_capacity = sizeof(created_storage),
                             .sub_qos = &sub_qos,
                             .sub_callback = sim_on_data,
                             .sub_storage = sim_reorder,
                             .sub_storage_capacity = sizeof(sim_reorder),
                         });
    sim_head = sim_tail = 0;
    sim_loss_per_mille = loss_per_mille;
    sim_rng = 1;
    memset(sim_sent, 0, sizeof(sim_sent));
    memset(sim_dropped, 0, sizeof(sim_dropped));
    sim_last_seq = 0;
    sim_in_order = true;

    memset(sim_cache_index, 0, sizeof(sim_cache_index));
    memset(&sim_cache, 0, sizeof(sim_cache));
    sim_cache.index = sim_cache_index;
    sim_cache.capacity = SIM_CACHE_DEPTH;
    sim_cache.depth = SIM_CACHE_DEPTH;
    sim_cache.arena = sim_cache_arena;
    sim_cache.arena_size = (uint32_t)sizeof(sim_cache_arena);
    pub.reliable_cache = &sim_cache; // c6 is KEEP_ALL, where a Publisher never stops answering for a sample

    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
    for (int i = 0; i < SIM_SAMPLES; i++) {
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
        sim_drain();
        test_mock_now += SIM_PUBLISH_SPACING;
    }
    uint64_t published = test_mock_now;
    while (delivered_count < SIM_SAMPLES && test_mock_now - published < budget) {
        if (!sim_step()) {
            break; // nothing left to happen: a stall, which the assertions below report
        }
    }
    test_mock_send_hook = NULL;
    return test_mock_now - published;
}

static void test_lossless_link_sends_two_datagrams_a_sample(void) {
    // Control for the lossy run below: the same simulation with nothing dropped.
    sim_run(2800, 0, tt_SECOND);
    EXPECT_EQ_INT(SIM_SAMPLES, delivered_count);
    EXPECT_TRUE(sim_in_order);
    EXPECT_EQ_U64(SIM_SAMPLES, receiver.frag_reassembled);
    EXPECT_EQ_U64(0, receiver.frag_abandoned);
    EXPECT_EQ_U64(0, sim_dropped[SENDER_ID]);
}

static void test_lossy_link_recovers_every_sample(void) {
    // 5% of every datagram, both ways - c6's condition. Every sample must arrive, once, in order, well
    // inside the harness's 3 s drain window; and the sender must not have needed more than a small
    // multiple of the lossless two datagrams a sample to get there (DATAFRAG_PLAN.md section 8 puts the
    // target at 2.2-2.5 on the wire; heartbeats and retransmissions are both counted here).
    uint64_t took = sim_run(2800, 50, 3 * tt_SECOND);
    EXPECT_EQ_INT(SIM_SAMPLES, delivered_count);
    EXPECT_TRUE(sim_in_order);
    EXPECT_TRUE(took < 500 * tt_MILLISECOND);
    // The channel really was lossy: roughly 5% of what the sender sent never arrived.
    EXPECT_TRUE(sim_dropped[SENDER_ID] * 100 > sim_sent[SENDER_ID] * 3);
    EXPECT_TRUE(sim_dropped[SENDER_ID] * 100 < sim_sent[SENDER_ID] * 7);
    double per_sample = (double)sim_sent[SENDER_ID] / SIM_SAMPLES;
    EXPECT_TRUE(per_sample < 3.0);
    printf("test_data_frag: 5%% loss - %d/%d delivered, recovery %.1f ms after the last publish, sender "
           "%.2f datagrams a sample (%lu of %lu lost), receiver %.2f, %lu reassemblies abandoned\n",
           delivered_count, SIM_SAMPLES, (double)took / 1e6, per_sample, (unsigned long)sim_dropped[SENDER_ID],
           (unsigned long)sim_sent[SENDER_ID], (double)sim_sent[RECEIVER_ID] / SIM_SAMPLES,
           (unsigned long)receiver.frag_abandoned);
}

static void test_loss_costs_the_lost_datagram_not_the_sample(void) {
    // DATAFRAG_PLAN.md 13.3, pre-registered: at 5% loss a datagram needs 1 / 0.95 sends on average, so a
    // k-fragment sample costs k / 0.95 datagrams when only the lost one is resent, against k / 0.95^k when
    // the whole sample is. At 1 fragment the two coincide (control); the gap is what this design buys, and
    // it grows with k: 4.21 against 4.91 at 4, 10.5 against 16.7 at 10. Loss here is 5% both ways, so an
    // ACKNACK can be lost too, and heartbeats count: within 10% of the per-datagram figure is the bar.
    static const uint32_t fragments[] = {1, 2, 4, 10};
    for (size_t f = 0; f < sizeof(fragments) / sizeof(fragments[0]); f++) {
        uint32_t k = fragments[f];
        uint32_t size = k == 1 ? 1000 : 1447 + ((k - 1) * 1454) - 100; // k fragments, the last one short
        sim_run(size, 50, 5 * tt_SECOND);
        double per_sample = (double)sim_sent[SENDER_ID] / SIM_SAMPLES;
        double per_datagram_model = (double)k / 0.95;
        double whole_sample_model = (double)k;
        for (uint32_t i = 0; i < k; i++) {
            whole_sample_model /= 0.95;
        }
        EXPECT_EQ_INT(SIM_SAMPLES, delivered_count);
        EXPECT_TRUE(sim_in_order);
        EXPECT_TRUE(per_sample < per_datagram_model * 1.10);
        if (k >= 4) {
            EXPECT_TRUE(per_sample < whole_sample_model); // measurably below what whole-sample resend costs
        }
        printf("test_data_frag: %u fragment(s) at 5%% loss - %.2f datagrams a sample (per-datagram model %.2f, "
               "whole-sample model %.2f), %d/%d delivered\n",
               k, per_sample, per_datagram_model, whole_sample_model, delivered_count, SIM_SAMPLES);
    }
}

// --- RELIABLE receiver: every datagram its own seq_no (DATAFRAG_PLAN.md section 13) ---------------

#define TEST_REORDER_SLOTS tt_RELIABLE_BITMAP_BITS // the window: fewer are refused (test_reorder_window.c)
#define TEST_REORDER_SLOT_BYTES tt_REORDER_SLOT_SIZE(1472)
static uint64_t test_reorder[TEST_REORDER_SLOTS * TEST_REORDER_SLOT_BYTES / sizeof(uint64_t)];

// init_pair() with a RELIABLE Subscriber, with TEST_REORDER_SLOTS reorder slots in test_reorder or none.
static void init_pair_reliable_receiver(uint32_t len, bool with_buffer) {
    struct tt_QosProfile qos = TEST_QOS_RELIABLE_READER;
    if (with_buffer) {
        qos.reliability.reorder_slots = TEST_REORDER_SLOTS;
    }
    // Zeroed for every pair, as before the creation API: creation frees the slots by their headers only
    // (qos_free_reorder_slots()), so payload bytes of the last test would stay, and an out-of-order run that writes
    // the same bytes to the same slots would then read as "untouched" (the control below).
    memset(test_reorder, 0, sizeof(test_reorder));
    init_pair_with(len, &(struct pair_qos) {.sub_qos = &qos,
                                            .sub_storage = test_reorder,
                                            .sub_storage_capacity = sizeof(test_reorder)});
    EXPECT_TRUE(!with_buffer || sub.reorder_slot_bytes == TEST_REORDER_SLOT_BYTES);
}

static struct tt_WriterProxy* sender_proxy(void) {
    return find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
}

static void test_reliable_fragments_reassemble_in_every_order(void) {
    // A continuation reaches a RELIABLE Subscriber only once it tracks the writer, which takes a first
    // datagram or a DATA - so one that overtakes its writer's very first datagram is not taken, stays
    // unacknowledged, and is what the ACKNACK asks for. Modelled here by resending exactly the datagrams
    // still unacknowledged, as the writer would.
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (int o = 0; o < 6; o++) {
        init_pair_reliable_receiver(4000, true);
        publish_captured();
        for (int k = 0; k < 3; k++) {
            deliver(orders[o][k]);
        }
        int resent = 0;
        for (int d = 0; d < 3; d++) {
            struct tt_WriterProxy* proxy = sender_proxy();
            uint32_t seq_no = (uint32_t)d + 1;
            if (seq_no >= proxy->ack_seq_no && !bitmap_test_bit(proxy->received_bitmap, seq_no - proxy->ack_seq_no)) {
                deliver(d);
                resent++;
            }
        }
        int ahead_of_first = 0; // continuations delivered before the first fragment
        while (orders[o][ahead_of_first] != 0) {
            ahead_of_first++;
        }
        EXPECT_EQ_INT(ahead_of_first, resent); // exactly the continuations that overtook it, nothing else
        expect_delivered_once(4000);
        EXPECT_EQ_U32(4, sender_proxy()->ack_seq_no); // all three datagrams acknowledged
        EXPECT_EQ_U32(0, sub.reorder_held);           // and nothing left behind
    }
}

static void test_reliable_missing_fragment_is_tracked_as_its_own_gap(void) {
    init_pair_reliable_receiver(4000, true);
    publish_captured();
    deliver(0);
    deliver(2);
    EXPECT_EQ_INT(0, delivered_count);
    struct tt_WriterProxy* proxy = sender_proxy();
    EXPECT_EQ_U32(2, proxy->ack_seq_no);                      // stuck at the lost datagram, seq_no 2
    EXPECT_TRUE(bitmap_test_bit(proxy->received_bitmap, 1));  // seq_no 3 held ahead of it
    EXPECT_TRUE(!bitmap_test_bit(proxy->received_bitmap, 0)); // seq_no 2 is what an ACKNACK names
    deliver(1);
    expect_delivered_once(4000);
}

static void test_reliable_sample_given_up_on_is_never_delivered_torn(void) {
    // Its first datagram arrives, the rest never do, and the writer declares them gone (an eviction
    // Heartbeat). The held first datagram must be dropped - delivering part of a sample would hand the
    // application garbage - and the next sample must still arrive intact.
    init_pair_reliable_receiver(4000, true);
    publish_captured(); // seq_no 1..3
    deliver(0);
    struct tt_WriterProxy* proxy = sender_proxy();
    advance_past_unavailable(proxy, 4);
    drain_reorder(&receiver, &sub, proxy);
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_EQ_U32(0, sub.reorder_held);
    EXPECT_TRUE(sub.reorder_abandoned >= 1);
    publish_captured(); // seq_no 4..6 - control: the stream carries on
    for (int d = 0; d < 3; d++) {
        deliver(d);
    }
    expect_delivered_once(4000);
}

static void test_reliable_fragment_without_room_is_left_unrecorded(void) {
    // No reorder buffer: a fragment cannot be held until its sample is whole, so it must not be
    // acknowledged either - an acknowledged fragment is never sent again.
    init_pair_reliable_receiver(2800, false);
    publish_captured();
    deliver(0);
    struct tt_WriterProxy* proxy = sender_proxy();
    EXPECT_TRUE(proxy == NULL || proxy->ack_seq_no <= 1);
    EXPECT_TRUE(sub.reorder_overflow >= 1);
    EXPECT_EQ_INT(0, delivered_count);
    // Control: the same datagrams with a buffer are recorded and delivered.
    init_pair_reliable_receiver(2800, true);
    publish_captured();
    deliver(0);
    deliver(1);
    expect_delivered_once(2800);
}

// --- RELIABLE receiver with no reorder buffer, once it tracks the writer (2026-10-24) -------------------------------
//
// rmw_tickle gives an unbounded or large type no reorder buffer (since 2026-10-20: its 8192-sample window is wider
// than the slots its budget pays for, and core refuses a buffer narrower than the window). Such a Subscriber took no
// fragment at all - the in-order fast path needed a slot for each, the buffer had none - so every fragmented sample was
// re-requested forever and none was delivered: the rmw acceptance case `durable` (TRANSIENT_LOCAL, 60,000-byte
// sensor_msgs/Image) received 0 of 4. These pin the repair: put together unrecorded until whole - in frag_scratch at
// first (a0d6ea3a), in the reassembly pool since (frag_pool_offer_unbuffered(), and the tests after the fast path's).

// A DATA first, which the Subscriber records and delivers, and which makes it track the writer (seq_no 1); then the
// fragmented sample under test, seq_no 2 on, captured and not yet delivered.
static void tracked_pair(uint32_t len, bool with_buffer) {
    init_pair_reliable_receiver(len, with_buffer);
    EXPECT_TRUE(with_buffer || reorder_payload_capacity(&sub) == 0); // the shape under test: no buffer
    sample_len = 100;
    publish_captured();
    EXPECT_EQ_INT(1, datagram_count);
    deliver(0);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_TRUE(sender_proxy() != NULL && sender_proxy()->ack_seq_no == 2);
    delivered_count = 0;
    sample_len = len;
    sample_seed = expected_seed = 9; // the sample's bytes differ from the DATA's, so a stale copy cannot pass
    publish_captured();
}

static void tracked_none_pair(uint32_t len) {
    tracked_pair(len, false);
}

static void test_reliable_none_fragments_in_order_are_delivered(void) {
    tracked_none_pair(4000); // 3 fragments, seq_no 2..4
    EXPECT_EQ_INT(3, datagram_count);
    deliver(0);
    deliver(1);
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_EQ_U32(4, sender_proxy()->ack_seq_no); // each datagram recorded as it lands; the sample is not whole yet
    deliver(2);
    expect_delivered_once(4000);
    EXPECT_EQ_U32(5, sender_proxy()->ack_seq_no);
    EXPECT_EQ_U32(0, sub.reorder_overflow);
}

static void test_reliable_none_lost_fragment_is_asked_for_and_completes(void) {
    tracked_none_pair(4000);
    deliver(0);
    deliver(2); // seq_no 3 lost: 4 waits in the pool, recorded
    EXPECT_EQ_INT(0, delivered_count);
    struct tt_WriterProxy* proxy = sender_proxy();
    EXPECT_EQ_U32(3, proxy->ack_seq_no);                     // the gap is seq_no 3 alone: what an ACKNACK names
    EXPECT_TRUE(bitmap_test_bit(proxy->received_bitmap, 1)); // seq_no 4 is received
    // The writer resends what the ACKNACK names, from the watermark: all three. The first, already in the pool, is
    // a duplicate that must not disturb the sample; the other two complete it.
    deliver(0);
    deliver(1);
    deliver(2);
    expect_delivered_once(4000);
    EXPECT_EQ_U32(5, sender_proxy()->ack_seq_no);
}

static void test_reliable_none_duplicate_does_not_disturb_the_sample(void) {
    // A resend of a fragment already in the pool (the writer answering an ACKNACK sent while the sample was being
    // put together) arrives mid-sample: it is not taken, and the part-built sample must survive it.
    tracked_none_pair(4000);
    deliver(0);
    deliver(1);
    deliver(0);
    deliver(1);
    EXPECT_EQ_INT(0, delivered_count);
    deliver(2);
    expect_delivered_once(4000);
    EXPECT_EQ_U32(5, sender_proxy()->ack_seq_no);
}

static void test_reliable_none_sample_given_up_on_frees_the_pool(void) {
    // Its first fragment arrives, the writer then declares the sample gone (an eviction Heartbeat). The part-built
    // sample is never delivered, and the next one - which needs a pool slot - still arrives intact.
    tracked_none_pair(4000);
    deliver(0);
    advance_past_unavailable(sender_proxy(), 5);
    EXPECT_EQ_INT(0, delivered_count);
    sample_seed = expected_seed = 11;
    publish_captured(); // seq_no 5..7
    for (int d = 0; d < 3; d++) {
        deliver(d);
    }
    expect_delivered_once(4000);
    EXPECT_EQ_U32(8, sender_proxy()->ack_seq_no);
}

static void test_reliable_two_writers_interleaved(void) {
    // Two writers of one topic count from 1 alike, and a fragment is held even in order - the per-writer
    // slot offset is what keeps their datagrams from colliding.
    init_pair_reliable_receiver(2800, true);
    static struct tt_Context second;
    static struct tt_Publisher second_pub;
    init_bare_node(&second, 3);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&second), &second_pub, &sender_topic,
                                                      ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    match_by_broadcast(&second_pub);
    publish_captured();
    uint8_t first_writer[2][tt_MAX_BUFFER_LENGTH];
    uint32_t first_len[2];
    for (int d = 0; d < 2; d++) {
        memcpy(first_writer[d], datagrams[d], datagram_len[d]);
        first_len[d] = datagram_len[d];
    }
    start_capture();
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&second_pub, (struct tt_Data*)&sample_len));
    EXPECT_TRUE(process_packet(&receiver, first_writer[0], 0, first_len[0], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    EXPECT_TRUE(process_packet(&receiver, datagrams[0], 0, datagram_len[0], SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
    EXPECT_TRUE(process_packet(&receiver, first_writer[1], 0, first_len[1], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    EXPECT_TRUE(process_packet(&receiver, datagrams[1], 0, datagram_len[1], SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_U32(0, sub.reorder_overflow);
}

// --- RELIABLE receiver: the in-order fast path (2026-10-06) -----------------------------------------
//
// A fragment used to be stored in its reorder slot even in order, so every sample wrote its whole payload
// into the reorder buffer before being copied out again - at the bench's 11.6 MB buffer, first-touch page
// faults on every sample of the first lap. In order, a sample is now put together in the node's
// frag_scratch and never touches the buffer; anything else falls back to it. The buffer's bytes are the
// instrument: a snapshot taken before a sample must be what is there after it.

static uint64_t reorder_snapshot[sizeof(test_reorder) / sizeof(uint64_t)];

static void snapshot_reorder(void) {
    memcpy(reorder_snapshot, test_reorder, sizeof(test_reorder));
}

static bool reorder_untouched(void) {
    return memcmp(reorder_snapshot, test_reorder, sizeof(test_reorder)) == 0;
}

// The writer's first sample, which goes through the buffer: it is what creates the WriterProxy, and with
// no tracking yet there is no "next expected" for the fast path to compare with.
static void first_contact_sample(uint32_t len) {
    init_pair_reliable_receiver(len, true);
    publish_captured();
    for (int d = 0; d < datagram_count; d++) {
        deliver(d);
    }
    expect_delivered_once(len);
    delivered_count = 0;
    sample_seed = expected_seed = 9; // the next sample's bytes differ, so a stale copy cannot pass
}

static uint32_t last_delivered_seq;
static bool delivered_ascending;
static void ordered_on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)data;
    delivered_ascending = delivered_ascending && (delivered_count == 0 || seq_no > (uint16_t)last_delivered_seq);
    last_delivered_seq = seq_no;
    delivered_count++;
}

static void test_reliable_in_order_sample_never_touches_the_reorder_buffer(void) {
    first_contact_sample(4000);
    for (int round = 0; round < 3; round++) {
        delivered_count = 0;
        snapshot_reorder();
        publish_captured();
        EXPECT_EQ_INT(3, datagram_count);
        for (int d = 0; d < 3; d++) {
            deliver(d);
        }
        expect_delivered_once(4000);
        EXPECT_TRUE(reorder_untouched()); // not one byte of the buffer written
        EXPECT_EQ_U32(0, sub.reorder_held);
        EXPECT_EQ_U32(7 + (3 * (uint32_t)round), sender_proxy()->ack_seq_no);      // every datagram acknowledged
        EXPECT_EQ_U32(sender_proxy()->ack_seq_no, sender_proxy()->reorder_cursor); // the cursor kept up with it
        EXPECT_TRUE(receiver.frag_fast_sub == NULL);                               // and the scratch released
    }
}

static void test_reliable_out_of_order_fragments_still_deliver_through_the_buffer(void) {
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (int o = 0; o < 6; o++) {
        first_contact_sample(4000);
        snapshot_reorder();
        publish_captured();
        for (int k = 0; k < 3; k++) {
            deliver(orders[o][k]);
        }
        expect_delivered_once(4000);
        EXPECT_EQ_U32(7, sender_proxy()->ack_seq_no);
        EXPECT_EQ_U32(0, sub.reorder_held);
        EXPECT_TRUE(receiver.frag_fast_sub == NULL);
        // Control for the test above: only the in-order arrival leaves the buffer as it was.
        EXPECT_TRUE(reorder_untouched() == (o == 0));
    }
}

static void test_reliable_fast_path_interrupted_by_a_lost_fragment(void) {
    // Four datagrams (5000 bytes). Fragments 0 and 1 arrive in order and are put together in the scratch;
    // 2 is lost and 3 arrives ahead of it - the first two must move into the buffer, since they are
    // acknowledged and will never be sent again. The retransmission of 2 then completes the sample: once,
    // intact, in order with the samples around it. And the next sample takes the fast path again.
    first_contact_sample(5000);
    sub.callback = ordered_on_data;
    delivered_ascending = true;
    publish_captured();
    EXPECT_EQ_INT(4, datagram_count);
    deliver(0);
    deliver(1);
    // A Heartbeat in between drains this writer with the sample part-way through: the drain moves the
    // reorder_cursor up to the watermark, past the sample's start, where the move into the buffer must
    // bring it back - or the next drain starts after the fragments it moved in.
    drain_reorder(&receiver, &sub, sender_proxy());
    EXPECT_TRUE(receiver.frag_fast_sub == &sub); // control: the interruption interrupts something
    deliver(3);
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_TRUE(receiver.frag_fast_sub == NULL);
    EXPECT_EQ_U32(3, sub.reorder_held); // 0 and 1 moved in, 3 stored
    struct tt_WriterProxy* proxy = sender_proxy();
    EXPECT_EQ_U32(7, proxy->ack_seq_no); // stuck at the lost datagram
    deliver(2);                          // its retransmission
    expect_delivered_once(5000);
    deliver(2); // and a duplicate of it, and of the rest
    deliver(1);
    deliver(3);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U32(0, sub.reorder_held);
    EXPECT_EQ_U32(0, sub.reorder_abandoned);
    EXPECT_EQ_U32(9, proxy->ack_seq_no);

    snapshot_reorder();
    publish_captured();
    for (int d = 0; d < 4; d++) {
        deliver(d);
    }
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_TRUE(delivered_intact);
    EXPECT_TRUE(delivered_ascending);
    EXPECT_TRUE(reorder_untouched()); // the fast path again
}

static void test_reliable_fast_path_sample_given_up_on_is_never_delivered_torn(void) {
    // The watermark moves under a sample the scratch holds part of - the writer declares the rest gone.
    // The partial sample must be dropped and counted, never delivered, and the scratch freed.
    first_contact_sample(5000);
    publish_captured(); // seq_no 5..8
    deliver(0);
    deliver(1);
    EXPECT_TRUE(receiver.frag_fast_sub == &sub);
    struct tt_WriterProxy* proxy = sender_proxy();
    advance_past_unavailable(proxy, 9);
    drain_reorder(&receiver, &sub, proxy);
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_TRUE(receiver.frag_fast_sub == NULL);
    EXPECT_EQ_U32(0, sub.reorder_held);
    EXPECT_EQ_U32(2, sub.reorder_abandoned);
    publish_captured(); // control: the stream carries on
    for (int d = 0; d < 4; d++) {
        deliver(d);
    }
    expect_delivered_once(5000);
}

// Captures one sample of `writer` into its own copy, so two writers' datagrams can be interleaved.
struct captured_sample {
    uint8_t bytes[4][tt_MAX_BUFFER_LENGTH * 2];
    uint32_t len[4];
    int count;
};

static void capture_sample(struct tt_Publisher* writer, struct captured_sample* out) {
    start_capture();
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(writer, (struct tt_Data*)&sample_len));
    test_mock_send_hook = NULL;
    EXPECT_TRUE(datagram_count <= 4);
    out->count = datagram_count;
    for (int d = 0; d < datagram_count && d < 4; d++) {
        memcpy(out->bytes[d], datagrams[d], datagram_len[d]);
        out->len[d] = datagram_len[d];
    }
}

static void test_reliable_two_writers_interleaved_after_first_contact(void) {
    // Two writers, both tracked, their in-order fragments interleaved datagram by datagram, three samples
    // each. The scratch can hold one sample at a time; the other writer's must not corrupt it, and every
    // sample of both must arrive once and intact.
    init_pair_reliable_receiver(4000, true);
    static struct tt_Context second;
    static struct tt_Publisher second_pub;
    init_bare_node(&second, 3);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&second), &second_pub, &sender_topic,
                                                      ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    match_by_broadcast(&second_pub);
    static struct captured_sample one;
    static struct captured_sample two;
    int expected = 0;
    for (int round = 0; round < 3; round++) {
        sample_seed = expected_seed = (uint8_t)(20 + round);
        capture_sample(&pub, &one);
        capture_sample(&second_pub, &two);
        EXPECT_EQ_INT(3, one.count);
        EXPECT_EQ_INT(3, two.count);
        for (int d = 0; d < 3; d++) {
            EXPECT_TRUE(process_packet(&receiver, one.bytes[d], 0, one.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
            EXPECT_TRUE(process_packet(&receiver, two.bytes[d], 0, two.len[d], SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
        }
        expected += 2;
        EXPECT_EQ_INT(expected, delivered_count);
        EXPECT_TRUE(delivered_intact);
        EXPECT_EQ_U32(4000, delivered_len);
    }
    EXPECT_EQ_U32(0, sub.reorder_held);
    EXPECT_EQ_U32(0, sub.reorder_overflow);
    EXPECT_EQ_U32(0, sub.reorder_abandoned);
    EXPECT_TRUE(receiver.frag_fast_sub == NULL);
    // Control: the same two writers one after the other, each sample whole before the next starts, take
    // the fast path and leave the buffer alone.
    snapshot_reorder();
    capture_sample(&pub, &one);
    capture_sample(&second_pub, &two);
    for (int d = 0; d < 3; d++) {
        EXPECT_TRUE(process_packet(&receiver, one.bytes[d], 0, one.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    for (int d = 0; d < 3; d++) {
        EXPECT_TRUE(process_packet(&receiver, two.bytes[d], 0, two.len[d], SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(expected + 2, delivered_count);
    EXPECT_TRUE(delivered_intact);
    EXPECT_TRUE(reorder_untouched());
}

// --- RELIABLE receiver with no reorder buffer: the reassembly pool holds what order cannot (2026-10-24) ------------
//
// The first repair (a0d6ea3a) put a no-buffer Subscriber's sample together in frag_scratch, one sample at a time and
// only in order, and dropped it when anything disturbed it. The rmw acceptance case `fragstream` (two writers of
// 60,000-byte Images and one of 30,000-byte PointCloud2s, RELIABLE VOLATILE KEEP_LAST 10, 30 Hz) then lost 11 of 30
// Images of one writer on a clean link and 42 of 90 samples at 1% loss. Each of these was red on that version.

// A fragment lost: only it is resent, and the sample completes - the fragments after it were kept.
static void test_reliable_none_lost_fragment_alone_completes(void) {
    tracked_none_pair(4000); // seq_no 2..4
    deliver(0);
    deliver(2); // seq_no 3 lost
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_EQ_U32(3, sender_proxy()->ack_seq_no); // the gap is seq_no 3 alone
    deliver(1);
    expect_delivered_once(4000);
    EXPECT_EQ_U32(5, sender_proxy()->ack_seq_no);
}

// A whole sample ahead of an incomplete one waits for it, and follows it in order.
static void test_reliable_none_sample_ahead_follows_in_order(void) {
    tracked_none_pair(4000);
    static struct captured_sample first;
    static struct captured_sample second;
    first.count = datagram_count; // tracked_none_pair()'s sample, seq_no 2..4
    for (int d = 0; d < datagram_count && d < 4; d++) {
        memcpy(first.bytes[d], datagrams[d], datagram_len[d]);
        first.len[d] = datagram_len[d];
    }
    capture_sample(&pub, &second); // seq_no 5..7
    EXPECT_EQ_INT(3, first.count);
    EXPECT_EQ_INT(3, second.count);
    EXPECT_TRUE(process_packet(&receiver, first.bytes[0], 0, first.len[0], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    for (int d = 0; d < 3; d++) {
        EXPECT_TRUE(process_packet(&receiver, second.bytes[d], 0, second.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(0, delivered_count);            // whole, but not next
    EXPECT_EQ_U32(3, sender_proxy()->ack_seq_no); // the gap is the first sample's seq_no 3 and 4
    for (uint32_t offset = 2; offset <= 4; offset++) {
        EXPECT_TRUE(bitmap_test_bit(sender_proxy()->received_bitmap, offset)); // the second one recorded: 5..7
    }
    for (int d = 1; d < 3; d++) {
        EXPECT_TRUE(process_packet(&receiver, first.bytes[d], 0, first.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(2, delivered_count); // both, so in order: a later one first would have discarded the earlier
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_U32(0, sub.out_of_order_discarded);
    EXPECT_EQ_U32(8, sender_proxy()->ack_seq_no);
}

// Two tracked writers whose fragments interleave datagram by datagram: every sample of both arrives once.
static void test_reliable_none_two_writers_interleaved(void) {
    tracked_none_pair(4000);
    for (int d = 0; d < 3; d++) {
        deliver(d);
    }
    expect_delivered_once(4000);
    delivered_count = 0;
    static struct tt_Context second;
    static struct tt_Publisher second_pub;
    init_bare_node(&second, 3);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&second), &second_pub, &sender_topic,
                                                      ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    match_by_broadcast(&second_pub);
    static struct captured_sample one;
    static struct captured_sample two;
    sample_len = 100; // a DATA from the second writer first, so the Subscriber tracks it
    capture_sample(&second_pub, &two);
    EXPECT_TRUE(process_packet(&receiver, two.bytes[0], 0, two.len[0], SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
    EXPECT_EQ_INT(1, delivered_count);
    delivered_count = 0;
    sample_len = 4000;
    for (int round = 0; round < 3; round++) {
        capture_sample(&pub, &one);
        capture_sample(&second_pub, &two);
        for (int d = 0; d < 3; d++) {
            EXPECT_TRUE(process_packet(&receiver, one.bytes[d], 0, one.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
            EXPECT_TRUE(process_packet(&receiver, two.bytes[d], 0, two.len[d], SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
        }
    }
    EXPECT_EQ_INT(6, delivered_count);
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_U32(4000, delivered_len);
    EXPECT_EQ_U32(0, sub.reorder_overflow);
}

// A sample part-recorded in the pool is not reclaimed for a newer one when the pool is full: its recorded datagrams
// would be lost for good. Here a second writer's partial samples fill the pool while the first writer's sample, its
// first datagram recorded, waits on the other two.
static void test_reliable_none_recorded_sample_survives_a_full_pool(void) {
    tracked_none_pair(4000); // the first writer's sample, seq_no 2..4
    static struct captured_sample waited;
    waited.count = datagram_count;
    for (int d = 0; d < datagram_count && d < 4; d++) {
        memcpy(waited.bytes[d], datagrams[d], datagram_len[d]);
        waited.len[d] = datagram_len[d];
    }
    EXPECT_TRUE(process_packet(&receiver, waited.bytes[0], 0, waited.len[0], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    EXPECT_EQ_U32(3, sender_proxy()->ack_seq_no); // seq_no 2 recorded
    static struct tt_Context second;
    static struct tt_Publisher second_pub;
    init_bare_node(&second, 3);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&second), &second_pub, &sender_topic,
                                                      ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    match_by_broadcast(&second_pub);
    static struct captured_sample other;
    sample_len = 100; // tracked first
    capture_sample(&second_pub, &other);
    EXPECT_TRUE(process_packet(&receiver, other.bytes[0], 0, other.len[0], SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
    delivered_count = 0;
    sample_len = 4000;
    for (int k = 0; k < 2 * tt_FRAG_REASSEMBLY_SLOTS; k++) {
        capture_sample(&second_pub, &other); // only its last datagram arrives: never whole
        EXPECT_TRUE(process_packet(&receiver, other.bytes[2], 0, other.len[2], SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_TRUE(receiver.frag_abandoned > 0); // control: the pool really was full, and reclaimed
    for (int d = 1; d < 3; d++) {
        EXPECT_TRUE(process_packet(&receiver, waited.bytes[d], 0, waited.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    expect_delivered_once(4000);
    EXPECT_EQ_U32(5, sender_proxy()->ack_seq_no);
}

// A second Subscriber of the same endpoint, on a topic of its own so that what it decodes is checked apart
// from what the first one does: the two share frag_scratch, and one corrupting the other's sample would
// otherwise be hidden by whichever decoded last.
static struct tt_TypeSupport second_topic;
static struct tt_Subscriber sub2;
static uint64_t reorder2[TEST_REORDER_SLOTS * TEST_REORDER_SLOT_BYTES / sizeof(uint64_t)];
static int second_delivered;
static bool second_intact;

static int32_t second_checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    second_delivered++;
    second_intact = len == sample_len;
    for (uint32_t i = 0; i < sample_len && i < len && second_intact; i++) {
        second_intact = payload[i] == pattern(i, expected_seed);
    }
    return 0;
}

// The first Subscriber's own verdict, kept apart in the same way.
static int first_delivered;
static bool first_intact;
static int32_t first_checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    first_delivered++;
    first_intact = len == sample_len;
    for (uint32_t i = 0; i < sample_len && i < len && first_intact; i++) {
        first_intact = payload[i] == pattern(i, expected_seed);
    }
    return 0;
}

static void add_second_subscriber(void) {
    second_topic = receiver_topic;
    second_topic.data_decode = second_checking_decode;
    receiver_topic.data_decode = first_checking_decode;
    memset(&sub2, 0, sizeof(sub2));
    memset(reorder2, 0, sizeof(reorder2));
    struct tt_QosProfile qos = TEST_QOS_RELIABLE_READER;
    qos.reliability.reorder_slots = TEST_REORDER_SLOTS;
    size_t storage_size = tt_Subscriber_storage_size(&second_topic, &qos);
    EXPECT_TRUE(storage_size <= sizeof(reorder2));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(
                                 tt_Context_default_node(&receiver), &sub2, &second_topic, ENDPOINT_NAME, &qos,
                                 &(struct tt_SubscriberOptions) {.callback = on_data}, reorder2, storage_size));
    EXPECT_EQ_U32(TEST_REORDER_SLOT_BYTES, sub2.reorder_slot_bytes);
    first_delivered = second_delivered = 0;
}

static void test_reliable_two_subscribers_share_the_scratch(void) {
    // frag_scratch is one per node, and one datagram goes to every Subscriber tracking its writer - so two
    // Subscribers of one writer take each fragment in turn. Only one can hold its sample in the scratch;
    // the other must not overwrite it, and both must get every sample intact, once.
    init_pair_reliable_receiver(4000, true);
    add_second_subscriber();
    publish_captured(); // first contact for both
    for (int d = 0; d < 3; d++) {
        deliver(d);
    }
    EXPECT_EQ_INT(1, first_delivered);
    EXPECT_EQ_INT(1, second_delivered);
    snapshot_reorder();
    for (int round = 0; round < 3; round++) {
        sample_seed = expected_seed = (uint8_t)(40 + round);
        publish_captured();
        for (int d = 0; d < 3; d++) {
            deliver(d);
        }
        EXPECT_EQ_INT(2 + round, first_delivered);
        EXPECT_EQ_INT(2 + round, second_delivered);
        EXPECT_TRUE(first_intact);
        EXPECT_TRUE(second_intact);
    }
    // The first Subscriber took the fast path every time: the second one's stores never moved it out.
    EXPECT_TRUE(reorder_untouched());
    EXPECT_EQ_U32(0, sub.reorder_held);
    EXPECT_EQ_U32(0, sub2.reorder_held);
    EXPECT_TRUE(receiver.frag_fast_sub == NULL);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub2));
}

static void test_reliable_buffer_delivery_does_not_overwrite_the_fast_path(void) {
    // The second Subscriber misses a sample X (no room for it, so it is left unrecorded) that the first one
    // takes. The first then starts the next sample Y in the scratch, and X's retransmission completes X for
    // the second one out of its buffer - put together in the same scratch. Y must survive that.
    init_pair_reliable_receiver(4000, true);
    add_second_subscriber();
    publish_captured(); // seq_no 1..3: first contact for both
    for (int d = 0; d < 3; d++) {
        deliver(d);
    }
    sample_seed = expected_seed = 50;
    static struct captured_sample x;
    capture_sample(&pub, &x); // seq_no 4..6
    // No room for X in the second one's buffer: its slots taken by another writer's datagrams. (Not by taking the
    // buffer away, as this test first did: a Subscriber with no buffer now puts X together in the reassembly pool.)
    struct tt_WriterProxy* proxy2 = find_writer_proxy(&sub2, SENDER_ID, pub.endpoint.entity_id);
    struct tt_ReorderSlot* taken[3];
    for (uint32_t i = 0; i < 3; i++) {
        taken[i] = reorder_writer_slot(&sub2, proxy2, SENDER_ID, pub.endpoint.entity_id, 4 + i);
        EXPECT_TRUE(!taken[i]->occupied);
        taken[i]->occupied = true;
        taken[i]->context_id = 99;
    }
    for (int d = 0; d < 3; d++) {
        EXPECT_TRUE(process_packet(&receiver, x.bytes[d], 0, x.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(2, first_delivered);
    EXPECT_EQ_INT(1, second_delivered);
    EXPECT_EQ_U32(4, find_writer_proxy(&sub2, SENDER_ID, pub.endpoint.entity_id)->ack_seq_no); // X unrecorded
    for (uint32_t i = 0; i < 3; i++) {
        taken[i]->occupied = false;
    }

    sample_seed = 60;
    static struct captured_sample y;
    capture_sample(&pub, &y); // seq_no 7..9
    EXPECT_TRUE(process_packet(&receiver, y.bytes[0], 0, y.len[0], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    EXPECT_TRUE(receiver.frag_fast_sub == &sub); // control: Y really is in the scratch
    for (int d = 0; d < 3; d++) {                // X again, as the writer resends it to the second one
        EXPECT_TRUE(process_packet(&receiver, x.bytes[d], 0, x.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(2, second_delivered);
    EXPECT_TRUE(second_intact); // X, intact
    expected_seed = 60;
    for (int d = 1; d < 3; d++) {
        EXPECT_TRUE(process_packet(&receiver, y.bytes[d], 0, y.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(3, first_delivered);
    EXPECT_TRUE(first_intact); // Y, intact for the first one
    EXPECT_EQ_INT(3, second_delivered);
    EXPECT_TRUE(second_intact);
    EXPECT_EQ_U32(0, sub.reorder_held);
    EXPECT_EQ_U32(0, sub2.reorder_held);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub2));
}

static void test_reliable_fast_path_does_not_overtake_a_lost_sample(void) {
    // Sample X is lost whole, and nothing is held: the next sample's first fragment is not in order, and
    // must wait for X rather than be put together and delivered ahead of it.
    first_contact_sample(4000);
    sub.callback = ordered_on_data;
    delivered_ascending = true;
    static struct captured_sample x;
    capture_sample(&pub, &x); // seq_no 4..6, lost for now
    publish_captured();       // seq_no 7..9
    for (int d = 0; d < 3; d++) {
        deliver(d);
    }
    EXPECT_EQ_INT(0, delivered_count);
    for (int d = 0; d < 3; d++) {
        EXPECT_TRUE(process_packet(&receiver, x.bytes[d], 0, x.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_TRUE(delivered_ascending);
    EXPECT_TRUE(delivered_intact);
}

static void test_reliable_fast_path_needs_room_in_the_buffer(void) {
    // The fast path takes a fragment only where the buffer could hold it, since it may have to move it there.
    // With slots too small for a fragment, nothing is acknowledged - the same as the buffer path.
    first_contact_sample(4000);
    sub.reorder_slot_bytes = (uint16_t)tt_REORDER_SLOT_SIZE(1000);
    publish_captured(); // seq_no 4..6
    for (int d = 0; d < 3; d++) {
        deliver(d);
    }
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_EQ_U32(4, sender_proxy()->ack_seq_no);
    EXPECT_TRUE(receiver.frag_fast_sub == NULL);
    sub.reorder_slot_bytes = TEST_REORDER_SLOT_BYTES; // control: with room, the same datagrams deliver
    for (int d = 0; d < 3; d++) {
        deliver(d);
    }
    expect_delivered_once(4000);
}

static size_t frag_header_offset(void) {
    return sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader);
}

static void set_cont_seq_no(int d, uint32_t seq_no) {
    memcpy(&datagrams[d][frag_header_offset() + offsetof(struct tt_FragContHeader, seq_no)], &seq_no, sizeof(seq_no));
}

static void test_reliable_fast_path_refuses_what_does_not_belong_to_its_sample(void) {
    // A continuation in order by seq_no that names another place in the sample, or another count, is not
    // this sample's next fragment. The buffer path refuses such a sample; the fast path must too.
    // Datagram 2, in order, claiming to be the last (index 3): a size check cannot catch it - the last
    // fragment may be any size.
    first_contact_sample(5000); // 4 datagrams
    publish_captured();         // seq_no 5..8
    set_cont_field(2, offsetof(struct tt_FragContHeader, frag_index), 3);
    for (int d = 0; d < 4; d++) {
        deliver(d);
    }
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_TRUE(receiver.frag_fast_sub == NULL);

    first_contact_sample(5000);
    publish_captured();
    deliver(0);
    set_cont_field(1, offsetof(struct tt_FragContHeader, frag_count), 3);
    for (int d = 1; d < 4; d++) {
        deliver(d);
    }
    EXPECT_EQ_INT(0, delivered_count);
    EXPECT_TRUE(receiver.frag_fast_sub == NULL);

    // A non-last continuation shorter than the one before it: what the buffer path delivers is every
    // fragment as it came, 8 bytes short - and moving the fast path's part into the buffer must not change
    // that, which it would if it cut the fragments apart at the wrong places.
    first_contact_sample(6500); // 5 datagrams
    publish_captured();         // seq_no 6..10
    EXPECT_EQ_INT(5, datagram_count);
    datagram_len[2] -= 8;
    uint16_t submessage_length = 0;
    memcpy(&submessage_length, &datagrams[2][sizeof(struct tt_Header) + 2], sizeof(submessage_length));
    submessage_length = (uint16_t)(submessage_length - 8);
    memcpy(&datagrams[2][sizeof(struct tt_Header) + 2], &submessage_length, sizeof(submessage_length));
    deliver(0);
    deliver(1);
    deliver(2);
    deliver(4); // 3 is late
    deliver(3);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U32(6500 - 8, delivered_len);

    // More continuations than frag_scratch can hold, from a sender that claims 64 fragments: refused before
    // anything is written past the scratch, and nothing delivered.
    first_contact_sample(4000);
    publish_captured(); // seq_no 4..6
    uint32_t first = 4;
    datagrams[0][frag_header_offset() + offsetof(struct tt_FragFirstHeader, frag_count)] = 64;
    deliver(0);
    for (uint32_t index = 1; index < 14; index++) {
        set_cont_field(1, offsetof(struct tt_FragContHeader, frag_count), 64);
        set_cont_field(1, offsetof(struct tt_FragContHeader, frag_index), (uint8_t)index);
        set_cont_seq_no(1, first + index);
        deliver(1);
    }
    EXPECT_TRUE(receiver.frag_fast_sub == NULL);
    EXPECT_EQ_INT(0, delivered_count);
}

static void test_reliable_fast_path_writes_nothing_past_its_scratch(void) {
    // The last case above, looked at where the damage would be: a sender claiming 64 fragments of a sample larger than
    // tt_MAX_SAMPLE_LENGTH sends continuations in order until frag_scratch is full. The fragment that does not fit is
    // refused before it is copied, so the fields after frag_scratch in the context - the fast path's own bookkeeping
    // first - are as the last fragment that fitted left them. Delivering nothing is not enough to show that: copied
    // past the scratch, the fragment overwrote frag_fast_count and frag_fast_placed with its own bytes, and the next
    // checks refused the sample for the garbage (mutants_frag_fast_path.py, "the sample fits the scratch", 2026-10-16).
    first_contact_sample(4000);
    publish_captured(); // seq_no 4..6
    uint32_t first = 4;
    datagrams[0][frag_header_offset() + offsetof(struct tt_FragFirstHeader, frag_count)] = 64;
    deliver(0);
    const size_t from = offsetof(struct tt_Context, frag_fast_timestamp);
    const size_t to = offsetof(struct tt_Context, frag_duplicate) + sizeof(receiver.frag_duplicate);
    static uint8_t before[sizeof(struct tt_Context)];
    uint32_t index = 1;
    // In order until the scratch is full: each one fits and is taken.
    for (;
         index == 1 || receiver.frag_fast_length + receiver.frag_fast_cont_length <= sizeof(receiver.frag_scratch) - 8;
         index++) {
        set_cont_field(1, offsetof(struct tt_FragContHeader, frag_count), 64);
        set_cont_field(1, offsetof(struct tt_FragContHeader, frag_index), (uint8_t)index);
        set_cont_seq_no(1, first + index);
        deliver(1);
        EXPECT_TRUE(receiver.frag_fast_sub != NULL); // the control: the fast path holds the sample this far
    }
    EXPECT_EQ_U32(index, receiver.frag_fast_placed);
    memcpy(before + from, (const uint8_t*)&receiver + from, to - from);
    // The next one does not fit.
    set_cont_field(1, offsetof(struct tt_FragContHeader, frag_count), 64);
    set_cont_field(1, offsetof(struct tt_FragContHeader, frag_index), (uint8_t)index);
    set_cont_seq_no(1, first + index);
    deliver(1);
    EXPECT_TRUE(receiver.frag_fast_sub == NULL); // refused, and what the scratch held moved out
    EXPECT_EQ_INT(0, memcmp(before + from, (const uint8_t*)&receiver + from, to - from));
    EXPECT_EQ_U32(64, receiver.frag_fast_count);
    EXPECT_EQ_U32(index, receiver.frag_fast_placed);
    EXPECT_EQ_INT(0, delivered_count);
}

// The second writer of the collision tests: another node, the same topic, DATA-sized samples.
static struct tt_Context collide_ctx;
static struct tt_Publisher collide_pub;
static uint8_t collide_gap[tt_MAX_BUFFER_LENGTH * 2]; // its seq_no 2, kept back to open a gap
static uint32_t collide_gap_len;
static uint8_t collide_hit[tt_MAX_BUFFER_LENGTH * 2]; // its seq_no that lands on the first writer's slot
static uint32_t collide_hit_len;

// The first writer tracked, at seq_no 5 next (4 datagrams a sample); the second writer tracked too, and one
// of its DATA captured whose reorder slot is the one the first writer's seq_no 5 maps to.
static void collision_setup(void) {
    first_contact_sample(5000); // seq_no 1..4
    init_bare_node(&collide_ctx, 3);
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_publisher(tt_Context_default_node(&collide_ctx), &collide_pub, &sender_topic,
                                           ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    match_by_broadcast(&collide_pub);
    sample_len = 100;
    struct tt_ReorderSlot* target = reorder_writer_slot(
        &sub, find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id), SENDER_ID, pub.endpoint.entity_id, 5);
    bool found = false;
    for (uint32_t n = 1; n < 200 && !found; n++) {
        start_capture();
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&collide_pub, (struct tt_Data*)&sample_len));
        test_mock_send_hook = NULL;
        EXPECT_EQ_INT(1, datagram_count);
        uint32_t seq_no = 0;
        memcpy(&seq_no, &datagrams[0][frag_header_offset() + offsetof(struct tt_DataHeader, seq_no)], sizeof(seq_no));
        if (seq_no == 1) {
            EXPECT_TRUE(process_packet(&receiver, datagrams[0], 0, datagram_len[0], SENDER_IP + 2, PORT,
                                       tt_TRANSPORT_UDP)); // its first contact
        } else if (seq_no == 2) {
            memcpy(collide_gap, datagrams[0], datagram_len[0]);
            collide_gap_len = datagram_len[0];
        } else if (reorder_writer_slot(&sub, find_writer_proxy(&sub, 3, collide_pub.endpoint.entity_id), 3,
                                       collide_pub.endpoint.entity_id, seq_no) == target) {
            memcpy(collide_hit, datagrams[0], datagram_len[0]);
            collide_hit_len = datagram_len[0];
            found = true;
        }
    }
    EXPECT_TRUE(found);
    delivered_count = 0;
    sample_len = 5000;
}

static struct tt_WriterProxy* collide_proxy(void) {
    return find_writer_proxy(&sub, 3, collide_pub.endpoint.entity_id);
}

static void deliver_collide_hit(void) {
    EXPECT_TRUE(process_packet(&receiver, collide_hit, 0, collide_hit_len, SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
}

static void test_reliable_fast_path_moves_out_before_another_writer_is_held(void) {
    // The first writer's sample is part-way through the scratch when the second writer's DATA arrives ahead
    // of a gap and is held - in the very slot the first writer's sample would move into. The move must
    // come first, so that it cannot fail: that DATA is then the one refused (and asked for again), and the
    // first writer's sample, interrupted by a loss of its own, is still delivered.
    collision_setup();
    publish_captured(); // the first writer's seq_no 5..8
    deliver(0);
    deliver(1);
    EXPECT_TRUE(receiver.frag_fast_sub == &sub);
    deliver_collide_hit();
    deliver(3); // 2 is late
    deliver(2);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_U32(5000, delivered_len);
    EXPECT_EQ_U32(0, sub.reorder_abandoned);
}

static void test_reliable_fast_path_waits_while_anything_is_held(void) {
    // The second writer has a DATA held in the slot the first writer's next sample maps to. The first
    // writer's sample must not start in the scratch: with that slot taken it could not be moved out, and an
    // interruption would lose it. It goes the buffer's way - its first datagram refused until the slot frees.
    collision_setup();
    deliver_collide_hit(); // held behind the second writer's gap
    EXPECT_EQ_U32(1, sub.reorder_held);
    publish_captured(); // the first writer's seq_no 5..8
    deliver(0);
    deliver(1);
    deliver(3); // 2 is late
    deliver(2);
    EXPECT_EQ_INT(0, delivered_count);
    struct tt_WriterProxy* other = collide_proxy();
    uint32_t hit_seq_no = 0;
    memcpy(&hit_seq_no, &collide_hit[frag_header_offset() + offsetof(struct tt_DataHeader, seq_no)],
           sizeof(hit_seq_no));
    advance_past_unavailable(other, hit_seq_no); // the second writer's gap given up: its held DATA goes
    sample_len = 100;
    drain_reorder(&receiver, &sub, other);
    EXPECT_EQ_INT(1, delivered_count);
    sample_len = 5000;
    deliver(0); // the first writer's resend of what was refused
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_U32(5000, delivered_len);
    EXPECT_EQ_U32(0, sub.reorder_held);
}

static void test_reliable_fast_path_survives_another_writers_drain(void) {
    // A Heartbeat from another writer drains that writer's part of the buffer. It has nothing to do with the
    // sample in the scratch, which must stay there.
    collision_setup();
    snapshot_reorder();
    publish_captured(); // seq_no 5..8
    deliver(0);
    drain_reorder(&receiver, &sub, collide_proxy());
    EXPECT_TRUE(receiver.frag_fast_sub == &sub);
    deliver(1);
    drain_reorder(&receiver, &sub, sender_proxy()); // and its own writer's, with the watermark where it was
    EXPECT_TRUE(receiver.frag_fast_sub == &sub);
    for (int d = 2; d < 4; d++) {
        deliver(d);
    }
    expect_delivered_once(5000);
    EXPECT_TRUE(reorder_untouched());
}

static void test_reliable_fast_path_ignores_another_subscribers_hold(void) {
    // A DATA held by a Subscriber of another topic does not concern the sample in the scratch either.
    first_contact_sample(5000);
    static struct tt_TypeSupport other_topic;
    static struct tt_TypeSupport other_sender_topic;
    static struct tt_Subscriber other_sub;
    other_topic = receiver_topic;
    other_topic.name = "other_topic";
    other_sender_topic = sender_topic;
    other_sender_topic.name = "other_topic";
    memset(&other_sub, 0, sizeof(other_sub));
    memset(reorder2, 0, sizeof(reorder2));
    struct tt_QosProfile qos = TEST_QOS_RELIABLE_READER;
    qos.reliability.reorder_slots = TEST_REORDER_SLOTS;
    size_t storage_size = tt_Subscriber_storage_size(&other_topic, &qos);
    EXPECT_TRUE(storage_size <= sizeof(reorder2));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(
                                 tt_Context_default_node(&receiver), &other_sub, &other_topic, ENDPOINT_NAME, &qos,
                                 &(struct tt_SubscriberOptions) {.callback = on_data}, reorder2, storage_size));
    EXPECT_EQ_U32(TEST_REORDER_SLOT_BYTES, other_sub.reorder_slot_bytes);
    init_bare_node(&collide_ctx, 3);
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_publisher(tt_Context_default_node(&collide_ctx), &collide_pub, &other_sender_topic,
                                           ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    match_by_broadcast(&collide_pub);
    sample_len = 100;
    static struct captured_sample data;
    for (int n = 0; n < 3; n++) { // seq_no 1 delivered, 2 lost, 3 held behind it
        capture_sample(&collide_pub, &data);
        if (n != 1) {
            EXPECT_TRUE(
                process_packet(&receiver, data.bytes[0], 0, data.len[0], SENDER_IP + 2, PORT, tt_TRANSPORT_UDP));
        }
        if (n == 0) {
            sample_len = 5000;
            snapshot_reorder();
            publish_captured(); // the first topic's seq_no 5..8, its first datagram before the held DATA
            deliver(0);
            sample_len = 100;
        }
    }
    EXPECT_EQ_U32(1, other_sub.reorder_held);
    EXPECT_TRUE(receiver.frag_fast_sub == &sub);
    sample_len = 5000;
    delivered_count = 0;
    for (int d = 1; d < 4; d++) {
        deliver(d);
    }
    expect_delivered_once(5000);
    EXPECT_TRUE(reorder_untouched());
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&other_sub));
}

static void test_reliable_fast_path_keeps_the_reorder_cursor_with_the_watermark(void) {
    // More in-order samples than the tracking window is wide, then one out of order. The drain walks from
    // reorder_cursor at most a window, so a cursor left where the fast path began would never reach it.
    first_contact_sample(4000);
    for (int n = 0; n < 100; n++) {
        publish_captured();
        for (int d = 0; d < 3; d++) {
            deliver(d);
        }
    }
    EXPECT_EQ_INT(100, delivered_count);
    publish_captured();
    deliver(0);
    deliver(2);
    deliver(1);
    EXPECT_EQ_INT(101, delivered_count);
    EXPECT_EQ_U32(0, sub.reorder_held);
}

static void test_reliable_fast_path_follows_its_writer_and_subscriber_out(void) {
    // A writer forgotten (its node gone, or the endpoint) takes the part of its sample in the scratch with
    // it, counted as abandoned like a held one; another writer forgotten leaves it alone. A Subscriber
    // destroyed leaves no pointer to itself behind.
    static struct tt_Publisher sibling_pub; // another writer on the first writer's own node
    for (int arm = 0; arm < 4; arm++) {
        first_contact_sample(5000);
        uint32_t entity = pub.endpoint.entity_id;
        // arm 0: a writer on another node; arm 1: another writer on the same node - each tracked, so that
        // forgetting it really reaches the Subscriber.
        struct tt_Publisher* other = NULL;
        uint32_t other_ip = SENDER_IP;
        if (arm == 0) {
            init_bare_node(&collide_ctx, 3);
            EXPECT_EQ_INT(tt_RET_OK,
                          tt_Node_create_publisher(tt_Context_default_node(&collide_ctx), &collide_pub, &sender_topic,
                                                   ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
            match_by_broadcast(&collide_pub);
            other = &collide_pub;
            other_ip = SENDER_IP + 2;
        } else if (arm == 1) {
            EXPECT_EQ_INT(tt_RET_OK,
                          tt_Node_create_publisher(tt_Context_default_node(&sender), &sibling_pub, &sender_topic,
                                                   ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
            match_by_broadcast(&sibling_pub);
            EXPECT_TRUE(sibling_pub.endpoint.entity_id != entity);
            other = &sibling_pub;
        }
        if (other != NULL) {
            static struct captured_sample data;
            sample_len = 100;
            capture_sample(other, &data);
            EXPECT_TRUE(process_packet(&receiver, data.bytes[0], 0, data.len[0], other_ip, PORT, tt_TRANSPORT_UDP));
            EXPECT_TRUE(find_writer_proxy(&sub, other == &collide_pub ? 3 : SENDER_ID, other->endpoint.entity_id) !=
                        NULL);
            sample_len = 5000;
            delivered_count = 0;
        }
        publish_captured(); // seq_no 5..8
        deliver(0);
        deliver(1);
        EXPECT_TRUE(receiver.frag_fast_sub == &sub);
        if (arm == 0) {
            forget_writer_proxies_for_endpoint(&receiver, sub.endpoint.id, 3, 0, true); // another node
        } else if (arm == 1) {
            forget_writer_proxies_for_endpoint(&receiver, sub.endpoint.id, SENDER_ID, sibling_pub.endpoint.entity_id,
                                               false);
        } else if (arm == 2) {
            forget_writer_proxies_for_endpoint(&receiver, sub.endpoint.id, SENDER_ID, entity, false);
        } else {
            forget_writer_proxies_for_endpoint(&receiver, sub.endpoint.id, SENDER_ID, 0, true);
        }
        if (arm < 2) {
            EXPECT_TRUE(receiver.frag_fast_sub == &sub);
            deliver(2);
            deliver(3);
            expect_delivered_once(5000);
        } else {
            EXPECT_TRUE(receiver.frag_fast_sub == NULL);
            EXPECT_EQ_U32(2, sub.reorder_abandoned);
        }
    }
    first_contact_sample(5000);
    publish_captured();
    deliver(0);
    EXPECT_TRUE(receiver.frag_fast_sub == &sub);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub));
    EXPECT_TRUE(receiver.frag_fast_sub == NULL);
}

static void test_keep_last_evicts_whole_samples(void) {
    // Depth counts seq_no - datagrams - and eviction takes a whole sample, never leaving a continuation
    // without its first datagram at the old end of the cache.
    init_pair(4000); // 3 datagrams a sample
    make_reliable();
    frag_cache.depth = 5;
    publish_captured(); // seq_no 1..3
    publish_captured(); // seq_no 4..6: needs 4..6 in a 5-deep ring, so sample 1 goes - all of it
    EXPECT_EQ_U32(4, frag_cache.oldest_seq_no);
    EXPECT_TRUE(reliable_cache_slot_live(&frag_cache, 5, 4));
    EXPECT_TRUE(reliable_cache_slot_live(&frag_cache, 5, 6));
    EXPECT_TRUE(!reliable_cache_slot_live(&frag_cache, 5, 3)); // not a stranded continuation
}

static void test_sample_depth_bounds_keep_last_in_samples(void) {
    // A ring sized in datagrams for the largest sample would keep more small samples than HISTORY.depth
    // says; sample_depth holds it to that many whatever their size. Control first: without it, the ring
    // alone bounds the cache and all five single-datagram samples stay.
    init_pair(100);
    make_reliable();
    frag_cache.depth = 12;
    for (int i = 0; i < 5; i++) {
        publish_captured();
    }
    EXPECT_EQ_U32(1, frag_cache.oldest_seq_no);
    EXPECT_EQ_U32(5, frag_cache.retained_samples);

    init_pair(100);
    make_reliable();
    frag_cache.depth = 12;
    frag_cache.sample_depth = 3;
    for (int i = 0; i < 5; i++) {
        publish_captured();
    }
    EXPECT_EQ_U32(3, frag_cache.oldest_seq_no);
    EXPECT_EQ_U32(3, frag_cache.retained_samples);
    EXPECT_TRUE(!reliable_cache_slot_live(&frag_cache, 12, 2));

    // Fragmented samples count once each, and go whole: three 3-datagram samples under a bound of 2 keep
    // the last two, seq_no 4..9, with the ring (12) never the thing that evicted.
    init_pair(4000);
    make_reliable();
    frag_cache.depth = 12;
    frag_cache.sample_depth = 2;
    for (int i = 0; i < 3; i++) {
        publish_captured();
    }
    EXPECT_EQ_U32(4, frag_cache.oldest_seq_no);
    EXPECT_EQ_U32(2, frag_cache.retained_samples);
    EXPECT_TRUE(reliable_cache_slot_live(&frag_cache, 12, 9));
    EXPECT_TRUE(!reliable_cache_slot_live(&frag_cache, 12, 3));
}

static void test_keep_all_counts_every_datagram(void) {
    // KEEP_ALL refuses a sample whose datagrams would take the unacknowledged run past its bound, and
    // says why, so tt_Publisher_writable() answers about the sample the caller will retry.
    init_pair(4000); // 3 datagrams a sample
    make_reliable();
    frag_cache.depth = 4;
    pub.keep_all = true;
    pub.peer_acks[0].context_id = RECEIVER_ID;
    pub.peer_acks[0].ack_seq_no = 1; // nothing acknowledged yet
    publish_captured();              // 3 unacknowledged: fits 4
    EXPECT_EQ_U32(3, pub.seq_no);
    start_capture();
    EXPECT_EQ_INT(tt_RET_WOULD_BLOCK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_INT(0, datagram_count);
    EXPECT_EQ_U32(3, pub.seq_no);
    EXPECT_TRUE(!tt_Publisher_writable(&pub));
    pub.peer_acks[0].ack_seq_no = 4; // all three acknowledged
    EXPECT_TRUE(tt_Publisher_writable(&pub));
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_U32(6, pub.seq_no);
}

static void test_keep_all_never_evicts_the_unacknowledged_rest_of_a_sample(void) {
    // The cache evicts whole samples, readers acknowledge datagrams: an acknowledgement that stops inside a
    // sample must count as stopping before it, or admitting the next sample evicts the acknowledged first
    // fragment AND its unacknowledged continuations (rig, 2026-10-05: gap_evicted 751-1912 per run, Array4k).
    // Depth 8 is chosen so the record count decides: 6 + 3 - 1 = 8 fits if fragment 1 alone counts as
    // acknowledged; 6 + 3 - 0 = 9 does not once the acknowledgement is rounded to whole samples.
    init_pair(4000); // 3 datagrams a sample
    make_reliable();
    frag_cache.depth = 8;
    pub.keep_all = true;
    pub.peer_acks[0].context_id = RECEIVER_ID;
    pub.peer_acks[0].ack_seq_no = 1;
    publish_captured(); // seq_no 1..3
    publish_captured(); // seq_no 4..6
    EXPECT_EQ_U32(6, pub.seq_no);

    pub.peer_acks[0].ack_seq_no = 2; // fragment 1 of sample 1 acknowledged, its fragments 2 and 3 not
    EXPECT_EQ_U32(0, keep_all_acked_through(&pub));
    start_capture();
    EXPECT_EQ_INT(tt_RET_WOULD_BLOCK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_U32(1, frag_cache.oldest_seq_no); // nothing evicted
    EXPECT_TRUE(reliable_cache_slot_live(&frag_cache, 8, 3));

    pub.peer_acks[0].ack_seq_no = 3; // still inside sample 1
    EXPECT_EQ_U32(0, keep_all_acked_through(&pub));
    pub.peer_acks[0].ack_seq_no = 4; // all of sample 1: now it may go
    EXPECT_EQ_U32(3, keep_all_acked_through(&pub));
    EXPECT_TRUE(tt_Publisher_writable(&pub));
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_U32(9, pub.seq_no);
    EXPECT_EQ_U32(4, frag_cache.oldest_seq_no); // sample 1 went whole, and only after it was all acknowledged
}

// --- A smaller link MTU (docs/DESIGN.md "Link profiles", item G): the sender cuts at its control_max -----------------

#define NARROW_CONTROL 700U // a link MTU of 728

// The sender as link_profiles_apply() leaves a context whose smallest link has MTU NARROW_CONTROL + 28; the receiver
// keeps the default, as a peer on a wider link or of an older build would.
static void narrow_sender(void) {
    sender.control_max = NARROW_CONTROL;
    sender.frag_cont = NARROW_CONTROL - LINK_FRAG_CONT_OVERHEAD;
}

static void expect_every_datagram_within(uint32_t limit) {
    for (int d = 0; d < datagram_count; d++) {
        EXPECT_TRUE(wire_len[d] <= limit);
    }
}

static void test_a_narrow_link_cuts_fragments_a_default_reader_reassembles(void) {
    // Control: at the default the sample is three fragments, the first filling its datagram.
    init_pair(4000);
    publish_captured();
    EXPECT_EQ_INT(3, datagram_count);
    EXPECT_TRUE(wire_len[0] > NARROW_CONTROL);
    // Narrowed, every datagram fits the link, there are as many as the shorter cut needs, and the reader - which
    // learns a sample's continuation length from its fragments - puts them back together, in either order.
    for (int reversed = 0; reversed < 2; reversed++) {
        init_pair(4000);
        narrow_sender();
        publish_captured();
        EXPECT_EQ_INT((int)frag_count_for(&sender, 4000), datagram_count);
        EXPECT_TRUE(datagram_count > 3);
        EXPECT_EQ_U32((uint32_t)datagram_count, pub.seq_no); // one seq_no a datagram, as at the default
        expect_every_datagram_within(NARROW_CONTROL);
        EXPECT_EQ_U32(NARROW_CONTROL, wire_len[0] + 4U); // fragment 0 fills its (classic-form) datagram
        for (int d = 0; d < datagram_count; d++) {
            deliver(reversed ? datagram_count - 1 - d : d);
        }
        expect_delivered_once(4000);
        EXPECT_EQ_U64(0, receiver.frag_dropped);
    }
}

static void test_a_narrow_link_keeps_whole_only_what_fits_it(void) {
    // A sample that went whole at the default fragments on the narrow link; one that fits the link goes whole.
    init_pair(1000);
    narrow_sender();
    publish_captured();
    EXPECT_EQ_INT(2, datagram_count);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_FRAG_FIRST, submessage_of(0)->type);
    expect_every_datagram_within(NARROW_CONTROL);
    deliver(0);
    deliver(1);
    expect_delivered_once(1000);
    init_pair(600);
    narrow_sender();
    publish_captured();
    EXPECT_EQ_INT(1, datagram_count);
    EXPECT_EQ_INT(tt_SUBMESSAGE_TYPE_DATA, submessage_of(0)->type);
    // Batched samples share a datagram up to the link's length, not the build's.
    init_pair(200);
    narrow_sender();
    pub.batch = true;
    start_capture();
    for (int i = 0; i < 6; i++) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    }
    node_flush(&sender, 0, NULL);
    test_mock_send_hook = NULL;
    EXPECT_TRUE(datagram_count >= 2); // 6 x ~230 bytes: two datagrams of the narrow link, one at the default
    expect_every_datagram_within(NARROW_CONTROL);
}

static void test_a_narrow_link_retransmits_the_fragment_it_cut(void) {
    // RELIABLE through the fast path, a lost middle fragment resent from the cache at the same cut.
    init_pair_reliable_receiver(4000, true);
    make_reliable();
    narrow_sender();
    publish_captured();
    int count = datagram_count;
    EXPECT_TRUE(count > 3);
    expect_every_datagram_within(NARROW_CONTROL);
    for (int d = 0; d < count; d++) {
        if (d != 2) {
            deliver(d);
        }
    }
    EXPECT_EQ_INT(0, delivered_count);
    start_capture();
    struct tt_ReliableCache* cache = pub.reliable_cache;
    EXPECT_TRUE(!retransmit_one_sample(&sender, &pub, cache, reliable_cache_depth(cache), 3, &receiver_peer));
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(1, datagram_count);
    expect_every_datagram_within(NARROW_CONTROL);
    const struct tt_FragContHeader* cont = (const struct tt_FragContHeader*)(submessage_of(0) + 1);
    EXPECT_EQ_INT(count, cont->frag_count);
    EXPECT_EQ_INT(2, cont->frag_index);
    deliver(0);
    expect_delivered_once(4000);
    EXPECT_EQ_U64(0, receiver.frag_dropped);
}

#if tt_HYBRID_TRANSPORT
static void test_a_narrow_link_sends_no_runs(void) {
    // A run's units are full-length continuations: on a narrower link a Publisher asking for IP gets MTU units.
    init_pair(8);
    pub.large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_IP, hybrid_mode(&sender, &pub));
    narrow_sender();
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_MTU, hybrid_mode(&sender, &pub));
}
#endif

// --- the pool as a reorder buffer: its residual risks (2026-10-25) -------------------------------------------------
//
// ffc93e11 made the reassembly pool the reorder buffer of a RELIABLE Subscriber with none. Its author listed four
// risks it left untested: (a) an eviction HEARTBEAT past a whole sample held ahead, (b) more no-buffer streams than
// the pool can pin, (c) a BEST_EFFORT Subscriber sharing the pool with such a one, (d) the cost of counting pins. Each
// case below is the one that would show its risk.

// (a) An eviction HEARTBEAT moves the watermark past a whole sample the reader holds ahead of an incomplete one. The
// buffered path hands over what it holds behind a gap given up on (drain_reorder() after advance_past_unavailable()):
// KEEP_LAST loses what the writer no longer has, not what the reader already has. The pool must do the same. Run with
// a buffer as the control, then without, for both baselines a writer can announce: B's own first datagram (A evicted;
// B is recorded, so the watermark absorbs it at once) and past B (both evicted). And with A wholly lost, so the pool
// has no slot of A's and B is whole but not recorded (frag_pool_record_ahead()): the watermark then stops at B, and
// B is delivered at once rather than after its resend.
static void eviction_heartbeat_case(bool with_buffer, uint32_t first_available, bool a_lost) {
    tracked_pair(4000, with_buffer); // A: seq_no 2..4, captured
    static struct captured_sample a;
    static struct captured_sample b;
    static struct captured_sample c;
    a.count = datagram_count;
    for (int d = 0; d < datagram_count && d < 4; d++) {
        memcpy(a.bytes[d], datagrams[d], datagram_len[d]);
        a.len[d] = datagram_len[d];
    }
    capture_sample(&pub, &b); // B: 5..7
    capture_sample(&pub, &c); // C: 8..10
    EXPECT_EQ_INT(3, a.count);
    EXPECT_EQ_INT(3, b.count);
    EXPECT_EQ_INT(3, c.count);
    if (!a_lost) {
        EXPECT_TRUE(process_packet(&receiver, a.bytes[0], 0, a.len[0], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    for (int d = 0; d < 3; d++) {
        EXPECT_TRUE(process_packet(&receiver, b.bytes[d], 0, b.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(0, delivered_count); // B whole, behind A
    EXPECT_TRUE(with_buffer || bitmap_test_bit(sender_proxy()->received_bitmap, 5 - sender_proxy()->ack_seq_no) ==
                                   !a_lost); // recorded ahead, unless the pool holds nothing of A
    struct tt_Header header;
    memcpy(&header, a.bytes[0], sizeof(header)); // the writer's own header, so its byte order
    struct heartbeat_ctx heartbeat = {
        &header, SENDER_IP, PORT, pub.endpoint.entity_id, first_available, 10, tt_HEARTBEAT_FLAG_FINAL};
    inform_subscriber_of_heartbeat(&receiver, &sub.endpoint, &heartbeat);
    EXPECT_EQ_U32(8, sender_proxy()->ack_seq_no);
    EXPECT_EQ_INT(1, delivered_count); // B, which the reader had whole
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_U32(4000, delivered_len);
    for (int d = a_lost ? 0 : 1; d < 3; d++) { // A's rest, late: A is never delivered, torn or whole
        EXPECT_TRUE(process_packet(&receiver, a.bytes[d], 0, a.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(1, delivered_count);
    for (int d = 0; d < 3; d++) { // and the stream goes on
        EXPECT_TRUE(process_packet(&receiver, c.bytes[d], 0, c.len[d], SENDER_IP, PORT, tt_TRANSPORT_UDP));
    }
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_U32(11, sender_proxy()->ack_seq_no);
}

static void test_reliable_none_eviction_heartbeat_keeps_a_whole_sample_ahead(void) {
    for (int a_lost = 0; a_lost < 2; a_lost++) {
        eviction_heartbeat_case(true, 5, a_lost); // the control: the buffered path
        eviction_heartbeat_case(true, 8, a_lost);
        eviction_heartbeat_case(false, 5, a_lost);
        eviction_heartbeat_case(false, 8, a_lost);
    }
}

// (c) A BEST_EFFORT Subscriber of the same writers, beside the RELIABLE one, on a topic of its own so that what each
// decodes is counted apart (sub2, second_delivered; the RELIABLE one first_delivered).
static void add_best_effort_subscriber(void) {
    second_topic = receiver_topic;
    second_topic.data_decode = second_checking_decode;
    receiver_topic.data_decode = first_checking_decode;
    memset(&sub2, 0, sizeof(sub2));
    EXPECT_TRUE(tt_Subscriber_storage_size(&second_topic, &TEST_QOS_UNRELIABLE) <= sizeof(own_slot_storage[1]));
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub2, &second_topic, ENDPOINT_NAME,
                                            &TEST_QOS_UNRELIABLE, &(struct tt_SubscriberOptions) {.callback = on_data},
                                            own_slot_storage[1], sizeof(own_slot_storage[1])));
    first_delivered = second_delivered = 0;
}

// More writers for one receiver, each on a node of its own (id 3 on), each tracked by every Subscriber through one
// DATA first.
#define EXTRA_WRITERS 2
static struct tt_Context extra_node[EXTRA_WRITERS];
static struct tt_Publisher extra_pub[EXTRA_WRITERS];

static void add_tracked_writers(void) {
    uint32_t len = sample_len;
    for (int k = 0; k < EXTRA_WRITERS; k++) {
        init_bare_node(&extra_node[k], (uint8_t)(3 + k));
        EXPECT_EQ_INT(tt_RET_OK,
                      tt_Node_create_publisher(tt_Context_default_node(&extra_node[k]), &extra_pub[k], &sender_topic,
                                               ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
        match_by_broadcast(&extra_pub[k]);
        static struct captured_sample data;
        sample_len = 100;
        capture_sample(&extra_pub[k], &data);
        EXPECT_EQ_INT(1, data.count);
        EXPECT_TRUE(process_packet(&receiver, data.bytes[0], 0, data.len[0], SENDER_IP + 2 + (uint32_t)k, PORT,
                                   tt_TRANSPORT_UDP));
    }
    sample_len = len;
}

static void deliver_from(int k, const struct captured_sample* s, int d) {
    uint32_t ip = k < 0 ? SENDER_IP : SENDER_IP + 2 + (uint32_t)k;
    EXPECT_TRUE(process_packet(&receiver, s->bytes[d], 0, s->len[d], ip, PORT, tt_TRANSPORT_UDP));
}

// On a clean link, two writers interleaved datagram by datagram: each Subscriber gets every sample once, intact.
static void test_best_effort_beside_reliable_none_gets_every_sample(void) {
    tracked_none_pair(4000); // the first writer's sample A, seq_no 2..4, captured
    add_best_effort_subscriber();
    for (int d = 0; d < 3; d++) {
        deliver(d);
    }
    EXPECT_EQ_INT(1, first_delivered);
    EXPECT_EQ_INT(1, second_delivered);
    add_tracked_writers();
    first_delivered = second_delivered = 0;
    static struct captured_sample one;
    static struct captured_sample two;
    for (int round = 0; round < 3; round++) {
        capture_sample(&pub, &one);
        capture_sample(&extra_pub[0], &two);
        for (int d = 0; d < 3; d++) {
            deliver_from(-1, &one, d);
            deliver_from(0, &two, d);
        }
    }
    EXPECT_EQ_INT(6, first_delivered);
    EXPECT_EQ_INT(6, second_delivered);
    EXPECT_TRUE(first_intact);
    EXPECT_TRUE(second_intact);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub2));
}

// Every pool slot and its own pinned by the RELIABLE Subscriber - its first writer's sample A waiting on two datagrams
// (in its own slot) with six whole samples recorded behind it, and two other writers' samples part-placed - when a
// sample G arrives whole. With a buffer the RELIABLE Subscriber pins nothing (the control): G is delivered to the
// BEST_EFFORT one on arrival. Without one, until 2026-10-27 there was no slot for G, and the BEST_EFFORT Subscriber got
// it only from the resend the RELIABLE one asks for - late, and only if no later sample of that writer reached it first
// (2026-10-25: the pool's slots were the node's, and a sample waiting on a repair holds its slot for the round trip).
// Now G goes to the BEST_EFFORT Subscriber's own slot, which nothing else may take (docs/DESIGN.md section 8,
// "Reassembly slots").
#define PINNED_AHEAD (tt_FRAG_REASSEMBLY_SLOTS - EXTRA_WRITERS) // whole samples recorded ahead: the rest of the pool
struct pinned_outcome {
    int best_effort_on_arrival; // G's original datagrams
    int best_effort_after;      // and after A's repair and G's resend
    int reliable;
};

static struct pinned_outcome pool_pinned_full_case(bool with_buffer) {
    tracked_pair(4000, with_buffer); // A: seq_no 2..4, captured
    static struct captured_sample a;
    for (int d = 0; d < datagram_count && d < 4; d++) {
        memcpy(a.bytes[d], datagrams[d], datagram_len[d]);
        a.len[d] = datagram_len[d];
    }
    add_best_effort_subscriber();
    add_tracked_writers();
    first_delivered = second_delivered = 0;
    deliver_from(-1, &a, 0); // A's first datagram only: the watermark is in it
    static struct captured_sample s;
    for (int k = 0; k < PINNED_AHEAD; k++) {
        capture_sample(&pub, &s); // whole, ahead of A: recorded while two slots stay unpinned
        for (int d = 0; d < 3; d++) {
            deliver_from(-1, &s, d);
        }
    }
    for (int k = 0; k < EXTRA_WRITERS; k++) {
        capture_sample(&extra_pub[k], &s); // their own watermark samples, part-placed
        deliver_from(k, &s, 0);
    }
    EXPECT_EQ_INT(0, first_delivered);
    EXPECT_EQ_INT(PINNED_AHEAD, second_delivered); // BEST_EFFORT took the whole ones
    EXPECT_EQ_U32(with_buffer ? 0U : (uint32_t)tt_FRAG_REASSEMBLY_SLOTS, frag_pool_pinned(&receiver)); // the shape
    EXPECT_TRUE(with_buffer || frag_slot_pinned(&sub.frag_own)); // and A in the RELIABLE Subscriber's own
    int before = second_delivered;
    capture_sample(&pub, &s); // G, whole on the wire
    for (int d = 0; d < 3; d++) {
        deliver_from(-1, &s, d);
    }
    struct pinned_outcome outcome = {.best_effort_on_arrival = second_delivered - before};
    for (int d = 1; d < 3; d++) {
        deliver_from(-1, &a, d); // A's repair: A and the six behind it are delivered, their slots unpinned
    }
    for (int d = 0; d < 3; d++) {
        deliver_from(-1, &s, d); // G's resend, which the RELIABLE Subscriber asks for unless it has G already
    }
    outcome.best_effort_after = second_delivered - before;
    outcome.reliable = first_delivered;
    EXPECT_TRUE(first_intact);
    EXPECT_TRUE(second_intact);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub2));
    return outcome;
}

static void test_best_effort_beside_reliable_none_when_every_slot_is_pinned(void) {
    struct pinned_outcome control = pool_pinned_full_case(true);
    EXPECT_EQ_INT(1, control.best_effort_on_arrival);
    EXPECT_EQ_INT(1, control.best_effort_after);       // the resend is a duplicate to it
    EXPECT_EQ_INT(PINNED_AHEAD + 2, control.reliable); // A, the six, G
    struct pinned_outcome none = pool_pinned_full_case(false);
    EXPECT_EQ_INT(1, none.best_effort_on_arrival); // its own slot (0 until 2026-10-27: no slot for G)
    EXPECT_EQ_INT(1, none.best_effort_after);      // the resend is a duplicate to it
    EXPECT_EQ_INT(PINNED_AHEAD + 2, none.reliable);
}

// --- Reassembly slots: one per Subscriber, the pool as overflow (docs/DESIGN.md section 8; the user, 2026-10-27)
// -------

static void on_data_uncounted(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)seq_no;
    (void)data;
}

// A second BEST_EFFORT Subscriber on the receiver, of a topic of its own ("frag_topic_b") with a Publisher of its own
// on the sender; what it decodes is counted in second_delivered, and delivered_count stays the pair's Subscriber's.
static struct tt_TypeSupport topic_b_tx;
static struct tt_TypeSupport topic_b_rx;
static struct tt_Publisher pub_b;

static void add_best_effort_topic_b(void) {
    topic_b_tx = sender_topic;
    topic_b_tx.name = "frag_topic_b";
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&sender), &pub_b, &topic_b_tx,
                                                      ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    match_by_broadcast(&pub_b);
    topic_b_rx = receiver_topic;
    topic_b_rx.name = "frag_topic_b";
    topic_b_rx.data_decode = second_checking_decode;
    memset(&sub2, 0, sizeof(sub2));
    size_t size = tt_Subscriber_storage_size(&topic_b_rx, &TEST_QOS_UNRELIABLE);
    EXPECT_TRUE(size <= sizeof(own_slot_storage[1]));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub2, &topic_b_rx,
                                                       ENDPOINT_NAME, &TEST_QOS_UNRELIABLE,
                                                       &(struct tt_SubscriberOptions) {.callback = on_data_uncounted},
                                                       size != 0 ? own_slot_storage[1] : NULL, size));
    second_delivered = 0;
    second_intact = false;
}

// B, a sample of topic b, has its first fragment in; then `starts` samples of the pair's topic arrive with only their
// first fragment each (never whole); then B's last. Whether B is delivered: until 2026-10-27 the pool was both
// Subscribers' only room, and B's reassembly, the oldest, was abandoned for the other topic's once they filled it.
static int two_best_effort_case(int starts) {
    init_pair(2800);
    add_best_effort_topic_b();
    static struct captured_sample b;
    static struct captured_sample a;
    capture_sample(&pub_b, &b);
    EXPECT_EQ_INT(2, b.count);
    deliver_from(-1, &b, 0);
    for (int k = 0; k < starts; k++) {
        capture_sample(&pub, &a);
        deliver_from(-1, &a, 0);
    }
    deliver_from(-1, &b, 1);
    int got = second_delivered;
    EXPECT_TRUE(got == 0 || second_intact);
    EXPECT_EQ_INT(0, delivered_count); // none of the other topic's was whole
    // Control: the other Subscriber still takes a whole sample of its own.
    capture_sample(&pub, &a);
    deliver_from(-1, &a, 0);
    deliver_from(-1, &a, 1);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub2));
    return got;
}

static void test_two_best_effort_subscribers_do_not_take_each_others_slots(void) {
    // Control: with room in the pool, B is delivered whatever owns the slots.
    EXPECT_EQ_INT(1, two_best_effort_case(tt_FRAG_REASSEMBLY_SLOTS - 2));
    // Three pools' worth of the other topic's samples, each abandoned in turn: B is in its Subscriber's own slot.
    EXPECT_EQ_INT(1, two_best_effort_case(3 * tt_FRAG_REASSEMBLY_SLOTS));
    EXPECT_TRUE(receiver.frag_abandoned > 0); // the pool really was full, and reclaimed
}

// A BEST_EFFORT Subscriber's older sample, part-placed in its own slot, moves to the pool when a newer one of the same
// writer arrives (frag_newest_takes_own()), and still completes intact there: once with fragment 0 placed (the
// continuation size known, the bytes placed so far copied), once with only its last fragment (parked at the end of the
// slot, moved to the end of the pool slot's).
static void older_sample_moved_case(bool last_only) {
    init_pair(4000); // three fragments
    static struct captured_sample older;
    static struct captured_sample newer;
    sample_len = 100; // a DATA first: the Subscriber tracks the writer, so a continuation is its own from the start
    capture_sample(&pub, &older);
    deliver_from(-1, &older, 0);
    EXPECT_EQ_INT(1, delivered_count);
    delivered_count = 0;
    sample_len = 4000;
    sample_seed = 7;
    capture_sample(&pub, &older);
    EXPECT_EQ_INT(3, older.count);
    sample_seed = 9;
    capture_sample(&pub, &newer);
    if (last_only) {
        deliver_from(-1, &older, 2);
    } else {
        deliver_from(-1, &older, 0);
        deliver_from(-1, &older, 2);
    }
    uint32_t older_seq_no = sub.frag_own.seq_no;
    EXPECT_TRUE(sub.frag_own.received != 0); // the older, in its own slot
    deliver_from(-1, &newer, 0);
    EXPECT_TRUE(sub.frag_own.received != 0 && sub.frag_own.seq_no == older_seq_no + 3U); // the newer took it
    EXPECT_EQ_U64(0, receiver.frag_abandoned); // the older was moved, not given up
    expected_seed = 7;
    deliver_from(-1, &older, 1);
    if (last_only) {
        deliver_from(-1, &older, 0);
    }
    expect_delivered_once(4000);
    expected_seed = 9;
    deliver_from(-1, &newer, 1);
    deliver_from(-1, &newer, 2);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_TRUE(delivered_intact);
    sample_seed = 3;
    expected_seed = 3;
}

static void test_older_sample_moved_out_of_the_own_slot_completes(void) {
    older_sample_moved_case(false);
    older_sample_moved_case(true);
}

// A fragment of a sample older than the last one a BEST_EFFORT Subscriber was handed from that writer - a resend
// another Subscriber asked for - claims no slot: it could only be discarded.
static void test_best_effort_fragment_of_a_passed_sample_claims_nothing(void) {
    init_pair(2800);
    static struct captured_sample first;
    static struct captured_sample second;
    capture_sample(&pub, &first);
    capture_sample(&pub, &second);
    deliver_from(-1, &first, 0);
    deliver_from(-1, &first, 1);
    deliver_from(-1, &second, 0);
    deliver_from(-1, &second, 1);
    EXPECT_EQ_INT(2, delivered_count);
    uint64_t duplicates = receiver.frag_duplicate;
    deliver_from(-1, &first, 1); // the first sample's last fragment again: its slot is long reused
    EXPECT_EQ_INT(0, busy_slots());
    EXPECT_EQ_U64(duplicates + 1U, receiver.frag_duplicate);
    EXPECT_EQ_INT(2, delivered_count);
    // Control: the next sample's does claim one.
    capture_sample(&pub, &first);
    deliver_from(-1, &first, 1);
    EXPECT_EQ_INT(1, busy_slots());
}

// A sample larger than the Subscriber's own slot was sized for - a bound its binding under-stated, 1,000 bytes for a
// 2,800-byte sample - is put together in the pool instead, whole and intact, in either order of its fragments.
static void test_sample_larger_than_its_own_slot_goes_to_the_pool(void) {
    for (int reversed = 0; reversed < 2; reversed++) {
        init_pair(2800);
        EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub));
        receiver_topic.max_encoded_size = 1000;
        size_t size = tt_Subscriber_storage_size(&receiver_topic, &TEST_QOS_UNRELIABLE);
        EXPECT_EQ_U64(QOS_ALIGN8((size_t)tt_FRAG_DATA_HEADER_LENGTH + 1000U + 4U), size);
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub, &receiver_topic,
                                                           ENDPOINT_NAME, &TEST_QOS_UNRELIABLE,
                                                           &(struct tt_SubscriberOptions) {.callback = on_data},
                                                           own_slot_storage[0], sizeof(own_slot_storage[0])));
        static struct captured_sample s;
        sample_len = 100; // a DATA first: the Subscriber tracks the writer, so its continuations are its own
        capture_sample(&pub, &s);
        deliver_from(-1, &s, 0);
        delivered_count = 0;
        sample_len = 2800;
        capture_sample(&pub, &s);
        EXPECT_EQ_INT(2, s.count);
        deliver_from(-1, &s, reversed ? 1 : 0);
        EXPECT_EQ_U64(0, sub.frag_own.received); // moved out at once: it does not fit
        deliver_from(-1, &s, reversed ? 0 : 1);
        expect_delivered_once(2800);
        EXPECT_EQ_U64(0, receiver.frag_dropped);
    }
}

// Two BEST_EFFORT Subscribers of one writer each put its samples together in slots of their own, and each is handed
// each sample once - its own reassembly's, never the other's (process_data_for()'s `only`). Two samples interleaved,
// so the older one of each moves to the pool (frag_newest_takes_own()) where the other Subscriber's copy also is.
// The cost, counted: one reassembly per Subscriber per sample.
static void test_two_best_effort_subscribers_of_one_writer_each_reassemble(void) {
    init_pair(2800);
    add_best_effort_subscriber(); // sub2, same topic: what each decodes is counted apart
    static struct captured_sample s;
    static struct captured_sample t;
    sample_len = 100; // a DATA first: both track the writer
    capture_sample(&pub, &s);
    deliver_from(-1, &s, 0);
    EXPECT_EQ_INT(1, first_delivered);
    EXPECT_EQ_INT(1, second_delivered);
    first_delivered = second_delivered = 0;
    sample_len = 2800;
    capture_sample(&pub, &s);
    capture_sample(&pub, &t);
    uint64_t reassembled = receiver.frag_reassembled;
    deliver_from(-1, &s, 0);
    deliver_from(-1, &t, 0);
    deliver_from(-1, &s, 1);
    deliver_from(-1, &t, 1);
    EXPECT_EQ_INT(2, first_delivered);
    EXPECT_EQ_INT(2, second_delivered);
    EXPECT_TRUE(first_intact);
    EXPECT_TRUE(second_intact);
    EXPECT_EQ_U32(0, sub.out_of_order_discarded); // nothing handed twice
    EXPECT_EQ_U32(0, sub2.out_of_order_discarded);
    EXPECT_EQ_U64(reassembled + 4U, receiver.frag_reassembled);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub2));
}

// A no-buffer RELIABLE Subscriber records a sample ahead of its watermark only in the pool, never in its own slot:
// pinned there by a sample ahead, the slot would not be there for the sample the watermark moves to next, which would
// then wait on what other Subscribers leave in the pool (frag_pool_record_ahead()). So a sample ahead goes to the pool
// first - a free or spent slot, else the oldest unpinned of any owner's (frag_pool_for_ahead()) - and is recorded
// there; only with every pool slot pinned does it take the own slot, unrecorded. A's first datagram in the own slot,
// B's first in the pool (recorded ahead); A completes and the watermark moves to B; C arrives whole. With the pool full
// of another Subscriber's partial samples, C takes the oldest of them and is recorded (run 1 and run 2 of the PC A/B,
// 2026-10-27: C took the own slot, unrecorded, and was asked for again - 4 streams at 5% loss lost RELIABLE samples).
// B's repair delivers B, then C, in order.
static void ahead_case(bool pool_has_room) {
    tracked_none_pair(4000); // A: seq_no 2..4
    static struct captured_sample a;
    static struct captured_sample b;
    static struct captured_sample c;
    static struct captured_sample other;
    for (int d = 0; d < 3; d++) {
        memcpy(a.bytes[d], datagrams[d], datagram_len[d]);
        a.len[d] = datagram_len[d];
    }
    capture_sample(&pub, &b); // 5..7
    capture_sample(&pub, &c); // 8..10
    if (!pool_has_room) {
        add_best_effort_topic_b();
    }
    deliver_from(-1, &a, 0);
    EXPECT_TRUE(sub.frag_own.received != 0 && frag_slot_pinned(&sub.frag_own)); // A, at the watermark
    deliver_from(-1, &b, 0);
    EXPECT_TRUE(proxy_has_recorded(sender_proxy(), 5)); // B recorded ahead, in the pool
    deliver_from(-1, &a, 1);
    deliver_from(-1, &a, 2);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U32(6, sender_proxy()->ack_seq_no); // the watermark is in B
    for (int k = 0; !pool_has_room && k < 2 * tt_FRAG_REASSEMBLY_SLOTS; k++) {
        capture_sample(&pub_b, &other); // first datagrams only: another Subscriber's partial samples, unpinned
        deliver_from(-1, &other, 0);
    }
    if (!pool_has_room) {
        EXPECT_EQ_U32(tt_FRAG_REASSEMBLY_SLOTS, receiver.frag_slots_used);
        for (uint32_t i = 0; i < receiver.frag_slots_used; i++) {
            EXPECT_TRUE(receiver.frag_slots[i].received != 0); // nothing free or spent
        }
    }
    uint64_t abandoned = receiver.frag_abandoned;
    for (int d = 0; d < 3; d++) {
        deliver_from(-1, &c, d);
    }
    EXPECT_TRUE(!(sub.frag_own.done && sub.frag_own.seq_no == 8U));                // not in the own slot
    EXPECT_TRUE(proxy_has_recorded(sender_proxy(), 8));                            // recorded, in the pool
    EXPECT_EQ_U64(abandoned + (pool_has_room ? 0U : 1U), receiver.frag_abandoned); // the oldest partial, given up
    EXPECT_TRUE(!frag_slot_pinned(&sub.frag_own));
    deliver_from(-1, &b, 1);
    deliver_from(-1, &b, 2);
    EXPECT_EQ_INT(3, delivered_count); // B, then C
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_U32(11, sender_proxy()->ack_seq_no);
    if (!pool_has_room) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub2));
    }
}

// The guarantee itself: with every pool slot pinned, a sample ahead takes the own slot and stays unrecorded there.
// Seven more writers of the topic, each tracked (a DATA first), each with its watermark sample part-placed in the pool
// while A holds the own slot: seven pins whatever the record-ahead bound. B's first datagram takes the eighth slot,
// unrecorded (two unpinned would not stay); A completes, the watermark moves into B, which pins it. C arrives whole:
// nothing in the pool may be taken, the own slot is spent - C goes there, and is not recorded.
#define AHEAD_WRITERS (tt_FRAG_REASSEMBLY_SLOTS - 1)
static struct tt_Context ahead_node[AHEAD_WRITERS];
static struct tt_Publisher ahead_pub[AHEAD_WRITERS];

static void test_reliable_none_ahead_never_pins_the_own_slot(void) {
    ahead_case(true);
    ahead_case(false);

    tracked_none_pair(4000); // A: seq_no 2..4
    static struct captured_sample a;
    static struct captured_sample b;
    static struct captured_sample c;
    static struct captured_sample w;
    static struct captured_sample ws[AHEAD_WRITERS];
    for (int d = 0; d < 3; d++) {
        memcpy(a.bytes[d], datagrams[d], datagram_len[d]);
        a.len[d] = datagram_len[d];
    }
    capture_sample(&pub, &b); // 5..7
    capture_sample(&pub, &c); // 8..10
    uint32_t len = sample_len;
    for (int k = 0; k < AHEAD_WRITERS; k++) {
        init_bare_node(&ahead_node[k], (uint8_t)(3 + k));
        EXPECT_EQ_INT(tt_RET_OK,
                      tt_Node_create_publisher(tt_Context_default_node(&ahead_node[k]), &ahead_pub[k], &sender_topic,
                                               ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
        match_by_broadcast(&ahead_pub[k]);
        sample_len = 100;
        capture_sample(&ahead_pub[k], &w);
        deliver_from(k, &w, 0); // tracked
    }
    sample_len = len;
    delivered_count = 0;
    deliver_from(-1, &a, 0);
    for (int k = 0; k < AHEAD_WRITERS; k++) {
        capture_sample(&ahead_pub[k], &ws[k]);
        deliver_from(k, &ws[k], 0); // its watermark sample, in the pool: pinned
    }
    deliver_from(-1, &b, 0);
    EXPECT_TRUE(!proxy_has_recorded(sender_proxy(), 5)); // the bound: not recorded
    deliver_from(-1, &a, 1);
    deliver_from(-1, &a, 2);
    EXPECT_EQ_INT(1, delivered_count);
    EXPECT_EQ_U32((uint32_t)tt_FRAG_REASSEMBLY_SLOTS, frag_pool_pinned(&receiver)); // B now pinned too: all of them
    deliver_from(-1, &c, 0);
    deliver_from(-1, &c, 1);
    EXPECT_TRUE(sub.frag_own.received != 0 && sub.frag_own.seq_no == 8U); // C, part-placed, in the own slot
    // The other writers' samples complete, and the pool has room again: C's last datagram still lands in the own slot,
    // and recording ahead there stays refused - room in the pool does not make the own slot the place to pin.
    int before = delivered_count;
    for (int k = 0; k < AHEAD_WRITERS; k++) {
        deliver_from(k, &ws[k], 1);
        deliver_from(k, &ws[k], 2);
    }
    EXPECT_EQ_INT(before + AHEAD_WRITERS, delivered_count);
    EXPECT_TRUE(frag_pool_unpinned(&receiver, 3) > 2U);
    deliver_from(-1, &c, 2);
    EXPECT_TRUE(sub.frag_own.done && sub.frag_own.seq_no == 8U); // C, whole, in the own slot
    EXPECT_TRUE(!proxy_has_recorded(sender_proxy(), 8) && !proxy_has_recorded(sender_proxy(), 10)); // not recorded
    EXPECT_TRUE(!frag_slot_pinned(&sub.frag_own));
    deliver_from(-1, &b, 1);
    deliver_from(-1, &b, 2);
    EXPECT_EQ_INT(before + AHEAD_WRITERS + 2, delivered_count); // B, then C
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_U32(11, sender_proxy()->ack_seq_no);
}

// A destroyed Subscriber's pool slots go with it: no slot is left naming it, and its samples' fragments open none.
static void test_destroyed_subscriber_leaves_no_pool_slot(void) {
    init_pair(2800);
    add_best_effort_topic_b();
    static struct captured_sample b;
    for (int k = 0; k < 3; k++) {
        capture_sample(&pub_b, &b); // first fragments only: the newest in its own slot, the older ones in the pool
        deliver_from(-1, &b, 0);
    }
    int held = 0;
    for (uint32_t i = 0; i < receiver.frag_slots_used; i++) {
        held += receiver.frag_slots[i].owner == &sub2 && receiver.frag_slots[i].received != 0 ? 1 : 0;
    }
    EXPECT_EQ_INT(2, held);
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub2));
    for (uint32_t i = 0; i < receiver.frag_slots_used; i++) {
        EXPECT_TRUE(receiver.frag_slots[i].owner != &sub2);
        EXPECT_EQ_U64(0, receiver.frag_slots[i].received);
    }
}

// What each Subscriber's caller storage takes for its own slot (tt_Subscriber_storage_size()): a BEST_EFFORT or
// no-buffer RELIABLE one of an unbounded type, the largest sample; of a bounded one, its bound; nothing for a type no
// link fragments or a RELIABLE one with a buffer (its fragments wait there). Printed: the delta per Subscriber.
static void test_own_slot_storage_size(void) {
    struct tt_TypeSupport type = receiver_topic;
    struct tt_QosProfile none = TEST_QOS_RELIABLE_READER;
    none.reliability.reorder_slots = tt_QOS_REORDER_NONE;
    struct tt_QosProfile buffered = TEST_QOS_RELIABLE_READER;
    buffered.reliability.reorder_slots = TEST_REORDER_SLOTS;
    const size_t unbounded = QOS_ALIGN8((size_t)tt_FRAG_DATA_HEADER_LENGTH + tt_MAX_SAMPLE_LENGTH + 4U);
    type.max_encoded_size = 0;
    EXPECT_EQ_U64(unbounded, tt_Subscriber_storage_size(&type, &TEST_QOS_UNRELIABLE));
    EXPECT_EQ_U64(unbounded, tt_Subscriber_storage_size(&type, &none));
    size_t buffered_size = tt_Subscriber_storage_size(&type, &buffered);
    EXPECT_EQ_U64((size_t)TEST_REORDER_SLOTS * TEST_REORDER_SLOT_BYTES, buffered_size); // the buffer alone
    type.max_encoded_size = 1000;
    EXPECT_EQ_U64(QOS_ALIGN8((size_t)tt_FRAG_DATA_HEADER_LENGTH + 1000U + 4U),
                  tt_Subscriber_storage_size(&type, &TEST_QOS_UNRELIABLE));
    type.max_encoded_size_estimated = true; // a size the binding chose, not a bound: the largest sample
    EXPECT_EQ_U64(unbounded, tt_Subscriber_storage_size(&type, &TEST_QOS_UNRELIABLE));
    type.max_encoded_size_estimated = false;
    type.max_encoded_size = 64; // goes whole even on a tt_LINK_MTU_MIN link
    EXPECT_EQ_U64(0, tt_Subscriber_storage_size(&type, &TEST_QOS_UNRELIABLE));
    printf("test_data_frag: own reassembly slot per Subscriber at tt_MAX_SAMPLE_LENGTH %u: unbounded %zu B, bound "
           "1000 %zu B, bound 64 0 B, buffered RELIABLE 0 B\n",
           (unsigned)tt_MAX_SAMPLE_LENGTH, unbounded, QOS_ALIGN8((size_t)tt_FRAG_DATA_HEADER_LENGTH + 1000U + 4U));
}

// (a) again, the other way a watermark moves past a sample recorded ahead: a DATA. A writer's A (fragmented) has its
// first datagram only, the DATA X after it is lost, and B (fragmented) after that arrives whole and is recorded. A's
// repair delivers A, and the watermark stops at X; X's repair moves it past B's recorded datagrams at once. B must
// follow X, as it does from a buffer (the control). The pool walk runs when a fragment lands, and none does. With A's
// first datagram lost too, B is whole but not recorded (the pool held nothing of A's): X's repair moves the watermark
// to B, and B follows X at once rather than after its resend.
static void data_absorb_case(bool with_buffer, bool a_first_lost) {
    tracked_pair(4000, with_buffer); // A: seq_no 2..4, captured
    static struct captured_sample a;
    static struct captured_sample x;
    static struct captured_sample b;
    for (int d = 0; d < datagram_count && d < 4; d++) {
        memcpy(a.bytes[d], datagrams[d], datagram_len[d]);
        a.len[d] = datagram_len[d];
    }
    sample_len = 100;
    capture_sample(&pub, &x); // X: 5, a DATA
    sample_len = 4000;
    capture_sample(&pub, &b); // B: 6..8
    EXPECT_EQ_INT(1, x.count);
    if (!a_first_lost) {
        deliver_from(-1, &a, 0);
    }
    for (int d = 0; d < 3; d++) {
        deliver_from(-1, &b, d);
    }
    EXPECT_EQ_INT(0, delivered_count);
    for (int d = a_first_lost ? 0 : 1; d < 3; d++) {
        deliver_from(-1, &a, d);
    }
    EXPECT_EQ_INT(1, delivered_count); // A; X is missing
    EXPECT_EQ_U32(5, sender_proxy()->ack_seq_no);
    deliver_from(-1, &x, 0);
    EXPECT_EQ_U32(9, sender_proxy()->ack_seq_no);
    EXPECT_EQ_INT(3, delivered_count); // X, then B
    EXPECT_EQ_U32(4000, delivered_len);
    EXPECT_TRUE(delivered_intact);
    EXPECT_EQ_U32(0, sub.out_of_order_discarded);
}

static void test_reliable_none_data_absorbs_a_whole_sample(void) {
    for (int a_first_lost = 0; a_first_lost < 2; a_first_lost++) {
        data_absorb_case(true, a_first_lost); // the control
        data_absorb_case(false, a_first_lost);
    }
}

// (b) Many no-buffer RELIABLE streams in one node, more than the pool can pin. Two real nodes as in sim_run(): the
// sender has `streams` RELIABLE KEEP_ALL Publishers, each on a topic of its own, and the receiver a RELIABLE
// Subscriber of each - with no reorder buffer, or with a window-wide one (the control: it never touches the pool) -
// and optionally a BEST_EFFORT Subscriber of each beside it, for (c). Each round every Publisher publishes one
// 3-datagram sample, and the round's datagrams are shuffled before they cross the channel, so every stream's sample
// is being put together at once. What would show a livelock or a starved stream: a RELIABLE stream short of
// MS_SAMPLES, out of order, or torn when the budget ends.
#define MS_CACHE_DEPTH 256
#define MS_SAMPLE_LEN 4000 // 3 datagrams
#define MS_SUB_STORAGE (TEST_REORDER_SLOTS * TEST_REORDER_SLOT_BYTES + 65536)
static char ms_name[MS_MAX_STREAMS][16];
static struct tt_TypeSupport ms_tx_topic[MS_MAX_STREAMS];
static struct tt_TypeSupport ms_rx_topic[MS_MAX_STREAMS];
static struct tt_Publisher ms_pub[MS_MAX_STREAMS];
static struct tt_Subscriber ms_rel[MS_MAX_STREAMS];
static struct tt_Subscriber ms_be[MS_MAX_STREAMS];
static uint64_t ms_pub_storage[MS_MAX_STREAMS][(size_t)64U * 1024U / sizeof(uint64_t)];
static uint64_t ms_sub_storage[MS_MAX_STREAMS][MS_SUB_STORAGE / sizeof(uint64_t)];
static uint64_t ms_be_storage[MS_MAX_STREAMS][OWN_SLOT_STORAGE / sizeof(uint64_t)];
static struct tt_ReliableCacheIndex ms_cache_index[MS_MAX_STREAMS][MS_CACHE_DEPTH];
static uint8_t ms_cache_arena[MS_MAX_STREAMS]
                             [tt_RELIABLE_CACHE_ARENA_BYTES(MS_CACHE_DEPTH, tt_RELIABLE_RECORD_BYTES(1472))];
static struct tt_ReliableCache ms_cache[MS_MAX_STREAMS];

struct ms_tally {
    int got;
    uint16_t last;
    bool in_order;
    bool intact;
    bool has[MS_SAMPLES];
};
static struct ms_tally ms_rel_tally[MS_MAX_STREAMS];
static struct ms_tally ms_be_tally[MS_MAX_STREAMS];
static int ms_decoded_stream;
static bool ms_decoded_priming; // the DATA each stream starts with
static bool ms_decoded_intact;
static uint32_t ms_peak_pinned;

// The stream is in the bytes: seed 20 + its index.
static int32_t ms_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    uint8_t seed = len != 0 ? payload[0] : 0;
    ms_decoded_priming = len == 100;
    ms_decoded_stream = (int)seed - 20;
    ms_decoded_intact = len == MS_SAMPLE_LEN;
    for (uint32_t i = 0; i < len && ms_decoded_intact; i++) {
        ms_decoded_intact = payload[i] == pattern(i, seed);
    }
    return 0;
}

static void ms_tally(struct ms_tally* t, int stream, int expected_stream, uint16_t seq_no) {
    t->intact = t->intact && ms_decoded_intact && stream == expected_stream;
    t->in_order = t->in_order && (t->got == 0 || (uint16_t)(seq_no - t->last) < 0x8000U);
    t->last = seq_no;
    t->got++;
    uint32_t sample = ((uint32_t)seq_no - 2U) / 3U; // every sample takes three seq_no, the first from 2
    if (sample < MS_SAMPLES) {
        t->has[sample] = true;
    }
}

static void ms_on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)data;
    if (ms_decoded_priming) {
        return;
    }
    if (s >= ms_rel && s < ms_rel + MS_MAX_STREAMS) {
        ms_tally(&ms_rel_tally[s - ms_rel], ms_decoded_stream, (int)(s - ms_rel), seq_no);
    } else if (s >= ms_be && s < ms_be + MS_MAX_STREAMS) {
        ms_tally(&ms_be_tally[s - ms_be], ms_decoded_stream, (int)(s - ms_be), seq_no);
    }
}

static bool ms_all_reliable_done(int streams) {
    for (int w = 0; w < streams; w++) {
        if (ms_rel_tally[w].got < MS_SAMPLES) {
            return false;
        }
    }
    return true;
}

static uint32_t ms_shuffle_rng;
static void ms_shuffle_in_flight(void) {
    for (uint32_t i = sim_tail - sim_head; i > 1; i--) {
        ms_shuffle_rng = (ms_shuffle_rng * 1103515245U) + 12345U;
        uint32_t j = (ms_shuffle_rng >> 8) % i;
        static struct sim_datagram tmp;
        struct sim_datagram* x = &sim_queue[(sim_head + i - 1) % SIM_QUEUE];
        struct sim_datagram* y = &sim_queue[(sim_head + j) % SIM_QUEUE];
        tmp = *x;
        *x = *y;
        *y = tmp;
    }
}

struct ms_result {
    uint64_t took;     // simulated time from the last publish until every RELIABLE stream was complete, or budget
    double per_sample; // datagrams the sender sent, a sample
    uint64_t dropped;  // fragments dropped because every slot was pinned (frag_dropped)
    int be_complete_on_wire;
    int be_got_of_those;
};

// Stream w of multi_run(): its Publisher, with the simulation's own cache swapped in, and its Subscribers.
static void ms_create_stream(int w, const struct tt_QosProfile* pub_qos, const struct tt_QosProfile* rel_qos,
                             bool best_effort) {
    ms_rel_tally[w].in_order = ms_rel_tally[w].intact = true;
    ms_be_tally[w].in_order = ms_be_tally[w].intact = true;
    (void)snprintf(ms_name[w], sizeof(ms_name[w]), "ms_topic_%d", w);
    memset(&ms_tx_topic[w], 0, sizeof(ms_tx_topic[w]));
    ms_tx_topic[w].name = ms_name[w];
    ms_tx_topic[w].data_size = 8;
    ms_tx_topic[w].data_encode_size = sized_encode_size;
    ms_tx_topic[w].data_encode = sized_encode;
    size_t pub_size = tt_Publisher_storage_size(&ms_tx_topic[w], pub_qos);
    EXPECT_TRUE(pub_size <= sizeof(ms_pub_storage[w]));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(
                                 tt_Context_default_node(&sender), &ms_pub[w], &ms_tx_topic[w], ENDPOINT_NAME, pub_qos,
                                 &(struct tt_PublisherOptions) {.heartbeat_period_ns = 10 * tt_MILLISECOND},
                                 ms_pub_storage[w], pub_size));
    match_by_broadcast(&ms_pub[w]);
    memset(ms_cache_index[w], 0, sizeof(ms_cache_index[w]));
    memset(&ms_cache[w], 0, sizeof(ms_cache[w]));
    ms_cache[w].index = ms_cache_index[w];
    ms_cache[w].capacity = MS_CACHE_DEPTH;
    ms_cache[w].depth = MS_CACHE_DEPTH;
    ms_cache[w].arena = ms_cache_arena[w];
    ms_cache[w].arena_size = (uint32_t)sizeof(ms_cache_arena[w]);
    ms_pub[w].reliable_cache = &ms_cache[w];

    memset(&ms_rx_topic[w], 0, sizeof(ms_rx_topic[w]));
    ms_rx_topic[w].name = ms_name[w];
    ms_rx_topic[w].data_size = 8;
    ms_rx_topic[w].data_decode = ms_decode;
    ms_rx_topic[w].data_free = free_nothing;
    size_t sub_size = tt_Subscriber_storage_size(&ms_rx_topic[w], rel_qos);
    EXPECT_TRUE(sub_size <= sizeof(ms_sub_storage[w]));
    memset(ms_sub_storage[w], 0, sub_size);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&receiver), &ms_rel[w], &ms_rx_topic[w],
                                                       ENDPOINT_NAME, rel_qos,
                                                       &(struct tt_SubscriberOptions) {.callback = ms_on_data},
                                                       sub_size != 0 ? ms_sub_storage[w] : NULL, sub_size));
    if (best_effort) {
        size_t be_size = tt_Subscriber_storage_size(&ms_rx_topic[w], &TEST_QOS_UNRELIABLE);
        EXPECT_TRUE(be_size <= sizeof(ms_be_storage[w]));
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&receiver), &ms_be[w],
                                                           &ms_rx_topic[w], ENDPOINT_NAME, &TEST_QOS_UNRELIABLE,
                                                           &(struct tt_SubscriberOptions) {.callback = ms_on_data},
                                                           be_size != 0 ? ms_be_storage[w] : NULL, be_size));
    }
}

// Stream w's sample i, its original datagrams tagged; a publish that would block lets the protocol run until it can.
static void ms_publish(int w, int i, uint64_t budget) {
    sample_seed = (uint8_t)(20 + w);
    tt_ret_t result = tt_RET_WOULD_BLOCK;
    uint64_t blocked_since = test_mock_now;
    for (;;) {
        sim_tag_stream = (int16_t)w;
        sim_tag_sample = (int16_t)i;
        sim_tagged = 0;
        sim_acting = SENDER_ID;
        result = tt_Publisher_publish(&ms_pub[w], (struct tt_Data*)&sample_len);
        sim_tag_stream = sim_tag_sample = -1;
        if (result != tt_RET_WOULD_BLOCK || test_mock_now - blocked_since >= budget) {
            break;
        }
        sim_drain();
        if (!sim_step()) {
            break;
        }
    }
    EXPECT_EQ_INT(tt_RET_OK, result);
    EXPECT_EQ_U32(3, sim_tagged); // the sample's own datagrams, and nothing else
}

static void ms_note_pinned(void) {
    uint32_t pinned = frag_pool_pinned(&receiver);
    ms_peak_pinned = pinned > ms_peak_pinned ? pinned : ms_peak_pinned;
}

static struct ms_result multi_run(int streams, bool with_buffer, bool best_effort, uint32_t loss_per_mille,
                                  uint64_t budget) {
    const struct tt_QosProfile pub_qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL, .depth = 1},
                                          .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    struct tt_QosProfile rel_qos = TEST_QOS_RELIABLE_READER;
    if (with_buffer) {
        rel_qos.reliability.reorder_slots = TEST_REORDER_SLOTS;
    }
    test_mock_reset();
    sample_len = MS_SAMPLE_LEN;
    init_bare_node(&sender, SENDER_ID);
    init_bare_node(&receiver, RECEIVER_ID);
    memset(ms_rel_tally, 0, sizeof(ms_rel_tally));
    memset(ms_be_tally, 0, sizeof(ms_be_tally));
    memset(sim_on_wire, 0, sizeof(sim_on_wire));
    for (int w = 0; w < streams; w++) {
        ms_create_stream(w, &pub_qos, &rel_qos, best_effort);
        EXPECT_TRUE(with_buffer || reorder_payload_capacity(&ms_rel[w]) == 0);
    }
    sim_head = sim_tail = 0;
    sim_rng = 1;
    ms_shuffle_rng = 7;
    ms_peak_pinned = 0;
    memset(sim_sent, 0, sizeof(sim_sent));
    memset(sim_dropped, 0, sizeof(sim_dropped));
    test_mock_send_hook = sim_capture;
    test_mock_now = tt_SECOND;
    // A DATA first on each stream (seq_no 1), over a clean channel, so every Subscriber tracks its writer before the
    // first fragment: a RELIABLE one with no buffer takes no fragment from a writer it does not track yet
    // (accept_reliable_fragment()), and a VOLATILE one would then start from the writer's first HEARTBEAT, past what
    // was sent before it.
    sim_loss_per_mille = 0;
    sample_len = 100;
    for (int w = 0; w < streams; w++) {
        sample_seed = (uint8_t)(20 + w);
        sim_acting = SENDER_ID;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&ms_pub[w], (struct tt_Data*)&sample_len));
    }
    sample_len = MS_SAMPLE_LEN;
    sim_drain();
    sim_loss_per_mille = loss_per_mille;
    for (int i = 0; i < MS_SAMPLES; i++) {
        sim_run_due(&sender);
        sim_run_due(&receiver);
        for (int w = 0; w < streams; w++) { // nothing crosses the channel until the round is out, unless one blocks
            ms_publish(w, i, budget);
        }
        ms_shuffle_in_flight();
        sim_drain();
        ms_note_pinned();
        test_mock_now += SIM_PUBLISH_SPACING;
    }
    uint64_t published = test_mock_now;
    while (!ms_all_reliable_done(streams) && test_mock_now - published < budget && sim_step()) {
        ms_note_pinned();
    }
    test_mock_send_hook = NULL;
    struct ms_result r = {.took = test_mock_now - published,
                          .per_sample = (double)sim_sent[SENDER_ID] / (double)(streams * MS_SAMPLES),
                          .dropped = receiver.frag_dropped};
    for (int w = 0; w < streams; w++) {
        for (int i = 0; i < MS_SAMPLES; i++) {
            if (sim_on_wire[w][i] == 3) {
                r.be_complete_on_wire++;
                r.be_got_of_those += ms_be_tally[w].has[i] ? 1 : 0;
            }
        }
    }
    return r;
}

static void expect_every_reliable_stream_complete(int streams) {
    for (int w = 0; w < streams; w++) {
        if (ms_rel_tally[w].got != MS_SAMPLES) {
            struct tt_WriterProxy* proxy = find_writer_proxy(&ms_rel[w], SENDER_ID, ms_pub[w].endpoint.entity_id);
            printf("test_data_frag: stream %d of %d got %d of %d (last seq_no %u, watermark %u)\n", w, streams,
                   ms_rel_tally[w].got, MS_SAMPLES, ms_rel_tally[w].last, proxy != NULL ? proxy->ack_seq_no : 0U);
        }
        EXPECT_EQ_INT(MS_SAMPLES, ms_rel_tally[w].got);
        EXPECT_TRUE(ms_rel_tally[w].in_order);
        EXPECT_TRUE(ms_rel_tally[w].intact);
    }
}

static void test_many_reliable_none_streams_all_progress(void) {
    static const int stream_counts[] = {4, 8, 12, 16};
    static const uint32_t losses[] = {0, 10, 50};
    for (size_t s = 0; s < sizeof(stream_counts) / sizeof(stream_counts[0]); s++) {
        for (size_t l = 0; l < sizeof(losses) / sizeof(losses[0]); l++) {
            int streams = stream_counts[s];
            struct ms_result control = multi_run(streams, true, false, losses[l], 20 * tt_SECOND);
            expect_every_reliable_stream_complete(streams);
            struct ms_result none = multi_run(streams, false, false, losses[l], 20 * tt_SECOND);
            expect_every_reliable_stream_complete(streams);
            if (losses[l] == 0 && streams <= tt_FRAG_REASSEMBLY_SLOTS) {
                EXPECT_EQ_U64(0, none.dropped); // a slot for every stream's sample: nothing to drop on a clean link
            }
            printf("test_data_frag: %2d no-buffer streams at %2u/1000 loss - recovery %6.1f ms, %.2f datagrams a "
                   "sample, %lu dropped (all pinned), peak %u pinned; buffered control %6.1f ms, %.2f\n",
                   streams, losses[l], (double)none.took / 1e6, none.per_sample, (unsigned long)none.dropped,
                   ms_peak_pinned, (double)control.took / 1e6, control.per_sample);
        }
    }
    // The pool was under pressure: with more streams than slots and loss, recording ahead filled it to its bound (two
    // left unpinned, frag_pool_record_ahead()). Until 2026-10-27, with the pool every stream's only room, this run
    // dropped 4,201 fragments with every slot pinned; each Subscriber's own slot now takes the sample it waits on.
    struct ms_result crowded = multi_run(16, false, false, 50, 20 * tt_SECOND);
    EXPECT_TRUE(ms_peak_pinned >= (uint32_t)tt_FRAG_REASSEMBLY_SLOTS - 2U);
    EXPECT_EQ_U64(0, crowded.dropped);
}

// (c) again, under load: of the samples whose original datagrams all crossed the channel, how many the BEST_EFFORT
// Subscriber got - beside a no-buffer RELIABLE one, and beside a buffered one (which leaves the pool to the BEST_EFFORT
// Subscribers). Every one, in both (the user, 2026-10-27: a Subscriber's delivery depends on its own QoS, the network
// and its own reading only). Until then, with the pool every Subscriber's only room, the BEST_EFFORT ones got 152 of
// 265 beside 8 no-buffer streams at 5% loss - the RELIABLE ones' held samples pinned every slot - and 154 of 640 beside
// 16 buffered ones on a clean link: 16 BEST_EFFORT Subscribers' samples, interleaved, took each other's slots.
static void test_best_effort_beside_many_reliable_none_streams(void) {
    static const int stream_counts[] = {3, 8, 16};
    static const uint32_t losses[] = {0, 10, 50};
    for (size_t s = 0; s < sizeof(stream_counts) / sizeof(stream_counts[0]); s++) {
        for (size_t l = 0; l < sizeof(losses) / sizeof(losses[0]); l++) {
            int streams = stream_counts[s];
            struct ms_result control = multi_run(streams, true, true, losses[l], 20 * tt_SECOND);
            expect_every_reliable_stream_complete(streams);
            struct ms_result none = multi_run(streams, false, true, losses[l], 20 * tt_SECOND);
            expect_every_reliable_stream_complete(streams);
            for (int w = 0; w < streams; w++) {
                EXPECT_TRUE(ms_be_tally[w].in_order);
                EXPECT_TRUE(ms_be_tally[w].intact);
            }
            if (losses[l] == 0) {
                EXPECT_EQ_INT(streams * MS_SAMPLES, none.be_got_of_those);
            }
            EXPECT_EQ_INT(none.be_complete_on_wire, none.be_got_of_those);
            EXPECT_EQ_INT(control.be_complete_on_wire, control.be_got_of_those);
            printf("test_data_frag: BEST_EFFORT beside %2d no-buffer streams at %2u/1000 loss - got %d of %d whole on "
                   "the wire (%lu dropped all pinned); beside buffered %d of %d\n",
                   streams, losses[l], none.be_got_of_those, none.be_complete_on_wire, (unsigned long)none.dropped,
                   control.be_got_of_those, control.be_complete_on_wire);
        }
    }
}

int main(void) {
    test_a_narrow_link_cuts_fragments_a_default_reader_reassembles();
    test_a_narrow_link_keeps_whole_only_what_fits_it();
    test_a_narrow_link_retransmits_the_fragment_it_cut();
#if tt_HYBRID_TRANSPORT
    test_a_narrow_link_sends_no_runs();
#endif
    test_topics_up_to_the_sample_limit_can_be_created();
    test_largest_data_is_not_fragmented();
    test_one_byte_more_is_two_fragments();
    test_p4_is_two_datagrams_of_the_predicted_size();
    test_every_arrival_order_reassembles();
    test_missing_fragment_delivers_nothing();
    test_duplicate_fragments_deliver_once();
    test_samples_interleave_across_slots();
    test_late_fragment_of_a_completed_sample_opens_no_slot();
    test_full_pool_abandons_the_oldest_and_counts_it();
    test_inconsistent_fragments_are_dropped_and_counted();
    test_sample_above_the_limit_is_refused();
    test_batch_ahead_of_a_fragmented_sample_goes_first();
    test_zero_copy_publish_fragments_too();
    test_every_datagram_takes_its_own_seq_no();
    test_sample_datagrams_matches_what_is_sent();
    test_sample_cache_bytes_bounds_what_is_cached();
    test_reliable_fragments_reassemble_in_every_order();
    test_reliable_missing_fragment_is_tracked_as_its_own_gap();
    test_reliable_sample_given_up_on_is_never_delivered_torn();
    test_reliable_fragment_without_room_is_left_unrecorded();
    test_reliable_none_fragments_in_order_are_delivered();
    test_reliable_none_lost_fragment_is_asked_for_and_completes();
    test_reliable_none_duplicate_does_not_disturb_the_sample();
    test_reliable_none_sample_given_up_on_frees_the_pool();
    test_reliable_two_writers_interleaved();
    test_reliable_in_order_sample_never_touches_the_reorder_buffer();
    test_reliable_out_of_order_fragments_still_deliver_through_the_buffer();
    test_reliable_fast_path_interrupted_by_a_lost_fragment();
    test_reliable_fast_path_sample_given_up_on_is_never_delivered_torn();
    test_reliable_two_writers_interleaved_after_first_contact();
    test_reliable_none_lost_fragment_alone_completes();
    test_reliable_none_sample_ahead_follows_in_order();
    test_reliable_none_two_writers_interleaved();
    test_reliable_none_recorded_sample_survives_a_full_pool();
    test_reliable_two_subscribers_share_the_scratch();
    test_reliable_buffer_delivery_does_not_overwrite_the_fast_path();
    test_reliable_fast_path_does_not_overtake_a_lost_sample();
    test_reliable_fast_path_needs_room_in_the_buffer();
    test_reliable_fast_path_refuses_what_does_not_belong_to_its_sample();
    test_reliable_fast_path_writes_nothing_past_its_scratch();
    test_reliable_fast_path_moves_out_before_another_writer_is_held();
    test_reliable_fast_path_waits_while_anything_is_held();
    test_reliable_fast_path_survives_another_writers_drain();
    test_reliable_fast_path_ignores_another_subscribers_hold();
    test_reliable_fast_path_keeps_the_reorder_cursor_with_the_watermark();
    test_reliable_fast_path_follows_its_writer_and_subscriber_out();
    test_keep_last_evicts_whole_samples();
    test_sample_depth_bounds_keep_last_in_samples();
    test_keep_all_counts_every_datagram();
    test_keep_all_never_evicts_the_unacknowledged_rest_of_a_sample();
    test_retransmission_resends_only_the_lost_datagram();
    test_original_and_retransmission_agree_on_fragment_count();
    test_durability_backlog_sends_fragmented_samples();
    test_reverse_endian_fragments_reassemble();
    test_a_fragmented_sample_is_one_batch_per_destination();
    test_one_datagram_to_one_destination_is_not_batched();
    test_one_datagram_to_several_peers_is_one_batch();
    test_a_failed_batch_fails_the_publish();
    test_lossless_link_sends_two_datagrams_a_sample();
    test_lossy_link_recovers_every_sample();
    test_loss_costs_the_lost_datagram_not_the_sample();
    test_reliable_none_eviction_heartbeat_keeps_a_whole_sample_ahead();
    test_reliable_none_data_absorbs_a_whole_sample();
    test_best_effort_beside_reliable_none_gets_every_sample();
    test_best_effort_beside_reliable_none_when_every_slot_is_pinned();
    test_two_best_effort_subscribers_do_not_take_each_others_slots();
    test_older_sample_moved_out_of_the_own_slot_completes();
    test_best_effort_fragment_of_a_passed_sample_claims_nothing();
    test_sample_larger_than_its_own_slot_goes_to_the_pool();
    test_two_best_effort_subscribers_of_one_writer_each_reassemble();
    test_reliable_none_ahead_never_pins_the_own_slot();
    test_destroyed_subscriber_leaves_no_pool_slot();
    test_own_slot_storage_size();
    test_many_reliable_none_streams_all_progress();
    test_best_effort_beside_many_reliable_none_streams();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_data_frag: all tests passed\n");
    return 0;
}
