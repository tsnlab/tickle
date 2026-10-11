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
// count is the socket's, and that the drain (tt_rx_drain_start(), hal.h) keeps it at 0, on loopback only:
//   1. control - a fresh context's count is 0, and reading a burst that fits loses nothing and counts nothing.
//   2. drops   - the data socket's buffer shrunk to the kernel's minimum, BURST datagrams sent without reading: some
//                must be lost (else the scenario was not built: exit 3), and the count must rise by exactly the
//                number that did not arrive. This is also the drain's control: the same burst, nothing draining.
//   3. drain   - the same burst through the same buffer with a drain started first: every datagram arrives, in the
//                order sent, and the count does not move. LATE more sent after the first read come last: the drain
//                goes on taking while it holds datagrams, and stops - before any socket read - only once it holds none.
//   4. cut     - a datagram longer than a drain buffer is dropped and counted (drain_truncated), and no drain starts
//                until tt_RX_DRAIN_HOLDOFF_NS has passed, so the same datagram sent again is read whole.
//   5. sized   - asked for 65,507-byte datagrams, the drain rebuilds its buffers for them, and a burst of them through
//   the
//                smallest socket buffer arrives whole.
// The drain's task work runs here on this thread's own sendto() returns; rx_drain_pc.sh is the check that it runs
// while a thread is busy in user space. Exit 0 pass, 1 fail, 2 no drain (io_uring refused, or a kernel before 6.0),
// 3 setup failed.
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
#define LATE 1                  // sent after a drain stopped: the smallest socket buffer holds one
#define CUT_BYTES 3000          // longer than a drain buffer for 1,472-byte datagrams
#define BIG_BYTES 60000         // a datagram the kernel would reassemble on a real link
#define BIG_BURST 24            // more than the smallest socket buffer holds of them
#define HOLDOFF_PAST_US ((useconds_t)(tt_RX_DRAIN_HOLDOFF_NS / 1000U) + 10000U)

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_NO_DRAIN = 2, CHECK_SETUP_FAILED = 3 };

