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

// Whitebox: process_data()/decode_update_entities()/upsert_peer()/count_peers() are static.
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
    node_init_locks(node);
    node->id = LOCAL_NODE_ID;
    // Real baseline every other caller of encode()/start_encode() assumes (see tt_Node_create()'s
    // own reset_node_state()) - needed now that reply_with_own_announce()'s tests below exercise
    // that encode path, not just process_data()'s incoming-side decode path the earlier tests
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
// (matching what process_packet() would have handed process_data()).
static uint32_t write_update_one_entity(uint8_t* buf, uint64_t last_modified, uint32_t endpoint_id, uint8_t kind,
                                        const char* type, const char* name) {
    struct test_announce* update_header = test_announce_at(buf);
    test_announce_set_last_modified(update_header, last_modified);
    update_header->announce.entity_count = 1;
    uint32_t tail = sizeof(struct test_announce);

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

// An UpdateHeader that announces no endpoints at all - what tt_Node_destroy() broadcasts on the
// way out.
static uint32_t write_update_no_entities(uint8_t* buf, uint64_t last_modified) {
    struct test_announce* update_header = test_announce_at(buf);
    test_announce_set_last_modified(update_header, last_modified);
    update_header->announce.entity_count = 0;
    return sizeof(struct test_announce);
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
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, sender_ip, sender_port));

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
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, sender_ip, sender_port));

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

    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a04, 8282));
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
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));

    tail = write_update_one_entity(node.rx_buffer, 200, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    uint32_t new_ip = 0xc0a80a09;
    uint16_t new_port = 7777;
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, new_ip, new_port));

    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers)); // refreshed, not duplicated
    EXPECT_EQ_U32(new_ip, pub.peers[0].ip);
    EXPECT_EQ_U32((uint32_t)new_port, (uint32_t)pub.peers[0].port);
}

// A repeated announce with the SAME last_modified hits process_data()'s existing dedup
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
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, first_ip, 8282));

    // Same last_modified (100) as before - dedup path, decode_update_entities() must not run.
    tail = write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80aff, 9999));

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
        EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a00 + source, 8282));
    }
    EXPECT_EQ_U32(tt_MAX_PEER_COUNT, (uint32_t)count_peers(pub.peers));

    // One more, distinct source - table is already full.
    uint8_t overflow_source = (uint8_t)(REMOTE_NODE_ID + tt_MAX_PEER_COUNT);
    init_header(&header, overflow_source);
    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80aff, 9999)); // still returns true

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
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, sender_ip, sender_port));

    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // replied once, unicast
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_call_count);    // and only that - no broadcast too
    EXPECT_EQ_U32(sender_ip, test_mock_send_to_last_ip);
    EXPECT_EQ_U32((uint32_t)sender_port, (uint32_t)test_mock_send_to_last_port);
    EXPECT_EQ_U32(sizeof(struct tt_Header), node.tx_tail); // flushed immediately, drained back down
}

// A second, changed announce from a node we already know is answered once more when it came by broadcast -
// the node may have just created an endpoint matching one of ours (2026-09-26) - but never when it came
// unicast: a unicast announce is itself a reply, and answering replies is what would never stop.
static void test_repeat_contact_is_answered_only_when_broadcast(void) {
    test_mock_reset();

    struct tt_Node node;
    init_node(&node);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count);

    tail = write_update_one_entity(node.rx_buffer, 200, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    node.rx_via_data_port = true; // unicast: a reply
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(1, (uint32_t)test_mock_send_to_call_count); // not answered

    tail = write_update_one_entity(node.rx_buffer, 300, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    node.rx_via_data_port = false; // broadcast: a change
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_to_call_count); // answered once

    tail = write_update_one_entity(node.rx_buffer, 300, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(2, (uint32_t)test_mock_send_to_call_count); // its periodic resend is not
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
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));

    EXPECT_EQ_U32(0, (uint32_t)test_mock_send_to_call_count);
}

// A later announce from the same source that no longer lists the endpoint (it dropped that
// Subscriber, or - entity_count 0 - it's a tt_Node_destroy() farewell) must drop the peer entry
// its earlier announce created. Without this, a peer that leaves lingers forever (there's no
// other expiry).
static void test_source_dropping_endpoint_forgets_its_peer(void) {
    test_mock_reset();

    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));

    // Farewell: same source, newer last_modified, no entities.
    tail = write_update_no_entities(node.rx_buffer, 200);
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers));
}

