/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Hybrid transport, step 1 (DESIGN.md section 8, "Hybrid transport for multi-datagram samples"): the reassembly
// capability a context advertises reaches its peers through discovery and is recorded per peer, and nothing else
// changes. Built as rmw_tickle builds core (a 65507-byte datagram, large samples), the Linux side of the design; a
// context set up with FreeRTOS's setting (0) stands for the FreeRTOS peer in the same process. test_hybrid_mtu.c is the
// core-default build.
//
// Every announce here is the real one: build_and_send_update() on one context, its datagrams captured from the mock
// HAL and handed to another context's process_packet().

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1 // as rmw_tickle builds core

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: process_packet() is static
#include "test_mock.h"

#define LINUX_ID 1
#define RECEIVER_ID 2
#define FREERTOS_ID 3
#define PEER_IP(id) (0x0a000000U | (uint32_t)(id))
#define PORT 8282

// --- capture: every datagram a context hands the mock HAL, whole -------------------------------------------------

#define CAPTURE_MAX 64
#define CAPTURE_BYTES 1600
static uint8_t captured[CAPTURE_MAX][CAPTURE_BYTES];
static uint32_t captured_len[CAPTURE_MAX];
static uint32_t captured_count;

static void capture_hook(const void* buf, size_t len) {
    if (captured_count == CAPTURE_MAX || len > CAPTURE_BYTES) {
        test_failures++;
        return;
    }
    memcpy(captured[captured_count], buf, len);
    captured_len[captured_count++] = (uint32_t)len;
}

// --- contexts ------------------------------------------------------------------------------------------------------

static struct tt_Context linux_node;
static struct tt_Context freertos_node;
static struct tt_Context receiver;

// The next init_node()'s opts.net.large_fragmentation: 0, the default (AUTO), unless a test sets it.
static uint8_t init_large_fragmentation;

// A context as tt_Context_create_with() leaves its state - reset_node_state() reads node->opts - without a socket.
// The reader's cap is switched off (as a forced arm's reader) so that what a node advertises is its configured ceiling,
// which is what these tests carry from end to end; test_the_capped_reader_advertises_its_cap is the default.
// ip_reassembly_max 0 is none (tt_IP_REASSEMBLY_NONE), as FreeRTOS advertises.
static void init_node(struct tt_Context* node, uint8_t id, uint16_t ip_reassembly_max) {
    memset(node, 0, sizeof(*node));
    node->opts.net.ip_reassembly_max = ip_reassembly_max != 0 ? ip_reassembly_max : tt_IP_REASSEMBLY_NONE;
    node->opts.net.reassembly_cap_off = 1;
    node->opts.net.large_fragmentation = init_large_fragmentation;
    node_init_locks(node);
    reset_node_state(node);
    node->opts.net.reassembly_cap_off = 0;
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
    node->last_modified = 1000U + id; // a generation of its own
}

// Sends `from`'s announce to the receiver, unicast and so at once, and returns how many datagrams it took.
static uint32_t announce(struct tt_Context* from) {
    captured_count = 0;
    test_mock_send_hook = capture_hook;
    struct tt_Peer peer = {.context_id = RECEIVER_ID, .ip = PEER_IP(RECEIVER_ID), .port = PORT};
    EXPECT_TRUE(build_and_send_update(from, &peer, 1));
    test_mock_send_hook = NULL;
    return captured_count;
}

static bool deliver(uint32_t i, uint8_t from_id) {
    return process_packet(&receiver, captured[i], 0, captured_len[i], PEER_IP(from_id), PORT, tt_TRANSPORT_UDP);
}

// Where the tt_AnnounceHeader of captured datagram i starts, or 0 when it is not an announce's first datagram.
static uint32_t announce_header_at(uint32_t i) {
    const uint8_t* bytes = captured[i];
    bool single = bytes[0] == tt_SINGLE_MARKER_LE || bytes[0] == tt_SINGLE_MARKER_BE;
    uint32_t body = single ? (uint32_t)sizeof(struct tt_SingleHeader)
                           : (uint32_t)(sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    uint8_t type = single ? bytes[3] : bytes[sizeof(struct tt_Header)];
    if (type == tt_SUBMESSAGE_TYPE_DATA) {
        return body + (uint32_t)sizeof(struct tt_DataHeader);
    }
    if (type == tt_SUBMESSAGE_TYPE_FRAG_FIRST) {
        return body + (uint32_t)sizeof(struct tt_FragFirstHeader);
    }
    if (type == tt_SUBMESSAGE_TYPE_FRAG_CONT) {
        return body + (uint32_t)sizeof(struct tt_FragContHeader);
    }
    return 0;
}

// --- tests -----------------------------------------------------------------------------------------------------------

static struct tt_TypeSupport topics[48];
static struct tt_Publisher pubs[48];
static char topic_names[48][96];

static int32_t encode_size_8(struct tt_Data* data) {
    (void)data;
    return 8;
}

static int32_t encode_zero(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0, len);
    return (int32_t)len;
}

