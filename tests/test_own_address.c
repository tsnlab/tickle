/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A context's own address on the default configuration, on the REAL Linux HAL and the real core (2026-10-06).
//
// The defect: rmw_tickle's default broadcast is the limited broadcast 255.255.255.255, which no interface owns. The
// HAL found its own address only by matching the configured broadcast against an interface, so on that default it
// recorded 0 - and note_same_host_peer() treats 0 as "not known yet" and declares no peer same-host. Two processes on
// one host built no segment and sent every datagram over UDP (found by the rmw same-host harness's preflight: 0
// segments created). Every rig harness set TICKLE_BROADCAST_ADDR to a directed broadcast, which is why none saw it.
//
// Checked, each with the real data socket and the real segment in /dev/shm - nothing is sent:
// - on the limited broadcast, a context learns an address, and it is one of this host's interface addresses;
// - a peer at a same-host context's address is same-host: the segment is built, and the peer can attach to it by
//   the name it computes from that address (the name agreement is what makes the address matter at all);
// - a peer at a foreign address is not same-host, and a datagram from it is not this context's own;
// - a context bound to a specific address keeps that address: the new fallback is not consulted.
// Mutants, each killed here (run by hand on 2026-10-06): the fallback removed (the original defect); the fallback
// taken even when the bound address is known; SO_BROADCAST not set on the probe, so connect() is refused.
//
// The two-process version - shared memory actually carrying traffic on the default configuration, and NOT across two
// hosts - is platform/linux/test.sh's default-broadcast step, in a private network namespace.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier) - the real HAL below needs ppoll() declared
#endif
#include <ifaddrs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "../src/hal_linux.c" // NOLINT(bugprone-suspicious-include) - the real HAL, deliberately
#include "test_common.h"
// Whitebox: note_same_host_peer(), peer_segment() and forget_same_host_peer() are static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define OWNER_ID 41
#define PEER_ID 42
#define FOREIGN_IP 0xCB007107U  // 203.0.113.7, TEST-NET-3: documentation only, never a local address
#define LOOPBACK_IP 0x7F000001U // 127.0.0.1

// A context as tt_bind() leaves it for this purpose: a data socket bound to _tt_CONFIG.addr on a kernel-chosen port,
// the broadcast it sends to, and the address recorded from both. No well-known socket, so nothing is received either.
static bool open_context(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node->id = id;
    node->hal.sock = -1;
    node->hal.wake_fd = -1;
    node->hal.broadcast_addr.sin_family = AF_INET;
    node->hal.broadcast_addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.broadcast);
    node->hal.broadcast_addr.sin_port = htons(_tt_CONFIG.port);
    node->hal.data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = 0};
    local.sin_addr.s_addr = inet_addr(_tt_CONFIG.addr);
    if (node->hal.data_sock < 0 || bind(node->hal.data_sock, (struct sockaddr*)&local, sizeof(local)) != 0) {
        return false;
    }
    record_own_address(node);
    return true;
}

static void close_context(struct tt_Context* node) {
    release_segments(node);
    if (node->hal.data_sock >= 0) {
        close(node->hal.data_sock);
    }
}

// The oracle, independent of the code under test: getifaddrs(), not a route lookup.
static bool is_interface_address(uint32_t ip) {
    struct ifaddrs* ifaddrs = NULL;
    if (getifaddrs(&ifaddrs) != 0) {
        return false;
    }
    bool found = false;
    for (struct ifaddrs* ifaddr = ifaddrs; ifaddr != NULL && !found; ifaddr = ifaddr->ifa_next) {
        found = ifaddr->ifa_addr != NULL && ifaddr->ifa_addr->sa_family == AF_INET &&
                ntohl(((struct sockaddr_in*)ifaddr->ifa_addr)->sin_addr.s_addr) == ip;
    }
    freeifaddrs(ifaddrs);
    return found;
}

static void use_default_config(void) {
    _tt_CONFIG.addr = "0.0.0.0";
    _tt_CONFIG.broadcast = "255.255.255.255";
}

static void test_the_limited_broadcast_still_gives_an_address(void) {
    use_default_config();
    struct tt_Context node;
    EXPECT_TRUE(open_context(&node, OWNER_ID));
    uint32_t ip = 0;
    uint16_t port = 0;
    tt_own_address(&node, &ip, &port);
    if (ip == 0) {
        printf("  no own address on the limited broadcast - the 2026-10-06 defect, or a host with no route for "
               "255.255.255.255 at all (`ip route get 255.255.255.255` says which)\n");
    }
    EXPECT_TRUE(ip != 0);
    EXPECT_TRUE(is_interface_address(ip));
    EXPECT_TRUE(port != 0);
    close_context(&node);
}

