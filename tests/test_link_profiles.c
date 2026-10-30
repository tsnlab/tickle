/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Link profiles (struct tt_LinkOptions.profile and its value fields; docs/DESIGN.md "Link profiles"), through the mock
// HAL and tt_Context_create_with():
//   - ETHERNET, WIFI and a context with no links configured keep every default, whatever the interface's MTU - but
//     WIFI's discovery: unicast copies of the summary to its known peers (one round per update interval, capped, taken
//     in turn), a miss threshold of 6 where the context leaves it, and a change's announce repeated at 100/300/700 ms;
//   - T1S and TSN fill their links' zero fields with their values, and the context derives what it applies to all its
//     sends from its links (the smallest MTU, the longest retry floor, the shortest idle HEARTBEAT and tick, the most
//     frequent piggyback, batching where every link batches, MTU units where any link asks);
//   - every field changes what the context does: the datagram length of a batch and of the announce (mtu), unicast or
//     broadcast (unicast_threshold), a Publisher batching (batch), its piggybacked HEARTBEATs (piggyback_divisor), the
//     measured G's floor (retry_floor_ns, and link_rate through T1S's), the flush tick (batch_tick_ns), the idle
//     HEARTBEAT (idle_heartbeat_ns) and the large-sample mode (large_fragmentation). The fragments a smaller MTU cuts
//     are tests/test_data_frag.c's (test_a_narrow_link_*), in a build that fragments;
//   - every check refuses the value past its bound with tt_RET_INVALID_ARGUMENT and accepts the bound itself, and a
//   link
//     value that contradicts the context's own setting of it is refused
//     (tests/mutants_link_profiles.sh removes each check and each reader and expects a test to fail).

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_link_profiles.sh passes a mutant

#define LINK_BROADCAST "10.0.0.255"
#define PEER_IP 0x0a000009U
#define PEER_ID 9
#define PORT 8282
#define T1S_FLOOR_1000 13286400ULL // 2 cycles x 8 nodes x (1000 + 38) bytes x 8 bits at 10 Mbit/s, in ns
#define T1S_FLOOR_1500 19686400ULL // the same at 1500 bytes

static struct tt_Context node;
static struct tt_TypeSupport topic;
static uint8_t sample[256];
static uint32_t sample_len = 4;

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memcpy(payload, sample, len);
    return (int32_t)len;
}

// For a creation: the mock's address auto-detection answers an id, every link resolves as an interface would, and the
// interface reports `mtu` (-1: none).
static void reset_mock_for_create(int32_t mtu) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    test_mock_link_mtu = mtu;
}

static struct tt_InitOptions one_link(uint8_t profile) {
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = LINK_BROADCAST, .profile = profile};
    opts.net.link_count = 1;
    return opts;
}

static void create(const struct tt_InitOptions* opts, int32_t interface_mtu) {
    reset_mock_for_create(interface_mtu);
    memset(&node, 0, sizeof(node));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&node, opts));
    memset(&topic, 0, sizeof(topic));
    topic.name = "link_profile_topic";
    topic.data_size = sizeof(sample);
    topic.data_encode_size = sized_encode_size;
    topic.data_encode = sized_encode;
}

static void destroy(void) {
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
}

// ---- Defaults: ETHERNET, WIFI and no links configured change nothing.

static void expect_context_defaults(const struct tt_Context* c) {
    EXPECT_EQ_U32(tt_CONTROL_MAX_LENGTH, c->control_max);
    EXPECT_EQ_U32(tt_CONTROL_MAX_LENGTH - LINK_FRAG_CONT_OVERHEAD, c->frag_cont);
    EXPECT_TRUE(!c->batch_default);
    EXPECT_EQ_U32(0, c->piggyback_every);
    EXPECT_EQ_U32(0, c->retry_floor_ns);
    EXPECT_EQ_U32(1, c->timer_resolution_ns); // the mock's
    EXPECT_EQ_U64(tt_TIMER_LATENESS_INITIAL, c->opts.reliable.timer_lateness_initial);
    EXPECT_EQ_U64(tt_CONTEXT_TX_INTERVAL, c->opts.discovery.tx_interval_ns);
    EXPECT_EQ_U64(reliable_defaults.idle_heartbeat_period, c->opts.reliable.idle_heartbeat_period);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_AUTO, c->opts.net.large_fragmentation);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_AUTO, c->large_fragmentation);
}

static void test_ethernet_wifi_and_no_links_keep_the_defaults(void) {
    // A hand-built context (node_init_locks()).
    memset(&node, 0, sizeof(node));
    test_mock_reset();
    node_init_locks(&node);
    EXPECT_EQ_U32(tt_CONTROL_MAX_LENGTH, node.control_max);
    EXPECT_EQ_U32(tt_CONTROL_MAX_LENGTH - LINK_FRAG_CONT_OVERHEAD, node.frag_cont);
    EXPECT_TRUE(!node.batch_default);
    EXPECT_EQ_U32(0, node.piggyback_every);

    // No links configured, on an interface narrower than 1500: the default link's 1500 is kept (and warned about).
    create(NULL, 1000);
    expect_context_defaults(&node);
    destroy();

    static const uint8_t profiles[] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_WIFI};
    for (size_t i = 0; i < sizeof(profiles); i++) {
        struct tt_InitOptions opts = one_link(profiles[i]);
        create(&opts, 1000);
        expect_context_defaults(&node);
        const struct tt_LinkOptions* link = &node.opts.net.links[0];
        EXPECT_EQ_U32(1500, link->mtu); // not the interface's
        EXPECT_EQ_U32(tt_UNICAST_PEER_THRESHOLD, link->unicast_threshold);
        EXPECT_EQ_U32(tt_SWITCH_OFF, link->batch);
        EXPECT_EQ_U32(0, link->large_fragmentation);
        EXPECT_EQ_U32(0, link->piggyback_divisor);
        EXPECT_EQ_U64(1000000000ULL, link->link_rate);
        EXPECT_EQ_U64(0, link->retry_floor_ns);
        EXPECT_EQ_U64(0, link->idle_heartbeat_ns);
        EXPECT_EQ_U64(0, link->batch_tick_ns);
        EXPECT_EQ_U32(tt_UNICAST_PEER_THRESHOLD, node.links[0].unicast_threshold);
        destroy();
    }

    // tt_InitOptions_default() then a T1S link: the defaults it resolved are not "set", so the profile still applies.
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = LINK_BROADCAST, .profile = tt_LINK_PROFILE_T1S};
    opts.net.link_count = 1;
    create(&opts, -1);
    EXPECT_EQ_U64(tt_SECOND, node.opts.reliable.idle_heartbeat_period);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_MTU, node.opts.net.large_fragmentation);
    destroy();
}

// ---- The profiles' values.

