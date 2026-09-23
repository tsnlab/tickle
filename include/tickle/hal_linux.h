/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

#include <stdbool.h> // rx_prefer_data

#include <netinet/in.h>

// Linux-specific hardware abstraction layer structure
struct tt_hal {
    // The well-known port (_tt_CONFIG.port), shared with every other node on this host via
    // SO_REUSEADDR. Broadcasts are addressed here, so this is how a node is reached before anyone
    // knows anything about it. Receive-only in practice: nothing is ever sent from it.
    int sock;
    // This node's own data port, bound to whatever the kernel hands out. Everything is sent from
    // here, which is what makes a peer addressable: upsert_peer() records a peer at the source
    // port of the packet it was heard on, so every peer learns this node's own port for free and
    // a unicast can then reach this node specifically.
    //
    // Why it has to exist (2026-09-23, measured, not reasoned): with every node bound only to the
    // shared port, two nodes on one host share an address, and a unicast to it is delivered by the
    // kernel to exactly one of the two sockets - which may be the sender's own. Measured directly
    // with two sockets bound to one UDP port with SO_REUSEADDR: a broadcast reaches both, a
    // unicast reaches one. In the rmw_tickle benchmark that showed up as a Publisher receiving
    // 10009 of its own 10010 datagrams while its Subscriber got 25, and - far worse - as roughly
    // 10% of the stream going to the sender in runs that passed and were recorded as clean.
    int data_sock;
    struct sockaddr_in broadcast_addr; // Precomputed once in tt_bind(), reused by every tt_send()
    // eventfd(2): tt_receive()'s poll() watches this alongside sock, and tt_wake_signal() writes
    // to it to interrupt a blocked receive. Deliberately not a loopback UDP socket the way
    // hal_freertos.h's own wake_sock is (the two HALs otherwise mirror each other closely) -
    // platform/linux/test.sh runs each side in its own network namespace with only the veth pair
    // brought up, not `lo` (see netns.mk), so binding anything to 127.0.0.1 there fails with
    // EADDRNOTAVAIL. eventfd needs no address or interface at all. -1 before tt_bind() creates it
    // (or if creation fails partway through), so tt_close() knows not to close it.
    int wake_fd;
    // Which socket gets first refusal on the next read, alternating. Without it, preferring one
    // socket whenever both are ready is not merely a delay: under a sustained stream on the
    // preferred socket the other is never read at all. That matters most exactly where it is
    // least visible - a Publisher above tt_UNICAST_PEER_THRESHOLD broadcasts its data while the
    // ACKNACKs and retransmit requests answering it arrive as unicast on the data socket, so the
    // starved path would be RELIABLE recovery. Alternating bounds the wait at one datagram.
    bool rx_prefer_data;
};
