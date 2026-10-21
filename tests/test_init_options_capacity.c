/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions, bundle 9: the capacity group (struct tt_Capacity), through the mock HAL.
//   - the defaults are the compiled sizes of the tables, and a context created with the options holds them resolved;
//   - every field below its default makes its table full at that limit and not at the compiled size - the control is
//     the same scenario at the default (a context built by hand, its options 0, which node_init_locks() resolves):
//       ack_entries       the RELIABLE readers a Publisher matches,
//       peers             the peers a Publisher learns from announces and from ACKNACKs, a Client from announces, and
//                         the writers a Subscriber tracks,
//       request_history   the ACKNACK requests a reader remembers per writer;
//     publisher_claims is tests/test_publish_claim.c's and large_retained tests/test_large_sample.c's, which have the
//     shared-memory ring and the large-sample link they need;
//   - every check refuses the value past its bound with tt_RET_INVALID_ARGUMENT and accepts the bound itself, and
//     capacity.peers must be above every link's unicast threshold
//     (tests/mutants_init_options_capacity.sh removes each check and each reader and expects a test to fail).

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_capacity.sh passes a mutant

#define NODE_ID 1
#define FIRST_REMOTE 10 // remote contexts FIRST_REMOTE, FIRST_REMOTE + 1, ...
#define REMOTE_IP(i) (0x0a000000U + (uint32_t)(i))
#define PORT 8282
#define TOPIC_NAME "capacity_topic"
#define ENDPOINT_NAME "capacity_endpoint"
#define SUB_ENTITY(i) (0x22220000U + (uint32_t)(i))
#define PUB_ENTITY(i) (0x11110000U + (uint32_t)(i))

// The limits shown: each below its compiled size, peers above the default link's unicast threshold.
#define LIMIT_ACK_ENTRIES 2U
#define LIMIT_PEERS 3U
#define LIMIT_REQUEST_HISTORY 4U
static_assert(LIMIT_ACK_ENTRIES + 1U < tt_MAX_ACK_ENTRIES, "the default must take what the limit refuses");
static_assert(LIMIT_PEERS + 1U < tt_MAX_PEER_COUNT, "the default must take what the limit refuses");
static_assert(LIMIT_PEERS > tt_UNICAST_PEER_THRESHOLD, "a peers limit must be above the default link's threshold");
static_assert(LIMIT_REQUEST_HISTORY + 2U <= tt_RELIABLE_REQUEST_HISTORY, "the default must hold what the limit drops");

static struct tt_Context node;
static struct tt_TypeSupport topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static uint64_t pub_storage[(size_t)64U * 1024U / sizeof(uint64_t)];
static uint64_t sub_storage[(size_t)256U * 1024U / sizeof(uint64_t)];

static int32_t u32_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}

static int32_t u32_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static int32_t u32_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)seq_no;
    (void)data;
}

// For a creation: the mock's address auto-detection answers an id, and every link resolves as an interface would.
static void reset_mock_for_create(void) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
}

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts;
}

// A context built by hand, its options 0 (node_init_locks() resolves them), then `capacity`, resolved, when not NULL.
static void setup(const struct tt_Capacity* capacity) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = NODE_ID;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = sizeof(node.tx_buffer);
    if (capacity != NULL) {
        node.opts.capacity = *capacity;
        resolve_capacity_options(&node.opts.capacity); // as a creation would: the fields not given at their default
    }
    memset(&topic, 0, sizeof(topic));
    topic.name = TOPIC_NAME;
    topic.data_size = sizeof(uint32_t);
    topic.data_encode_size = u32_encode_size;
    topic.data_encode = u32_encode;
    topic.data_decode = u32_decode;
    topic.data_free = free_nothing;
    topic.max_encoded_size = sizeof(uint32_t);
}

static void create_publisher(bool reliable) {
    struct tt_QosProfile qos = TEST_QOS_UNRELIABLE;
    if (reliable) {
        qos.reliability.kind = tt_QOS_RELIABILITY_RELIABLE;
    }
    size_t size = tt_Publisher_storage_size(&topic, &qos);
    EXPECT_TRUE(size <= sizeof(pub_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node), &pub, &topic, ENDPOINT_NAME, &qos,
                                                      NULL, size != 0 ? pub_storage : NULL, size));
}