static void test_t1s_fills_its_link_and_the_context(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_T1S);
    create(&opts, 1000);
    const struct tt_LinkOptions* link = &node.opts.net.links[0];
    EXPECT_EQ_U32(1000, link->mtu); // the interface's
    EXPECT_EQ_U32(tt_UNICAST_ALWAYS_BROADCAST, link->unicast_threshold);
    EXPECT_EQ_U32(0, node.links[0].unicast_threshold); // as the send path compares it: no peer is unicast to
    EXPECT_EQ_U32(tt_SWITCH_ON, link->batch);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_MTU, link->large_fragmentation);
    EXPECT_EQ_U32(8, link->piggyback_divisor);
    EXPECT_EQ_U64(10000000ULL, link->link_rate);
    EXPECT_EQ_U64(T1S_FLOOR_1000, link->retry_floor_ns);
    EXPECT_EQ_U64(tt_SECOND, link->idle_heartbeat_ns);
    EXPECT_EQ_U64(0, link->batch_tick_ns); // the context's 1 ms
    // The context.
    EXPECT_EQ_U32(1000 - 28, node.control_max);
    EXPECT_EQ_U32(1000 - 28 - LINK_FRAG_CONT_OVERHEAD, node.frag_cont);
    EXPECT_TRUE(node.batch_default);
    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_BITS / 8U, node.piggyback_every);
    EXPECT_EQ_U32(T1S_FLOOR_1000, node.retry_floor_ns);
    EXPECT_EQ_U32(T1S_FLOOR_1000, node.timer_resolution_ns);
    EXPECT_EQ_U64(T1S_FLOOR_1000, node.opts.reliable.timer_lateness_initial);
    EXPECT_EQ_U64(tt_CONTEXT_TX_INTERVAL, node.opts.discovery.tx_interval_ns); // discovery timing unchanged
    EXPECT_EQ_U64(tt_CONTEXT_UPDATE_INTERVAL, node.opts.discovery.update_interval_ns);
    EXPECT_EQ_U64(tt_SECOND, node.opts.reliable.idle_heartbeat_period);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_MTU, node.opts.net.large_fragmentation);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_MTU, node.large_fragmentation);
    destroy();

    // An interface that reports no MTU: 1500, and the floor at 1500.
    create(&opts, -1);
    EXPECT_EQ_U32(1500, node.opts.net.links[0].mtu);
    EXPECT_EQ_U32(tt_CONTROL_MAX_LENGTH, node.control_max);
    EXPECT_EQ_U32(T1S_FLOOR_1500, node.retry_floor_ns);
    destroy();
    // Nor one below tt_LINK_MTU_MIN, nor above the link's: an interface MTU of 9000 caps nothing.
    create(&opts, 500);
    EXPECT_EQ_U32(1500, node.opts.net.links[0].mtu);
    destroy();
    create(&opts, 9000);
    EXPECT_EQ_U32(9000, node.opts.net.links[0].mtu);
    EXPECT_EQ_U32(tt_CONTROL_MAX_LENGTH, node.control_max);
    destroy();
}

static void test_tsn_takes_the_interface_mtu_and_ethernet_values(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_TSN);
    create(&opts, 1000);
    const struct tt_LinkOptions* link = &node.opts.net.links[0];
    EXPECT_EQ_U32(1000, link->mtu);
    EXPECT_EQ_U32(tt_UNICAST_PEER_THRESHOLD, link->unicast_threshold);
    EXPECT_EQ_U32(tt_SWITCH_OFF, link->batch);
    EXPECT_EQ_U32(0, link->large_fragmentation);
    EXPECT_EQ_U64(0, link->retry_floor_ns);
    EXPECT_EQ_U64(1000000000ULL, link->link_rate);
    EXPECT_EQ_U32(1000 - 28, node.control_max);
    EXPECT_TRUE(!node.batch_default);
    EXPECT_EQ_U32(0, node.piggyback_every);
    EXPECT_EQ_U32(0, node.retry_floor_ns);
    EXPECT_EQ_U64(reliable_defaults.idle_heartbeat_period, node.opts.reliable.idle_heartbeat_period);
    // The narrower MTU alone still means MTU units: a run's units do not fit it.
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_AUTO, node.opts.net.large_fragmentation);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_MTU, node.large_fragmentation);
    destroy();
}

static void test_several_links_combine(void) {
    // T1S beside ETHERNET: the smaller MTU, the T1S floor, the shorter idle HEARTBEAT (ETHERNET's default), no batching
    // (ETHERNET does not batch), the T1S piggyback, MTU units; each link keeps its own threshold.
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] =
        (struct tt_LinkOptions) {.broadcast = "10.0.0.255", .profile = tt_LINK_PROFILE_T1S, .mtu = 1200};
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .profile = tt_LINK_PROFILE_ETHERNET};
    opts.net.link_count = 2;
    create(&opts, -1);
    EXPECT_EQ_U32(1200 - 28, node.control_max);
    EXPECT_EQ_U32(link_t1s_retry_floor(1200, 10000000ULL), node.retry_floor_ns);
    EXPECT_EQ_U64(reliable_defaults.idle_heartbeat_period, node.opts.reliable.idle_heartbeat_period);
    EXPECT_TRUE(!node.batch_default);
    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_BITS / 8U, node.piggyback_every);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_MTU, node.opts.net.large_fragmentation);
    EXPECT_EQ_U32(0, node.links[0].unicast_threshold);
    EXPECT_EQ_U32(tt_UNICAST_PEER_THRESHOLD, node.links[1].unicast_threshold);
    destroy();
    // Two T1S links batch; two links agreeing on IP send IP.
    opts.net.links[1].profile = tt_LINK_PROFILE_T1S;
    create(&opts, -1);
    EXPECT_TRUE(node.batch_default);
    EXPECT_EQ_U64(tt_SECOND, node.opts.reliable.idle_heartbeat_period);
    destroy();
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255", .large_fragmentation = 3};
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .large_fragmentation = 3};
    create(&opts, -1);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_IP, node.opts.net.large_fragmentation);
    destroy();
    opts.net.links[1].large_fragmentation = 0; // one leaves it to the context: no agreement, AUTO
    create(&opts, -1);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_AUTO, node.opts.net.large_fragmentation);
    destroy();
}

// ---- Each field changes what the context does.

// What went on the wire: the count and the longest datagram.
static int sent_count;
static size_t sent_longest;
static int sent_heartbeats;

// Counts the HEARTBEAT submessages of a classic-form datagram.
static int heartbeats_in(const uint8_t* classic, size_t len) {
    int found = 0;
    size_t head = sizeof(struct tt_Header);
    while (head + sizeof(struct tt_SubmessageHeader) <= len) {
        const struct tt_SubmessageHeader* sub = (const struct tt_SubmessageHeader*)(classic + head);
        if (sub->length < sizeof(struct tt_SubmessageHeader)) {
            break;
        }
        found += sub->type == tt_SUBMESSAGE_TYPE_HEARTBEAT ? 1 : 0;
        head += sub->length;
    }
    return found;
}

static void capture(const void* buf, size_t len) {
    static uint8_t classic[tt_MAX_BUFFER_LENGTH * 2];
    sent_count++;
    sent_longest = len > sent_longest ? len : sent_longest;
    if (len + sizeof(struct tt_Header) <= sizeof(classic)) {
        size_t classic_len = test_classic_form(buf, len, classic);
        sent_heartbeats += heartbeats_in(classic, classic_len);
    }
}

