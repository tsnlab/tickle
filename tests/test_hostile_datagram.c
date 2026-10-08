/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g11 (rmw_tickle/RMW_GAPS_PLAN.md): a received datagram must never make tt_Context_poll() return
// an error. Whatever a peer sends - a version we do not speak, a bad magic, a truncated header, a
// submessage that does not walk - is a dropped datagram with a counter, not a fatal return.
//
// The defect this locks down was observed, not imagined: on the rig a v11 server received one v10
// datagram from a leftover process and exited in the same second, twenty seconds into a thirty
// second run, taking its peer's measurement with it. Every poll loop TickLE ships ends on anything
// but OK or TIMEOUT, so one datagram from anyone who can reach the port ends the node.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> // free
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: process_datagram() and the counters are internal, and this is exactly the untrusted
// input boundary (broadcast UDP, no authentication) the rule is about.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define HOSTILE_SOURCE 2
#define FRIENDLY_SOURCE 3
#define SENDER_IP 0x0a000002
#define SENDER_PORT 7000

static void setup(struct tt_Context* node) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    test_mock_reset();
    node->id = LOCAL_NODE_ID;
}

// A datagram carrying one DATA submessage for an endpoint nobody here subscribes to: valid in
// every way this file is about, so what it proves is that processing happened at all.
static int32_t write_valid(uint8_t* buffer, uint8_t source) {
    struct tt_Header* header = (struct tt_Header*)buffer;
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = source;
    struct tt_SubmessageHeader* submessage = (struct tt_SubmessageHeader*)(buffer + sizeof(*header));
    submessage->type = tt_SUBMESSAGE_TYPE_DATA;
    submessage->receiver = tt_SUBMESSAGE_ID_ALL;
    submessage->length = (uint16_t)(sizeof(*submessage) + sizeof(struct tt_DataHeader));
    struct tt_DataHeader* data = (struct tt_DataHeader*)(buffer + sizeof(*header) + sizeof(*submessage));
    memset(data, 0, sizeof(*data));
    data->endpoint_id = 0x1234;
    return (int32_t)(sizeof(*header) + sizeof(*submessage) + sizeof(*data));
}

// A datagram carrying the shared-memory-only record type (SHM_PLAN 6e). It only ever exists inside a
// segment, and section 1's safety claim - that a segment record takes the same acceptance path as a
// datagram - is what makes arriving on the socket an injection rather than a protocol variant.
static int32_t write_shm_only_record(uint8_t* buffer) {
    int32_t len = write_valid(buffer, HOSTILE_SOURCE);
    ((struct tt_SubmessageHeader*)(buffer + sizeof(struct tt_Header)))->type = tt_SUBMESSAGE_TYPE_SHM_DATA;
    return len;
}

static int32_t write_wrong_version(uint8_t* buffer) {
    int32_t len = write_valid(buffer, HOSTILE_SOURCE);
    ((struct tt_Header*)buffer)->version = (uint8_t)(tt_VERSION - 1);
    return len;
}

static int32_t write_bad_magic(uint8_t* buffer) {
    int32_t len = write_valid(buffer, HOSTILE_SOURCE);
    ((struct tt_Header*)buffer)->magic_value = (uint16_t)(NATIVE_MAGIC_VALUE ^ 0x0101U);
    return len;
}

// Shorter than a header: the decode cursor runs off the end before anything is read.
static int32_t write_truncated(uint8_t* buffer) {
    (void)write_valid(buffer, HOSTILE_SOURCE);
    return (int32_t)(sizeof(struct tt_Header) - 1);
}

// A submessage whose length claims far more than the datagram holds.
static int32_t write_overlong_submessage(uint8_t* buffer) {
    int32_t len = write_valid(buffer, HOSTILE_SOURCE);
    struct tt_SubmessageHeader* submessage = (struct tt_SubmessageHeader*)(buffer + sizeof(struct tt_Header));
    submessage->length = (uint16_t)(tt_MAX_BUFFER_LENGTH - 1);
    return len;
}

struct hostile_case {
    const char* name;
    int32_t (*write)(uint8_t* buffer);
    bool counts_version_mismatch; // otherwise it is only a malformed drop
    // Whether process_packet() got as far as stamping the sender's liveliness. It stamps once the
    // header validates, before the submessages are walked, so a datagram whose header is good and
    // whose body is not does extend the sender's lease. That is deliberate and older than g11 -
    // the header proves a peer speaking this wire version is transmitting - and is recorded here
    // rather than changed, so that a later change to it is a deliberate one.
    bool stamps_liveliness;
};

