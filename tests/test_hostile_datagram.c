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

int main(void) {
    test_a_hostile_datagram_is_a_drop_not_an_error();
    test_a_shm_only_record_is_refused_on_the_socket();
    test_a_hostile_first_datagram_does_not_stop_the_drain();
    test_delivery_continues_after_a_hostile_datagram();
    test_a_flood_of_wrong_version_logs_once();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_hostile_datagram: all tests passed\n");
    return 0;
}
