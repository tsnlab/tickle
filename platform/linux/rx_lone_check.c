// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// The read after a wake-up's datagram is skipped once datagrams come one per wake-up (hal_linux.c
// rx_lone_after_wake()), and skipping it loses nothing, against the real Linux HAL, run by test_samehost.sh.
//
// Why (2026-10-15): on the core p1 ping-pong every drain after a wake-up asked the socket once more and found it empty,
// one recvmmsg() returning EAGAIN per round trip at each end (lat_gap_pc.sh on affea967).
//
// The check, on loopback only, each round being what a poll does - tt_receive() (the wait and its one read), then
// tt_try_receive() until it says nothing is waiting (the drain):
//   1. lone   - LONE_ROUNDS rounds of one datagram each. After TT_RX_LONE_STREAK of them every drain but one in
//               TT_RX_LONE_RECHECK must ask no socket at all (rx_idle_skips rises by 2, both sockets skipped). If none
//               does, the skip is not engaged (a build with TT_RX_LONE_STREAK=0 fails here: the check can fail).
//   2. burst  - BURST datagrams queued at once on the socket of a node in that state, then rounds until a wait times
//               out. Every datagram must come back, in order, none twice (the skipped read loses nothing because the
//               wait is level-triggered), and within TT_RX_LONE_RECHECK + 1 rounds: one datagram per wait for at most
//               TT_RX_LONE_RECHECK - 1 wake-ups, then the read that finds the backlog and the batches after it.
//   3. lone again - LONE_ROUNDS more single rounds: the skip must engage again.
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

#define LONE_ROUNDS 64           // well past TT_RX_LONE_STREAK + TT_RX_LONE_RECHECK
#define BURST 200                // queued at once; several batches (tt_RX_BATCH) deep
#define WAIT_NS 100000000        // a round's wait: 100 ms, far longer than loopback delivery
#define READ_BUFFER_BYTES 2048   // larger than anything sent here
#define CHECK_CONTEXT_ID 59      // any valid id; this context never meets another
#define LOOPBACK_SETTLE_US 20000 // the burst is all queued before the first wait reads it
#define RECHECK_ROUNDS 16        // TT_RX_LONE_RECHECK (hal_linux.c), which is not exported
#define BURST_FIRST 1000         // the burst's first sequence number
#define LONE_AGAIN_FIRST 5000    // the second lone phase's
#define STREAK_ROUNDS 8          // TT_RX_LONE_STREAK (hal_linux.c)

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_SETUP_FAILED = 3 };

