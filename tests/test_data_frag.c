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
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

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

static struct tt_Node sender;
static struct tt_Node receiver;
static struct tt_Topic sender_topic;
static struct tt_Topic receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;

static void init_bare_node(struct tt_Node* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

static void init_pair(uint32_t len) {
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
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&sender, &pub, &sender_topic, ENDPOINT_NAME));

    init_bare_node(&receiver, RECEIVER_ID);
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = 8;
    receiver_topic.data_decode = checking_decode;
    receiver_topic.data_free = free_nothing;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&receiver, &sub, &receiver_topic, ENDPOINT_NAME, on_data));
}

static void deliver(int d) {
    EXPECT_TRUE(process_packet(&receiver, datagrams[d], 0, datagram_len[d], SENDER_IP, PORT));
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
    struct tt_Topic big_topic = sender_topic;
    big_topic.name = "big";
    big_topic.data_decode = checking_decode;
    big_topic.data_free = free_nothing;
    big_topic.data_size = tt_MAX_SAMPLE_LENGTH;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&sender, &big_pub, &big_topic, ENDPOINT_NAME));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&receiver, &big_sub, &big_topic, ENDPOINT_NAME, on_data));
    // Control: one byte more is still refused.
    struct tt_Publisher too_big_pub;
    struct tt_Subscriber too_big_sub;
    big_topic.data_size = tt_MAX_SAMPLE_LENGTH + 1;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Node_create_publisher(&sender, &too_big_pub, &big_topic, "other"));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT,
                  tt_Node_create_subscriber(&receiver, &too_big_sub, &big_topic, "other", on_data));
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

static int busy_slots(void) {
    int busy = 0;
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
    EXPECT_TRUE(process_packet(&receiver, first[0], 0, first_len[0], SENDER_IP, PORT));
    deliver(0);
    EXPECT_TRUE(process_packet(&receiver, first[1], 0, first_len[1], SENDER_IP, PORT));
    EXPECT_EQ_INT(1, delivered_count);
    deliver(1);
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_TRUE(delivered_intact);
}

