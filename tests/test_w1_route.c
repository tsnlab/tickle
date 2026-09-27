/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// W1 (rmw_tickle/WIRE_PLAN.md 9.1, branch prototype): the short form's routing and the writer's long/short choice.
// Each routing test names the mutant it was shown to fail against (examples/perf_hil/experiments/w1_mutants.sh).

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

#define LOCAL_NODE_ID 1
#define ENDPOINT_ID 0xaabbccdd
// (source 2, handle 5) and (source 10, handle 4) share route slot 42: (2 ^ 40) & 63 == (10 ^ 32) & 63.
#define WRITER_A_SOURCE 2
#define WRITER_A_HANDLE 5
#define WRITER_A_ENTITY 77
#define WRITER_B_SOURCE 10
#define WRITER_B_HANDLE 4
#define WRITER_B_ENTITY 88
#define SAMPLE_BYTES 4
#define SAMPLES 6

static int callbacks[2];

static int32_t stub_encode_size(struct tt_Data* data) {
    (void)data;
    return SAMPLE_BYTES;
}

static int32_t stub_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < SAMPLE_BYTES) {
        return -1;
    }
    memcpy(payload, data, SAMPLE_BYTES);
    return SAMPLE_BYTES;
}

static int32_t stub_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < SAMPLE_BYTES) {
        return -1;
    }
    memcpy(data, payload, SAMPLE_BYTES);
    return SAMPLE_BYTES;
}

static void stub_free(struct tt_Data* data) {
    (void)data;
}

static struct tt_Subscriber subs[2];

static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)seq_no;
    (void)data;
    callbacks[sub - subs]++;
}

static struct tt_Topic topic;
static struct tt_Node receiver;

static void init_receiver(int subscriber_count) {
    test_mock_reset();
    memset(&receiver, 0, sizeof(receiver));
    node_init_locks(&receiver);
    receiver.id = LOCAL_NODE_ID;
    receiver.tx_tail = sizeof(struct tt_Header);
    receiver.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    receiver.route_generation = 1;
    memset(&topic, 0, sizeof(topic));
    topic.name = "w1_topic";
    topic.data_size = SAMPLE_BYTES;
    topic.data_encode_size = stub_encode_size;
    topic.data_encode = stub_encode;
    topic.data_decode = stub_decode;
    topic.data_free = stub_free;
    for (int i = 0; i < subscriber_count; i++) {
        memset(&subs[i], 0, sizeof(subs[i]));
        subs[i].endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
        subs[i].endpoint.id = ENDPOINT_ID;
        subs[i].node = &receiver;
        subs[i].topic = &topic;
        subs[i].callback = on_sample;
        receiver.endpoints[i] = (struct tt_Endpoint*)&subs[i];
    }
    receiver.endpoint_count = (uint32_t)subscriber_count;
    receiver.endpoint_index_valid = false;
    callbacks[0] = 0;
    callbacks[1] = 0;
}

// A short form from (source, handle), in the single-submessage form, as to_short_form() leaves it.
static void receive_short(uint8_t source, uint16_t handle, uint32_t seq_no) {
    uint8_t datagram[sizeof(struct tt_SingleHeader) + sizeof(struct tt_ShortDataHeader) + SAMPLE_BYTES];
    struct tt_SingleHeader single = {native_single_marker(), tt_VERSION, source, tt_SUBMESSAGE_TYPE_DATA_SHORT};
    struct tt_ShortDataHeader header = {handle, 0, seq_no, 0, 0};
    uint32_t value = seq_no;
    memcpy(datagram, &single, sizeof(single));
    memcpy(datagram + sizeof(single), &header, sizeof(header));
    memcpy(datagram + sizeof(single) + sizeof(header), &value, SAMPLE_BYTES);
    memcpy(receiver.rx_buffer, datagram, sizeof(datagram));
    (void)process_packet(&receiver, receiver.rx_buffer, 0, (uint32_t)sizeof(datagram), 0x0a000002U, 8282);
}

// A short form from a known writer is delivered, the first through the directory and the rest through the route.
// Mutant drop_on_miss (a route-table miss drops rather than asks the directory) fails it.
static void test_known_writer_routes_through_directory_then_table(void) {
    init_receiver(1);
    handle_directory_put(&receiver, WRITER_A_SOURCE, WRITER_A_HANDLE, ENDPOINT_ID, WRITER_A_ENTITY);
    for (uint32_t seq = 1; seq <= SAMPLES; seq++) {
        receive_short(WRITER_A_SOURCE, WRITER_A_HANDLE, seq);
    }
    EXPECT_EQ_INT(SAMPLES, callbacks[0]);
    EXPECT_EQ_U32(1, (uint32_t)receiver.short_slow);
    EXPECT_EQ_U32(SAMPLES - 1, (uint32_t)receiver.short_routed);
    EXPECT_EQ_U32(0, (uint32_t)receiver.short_unrouted);
}

