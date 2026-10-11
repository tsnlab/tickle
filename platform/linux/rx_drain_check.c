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
//   5. sized   - asked for 65,507-byte datagrams, the drain rebuilds its buffers for them, and a burst of them
//                through the smallest socket buffer arrives whole.
//   6. merged  - receive offload on (UDP_GRO, the state core's reads put it in for MTU-mode fragments), a run of
//                same-size datagrams sent as one UDP_SEGMENT send, which loopback hands to the socket merged: the drain
//                takes it with UDP_GRO's size, its buffers sized for a merged read whatever core asked, and hands the
//                datagrams out one at a time, whole and in order - none cut, none dropped (drain_merged counts the
//                merged reads: 0 means loopback did not merge, and the check says so rather than pass on nothing).
//   7. rearm   - an overrun does not end the drain for the rest of its time: every buffer filled and one datagram more
//                (the overrun), then a few read, then more sent - those are drained too (the recvmsg submitted again as
//                soon as a buffer was back), not left to the smallest socket buffer, which would drop them. All
//                arrive, in order, and the socket's count does not move. Not with classic buffers, which come back
//                only at the next start (the check says so and passes on the rest).
//   8. ring    - the buffers reached the kernel the way TT_EXPECT_DRAIN_RING says, when it is set: test_samehost.sh
//                runs this under refuse_pbuf_ring.so, which holds every IORING_REGISTER_PBUF_RING to Linux 6.8's
//                rules and can refuse it as the rig's Pi 5s did (6.8.0-1064-raspi, EINVAL), so checks 3-5 also run
//                over the kernel's own ring and over classic provided buffers.
// The drain's task work runs here on this thread's own sendto() returns; rx_drain_pc.sh is the check that it runs
// while a thread is busy in user space. Exit 0 pass, 1 fail, 2 no drain (io_uring refused, or a kernel before 6.0),
// 3 setup failed.

// Built as rmw_tickle is (platform/linux/Makefile passes the same), so a lint of this file alone sees the drain too.
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507
#endif
#ifndef tt_LARGE_SAMPLES
#define tt_LARGE_SAMPLES 1
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> // getenv()
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/udp.h> // UDP_GRO, UDP_SEGMENT
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
#define MERGED_SEGMENTS 30   // datagrams in the one UDP_SEGMENT send of check 6
#define CHECK_GRO_ON 1U      // hal_linux.c's TT_GRO_ON: the state its reads leave once a large fragment came
#define MERGED_SEND_EXTRA 64 // room for the send's own UDP_SEGMENT cmsg
#define REARM_READ 24        // datagrams read after the overrun, each handing a buffer back: more than REARM_MORE
#define REARM_MORE 16        // sent after them: more than the smallest socket buffer holds

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

// 3. drain; the data socket's buffer is already at its smallest.
static enum check_exit check_drained_burst(struct tt_Context* node, int sender) {
    uint32_t drops = tt_hal_rx_socket_drops(node);
    if (!tt_rx_drain_start(node, tt_CONTROL_MAX_LENGTH)) {
        printf("rx_drain_check: NO DRAIN - tt_rx_drain_start() refused (io_uring refused, or Linux < 6.0)\n");
        return CHECK_NO_DRAIN;
    }
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
    return CHECK_PASS;
}

// 4. cut
static enum check_exit check_cut(struct tt_Context* node, int sender) {
    uint32_t index = 0;
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
    return CHECK_PASS;
}