static void create_reliable_subscriber(void) {
    struct tt_QosProfile qos = TEST_QOS_RELIABLE_READER;
    size_t size = tt_Subscriber_storage_size(&topic, &qos);
    EXPECT_TRUE(size <= sizeof(sub_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&node), &sub, &topic, ENDPOINT_NAME,
                                                       &qos, &(struct tt_SubscriberOptions) {.callback = on_data},
                                                       size != 0 ? sub_storage : NULL, size));
}

static void teardown(void) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
}

static uint32_t peers_in(const struct tt_Peer* peers) {
    return count_peers(peers);
}

// What remote context `i`'s announce of a matching endpoint hands register_*_peer_*() (decode_update_entities()).
static struct update_peer_ctx announce_of(struct tt_Header* header, uint8_t i, uint16_t qos) {
    header->source = (uint8_t)(FIRST_REMOTE + i);
    struct update_peer_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.header = header;
    ctx.sender_ip = REMOTE_IP(i);
    ctx.sender_port = PORT;
    ctx.qos = qos;
    ctx.entity_id = SUB_ENTITY(i);
    ctx.tracking_words = 1;
    ctx.announce_generation = 1;
    return ctx;
}

// ---- The defaults.

static void test_defaults_are_the_compiled_sizes(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32(tt_MAX_ACK_ENTRIES, opts.capacity.ack_entries);
    EXPECT_EQ_U32(tt_MAX_PEER_COUNT, opts.capacity.peers);
    EXPECT_EQ_U32(tt_RELIABLE_REQUEST_HISTORY, opts.capacity.request_history);
    EXPECT_EQ_U32(tt_PUBLISHER_CLAIMS, opts.capacity.publisher_claims);
    EXPECT_EQ_U32(tt_LARGE_RETAINED, opts.capacity.large_retained);

    // Created with options: the context holds them, resolved, and the default where a value was not given.
    reset_mock_for_create();
    struct tt_InitOptions given;
    memset(&given, 0, sizeof(given));
    given.capacity.peers = LIMIT_PEERS;
    given.capacity.request_history = LIMIT_REQUEST_HISTORY;
    struct tt_Context created;
    memset(&created, 0, sizeof(created));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&created, &given));
    EXPECT_EQ_U32(tt_MAX_ACK_ENTRIES, created.opts.capacity.ack_entries);
    EXPECT_EQ_U32(LIMIT_PEERS, created.opts.capacity.peers);
    EXPECT_EQ_U32(LIMIT_REQUEST_HISTORY, created.opts.capacity.request_history);
    EXPECT_EQ_U32(tt_PUBLISHER_CLAIMS, created.opts.capacity.publisher_claims);
    EXPECT_EQ_U32(tt_LARGE_RETAINED, created.opts.capacity.large_retained);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));

    // A context built by hand: node_init_locks() resolves the zeroed group.
    setup(NULL);
    EXPECT_EQ_U32(tt_MAX_ACK_ENTRIES, node.opts.capacity.ack_entries);
    EXPECT_EQ_U32(tt_MAX_PEER_COUNT, node.opts.capacity.peers);
    EXPECT_EQ_U32(tt_RELIABLE_REQUEST_HISTORY, node.opts.capacity.request_history);
    teardown();
}

// ---- ack_entries: RELIABLE Subscribers announced to a RELIABLE Publisher, one more than the limit.

static uint32_t readers_matched(const struct tt_Capacity* capacity) {
    setup(capacity);
    create_publisher(true);
    struct tt_Header header = {0};
    for (uint8_t i = 0; i <= LIMIT_ACK_ENTRIES; i++) {
        struct update_peer_ctx ctx = announce_of(&header, i, tt_UPDATE_QOS_RELIABLE);
        register_subscriber_peer_on_publisher(&node, (struct tt_Endpoint*)&pub, &ctx);
    }
    uint32_t matched = 0;
    for (int i = 0; i < tt_MAX_ACK_ENTRIES; i++) {
        matched += pub.peer_acks[i].context_id != tt_CONTEXT_ID_INVALID ? 1U : 0U;
    }
    // A reader refused its ack entry is not matched at all: not a peer either.
    EXPECT_EQ_U32(matched, peers_in(pub.peers));
    teardown();
    return matched;
}

static void test_ack_entries_is_the_readers_matched(void) {
    struct tt_Capacity limited = {.ack_entries = LIMIT_ACK_ENTRIES};
    EXPECT_EQ_U32(LIMIT_ACK_ENTRIES + 1U, readers_matched(NULL));
    EXPECT_EQ_U32(LIMIT_ACK_ENTRIES, readers_matched(&limited));
}

