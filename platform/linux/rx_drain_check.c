// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// A receive socket's drops, against the real Linux HAL, run by test_samehost.sh.
//
// Why (2026-10-11): rig cell I4s (RESULTS L4, 4 MB best effort over a saturated 1 GbE link) lost 14-38 samples a run
// on the subscriber's side, and nothing in the run could say where - the socket, the NIC or the wire. The HAL now
// reads the kernel's own count for each socket (tt_hal_rx_socket_drops(), the "Node N rx:" line). This checks that the
// count is the socket's, on loopback only:
//   1. control - a fresh context's count is 0, and reading a burst that fits loses nothing and counts nothing.
//   2. drops   - the data socket's buffer shrunk to the kernel's minimum, BURST datagrams sent without reading: some
//                must be lost (else the scenario was not built: exit 3), and the count must rise by exactly the
//                number that did not arrive.
//
// Exit 0 pass, 1 fail, 3 setup failed.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h>
#include <tickle/tickle.h>

#define BURST 200               // datagrams sent without reading; the minimum buffer holds a few
#define FITS 4                  // a burst the default buffer takes whole
#define PAYLOAD_BYTES 1000      // below the control datagram, as a fragment's
#define READ_BUFFER_BYTES 2048  // larger than anything sent here
#define CHECK_CONTEXT_ID 59     // any valid id; this context never meets another
#define LOOPBACK_SETTLE_US 1000 // loopback delivery is immediate in practice; this only removes the doubt
#define MARK 0xD5               // the first byte of every datagram this check sends, so its own are told apart

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_SETUP_FAILED = 3 };

static bool send_burst(int sender, int socket_fd, int count) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof dest;
    if (getsockname(socket_fd, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    uint8_t payload[PAYLOAD_BYTES];
    memset(payload, MARK, sizeof payload);
    for (int i = 0; i < count; i++) {
        if (sendto(sender, payload, sizeof payload, 0, (struct sockaddr*)&dest, sizeof dest) !=
            (ssize_t)sizeof payload) {
            return false;
        }
    }
    usleep(LOOPBACK_SETTLE_US);
    return true;
}

// Reads until nothing is waiting; returns how many of this check's own datagrams came.
static int read_all(struct tt_Context* node) {
    uint8_t buf[READ_BUFFER_BYTES];
    uint32_t ip = 0;
    uint16_t port = 0;
    int mine = 0;
    int32_t got;
    while ((got = tt_try_receive(node, buf, sizeof buf, &ip, &port)) >= 0) {
        if (got == PAYLOAD_BYTES && buf[0] == MARK) {
            mine++;
        }
    }
    return mine;
}

int main(void) {
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_drain_check: SETUP FAILED - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    enum check_exit result = CHECK_SETUP_FAILED;
    if (sender < 0) {
        printf("rx_drain_check: SETUP FAILED - no sender socket\n");
        goto out;
    }

    // 1. control
    (void)read_all(&node);
    uint32_t before = tt_hal_rx_socket_drops(&node);
    if (!send_burst(sender, node.hal.data_sock, FITS)) {
        printf("rx_drain_check: SETUP FAILED - could not send the control burst\n");
        goto out;
    }
    int fit = read_all(&node);
    if (before != 0 || fit != FITS || tt_hal_rx_socket_drops(&node) != 0) {
        printf(
            "rx_drain_check: FAIL - control: drops %u before, %d of %d read, drops %u after (all must be 0 and %d)\n",
            (unsigned)before, fit, FITS, (unsigned)tt_hal_rx_socket_drops(&node), FITS);
        result = CHECK_FAIL;
        goto out;
    }

    // 2. drops
    int smallest = 1; // the kernel raises it to its minimum
    if (setsockopt(node.hal.data_sock, SOL_SOCKET, SO_RCVBUF, &smallest, sizeof smallest) != 0 ||
        !send_burst(sender, node.hal.data_sock, BURST)) {
        printf("rx_drain_check: SETUP FAILED - could not shrink the buffer or send the burst\n");
        goto out;
    }
    int arrived = read_all(&node);
    uint32_t dropped = tt_hal_rx_socket_drops(&node);
    if (arrived >= BURST) {
        printf("rx_drain_check: SETUP FAILED - the shrunk buffer took all %d datagrams, nothing to count\n", BURST);
        goto out;
    }
    if (dropped != (uint32_t)(BURST - arrived)) {
        printf("rx_drain_check: FAIL - drops: %d of %d arrived, the count says %u dropped (must be %d)\n", arrived,
               BURST, (unsigned)dropped, BURST - arrived);
        result = CHECK_FAIL;
        goto out;
    }
    printf("rx_drain_check: PASS - drops: %d of %d arrived through the smallest buffer and the count is the other %u\n",
           arrived, BURST, (unsigned)dropped);
    result = CHECK_PASS;

out:
    if (sender >= 0) {
        (void)close(sender);
    }
    tt_Context_destroy(&node);
    return (int)result;
}