static void add_publishers(struct tt_Context* node, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        memset(&topics[i], 0, sizeof(topics[i]));
        (void)snprintf(topic_names[i], sizeof(topic_names[i]),
                       "/a/rather/long/topic/name/so/that/the/announce/needs/fragments/%02u", (unsigned)i);
        topics[i].name = topic_names[i];
        topics[i].data_size = 8;
        topics[i].data_encode_size = encode_size_8;
        topics[i].data_encode = encode_zero;
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(node), &pubs[i], &topics[i], "pub",
                                                          &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    }
}

// The settings, as tt_Context_create() reads them: Linux's default, the cap by the receive buffer, FreeRTOS's 0.
static void test_settings_and_defaults(void) {
    test_mock_reset();
    struct tt_InitOptions defaults;
    tt_InitOptions_default(&defaults);
    EXPECT_EQ_U32(tt_IPV4_UDP_MAX_PAYLOAD, defaults.net.ip_reassembly_max); // the Linux default (_tt_IP_REASSEMBLY_MAX)
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_AUTO, defaults.net.large_fragmentation);
    init_node(&linux_node, LINUX_ID, defaults.net.ip_reassembly_max);
    EXPECT_EQ_U32(65507, linux_node.ip_reassembly_max);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_AUTO, linux_node.large_fragmentation);
    init_node(&freertos_node, FREERTOS_ID, 0);
    EXPECT_EQ_U32(0, freertos_node.ip_reassembly_max);
    // Capped by the receive buffer: never more than a datagram this build can receive whole.
    EXPECT_EQ_U32(tt_MAX_BUFFER_LENGTH, advertised_ip_reassembly_max(100000U));
    EXPECT_EQ_U32(9000, advertised_ip_reassembly_max(9000U));
    // A context's 0 (INHERIT) reads as AUTO; MTU and IP are kept.
    init_large_fragmentation = tt_LARGE_FRAGMENTATION_INHERIT;
    init_node(&receiver, RECEIVER_ID, 65507);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_AUTO, receiver.large_fragmentation);
    init_large_fragmentation = tt_LARGE_FRAGMENTATION_MTU;
    init_node(&receiver, RECEIVER_ID, 65507);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_MTU, receiver.large_fragmentation);
    init_large_fragmentation = 0;
    // A Publisher inherits both by default.
    add_publishers(&linux_node, 1);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_INHERIT, pubs[0].large_fragmentation);
    EXPECT_EQ_U32(0, pubs[0].ip_datagram_max);
    // The wire layout the design gives: three uint16_t after the entity count, and the two run headers.
    EXPECT_EQ_U32(7, sizeof(struct tt_AnnounceHeader));
    EXPECT_EQ_U32(24, sizeof(struct tt_FragRunLHeader));
    EXPECT_EQ_U32(20, sizeof(struct tt_FragRunHeader));
    // 13 brought these; 14 (QoS plan D9) changed only the announce's entity record (struct tt_UpdateEntity).
    EXPECT_EQ_U32(14, tt_VERSION);
    // 14 and 15: 13 is large-message step B's LARGE_REF (DESIGN.md section 8).
    EXPECT_EQ_U32(14, tt_SUBMESSAGE_TYPE_FRAG_RUN_L);
    EXPECT_EQ_U32(15, tt_SUBMESSAGE_TYPE_FRAG_RUN);
}