static void start_capture(void) {
    sent_count = 0;
    sent_longest = 0;
    sent_heartbeats = 0;
    test_mock_send_call_count = 0;
    test_mock_send_to_call_count = 0;
    test_mock_send_hook = capture;
}

static struct tt_Publisher pubs[12];
static uint64_t pub_storage[(size_t)64U * 1024U / sizeof(uint64_t)];

static void create_publisher(struct tt_Publisher* pub, const char* name, const struct tt_QosProfile* qos,
                             const struct tt_PublisherOptions* options) {
    size_t storage = tt_Publisher_storage_size(&topic, qos);
    EXPECT_TRUE(storage <= sizeof(pub_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node), pub, &topic, name, qos, options,
                                                      storage != 0 ? pub_storage : NULL, storage));
}

static void publish_one(struct tt_Publisher* pub) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(pub, (struct tt_Data*)sample));
}

static void test_mtu_caps_a_batch_and_the_announce(void) {
    for (int narrow = 0; narrow < 2; narrow++) {
        struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_ETHERNET);
        opts.net.links[0].mtu = narrow ? 700 : 0;
        create(&opts, -1);
        // A batch of 8 samples of 200 bytes: about 1,800 bytes, one datagram at 1500 and three at 700.
        sample_len = 200;
        struct tt_PublisherOptions batched = {.batch = true};
        create_publisher(&pubs[0], "batched", &TEST_QOS_UNRELIABLE, &batched);
        (void)upsert_peer(pubs[0].peers, PEER_ID, PEER_IP, PORT);
        (void)upsert_peer(pubs[0].peers, PEER_ID + 1, PEER_IP + 1, PORT); // two peers: one is unicast, so one each
        (void)upsert_peer(pubs[0].peers, PEER_ID + 2, PEER_IP + 2, PORT); // three: the batch is one broadcast
        start_capture();
        for (int i = 0; i < 8; i++) {
            publish_one(&pubs[0]);
        }
        node_flush(&node, 0, NULL);
        EXPECT_TRUE(sent_longest <= (narrow ? 672U : (size_t)tt_CONTROL_MAX_LENGTH));
        EXPECT_TRUE(narrow ? sent_count >= 3 : sent_count == 2); // the last (8th) one fits the second at 1500

        // The announce: 12 endpoints with long names, 1388 bytes - one datagram at 1500, three parts at 700.
        static const char* const names[] = {
            "an_endpoint_name_long_enough_to_fill_announces_0", "an_endpoint_name_long_enough_to_fill_announces_1",
            "an_endpoint_name_long_enough_to_fill_announces_2", "an_endpoint_name_long_enough_to_fill_announces_3",
            "an_endpoint_name_long_enough_to_fill_announces_4", "an_endpoint_name_long_enough_to_fill_announces_5",
            "an_endpoint_name_long_enough_to_fill_announces_6", "an_endpoint_name_long_enough_to_fill_announces_7",
            "an_endpoint_name_long_enough_to_fill_announces_8", "an_endpoint_name_long_enough_to_fill_announces_9",
            "an_endpoint_name_long_enough_to_fill_announces_a"};
        for (int i = 0; i < 11; i++) {
            create_publisher(&pubs[1 + i], names[i], &TEST_QOS_UNRELIABLE, NULL);
        }
        start_capture();
        EXPECT_TRUE(build_and_send_update(&node, NULL, 0));
        node_flush(&node, 0, NULL); // the last part may wait in the buffer for the tick
        EXPECT_TRUE(sent_longest <= (narrow ? 672U : (size_t)tt_CONTROL_MAX_LENGTH));
        EXPECT_EQ_INT(narrow ? 3 : 1, sent_count); // one datagram of 1388 bytes at 1500
        EXPECT_TRUE(narrow || sent_longest > 672U);
        sample_len = 4;
        destroy();
    }
}

static void test_unicast_threshold_t1s_broadcasts_to_one_peer(void) {
    static const uint8_t profiles[] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_T1S};
    for (size_t i = 0; i < sizeof(profiles); i++) {
        struct tt_InitOptions opts = one_link(profiles[i]);
        create(&opts, -1);
        struct tt_PublisherOptions unbatched = {.batching = tt_SWITCH_OFF};
        create_publisher(&pubs[0], "one_reader", &TEST_QOS_UNRELIABLE, &unbatched);
        (void)upsert_peer(pubs[0].peers, PEER_ID, PEER_IP, PORT);
        start_capture();
        publish_one(&pubs[0]);
        bool t1s = profiles[i] == tt_LINK_PROFILE_T1S;
        EXPECT_EQ_INT(t1s ? 0 : 1, test_mock_send_to_call_count); // ETHERNET: unicast to its one peer
        EXPECT_EQ_INT(t1s ? 1 : 0, test_mock_send_call_count - test_mock_send_to_call_count); // T1S: one broadcast
        destroy();
        // An explicit threshold beats the profile's.
        opts.net.links[0].unicast_threshold = 2;
        create(&opts, -1);
        create_publisher(&pubs[0], "one_reader", &TEST_QOS_UNRELIABLE, &unbatched);
        (void)upsert_peer(pubs[0].peers, PEER_ID, PEER_IP, PORT);
        start_capture();
        publish_one(&pubs[0]);
        EXPECT_EQ_INT(1, test_mock_send_to_call_count);
        destroy();
    }
}

// The zero-copy publish (publish_zerocopy(), a topic with an in-place encoder and a sample that fills a datagram) takes
// the link's resolved threshold, as the staging path does: a T1S link broadcasts to its one peer, ETHERNET unicasts,
// and a T1S link given threshold 2 unicasts. Before 2026-10-22 it compared with the compiled tt_UNICAST_PEER_THRESHOLD
// and unicast on T1S too.
#define INPLACE_LEN 1440U // 4-aligned, and with its framing past what a second submessage could share
static uint8_t inplace_body[INPLACE_LEN];

static int32_t inplace_encode(struct tt_Data* data, const uint8_t** body) {
    (void)data;
    *body = inplace_body;
    return (int32_t)INPLACE_LEN;
}