static const struct hostile_case hostile_cases[] = {
    {"wrong wire version", write_wrong_version, true, false},
    {"bad magic", write_bad_magic, false, false},
    {"truncated header", write_truncated, false, false},
    {"overlong submessage", write_overlong_submessage, false, true},
};
#define HOSTILE_CASE_COUNT (sizeof(hostile_cases) / sizeof(hostile_cases[0]))

// Pass criterion 1: the unit contract. Each hostile datagram is processed as OK, the right counter
// moves by one, and no delivery-side state is touched.
static void test_a_hostile_datagram_is_a_drop_not_an_error(void) {
    for (size_t i = 0; i < HOSTILE_CASE_COUNT; i++) {
        struct tt_Context node;
        setup(&node);
        int32_t len = hostile_cases[i].write(node.rx_buffer);

        tt_ret_t result = process_datagram(&node, len, SENDER_IP, SENDER_PORT, tt_TRANSPORT_UDP);

        printf("  case: %s\n", hostile_cases[i].name);
        EXPECT_EQ_INT(tt_RET_OK, result);
        EXPECT_EQ_U64(1, node.rx_malformed_drops);
        EXPECT_EQ_U64(hostile_cases[i].counts_version_mismatch ? 1 : 0, node.version_mismatch_drops);
        EXPECT_EQ_U64(hostile_cases[i].stamps_liveliness ? test_mock_now : 0, node.traffic_last_seen[HOSTILE_SOURCE]);
    }
}

// SHM_PLAN 6e names this as the one safety condition that must not be skipped: a record that declares
// how many seq_nos it covers may only come from something with write access to a segment. Arriving on
// the socket it must be REFUSED, or a multi-slot record can be injected from the network.
//
// The same bytes down both paths, because a refusal that also fires for the legitimate arrival would
// be indistinguishable from this one working.
static void test_a_shm_only_record_is_refused_on_the_socket(void) {
    struct tt_Context node;

    setup(&node);
    int32_t len = write_shm_only_record(node.rx_buffer);
    tt_ret_t result = process_datagram(&node, len, SENDER_IP, SENDER_PORT, tt_TRANSPORT_UDP);
    printf("  case: shared-memory-only record over the socket\n");
    EXPECT_EQ_INT(tt_RET_OK, result); // dropped and counted, never an error - a peer must not end a poll loop
    EXPECT_EQ_U64(1, node.rx_shm_only_on_socket);
    EXPECT_EQ_U64(1, node.rx_malformed_drops);
    EXPECT_EQ_U64(0, node.version_mismatch_drops); // not a rolling upgrade, and must not read as one

    // The control. Identical bytes arriving the way this type is meant to: not refused, not counted.
    // Without this the test passes just as well against a build that refuses the type everywhere.
    setup(&node);
    len = write_shm_only_record(node.rx_buffer);
    result = process_datagram(&node, len, SENDER_IP, SENDER_PORT, tt_TRANSPORT_SHM);
    printf("  case: the same record arriving from a segment\n");
    EXPECT_EQ_INT(tt_RET_OK, result);
    EXPECT_EQ_U64(0, node.rx_shm_only_on_socket);
    EXPECT_EQ_U64(0, node.rx_malformed_drops);
}

// Pass criterion 2: the property the rig lost. The first datagram of a poll being hostile must not
// stop the drain - before this, drain_rx() returned on it and the backlog was never read.
static void test_a_hostile_first_datagram_does_not_stop_the_drain(void) {
    for (size_t i = 0; i < HOSTILE_CASE_COUNT; i++) {
        struct tt_Context node;
        setup(&node);
        test_mock_receive_return = hostile_cases[i].write(node.rx_buffer);
        test_mock_try_receive_remaining = 4; // a backlog behind it, of the same bytes
        test_mock_try_receive_len = test_mock_receive_return;

        tt_ret_t result = tt_Context_poll(&node, -1);

        printf("  case: %s\n", hostile_cases[i].name);
        EXPECT_EQ_INT(tt_RET_OK, result);
        EXPECT_EQ_INT(0, test_mock_try_receive_remaining); // every one of them was read
        EXPECT_EQ_U64(5, node.rx_datagrams);
        EXPECT_EQ_U64(5, node.rx_malformed_drops);
    }
}