// A farewell from one source must not disturb a peer entry another source established.
static void test_farewell_from_one_source_leaves_other_peers_intact(void) {
    test_mock_reset();

    struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    struct tt_Header from2;
    init_header(&from2, 2);
    struct tt_Header from3;
    init_header(&from3, 3);

    uint32_t tail =
        write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_data(&node, &from2, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    tail = write_update_one_entity(node.rx_buffer, 100, PUB_ENDPOINT_ID, tt_KIND_TOPIC_SUBSCRIBER, "topic", "sub");
    EXPECT_TRUE(process_data(&node, &from3, node.rx_buffer, 0, tail, 0xc0a80a03, 8282));
    EXPECT_EQ_U32(2, (uint32_t)count_peers(pub.peers));

    tail = write_update_no_entities(node.rx_buffer, 200);
    EXPECT_TRUE(process_data(&node, &from2, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));

    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    bool found_node3 = false;
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        if (pub.peers[i].node_id == 3) {
            found_node3 = true;
        }
        EXPECT_TRUE(pub.peers[i].node_id != 2); // source 2 fully gone
    }
    EXPECT_TRUE(found_node3);
}

static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return 4;
}

// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)data;
    (void)payload;
    (void)len;
    return 4;
}

// A Publisher created AFTER the remote Subscriber was announced still learns it, from the next periodic
// resend of that same announce - which the dedup would otherwise skip forever, since the remote's endpoints
// never change. On the rig this left 4 of 7 rmw_tickle pings broadcasting every sample (2026-09-26).
static void test_publisher_created_after_the_announce_learns_the_peer_from_its_resend(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Topic topic = {.name = "late_topic",
                             .data_size = 4,
                             .data_encode_size = fake_encode_size,
                             .data_encode = fake_encode};
    const uint32_t endpoint_id = tt_hash_id("late_topic", "late_pub");

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);
    uint32_t tail = write_update_one_entity(node.rx_buffer, 100, endpoint_id, tt_KIND_TOPIC_SUBSCRIBER, "t", "sub");
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282)); // no Publisher yet

    struct tt_Publisher pub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&node, &pub, &topic, "late_pub"));
    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers)); // control: nothing is learned at creation itself

    tail = write_update_one_entity(node.rx_buffer, 100, endpoint_id, tt_KIND_TOPIC_SUBSCRIBER, "t", "sub");
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282)); // the same resend
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    EXPECT_EQ_U32(REMOTE_NODE_ID, (uint32_t)pub.peers[0].node_id);

    // ...and the resend after that is a plain duplicate again: the stored generation is the real one.
    EXPECT_EQ_U32(100, node.update_generation[REMOTE_NODE_ID]);
}

static int32_t fake_request_encode_size(struct tt_Request* request) {
    (void)request;
    return 4;
}

// NOLINTNEXTLINE(readability-non-const-parameter) - must match the service codec's fixed signature
static int32_t fake_request_encode(struct tt_Request* request, uint8_t* payload, const uint32_t len) {
    (void)request;
    (void)payload;
    (void)len;
    return 4;
}

static int32_t fake_response_decode(struct tt_Response* response, const uint8_t* payload, const uint32_t len,
                                    bool is_native) {
    (void)response;
    (void)payload;
    (void)is_native;
    return (int32_t)len;
}

static void fake_response_free(struct tt_Response* response) {
    (void)response;
}

static void fake_client_callback(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    (void)return_code;
    (void)response;
}

// The Client mirror of the test above: a Server announced before the Client existed is learned from the
// resend.
static void test_client_created_after_the_announce_learns_the_peer_from_its_resend(void) {
    struct tt_Node node;
    init_node(&node);
    struct tt_Service service = {.name = "late_service",
                                 .request_size = 4,
                                 .response_size = 4,
                                 .request_encode_size = fake_request_encode_size,
                                 .request_encode = fake_request_encode,
                                 .response_decode = fake_response_decode,
                                 .response_free = fake_response_free};
    const uint32_t endpoint_id = tt_hash_id("late_service", "late_client");

    struct tt_Header header;
    init_header(&header, REMOTE_NODE_ID);
    uint32_t tail = write_update_one_entity(node.rx_buffer, 100, endpoint_id, tt_KIND_SERVICE_SERVER, "s", "srv");
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282)); // no Client yet

    struct tt_Client client;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_client(&node, &client, &service, "late_client", fake_client_callback));
    EXPECT_EQ_U32(0, (uint32_t)count_peers(client.peers));

    tail = write_update_one_entity(node.rx_buffer, 100, endpoint_id, tt_KIND_SERVICE_SERVER, "s", "srv");
    EXPECT_TRUE(process_data(&node, &header, node.rx_buffer, 0, tail, 0xc0a80a02, 8282));
    EXPECT_EQ_U32(1, (uint32_t)count_peers(client.peers));
}