static void test_unicast_threshold_on_the_zero_copy_path(void) {
    struct {
        uint8_t profile;
        uint8_t threshold;
        bool unicast;
    } const cases[] = {
        {tt_LINK_PROFILE_ETHERNET, 0, true},
        {tt_LINK_PROFILE_T1S, 0, false},
        {tt_LINK_PROFILE_T1S, 2, true},
        {tt_LINK_PROFILE_ETHERNET, tt_UNICAST_ALWAYS_BROADCAST, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct tt_InitOptions opts = one_link(cases[i].profile);
        opts.net.links[0].unicast_threshold = cases[i].threshold;
        create(&opts, -1);
        topic.data_encode_inplace = inplace_encode;
        struct tt_PublisherOptions unbatched = {.batching = tt_SWITCH_OFF};
        create_publisher(&pubs[0], "zero_copy", &TEST_QOS_UNRELIABLE, &unbatched);
        (void)upsert_peer(pubs[0].peers, PEER_ID, PEER_IP, PORT);
        start_capture();
        test_mock_send_to_ip_count = 0;
        publish_one(&pubs[0]);
        EXPECT_EQ_INT(1, sent_count);
        EXPECT_TRUE(sent_longest > INPLACE_LEN); // the zero-copy datagram itself, not a batch
        EXPECT_EQ_INT(cases[i].unicast ? 1 : 0, test_mock_send_to_call_count);
        EXPECT_EQ_INT(cases[i].unicast ? 0 : 1, test_mock_send_call_count - test_mock_send_to_call_count);
        EXPECT_TRUE(!cases[i].unicast || test_mock_send_to_ips[0] == PEER_IP);
        topic.data_encode_inplace = NULL;
        destroy();
    }
}

static void test_batch_is_the_publishers_default(void) {
    struct {
        uint8_t profile;
        uint8_t link_batch;
        struct tt_PublisherOptions options;
        bool batches;
    } const cases[] = {
        {tt_LINK_PROFILE_ETHERNET, 0, {0}, false},
        {tt_LINK_PROFILE_T1S, 0, {0}, true},
        {tt_LINK_PROFILE_T1S, tt_SWITCH_OFF, {0}, false},             // the link's own switch
        {tt_LINK_PROFILE_ETHERNET, tt_SWITCH_ON, {0}, true},          // the link's own switch
        {tt_LINK_PROFILE_T1S, 0, {.batching = tt_SWITCH_OFF}, false}, // the Publisher's
        {tt_LINK_PROFILE_ETHERNET, 0, {.batching = tt_SWITCH_ON}, true},
        {tt_LINK_PROFILE_ETHERNET, 0, {.batch = true}, true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct tt_InitOptions opts = one_link(cases[i].profile);
        opts.net.links[0].batch = cases[i].link_batch;
        create(&opts, -1);
        create_publisher(&pubs[0], "batching", &TEST_QOS_UNRELIABLE, &cases[i].options);
        EXPECT_EQ_INT(cases[i].batches, pubs[0].batch);
        (void)upsert_peer(pubs[0].peers, PEER_ID, PEER_IP, PORT);
        start_capture();
        publish_one(&pubs[0]);
        EXPECT_EQ_INT(cases[i].batches ? 0 : 1, sent_count); // a batched sample waits for the tick
        node_flush(&node, 0, NULL);
        EXPECT_EQ_INT(1, sent_count);
        destroy();
    }
    // The deprecated create takes the links' default too.
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_T1S);
    create(&opts, -1);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&node, &pubs[0], &topic, "legacy"));
    EXPECT_TRUE(pubs[0].batch);
    EXPECT_EQ_U32(tt_RELIABLE_BITMAP_BITS / 8U, pubs[0].heartbeat_piggyback_every);
    destroy();
}

static void test_piggyback_divisor_piggybacks_heartbeats(void) {
    // An unbatched RELIABLE Publisher, 8 samples: with a divisor of tt_RELIABLE_BITMAP_BITS / 4, a HEARTBEAT rides on
    // every 4th - two; with none, none.
    struct tt_QosProfile reliable = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 16},
                                     .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    for (int divided = 0; divided < 2; divided++) {
        struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_ETHERNET);
        opts.net.links[0].piggyback_divisor = divided ? tt_RELIABLE_BITMAP_BITS / 4U : 0U;
        create(&opts, -1);
        create_publisher(&pubs[0], "piggyback", &reliable, NULL);
        EXPECT_EQ_U32(divided ? 4U : 0U, pubs[0].heartbeat_piggyback_every);
        (void)upsert_peer(pubs[0].peers, PEER_ID, PEER_IP, PORT);
        start_capture();
        for (int i = 0; i < 8; i++) {
            publish_one(&pubs[0]);
        }
        EXPECT_EQ_INT(divided ? 2 : 0, sent_heartbeats);
        destroy();
    }
    // A Publisher's own period wins, and tt_HEARTBEAT_PIGGYBACK_NONE is none whatever the links say.
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_T1S);
    create(&opts, -1);
    struct tt_PublisherOptions own = {.heartbeat_piggyback_every = 5};
    create_publisher(&pubs[0], "own", &reliable, &own);
    EXPECT_EQ_U32(5, pubs[0].heartbeat_piggyback_every);
    struct tt_PublisherOptions none = {.heartbeat_piggyback_every = tt_HEARTBEAT_PIGGYBACK_NONE};
    create_publisher(&pubs[1], "none", &reliable, &none);
    EXPECT_EQ_U32(0, pubs[1].heartbeat_piggyback_every);
    destroy();
}

static void test_retry_floor_holds_up_the_measured_g(void) {
    // A floor on an ETHERNET link: G cold, and G measured below it, are the floor; without one they are not.
    for (int floored = 0; floored < 2; floored++) {
        struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_ETHERNET);
        opts.net.links[0].retry_floor_ns = floored ? 5 * tt_MILLISECOND : 0;
        create(&opts, -1);
        EXPECT_EQ_U64(floored ? 5 * tt_MILLISECOND : tt_TIMER_LATENESS_INITIAL, reliable_retry_granularity(&node));
        timer_lateness_fold(&node, 1000);
        EXPECT_TRUE(floored ? reliable_retry_granularity(&node) == 5 * tt_MILLISECOND
                            : reliable_retry_granularity(&node) < tt_TIMER_LATENESS_INITIAL);
        destroy();
    }
    // A fixed G is the caller's own: the floor leaves it.
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[0].retry_floor_ns = 5 * tt_MILLISECOND;
    opts.reliable.retry_granularity = tt_MILLISECOND;
    create(&opts, -1);
    EXPECT_EQ_U64(tt_MILLISECOND, reliable_retry_granularity(&node));
    destroy();
    // T1S derives its floor from its link_rate: twice the rate, half the floor.
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].link_rate = 20000000ULL;
    create(&opts, 1000);
    EXPECT_EQ_U32(T1S_FLOOR_1000 / 2U, node.retry_floor_ns);
    destroy();
}

static void test_tick_idle_heartbeat_and_mode_set_the_context(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[0].batch_tick_ns = 2 * tt_MILLISECOND;
    opts.net.links[0].idle_heartbeat_ns = 2 * tt_SECOND;
    opts.net.links[0].large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    create(&opts, -1);
    EXPECT_EQ_U64(2 * tt_MILLISECOND, node.opts.discovery.tx_interval_ns);
    EXPECT_EQ_U64(2 * tt_SECOND, node.opts.reliable.idle_heartbeat_period);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_IP, node.large_fragmentation);
    struct tt_QosProfile reliable = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 16},
                                     .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    create_publisher(&pubs[0], "idle", &reliable, NULL);
    EXPECT_EQ_U64(2 * tt_SECOND, pubs[0].idle_heartbeat_ns);
    destroy();
    // Two links: the shorter tick.
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .batch_tick_ns = 3 * tt_MILLISECOND};
    opts.net.link_count = 2;
    create(&opts, -1);
    EXPECT_EQ_U64(2 * tt_MILLISECOND, node.opts.discovery.tx_interval_ns);
    destroy();
    // A T1S Publisher's idle HEARTBEAT: 1 s.
    opts = one_link(tt_LINK_PROFILE_T1S);
    create(&opts, -1);
    create_publisher(&pubs[0], "idle", &reliable, NULL);
    EXPECT_EQ_U64(tt_SECOND, pubs[0].idle_heartbeat_ns);
    destroy();
}

