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

// Whitebox: process_packet()/process_submessage() are static, and this is exactly the
// untrusted-input boundary (broadcast UDP, no authentication - see README's "Security &
// concurrency model") that most needs coverage against malformed/adversarial input.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define ENDPOINT_ID 0xaabbccdd

static void init_node(struct tt_Node* node) {
    memset(node, 0, sizeof(*node));
    node->id = LOCAL_NODE_ID;
}

// rmw_tickle/PLAN.md's Milestone 17: proves process_packet()'s own self-sent handling is scoped
// per-submessage-type, not per-packet - a co-located client and service on the exact same
// tt_Node (this test's own LOCAL_NODE_ID for both sides at once) must still be able to talk to
// each other, unlike a node hearing its own topic pub/sub broadcast back.

static int self_sent_callback_count = 0;

static int32_t stub_request_decode(struct tt_Request* request, const uint8_t* payload, const uint32_t len,
                                   bool is_native_endian) {
    (void)request;
    (void)payload;
    (void)is_native_endian;
    return (int32_t)len;
}
static void stub_request_free(struct tt_Request* request) {
    (void)request;
}
static int8_t stub_server_callback(struct tt_Server* server, struct tt_Request* request, struct tt_Response* response) {
    (void)server;
    (void)request;
    (void)response;
    self_sent_callback_count++;
    return 0;
}
static int32_t stub_response_encode_size(struct tt_Response* response) {
    (void)response;
    return 1;
}
static int32_t stub_response_encode(struct tt_Response* response, uint8_t* payload, const uint32_t len) {
    (void)response;
    if (len < 1) {
        return -1;
    }
    payload[0] = 0;
    return 1;
}
static void stub_response_free(struct tt_Response* response) {
    (void)response;
}

static void init_node_service_server(struct tt_Node* node, struct tt_Service* service, struct tt_Server* server) {
    memset(node, 0, sizeof(*node));
    node->id = LOCAL_NODE_ID;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = tt_MAX_BUFFER_LENGTH * 2;

    memset(service, 0, sizeof(*service));
    service->name = "test_service";
    service->request_size = 1;
    service->response_size = 1;
    service->request_decode = stub_request_decode;
    service->request_free = stub_request_free;
    service->response_encode_size = stub_response_encode_size;
    service->response_encode = stub_response_encode;
    service->response_free = stub_response_free;

    memset(server, 0, sizeof(*server));
    server->endpoint.kind = tt_KIND_SERVICE_SERVER;
    server->endpoint.id = ENDPOINT_ID;
    server->node = node;
    server->service = service;
    server->callback = stub_server_callback;

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)server;
}

// Appends a CallRequestHeader + 1-byte body at `offset` (matching test_process_callrequest.c's
// own write_callrequest(), which writes the same shape straight at rx_buffer's start).
static uint32_t append_callrequest_header(uint8_t* buf, uint32_t offset, uint32_t endpoint_id, uint16_t seq_no) {
    struct tt_CallRequestHeader* callrequest_header = (struct tt_CallRequestHeader*)(buf + offset);
    callrequest_header->endpoint_id = endpoint_id;
    callrequest_header->seq_no = seq_no;
    callrequest_header->retry = 0;
    callrequest_header->reserved = 0;
    uint32_t body_offset = offset + sizeof(struct tt_CallRequestHeader);
    buf[body_offset] = 0xab; // 1-byte request body (service->request_size == 1)
    return body_offset + 1;
}

static int self_sent_subscriber_callback_count = 0;

static void stub_self_sent_subscriber_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                               struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
    self_sent_subscriber_callback_count++;
}
// Real (if trivial) implementations, not left NULL: if the self-sent-still-ignored invariant this
// test guards ever regresses, it should fail cleanly via the callback-count assertion below, not
// crash the whole test binary on a NULL function pointer call inside process_data().
static int32_t stub_topic_data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len,
                                      bool is_native_endian) {
    (void)data;
    (void)payload;
    (void)is_native_endian;
    return (int32_t)len;
}
static void stub_topic_data_free(struct tt_Data* data) {
    (void)data;
}