// --- Two nodes exchanging announces for real (2026-09-26) -----------------------------------------------
// Every datagram either node sends is delivered to the other: broadcast on the well-known socket, unicast on
// the data socket (rx_via_data_port), as hal_linux.c reports them.
#define DUO_QUEUE 64
struct duo_datagram {
    uint8_t from;
    bool unicast;
    uint32_t len;
    uint8_t bytes[tt_MAX_BUFFER_LENGTH];
    // Its discovery submessages, by what they are (duo_classify()).
    int summaries; // HEARTBEAT of the discovery endpoint
    int requests;  // ACKNACK of the discovery endpoint
    int lists;     // DATA of the discovery endpoint: the full announce
};
static struct duo_datagram duo_queue[DUO_QUEUE];
static int duo_count;
static uint8_t duo_acting;
static int duo_seen_send_to;
static int duo_sent[3];
// What each node sent, by kind; lists split by how they were addressed.
static int duo_summaries[3];
static int duo_requests[3];
static int duo_lists_broadcast[3];
static int duo_lists_unicast[3];
// Loses a datagram it returns true for, on the way to the other node. NULL loses nothing.
static bool (*duo_drop)(const struct duo_datagram* datagram);

// Walks a datagram's submessages and counts the discovery ones: every body starts with the endpoint_id.
static void duo_classify(struct duo_datagram* datagram) {
    uint32_t head = sizeof(struct tt_Header);
    while (head + sizeof(struct tt_SubmessageHeader) + sizeof(uint32_t) <= datagram->len) {
        struct tt_SubmessageHeader sub;
        uint32_t endpoint_id = 0;
        memcpy(&sub, &datagram->bytes[head], sizeof(sub));
        memcpy(&endpoint_id, &datagram->bytes[head + sizeof(sub)], sizeof(endpoint_id));
        if (endpoint_id == tt_DISCOVERY_ENDPOINT_ID) {
            datagram->summaries += sub.type == tt_SUBMESSAGE_TYPE_HEARTBEAT;
            datagram->requests += sub.type == tt_SUBMESSAGE_TYPE_ACKNACK;
            datagram->lists += sub.type == tt_SUBMESSAGE_TYPE_DATA;
        }
        if (sub.length < sizeof(sub)) {
            return;
        }
        head += sub.length;
    }
}

static void duo_capture(const void* buf, size_t len) {
    EXPECT_TRUE(duo_count < DUO_QUEUE && len <= tt_MAX_BUFFER_LENGTH);
    struct duo_datagram* datagram = &duo_queue[duo_count++];
    datagram->from = duo_acting;
    datagram->unicast = test_mock_send_to_call_count != duo_seen_send_to; // tt_send_to() counted it first
    duo_seen_send_to = test_mock_send_to_call_count;
    datagram->len = (uint32_t)len;
    memcpy(datagram->bytes, buf, len);
    datagram->summaries = 0;
    datagram->requests = 0;
    datagram->lists = 0;
    duo_classify(datagram);
    duo_sent[duo_acting]++;
    duo_summaries[duo_acting] += datagram->summaries;
    duo_requests[duo_acting] += datagram->requests;
    if (datagram->unicast) {
        duo_lists_unicast[duo_acting] += datagram->lists;
    } else {
        duo_lists_broadcast[duo_acting] += datagram->lists;
    }
}

static void duo_run_due(struct tt_Node* node) {
    bool has_next = false;
    uint64_t next = 0;
    duo_acting = node->id;
    while (run_due_entry(node, test_mock_now, &has_next, &next)) {
    }
}