static void test_a_same_host_peer_on_the_default_config_gets_shared_memory(void) {
    use_default_config();
    struct tt_Context owner;
    struct tt_Context peer;
    EXPECT_TRUE(open_context(&owner, OWNER_ID));
    EXPECT_TRUE(open_context(&peer, PEER_ID));
    uint32_t owner_ip = 0;
    uint16_t owner_port = 0;
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    tt_own_address(&owner, &owner_ip, &owner_port);
    tt_own_address(&peer, &peer_ip, &peer_port);

    // Discovery hears the peer's announce from the peer's address.
    note_same_host_peer(&owner, PEER_ID, peer_ip);
    EXPECT_EQ_U32(1, (uint32_t)owner.same_host_peer_count);
    EXPECT_EQ_U32(1, (uint32_t)owner.segments_created);
    EXPECT_TRUE(owner.own_segment != NULL);

    // And the peer, sending to the owner at the address it heard the owner from, finds the file the owner built.
    struct tt_SegmentHeader* attached = peer_segment(&peer, OWNER_ID, owner_ip, owner_port);
    EXPECT_TRUE(attached != NULL);
    if (attached != NULL) {
        EXPECT_EQ_U32(owner_ip, attached->owner_ip);
        EXPECT_EQ_U32(OWNER_ID, attached->owner_context_id);
    }

    close_context(&peer);
    forget_same_host_peer(&owner, PEER_ID);
    EXPECT_TRUE(owner.own_segment == NULL);
    close_context(&owner);
}

// The control: the same default configuration, a peer on another host. Fails if same-host became "any address".
static void test_a_foreign_peer_is_neither_same_host_nor_own(void) {
    use_default_config();
    struct tt_Context node;
    EXPECT_TRUE(open_context(&node, OWNER_ID));
    uint32_t ip = 0;
    uint16_t port = 0;
    tt_own_address(&node, &ip, &port);

    note_same_host_peer(&node, PEER_ID, FOREIGN_IP);
    EXPECT_EQ_U32(0, (uint32_t)node.same_host_peer_count);
    EXPECT_EQ_U32(0, (uint32_t)node.segments_created);
    EXPECT_TRUE(node.own_segment == NULL);

    // Another host whose context took the same port number: a collision to resolve, not our own echo. With the
    // address unknown (0) the port alone decided, and this read as our own.
    EXPECT_TRUE(!tt_is_own_address(&node, FOREIGN_IP, port));
    EXPECT_TRUE(tt_is_own_address(&node, ip, port));
    close_context(&node);
}

// A context bound to one address (rmw_tickle's LOCALHOST range binds 127.0.0.1) keeps it, even on the limited
// broadcast whose route leaves by another interface.
static void test_a_bound_address_is_kept(void) {
    _tt_CONFIG.addr = "127.0.0.1";
    _tt_CONFIG.broadcast = "255.255.255.255";
    struct tt_Context node;
    EXPECT_TRUE(open_context(&node, OWNER_ID));
    uint32_t ip = 0;
    uint16_t port = 0;
    tt_own_address(&node, &ip, &port);
    EXPECT_EQ_U32(LOOPBACK_IP, ip);
    close_context(&node);
}

// The explicit path, unchanged: a directed broadcast an interface owns names that interface's address. Loopback owns
// 127.255.255.255 on every Linux host.
static void test_an_owned_broadcast_names_its_interface(void) {
    _tt_CONFIG.addr = "0.0.0.0";
    _tt_CONFIG.broadcast = "127.255.255.255";
    struct tt_Context node;
    EXPECT_TRUE(open_context(&node, OWNER_ID));
    uint32_t ip = 0;
    uint16_t port = 0;
    tt_own_address(&node, &ip, &port);
    EXPECT_EQ_U32(LOOPBACK_IP, ip);
    close_context(&node);
}

int main(void) {
    test_the_limited_broadcast_still_gives_an_address();
    test_a_same_host_peer_on_the_default_config_gets_shared_memory();
    test_a_foreign_peer_is_neither_same_host_nor_own();
    test_a_bound_address_is_kept();
    test_an_owned_broadcast_names_its_interface();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_own_address: all tests passed\n");
    return 0;
}