// Linux to Linux, and a FreeRTOS-configured peer beside it: each peer's own figure, recorded per peer.
static void test_capability_round_trip_mixed_peers(void) {
    test_mock_reset();
    init_node(&linux_node, LINUX_ID, 65507);
    init_node(&freertos_node, FREERTOS_ID, 0);
    init_node(&receiver, RECEIVER_ID, 65507);
    add_publishers(&linux_node, 1);
    EXPECT_EQ_U32(0, receiver.peer_ip_reassembly_max[LINUX_ID]); // not heard yet: n = 1

    EXPECT_EQ_U32(1, announce(&linux_node));
    uint32_t at = announce_header_at(0);
    EXPECT_TRUE(at != 0);
    struct tt_AnnounceHeader wire;
    memcpy(&wire, captured[0] + at, sizeof(wire));
    EXPECT_EQ_U32(65507, wire.ip_reassembly_max);
    EXPECT_EQ_U32(0, wire.loss_p); // no estimate yet
    EXPECT_EQ_U32(0, wire.reserved);
    EXPECT_TRUE(deliver(0, LINUX_ID));
    EXPECT_EQ_U32(65507, receiver.peer_ip_reassembly_max[LINUX_ID]);
    EXPECT_TRUE(receiver.update_seen[LINUX_ID]); // the announce was applied, not only read

    EXPECT_EQ_U32(1, announce(&freertos_node));
    EXPECT_TRUE(deliver(0, FREERTOS_ID));
    EXPECT_EQ_U32(0, receiver.peer_ip_reassembly_max[FREERTOS_ID]);
    EXPECT_TRUE(receiver.update_seen[FREERTOS_ID]);
    EXPECT_EQ_U32(65507, receiver.peer_ip_reassembly_max[LINUX_ID]); // one peer's record is not another's

    // The other direction: the FreeRTOS-configured context records the Linux one's figure (it is a Linux build here,
    // so it keeps the record; a real FreeRTOS build without fragmentation keeps none - test_hybrid_mtu.c).
    captured_count = 0;
    test_mock_send_hook = capture_hook;
    struct tt_Peer peer = {.context_id = FREERTOS_ID, .ip = PEER_IP(FREERTOS_ID), .port = PORT};
    EXPECT_TRUE(build_and_send_update(&linux_node, &peer, 1));
    test_mock_send_hook = NULL;
    EXPECT_TRUE(
        process_packet(&freertos_node, captured[0], 0, captured_len[0], PEER_IP(LINUX_ID), PORT, tt_TRANSPORT_UDP));
    EXPECT_EQ_U32(65507, freertos_node.peer_ip_reassembly_max[LINUX_ID]);

    // A node presumed dead is not trusted to reassemble when it comes back until it says so again.
    presume_node_dead(&receiver, LINUX_ID, tt_SECOND);
    EXPECT_EQ_U32(0, receiver.peer_ip_reassembly_max[LINUX_ID]);

    // The frame loss a node measures as a reader rides the same announce, is recorded from it, and is forgotten with
    // the rest when the node is presumed dead.
    linux_node.hybrid.advertised_loss = hybrid_loss_code(0.001);
    EXPECT_EQ_U32(1, announce(&linux_node));
    memcpy(&wire, captured[0] + announce_header_at(0), sizeof(wire));
    EXPECT_EQ_U32(hybrid_loss_code(0.001), wire.loss_p);
    EXPECT_TRUE(deliver(0, LINUX_ID));
    EXPECT_EQ_U32(hybrid_loss_code(0.001), receiver.hybrid.peer_loss_p[LINUX_ID]);
    presume_node_dead(&receiver, LINUX_ID, tt_SECOND);
    EXPECT_EQ_U32(0, receiver.hybrid.peer_loss_p[LINUX_ID]);
}

// An announce in fragments: every fragment carries the figure, and the first one alone records it.
static void test_fragmented_announce_carries_it(void) {
    test_mock_reset();
    init_node(&linux_node, LINUX_ID, 9000);
    init_node(&receiver, RECEIVER_ID, 65507);
    add_publishers(&linux_node, 40);
    uint32_t datagrams = announce(&linux_node);
    EXPECT_TRUE(datagrams >= 2);
    for (uint32_t i = 0; i < datagrams; i++) {
        uint32_t at = announce_header_at(i);
        EXPECT_TRUE(at != 0);
        struct tt_AnnounceHeader wire;
        memcpy(&wire, captured[i] + at, sizeof(wire));
        EXPECT_EQ_U32(9000, wire.ip_reassembly_max);
    }
    EXPECT_TRUE(deliver(datagrams - 1, LINUX_ID)); // the last fragment first: any fragment records it
    EXPECT_EQ_U32(9000, receiver.peer_ip_reassembly_max[LINUX_ID]);
    for (uint32_t i = 0; i + 1 < datagrams; i++) {
        EXPECT_TRUE(deliver(i, LINUX_ID));
    }
    EXPECT_TRUE(receiver.update_seen[LINUX_ID]);
    EXPECT_EQ_U32(9000, receiver.peer_ip_reassembly_max[LINUX_ID]);
}