static void duo_deliver(struct tt_Node* one, struct tt_Node* two) {
    for (int i = 0; i < duo_count; i++) { // grows while delivering: a reply is delivered in the same pass
        struct duo_datagram* datagram = &duo_queue[i];
        if (duo_drop != NULL && duo_drop(datagram)) {
            continue;
        }
        struct tt_Node* to = datagram->from == one->id ? two : one;
        duo_acting = to->id;
        memcpy(to->rx_buffer, datagram->bytes, datagram->len);
        to->rx_via_data_port = datagram->unicast;
        EXPECT_TRUE(process_packet(to, to->rx_buffer, 0, datagram->len, 0x0a000000U + datagram->from, 8282));
    }
    duo_count = 0;
}

// Runs both nodes until `until`, a scheduler entry at a time.
static void duo_run_until(struct tt_Node* one, struct tt_Node* two, uint64_t until) {
    while (true) {
        uint64_t next_one = UINT64_MAX;
        uint64_t next_two = UINT64_MAX;
        (void)sched_next_time(one, &next_one);
        (void)sched_next_time(two, &next_two);
        uint64_t next = next_one < next_two ? next_one : next_two;
        if (next > until) {
            test_mock_now = until;
            return;
        }
        if (next > test_mock_now) {
            test_mock_now = next;
        }
        duo_run_due(one);
        duo_run_due(two);
        duo_deliver(one, two);
    }
}

static void duo_on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}

static int32_t duo_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native) {
    (void)data;
    (void)payload;
    (void)is_native;
    return (int32_t)len;
}

static void duo_free(struct tt_Data* data) {
    (void)data;
}

// A Publisher created on a node that already knows the matching Subscriber's node learns it within a few
// milliseconds: its node announces the change at once, and the other node answers a changed broadcast.
// Until 2026-09-26 it waited for the peer's next periodic announce, up to tt_NODE_UPDATE_INTERVAL, sending by
// broadcast meanwhile. And the exchange ends there: over the following seconds the two nodes send exactly two
// announces more than their periodic ones - the early announce and one reply - so they cannot trade replies.
static void test_a_new_publisher_learns_a_known_peer_at_once_and_the_exchange_ends(void) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    test_mock_send_hook = duo_capture;
    duo_count = 0;
    duo_seen_send_to = 0;
    memset(duo_sent, 0, sizeof(duo_sent));

    static struct tt_Node one;
    static struct tt_Node two;
    init_node(&one);
    init_node(&two);
    one.id = 1;
    two.id = 2;
    EXPECT_EQ_INT(tt_RET_OK, schedule_periodic_tasks(&one));
    EXPECT_EQ_INT(tt_RET_OK, schedule_periodic_tasks(&two));

    struct tt_Topic sub_topic = {.name = "duo", .data_size = 4, .data_decode = duo_decode, .data_free = duo_free};
    struct tt_Subscriber sub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&two, &sub, &sub_topic, "duo_ep", duo_on_data));
    duo_run_until(&one, &two, tt_SECOND + (uint64_t)(3.5 * tt_SECOND)); // both know each other, steady state

    int periodic_one = duo_sent[1];
    int periodic_two = duo_sent[2];
    duo_run_until(&one, &two, test_mock_now + (3 * tt_SECOND));
    periodic_one = duo_sent[1] - periodic_one;
    periodic_two = duo_sent[2] - periodic_two;
    EXPECT_TRUE(periodic_one >= 2 && periodic_two >= 2); // control: both really announce, ~1 a second

    int before_one = duo_sent[1];
    int before_two = duo_sent[2];
    struct tt_Topic pub_topic = {.name = "duo",
                                 .data_size = 4,
                                 .data_encode_size = fake_encode_size,
                                 .data_encode = fake_encode};
    struct tt_Publisher pub;
    uint64_t created = test_mock_now;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&one, &pub, &pub_topic, "duo_ep"));
    duo_run_until(&one, &two, created + (5 * tt_MILLISECOND));
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers)); // within 5 ms, where it used to take up to a second

    duo_run_until(&one, &two, created + (3 * tt_SECOND));
    EXPECT_EQ_INT(periodic_one + 1, duo_sent[1] - before_one); // its periodic announces, plus the early one
    EXPECT_EQ_INT(periodic_two + 1, duo_sent[2] - before_two); // its periodic announces, plus one reply
    test_mock_send_hook = NULL;
}

// --- The discovery summary (tt_VERSION 8, rmw_tickle/DISCOVERY_PLAN.md) --------------------------------

