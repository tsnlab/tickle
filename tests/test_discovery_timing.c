/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The discovery summary carries its sender's update interval and miss threshold (the user's decision of 2026-10-22,
// network-profiles item C, option B: tt_HeartbeatHeader's three reserved bytes, zero = the default, no tt_VERSION
// bump), and a receiver judges that sender's silence by them instead of its own. On the mock HAL and clock:
//   - the encoding: zero for the default, round trips exact where the value is a whole number of a unit, never shorter
//     than the value anywhere in the range validate_discovery_options() accepts, and a hostile code held to a day;
//   - a default context's summary carries zeros (its bytes are what they were; tests/wire_identity_discovery_timing.sh
//     compares a whole capture with the parent's), a non-default one its own values;
//   - a default-interval receiver keeps a peer advertising 4 s past its own 3.5 s silence and drops it after the
//     peer's 14 s; a peer advertising 100 ms is dropped after 350 ms; an advertised miss threshold counts;
//   - zeros (a peer built before the change) are judged by the default - not by the receiver's own, which differs
//     here, so the test tells the two apart; a peer not yet heard in a summary is judged by the receiver's own;
//   - a reverse-endian sender's bytes read the same; a presumed-dead peer's value is forgotten.
// Each default-receiver case has its control: the same peer with zeros, judged at the same instants.
// tests/mutants_discovery_timing.py breaks each decision and expects this test to fail.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_discovery_timing.py passes a mutant

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define REMOTE_IP 0x0a000002U
#define PUB_ENDPOINT_ID 0x11111111
#define MS tt_MILLISECOND
#define SEC tt_SECOND

static struct tt_DiscoveryOptions none(void) {
    struct tt_DiscoveryOptions discovery;
    memset(&discovery, 0, sizeof(discovery));
    return discovery;
}

// A context built by hand, as the whitebox tests build theirs, holding `discovery` as a created one would.
static void init_node(struct tt_Context* node, const struct tt_DiscoveryOptions* discovery) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    if (discovery != NULL) {
        opts.discovery = *discovery;
        resolve_init_options(&opts);
    }
    node->opts = opts;
}

// ---- The encoding.

static uint16_t code_of(uint32_t unit, uint32_t count) {
    return (uint16_t)((unit << tt_SUMMARY_INTERVAL_MANTISSA_BITS) | count);
}

// Exact where the value is a whole number of the unit chosen; the default is 0.
static void test_encoding_round_trips_at_the_range_ends(void) {
    EXPECT_EQ_U32(0, summary_interval_encode(tt_CONTEXT_UPDATE_INTERVAL));
    EXPECT_EQ_U64(tt_CONTEXT_UPDATE_INTERVAL, summary_interval_decode(0));

    const struct {
        uint64_t ns;
        uint16_t code;
    } exact[] = {
        {tt_MICROSECOND, code_of(0, 1)},
        {8191U * tt_MICROSECOND, code_of(0, 8191)},
        {8200U * tt_MICROSECOND, code_of(1, 820)},
        {100U * MS, code_of(2, 1000)},
        {4U * SEC, code_of(3, 4000)},
        {8191U * MS, code_of(3, 8191)},
        {9U * SEC, code_of(4, 900)},
        {8191U * SEC, code_of(6, 8191)},
        {8200U * SEC, code_of(7, 82)},
        {tt_DISCOVERY_MAX_DURATION_NS, code_of(7, 864)},
    };
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        EXPECT_EQ_U32(exact[i].code, summary_interval_encode(exact[i].ns));
        EXPECT_EQ_U64(exact[i].ns, summary_interval_decode(exact[i].code));
    }
    // Below a microsecond, and between steps: rounded up, never down.
    EXPECT_EQ_U32(code_of(0, 1), summary_interval_encode(1));
    EXPECT_EQ_U32(code_of(0, 2), summary_interval_encode(tt_MICROSECOND + 1));
    EXPECT_EQ_U32(code_of(1, 820), summary_interval_encode((8191U * tt_MICROSECOND) + 1));
    EXPECT_EQ_U32(code_of(7, 82), summary_interval_encode((8191U * SEC) + 1));
    // A code counting no units is the default; one past a day (no context sends it) is held to a day.
    EXPECT_EQ_U64(tt_CONTEXT_UPDATE_INTERVAL, summary_interval_decode(code_of(5, 0)));
    EXPECT_EQ_U64(tt_DISCOVERY_MAX_DURATION_NS, summary_interval_decode(UINT16_MAX));
    EXPECT_EQ_U32(UINT16_MAX, summary_interval_encode(10U * tt_DISCOVERY_MAX_DURATION_NS)); // past every unit
}

