// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// A recvmmsg() batch's datagrams 2..N are handed to core where they lie (hal_linux.c rx_take_pending(),
// struct tt_Context.rx_in_place) rather than copied into the receive buffer, against the real Linux HAL, on loopback.
//
// Each phase queues BURST datagrams of different lengths and contents, then reads them as a poll does - tt_receive(),
// then tt_try_receive() until nothing is waiting - always into the buffer named in the phase:
//   1. rows   - into the context's rx_buffer with rx_rows_in_place set (core's own receive). Every datagram must read
//               back whole and in order from rx_in_place when it is set and from rx_buffer otherwise; rx_in_place must
//               be 8-aligned; and at least one must have come in place, or the path is not engaged and the check could
//               not fail for the reason it exists.
//   2. off    - the same with rx_rows_in_place cleared (a lending pool attached): none in place, all whole in
//   rx_buffer.
//   3. other  - rx_rows_in_place set but another buffer passed: none in place, all whole in that buffer.
//
// Exit 0 pass, 1 fail, 2 a build without in-place rows (TT_HAL_RX_IN_PLACE 0: nothing to check), 3 setup failed.
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

#define BURST 60                 // queued at once: more than one tt_RX_BATCH batch, within the socket buffer
#define WAIT_NS 100000000        // a wait: 100 ms, far longer than loopback delivery
#define CHECK_CONTEXT_ID 61      // any valid id; this context never meets another
#define LOOPBACK_SETTLE_US 20000 // the burst is all queued before the first wait reads it
#define MAX_LENGTH 1400          // within tt_MAX_BUFFER_LENGTH
#define LENGTH_STEP 13           // lengths 8, 21, 34, ...: no two neighbours alike

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_NOT_BUILT = 2, CHECK_SETUP_FAILED = 3 };

static uint32_t length_of(uint32_t seq) {
    return 8U + ((seq * LENGTH_STEP) % (MAX_LENGTH - 8U));
}

static uint8_t byte_of(uint32_t seq, uint32_t i) {
    return (uint8_t)((seq * 31U) + (i * 7U) + 1U);
}

static bool send_seq(int socket_fd, uint32_t seq, int sender) {
    uint8_t datagram[MAX_LENGTH];
    uint32_t length = length_of(seq);
    memcpy(datagram, &seq, sizeof seq);
    for (uint32_t i = sizeof seq; i < length; i++) {
        datagram[i] = byte_of(seq, i);
    }
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof dest;
    if (getsockname(socket_fd, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sendto(sender, datagram, length, 0, (struct sockaddr*)&dest, sizeof dest) == (ssize_t)length;
}

// Whether `got` bytes at `at` are datagram `seq` exactly.
static bool is_datagram(const uint8_t* at, int32_t got, uint32_t seq) {
    uint32_t read_seq = 0;
    if (got != (int32_t)length_of(seq)) {
        return false;
    }
    memcpy(&read_seq, at, sizeof read_seq);
    if (read_seq != seq) {
        return false;
    }
    for (uint32_t i = sizeof seq; i < (uint32_t)got; i++) {
        if (at[i] != byte_of(seq, i)) {
            return false;
        }
    }
    return true;
}

// One phase: BURST datagrams from `first`, read into `buf`. Returns how many came in place, or -1 on a failure (said).
static int phase(struct tt_Context* node, int sender, uint32_t first, uint8_t* buf, size_t len, const char* name) {
    for (uint32_t i = 0; i < BURST; i++) {
        if (!send_seq(node->hal.data_sock, first + i, sender)) {
            printf("rx_rows_check: SETUP FAILED - %s: could not queue datagram %u\n", name, i);
            return -2;
        }
    }
    usleep(LOOPBACK_SETTLE_US);
    uint32_t ip = 0;
    uint16_t port = 0;
    uint32_t next = first;
    int in_place = 0;
    int32_t got = tt_receive(node, buf, len, &ip, &port, WAIT_NS);
    while (got >= 0) {
        const uint8_t* at = node->rx_in_place != NULL ? node->rx_in_place : buf;
        if (node->rx_in_place != NULL) {
            in_place++;
            if (((uintptr_t)node->rx_in_place % 8U) != 0) {
                printf("rx_rows_check: FAIL - %s: datagram %u handed out in place at %p, not 8-aligned\n", name, next,
                       (const void*)node->rx_in_place);
                return -1;
            }
        }
        if (next >= first + BURST || !is_datagram(at, got, next)) {
            printf("rx_rows_check: FAIL - %s: datagram %u (%s, %d bytes) is not what was sent\n", name, next,
                   node->rx_in_place != NULL ? "in place" : "in the buffer", (int)got);
            return -1;
        }
        next++;
        got = tt_try_receive(node, buf, len, &ip, &port);
        if (got < 0 && next < first + BURST) {
            got = tt_receive(node, buf, len, &ip, &port, WAIT_NS);
        }
    }
    if (next != first + BURST) {
        printf("rx_rows_check: FAIL - %s: %u of %d datagrams came back\n", name, next - first, BURST);
        return -1;
    }
    printf("rx_rows_check: %s: all %d datagrams whole and in order, %d of them in place\n", name, BURST, in_place);
    return in_place;
}

int main(void) {
#if !TT_HAL_RX_IN_PLACE
    printf("rx_rows_check: not built with in-place rows (TT_HAL_RX_IN_PLACE 0)\n");
    return CHECK_NOT_BUILT;
#else
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    static struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_rows_check: SETUP FAILED - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    enum check_exit result = CHECK_SETUP_FAILED;
    static uint8_t other[tt_MAX_BUFFER_LENGTH];
    if (sender < 0) {
        printf("rx_rows_check: SETUP FAILED - no sender socket\n");
        goto out;
    }
    if (!node.rx_rows_in_place) {
        printf("rx_rows_check: FAIL - a new context does not ask for rows in place\n");
        result = CHECK_FAIL;
        goto out;
    }
    int rows = phase(&node, sender, 0, node.rx_buffer, tt_MAX_BUFFER_LENGTH, "rows");
    if (rows < 0) {
        result = rows == -2 ? CHECK_SETUP_FAILED : CHECK_FAIL;
        goto out;
    }
    if (rows == 0) {
        printf("rx_rows_check: FAIL - rows: no datagram came in place: the path is not engaged\n");
        result = CHECK_FAIL;
        goto out;
    }
    node.rx_rows_in_place = false;
    int off = phase(&node, sender, 1000, node.rx_buffer, tt_MAX_BUFFER_LENGTH, "off");
    node.rx_rows_in_place = true;
    int elsewhere = off < 0 ? off : phase(&node, sender, 2000, other, sizeof other, "other");
    if (off < 0 || elsewhere < 0) {
        result = (off == -2 || elsewhere == -2) ? CHECK_SETUP_FAILED : CHECK_FAIL;
        goto out;
    }
    if (off != 0 || elsewhere != 0) {
        printf("rx_rows_check: FAIL - %d datagrams in place with rows not asked for, %d into another buffer\n", off,
               elsewhere);
        result = CHECK_FAIL;
        goto out;
    }
    result = CHECK_PASS;
    printf("rx_rows_check: PASS\n");

out:
    if (sender >= 0) {
        (void)close(sender);
    }
    tt_Context_destroy(&node);
    return (int)result;
#endif
}