static void duo_reset_counts(void) {
    memset(duo_summaries, 0, sizeof(duo_summaries));
    memset(duo_requests, 0, sizeof(duo_requests));
    memset(duo_lists_broadcast, 0, sizeof(duo_lists_broadcast));
    memset(duo_lists_unicast, 0, sizeof(duo_lists_unicast));
}

// Two nodes, 1 and 2, on the mock clock at 1 s, their periodic tasks scheduled and nothing lost.
static void duo_start(struct tt_Node* one, struct tt_Node* two) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    test_mock_send_hook = duo_capture;
    duo_count = 0;
    duo_seen_send_to = 0;
    duo_drop = NULL;
    memset(duo_sent, 0, sizeof(duo_sent));
    duo_reset_counts();
    init_node(one);
    init_node(two);
    one->id = 1;
    two->id = 2;
    EXPECT_EQ_INT(tt_RET_OK, schedule_periodic_tasks(one));
    EXPECT_EQ_INT(tt_RET_OK, schedule_periodic_tasks(two));
}

static void duo_stop(void) {
    duo_drop = NULL;
    test_mock_send_hook = NULL;
}

static struct tt_Topic duo_sub_topic = {.name = "duo",
                                        .data_size = 4,
                                        .data_decode = duo_decode,
                                        .data_free = duo_free};
static struct tt_Topic duo_pub_topic = {.name = "duo",
                                        .data_size = 4,
                                        .data_encode_size = fake_encode_size,
                                        .data_encode = fake_encode};

static bool drop_summaries(const struct duo_datagram* datagram) {
    return datagram->summaries > 0;
}

static bool drop_node_one_lists(const struct duo_datagram* datagram) {
    return datagram->from == 1 && datagram->lists > 0;
}

static bool drop_node_one_broadcast_lists(const struct duo_datagram* datagram) {
    return datagram->from == 1 && datagram->lists > 0 && !datagram->unicast;
}

// Rules 1 and 2: once two nodes know each other, each sends one summary a second and nothing else - no
// request, no list - and the summaries alone keep the other alive. The liveliness half is checked with an
// entity lease of 1.5 s, sampled every 50 ms over 3 s: shorter than the 3 s since the last full list, so only
// the summaries can keep it; and at the node level any traffic vetoes a death (check_liveliness()), so the
// lease is where a summary that did not refresh liveliness shows. Control: with the summaries lost the same
// entity expires, so the check can fail.
static void test_steady_state_is_summaries_that_keep_the_peer_alive(void) {
    static struct tt_Node one;
    static struct tt_Node two;
    duo_start(&one, &two);
    struct tt_Subscriber sub;
    struct tt_Publisher pub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&two, &sub, &duo_sub_topic, "duo_ep", duo_on_data));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&one, &pub, &duo_pub_topic, "duo_ep"));
    duo_run_until(&one, &two, test_mock_now + (3 * tt_SECOND) + (tt_SECOND / 2));
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));

    duo_reset_counts();
    struct tt_DiscoveredEntity entity;
    memset(&entity, 0, sizeof(entity));
    entity.node_id = 2;
    entity.alive = true;
    entity.liveliness_lease_duration_ns = tt_SECOND + (tt_SECOND / 2);
    int dead_samples = 0;
    uint64_t end = test_mock_now + (3 * tt_SECOND);
    while (test_mock_now < end) {
        duo_run_until(&one, &two, test_mock_now + (50 * tt_MILLISECOND));
        dead_samples += !tt_Node_entity_alive(&one, &entity, test_mock_now);
    }
    EXPECT_EQ_INT(0, dead_samples);
    EXPECT_TRUE(duo_summaries[1] >= 2 && duo_summaries[1] <= 4);
    EXPECT_TRUE(duo_summaries[2] >= 2 && duo_summaries[2] <= 4);
    EXPECT_EQ_INT(0, duo_requests[1] + duo_requests[2]);
    EXPECT_EQ_INT(0, duo_lists_broadcast[1] + duo_lists_broadcast[2] + duo_lists_unicast[1] + duo_lists_unicast[2]);
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));

    duo_drop = drop_summaries; // control
    end = test_mock_now + (3 * tt_SECOND);
    dead_samples = 0;
    while (test_mock_now < end) {
        duo_run_until(&one, &two, test_mock_now + (50 * tt_MILLISECOND));
        dead_samples += !tt_Node_entity_alive(&one, &entity, test_mock_now);
    }
    EXPECT_TRUE(dead_samples > 0);
    duo_stop();
}