// Across the whole accepted range: decode(encode(v)) is v or up to one step above it - at most 1.25% of v, or 1 us.
static void test_encoding_never_shortens(void) {
    int checked = 0;
    for (uint64_t v = 1; v <= tt_DISCOVERY_MAX_DURATION_NS; v = (v * 1031U / 1000U) + 1U) {
        if (v == tt_CONTEXT_UPDATE_INTERVAL) {
            continue;
        }
        uint16_t code = summary_interval_encode(v);
        uint64_t back = summary_interval_decode(code);
        bool ok = back >= v && (back - v <= v / 80U || back - v < tt_MICROSECOND);
        if (!ok) {
            fprintf(stderr, "  %llu ns -> 0x%04x -> %llu ns\n", (unsigned long long)v, code, (unsigned long long)back);
        }
        EXPECT_TRUE(ok);
        checked++;
    }
    EXPECT_TRUE(checked > 900);
}

// ---- The sender.

static void summary_bytes(const struct tt_DiscoveryOptions* discovery, uint8_t out[3]) {
    static struct tt_Context node;
    init_node(&node, discovery);
    struct tt_HeartbeatHeader summary;
    memset(&summary, 0xa5, sizeof(summary));
    fill_summary(&node, &summary);
    out[0] = summary.summary_interval[0];
    out[1] = summary.summary_interval[1];
    out[2] = summary.summary_miss_threshold;
}

static void test_summary_carries_the_senders_timing(void) {
    uint8_t b[3];
    summary_bytes(NULL, b); // the default: the zeros every summary carried
    EXPECT_EQ_U32(0, b[0]);
    EXPECT_EQ_U32(0, b[1]);
    EXPECT_EQ_U32(0, b[2]);

    struct tt_DiscoveryOptions d = none();
    d.update_interval_ns = 4 * SEC;
    summary_bytes(&d, b);
    EXPECT_EQ_U32(code_of(3, 4000) >> 8, b[0]); // most significant first
    EXPECT_EQ_U32(code_of(3, 4000) & 0xffU, b[1]);
    EXPECT_EQ_U32(0, b[2]);

    d = none();
    d.miss_threshold = 5;
    summary_bytes(&d, b);
    EXPECT_EQ_U32(0, b[0]);
    EXPECT_EQ_U32(0, b[1]);
    EXPECT_EQ_U32(5, b[2]);

    d.miss_threshold = tt_LIVELINESS_MISS_THRESHOLD; // set to the default: sent as the default
    d.update_interval_ns = tt_CONTEXT_UPDATE_INTERVAL;
    summary_bytes(&d, b);
    EXPECT_EQ_U32(0, b[0] | b[1] | b[2]);
}

// ---- The receiver.

#define TEST_ENTITY_ID_OF(endpoint_id) ((uint32_t)(endpoint_id) ^ 0xE1D00000U)

static void hear_announce(struct tt_Context* node, uint64_t at) {
    test_mock_now = at;
    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;
    uint8_t* buf = node->rx_buffer;
    struct test_announce* update = test_announce_at(buf);
    test_announce_set_last_modified(update, 100);
    update->announce.entity_count = 1;
    uint32_t tail = sizeof(struct test_announce);
    struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)(buf + tail);
    entity->endpoint_id = PUB_ENDPOINT_ID;
    entity->entity_id = TEST_ENTITY_ID_OF(PUB_ENDPOINT_ID);
    entity->kind = tt_KIND_TOPIC_SUBSCRIBER;
    tail += sizeof(struct tt_UpdateEntity);
    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, "topic");
    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, "sub");
    EXPECT_TRUE(process_data(node, &header, buf, 0, tail, REMOTE_IP, 8282));
    EXPECT_TRUE(node->update_seen[REMOTE_NODE_ID]);
}

// A whole summary datagram from REMOTE_NODE_ID (generation 100, the announce's) through process_packet(), carrying
// `interval` and `miss` in its three bytes; `reverse` writes it as a peer of the other byte order would.
static void hear_summary(struct tt_Context* node, uint64_t at, uint16_t interval, uint8_t miss, bool reverse) {
    test_mock_now = at;
    tt_ALIGNAS(8)
        uint8_t buf[sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_HeartbeatHeader)];
    memset(buf, 0, sizeof(buf));
    struct tt_Header* header = (struct tt_Header*)buf;
    header->magic_value = reverse ? REVERSE_MAGIC_VALUE : NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = REMOTE_NODE_ID;
    struct tt_SubmessageHeader* sub = (struct tt_SubmessageHeader*)(buf + sizeof(struct tt_Header));
    sub->type = tt_SUBMESSAGE_TYPE_HEARTBEAT;
    sub->receiver = tt_SUBMESSAGE_ID_ALL;
    uint16_t length = (uint16_t)(sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_HeartbeatHeader));
    sub->length = reverse ? _tt_bswap_16(length) : length;
    struct tt_HeartbeatHeader* hb =
        (struct tt_HeartbeatHeader*)(buf + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    uint32_t generation = reverse ? _tt_bswap_32(100) : 100;
    hb->endpoint_id = tt_DISCOVERY_ENDPOINT_ID;
    hb->first_available_seq_no = generation;
    hb->last_seq_no = generation;
    hb->entity_id = tt_DISCOVERY_ENTITY_ID; // all ones in either order
    hb->flags = tt_HEARTBEAT_FLAG_FINAL;
    hb->summary_interval[0] = (uint8_t)(interval >> 8);
    hb->summary_interval[1] = (uint8_t)interval;
    hb->summary_miss_threshold = miss;
    EXPECT_TRUE(process_packet(node, buf, 0, (uint32_t)sizeof(buf), REMOTE_IP, 8282, tt_TRANSPORT_UDP));
}