// Pass criterion 3: a valid datagram after a hostile one is processed in full. The stamp is the
// evidence - process_packet() sets it only past validation, so the hostile source has none and the
// friendly one does.
static void test_delivery_continues_after_a_hostile_datagram(void) {
    for (size_t i = 0; i < HOSTILE_CASE_COUNT; i++) {
        struct tt_Context node;
        setup(&node);
        test_mock_now = tt_SECOND;

        int32_t hostile_len = hostile_cases[i].write(node.rx_buffer);
        EXPECT_EQ_INT(tt_RET_OK, process_datagram(&node, hostile_len, SENDER_IP, SENDER_PORT, tt_TRANSPORT_UDP));

        int32_t valid_len = write_valid(node.rx_buffer, FRIENDLY_SOURCE);
        EXPECT_EQ_INT(tt_RET_OK, process_datagram(&node, valid_len, SENDER_IP, SENDER_PORT, tt_TRANSPORT_UDP));

        printf("  case: %s\n", hostile_cases[i].name);
        EXPECT_EQ_U64(tt_SECOND, node.traffic_last_seen[FRIENDLY_SOURCE]);
        EXPECT_EQ_U64(hostile_cases[i].stamps_liveliness ? tt_SECOND : 0, node.traffic_last_seen[HOSTILE_SOURCE]);
        EXPECT_EQ_U64(1, node.rx_malformed_drops); // and the valid one is not counted as a drop
    }
}

// The log stays rate-limited per source, which is its own denial of service otherwise - the fix
// must not turn a per-packet drop into a per-packet log line.
static void test_a_flood_of_wrong_version_logs_once(void) {
    struct tt_Context node;
    setup(&node);
    int32_t len = write_wrong_version(node.rx_buffer);

    for (int i = 0; i < 100; i++) {
        EXPECT_EQ_INT(tt_RET_OK, process_datagram(&node, len, SENDER_IP, SENDER_PORT, tt_TRANSPORT_UDP));
    }

    EXPECT_EQ_U64(100, node.version_mismatch_drops);
    EXPECT_EQ_INT((int)(tt_VERSION - 1), (int)node.version_mismatch_logged[HOSTILE_SOURCE]);
}

// --- Segment-only signals arriving from the network (ROADMAP, SHM_PLAN 6e(a), 2026-10-09) ---------------------------
//
// The test above feeds process_datagram() a DATA header for an endpoint nobody subscribes to, so it shows the record is
// counted and refused, not that nothing reaches a Subscriber. These go in through the socket the way a peer's datagram
// does (tt_Context_poll() over the mock HAL), with a Subscriber of the topic present, and their control is the same
// datagram as plain DATA, which that Subscriber is shown to receive.

#define REMOTE_ID 4
#define REMOTE_VALUE 0x5eed1234U

static int32_t value_size(struct tt_Data* data) {
    (void)data;
    return 4;
}
static int32_t value_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < 4) {
        return -1;
    }
    memcpy(payload, data, 4);
    return 4;
}
static int32_t value_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)native;
    if (len < 4) {
        return -1;
    }
    memcpy(data, payload, 4);
    return 4;
}
static void value_free(struct tt_Data* data) {
    (void)data;
}

static int delivered;
static uint32_t delivered_value;
static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    memcpy(&delivered_value, data, sizeof(delivered_value));
    delivered++;
}

// The first datagram the remote context sends that carries DATA, as sent (the single-submessage form a peer puts on
// the wire) and in the classic form (tt_Header, then the submessage header), which the receive path also reads.
static uint8_t remote_wire[tt_MAX_BUFFER_LENGTH];
static size_t remote_wire_len;
static uint8_t remote_classic[((size_t)tt_MAX_BUFFER_LENGTH * 2) + sizeof(struct tt_Header)];
static size_t remote_classic_len;
static void capture_data(const void* buf, size_t len) {
    if (remote_wire_len != 0 || len == 0 || len > sizeof(remote_wire)) {
        return;
    }
    size_t classic_len = test_classic_form(buf, len, remote_classic);
    const struct tt_SubmessageHeader* submessage =
        (const struct tt_SubmessageHeader*)(remote_classic + sizeof(struct tt_Header));
    if (classic_len > sizeof(struct tt_Header) + sizeof(*submessage) && submessage->type == tt_SUBMESSAGE_TYPE_DATA) {
        memcpy(remote_wire, buf, len);
        remote_wire_len = len;
        remote_classic_len = classic_len;
    }
}