// ---- WIFI's discovery (2026-10-23): summary copies, the miss threshold, announce repeats.

static void expect_discovery(bool copies, uint8_t repeats, uint8_t miss) {
    EXPECT_TRUE(node.summary_copies == copies);
    EXPECT_EQ_U32(repeats, node.announce_repeats);
    EXPECT_EQ_U32(miss, node.opts.discovery.miss_threshold);
}

static void test_wifi_discovery_values(void) {
    // Each profile's, on the link and the context; WIFI's summary carries its threshold.
    static const uint8_t others[] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_T1S, tt_LINK_PROFILE_TSN};
    for (size_t i = 0; i < sizeof(others); i++) {
        struct tt_InitOptions opts = one_link(others[i]);
        create(&opts, -1);
        EXPECT_EQ_U32(tt_SWITCH_OFF, node.opts.net.links[0].summary_unicast);
        EXPECT_EQ_U32(tt_ANNOUNCE_REPEATS_NONE, node.opts.net.links[0].announce_repeats);
        expect_discovery(false, 0, tt_LIVELINESS_MISS_THRESHOLD);
        destroy();
    }
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    create(&opts, -1);
    EXPECT_EQ_U32(tt_SWITCH_ON, node.opts.net.links[0].summary_unicast);
    EXPECT_EQ_U32(tt_LINK_WIFI_ANNOUNCE_REPEATS, node.opts.net.links[0].announce_repeats);
    expect_discovery(true, tt_LINK_WIFI_ANNOUNCE_REPEATS, tt_LINK_WIFI_MISS_THRESHOLD);
    struct tt_HeartbeatHeader summary;
    memset(&summary, 0, sizeof(summary));
    fill_summary(&node, &summary);
    EXPECT_EQ_U32(tt_LINK_WIFI_MISS_THRESHOLD, summary.summary_miss_threshold);
    destroy();
    // From tt_InitOptions_default(): its resolved default threshold is not "set".
    tt_InitOptions_default(&opts);
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = LINK_BROADCAST, .profile = tt_LINK_PROFILE_WIFI};
    opts.net.link_count = 1;
    create(&opts, -1);
    expect_discovery(true, tt_LINK_WIFI_ANNOUNCE_REPEATS, tt_LINK_WIFI_MISS_THRESHOLD);
    destroy();

    // Explicit fields win: on WIFI, off and none and another count and threshold; on ETHERNET, on and a count.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].summary_unicast = tt_SWITCH_OFF;
    opts.net.links[0].announce_repeats = tt_ANNOUNCE_REPEATS_NONE;
    opts.discovery.miss_threshold = 4;
    create(&opts, -1);
    expect_discovery(false, 0, 4);
    destroy();
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].announce_repeats = 5;
    create(&opts, -1);
    expect_discovery(true, 5, tt_LINK_WIFI_MISS_THRESHOLD);
    destroy();
    opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[0].summary_unicast = tt_SWITCH_ON;
    opts.net.links[0].announce_repeats = 2;
    create(&opts, -1);
    expect_discovery(true, 2, tt_LIVELINESS_MISS_THRESHOLD); // the threshold is WIFI's profile value only
    destroy();
    // A WIFI link that broadcasts as soon as one peer is known takes no copies: each would be one more broadcast.
    opts = one_link(tt_LINK_PROFILE_WIFI);
    opts.net.links[0].unicast_threshold = tt_UNICAST_ALWAYS_BROADCAST;
    create(&opts, -1);
    expect_discovery(false, tt_LINK_WIFI_ANNOUNCE_REPEATS, tt_LINK_WIFI_MISS_THRESHOLD);
    destroy();

    // Several links: WIFI first or last sets the threshold and the copies; the repeats are the most any link asks.
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255", .profile = tt_LINK_PROFILE_WIFI};
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .profile = tt_LINK_PROFILE_ETHERNET};
    opts.net.link_count = 2;
    create(&opts, -1);
    expect_discovery(true, tt_LINK_WIFI_ANNOUNCE_REPEATS, tt_LINK_WIFI_MISS_THRESHOLD);
    destroy();
    opts.net.links[0].profile = tt_LINK_PROFILE_ETHERNET;
    opts.net.links[0].announce_repeats = 5;
    opts.net.links[1].profile = tt_LINK_PROFILE_WIFI;
    opts.net.links[1].announce_repeats = 2;
    create(&opts, -1);
    expect_discovery(true, 5, tt_LINK_WIFI_MISS_THRESHOLD);
    destroy();
}

// WIFI's reliable readers' retry estimate (tt_LinkOptions.repair_timing, 2026-10-23): on for WIFI, off for the rest;
// explicit fields win either way; the context's is on if any link's is. What it does is test_reliable_pubsub's.
static void test_wifi_repair_timing(void) {
    static const uint8_t others[] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_T1S, tt_LINK_PROFILE_TSN};
    for (size_t i = 0; i < sizeof(others); i++) {
        struct tt_InitOptions opts = one_link(others[i]);
        create(&opts, -1);
        EXPECT_EQ_U32(tt_SWITCH_OFF, node.opts.net.links[0].repair_timing);
        EXPECT_TRUE(!node.repair_timing);
        destroy();
    }
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    create(&opts, -1);
    EXPECT_EQ_U32(tt_SWITCH_ON, node.opts.net.links[0].repair_timing);
    EXPECT_TRUE(node.repair_timing);
    destroy();
    opts.net.links[0].repair_timing = tt_SWITCH_OFF;
    create(&opts, -1);
    EXPECT_TRUE(!node.repair_timing);
    destroy();
    opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[0].repair_timing = tt_SWITCH_ON;
    create(&opts, -1);
    EXPECT_TRUE(node.repair_timing);
    destroy();
    // Two links, WIFI second or first: on. No links at all: off (a context with no link options is ETHERNET's).
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255", .profile = tt_LINK_PROFILE_ETHERNET};
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .profile = tt_LINK_PROFILE_WIFI};
    opts.net.link_count = 2;
    create(&opts, -1);
    EXPECT_TRUE(node.repair_timing);
    destroy();
    opts.net.links[0].profile = tt_LINK_PROFILE_WIFI; // and first
    opts.net.links[1].profile = tt_LINK_PROFILE_ETHERNET;
    create(&opts, -1);
    EXPECT_TRUE(node.repair_timing);
    destroy();
    memset(&opts, 0, sizeof(opts));
    create(&opts, -1);
    EXPECT_TRUE(!node.repair_timing);
    destroy();
}

