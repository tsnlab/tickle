/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g6 (rmw_tickle/RMW_GAPS_PLAN.md, 2026-09-28): the discovery range and static peers, built with
// tt_DISCOVERY_OPTIONS (rmw_tickle's setting). Checked:
// - LOCALHOST: a datagram from a foreign address is dropped and counted; one from loopback, or from a static peer,
//   is processed;
// - OFF: nothing received is processed, and nothing is sent;
// - a static peer link: a broadcast-class datagram also goes to it, on the well-known port; a /24 peer is reached at
//   its directed broadcast and matches its subnet;
// - SUBNET: everything is processed, as before.
// Mutants, each killed here: the filter off; LOCALHOST passing a foreign sender; peers not among the destinations.
#define tt_DISCOVERY_OPTIONS 1

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define LOCAL_ID 1
#define LOCAL_IP 0x0a000001 // 10.0.0.1: this host on the main link
#define REMOTE_ID 2
#define FOREIGN_IP 0x0a000002  // 10.0.0.2: another host
#define LOOPBACK_IP 0x7f000001 // 127.0.0.1
#define PEER_IP 0xc0a80105     // 192.168.1.5
#define SUBNET_PEER_BROADCAST 0xc0a802ffU
#define SUBNET_MEMBER 0xc0a80207U // 192.168.2.7
#define PORT 40000
#define CLOCK_NS 1000000000ULL

static struct tt_Context local;
static struct tt_Context remote;
static struct tt_Discovery discovery;
static uint8_t announce[tt_MAX_BUFFER_LENGTH];
static size_t announce_length;
static char peer_one[] = "192.168.1.5";
static char peer_subnet[] = "192.168.2.0/24";
static char main_broadcast[] = "255.255.255.255";

static void capture(const void* buf, size_t len) {
    if (announce_length == 0 && len <= sizeof(announce)) {
        memcpy(announce, buf, len);
        announce_length = len;
    }
}

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_MAX_BUFFER_LENGTH * 2;
    context->last_modified = CLOCK_NS;
}

// One announce from the remote context, captured once.
static void make_announce(void) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&remote, REMOTE_ID);
    announce_length = 0;
    test_mock_send_hook = capture;
    EXPECT_TRUE(build_and_send_update(&remote, NULL, 0));
    node_flush(&remote, 0, NULL);
    test_mock_send_hook = NULL;
    EXPECT_TRUE(announce_length > 0);
}

static void configure(uint8_t range, bool with_peers) {
    _tt_CONFIG.discovery_range = range;
    _tt_CONFIG.link_count = 0;
    if (with_peers) {
        _tt_CONFIG.links[0] = (struct _tt_Link) {.broadcast = main_broadcast, .unicast_threshold = 2};
        _tt_CONFIG.links[1] = (struct _tt_Link) {.broadcast = peer_one, .unicast_threshold = 2, .peer = true};
        _tt_CONFIG.links[2] = (struct _tt_Link) {.broadcast = peer_subnet, .unicast_threshold = 2, .peer = true};
        _tt_CONFIG.link_count = 3;
    }
    test_mock_link_resolves = true; // the main link is a real interface, 10.0.0.1/24
    test_mock_link_addr = LOCAL_IP;
    test_mock_link_netmask = 0xffffff00U;
    EXPECT_EQ_INT(tt_RET_OK, resolve_links());
    init_context(&local, LOCAL_ID);
    memset(&discovery, 0, sizeof(discovery));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&local, &discovery, NULL, NULL));
}

// Whether the local context learned the remote one from the announce handed to it from `ip`.
static bool heard_from(uint32_t ip) {
    memcpy(local.rx_buffer, announce, announce_length);
    (void)process_packet(&local, local.rx_buffer, 0, (uint32_t)announce_length, ip, PORT);
    return local.update_seen[REMOTE_ID];
}

static void test_localhost(void) {
    configure(tt_DISCOVERY_RANGE_LOCALHOST, true);
    EXPECT_TRUE(!heard_from(FOREIGN_IP));
    EXPECT_EQ_U64(1, local.rx_out_of_range);
    configure(tt_DISCOVERY_RANGE_LOCALHOST, true);
    EXPECT_TRUE(heard_from(LOOPBACK_IP));
    configure(tt_DISCOVERY_RANGE_LOCALHOST, true);
    EXPECT_TRUE(heard_from(PEER_IP));
    configure(tt_DISCOVERY_RANGE_LOCALHOST, true);
    EXPECT_TRUE(heard_from(SUBNET_MEMBER)); // inside the /24 peer
}

static void test_off(void) {
    configure(tt_DISCOVERY_RANGE_OFF, false);
    EXPECT_TRUE(!heard_from(LOOPBACK_IP));
    int sent_before = test_mock_send_call_count;
    EXPECT_TRUE(build_and_send_update(&local, NULL, 0));
    node_flush(&local, 0, NULL);
    EXPECT_EQ_INT(sent_before, test_mock_send_call_count);
}

static void test_subnet(void) {
    configure(tt_DISCOVERY_RANGE_SUBNET, false);
    EXPECT_TRUE(heard_from(FOREIGN_IP));
    EXPECT_EQ_U64(0, local.rx_out_of_range);
}

// A broadcast-class datagram goes to the main link's broadcast and to each peer link, on the well-known port.
static void test_peers_are_destinations(void) {
    configure(tt_DISCOVERY_RANGE_SUBNET, true);
    struct tx_destination out[TX_MAX_DESTINATIONS];
    uint8_t count = broadcast_destinations(out);
    EXPECT_EQ_INT(3, count);
    bool to_peer = false;
    bool to_subnet = false;
    for (uint8_t i = 0; i < count; i++) {
        to_peer = to_peer || (out[i].ip == PEER_IP && out[i].port == (uint16_t)_tt_CONFIG.port);
        to_subnet = to_subnet || (out[i].ip == SUBNET_PEER_BROADCAST && out[i].port == (uint16_t)_tt_CONFIG.port);
    }
    EXPECT_TRUE(to_peer);
    EXPECT_TRUE(to_subnet);
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    make_announce();
    test_localhost();
    test_off();
    test_subnet();
    test_peers_are_destinations();
    _tt_CONFIG.discovery_range = tt_DISCOVERY_RANGE_SUBNET;
    _tt_CONFIG.link_count = 0;

    if (test_result() != 0) {
        return 1;
    }
    printf("test_discovery_range: all tests passed\n");
    return 0;
}