static void test_full_pool_abandons_the_oldest_and_counts_it(void) {
    // Fill every slot with a sample missing its last fragment, then start one more.
    init_pair(2800);
    uint8_t lasts[tt_FRAG_REASSEMBLY_SLOTS + 1][tt_MAX_BUFFER_LENGTH];
    uint32_t last_len[tt_FRAG_REASSEMBLY_SLOTS + 1];
    for (int s = 0; s <= tt_FRAG_REASSEMBLY_SLOTS; s++) {
        publish_captured();
        deliver(0);
        memcpy(lasts[s], datagrams[1], datagram_len[1]);
        last_len[s] = datagram_len[1];
    }
    EXPECT_EQ_U64(1, receiver.frag_abandoned); // the ninth took the first one's slot
    EXPECT_EQ_INT(0, delivered_count);

    // The abandoned sample's last fragment cannot complete it - it starts a new, incomplete slot...
    EXPECT_TRUE(process_packet(&receiver, lasts[0], 0, last_len[0], SENDER_IP, PORT));
    EXPECT_EQ_INT(0, delivered_count);
    // ...while every sample that kept its slot still completes (control: abandonment took exactly one).
    // The new slot for sample 0 took sample 1's, so start from 2.
    EXPECT_EQ_U64(2, receiver.frag_abandoned);
    for (int s = 2; s <= tt_FRAG_REASSEMBLY_SLOTS; s++) {
        EXPECT_TRUE(process_packet(&receiver, lasts[s], 0, last_len[s], SENDER_IP, PORT));
    }
    EXPECT_EQ_INT(tt_FRAG_REASSEMBLY_SLOTS - 1, delivered_count);
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
    EXPECT_EQ_INT(tt_RET_PROTOCOL_ERROR, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_INT(0, datagram_count);
    EXPECT_EQ_U32(0, pub.seq_no);
    EXPECT_EQ_U32(sizeof(struct tt_Header), sender.tx_tail);
    // Control: the limit itself is accepted.
    sample_len = tt_MAX_SAMPLE_LENGTH;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    EXPECT_EQ_INT((int)frag_count_for(tt_MAX_SAMPLE_LENGTH), datagram_count);
    for (int d = 0; d < datagram_count; d++) {
        deliver(d);
    }
    expect_delivered_once(tt_MAX_SAMPLE_LENGTH);
}

static void test_batch_ahead_of_a_fragmented_sample_goes_first(void) {
    // A batching publisher leaves a small DATA pending; the large sample behind it must not overtake
    // it, and must not stay in tx_buffer either.
    init_pair(100);
    pub.batch = true;
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
    for (uint8_t i = 0; i < n; i++) {
        pub.peers[i] = (struct tt_Peer) {.ip = RECEIVER_IP + i, .port = PORT, .node_id = (uint8_t)(RECEIVER_ID + i)};
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

static const struct tt_Peer receiver_peer = {.ip = RECEIVER_IP, .port = PORT, .node_id = RECEIVER_ID};

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
    init_pair(2800);
    make_reliable();
    pub.durable = true;
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

static void sim_capture(const void* buf, size_t len) {
    EXPECT_TRUE(sim_tail - sim_head < SIM_QUEUE);
    EXPECT_TRUE(len <= tt_MAX_BUFFER_LENGTH);
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

// Delivers everything in flight, and whatever that provokes, until the channel is empty.
static void sim_drain(void) {
    while (sim_head != sim_tail) {
        struct sim_datagram* d = &sim_queue[sim_head % SIM_QUEUE];
        sim_head++;
        if (sim_lost()) {
            sim_dropped[d->from]++;
            continue;
        }
        struct tt_Node* to = d->from == SENDER_ID ? &receiver : &sender;
        sim_acting = to->id;
        process_packet(to, d->bytes, 0, d->len, d->from == SENDER_ID ? SENDER_IP : RECEIVER_IP, PORT);
    }
}

static void sim_run_due(struct tt_Node* node) {
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
    init_pair(size);
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
    pub.reliable_cache = &sim_cache;
    pub.reliable = true;
    pub.keep_all = true; // c6 is KEEP_ALL, where a Publisher never stops answering for a sample
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, 10 * tt_MILLISECOND));

    sub.callback = sim_on_data;
    sub.reliable = true;
    memset(sim_reorder, 0, sizeof(sim_reorder));
    sub.reorder_storage = sim_reorder;
    sub.reorder_slots = SIM_REORDER_SLOTS;
    sub.reorder_slot_bytes = SIM_REORDER_SLOT_BYTES;

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

#define TEST_REORDER_SLOTS 64
#define TEST_REORDER_SLOT_BYTES tt_REORDER_SLOT_SIZE(1472)
static uint64_t test_reorder[TEST_REORDER_SLOTS * TEST_REORDER_SLOT_BYTES / sizeof(uint64_t)];

static void make_receiver_reliable(bool with_buffer) {
    sub.reliable = true;
    memset(test_reorder, 0, sizeof(test_reorder));
    sub.reorder_storage = with_buffer ? test_reorder : NULL;
    sub.reorder_slots = with_buffer ? TEST_REORDER_SLOTS : 0;
    sub.reorder_slot_bytes = with_buffer ? TEST_REORDER_SLOT_BYTES : 0;
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
        init_pair(4000);
        make_receiver_reliable(true);
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
    init_pair(4000);
    make_receiver_reliable(true);
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
    init_pair(4000);
    make_receiver_reliable(true);
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
    init_pair(2800);
    make_receiver_reliable(false);
    publish_captured();
    deliver(0);
    struct tt_WriterProxy* proxy = sender_proxy();
    EXPECT_TRUE(proxy == NULL || proxy->ack_seq_no <= 1);
    EXPECT_TRUE(sub.reorder_overflow >= 1);
    EXPECT_EQ_INT(0, delivered_count);
    // Control: the same datagrams with a buffer are recorded and delivered.
    init_pair(2800);
    make_receiver_reliable(true);
    publish_captured();
    deliver(0);
    deliver(1);
    expect_delivered_once(2800);
}

static void test_reliable_two_writers_interleaved(void) {
    // Two writers of one topic count from 1 alike, and a fragment is held even in order - the per-writer
    // slot offset is what keeps their datagrams from colliding.
    init_pair(2800);
    make_receiver_reliable(true);
    static struct tt_Node second;
    static struct tt_Publisher second_pub;
    init_bare_node(&second, 3);
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&second, &second_pub, &sender_topic, ENDPOINT_NAME));
    publish_captured();
    uint8_t first_writer[2][tt_MAX_BUFFER_LENGTH];
    uint32_t first_len[2];
    for (int d = 0; d < 2; d++) {
        memcpy(first_writer[d], datagrams[d], datagram_len[d]);
        first_len[d] = datagram_len[d];
    }
    start_capture();
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&second_pub, (struct tt_Data*)&sample_len));
    EXPECT_TRUE(process_packet(&receiver, first_writer[0], 0, first_len[0], SENDER_IP, PORT));
    EXPECT_TRUE(process_packet(&receiver, datagrams[0], 0, datagram_len[0], SENDER_IP + 2, PORT));
    EXPECT_TRUE(process_packet(&receiver, first_writer[1], 0, first_len[1], SENDER_IP, PORT));
    EXPECT_TRUE(process_packet(&receiver, datagrams[1], 0, datagram_len[1], SENDER_IP + 2, PORT));
    EXPECT_EQ_INT(2, delivered_count);
    EXPECT_EQ_U32(0, sub.reorder_overflow);
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
    pub.peer_acks[0].node_id = RECEIVER_ID;
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

int main(void) {
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
    test_reliable_two_writers_interleaved();
    test_keep_last_evicts_whole_samples();
    test_sample_depth_bounds_keep_last_in_samples();
    test_keep_all_counts_every_datagram();
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

    if (test_result() != 0) {
        return 1;
    }

    printf("test_data_frag: all tests passed\n");
    return 0;
}