// ---- peers: one more remote context than the limit, by each way a peer is added.

enum peer_way { BY_ANNOUNCE, BY_ACKNACK, BY_SERVER_ANNOUNCE, BY_WRITER_DATA };

static uint32_t peers_added(const struct tt_Capacity* capacity, enum peer_way way) {
    setup(capacity);
    struct tt_Client client;
    memset(&client, 0, sizeof(client)); // every peers[] slot tt_CONTEXT_ID_INVALID (0)
    if (way == BY_WRITER_DATA) {
        create_reliable_subscriber();
    } else if (way != BY_SERVER_ANNOUNCE) {
        create_publisher(way == BY_ACKNACK);
    }
    struct tt_Header header = {0};
    uint32_t added = 0;
    for (uint8_t i = 0; i <= LIMIT_PEERS; i++) {
        struct update_peer_ctx ctx = announce_of(&header, i, 0);
        switch (way) {
        case BY_ANNOUNCE:
            register_subscriber_peer_on_publisher(&node, (struct tt_Endpoint*)&pub, &ctx);
            break;
        case BY_ACKNACK:
            claim_from_acknack(&node, &pub, header.source, ((struct tt_Endpoint*)&pub)->id, SUB_ENTITY(i), REMOTE_IP(i),
                               PORT);
            break;
        case BY_SERVER_ANNOUNCE:
            register_server_peer_on_client(&node, (struct tt_Endpoint*)&client, &ctx);
            break;
        case BY_WRITER_DATA:
            added += find_or_create_writer_proxy(&sub, header.source, PUB_ENTITY(i), NULL) != NULL ? 1U : 0U;
            break;
        }
    }
    if (way == BY_ANNOUNCE || way == BY_ACKNACK) {
        added = peers_in(pub.peers);
        // A peer already in a full table is still refreshed: its address follows it.
        struct update_peer_ctx again = announce_of(&header, 0, 0);
        again.sender_ip = REMOTE_IP(100);
        register_subscriber_peer_on_publisher(&node, (struct tt_Endpoint*)&pub, &again);
        EXPECT_EQ_U32(REMOTE_IP(100), pub.peers[0].ip);
    } else if (way == BY_SERVER_ANNOUNCE) {
        added = peers_in(client.peers);
    }
    teardown();
    return added;
}

static void test_peers_is_the_peers_an_endpoint_keeps(void) {
    struct tt_Capacity limited = {.peers = LIMIT_PEERS};
    const enum peer_way ways[] = {BY_ANNOUNCE, BY_ACKNACK, BY_SERVER_ANNOUNCE, BY_WRITER_DATA};
    const char* names[] = {"announce", "ACKNACK", "server announce", "writer data"};
    for (size_t w = 0; w < sizeof(ways) / sizeof(ways[0]); w++) {
        uint32_t standard = peers_added(NULL, ways[w]);
        uint32_t fewer = peers_added(&limited, ways[w]);
        if (standard != LIMIT_PEERS + 1U || fewer != LIMIT_PEERS) {
            fprintf(stderr, "peers by %s: %u at the default (want %u), %u at the limit (want %u)\n", names[w],
                    (unsigned)standard, LIMIT_PEERS + 1U, (unsigned)fewer, LIMIT_PEERS);
            test_failures++;
        }
    }
}

// ---- request_history: more ACKNACK requests than the limit, each remembered until the ring comes round.

static uint32_t requests_remembered(const struct tt_Capacity* capacity) {
    setup(capacity);
    create_reliable_subscriber();
    struct tt_WriterProxy* proxy = find_or_create_writer_proxy(&sub, FIRST_REMOTE, PUB_ENTITY(0), NULL);
    EXPECT_TRUE(proxy != NULL);
    uint32_t remembered = 0;
    if (proxy != NULL) {
        for (uint32_t n = 0; n < LIMIT_REQUEST_HISTORY + 2U; n++) {
            note_requested(&node, proxy, 10U * n, (10U * n) + 5U, tt_SECOND + n);
        }
        for (int slot = 0; slot < tt_RELIABLE_REQUEST_HISTORY; slot++) {
            remembered += proxy->requests[slot].sent_ns != 0 ? 1U : 0U;
        }
        // The oldest are the ones gone: the newest request is always remembered.
        bool newest = false;
        for (int slot = 0; slot < tt_RELIABLE_REQUEST_HISTORY; slot++) {
            newest = newest || proxy->requests[slot].first_seq_no == 10U * (LIMIT_REQUEST_HISTORY + 1U);
        }
        EXPECT_TRUE(newest);
    }
    teardown();
    return remembered;
}

