/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rx_gro_probe - the kernel's per-datagram receive cost for a large sample's 1472-byte datagrams, by receive method,
// with no TickLE in it (rx_gro_probe.sh). One sender, one receiver, a 1 MB burst of 1472-byte UDP datagrams (713
// of them) 30 times a second, sent with sendmmsg() 64 at a time as core sends a large sample. On the wire every
// datagram is 1472 bytes in every arm, sent alone: what differs is only how the receiver takes them in.
//
//   rx_gro_probe send DEST_IP PORT SECONDS [gso]
//   rx_gro_probe recv PORT SECONDS recvfrom|mmsg|gro
//
// recvfrom: one recvfrom() a datagram (rmw build today, tt_RX_BATCH 1). mmsg: recvmmsg() of 32 into 1472-byte slots.
// gro: UDP_GRO on the socket, recvmsg() into one 64 KB buffer; the kernel hands several same-flow datagrams over at
// once, with their segment size in a cmsg, and the receiver walks them. Every arm copies each datagram's payload
// into a 1 MB sample buffer, as core's large_place() does, so the user-side copy is the same in all of them.
// "gso" sends with UDP_SEGMENT instead: one send of up to 44 datagrams, which the kernel splits into separate
// 1472-byte UDP datagrams (not IP fragments).
//
// Output: one RESULT line - datagrams, bytes, receive calls, wake-ups, datagrams per call, the largest datagram
// taken, and getrusage CPU per MB.

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif

#define DGRAM 1472U
#define SAMPLE_BYTES (1024U * 1024U)
#define PER_SAMPLE ((SAMPLE_BYTES + DGRAM - 1U) / DGRAM)
#define BATCH 64U
#define GSO_SEGS 44U
#define MMSG 32U
#define GRO_BUF 65536U
#define RATE_HZ 30U
#define NS 1000000000ULL
#define SOCKBUF (4 * 1024 * 1024)

// This process's user and system CPU so far, in ms (getrusage). Softirq work the kernel did on this process's behalf
// while another task ran is not in it: rx_gro_probe_rig.sh reads the host's /proc/stat for that.
static void cpu_ms(double* user_ms, double* sys_ms) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    *user_ms = ((double)usage.ru_utime.tv_sec * 1e3) + ((double)usage.ru_utime.tv_usec / 1e3);
    *sys_ms = ((double)usage.ru_stime.tv_sec * 1e3) + ((double)usage.ru_stime.tv_usec / 1e3);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * NS) + (uint64_t)ts.tv_nsec;
}

static uint8_t payload[PER_SAMPLE][DGRAM];
static uint8_t sample[SAMPLE_BYTES + DGRAM];