static void init_node_topic_subscriber(struct tt_Node* node, struct tt_Topic* topic, struct tt_Subscriber* sub) {
    memset(node, 0, sizeof(*node));
    node->id = LOCAL_NODE_ID;

    memset(topic, 0, sizeof(*topic));
    topic->name = "test_topic";
    topic->data_size = sizeof(uint32_t);
    topic->data_decode = stub_topic_data_decode;
    topic->data_free = stub_topic_data_free;

    memset(sub, 0, sizeof(*sub));
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = ENDPOINT_ID;
    sub->node = node;
    sub->topic = topic;
    sub->callback = stub_self_sent_subscriber_callback;

    node->endpoint_count = 1;
    node->endpoints[0] = (struct tt_Endpoint*)sub;
}

static void write_header(uint8_t* buf, uint16_t magic, uint8_t version, uint8_t source) {
    struct tt_Header* header = (struct tt_Header*)buf;
    header->magic_value = magic;
    header->version = version;
    header->source = source;
}

// Appends a submessage header at `offset` and returns the offset just past it (where the
// submessage body, if any, would go).
static uint32_t append_submessage_header(uint8_t* buf, uint32_t offset, uint8_t type, uint8_t receiver,
                                         uint16_t length) {
    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)(buf + offset);
    submessage_header->type = type;
    submessage_header->receiver = receiver;
    submessage_header->length = length;
    return offset + sizeof(struct tt_SubmessageHeader);
}

static uint32_t append_data_header(uint8_t* buf, uint32_t offset, uint32_t endpoint_id, uint32_t seq_no) {
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)(buf + offset);
    data_header->endpoint_id = endpoint_id;
    data_header->seq_no = seq_no;
    data_header->timestamp = 0;
    return offset + sizeof(struct tt_DataHeader);
}

// A packet shorter than tt_Header itself must be rejected, not read past the buffer.
static void test_rejects_truncated_header(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[sizeof(struct tt_Header) - 1];
    memset(buf, 0xff, sizeof(buf));

    EXPECT_TRUE(!process_packet(&node, buf, 0, sizeof(buf), 0, 0));
}

// Neither the native nor byte-swapped magic value - not this protocol at all (random noise, or
// some other application broadcasting on the same port/address).
static void test_rejects_bad_magic(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[sizeof(struct tt_Header)];
    write_header(buf, 0xdead, tt_VERSION, REMOTE_NODE_ID);

    EXPECT_TRUE(!process_packet(&node, buf, 0, sizeof(buf), 0, 0));
}

// A peer speaking an older wire version than we understand must be rejected, not misparsed as
// if it were current-version.
static void test_rejects_old_version(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[sizeof(struct tt_Header)];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION - 1, REMOTE_NODE_ID);

    EXPECT_TRUE(!process_packet(&node, buf, 0, sizeof(buf), 0, 0));
}

// A node hears its own broadcast back (normal on a shared broadcast domain) and must ignore it
// cleanly rather than treating it as an error or trying to process it as if from a peer.
static void test_ignores_self_sent_packet(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[sizeof(struct tt_Header)];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, LOCAL_NODE_ID);

    EXPECT_TRUE(process_packet(&node, buf, 0, sizeof(buf), 0, 0));
}

// A submessage claiming a length shorter than its own header can't be real - reject before the
// later `length - sizeof(header)` arithmetic can underflow.
static void test_rejects_submessage_length_too_small(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[64];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);
    uint32_t tail = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL,
                                             sizeof(struct tt_SubmessageHeader) - 1);

    EXPECT_TRUE(!process_packet(&node, buf, 0, tail, 0, 0));
}

// A submessage claiming to be far longer than the bytes actually available must be rejected,
// not trusted into reading (or letting a codec read) past the end of the real buffer.
static void test_rejects_submessage_length_exceeds_buffer(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[64];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);
    // Only sizeof(struct tt_SubmessageHeader) bytes actually follow the header in this buffer,
    // but the submessage claims to be 0xffff bytes long.
    uint32_t tail = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, 0xffff);

    EXPECT_TRUE(!process_packet(&node, buf, 0, tail, 0, 0));
}

// An unknown type (likely a newer protocol revision's submessage - validate_packet_header()
// deliberately accepts higher versions) must be skipped by its validated length, not treated as
// fatal: a valid DATA submessage right after it in the same datagram still has to parse.
static void test_skips_unknown_submessage_type_and_continues(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[128];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);
    offset = append_submessage_header(buf, offset, 99, tt_SUBMESSAGE_ID_ALL, sizeof(struct tt_SubmessageHeader));

    uint16_t data_len = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader);
    offset = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, data_len);
    offset =
        append_data_header(buf, offset, 0x1234, 1); // no subscriber -> process_data() returns "not mine", not error

    EXPECT_TRUE(process_packet(&node, buf, 0, offset, 0, 0));
}