// Whether the remote node, heard in an announce and then a summary carrying (interval, miss) at 0, is still alive
// when the receiver's liveliness check runs at `at`. `receiver` NULL: a default receiver.
static bool alive_at(const struct tt_DiscoveryOptions* receiver, bool summary, uint16_t interval, uint8_t miss,
                     uint64_t at) {
    static struct tt_Context node;
    test_mock_reset();
    init_node(&node, receiver);
    hear_announce(&node, 0);
    if (summary) {
        hear_summary(&node, 0, interval, miss, false);
    }
    test_mock_now = at;
    check_liveliness(&node, at, NULL);
    return node.update_seen[REMOTE_NODE_ID];
}

// A default receiver (3.5 s) and a peer advertising 4 s: alive past 3.5 s, dead after 14 s. Control: the same peer
// sending zeros is dead after 3.5 s.
static void test_long_interval_peer_is_judged_by_its_own(void) {
    const uint16_t four = code_of(3, 4000);
    const uint64_t own = tt_LIVELINESS_SILENCE_NS;
    EXPECT_TRUE(alive_at(NULL, true, 0, 0, own));
    EXPECT_TRUE(!alive_at(NULL, true, 0, 0, own + 1)); // control
    EXPECT_TRUE(alive_at(NULL, true, four, 0, own + 1));
    EXPECT_TRUE(alive_at(NULL, true, four, 0, 14 * SEC));
    EXPECT_TRUE(!alive_at(NULL, true, four, 0, (14 * SEC) + 1));
}

// A peer advertising 100 ms: dead after its 350 ms, long before the receiver's own 3.5 s.
static void test_short_interval_peer_is_judged_sooner(void) {
    const uint16_t tenth = code_of(2, 1000);
    EXPECT_TRUE(alive_at(NULL, true, tenth, 0, 350 * MS));
    EXPECT_TRUE(!alive_at(NULL, true, tenth, 0, (350 * MS) + 1));
    EXPECT_TRUE(alive_at(NULL, true, 0, 0, (350 * MS) + 1)); // control
}

// An advertised miss threshold counts: 5 with the default interval is 5.5 s; with 100 ms, 550 ms.
static void test_advertised_threshold_counts(void) {
    EXPECT_TRUE(alive_at(NULL, true, 0, 5, (5 * SEC) + (SEC / 2)));
    EXPECT_TRUE(!alive_at(NULL, true, 0, 5, (5 * SEC) + (SEC / 2) + 1));
    EXPECT_TRUE(alive_at(NULL, true, code_of(2, 1000), 5, 550 * MS));
    EXPECT_TRUE(!alive_at(NULL, true, code_of(2, 1000), 5, (550 * MS) + 1));
}

// Zeros are the default, not the receiver's own: a receiver at 2 s (its own silence 7 s) drops a peer sending zeros
// after 3.5 s. A peer it has heard no summary from yet is judged by its own 7 s, as every peer was before.
static void test_zeros_are_the_default_and_no_summary_is_our_own(void) {
    struct tt_DiscoveryOptions d = none();
    d.update_interval_ns = 2 * SEC;
    EXPECT_TRUE(alive_at(&d, true, 0, 0, tt_LIVELINESS_SILENCE_NS));
    EXPECT_TRUE(!alive_at(&d, true, 0, 0, tt_LIVELINESS_SILENCE_NS + 1));
    EXPECT_TRUE(alive_at(&d, false, 0, 0, 7 * SEC));
    EXPECT_TRUE(!alive_at(&d, false, 0, 0, (7 * SEC) + 1));
    d = none();
    d.miss_threshold = 5; // its own silence 5.5 s; zeros are still 3.5 s
    EXPECT_TRUE(alive_at(&d, true, 0, 0, tt_LIVELINESS_SILENCE_NS));
    EXPECT_TRUE(!alive_at(&d, true, 0, 0, tt_LIVELINESS_SILENCE_NS + 1));
    d.update_interval_ns = 2 * SEC;
    // And the receiver's own options do not leak into a peer's advertised value: 4 s is 14 s here too.
    EXPECT_TRUE(alive_at(&d, true, code_of(3, 4000), 0, 14 * SEC));
    EXPECT_TRUE(!alive_at(&d, true, code_of(3, 4000), 0, (14 * SEC) + 1));
}