// The readers' re-ask on a repair above the watermark (tt_LinkOptions.repair_reveal, 2026-10-30): ALL_MISSING on WIFI
// (2026-10-31), OFF on every other profile; an explicit mode wins; the context takes the most any link asks, and the
// longest pace any link sets (0 = the least repair transit). What it does is test_reliable_pubsub's and
// test_large_sample's.
static void test_repair_reveal_link_option(void) {
    static const uint8_t profiles[] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_T1S, tt_LINK_PROFILE_TSN,
                                       tt_LINK_PROFILE_WIFI};
    for (size_t i = 0; i < sizeof(profiles); i++) {
        struct tt_InitOptions opts = one_link(profiles[i]);
        uint32_t expected = profiles[i] == tt_LINK_PROFILE_WIFI ? tt_REPAIR_REVEAL_ALL_MISSING : tt_REPAIR_REVEAL_OFF;
        create(&opts, -1);
        EXPECT_EQ_U32(expected, node.opts.net.links[0].repair_reveal);
        EXPECT_EQ_U32(expected, node.repair_reveal);
        EXPECT_EQ_U32(0, node.repair_reveal_pace_ns);
        destroy();
        opts.net.links[0].repair_reveal = tt_REPAIR_REVEAL_REVEALED;
        opts.net.links[0].repair_reveal_pace_ns = 3 * tt_MILLISECOND;
        create(&opts, -1);
        EXPECT_EQ_U32(tt_REPAIR_REVEAL_REVEALED, node.repair_reveal);
        EXPECT_EQ_U32(3 * tt_MILLISECOND, node.repair_reveal_pace_ns);
        destroy();
    }
    // Two links: the most any asks, either order; the longest pace.
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255",
                                                 .repair_reveal = tt_REPAIR_REVEAL_REVEALED,
                                                 .repair_reveal_pace_ns = 5 * tt_MILLISECOND};
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255",
                                                 .profile = tt_LINK_PROFILE_WIFI,
                                                 .repair_reveal = tt_REPAIR_REVEAL_OFF,
                                                 .repair_reveal_pace_ns = 2 * tt_MILLISECOND};
    opts.net.link_count = 2;
    create(&opts, -1);
    EXPECT_EQ_U32(tt_REPAIR_REVEAL_REVEALED, node.repair_reveal);
    EXPECT_EQ_U32(5 * tt_MILLISECOND, node.repair_reveal_pace_ns);
    destroy();
    opts.net.links[0].repair_reveal = tt_REPAIR_REVEAL_DEFAULT;
    opts.net.links[1].repair_reveal = tt_REPAIR_REVEAL_ALL_MISSING;
    create(&opts, -1);
    EXPECT_EQ_U32(tt_REPAIR_REVEAL_ALL_MISSING, node.repair_reveal);
    destroy();
    // Both left to their profiles: WIFI's ALL_MISSING is the context's, the Ethernet link's readers included.
    opts.net.links[1].repair_reveal = tt_REPAIR_REVEAL_DEFAULT;
    create(&opts, -1);
    EXPECT_EQ_U32(tt_REPAIR_REVEAL_OFF, node.opts.net.links[0].repair_reveal);
    EXPECT_EQ_U32(tt_REPAIR_REVEAL_ALL_MISSING, node.opts.net.links[1].repair_reveal);
    EXPECT_EQ_U32(tt_REPAIR_REVEAL_ALL_MISSING, node.repair_reveal);
    destroy();
    memset(&opts, 0, sizeof(opts)); // no link options: none
    create(&opts, -1);
    EXPECT_TRUE(node.repair_reveal < tt_REPAIR_REVEAL_REVEALED);
    destroy();
}

// The unicast copies: what went to which address, and the copy's bytes.
static int copy_count;
static uint32_t copy_ips[32];
static uint8_t copy_miss;
static int copy_heartbeats;

static void capture_copy(const void* buf, size_t len) {
    static uint8_t classic[tt_MAX_BUFFER_LENGTH * 2];
    if (test_mock_send_hook_ip == 0) {
        return; // a broadcast
    }
    if (copy_count < (int)(sizeof(copy_ips) / sizeof(copy_ips[0]))) {
        copy_ips[copy_count] = test_mock_send_hook_ip;
    }
    copy_count++;
    size_t classic_len = test_classic_form(buf, len, classic);
    copy_heartbeats = heartbeats_in(classic, classic_len);
    const struct tt_HeartbeatHeader* hb =
        (const struct tt_HeartbeatHeader*)(classic + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    copy_miss = classic_len >= SUMMARY_DATAGRAM_LENGTH && hb->endpoint_id == tt_DISCOVERY_ENDPOINT_ID
                    ? hb->summary_miss_threshold
                    : 0xEEU;
}

static void start_copies(void) {
    copy_count = 0;
    copy_heartbeats = 0;
    copy_miss = 0;
    memset(copy_ips, 0, sizeof(copy_ips));
    test_mock_send_hook = capture_copy;
}

// A peer as discovery makes it: its list applied (update_seen) and its summary heard from ip:PORT.
static void know_peer(uint8_t id, uint32_t ip) {
    node.update_seen[id] = true;
    EXPECT_TRUE(process_discovery_summary(&node, id, node.update_generation[id], ip, PORT));
}

static void test_wifi_summary_copies(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    create(&opts, -1);
    know_peer(PEER_ID, PEER_IP);
    uint64_t t = test_mock_now;
    // From the summary tick: one copy, to the peer, a summary carrying WIFI's threshold.
    start_copies();
    node_update(&node, t, NULL);
    EXPECT_EQ_INT(1, copy_count);
    EXPECT_EQ_U32(PEER_IP, copy_ips[0]);
    EXPECT_EQ_INT(1, copy_heartbeats);
    EXPECT_EQ_U32(tt_LINK_WIFI_MISS_THRESHOLD, copy_miss);
    EXPECT_EQ_U64(1, node.summary_copies_sent);
    // One round per update interval.
    start_copies();
    send_summary_copies(&node, t + (tt_CONTEXT_UPDATE_INTERVAL / 2U));
    EXPECT_EQ_INT(0, copy_count);
    send_summary_copies(&node, t + tt_CONTEXT_UPDATE_INTERVAL);
    EXPECT_EQ_INT(1, copy_count);
    t += tt_CONTEXT_UPDATE_INTERVAL;
    // A peer whose list is not applied, or whose summaries were never heard, has none.
    node.update_seen[PEER_ID + 1] = false;
    EXPECT_TRUE(process_discovery_summary(&node, PEER_ID + 1, 1, PEER_IP + 1, PORT));
    node.update_seen[PEER_ID + 2] = true; // known by its announce alone
    t += tt_CONTEXT_UPDATE_INTERVAL;
    start_copies();
    send_summary_copies(&node, t);
    EXPECT_EQ_INT(1, copy_count);
    EXPECT_EQ_U32(PEER_IP, copy_ips[0]);
    // Presumed dead: none, even once it is known by an announce again, until a summary says where it is.
    presume_node_dead(&node, PEER_ID, 4 * tt_SECOND);
    node.update_seen[PEER_ID] = true;
    t += tt_CONTEXT_UPDATE_INTERVAL;
    start_copies();
    send_summary_copies(&node, t);
    EXPECT_EQ_INT(0, copy_count);
    destroy();

    // Past LINK_SUMMARY_COPIES_MAX peers a round sends that many, and the next starts where it stopped.
    create(&opts, -1);
    for (uint8_t i = 0; i < 20; i++) {
        know_peer((uint8_t)(PEER_ID + i), PEER_IP + i);
    }
    t = test_mock_now;
    start_copies();
    send_summary_copies(&node, t);
    EXPECT_EQ_INT((int)LINK_SUMMARY_COPIES_MAX, copy_count);
    EXPECT_EQ_U32(PEER_IP, copy_ips[0]);
    start_copies();
    send_summary_copies(&node, t + tt_CONTEXT_UPDATE_INTERVAL);
    EXPECT_EQ_INT((int)LINK_SUMMARY_COPIES_MAX, copy_count);
    EXPECT_EQ_U32(PEER_IP + LINK_SUMMARY_COPIES_MAX, copy_ips[0]);  // the 17th
    EXPECT_EQ_U32(PEER_IP, copy_ips[20 - LINK_SUMMARY_COPIES_MAX]); // then from the first again
    destroy();

    // ETHERNET sends none.
    opts = one_link(tt_LINK_PROFILE_ETHERNET);
    create(&opts, -1);
    know_peer(PEER_ID, PEER_IP);
    start_copies();
    node_update(&node, test_mock_now, NULL);
    EXPECT_EQ_INT(0, copy_count);
    EXPECT_EQ_U64(0, node.summary_copy_ns); // no round taken at all: no walk of the peers a second
    destroy();

    // A peer on a link that takes no copies has none: WIFI beside ETHERNET, the peer on ETHERNET's subnet (the mock
    // resolves every link to one subnet; link 1, WIFI, is moved to 10.0.1.0/24 here).
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255", .profile = tt_LINK_PROFILE_ETHERNET};
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .profile = tt_LINK_PROFILE_WIFI};
    opts.net.link_count = 2;
    create(&opts, -1);
    node.links[1].resolved_addr = 0x0a000107U;
    know_peer(PEER_ID, PEER_IP);         // 10.0.0.9: the ETHERNET link
    know_peer(PEER_ID + 1, 0x0a000109U); // 10.0.1.9: the WIFI link
    start_copies();
    send_summary_copies(&node, test_mock_now);
    EXPECT_EQ_INT(1, copy_count);
    EXPECT_EQ_U32(0x0a000109U, copy_ips[0]);
    destroy();
}