// A route filled at an older generation never routes: the Subscriber it names was replaced, and the sample goes
// to its successor. Mutant no_generation (the generation compare removed) fails it.
static void test_a_stale_generation_never_routes(void) {
    init_receiver(1);
    handle_directory_put(&receiver, WRITER_A_SOURCE, WRITER_A_HANDLE, ENDPOINT_ID, WRITER_A_ENTITY);
    receive_short(WRITER_A_SOURCE, WRITER_A_HANDLE, 1); // fills the route with subs[0]
    EXPECT_EQ_INT(1, callbacks[0]);
    // subs[0] goes and subs[1] takes its place, as a destroy and a create would leave it.
    memset(&subs[1], 0, sizeof(subs[1]));
    subs[1].endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    subs[1].endpoint.id = ENDPOINT_ID;
    subs[1].node = &receiver;
    subs[1].topic = &topic;
    subs[1].callback = on_sample;
    receiver.endpoints[0] = (struct tt_Endpoint*)&subs[1];
    receiver.endpoint_index_valid = false;
    receiver.route_generation++;
    receive_short(WRITER_A_SOURCE, WRITER_A_HANDLE, 2);
    EXPECT_EQ_INT(1, callbacks[0]);
    EXPECT_EQ_INT(1, callbacks[1]);
}

// A writer the directory does not know shares the slot of one it does: it is dropped and counted, never handed
// to the known writer's Subscriber. Mutant no_tag (the source/handle compare removed) fails it.
static void test_an_unknown_writer_in_a_known_slot_is_dropped_not_misrouted(void) {
    init_receiver(1);
    handle_directory_put(&receiver, WRITER_A_SOURCE, WRITER_A_HANDLE, ENDPOINT_ID, WRITER_A_ENTITY);
    receive_short(WRITER_A_SOURCE, WRITER_A_HANDLE, 1);
    receive_short(WRITER_B_SOURCE, WRITER_B_HANDLE, 1);
    EXPECT_EQ_INT(1, callbacks[0]);
    EXPECT_EQ_U32(1, (uint32_t)receiver.short_unrouted);
}

// Two known writers in one slot, alternating: each evicts the other, and every sample is still delivered through
// the directory. Mutant drop_on_miss fails it.
static void test_two_writers_in_one_slot_both_deliver_every_sample(void) {
    init_receiver(1);
    handle_directory_put(&receiver, WRITER_A_SOURCE, WRITER_A_HANDLE, ENDPOINT_ID, WRITER_A_ENTITY);
    handle_directory_put(&receiver, WRITER_B_SOURCE, WRITER_B_HANDLE, ENDPOINT_ID, WRITER_B_ENTITY);
    for (uint32_t seq = 1; seq <= SAMPLES; seq++) {
        receive_short(WRITER_A_SOURCE, WRITER_A_HANDLE, seq);
        receive_short(WRITER_B_SOURCE, WRITER_B_HANDLE, seq);
    }
    EXPECT_EQ_INT(2 * SAMPLES, callbacks[0]);
    EXPECT_EQ_U32(0, (uint32_t)receiver.short_unrouted);
}

// Two local Subscribers of one topic each receive every sample: the route is never filled for a fan-out. Mutant
// fill_multi (fills whenever at least one Subscriber took it) fails it.
static void test_two_local_subscribers_both_receive_every_sample(void) {
    init_receiver(2);
    handle_directory_put(&receiver, WRITER_A_SOURCE, WRITER_A_HANDLE, ENDPOINT_ID, WRITER_A_ENTITY);
    for (uint32_t seq = 1; seq <= SAMPLES; seq++) {
        receive_short(WRITER_A_SOURCE, WRITER_A_HANDLE, seq);
    }
    EXPECT_EQ_INT(SAMPLES, callbacks[0]);
    EXPECT_EQ_INT(SAMPLES, callbacks[1]);
}

// --- the writer's side ---

static uint8_t sent[tt_MAX_BUFFER_LENGTH];
static uint8_t sent_type[64];
static uint32_t sent_count;

static void keep_type(const void* buf, size_t len) {
    const uint8_t* bytes = buf;
    if (sent_count < sizeof(sent_type) && len >= sizeof(struct tt_SingleHeader) &&
        (bytes[0] == tt_SINGLE_MARKER_LE || bytes[0] == tt_SINGLE_MARKER_BE)) {
        sent_type[sent_count] = bytes[3];
    } else if (sent_count < sizeof(sent_type)) {
        sent_type[sent_count] = 0; // classic form
    }
    sent_count++;
    memcpy(sent, buf, len < sizeof(sent) ? len : sizeof(sent));
}

static struct tt_Node writer;
static struct tt_Publisher pub;

