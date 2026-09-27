/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// copy_cost - rmw_tickle/RMW_PERF_PLAN.md 11, M-b and M-c (2026-09-27): what a copy costs next to what it could be
// replaced by, on the machine it runs on (the rig's client Pi, pinned).
//
//   iovec <size> <ops> [iovec-first]  M-b: one send of a <size>-byte field behind a 24-byte header, two ways,
//   alternated per
//                       call: (i) the field memcpy'd into a contiguous buffer and sent as one iovec; (ii) sendmsg()
//                       with two iovecs, header and field in place. UDP over lo to this process's own socket,
//                       drained between batches, so both arms pay the same receive. Prints user+sys ns per op for
//                       each arm (getrusage(RUSAGE_THREAD)).
//   copy <size> <ops>   M-c: the held-back datagram's memcpy alone - ns per copy of <size> bytes (CLOCK_MONOTONIC).
//
// One line per call: RESULT: mode=... size=... ns_per_op_copy=... ns_per_op_iovec=... (iovec) or ns_per_copy=...
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h> // NOLINT(misc-include-cleaner) - struct iovec

#define HEADER_BYTES 24U
#define MAX_FIELD 61440U // 60 KB, inside one UDP datagram on lo
#define DRAIN_EVERY 32U
#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000ULL
#define ARG_BASE 10
#define RCVBUF_BYTES (8 * 1024 * 1024)
#define FIELD_FILL 0xa5
#define HEADER_FILL 0x5a
#define SOURCE_FILL 0x3c
#define WARM_FRACTION 10U

static uint8_t field[MAX_FIELD];
static uint8_t contiguous[HEADER_BYTES + MAX_FIELD];
static uint8_t header[HEADER_BYTES];
static uint8_t drain_buf[HEADER_BYTES + MAX_FIELD];

static uint64_t cpu_ns(void) {
    struct rusage usage;              // NOLINT(misc-include-cleaner) - <sys/resource.h>
    getrusage(RUSAGE_THREAD, &usage); // NOLINT(misc-include-cleaner) - <sys/resource.h>
    return ((uint64_t)usage.ru_utime.tv_sec * NS_PER_S) + ((uint64_t)usage.ru_utime.tv_usec * NS_PER_US) +
           ((uint64_t)usage.ru_stime.tv_sec * NS_PER_S) + ((uint64_t)usage.ru_stime.tv_usec * NS_PER_US);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - <time.h>
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static void drain(int rx_sock) {
    while (recv(rx_sock, drain_buf, sizeof(drain_buf), MSG_DONTWAIT) > 0) {
    }
}

// Returns the arm's thread CPU (user+sys) in ns; *wall gets its CLOCK_MONOTONIC duration. The CPU figure is only
// as fine as the kernel's accounting (on the PC it moves in whole milliseconds), so an arm must run long enough.
static uint64_t run_arm(int tx_sock, int rx_sock, uint32_t size, uint32_t ops, int two_iovecs, uint64_t* wall) {
    uint64_t wall_start = now_ns();
    uint64_t start = cpu_ns();
    for (uint32_t i = 0; i < ops; i++) {
        struct iovec iov[2]; // NOLINT(misc-include-cleaner) - <sys/uio.h>
        struct msghdr msg = {0};
        if (two_iovecs) {
            iov[0] = (struct iovec) {header, HEADER_BYTES};
            iov[1] = (struct iovec) {field, size};
            msg.msg_iovlen = 2;
        } else {
            memcpy(contiguous, header, HEADER_BYTES);
            memcpy(contiguous + HEADER_BYTES, field, size);
            iov[0] = (struct iovec) {contiguous, HEADER_BYTES + size};
            msg.msg_iovlen = 1;
        }
        msg.msg_iov = iov;
        if (sendmsg(tx_sock, &msg, 0) < 0) {
            perror("sendmsg");
            exit(1);
        }
        if (i % DRAIN_EVERY == DRAIN_EVERY - 1) {
            drain(rx_sock);
        }
    }
    drain(rx_sock);
    uint64_t cpu = cpu_ns() - start;
    *wall = now_ns() - wall_start;
    return cpu;
}

static int iovec_mode(uint32_t size, uint32_t ops, int iovec_first) {
    int rx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int tx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int rcvbuf = RCVBUF_BYTES;
    // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET/SO_RCVBUF: <sys/socket.h>
    setsockopt(rx_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t addr_len = sizeof(addr);
    if (bind(rx_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        getsockname(rx_sock, (struct sockaddr*)&addr, &addr_len) < 0 ||
        connect(tx_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("socket setup");
        return 1;
    }
    memset(field, FIELD_FILL, sizeof(field));
    memset(header, HEADER_FILL, sizeof(header));
    uint64_t wall_copy = 0;
    uint64_t wall_iovec = 0;
    (void)run_arm(tx_sock, rx_sock, size, (ops / WARM_FRACTION) + 1, 0, &wall_copy); // warm both paths
    (void)run_arm(tx_sock, rx_sock, size, (ops / WARM_FRACTION) + 1, 1, &wall_iovec);
    uint64_t copy_ns = 0;
    uint64_t iovec_ns = 0;
    if (iovec_first) { // the runner alternates the order by round, so drift lands on both arms alike
        iovec_ns = run_arm(tx_sock, rx_sock, size, ops, 1, &wall_iovec);
        copy_ns = run_arm(tx_sock, rx_sock, size, ops, 0, &wall_copy);
    } else {
        copy_ns = run_arm(tx_sock, rx_sock, size, ops, 0, &wall_copy);
        iovec_ns = run_arm(tx_sock, rx_sock, size, ops, 1, &wall_iovec);
    }
    printf("RESULT: mode=iovec size=%u ops=%u ns_per_op_copy=%.1f ns_per_op_iovec=%.1f wall_ns_per_op_copy=%.1f "
           "wall_ns_per_op_iovec=%.1f\n",
           size, ops, (double)copy_ns / ops, (double)iovec_ns / ops, (double)wall_copy / ops, (double)wall_iovec / ops);
    close(tx_sock);
    close(rx_sock);
    return 0;
}

static int copy_mode(uint32_t size, uint32_t ops) {
    static uint8_t src[2][MAX_FIELD];
    static uint8_t dst[MAX_FIELD];
    memset(src, SOURCE_FILL, sizeof(src));
    uint64_t start = now_ns();
    for (uint32_t i = 0; i < ops; i++) {
        memcpy(dst, src[i & 1U], size);
        __asm__ volatile("" : : "r"(dst) : "memory"); // the copy must happen, every time
    }
    uint64_t took = now_ns() - start;
    printf("RESULT: mode=copy size=%u ops=%u ns_per_copy=%.2f\n", size, ops, (double)took / ops);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s iovec|copy <size> <ops> [iovec-first]\n", argv[0]);
        return 2;
    }
    uint32_t size = (uint32_t)strtoul(argv[2], NULL, ARG_BASE);
    uint32_t ops = (uint32_t)strtoul(argv[3], NULL, ARG_BASE);
    if (size == 0 || size > MAX_FIELD || ops == 0) {
        fprintf(stderr, "size 1-%u, ops > 0\n", MAX_FIELD);
        return 2;
    }
    int iovec_first = argc == 5 && strcmp(argv[4], "iovec-first") == 0;
    return strcmp(argv[1], "iovec") == 0 ? iovec_mode(size, ops, iovec_first) : copy_mode(size, ops);
}