// Rules 3 and 5: a change is pushed by broadcast; a node that missed that broadcast asks when the next
// summary shows it a generation it has not applied, and gets the list unicast - within one interval and a
// round trip. Control: 5 ms after the change, with its broadcast lost, the peer is not yet known.
static void test_a_missed_change_is_pulled_on_the_next_summary(void) {
    static struct tt_Node one;
    static struct tt_Node two;
    duo_start(&one, &two);
    struct tt_Publisher pub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&two, &pub, &duo_pub_topic, "duo_ep"));
    duo_run_until(&one, &two, test_mock_now + (3 * tt_SECOND) + (tt_SECOND / 2));
    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers));

    duo_reset_counts();
    duo_drop = drop_node_one_broadcast_lists;
    struct tt_Subscriber sub;
    uint64_t created = test_mock_now;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&one, &sub, &duo_sub_topic, "duo_ep", duo_on_data));
    duo_run_until(&one, &two, created + (5 * tt_MILLISECOND));
    EXPECT_EQ_INT(1, duo_lists_broadcast[1]); // the push happened, and was lost
    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers));

    duo_run_until(&one, &two, created + tt_NODE_UPDATE_INTERVAL + (5 * tt_MILLISECOND));
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    EXPECT_EQ_INT(1, duo_requests[2]);
    EXPECT_EQ_INT(1, duo_lists_unicast[1]);
    EXPECT_EQ_INT(0, duo_requests[1]); // node 1 knew node 2's generation throughout

    duo_run_until(&one, &two, test_mock_now + (3 * tt_SECOND)); // and it ends there
    EXPECT_EQ_INT(1, duo_requests[2]);
    EXPECT_EQ_INT(1, duo_lists_unicast[1]);
    duo_stop();
}

// Rule 3's bound (DISCOVERY_PLAN.md section 4, first risk): a node whose lists never arrive is asked once
// per summary - never more - and the asking stops as soon as a list gets through.
static void test_requests_come_one_per_summary_until_a_list_arrives(void) {
    static struct tt_Node one;
    static struct tt_Node two;
    duo_start(&one, &two);
    struct tt_Publisher pub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(&two, &pub, &duo_pub_topic, "duo_ep"));
    duo_run_until(&one, &two, test_mock_now + (3 * tt_SECOND) + (tt_SECOND / 2));

    duo_reset_counts();
    duo_drop = drop_node_one_lists;
    struct tt_Subscriber sub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(&one, &sub, &duo_sub_topic, "duo_ep", duo_on_data));
    duo_run_until(&one, &two, test_mock_now + (5 * tt_SECOND));
    EXPECT_EQ_U32(0, (uint32_t)count_peers(pub.peers));
    EXPECT_TRUE(duo_summaries[1] >= 4);
    EXPECT_EQ_INT(duo_summaries[1], duo_requests[2]);     // one request per summary
    EXPECT_EQ_INT(duo_requests[2], duo_lists_unicast[1]); // each answered, and each answer lost

    duo_drop = NULL;
    duo_run_until(&one, &two, test_mock_now + tt_NODE_UPDATE_INTERVAL + (5 * tt_MILLISECOND));
    EXPECT_EQ_U32(1, (uint32_t)count_peers(pub.peers));
    int requests = duo_requests[2];
    duo_run_until(&one, &two, test_mock_now + (3 * tt_SECOND));
    EXPECT_EQ_INT(requests, duo_requests[2]);
    duo_stop();
}

static int answers_unicast;
static int answers_broadcast;

static void count_answers(const void* buf, size_t len) {
    struct duo_datagram datagram;
    memset(&datagram, 0, sizeof(datagram));
    datagram.len = (uint32_t)len;
    memcpy(datagram.bytes, buf, len);
    duo_classify(&datagram);
    bool unicast = test_mock_send_to_call_count != duo_seen_send_to;
    duo_seen_send_to = test_mock_send_to_call_count;
    if (unicast) {
        answers_unicast += datagram.lists;
    } else {
        answers_broadcast += datagram.lists;
    }
}