static void init_writer(void) {
    test_mock_reset();
    test_mock_send_hook = keep_type;
    sent_count = 0;
    memset(&writer, 0, sizeof(writer));
    node_init_locks(&writer);
    writer.id = WRITER_A_SOURCE;
    writer.tx_tail = sizeof(struct tt_Header);
    writer.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    memset(&pub, 0, sizeof(pub)); // zeroed, not created: the counters must start on the long side
    pub.endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub.endpoint.id = ENDPOINT_ID;
    pub.endpoint.entity_id = WRITER_A_ENTITY;
    pub.endpoint.name = "w1_writer";
    pub.node = &writer;
    pub.topic = &topic;
    pub.handle = WRITER_A_HANDLE;
}

// BEST_EFFORT: tt_W1_LONG_EVERY long samples first (a zeroed Publisher included), then short, with every
// tt_W1_LONG_EVERY-th long; the short form carries the handle and the sample's own seq_no.
static void test_best_effort_writer_goes_short_after_its_long_run(void) {
    init_receiver(1);
    init_writer();
    uint32_t value = 0;
    const uint32_t total = (3 * tt_W1_LONG_EVERY) + 1;
    for (uint32_t i = 0; i < total; i++) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_EQ_U32(total, sent_count);
    uint32_t longs = 0;
    for (uint32_t i = 0; i < total; i++) {
        longs += sent_type[i] == tt_SUBMESSAGE_TYPE_DATA;
    }
    for (uint32_t i = 0; i < tt_W1_LONG_EVERY; i++) {
        EXPECT_EQ_U32(tt_SUBMESSAGE_TYPE_DATA, sent_type[i]);
    }
    EXPECT_EQ_U32(tt_SUBMESSAGE_TYPE_DATA_SHORT, sent_type[tt_W1_LONG_EVERY]);
    EXPECT_EQ_U32(tt_W1_LONG_EVERY + 2, longs); // the long run, then one in each of the next two runs of 16
    const struct tt_ShortDataHeader* header = (const struct tt_ShortDataHeader*)(sent + sizeof(struct tt_SingleHeader));
    EXPECT_EQ_U32(WRITER_A_HANDLE, header->handle);
    EXPECT_EQ_U32(total, header->seq_no);
    test_mock_send_hook = NULL;
}

// RELIABLE: long until every matched reader has ACKNACKed, then short.
static void test_reliable_writer_stays_long_until_every_reader_acknacked(void) {
    init_receiver(1);
    init_writer();
    pub.reliable = true;
    struct tt_PeerAck* first = claim_peer_ack(&pub, 3, 31);
    struct tt_PeerAck* second = claim_peer_ack(&pub, 4, 41);
    EXPECT_TRUE(first != NULL && second != NULL);
    uint32_t value = 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    record_peer_ack(&pub, 3, 31, 1);
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    record_peer_ack(&pub, 4, 41, 1);
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    EXPECT_EQ_U32(3, sent_count);
    EXPECT_EQ_U32(tt_SUBMESSAGE_TYPE_DATA, sent_type[0]);
    EXPECT_EQ_U32(tt_SUBMESSAGE_TYPE_DATA, sent_type[1]);
    EXPECT_EQ_U32(tt_SUBMESSAGE_TYPE_DATA_SHORT, sent_type[2]);
    test_mock_send_hook = NULL;
}

// A retransmission is addressed to the reader that asked, so it never takes the single form - and the short form
// is made only from a single-form datagram: whatever is armed, it goes long.
static void test_an_addressed_datagram_is_never_shortened(void) {
    init_writer();
    uint8_t datagram[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader) +
                     SAMPLE_BYTES];
    struct tt_SubmessageHeader* submessage = (struct tt_SubmessageHeader*)(datagram + sizeof(struct tt_Header));
    submessage->type = tt_SUBMESSAGE_TYPE_DATA;
    submessage->receiver = 3; // addressed, as a retransmission is
    submessage->length = (uint16_t)(sizeof(datagram) - sizeof(struct tt_Header));
    struct tt_DataHeader* data = (struct tt_DataHeader*)(submessage + 1);
    data->endpoint_id = ENDPOINT_ID;
    data->entity_id = WRITER_A_ENTITY;
    writer.tx_short_armed = true;
    writer.tx_short_entity = WRITER_A_ENTITY;
    uint32_t skip = to_single_form(datagram, (uint32_t)sizeof(datagram), 0);
    EXPECT_EQ_U32(0, skip);
    EXPECT_EQ_U32(0, to_short_form(&writer, datagram, skip, (uint32_t)sizeof(datagram)));
    EXPECT_EQ_U32(tt_SUBMESSAGE_TYPE_DATA, submessage->type);
}

int main(void) {
    test_known_writer_routes_through_directory_then_table();
    test_a_stale_generation_never_routes();
    test_an_unknown_writer_in_a_known_slot_is_dropped_not_misrouted();
    test_two_writers_in_one_slot_both_deliver_every_sample();
    test_two_local_subscribers_both_receive_every_sample();
    test_best_effort_writer_goes_short_after_its_long_run();
    test_reliable_writer_stays_long_until_every_reader_acknacked();
    test_an_addressed_datagram_is_never_shortened();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_w1_route: all tests passed\n");
    return 0;
}