// Datagrams of `bytes`, each its index from `first` in bytes 1-4 after the mark.
static bool send_numbered(int sender, int socket_fd, uint32_t first, int count, size_t bytes) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof dest;
    if (getsockname(socket_fd, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static uint8_t payload[BIG_BYTES];
    memset(payload, MARK, bytes);
    for (int i = 0; i < count; i++) {
        uint32_t index = first + (uint32_t)i;
        memcpy(payload + 1, &index, sizeof index);
        if (sendto(sender, payload, bytes, 0, (struct sockaddr*)&dest, sizeof dest) != (ssize_t)bytes) {
            return false;
        }
    }
    return true;
}

// Reads one of this check's datagrams of `bytes`, or returns false when nothing more of them is waiting.
static bool read_numbered(struct tt_Context* node, size_t bytes, uint32_t* index) {
    static uint8_t buf[BIG_BYTES + 16];
    uint32_t ip = 0;
    uint16_t port = 0;
    int32_t got;
    while ((got = tt_try_receive(node, buf, sizeof buf, &ip, &port)) >= 0) {
        if ((size_t)got == bytes && buf[0] == MARK) {
            memcpy(index, buf + 1, sizeof *index);
            return true;
        }
    }
    return false;
}

// Reads every datagram of `bytes` waiting; true when they are exactly first..first + count - 1 in order.
static bool read_in_order(struct tt_Context* node, size_t bytes, uint32_t first, int count, int* arrived) {
    uint32_t index = 0;
    bool in_order = true;
    *arrived = 0;
    while (read_numbered(node, bytes, &index)) {
        in_order = in_order && index == first + (uint32_t)*arrived;
        (*arrived)++;
    }
    return in_order && *arrived == count;
}

// 3, 4 and 5; the data socket's buffer is already at its smallest.
static enum check_exit check_drain(struct tt_Context* node, int sender) {
    uint32_t drops = tt_hal_rx_socket_drops(node);
    if (!tt_rx_drain_start(node, tt_CONTROL_MAX_LENGTH)) {
        printf("rx_drain_check: NO DRAIN - tt_rx_drain_start() refused (io_uring refused, or Linux < 6.0)\n");
        return CHECK_NO_DRAIN;
    }
    // 3. drain
    if (!send_numbered(sender, node->hal.data_sock, 0, BURST, PAYLOAD_BYTES)) {
        printf("rx_drain_check: SETUP FAILED - could not send the drained burst\n");
        return CHECK_SETUP_FAILED;
    }
    uint32_t index = 0;
    bool first_ok = read_numbered(node, PAYLOAD_BYTES, &index) && index == 0; // stops the drain
    if (!send_numbered(sender, node->hal.data_sock, BURST, LATE, PAYLOAD_BYTES)) {
        printf("rx_drain_check: SETUP FAILED - could not send the late datagrams\n");
        return CHECK_SETUP_FAILED;
    }
    usleep(LOOPBACK_SETTLE_US);
    int arrived = 0;
    bool in_order = read_in_order(node, PAYLOAD_BYTES, 1, BURST + LATE - 1, &arrived);
    if (!first_ok || !in_order || tt_hal_rx_socket_drops(node) != drops) {
        printf("rx_drain_check: FAIL - drain: first read %s, then %d of %d in order: %s, socket drops %u -> %u (must "
               "not move); drain_datagrams %lu\n",
               first_ok ? "index 0" : "not index 0", arrived, BURST + LATE - 1, in_order ? "yes" : "no",
               (unsigned)drops, (unsigned)tt_hal_rx_socket_drops(node), (unsigned long)node->hal.drain_datagrams);
        return CHECK_FAIL;
    }
    printf("rx_drain_check: PASS - drain: %d datagrams through the smallest buffer, %lu of them from the drain, all in "
           "order, the %d sent after the first read last, no socket drop\n",
           BURST + LATE, (unsigned long)node->hal.drain_datagrams, LATE);

    // 4. cut
    uint64_t cut_before = node->hal.drain_truncated;
    if (!tt_rx_drain_start(node, tt_CONTROL_MAX_LENGTH) ||
        !send_numbered(sender, node->hal.data_sock, 0, 1, CUT_BYTES)) {
        printf("rx_drain_check: SETUP FAILED - could not start the drain or send the long datagram\n");
        return CHECK_SETUP_FAILED;
    }
    bool cut_read = read_numbered(node, CUT_BYTES, &index);
    bool restarted = tt_rx_drain_start(node, tt_CONTROL_MAX_LENGTH);
    if (!send_numbered(sender, node->hal.data_sock, 1, 1, CUT_BYTES)) {
        printf("rx_drain_check: SETUP FAILED - could not send the long datagram again\n");
        return CHECK_SETUP_FAILED;
    }
    bool again_read = read_numbered(node, CUT_BYTES, &index) && index == 1;
    if (cut_read || node->hal.drain_truncated != cut_before + 1 || restarted || !again_read) {
        printf("rx_drain_check: FAIL - cut: the long datagram %s, drain_truncated %lu -> %lu (must rise by 1), a drain "
               "%s during the hold-off, the retry %s\n",
               cut_read ? "was handed out" : "was dropped", (unsigned long)cut_before,
               (unsigned long)node->hal.drain_truncated, restarted ? "started" : "did not start",
               again_read ? "was read whole" : "was not read");
        return CHECK_FAIL;
    }
    printf("rx_drain_check: PASS - cut: a %d-byte datagram was dropped and counted while draining, and its retry read "
           "whole during the hold-off\n",
           CUT_BYTES);

    // 5. sized
    usleep(HOLDOFF_PAST_US);
    drops = tt_hal_rx_socket_drops(node);
    if (!tt_rx_drain_start(node, tt_MAX_BUFFER_LENGTH)) {
        printf("rx_drain_check: FAIL - sized: no drain for %d-byte datagrams after the hold-off\n",
               (int)tt_MAX_BUFFER_LENGTH);
        return CHECK_FAIL;
    }
    if (!send_numbered(sender, node->hal.data_sock, 0, BIG_BURST, BIG_BYTES)) {
        printf("rx_drain_check: SETUP FAILED - could not send the big burst\n");
        return CHECK_SETUP_FAILED;
    }
    usleep(LOOPBACK_SETTLE_US);
    in_order = read_in_order(node, BIG_BYTES, 0, BIG_BURST, &arrived);
    if (!in_order || tt_hal_rx_socket_drops(node) != drops || node->hal.drain_size < BIG_BYTES) {
        printf(
            "rx_drain_check: FAIL - sized: %d of %d big datagrams in order: %s, socket drops %u -> %u, buffers of %u "
            "bytes\n",
            arrived, BIG_BURST, in_order ? "yes" : "no", (unsigned)drops, (unsigned)tt_hal_rx_socket_drops(node),
            (unsigned)node->hal.drain_size);
        return CHECK_FAIL;
    }
    printf("rx_drain_check: PASS - sized: %d datagrams of %d bytes through the smallest buffer, into %u buffers of %u "
           "bytes\n",
           BIG_BURST, BIG_BYTES, (unsigned)node->hal.drain_count, (unsigned)node->hal.drain_size);
    return CHECK_PASS;
}

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
    result = check_drain(&node, sender);

out:
    if (sender >= 0) {
        (void)close(sender);
    }
    tt_Context_destroy(&node);
    return (int)result;
}