// ACKNACK is a known-but-unimplemented type in this release. Same contract: skip it, keep going.
static void test_skips_acknack_and_continues(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[128];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);
    offset = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_ACKNACK, tt_SUBMESSAGE_ID_ALL,
                                      sizeof(struct tt_SubmessageHeader));

    uint16_t data_len = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader);
    offset = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, data_len);
    offset = append_data_header(buf, offset, 0x1234, 1);

    EXPECT_TRUE(process_packet(&node, buf, 0, offset, 0, 0));
}

// Sanity check in the other direction: two well-formed DATA submessages back to back in one
// packet (the batching case node_flush()/tx_buffer exist for) must both parse and the loop must
// terminate cleanly at the real end of the buffer - malformed-input rejection elsewhere in this
// file shouldn't come at the cost of also rejecting valid, non-trivial input.
static void test_accepts_two_valid_data_submessages(void) {
    struct tt_Node node;
    init_node(&node);
    // No subscriber is registered for either endpoint_id: process_data() treats that as
    // "not for me", not an error, so this only needs the parse itself to succeed.

    uint8_t buf[128];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);

    uint16_t submessage_length = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader);
    offset = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, submessage_length);
    offset = append_data_header(buf, offset, 0x1111, 1);

    offset = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, submessage_length);
    offset = append_data_header(buf, offset, 0x2222, 2);

    EXPECT_TRUE(process_packet(&node, buf, 0, offset, 0, 0));
}

// Milestone 17: a self-sent CALLREQUEST (client and service co-located on the same tt_Node)
// must still reach the registered server - process_packet()'s own self-sent suppression is
// scoped to UPDATE/DATA only, not RPC.
static void test_self_sent_callrequest_reaches_server(void) {
    self_sent_callback_count = 0;

    struct tt_Node node;
    struct tt_Service service;
    struct tt_Server server;
    init_node_service_server(&node, &service, &server);

    uint8_t buf[128];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, LOCAL_NODE_ID); // self-sent: source == node->id
    uint32_t offset = sizeof(struct tt_Header);
    uint32_t body_start = offset + sizeof(struct tt_SubmessageHeader);
    uint32_t body_end = append_callrequest_header(buf, body_start, ENDPOINT_ID, 1);
    append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_CALLREQUEST, tt_SUBMESSAGE_ID_ALL,
                             (uint16_t)(body_end - offset));

    EXPECT_TRUE(process_packet(&node, buf, 0, body_end, 0, 0));
    EXPECT_EQ_U32(1, (uint32_t)self_sent_callback_count);
}

// Milestone 17's own flip side: a self-sent DATA submessage (this node hearing its own topic
// broadcast back) must still be ignored, exactly as before this milestone - only CALLREQUEST/
// CALLRESPONSE lost their self-sent suppression, not pub/sub.
static void test_self_sent_data_is_still_ignored(void) {
    self_sent_subscriber_callback_count = 0;

    struct tt_Node node;
    struct tt_Topic topic;
    struct tt_Subscriber sub;
    init_node_topic_subscriber(&node, &topic, &sub);

    uint8_t buf[128];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, LOCAL_NODE_ID); // self-sent: source == node->id
    uint32_t offset = sizeof(struct tt_Header);
    uint16_t data_len = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader);
    offset = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, data_len);
    offset = append_data_header(buf, offset, ENDPOINT_ID, 1);

    EXPECT_TRUE(process_packet(&node, buf, 0, offset, 0, 0));
    EXPECT_EQ_U32(0, (uint32_t)self_sent_subscriber_callback_count);
}

int main(void) {
    test_rejects_truncated_header();
    test_rejects_bad_magic();
    test_rejects_old_version();
    test_ignores_self_sent_packet();
    test_rejects_submessage_length_too_small();
    test_rejects_submessage_length_exceeds_buffer();
    test_skips_unknown_submessage_type_and_continues();
    test_skips_acknack_and_continues();
    test_accepts_two_valid_data_submessages();
    test_self_sent_callrequest_reaches_server();
    test_self_sent_data_is_still_ignored();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_malformed_packets: all tests passed\n");
    return 0;
}