static void test_request_history_is_the_requests_remembered(void) {
    struct tt_Capacity limited = {.request_history = LIMIT_REQUEST_HISTORY};
    EXPECT_EQ_U32(LIMIT_REQUEST_HISTORY + 2U, requests_remembered(NULL));
    EXPECT_EQ_U32(LIMIT_REQUEST_HISTORY, requests_remembered(&limited));
}

// ---- The checks: each bound refused one past it and accepted at it.

static void expect_created(const struct tt_InitOptions* opts, tt_ret_t expected, const char* what) {
    reset_mock_for_create();
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
    struct tt_InitOptions opts = defaults();
    opts.capacity.ack_entries = tt_MAX_ACK_ENTRIES + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "capacity.ack_entries above tt_MAX_ACK_ENTRIES");
    opts.capacity.ack_entries = tt_MAX_ACK_ENTRIES;
    expect_created(&opts, tt_RET_OK, "capacity.ack_entries at tt_MAX_ACK_ENTRIES");

    opts = defaults();
    opts.capacity.peers = tt_MAX_PEER_COUNT + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "capacity.peers above tt_MAX_PEER_COUNT");
    opts.capacity.peers = tt_MAX_PEER_COUNT;
    expect_created(&opts, tt_RET_OK, "capacity.peers at tt_MAX_PEER_COUNT");

    // Above the unicast threshold: the default link's, then the highest of two links', then a link that always
    // broadcasts (a threshold of no peers), where one peer is enough.
    opts = defaults();
    opts.capacity.peers = tt_UNICAST_PEER_THRESHOLD;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "capacity.peers at the default link's threshold");
    opts.capacity.peers = tt_UNICAST_PEER_THRESHOLD + 1U;
    expect_created(&opts, tt_RET_OK, "capacity.peers above the default link's threshold");
    opts = defaults();
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255", .unicast_threshold = 1};
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .unicast_threshold = 4};
    opts.net.link_count = 2;
    opts.capacity.peers = 4;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "capacity.peers at the second link's threshold");
    opts.capacity.peers = 5;
    expect_created(&opts, tt_RET_OK, "capacity.peers above both links' thresholds");
    opts = defaults();
    opts.net.links[0] =
        (struct tt_LinkOptions) {.broadcast = "10.0.0.255", .unicast_threshold = tt_UNICAST_ALWAYS_BROADCAST};
    opts.net.link_count = 1;
    opts.capacity.peers = 1;
    expect_created(&opts, tt_RET_OK, "capacity.peers 1 on a link that always broadcasts");

    opts = defaults();
    opts.capacity.request_history = tt_RELIABLE_REQUEST_HISTORY + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "capacity.request_history above tt_RELIABLE_REQUEST_HISTORY");
    opts.capacity.request_history = tt_RELIABLE_REQUEST_HISTORY;
    expect_created(&opts, tt_RET_OK, "capacity.request_history at tt_RELIABLE_REQUEST_HISTORY");

    opts = defaults();
    opts.capacity.publisher_claims = tt_PUBLISHER_CLAIMS + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "capacity.publisher_claims above tt_PUBLISHER_CLAIMS");
    opts.capacity.publisher_claims = tt_PUBLISHER_CLAIMS;
    expect_created(&opts, tt_RET_OK, "capacity.publisher_claims at tt_PUBLISHER_CLAIMS");

    opts = defaults();
    opts.capacity.large_retained = tt_LARGE_RETAINED + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "capacity.large_retained above tt_LARGE_RETAINED");
    opts.capacity.large_retained = tt_LARGE_RETAINED;
    expect_created(&opts, tt_RET_OK, "capacity.large_retained at tt_LARGE_RETAINED");
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_defaults_are_the_compiled_sizes();
    if (test_result() != 0) {
        return 1; // the rest builds contexts by hand: unresolved, a table's limit is 0
    }
    test_ack_entries_is_the_readers_matched();
    test_peers_is_the_peers_an_endpoint_keeps();
    test_request_history_is_the_requests_remembered();
    test_validation();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_capacity: all tests passed\n");
    return 0;
}
