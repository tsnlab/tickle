/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g8 (rmw_tickle/RMW_GAPS_PLAN.md, 2026-09-28): contexts that share an id - several processes on one host - each end
// up with one of their own. Built with tt_CONTEXT_ID_CLAIM (rmw_tickle's setting); the mock HAL's registry and own
// address stand in for hal_linux.c's. Checked:
// - a packet with this context's id from another address is a collision, not this context's own;
// - the newcomer yields and the established context keeps its id, whichever address is lower;
// - two established contexts: the higher (address, port) moves, once the collision has lasted the startup window;
// - three contexts: when the newcomer moves off the keeper's id, the third keeps the keeper's endpoints;
// - an explicit id never moves, and each foreign holder is reported once;
// - with no free id the context refuses at creation, and after a collision it sends nothing - each publish fails with
//   an error and is counted, never a silent success;
// - the id at creation: preferred when free, the highest free one when not.
// Mutants, each killed here: the self filter by id only; the established side yielding; no move on collision; an
// announce under the old id on moving; a muted publish reported as a success.
#define tt_CONTEXT_ID_CLAIM 1
#ifndef tt_MAX_DISCOVERED_ENTITIES
#define tt_MAX_DISCOVERED_ENTITIES 64
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/log.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: contexts in one process, datagrams carried between them by hand with the sender's address.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define SHARED_ID 5
#define THIRD_ID 7
#define LOW_IP 0x0a000001
#define HIGH_IP 0x0a000002
#define THIRD_IP 0x0a000003
#define PORT 40000
#define CLOCK_NS (100ULL * tt_SECOND)
#define CAPTURE_MAX 64
#define ESTABLISHED_AGE (3 * tt_SECOND)
#define MUTED_ATTEMPTS 3

static uint8_t captured[CAPTURE_MAX][tt_MAX_BUFFER_LENGTH];
static size_t captured_length[CAPTURE_MAX];
static int captured_count;

static void capture(const void* buf, size_t len) {
    if (captured_count < CAPTURE_MAX && len <= tt_MAX_BUFFER_LENGTH) {
        memcpy(captured[captured_count], buf, len);
        captured_length[captured_count++] = len;
    }
}

// What a receiver sends while it processes what it is handed - a collision's reaction - kept to be carried on.
static uint8_t reacted[CAPTURE_MAX][tt_MAX_BUFFER_LENGTH];
static size_t reacted_length[CAPTURE_MAX];
static int reacted_count;

static void capture_reaction(const void* buf, size_t len) {
    if (reacted_count < CAPTURE_MAX && len <= tt_MAX_BUFFER_LENGTH) {
        memcpy(reacted[reacted_count], buf, len);
        reacted_length[reacted_count++] = len;
    }
}

static int32_t data_size(struct tt_Data* data) {
    (void)data;
    return 4;
}
static int32_t data_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)data;
    return len < 4 ? -1 : (memset(payload, 0, 4), 4);
}
static int32_t data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)native;
    return (int32_t)len;
}
static void data_free(struct tt_Data* data) {
    (void)data;
}
static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}

static struct tt_Topic topic;
static struct tt_Context keeper;
static struct tt_Context newcomer;
static struct tt_Context third;
static struct tt_Discovery third_discovery;

// A context as tt_Context_create() leaves it, at this address, created `age` ago.
static void init_context(struct tt_Context* context, uint8_t id, uint32_t ip, uint64_t age) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    context->last_modified = test_mock_now;
    context->created_ns = test_mock_now - age;
    context->hal.own_ip = ip;
    context->hal.own_port = PORT;
}

static void setup(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    memset(test_mock_ids_held, 0, sizeof(test_mock_ids_held));
    memset(&topic, 0, sizeof(topic));
    topic.name = "std_msgs::msg::dds_::String_";
    topic.data_size = 4;
    topic.data_encode_size = data_size;
    topic.data_encode = data_encode;
    topic.data_decode = data_decode;
    topic.data_free = data_free;
}

// What `from` sends while `action` runs - its announce by default - handed to each of `to` as if received.
static void carry(struct tt_Context* from, struct tt_Context** to, int to_count) {
    captured_count = 0;
    test_mock_send_hook = capture;
    from->last_modified = ++test_mock_now; // a new generation
    (void)build_and_send_update(from, NULL, 0);
    node_flush(from, 0, NULL);
    test_mock_now++; // delivery takes time: whatever the receivers send is stamped after it
    test_mock_send_hook = capture_reaction;
    reacted_count = 0;
    for (int t = 0; t < to_count; t++) {
        for (int i = 0; i < captured_count; i++) {
            memcpy(to[t]->rx_buffer, captured[i], captured_length[i]);
            (void)process_packet(to[t], to[t]->rx_buffer, 0, (uint32_t)captured_length[i], from->hal.own_ip,
                                 from->hal.own_port);
        }
    }
    test_mock_send_hook = NULL;
}