static int run_send(const char* ip, int port, double seconds, int gso) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int size = SOCKBUF;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, ip, &dst.sin_addr);
    if (connect(sock, (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        perror("connect");
        return 1;
    }
    for (uint32_t i = 0; i < PER_SAMPLE; i++) {
        memset(payload[i], (int)(i & 0xffU), DGRAM);
    }
    static struct mmsghdr msgs[BATCH];
    static struct iovec iov[BATCH];
    uint64_t begin = now_ns();
    uint64_t next = begin;
    uint64_t sent = 0;
    uint64_t calls = 0;
    uint64_t eagain = 0;
    while ((double)(now_ns() - begin) / (double)NS < seconds) {
        uint64_t t = now_ns();
        if (t < next) {
            struct timespec ts = {(time_t)((next - t) / NS), (long)((next - t) % NS)};
            nanosleep(&ts, NULL);
            continue;
        }
        uint32_t index = 0;
        while (index < PER_SAMPLE) {
            int got;
            if (gso) {
                uint32_t n = PER_SAMPLE - index < GSO_SEGS ? PER_SAMPLE - index : GSO_SEGS;
                for (uint32_t i = 0; i < n; i++) {
                    iov[i].iov_base = payload[index + i];
                    iov[i].iov_len = DGRAM;
                }
                char control[CMSG_SPACE(sizeof(uint16_t))] = {0};
                struct msghdr msg = {.msg_iov = iov,
                                     .msg_iovlen = n,
                                     .msg_control = control,
                                     .msg_controllen = sizeof(control)};
                struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
                cm->cmsg_level = SOL_UDP;
                cm->cmsg_type = UDP_SEGMENT;
                cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
                uint16_t seg = DGRAM;
                memcpy(CMSG_DATA(cm), &seg, sizeof(seg));
                got = sendmsg(sock, &msg, 0) < 0 ? -1 : (int)n;
            } else {
                uint32_t n = PER_SAMPLE - index < BATCH ? PER_SAMPLE - index : BATCH;
                for (uint32_t i = 0; i < n; i++) {
                    iov[i].iov_base = payload[index + i];
                    iov[i].iov_len = DGRAM;
                    memset(&msgs[i].msg_hdr, 0, sizeof(msgs[i].msg_hdr));
                    msgs[i].msg_hdr.msg_iov = &iov[i];
                    msgs[i].msg_hdr.msg_iovlen = 1;
                }
                got = sendmmsg(sock, msgs, n, 0);
            }
            calls++;
            if (got < 0) {
                if (errno == EAGAIN || errno == ENOBUFS) {
                    eagain++;
                    continue;
                }
                perror("send");
                return 1;
            }
            index += (uint32_t)got;
            sent += (uint64_t)got;
        }
        next += NS / RATE_HZ;
    }
    double user_ms = 0;
    double sys_ms = 0;
    cpu_ms(&user_ms, &sys_ms);
    double mb = (double)sent * DGRAM / 1e6;
    printf("RESULT: role=send gso=%d datagrams=%lu calls=%lu eagain=%lu user_ms=%.1f sys_ms=%.1f cpu_ms_per_mb=%.4f\n",
           gso, (unsigned long)sent, (unsigned long)calls, (unsigned long)eagain, user_ms, sys_ms,
           mb > 0 ? (user_ms + sys_ms) / mb : 0.0);
    return 0;
}

static uint64_t datagrams;
static uint64_t bytes;
static uint32_t cursor;
static uint32_t max_len;    // the largest datagram (GRO: segment) taken - the wire size as the receiver sees it
static uint64_t oversize;   // datagrams or segments above DGRAM: must be 0
static uint64_t data_calls; // receive calls that returned at least one datagram

static void take(const uint8_t* data, uint32_t len) {
    if (cursor + len > sizeof(sample)) {
        cursor = 0;
    }
    memcpy(sample + cursor, data, len); // large_place()'s copy
    cursor = (cursor + len) % SAMPLE_BYTES;
    datagrams++;
    bytes += len;
    max_len = len > max_len ? len : max_len;
    oversize += len > DGRAM ? 1U : 0U;
}

static int run_recv(int port, double seconds, const char* mode) {
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int size = SOCKBUF;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    int gro = strcmp(mode, "gro") == 0;
    int mmsg = strcmp(mode, "mmsg") == 0;
    if (gro) {
        int one = 1;
        if (setsockopt(sock, SOL_UDP, UDP_GRO, &one, sizeof(one)) < 0) {
            perror("UDP_GRO");
            return 1;
        }
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN};
    epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);
    static uint8_t buf[GRO_BUF];
    static uint8_t slots[MMSG][DGRAM];
    static struct mmsghdr msgs[MMSG];
    static struct iovec iov[MMSG];
    static struct sockaddr_in from[MMSG];
    uint64_t calls = 0;
    uint64_t wakes = 0;
    uint64_t gro_merged = 0;
    uint64_t begin = now_ns();
    while ((double)(now_ns() - begin) / (double)NS < seconds) {
        struct epoll_event out;
        if (epoll_wait(ep, &out, 1, 100) <= 0) {
            continue;
        }
        wakes++;
        for (;;) {
            calls++;
            if (mmsg) {
                for (uint32_t i = 0; i < MMSG; i++) {
                    iov[i].iov_base = slots[i];
                    iov[i].iov_len = DGRAM;
                    msgs[i].msg_hdr = (struct msghdr) {.msg_name = &from[i],
                                                       .msg_namelen = sizeof(from[i]),
                                                       .msg_iov = &iov[i],
                                                       .msg_iovlen = 1};
                }
                int got = recvmmsg(sock, msgs, MMSG, MSG_DONTWAIT, NULL);
                if (got <= 0) {
                    break;
                }
                data_calls++;
                for (int i = 0; i < got; i++) {
                    if ((msgs[i].msg_hdr.msg_flags & MSG_TRUNC) != 0) {
                        oversize++; // a datagram larger than its 1472-byte slot
                    }
                    take(slots[i], msgs[i].msg_len);
                }
                if ((uint32_t)got < MMSG) {
                    break; // a short batch emptied the socket, as hal_linux.c's rx_idle reads it
                }
            } else if (gro) {
                char control[CMSG_SPACE(sizeof(int))];
                struct iovec one = {buf, sizeof(buf)};
                struct sockaddr_in src;
                struct msghdr msg = {.msg_name = &src,
                                     .msg_namelen = sizeof(src),
                                     .msg_iov = &one,
                                     .msg_iovlen = 1,
                                     .msg_control = control,
                                     .msg_controllen = sizeof(control)};
                ssize_t len = recvmsg(sock, &msg, MSG_DONTWAIT);
                if (len <= 0) {
                    break;
                }
                int seg = (int)len;
                for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm)) {
                    if (cm->cmsg_level == SOL_UDP && cm->cmsg_type == UDP_GRO) {
                        memcpy(&seg, CMSG_DATA(cm), sizeof(seg));
                    }
                }
                data_calls++;
                if (seg < len) {
                    gro_merged++;
                }
                for (ssize_t off = 0; off < len; off += seg) {
                    take(buf + off, (uint32_t)(len - off < seg ? len - off : seg));
                }
            } else {
                struct sockaddr_in src;
                socklen_t src_len = sizeof(src);
                ssize_t len = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&src, &src_len);
                if (len <= 0) {
                    break;
                }
                data_calls++;
                take(buf, (uint32_t)len);
            }
        }
    }
    double user_ms = 0;
    double sys_ms = 0;
    cpu_ms(&user_ms, &sys_ms);
    double mb = (double)bytes / 1e6;
    // per_call counts every receive call, the empty one that ends a drain included; per_recv only those that returned
    // data - "datagrams per recvmsg", the figure the GRO decision reads.
    printf("RESULT: role=recv mode=%s datagrams=%lu bytes=%lu calls=%lu data_calls=%lu wakes=%lu per_call=%.2f "
           "per_recv=%.2f gro_merged_calls=%lu max_len=%u oversize=%lu user_ms=%.1f sys_ms=%.1f cpu_ms_per_mb=%.4f\n",
           mode, (unsigned long)datagrams, (unsigned long)bytes, (unsigned long)calls, (unsigned long)data_calls,
           (unsigned long)wakes, calls > 0 ? (double)datagrams / (double)calls : 0.0,
           data_calls > 0 ? (double)datagrams / (double)data_calls : 0.0, (unsigned long)gro_merged, max_len,
           (unsigned long)oversize, user_ms, sys_ms, mb > 0 ? (user_ms + sys_ms) / mb : 0.0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && strcmp(argv[1], "send") == 0) {
        return run_send(argv[2], atoi(argv[3]), strtod(argv[4], NULL), argc > 5 && strcmp(argv[5], "gso") == 0);
    }
    if (argc >= 5 && strcmp(argv[1], "recv") == 0) {
        return run_recv(atoi(argv[2]), strtod(argv[3], NULL), argv[4]);
    }
    fprintf(stderr, "usage: %s send IP PORT SECONDS [gso] | recv PORT SECONDS recvfrom|mmsg|gro\n", argv[0]);
    return 2;
}
