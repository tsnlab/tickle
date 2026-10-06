// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// A busy socket must not starve one tt_try_receive() has marked idle, against the real Linux HAL, run by
// test_samehost.sh.
//
// Real failure (2026-10-05): a max-rate broadcasting rmw_tickle publisher's own looped-back datagrams kept its
// well-known socket from ever reading empty, so drain_rx() never returned, and the data socket - marked idle when the
// session began - was never read again. Every discovery reply and ACKNACK arrives there, so the publisher never matched
// its subscriber, and KEEP_ALL, with nobody to wait for, evicted ~5% of samples under 5% loss on the rig.
//
// The check, on loopback only:
//   1. setup     - queue FLOOD datagrams on the well-known socket and read until the data socket alone is marked idle.
//                  If that state is never reached, the scenario was not built and the run says so (exit 3).
//   2. starve    - send one probe to the data socket and keep reading the backlog, moving the poll's clock
//                  (tt_Context.rx_clock_ns, which drain_rx() keeps in a real poll) on by READ_STEP_NS per read. The
//                  data socket must stop being skipped within RECHECK_READS + 1 reads, because the recheck is a
//                  time, TT_RX_IDLE_RECHECK_NS (hal_linux.c); and the probe must come back within PROBE_WITHIN,
//                  since at most two batches then stand ahead of it - the one already read, and one more from the
//                  busy socket if it is its turn. Before the fix the probe came back only after the whole flood;
//                  with the count of 64 datagrams the recheck had until 2026-10-06, the data socket stayed skipped
//                  until read 64, which the first criterion fails and the second, batches being 32, does not see.
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

#define FLOOD 300       // well-known datagrams queued ahead of the probe; well past the recheck interval
#define RECHECK_READS 4 // reads per TT_RX_IDLE_RECHECK_NS: each read stands for a quarter of it
#define READ_STEP_NS ((uint64_t)tt_RECEIVE_TIMEOUT / RECHECK_READS) // the period hal_linux.c rechecks after
#define PROBE_WITHIN (RECHECK_READS + 1 + (2 * tt_RX_BATCH) + 1)    // the recheck, then two batches, then the probe
#define SETUP_TRIES 8           // attempts to get the data socket marked idle (the read order alternates)
#define READ_BUFFER_BYTES 2048  // larger than anything sent here
#define CHECK_CONTEXT_ID 58     // any valid id; this context never meets another
#define LOOPBACK_SETTLE_US 1000 // loopback delivery is immediate in practice; this only removes the doubt

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_SETUP_FAILED = 3 };

static bool send_to_socket(int socket_fd, const char* payload, size_t len, int sender) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof dest;
    if (getsockname(socket_fd, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sendto(sender, payload, len, 0, (struct sockaddr*)&dest, sizeof dest) == (ssize_t)len;
}

// Reads until nothing is waiting, which also clears every idle bit.
static void drain_all(struct tt_Context* node) {
    uint8_t buf[READ_BUFFER_BYTES];
    uint32_t ip = 0;
    uint16_t port = 0;
    while (tt_try_receive(node, buf, sizeof buf, &ip, &port) >= 0) {
    }
}

// 2. starve, after the probe is queued: reads the backlog, moving the poll's clock on by READ_STEP_NS per read, and
// judges when the data socket stopped being skipped and when the probe came back.
static enum check_exit starve(struct tt_Context* node, const char* probe, size_t probe_len) {
    uint8_t buf[READ_BUFFER_BYTES];
    uint32_t ip = 0;
    uint16_t port = 0;
    int reads = 0;
    int seen_at = -1;
    int unskipped_at = -1;
    while (reads < FLOOD + 1) { // a -1 before the probe is the whole backlog read with the probe still unread
        int32_t got = tt_try_receive(node, buf, sizeof buf, &ip, &port);
        if (got < 0) {
            break; // everything read: the well-known socket ran empty
        }
        reads++;
        node->rx_clock_ns += READ_STEP_NS;
        if (unskipped_at < 0 && (node->hal.rx_idle & TT_RX_IDLE_DATA) == 0) {
            unskipped_at = reads;
        }
        if ((size_t)got == probe_len && memcmp(buf, probe, probe_len) == 0) {
            seen_at = reads;
            break;
        }
    }
    if (seen_at < 0) {
        printf("rx_idle_check: FAIL - the probe on the idle data socket never came back in %d reads\n", reads);
        return CHECK_FAIL;
    }
    if (unskipped_at < 0 || unskipped_at > RECHECK_READS + 1) {
        printf("rx_idle_check: FAIL - the idle data socket was asked again only at read %d (-1: never), reads of %llu "
               "ns each (must be by read %d)\n",
               unskipped_at, (unsigned long long)READ_STEP_NS, RECHECK_READS + 1);
        return CHECK_FAIL;
    }
    if (seen_at > PROBE_WITHIN) {
        printf("rx_idle_check: FAIL - the probe came back at read %d of %d, behind the busy socket's backlog "
               "(must be within %d)\n",
               seen_at, FLOOD + 1, PROBE_WITHIN);
        return CHECK_FAIL;
    }
    printf("rx_idle_check: PASS - the idle data socket was asked again at read %d and its probe read at %d, with %d "
           "datagrams queued ahead on the busy one\n",
           unskipped_at, seen_at, FLOOD);
    return CHECK_PASS;
}

int main(void) {
    static const char filler[] = "rx_idle_check filler";
    static const char probe[] = "rx_idle_check probe";
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_idle_check: FAIL - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    enum check_exit result = CHECK_SETUP_FAILED;
    uint8_t buf[READ_BUFFER_BYTES];
    uint32_t ip = 0;
    uint16_t port = 0;

    // 1. setup: a backlog on the well-known socket, then reads until one has found the data socket empty (and so
    // skips it) while the well-known one, still holding most of the backlog, is not skipped - the state the publisher
    // was in. The read order alternates, so it takes one or two reads.
    // The poll's clock, set by hand from here on: the HAL keeps time by it (0 would make it read the real clock).
    node.rx_clock_ns = tt_SECOND;
    drain_all(&node);
    for (int i = 0; i < FLOOD && sender >= 0; i++) {
        if (!send_to_socket(node.hal.sock, filler, sizeof filler, sender)) {
            printf("rx_idle_check: SETUP FAILED - could not queue filler %d\n", i);
            goto out;
        }
    }
    usleep(LOOPBACK_SETTLE_US); // loopback delivery is immediate in practice; this only removes the doubt
    bool data_idle = false;
    for (int i = 0; i < SETUP_TRIES && !data_idle; i++) {
        if (tt_try_receive(&node, buf, sizeof buf, &ip, &port) < 0) {
            break;
        }
        data_idle = node.hal.rx_idle == TT_RX_IDLE_DATA;
    }
    if (!data_idle) {
        printf("rx_idle_check: SETUP FAILED - never reached 'data socket skipped, well-known not' (rx_idle=%u), so "
               "starvation cannot be tested\n",
               (unsigned)node.hal.rx_idle);
        goto out;
    }

    // 2. starve
    if (!send_to_socket(node.hal.data_sock, probe, sizeof probe, sender)) {
        printf("rx_idle_check: SETUP FAILED - could not send the probe\n");
        goto out;
    }
    usleep(LOOPBACK_SETTLE_US);
    result = starve(&node, probe, sizeof probe);

out:
    if (sender >= 0) {
        (void)close(sender);
    }
    tt_Context_destroy(&node);
    return (int)result;
}