// Hands what a receiver sent while reacting to the last carry() - sent from its address as it was then - to `to`.
static void carry_reaction(uint32_t ip, uint16_t port, struct tt_Context* to) {
    for (int i = 0; i < reacted_count; i++) {
        memcpy(to->rx_buffer, reacted[i], reacted_length[i]);
        (void)process_packet(to, to->rx_buffer, 0, (uint32_t)reacted_length[i], ip, port);
    }
}

// Runs a context's due scheduled work - the announce a collision arms - and carries what it sends.
static void run_due(struct tt_Context* from, struct tt_Context** to, int to_count) {
    captured_count = 0;
    test_mock_send_hook = capture;
    bool has_next = false;
    uint64_t next = 0;
    while (run_due_entry(from, test_mock_now, &has_next, &next)) {
    }
    node_flush(from, 0, NULL);
    test_mock_send_hook = NULL;
    for (int t = 0; t < to_count; t++) {
        for (int i = 0; i < captured_count; i++) {
            memcpy(to[t]->rx_buffer, captured[i], captured_length[i]);
            (void)process_packet(to[t], to[t]->rx_buffer, 0, (uint32_t)captured_length[i], from->hal.own_ip,
                                 from->hal.own_port);
        }
    }
}

static void test_own_packets_and_foreign_ones(void) {
    setup();
    init_context(&newcomer, SHARED_ID, LOW_IP, 0);
    struct tt_Context* self[] = {&newcomer};
    carry(&newcomer, self, 1); // its own announce, looped back from its own address
    EXPECT_EQ_INT(SHARED_ID, newcomer.id);
    EXPECT_TRUE(newcomer.rx_self_sent > 0);

    init_context(&keeper, SHARED_ID, HIGH_IP, ESTABLISHED_AGE);
    struct tt_Context* to_newcomer[] = {&newcomer};
    carry(&keeper, to_newcomer, 1); // the same id from another address
    EXPECT_TRUE(newcomer.id != SHARED_ID);
}

// The newcomer has the lower address, so ordering by address alone would move the established one.
static void test_the_newcomer_yields(void) {
    setup();
    init_context(&keeper, SHARED_ID, HIGH_IP, ESTABLISHED_AGE);
    init_context(&newcomer, SHARED_ID, LOW_IP, 0);
    struct tt_Context* to_keeper[] = {&keeper};
    struct tt_Context* to_newcomer[] = {&newcomer};
    carry(&newcomer, to_keeper, 1);
    carry(&keeper, to_newcomer, 1);
    EXPECT_EQ_INT(SHARED_ID, keeper.id);
    EXPECT_TRUE(newcomer.id != SHARED_ID && newcomer.id != tt_CONTEXT_ID_INVALID);
}

static void test_two_established_contexts(void) {
    setup();
    init_context(&keeper, SHARED_ID, LOW_IP, ESTABLISHED_AGE);
    init_context(&newcomer, SHARED_ID, HIGH_IP, ESTABLISHED_AGE); // a partition healed: both old
    struct tt_Context* to_keeper[] = {&keeper};
    struct tt_Context* to_other[] = {&newcomer};
    carry(&newcomer, to_keeper, 1);
    carry(&keeper, to_other, 1);
    EXPECT_EQ_INT(SHARED_ID, keeper.id); // neither moves within the window
    EXPECT_EQ_INT(SHARED_ID, newcomer.id);
    for (int second = 0; second < 2; second++) { // the collision goes on, an announce a second
        test_mock_now += tt_CONTEXT_UPDATE_INTERVAL;
        carry(&newcomer, to_keeper, 1);
        carry(&keeper, to_other, 1);
    }
    EXPECT_EQ_INT(SHARED_ID, keeper.id); // the lower address keeps it
    EXPECT_TRUE(newcomer.id != SHARED_ID);
}