// 5. sized
static enum check_exit check_sized(struct tt_Context* node, int sender) {
    usleep(HOLDOFF_PAST_US);
    uint32_t drops = tt_hal_rx_socket_drops(node);
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
    int arrived = 0;
    bool in_order = read_in_order(node, BIG_BYTES, 0, BIG_BURST, &arrived);
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

#if TT_HAL_UDP_GRO
// MERGED_SEGMENTS numbered datagrams of PAYLOAD_BYTES as one UDP_SEGMENT send, from `first`.
static bool send_segmented(int sender, int socket_fd, uint32_t first) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof dest;
    if (getsockname(socket_fd, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static uint8_t payload[MERGED_SEGMENTS * PAYLOAD_BYTES];
    memset(payload, MARK, sizeof payload);
    for (uint32_t i = 0; i < MERGED_SEGMENTS; i++) {
        uint32_t index = first + i;
        memcpy(payload + ((size_t)i * PAYLOAD_BYTES) + 1, &index, sizeof index);
    }
    // NOLINTNEXTLINE(misc-include-cleaner) - struct iovec: <sys/uio.h>, through <sys/socket.h>
    struct iovec iov = {.iov_base = payload, .iov_len = sizeof payload};
    union {
        char bytes[MERGED_SEND_EXTRA];
        struct cmsghdr align; // NOLINT(misc-include-cleaner) - <sys/socket.h>
    } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg = {0};
    msg.msg_name = &dest;
    msg.msg_namelen = sizeof dest;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = CMSG_SPACE(sizeof(uint16_t)); // NOLINT(misc-include-cleaner) - CMSG_*: <sys/socket.h>
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);          // NOLINT(misc-include-cleaner)
    cm->cmsg_level = SOL_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t)); // NOLINT(misc-include-cleaner)
    uint16_t segment = PAYLOAD_BYTES;
    memcpy(CMSG_DATA(cm), &segment, sizeof segment); // NOLINT(misc-include-cleaner)
    return sendmsg(sender, &msg, 0) == (ssize_t)sizeof payload;
}

// 6. merged
static enum check_exit check_merged(struct tt_Context* node, int sender) {
    usleep(HOLDOFF_PAST_US);
    int on = 1;
    if (setsockopt(node->hal.data_sock, SOL_UDP, UDP_GRO, &on, sizeof on) != 0) {
        printf("rx_drain_check: PASS - merged: not run, this kernel refuses UDP_GRO\n");
        return CHECK_PASS;
    }
    node->hal.gro_state = CHECK_GRO_ON;
    uint32_t drops = tt_hal_rx_socket_drops(node);
    uint64_t cut = node->hal.drain_truncated;
    uint64_t merged = node->hal.drain_merged;
    if (!tt_rx_drain_start(node, tt_CONTROL_MAX_LENGTH)) {
        printf("rx_drain_check: FAIL - merged: no drain with receive offload on\n");
        return CHECK_FAIL;
    }
    if (!send_segmented(sender, node->hal.data_sock, 0) ||
        !send_segmented(sender, node->hal.data_sock, MERGED_SEGMENTS)) {
        printf("rx_drain_check: SETUP FAILED - merged: the UDP_SEGMENT send was refused\n");
        return CHECK_SETUP_FAILED;
    }
    usleep(LOOPBACK_SETTLE_US);
    int arrived = 0;
    bool in_order = read_in_order(node, PAYLOAD_BYTES, 0, 2 * MERGED_SEGMENTS, &arrived);
    uint64_t merged_reads = node->hal.drain_merged - merged;
    if (merged_reads == 0) {
        printf("rx_drain_check: SETUP FAILED - merged: loopback handed the drain no merged read (%d of %d arrived "
               "alone), so nothing was split\n",
               arrived, 2 * MERGED_SEGMENTS);
        return CHECK_SETUP_FAILED;
    }
    if (!in_order || tt_hal_rx_socket_drops(node) != drops || node->hal.drain_truncated != cut ||
        node->hal.drain_size < (uint32_t)tt_MAX_BUFFER_LENGTH) {
        printf("rx_drain_check: FAIL - merged: %d of %d in order: %s, socket drops %u -> %u, drain_truncated %lu -> "
               "%lu, buffers of %u bytes (must hold a merged read)\n",
               arrived, 2 * MERGED_SEGMENTS, in_order ? "yes" : "no", (unsigned)drops,
               (unsigned)tt_hal_rx_socket_drops(node), (unsigned long)cut, (unsigned long)node->hal.drain_truncated,
               (unsigned)node->hal.drain_size);
        return CHECK_FAIL;
    }
    printf("rx_drain_check: PASS - merged: %d datagrams in %lu merged reads through the drain, split whole and in "
           "order, into buffers of %u bytes\n",
           arrived, (unsigned long)merged_reads, (unsigned)node->hal.drain_size);
    return CHECK_PASS;
}
#endif

