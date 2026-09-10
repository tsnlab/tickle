/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: process_update()/decode_update_entities()/upsert_peer()/count_peers() are static.
// This exercises the discovery-side half of the unicast/broadcast feature - learning a peer's
// address from its periodic UPDATE announce and storing it on the matching local
// Publisher/Client - which nothing else in this test suite covers.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define PUB_ENDPOINT_ID 0x11111111
#define CLIENT_ENDPOINT_ID 0x22222222

// Only ->name matters for these tests - reply_with_own_announce()'s encode_update_entities() call
// (see build_and_send_update()) reads topic->name/service->name and endpoint->name for every
// registered endpoint, including these, once a node with one registered actually replies.
static struct tt_Topic test_topic = {.name = "test_topic"};
static struct tt_Service test_service = {.name = "test_service"};

static void init_node(struct tt_Node* node) {
    memset(node, 0, sizeof(*node));
    node->id = LOCAL_NODE_ID;
    // Real baseline every other caller of encode()/start_encode() assumes (see tt_Node_create()'s
    // own reset_node_state()) - needed now that reply_with_own_announce()'s tests below exercise
    // that encode path, not just process_update()'s incoming-side decode path the earlier tests
    // in this file only needed.
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;
}

static void init_publisher(struct tt_Publisher* pub, struct tt_Node* node) {
    memset(pub, 0, sizeof(*pub));
    pub->endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub->endpoint.id = PUB_ENDPOINT_ID;
    pub->endpoint.name = "test_publisher";
    pub->topic = &test_topic;
    pub->node = node;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        pub->peers[i].node_id = tt_NODE_ID_INVALID;
    }
    node->endpoints[node->endpoint_count++] = (struct tt_Endpoint*)pub;
}

static void init_client(struct tt_Client* client, struct tt_Node* node) {
    memset(client, 0, sizeof(*client));
    client->endpoint.kind = tt_KIND_SERVICE_CLIENT;
    client->endpoint.id = CLIENT_ENDPOINT_ID;
    client->endpoint.name = "test_client";
    client->service = &test_service;
    client->node = node;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        client->peers[i].node_id = tt_NODE_ID_INVALID;
    }
    node->endpoints[node->endpoint_count++] = (struct tt_Endpoint*)client;
}

// Builds an UpdateHeader with a single following UpdateEntity in buf, returning the tail offset
// (matching what process_packet() would have handed process_update()).
static uint32_t write_update_one_entity(uint8_t* buf, uint64_t last_modified, uint32_t endpoint_id, uint8_t kind,
                                        const char* type, const char* name) {
    struct tt_UpdateHeader* update_header = (struct tt_UpdateHeader*)buf;
    update_header->last_modified = last_modified;
    update_header->entity_count = 1;
    uint32_t tail = sizeof(struct tt_UpdateHeader);

    struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)(buf + tail);
    entity->endpoint_id = endpoint_id;
    entity->kind = kind;
    tail += sizeof(struct tt_UpdateEntity);

    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, type);
    tt_encode_string(buf, &tail, tt_MAX_BUFFER_LENGTH * 2, name);

    return tail;
}

static void init_header(struct tt_Header* header, uint8_t source) {
    memset(header, 0, sizeof(*header));
    header->magic_value = NATIVE_MAGIC_VALUE;
    header->version = tt_VERSION;
    header->source = source;
}

// A remote TOPIC_SUBSCRIBER announcing the same endpoint_id as our local Publisher must be
// learned as that Publisher's peer, with the address the packet actually arrived from.
static void test_publisher_learns_subscriber_peer_from_update(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");

    uint32_t sender_ip = 0xc0a80a02; // 192.168.10.2
    uint16_t sender_port = 8282;
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, sender_ip, sender_port));

    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)pub.peers[0].node_id);
    EXPECT_EQ_U32(sender_ip, pub.peers[0].ip);
    EXPECT_EQ_U32((uint32_t)sender_port, (uint32_t)pub.peers[0].port);
}

// Same as above, mirrored for the Client/Server direction: a remote SERVICE_SERVER announcing a
// matching endpoint_id must be learned as our local Client's peer.
static void test_client_learns_server_peer_from_update(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Client client;
    init_client(&client, &node);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, CLIENT_ENDPOINT_ID, tt_KIND_SERVICE_SERVER, "service", "server");

    uint32_t sender_ip = 0xc0a80a03; // 192.168.10.3
    uint16_t sender_port = 9191;
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, sender_ip, sender_port));

    EXPECT_EQ_U32(1, (uint32_t)count_peers(client.peers));
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)client.peers[0].node_id);
    EXPECT_EQ_U32(sender_ip, client.peers[0].ip);
    EXPECT_EQ_U32((uint32_t)sender_port, (uint32_t)client.peers[0].port);
}

// A remote entity of a kind that doesn't complement anything we track peers for (e.g. another
// Publisher announcing itself) must not be recorded as a peer, even if its endpoint_id happens
// to match - only TOPIC_SUBSCRIBER/SERVICE_SERVER announcements are ever matched.
static void test_unrelated_entity_kind_is_not_tracked_as_peer(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_PUBLISHER, "topic", "pub2");

    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a04, 8282));
    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers));
}