static bool send_seq(int socket_fd, uint32_t seq, int sender) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof dest;
    if (getsockname(socket_fd, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sendto(sender, &seq, sizeof seq, 0, (struct sockaddr*)&dest, sizeof dest) == (ssize_t)sizeof seq;
}

// One poll's worth: the wait and its read, then the drain. Appends every datagram's sequence number to seen[] (up to
// cap), returns how many the round read, -1 when the wait timed out. *skipped_all: the drain asked no socket.
static int round_trip(struct tt_Context* node, uint32_t* seen, int* count, int cap, bool* skipped_all) {
    uint8_t buf[READ_BUFFER_BYTES];
    uint32_t ip = 0;
    uint16_t port = 0;
    int32_t got = tt_receive(node, buf, sizeof buf, &ip, &port, WAIT_NS);
    if (got < 0) {
        return -1;
    }
    int read = 0;
    const uint64_t skips_before = node->hal.rx_idle_skips;
    const uint64_t empty_before = node->hal.rx_empty_reads;
    do {
        if (got == (int32_t)sizeof(uint32_t) && *count < cap) {
            memcpy(&seen[(*count)++], buf, sizeof(uint32_t));
        }
        read++;
        got = tt_try_receive(node, buf, sizeof buf, &ip, &port);
    } while (got >= 0);
    // The drain's last call skipped both sockets: it made no read (a read that found nothing would have marked only
    // the one it asked, and the other was skipped since the wait did not report it).
    *skipped_all = node->hal.rx_idle_skips - skips_before >= 2 && node->hal.rx_empty_reads == empty_before && read == 1;
    return read;
}

// LONE_ROUNDS single-datagram rounds: how many drains skipped every socket after the streak was reached.
static int lone_rounds(struct tt_Context* node, int sender, uint32_t base, int* skipped) {
    uint32_t seen[2];
    *skipped = 0;
    for (int i = 0; i < LONE_ROUNDS; i++) {
        if (!send_seq(node->hal.data_sock, base + (uint32_t)i, sender)) {
            return -1;
        }
        int count = 0;
        bool skipped_all = false;
        if (round_trip(node, seen, &count, 2, &skipped_all) != 1 || count != 1 || seen[0] != base + (uint32_t)i) {
            printf("rx_lone_check: FAIL - lone round %d did not read exactly its datagram\n", i);
            return -2;
        }
        *skipped += skipped_all ? 1 : 0;
    }
    return 0;
}

static enum check_exit judge_lone(const char* phase, int skipped) {
    // Rounds left after the streak, of which one in RECHECK_ROUNDS still reads.
    const int after = LONE_ROUNDS - STREAK_ROUNDS;
    const int least = after - (after / RECHECK_ROUNDS) - 2;
    if (skipped < least) {
        printf("rx_lone_check: FAIL - %s: %d of %d drains skipped the read after the wake-up (need %d): the skip is "
               "not engaged\n",
               phase, skipped, LONE_ROUNDS, least);
        return CHECK_FAIL;
    }
    printf("rx_lone_check: %s: %d of %d drains made no read\n", phase, skipped, LONE_ROUNDS);
    return CHECK_PASS;
}

static enum check_exit burst(struct tt_Context* node, int sender) {
    static uint32_t seen[BURST + 1];
    int count = 0;
    for (uint32_t i = 0; i < BURST; i++) {
        if (!send_seq(node->hal.data_sock, BURST_FIRST + i, sender)) {
            printf("rx_lone_check: SETUP FAILED - could not queue burst datagram %u\n", i);
            return CHECK_SETUP_FAILED;
        }
    }
    usleep(LOOPBACK_SETTLE_US);
    int rounds = 0;
    int last = 0;
    bool skipped_all = false;
    while ((last = round_trip(node, seen, &count, BURST + 1, &skipped_all)) > 0) {
        rounds++;
    }
    if (count != BURST) {
        printf("rx_lone_check: FAIL - burst: %d of %d datagrams came back\n", count, BURST);
        return CHECK_FAIL;
    }
    for (int i = 0; i < BURST; i++) {
        if (seen[i] != BURST_FIRST + (uint32_t)i) {
            printf("rx_lone_check: FAIL - burst: datagram %d is %u, not %u\n", i, seen[i], BURST_FIRST + (uint32_t)i);
            return CHECK_FAIL;
        }
    }
    if (rounds > RECHECK_ROUNDS + 1) {
        printf("rx_lone_check: FAIL - burst: %d rounds to read %d datagrams (at most %d: one per wait until the next "
               "read after a wake-up, then batches)\n",
               rounds, BURST, RECHECK_ROUNDS + 1);
        return CHECK_FAIL;
    }
    printf("rx_lone_check: burst: all %d datagrams, in order, in %d rounds\n", BURST, rounds);
    return CHECK_PASS;
}

int main(void) {
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_lone_check: SETUP FAILED - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    enum check_exit result = CHECK_SETUP_FAILED;
    int skipped = 0;
    if (sender < 0) {
        printf("rx_lone_check: SETUP FAILED - no sender socket\n");
        goto out;
    }
    int ret = lone_rounds(&node, sender, 0, &skipped);
    if (ret != 0) {
        result = ret == -1 ? CHECK_SETUP_FAILED : CHECK_FAIL;
        goto out;
    }
    result = judge_lone("lone", skipped);
    if (result != CHECK_PASS) {
        goto out;
    }
    result = burst(&node, sender);
    if (result != CHECK_PASS) {
        goto out;
    }
    ret = lone_rounds(&node, sender, LONE_AGAIN_FIRST, &skipped);
    if (ret != 0) {
        result = ret == -1 ? CHECK_SETUP_FAILED : CHECK_FAIL;
        goto out;
    }
    result = judge_lone("lone again", skipped);
    if (result == CHECK_PASS) {
        printf("rx_lone_check: PASS\n");
    }

out:
    if (sender >= 0) {
        (void)close(sender);
    }
    tt_Context_destroy(&node);
    return (int)result;
}