// Runs the context's scheduler up to `until`, the mock clock following each entry.
static void run_until(uint64_t until) {
    for (;;) {
        bool has_next = false;
        uint64_t next = 0;
        while (run_due_entry(&node, test_mock_now, &has_next, &next)) {
        }
        if (!has_next || next > until) {
            test_mock_now = until;
            return;
        }
        test_mock_now = next > test_mock_now ? next : test_mock_now;
    }
}

// The broadcasts the context sent: announces, summaries and the repeats.
static int broadcasts(void) {
    return test_mock_send_call_count - test_mock_send_to_call_count;
}

static void test_wifi_announce_repeats(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    create(&opts, -1);
    run_until(test_mock_now + (2 * tt_SECOND)); // whatever the creation announced, and its repeats
    // A change: its announce once the tick is quiet, then the repeats at 100, 300 and 700 ms after it.
    uint64_t sent = node.announce_repeats_sent;
    create_publisher(&pubs[0], "changes_the_list", &TEST_QOS_UNRELIABLE, NULL);
    uint64_t change = test_mock_now + tt_CONTEXT_TX_INTERVAL; // announce_soon: the change's broadcast, once quiet
    start_capture();
    run_until(change + (99 * tt_MILLISECOND));
    EXPECT_EQ_U64(sent, node.announce_repeats_sent);
    int before = broadcasts();
    run_until(change + (101 * tt_MILLISECOND));
    run_until(change + (103 * tt_MILLISECOND)); // its flush tick
    EXPECT_EQ_U64(sent + 1U, node.announce_repeats_sent);
    EXPECT_TRUE(broadcasts() > before); // a broadcast went
    run_until(change + (299 * tt_MILLISECOND));
    EXPECT_EQ_U64(sent + 1U, node.announce_repeats_sent);
    run_until(change + (301 * tt_MILLISECOND));
    EXPECT_EQ_U64(sent + 2U, node.announce_repeats_sent);
    run_until(change + (699 * tt_MILLISECOND));
    EXPECT_EQ_U64(sent + 2U, node.announce_repeats_sent);
    run_until(change + (701 * tt_MILLISECOND));
    EXPECT_EQ_U64(sent + 3U, node.announce_repeats_sent);
    run_until(change + (3 * tt_SECOND));
    EXPECT_EQ_U64(sent + 3U, node.announce_repeats_sent);
    // A change while they run starts them again, for the newer list: the first chain's 300 ms repeat does not go.
    sent = node.announce_repeats_sent;
    create_publisher(&pubs[1], "changes_it_again", &TEST_QOS_UNRELIABLE, NULL);
    change = test_mock_now + tt_CONTEXT_TX_INTERVAL;
    run_until(change + (101 * tt_MILLISECOND));
    EXPECT_EQ_U64(sent + 1U, node.announce_repeats_sent);
    create_publisher(&pubs[2], "and_again", &TEST_QOS_UNRELIABLE, NULL);
    run_until(change + (104 * tt_MILLISECOND)); // the second change's announce; its repeats from ~204 ms
    run_until(change + (301 * tt_MILLISECOND));
    EXPECT_EQ_U64(sent + 2U, node.announce_repeats_sent); // the second chain's first, not the first chain's second
    run_until(change + (3 * tt_SECOND));
    EXPECT_EQ_U64(sent + 1U + tt_LINK_WIFI_ANNOUNCE_REPEATS, node.announce_repeats_sent);
    destroy();

    // ETHERNET repeats nothing and schedules nothing for it.
    opts = one_link(tt_LINK_PROFILE_ETHERNET);
    create(&opts, -1);
    create_publisher(&pubs[0], "changes_the_list", &TEST_QOS_UNRELIABLE, NULL);
    run_until(test_mock_now + (5 * tt_MILLISECOND));
    EXPECT_TRUE(!tt_Context_unschedule(&node, announce_repeat, NULL));
    run_until(test_mock_now + (3 * tt_SECOND));
    EXPECT_EQ_U64(0, node.announce_repeats_sent);
    destroy();
}

// ---- The checks.

static void expect_created(const struct tt_InitOptions* opts, tt_ret_t expected, const char* what) {
    reset_mock_for_create(-1);
    struct tt_Context created;
    memset(&created, 0, sizeof(created));
    tt_ret_t ret = tt_Context_create_with(&created, opts);
    if (ret != expected) {
        fprintf(stderr, "%s: returned %d, expected %d\n", what, (int)ret, (int)expected);
        test_failures++;
    }
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));
    }
}