// 7. rearm
static enum check_exit check_rearm(struct tt_Context* node, int sender) {
    if (node->hal.drain_ring == tt_RX_DRAIN_RING_CLASSIC) {
        printf("rx_drain_check: PASS - rearm: not run, classic buffers come back only at the next start\n");
        return CHECK_PASS;
    }
    uint32_t drops = tt_hal_rx_socket_drops(node);
    uint64_t overruns = node->hal.drain_overruns;
    uint64_t rearms = node->hal.drain_rearms;
    if (!tt_rx_drain_start(node, tt_MAX_BUFFER_LENGTH)) {
        printf("rx_drain_check: FAIL - rearm: no drain\n");
        return CHECK_FAIL;
    }
    int filled = (int)node->hal.drain_count + 1; // every buffer, and one more: the overrun
    if (!send_numbered(sender, node->hal.data_sock, 0, filled, PAYLOAD_BYTES)) {
        printf("rx_drain_check: SETUP FAILED - rearm: could not fill the buffers\n");
        return CHECK_SETUP_FAILED;
    }
    usleep(LOOPBACK_SETTLE_US);
    uint32_t index = 0;
    bool first_ok = true;
    for (uint32_t i = 0; i < REARM_READ; i++) {
        first_ok = first_ok && read_numbered(node, PAYLOAD_BYTES, &index) && index == i;
    }
    if (!send_numbered(sender, node->hal.data_sock, (uint32_t)filled, REARM_MORE, PAYLOAD_BYTES)) {
        printf("rx_drain_check: SETUP FAILED - rearm: could not send after the overrun\n");
        return CHECK_SETUP_FAILED;
    }
    usleep(LOOPBACK_SETTLE_US);
    int arrived = 0;
    bool in_order = read_in_order(node, PAYLOAD_BYTES, REARM_READ, filled + REARM_MORE - REARM_READ, &arrived);
    if (node->hal.drain_overruns == overruns) {
        printf("rx_drain_check: SETUP FAILED - rearm: %d datagrams into %u buffers made no overrun\n", filled,
               (unsigned)node->hal.drain_count);
        return CHECK_SETUP_FAILED;
    }
    if (!first_ok || !in_order || tt_hal_rx_socket_drops(node) != drops || node->hal.drain_rearms == rearms) {
        printf("rx_drain_check: FAIL - rearm: the first %d read %s, then %d of %d in order: %s, socket drops %u -> %u "
               "(must not move), re-arms %lu -> %lu\n",
               REARM_READ, first_ok ? "in order" : "not in order", arrived, filled + REARM_MORE - REARM_READ,
               in_order ? "yes" : "no", (unsigned)drops, (unsigned)tt_hal_rx_socket_drops(node), (unsigned long)rearms,
               (unsigned long)node->hal.drain_rearms);
        return CHECK_FAIL;
    }
    printf("rx_drain_check: PASS - rearm: %d datagrams through %u buffers and an overrun, the drain taking again once "
           "a buffer was back (%lu re-arms), all in order, no socket drop\n",
           filled + REARM_MORE, (unsigned)node->hal.drain_count, (unsigned long)(node->hal.drain_rearms - rearms));
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

// 8. ring - how the buffers reached the kernel. TT_EXPECT_DRAIN_RING (user, kernel or classic) is what
// test_samehost.sh's passes under tests/support/refuse_pbuf_ring.so must have fallen back to; unset, any is fine.
static enum check_exit check_ring(const struct tt_Context* node) {
    static const char* const names[] = {"none", "user", "kernel", "classic"};
    uint8_t ring = node->hal.drain_ring;
    const char* got = ring < sizeof(names) / sizeof(names[0]) ? names[ring] : "?";
    const char* want = getenv("TT_EXPECT_DRAIN_RING");
    if (ring == tt_RX_DRAIN_RING_NONE || (want != NULL && strcmp(want, got) != 0)) {
        printf("rx_drain_check: FAIL - ring: the buffers went by %s, must be %s\n", got,
               want != NULL ? want : "any but none");
        return CHECK_FAIL;
    }
    printf("rx_drain_check: PASS - ring: every check above ran with the buffers handed over as %s\n", got);
    return CHECK_PASS;
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
    // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET/SO_RCVBUF: <sys/socket.h>, included above
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
    result = check_drained_burst(&node, sender);
    if (result == CHECK_PASS) {
        result = check_cut(&node, sender);
    }
    if (result == CHECK_PASS) {
        result = check_sized(&node, sender);
    }
#if TT_HAL_UDP_GRO
    if (result == CHECK_PASS) {
        result = check_merged(&node, sender);
    }
#endif
    if (result == CHECK_PASS) {
        result = check_rearm(&node, sender);
    }
    if (result == CHECK_PASS) {
        result = check_ring(&node);
    }

out:
    if (sender >= 0) {
        (void)close(sender);
    }
    tt_Context_destroy(&node);
    return (int)result;
}