// Three contexts. The third learns the keeper's publisher; the newcomer's announce under the same id replaces it
// there; the keeper, seeing the collision, announces again; the newcomer moves. The third must end with the keeper's
// publisher under the shared id and the newcomer's under its new one.
static void test_the_third_context_keeps_the_keeper(void) {
    setup();
    init_context(&keeper, SHARED_ID, HIGH_IP, ESTABLISHED_AGE);
    init_context(&newcomer, SHARED_ID, LOW_IP, 0);
    init_context(&third, THIRD_ID, THIRD_IP, ESTABLISHED_AGE);
    memset(&third_discovery, 0, sizeof(third_discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&third, &third_discovery, NULL, NULL));
    static struct tt_Publisher keeper_pub;
    static struct tt_Publisher newcomer_pub;
    static struct tt_Subscriber third_sub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&keeper, &keeper_pub, &topic, "rt/keeper"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&newcomer, &newcomer_pub, &topic, "rt/newcomer"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&third, &third_sub, &topic, "rt/keeper", on_sample));

    struct tt_Context* to_third[] = {&third};
    struct tt_Context* to_keeper_and_third[] = {&keeper, &third};
    struct tt_Context* to_newcomer_and_third[] = {&newcomer, &third};
    carry(&keeper, to_third, 1);
    EXPECT_TRUE(tt_Discovery_find(&third_discovery, SHARED_ID, keeper_pub.endpoint.id) != NULL);
    carry(&newcomer, to_keeper_and_third, 2); // the keeper sees the collision and arms an announce
    run_due(&keeper, to_third, 1);            // which repairs the third's view of the shared id
    carry(&keeper, to_newcomer_and_third, 2); // the newcomer sees the collision and moves
    EXPECT_TRUE(newcomer.id != SHARED_ID);
    carry_reaction(newcomer.hal.own_ip, newcomer.hal.own_port, &third); // anything it sent on the way reaches the third
    run_due(&newcomer, to_third, 1);                                    // its announce under the new id

    EXPECT_TRUE(tt_Discovery_find(&third_discovery, SHARED_ID, keeper_pub.endpoint.id) != NULL);
    EXPECT_TRUE(tt_Discovery_find(&third_discovery, SHARED_ID, newcomer_pub.endpoint.id) == NULL);
    EXPECT_TRUE(tt_Discovery_find(&third_discovery, newcomer.id, newcomer_pub.endpoint.id) != NULL);
}

static int count_occurrences(const char* text, const char* needle) {
    int count = 0;
    for (const char* at = strstr(text, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

static void test_an_explicit_id_never_moves(void) {
    setup();
    init_context(&newcomer, SHARED_ID, LOW_IP, 0);
    newcomer.id_explicit = true;
    init_context(&keeper, SHARED_ID, HIGH_IP, ESTABLISHED_AGE);
    char* log_text = NULL;
    size_t log_size = 0;
    FILE* log = open_memstream(&log_text, &log_size);
    tt_log_set_output(log);
    struct tt_Context* to_newcomer[] = {&newcomer};
    carry(&keeper, to_newcomer, 1);
    carry(&keeper, to_newcomer, 1);
    (void)fclose(log);
    tt_log_set_output(stderr);
    EXPECT_EQ_INT(SHARED_ID, newcomer.id);
    EXPECT_EQ_INT(1, count_occurrences(log_text != NULL ? log_text : "", "was set explicitly"));
    free(log_text);
}

static void test_no_free_id(void) {
    setup();
    for (int id = 1; id < tt_CONTEXT_ID_BROADCAST; id++) {
        test_mock_ids_held[id] = true; // every id held by another context on this host
    }
    init_context(&newcomer, SHARED_ID, LOW_IP, 0);
    EXPECT_TRUE(!claim_initial_id(&newcomer)); // refused at creation

    init_context(&newcomer, SHARED_ID, LOW_IP, 0);
    init_context(&keeper, SHARED_ID, HIGH_IP, ESTABLISHED_AGE);
    struct tt_Context* to_newcomer[] = {&newcomer};
    carry(&keeper, to_newcomer, 1);
    EXPECT_TRUE(newcomer.id_muted);
    // Silent on the wire, never towards its user: each publish fails, and is counted.
    static struct tt_Publisher pub;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&newcomer, &pub, &topic, "rt/muted"));
    uint32_t value = 0;
    int sent_before = test_mock_send_call_count;
    for (int attempt = 0; attempt < MUTED_ATTEMPTS; attempt++) {
        EXPECT_EQ_INT(tt_RET_IO_ERROR, tt_Publisher_publish(&pub, (struct tt_Data*)&value));
    }
    EXPECT_EQ_U32(MUTED_ATTEMPTS, (uint32_t)newcomer.id_muted_drops);
    (void)build_and_send_update(&newcomer, NULL, 0); // an announce: refused at the HAL, and counted
    node_flush(&newcomer, 0, NULL);
    EXPECT_EQ_INT(sent_before, test_mock_send_call_count);
    EXPECT_TRUE(newcomer.id_muted_drops > MUTED_ATTEMPTS);
}

static void test_the_id_at_creation(void) {
    setup();
    test_mock_node_id = SHARED_ID;
    init_context(&newcomer, 0, LOW_IP, 0);
    EXPECT_TRUE(claim_initial_id(&newcomer));
    EXPECT_EQ_INT(SHARED_ID, newcomer.id); // the preferred id, free
    init_context(&keeper, 0, LOW_IP, 0);
    EXPECT_TRUE(claim_initial_id(&keeper));
    EXPECT_EQ_INT(tt_CONTEXT_ID_BROADCAST - 1, keeper.id); // held by the first: the highest free one
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_own_packets_and_foreign_ones();
    test_the_newcomer_yields();
    test_two_established_contexts();
    test_the_third_context_keeps_the_keeper();
    test_an_explicit_id_never_moves();
    test_no_free_id();
    test_the_id_at_creation();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_context_id_claim: all tests passed\n");
    return 0;
}