static void test_validation(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_WIFI);
    expect_created(&opts, tt_RET_OK, "profile WIFI");
    opts.net.links[0].profile = tt_LINK_PROFILE_WIFI + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "profile past WIFI");

    opts = one_link(0);
    opts.net.links[0].batch = tt_SWITCH_OFF;
    expect_created(&opts, tt_RET_OK, "batch tt_SWITCH_OFF");
    opts.net.links[0].batch = tt_SWITCH_OFF + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "batch past tt_SWITCH_OFF");

    opts = one_link(0);
    opts.net.links[0].large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    expect_created(&opts, tt_RET_OK, "large_fragmentation IP");
    // Past IP on one of two links: no agreement, so nothing carries it into net.large_fragmentation's own check.
    opts.net.links[1] =
        (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .large_fragmentation = tt_LARGE_FRAGMENTATION_IP + 1U};
    opts.net.link_count = 2;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "large_fragmentation past IP");

    opts = one_link(0);
    opts.net.links[0].mtu = tt_LINK_MTU_MIN;
    expect_created(&opts, tt_RET_OK, "mtu tt_LINK_MTU_MIN");
    opts.net.links[0].mtu = tt_LINK_MTU_MIN - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "mtu below tt_LINK_MTU_MIN");

    opts = one_link(0);
    opts.net.links[0].link_rate = tt_LINK_RATE_MIN;
    expect_created(&opts, tt_RET_OK, "link_rate tt_LINK_RATE_MIN");
    opts.net.links[0].link_rate = tt_LINK_RATE_MIN - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "link_rate below tt_LINK_RATE_MIN");

    opts = one_link(0);
    opts.net.links[0].retry_floor_ns = UINT32_MAX;
    expect_created(&opts, tt_RET_OK, "retry_floor_ns UINT32_MAX");
    opts.net.links[0].retry_floor_ns = (uint64_t)UINT32_MAX + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "retry_floor_ns above UINT32_MAX");

    opts = one_link(0);
    opts.net.links[0].piggyback_divisor = tt_RELIABLE_BITMAP_BITS;
    expect_created(&opts, tt_RET_OK, "piggyback_divisor tt_RELIABLE_BITMAP_BITS");
    opts.net.links[0].piggyback_divisor = tt_RELIABLE_BITMAP_BITS + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "piggyback_divisor above tt_RELIABLE_BITMAP_BITS");

    opts = one_link(0);
    opts.net.links[0].summary_unicast = tt_SWITCH_OFF;
    expect_created(&opts, tt_RET_OK, "summary_unicast tt_SWITCH_OFF");
    opts.net.links[0].summary_unicast = tt_SWITCH_OFF + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "summary_unicast past tt_SWITCH_OFF");
    opts.net.links[0].summary_unicast = tt_SWITCH_DEFAULT;
    opts.net.links[0].repair_timing = tt_SWITCH_OFF;
    expect_created(&opts, tt_RET_OK, "repair_timing tt_SWITCH_OFF");
    opts.net.links[0].repair_timing = tt_SWITCH_OFF + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "repair_timing past tt_SWITCH_OFF");
    opts.net.links[0].repair_timing = tt_SWITCH_DEFAULT;
    opts.net.links[0].repair_reveal = tt_REPAIR_REVEAL_ALL_MISSING;
    expect_created(&opts, tt_RET_OK, "repair_reveal tt_REPAIR_REVEAL_ALL_MISSING");
    opts.net.links[0].repair_reveal = tt_REPAIR_REVEAL_ALL_MISSING + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "repair_reveal past tt_REPAIR_REVEAL_ALL_MISSING");
    opts.net.links[0].repair_reveal = tt_REPAIR_REVEAL_DEFAULT;
    opts.net.links[0].repair_reveal_pace_ns = UINT32_MAX;
    expect_created(&opts, tt_RET_OK, "repair_reveal_pace_ns UINT32_MAX");
    opts.net.links[0].repair_reveal_pace_ns = (uint64_t)UINT32_MAX + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "repair_reveal_pace_ns above UINT32_MAX");

    opts = one_link(0);
    opts.net.links[0].announce_repeats = tt_ANNOUNCE_REPEATS_MAX;
    expect_created(&opts, tt_RET_OK, "announce_repeats tt_ANNOUNCE_REPEATS_MAX");
    opts.net.links[0].announce_repeats = tt_ANNOUNCE_REPEATS_NONE;
    expect_created(&opts, tt_RET_OK, "announce_repeats tt_ANNOUNCE_REPEATS_NONE");
    opts.net.links[0].announce_repeats = tt_ANNOUNCE_REPEATS_MAX + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "announce_repeats above tt_ANNOUNCE_REPEATS_MAX");

    // A link value against the context's own: refused when both are set and differ, taken when equal or when the
    // context's is its default.
    opts = one_link(0);
    opts.net.links[0].batch_tick_ns = 2 * tt_MILLISECOND;
    opts.discovery.tx_interval_ns = 3 * tt_MILLISECOND;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "batch_tick_ns against discovery.tx_interval_ns");
    opts.discovery.tx_interval_ns = 2 * tt_MILLISECOND;
    expect_created(&opts, tt_RET_OK, "batch_tick_ns equal to discovery.tx_interval_ns");
    opts.discovery.tx_interval_ns = tt_CONTEXT_TX_INTERVAL;
    expect_created(&opts, tt_RET_OK, "batch_tick_ns against the default discovery.tx_interval_ns");

    opts = one_link(0);
    opts.net.links[0].idle_heartbeat_ns = 2 * tt_SECOND;
    opts.reliable.idle_heartbeat_period = 3 * tt_SECOND;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "idle_heartbeat_ns against reliable.idle_heartbeat_period");
    opts.reliable.idle_heartbeat_period = 2 * tt_SECOND;
    expect_created(&opts, tt_RET_OK, "idle_heartbeat_ns equal to reliable.idle_heartbeat_period");

    opts = one_link(0);
    opts.net.links[0].large_fragmentation = tt_LARGE_FRAGMENTATION_MTU;
    opts.net.large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "large_fragmentation against net.large_fragmentation");
    opts.net.large_fragmentation = tt_LARGE_FRAGMENTATION_MTU;
    expect_created(&opts, tt_RET_OK, "large_fragmentation equal to net.large_fragmentation");
    // The context's own set where a profile's default would differ: the context's wins, no refusal.
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    create(&opts, -1);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_IP, node.opts.net.large_fragmentation);
    destroy();

    // A Publisher's batching.
    opts = one_link(0);
    create(&opts, -1);
    struct tt_PublisherOptions bad = {.batching = tt_SWITCH_OFF + 1U};
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Node_create_publisher(tt_Context_default_node(&node), &pubs[0], &topic,
                                                                    "bad", &TEST_QOS_UNRELIABLE, &bad, NULL, 0));
    struct tt_PublisherOptions good = {.batching = tt_SWITCH_OFF};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node), &pubs[0], &topic, "good",
                                                      &TEST_QOS_UNRELIABLE, &good, NULL, 0));
    destroy();
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_ethernet_wifi_and_no_links_keep_the_defaults();
    test_t1s_fills_its_link_and_the_context();
    test_tsn_takes_the_interface_mtu_and_ethernet_values();
    test_several_links_combine();
    test_mtu_caps_a_batch_and_the_announce();
    test_unicast_threshold_t1s_broadcasts_to_one_peer();
    test_unicast_threshold_on_the_zero_copy_path();
    test_batch_is_the_publishers_default();
    test_piggyback_divisor_piggybacks_heartbeats();
    test_retry_floor_holds_up_the_measured_g();
    test_tick_idle_heartbeat_and_mode_set_the_context();
    test_wifi_discovery_values();
    test_wifi_repair_timing();
    test_repair_reveal_link_option();
    test_wifi_summary_copies();
    test_wifi_announce_repeats();
    test_validation();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_link_profiles: all tests passed\n");
    return 0;
}