static struct tt_Context remote;
static struct tt_Context local;
static struct tt_Topic topic;
static struct tt_Publisher remote_pub;
static struct tt_Subscriber local_sub;

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    context->entity_id_base = 0x5A000000U | ((uint32_t)id << 16); // as tt_Context_create() draws one: never 0
    context->rx_seq_span = 1;
}

// A real DATA datagram from a remote publisher of the topic, and a local context with a Subscriber of it.
static void setup_pair(void) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    init_context(&remote, REMOTE_ID);
    init_context(&local, LOCAL_NODE_ID);
    memset(&topic, 0, sizeof(topic));
    topic.name = "hostile_topic";
    topic.data_size = 4;
    topic.data_encode_size = value_size;
    topic.data_encode = value_encode;
    topic.data_decode = value_decode;
    topic.data_free = value_free;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&remote, &remote_pub, &topic, "pub"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&local, &local_sub, &topic, "pub", on_sample));

    remote_wire_len = 0;
    test_mock_send_hook = capture_data;
    uint32_t value = REMOTE_VALUE;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&remote_pub, (struct tt_Data*)&value));
    node_flush(&remote, 0, NULL);
    test_mock_send_hook = NULL;
    EXPECT_TRUE(remote_wire_len != 0);
    delivered = 0;
    delivered_value = 0;
}

// One datagram of `len` bytes (already in the landing buffer) read off the local context's socket by a poll.
static void receive_on_socket(int32_t len) {
    test_mock_receive_return = len;
    test_mock_receive_limit = 1; // one datagram, then the mock reports an interrupt and the poll returns
    test_mock_receive_call_count = 0;
    (void)tt_Context_poll(&local, -1);
    test_mock_receive_return = -1;
    test_mock_receive_limit = 0;
}

// The captured DATA in the landing buffer with its submessage type set to `type`, in either form.
static int32_t land_remote_data(uint8_t type, bool single) {
    uint8_t* landing = RX_LANDING(&local);
    if (single) {
        memcpy(landing, remote_wire, remote_wire_len);
        ((struct tt_SingleHeader*)landing)->type = type;
        return (int32_t)remote_wire_len;
    }
    memcpy(landing, remote_classic, remote_classic_len);
    ((struct tt_SubmessageHeader*)(landing + sizeof(struct tt_Header)))->type = type;
    return (int32_t)remote_classic_len;
}

// The same publisher's DATA, once as DATA and once relabelled as the shared-memory-only type, in both wire forms: the
// first is delivered, the second refused, counted, and nothing of it reaches the Subscriber or its reliability state.
static void test_a_shm_only_record_from_the_network_delivers_nothing(void) {
    for (int single = 1; single >= 0; single--) {
        printf("  case: plain DATA over the socket, %s form (control)\n", single ? "single" : "classic");
        setup_pair();
        EXPECT_EQ_INT(0, delivered);
        receive_on_socket(land_remote_data(tt_SUBMESSAGE_TYPE_DATA, single != 0));
        EXPECT_EQ_INT(1, delivered);
        EXPECT_EQ_U32(REMOTE_VALUE, delivered_value);
        EXPECT_EQ_U64(0, local.rx_shm_only_on_socket);
        EXPECT_EQ_U64(0, local.rx_malformed_drops);

        printf("  case: the same DATA relabelled SHM_DATA over the socket, %s form\n", single ? "single" : "classic");
        setup_pair();
        receive_on_socket(land_remote_data(tt_SUBMESSAGE_TYPE_SHM_DATA, single != 0));
        EXPECT_EQ_INT(0, delivered);
        EXPECT_EQ_U64(1, local.rx_shm_only_on_socket);
        EXPECT_EQ_U64(1, local.rx_malformed_drops);
        EXPECT_EQ_U64(0, local.version_mismatch_drops);
        EXPECT_TRUE(find_writer_proxy(&local_sub, REMOTE_ID, remote_pub.endpoint.entity_id) == NULL);
        EXPECT_EQ_U32(1, local.rx_seq_span); // a socket datagram never carries a span: only a segment slot says one
    }
}