// Two announces from the same source node, with a changed last_modified (so the dedup check
// doesn't short-circuit the second one) and a changed address, must refresh the existing slot -
// not add a second entry.
static void test_repeated_announce_from_same_node_refreshes_peer_not_duplicates(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));

    tail = write_update_one_entity(node.rx_buffer, 200, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    uint32_t new_ip = 0xc0a80a09;
    uint16_t new_port = 7777;
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, new_ip, new_port));

    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers)); // refreshed, not duplicated
    EXPECT_EQ_U32(new_ip, pub.peers[0].ip);
    EXPECT_EQ_U32((uint32_t)new_port, (uint32_t)pub.peers[0].port);
}

// A repeated announce with the SAME last_modified hits process_update()'s existing dedup
// early-return, which skips decode_update_entities() (and thus peer matching) entirely - a
// changed sender address on that duplicate must NOT overwrite the peer already learned.
static void test_update_skipped_when_last_modified_unchanged_does_not_rerun_matching(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    uint32_t first_ip = 0xc0a80a02;
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, first_ip, 8282));

    // Same last_modified (100) as before - dedup path, decode_update_entities() must not run.
    tail = write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80aff, 9999));

    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    EXPECT_EQ_U32(first_ip, pub.peers[0].ip); // unchanged - the second call's address was ignored
}

// Once peers[] is full, a newly seen peer is silently dropped without disturbing the existing
// entries or failing the UPDATE processing itself.
static void test_peer_table_full_drops_new_peer_silently(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    struct tt_Header header;

    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        uint8_t source = (uint8_t)(REMOTE_NODE_ID + i);
        init_header(&header, source);
        uint32_t tail =
            write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
        EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a00 + source, 8282));
    }
    EXPECT_EQ_U32(tt_MAX_PEER_COUNT, (uint32_t)count_peers(pub.peers));

    // One more, distinct source - table is already full.
    uint8_t overflow_source = (uint8_t)(REMOTE_NODE_ID + tt_MAX_PEER_COUNT);
    init_header(&header, overflow_source);
    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80aff, 9999)); // still returns true

    EXPECT_EQ_U32(tt_MAX_PEER_COUNT, (uint32_t)count_peers(pub.peers)); // unchanged, not grown
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)pub.peers[0].node_id);      // first entry untouched
}

// Hearing from a node for the very first time must trigger an immediate unicast reply with our
// own announce, straight back to that sender - so two freshly-started nodes recognize each other
// right away instead of each waiting for the other's own next periodic broadcast (see
// reply_with_own_announce()'s own comment). A bare node with no registered endpoints is enough:
// the reply fires purely off "have I heard from this source before", not off matching anything.
static void test_first_contact_triggers_unicast_reply_with_own_announce(void) {
    test_mock_reset();

    struct tt_Node node;
    init_node(&node);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");

    uint32_t sender_ip = 0xc0a80a02; // 192.168.10.2
    uint16_t sender_port = 8282;
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, sender_ip, sender_port));

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // replied once, unicast
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);    // and only that - no broadcast too
    EXPECT_EQ_U32(sender_ip, test_mock_send_to_last_ip);
    EXPECT_EQ_U32((uint32_t)sender_port, (uint32_t)test_mock_send_to_last_port);
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail); // flushed immediately, drained back down
}

// A second announce from a node we already know (even one that legitimately changed - different
// last_modified, not the dedup-early-return case) must not trigger a second reply - only the
// very first contact does, which is what keeps this from replying forever (see
// reply_with_own_announce()'s own comment).
static void test_repeat_contact_does_not_trigger_reply(void) {
    test_mock_reset();

    struct tt_Node node;
    init_node(&node);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);

    tail = write_update_one_entity(node.rx_buffer, 200, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // still just the one reply
}

// If tx_buffer already has something else pending, replying would risk redirecting that
// unrelated content to this one peer - skip the reply this time (the periodic broadcast still
// reaches them eventually) rather than risk misdirecting it, mirroring process_callrequest()'s
// own shared-tx_buffer guard.
static void test_reply_skipped_when_tx_buffer_has_pending_content(void) {
    test_mock_reset();

    struct tt_Node node;
    init_node(&node);
    node.tx_tail += 4; // pretend something else is already batched/pending

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_update(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

int main(void) {
    test_publisher_learns_subscriber_peer_from_update();
    test_client_learns_server_peer_from_update();
    test_unrelated_entity_kind_is_not_tracked_as_peer();
    test_repeated_announce_from_same_node_refreshes_peer_not_duplicates();
    test_update_skipped_when_last_modified_unchanged_does_not_rerun_matching();
    test_peer_table_full_drops_new_peer_silently();
    test_first_contact_triggers_unicast_reply_with_own_announce();
    test_repeat_contact_does_not_trigger_reply();
    test_reply_skipped_when_tx_buffer_has_pending_content();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_peer_discovery: all tests passed\n");
    return 0;
}