// One peer's request for this node's list, as send_discovery_request() writes it.
static uint32_t write_request(uint8_t* buf, uint8_t source, uint32_t generation) {
    struct tt_Header header;
    init_header(&header, source);
    memcpy(buf, &header, sizeof(header));
    struct tt_SubmessageHeader sub = {tt_SUBMESSAGE_TYPE_ACKNACK, LOCAL_NODE_ID,
                                      (uint16_t)(sizeof(sub) + sizeof(struct tt_AckNackHeader))};
    memcpy(buf + sizeof(header), &sub, sizeof(sub));
    struct tt_AckNackHeader request;
    memset(&request, 0, sizeof(request));
    request.endpoint_id = tt_DISCOVERY_ENDPOINT_ID;
    request.entity_id = tt_DISCOVERY_ENTITY_ID;
    request.sender_entity_id = tt_DISCOVERY_ENTITY_ID;
    request.seq_no = generation;
    memcpy(buf + sizeof(header) + sizeof(sub), &request, sizeof(request));
    return (uint32_t)(sizeof(header) + sizeof(sub) + sizeof(request));
}

// Rule 4: requests are answered unicast, up to tt_UNICAST_PEER_THRESHOLD in one tt_NODE_TX_INTERVAL tick;
// the next is answered by one broadcast, and later ones in the tick by nothing more. A new tick starts over.
static void test_requests_beyond_the_threshold_are_answered_by_one_broadcast(void) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    test_mock_send_hook = count_answers;
    duo_seen_send_to = 0;
    answers_unicast = 0;
    answers_broadcast = 0;
    static struct tt_Node node;
    init_node(&node);
    struct tt_Publisher pub;
    init_publisher(&pub, &node);

    const int requesters = tt_UNICAST_PEER_THRESHOLD + 3;
    for (int i = 0; i < requesters; i++) {
        uint32_t len = write_request(node.rx_buffer, (uint8_t)(REMOTE_NODE_ID + i), (uint32_t)node.last_modified);
        node.rx_via_data_port = true;
        EXPECT_TRUE(process_packet(&node, node.rx_buffer, 0, len, 0xc0a80a02U + (uint32_t)i, 8282));
    }
    if (node.tx_tail != sizeof(struct tt_Header)) {
        (void)flush_tx(&node, node.tx_tail, NULL, 0); // what the flush tick does with the batched broadcast
    }
    EXPECT_EQ_INT(tt_UNICAST_PEER_THRESHOLD, answers_unicast);
    EXPECT_EQ_INT(1, answers_broadcast);

    test_mock_now += tt_NODE_TX_INTERVAL;
    uint32_t len = write_request(node.rx_buffer, REMOTE_NODE_ID, (uint32_t)node.last_modified);
    EXPECT_TRUE(process_packet(&node, node.rx_buffer, 0, len, 0xc0a80a02U, 8282));
    EXPECT_EQ_INT(tt_UNICAST_PEER_THRESHOLD + 1, answers_unicast);
    EXPECT_EQ_INT(1, answers_broadcast);
    test_mock_send_hook = NULL;
}

int main(void) {
    test_publisher_learns_subscriber_peer_from_update();
    test_client_learns_server_peer_from_update();
    test_unrelated_entity_kind_is_not_tracked_as_peer();
    test_repeated_announce_from_same_node_refreshes_peer_not_duplicates();
    test_update_skipped_when_last_modified_unchanged_does_not_rerun_matching();
    test_peer_table_full_drops_new_peer_silently();
    test_first_contact_triggers_unicast_reply_with_own_announce();
    test_repeat_contact_is_answered_only_when_broadcast();
    test_reply_skipped_when_tx_buffer_has_pending_content();
    test_source_dropping_endpoint_forgets_its_peer();
    test_farewell_from_one_source_leaves_other_peers_intact();
    test_publisher_created_after_the_announce_learns_the_peer_from_its_resend();
    test_client_created_after_the_announce_learns_the_peer_from_its_resend();
    test_a_new_publisher_learns_a_known_peer_at_once_and_the_exchange_ends();
    test_steady_state_is_summaries_that_keep_the_peer_alive();
    test_a_missed_change_is_pulled_on_the_next_summary();
    test_requests_come_one_per_summary_until_a_list_arrives();
    test_requests_beyond_the_threshold_are_answered_by_one_broadcast();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_peer_discovery: all tests passed\n");
    return 0;
}