// A flood of them is one log line per power of ten, not one per datagram: the refusal is of untrusted input, and a line
// per datagram is its own denial of service. The control is the counter: every one of them was refused.
static void test_a_flood_of_shm_only_records_logs_by_powers_of_ten(void) {
    setup_pair();
    char* text = NULL;
    size_t text_len = 0;
    FILE* log = open_memstream(&text, &text_len);
    EXPECT_TRUE(log != NULL);
    if (log == NULL) {
        return;
    }
    tt_log_set_output(log);
    enum { FLOOD = 100 };
    for (int i = 0; i < FLOOD; i++) {
        int32_t len = land_remote_data(tt_SUBMESSAGE_TYPE_SHM_DATA, true);
        EXPECT_EQ_INT(tt_RET_OK, process_datagram(&local, len, SENDER_IP, SENDER_PORT, tt_TRANSPORT_UDP));
    }
    tt_log_set_output(NULL);
    (void)fclose(log);
    int lines = 0;
    for (const char* at = text; at != NULL && (at = strstr(at, "Shared-memory-only submessage")) != NULL; at++) {
        lines++;
    }
    free(text);
    EXPECT_EQ_U64(FLOOD, local.rx_shm_only_on_socket);
    EXPECT_EQ_INT(0, delivered);
    printf("  %d refusals logged for %d datagrams\n", lines, FLOOD);
    EXPECT_EQ_INT(3, lines); // the 1st, 10th and 100th
}

#if tt_SEGMENT_ENABLED
// A zero-length datagram is the segment's UDP doorbell, and anyone who can reach the port can send one. What it can do
// is wake the reader to drain its own ring - which only a writer on this host with access to the segment can fill -
// and nothing else: it is not parsed, not delivered, not counted as malformed; and it can only make the next sleep
// a new generation (every writer rings again), never let a reader keep one it would otherwise give up, so a forged
// doorbell can cost a wake-up but never cost a ring.
static void test_a_zero_length_datagram_from_the_network_is_only_a_wake(void) {
    setup_pair();
    create_own_segment(&local);
    EXPECT_TRUE(local.own_segment != NULL);
    memset(RX_LANDING(&local), 0, sizeof(struct tt_Header));
    receive_on_socket(0);
    EXPECT_EQ_INT(0, delivered);
    EXPECT_EQ_U64(1, local.segment_doorbells_received);
    EXPECT_EQ_U64(0, local.rx_malformed_drops);
    EXPECT_EQ_U32(0, local.own_segment->read_index); // nothing in the ring, nothing read from it

    // Control: a sleep called off and announced again keeps its generation when no doorbell came between.
    segment_sleep_called_off(&local);
    uint32_t generation = local.segment_sleep_generation;
    segment_reader_waiting(&local, true);
    EXPECT_EQ_U32(generation, local.segment_sleep_generation);
    EXPECT_EQ_U64(1, local.segment_generations_kept);
    segment_reader_waiting(&local, false);

    // A forged doorbell in between: a new generation, which every writer rings - the safe direction.
    segment_sleep_called_off(&local);
    generation = local.segment_sleep_generation;
    EXPECT_EQ_INT(tt_RET_OK, process_datagram(&local, 0, SENDER_IP, SENDER_PORT, tt_TRANSPORT_UDP));
    segment_reader_waiting(&local, true);
    EXPECT_TRUE(local.segment_sleep_generation != generation);
    EXPECT_EQ_U64(1, local.segment_generations_kept);
    segment_reader_waiting(&local, false);
    EXPECT_EQ_INT(0, delivered);

    release_own_segment(&local);
    test_mock_segments_free();
}
#endif

int main(void) {
    test_a_hostile_datagram_is_a_drop_not_an_error();
    test_a_shm_only_record_is_refused_on_the_socket();
    test_a_shm_only_record_from_the_network_delivers_nothing();
    test_a_flood_of_shm_only_records_logs_by_powers_of_ten();
#if tt_SEGMENT_ENABLED
    test_a_zero_length_datagram_from_the_network_is_only_a_wake();
#endif
    test_a_hostile_first_datagram_does_not_stop_the_drain();
    test_delivery_continues_after_a_hostile_datagram();
    test_a_flood_of_wrong_version_logs_once();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_hostile_datagram: all tests passed\n");
    return 0;
}