// A reverse-endian peer's figure is swapped like every other field core reads. Hand-built: an announce of no
// entities from a big-endian sender (on this little-endian host), whose ids are byte-order invariant.
static void test_reverse_endian_announce(void) {
    test_mock_reset();
    init_node(&receiver, RECEIVER_ID, 65507);
    uint8_t bytes[64];
    memset(bytes, 0, sizeof(bytes));
    bool host_little = tt_is_native_endian(&(struct tt_Header) {.magic = {'K', 'T'}});
    struct tt_SingleHeader single = {host_little ? (uint8_t)tt_SINGLE_MARKER_BE : (uint8_t)tt_SINGLE_MARKER_LE,
                                     tt_VERSION, 5, tt_SUBMESSAGE_TYPE_DATA};
    memcpy(bytes, &single, sizeof(single));
    struct tt_DataHeader data = {.endpoint_id = tt_DISCOVERY_ENDPOINT_ID,
                                 .seq_no = __builtin_bswap32(77U),
                                 .timestamp = 0,
                                 .entity_id = tt_DISCOVERY_ENTITY_ID};
    memcpy(bytes + sizeof(single), &data, sizeof(data));
    struct tt_AnnounceHeader announce_header = {.entity_count = 0,
                                                .ip_reassembly_max = __builtin_bswap16(4000U),
                                                .loss_p = 0,
                                                .reserved = 0};
    memcpy(bytes + sizeof(single) + sizeof(data), &announce_header, sizeof(announce_header));
    uint32_t len = (uint32_t)(sizeof(single) + sizeof(data) + sizeof(announce_header));
    EXPECT_TRUE(process_packet(&receiver, bytes, 0, len, PEER_IP(5), PORT, tt_TRANSPORT_UDP));
    EXPECT_EQ_U32(4000, receiver.peer_ip_reassembly_max[5]);
}

// A peer of the previous protocol version is dropped and counted, as any version mismatch is (exact match since
// Phase 2): nothing it says is read, the capability included.
static void test_version_mismatch(void) {
    test_mock_reset();
    init_node(&linux_node, LINUX_ID, 65507);
    init_node(&receiver, RECEIVER_ID, 65507);
    add_publishers(&linux_node, 1);
    EXPECT_EQ_U32(1, announce(&linux_node));
    bool single = captured[0][0] == tt_SINGLE_MARKER_LE || captured[0][0] == tt_SINGLE_MARKER_BE;
    captured[0][single ? 1 : 2] = tt_VERSION - 1; // a tt_VERSION 12 sender
    EXPECT_TRUE(!deliver(0, LINUX_ID));
    EXPECT_EQ_U64(1, receiver.version_mismatch_drops);
    EXPECT_EQ_U32(0, receiver.peer_ip_reassembly_max[LINUX_ID]);
    EXPECT_TRUE(!receiver.update_seen[LINUX_ID]);
    // Control: the same datagram at this version is applied.
    captured[0][single ? 1 : 2] = tt_VERSION;
    EXPECT_TRUE(deliver(0, LINUX_ID));
    EXPECT_EQ_U32(65507, receiver.peer_ip_reassembly_max[LINUX_ID]);
}

// By default a reader advertises its control law's cap, starting at n_max (no slow start since 2026-10-13: DESIGN.md
// section 8) - its ceiling, which reads back as n_max units of either type pair; a node whose ceiling holds one unit
// keeps no cap and advertises its ceiling as it is.
static void test_the_capped_reader_advertises_its_cap(void) {
    test_mock_reset();
    memset(&receiver, 0, sizeof(receiver));
    receiver.opts.net.ip_reassembly_max = 65507;
    node_init_locks(&receiver);
    reset_node_state(&receiver);
    EXPECT_EQ_U32(45, receiver.hybrid.cap);
    EXPECT_EQ_U32(65507, receiver.hybrid.advertised);
    EXPECT_EQ_U32(45, hybrid_units_within(&receiver, receiver.hybrid.advertised, true));
    receiver.opts.net.ip_reassembly_max = 1472;
    reset_node_state(&receiver);
    EXPECT_TRUE(!receiver.hybrid.rx_capable);
    EXPECT_EQ_U32(1472, receiver.hybrid.advertised);
}

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    test_the_capped_reader_advertises_its_cap();
    test_settings_and_defaults();
    test_capability_round_trip_mixed_peers();
    test_fragmented_announce_carries_it();
    test_reverse_endian_announce();
    test_version_mismatch();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_hybrid_capability: all tests passed\n");
    return 0;
}