// The bytes are single bytes: a reverse-endian sender's are read as sent.
static void test_reverse_endian_sender(void) {
    static struct tt_Context node;
    test_mock_reset();
    init_node(&node, NULL);
    hear_announce(&node, 0);
    hear_summary(&node, 0, code_of(3, 4000), 5, true);
    EXPECT_EQ_U64(discovery_silence_of(4 * SEC, 5), node.peer_silence_ns[REMOTE_NODE_ID]);
}

// The latest summary decides (a peer restarted with other timing), and a peer presumed dead is forgotten: heard again
// before its next summary it is judged by our own.
static void test_latest_summary_and_death_reset(void) {
    static struct tt_Context node;
    test_mock_reset();
    init_node(&node, NULL);
    hear_announce(&node, 0);
    hear_summary(&node, 0, code_of(3, 4000), 0, false);
    EXPECT_EQ_U64(14 * SEC, node.peer_silence_ns[REMOTE_NODE_ID]);
    hear_summary(&node, SEC, 0, 0, false);
    EXPECT_EQ_U64(tt_LIVELINESS_SILENCE_NS, node.peer_silence_ns[REMOTE_NODE_ID]);
    hear_summary(&node, 2 * SEC, code_of(3, 4000), 0, false);
    test_mock_now = (16 * SEC) + 1;
    check_liveliness(&node, test_mock_now, NULL);
    EXPECT_TRUE(!node.update_seen[REMOTE_NODE_ID]);
    EXPECT_EQ_U64(0, node.peer_silence_ns[REMOTE_NODE_ID]);
}

// Sender to receiver: what a context's summary says is the silence that context gives its own peers, or a little more
// where the interval is rounded up.
static void test_sender_to_receiver(void) {
    const struct {
        uint64_t interval;
        uint8_t miss;
    } cases[] = {
        {0, 0}, {4 * SEC, 0}, {100 * MS, 0}, {0, 7}, {2500 * MS, 1}, {tt_DISCOVERY_MAX_DURATION_NS, 255}, {1234567, 2},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct tt_DiscoveryOptions d = none();
        d.update_interval_ns = cases[i].interval;
        d.miss_threshold = cases[i].miss;
        static struct tt_Context sender;
        init_node(&sender, &d);
        struct tt_HeartbeatHeader summary;
        fill_summary(&sender, &summary);
        uint16_t code = (uint16_t)((summary.summary_interval[0] << 8) | summary.summary_interval[1]);
        static struct tt_Context receiver;
        test_mock_reset();
        init_node(&receiver, NULL);
        hear_announce(&receiver, 0);
        hear_summary(&receiver, 0, code, summary.summary_miss_threshold, false);
        uint64_t want = discovery_silence_ns(&sender);
        uint64_t got = receiver.peer_silence_ns[REMOTE_NODE_ID];
        EXPECT_TRUE(got >= want && got - want <= (want / 80U) + tt_MICROSECOND);
    }
}

// A caller's context is not zeroed for it: creation clears every peer's value (reset_node_state()), or a stale one
// would keep a peer alive, or drop it, by a silence nobody advertised.
static void test_creation_clears_every_peer(void) {
    static struct tt_Context ctx;
    test_mock_reset();
    test_mock_now = 5 * SEC;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    memset(&ctx, 0xAA, sizeof(ctx));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&ctx, NULL));
    int stale = 0;
    for (int i = 0; i < tt_MAX_CONTEXT_IDS; i++) {
        stale += ctx.peer_silence_ns[i] != 0 ? 1 : 0;
    }
    EXPECT_EQ_INT(0, stale);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_encoding_round_trips_at_the_range_ends();
    test_encoding_never_shortens();
    test_summary_carries_the_senders_timing();
    test_long_interval_peer_is_judged_by_its_own();
    test_short_interval_peer_is_judged_sooner();
    test_advertised_threshold_counts();
    test_zeros_are_the_default_and_no_summary_is_our_own();
    test_reverse_endian_sender();
    test_latest_summary_and_death_reset();
    test_sender_to_receiver();
    test_creation_clears_every_peer();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_discovery_timing: all tests passed\n");
    return 0;
}
