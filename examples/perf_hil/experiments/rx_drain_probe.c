/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rx_drain_probe: does a receiver that stops reading for a while lose datagrams from its socket, and does an
// io_uring multishot recvmsg into provided buffers keep the socket drained while the thread is busy in user space?
// No TickLE in it: the question is about the kernel, asked for large-message cell I4s (RESULTS L4), where a
// saturated 1 GbE link, a 425,984-byte receive buffer (the rig's net.core.rmem_max 212,992, doubled) and a subscriber
// that spends milliseconds on each 4 MB sample lost 14-38 samples a run on the subscriber's side.
//
//   rx_drain_probe send IP PORT SECONDS                  1472-byte datagrams, each its seq_no, as fast as the
//                                                        socket takes them (blocking sendmmsg of 64)
//   rx_drain_probe recv PORT SECONDS MODE SPIN_US [PER] [BUFS]
//       MODE mmsg   recvmmsg() of 64 after ppoll(): TickLE's receive path today
//            uring  io_uring multishot recvmsg into BUFS provided buffers of 2048 B (default 4096)
//            coop   the same with IORING_SETUP_COOP_TASKRUN: completions run only when the thread enters the kernel
//       after every PER datagrams (default 2889, one 4 MB sample's) the thread spins SPIN_US in user space - the
//       subscriber's per-sample work (rmw's decode, the application's callback).
//
// Both sockets ask for 212,992 bytes, so the kernel grants 425,984 as on the rig's Pis whatever this PC's rmem_max.
//
// RESULT (recv): mode spin_us bufs received lost (seq gaps) socket_drops (SO_MEMINFO) rcvbuf rearms (multishot
//   ended, ENOBUFS) cpu_user_ms cpu_sys_ms. The reading rules are in rx_drain_pc.sh, which runs it, and its summary.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/io_uring.h>
#include <linux/sock_diag.h> // SK_MEMINFO_*
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define DGRAM 1472U
#define BATCH 64U
#define SOCK_BUF 212992
#define BUF_SIZE 2048U
#define BGID 7U
#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000ULL
#define DEFAULT_PER 2889U
#define DEFAULT_BUFS 4096U
#define TAIL_S 1ULL

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static void spin_us(uint64_t us) {
    uint64_t end = now_ns() + (us * NS_PER_US);
    while (now_ns() < end) {
    }
}

static int udp_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }
    int size = SOCK_BUF;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    return fd;
}

static int do_send(const char* ip, int port, double seconds) {
    int fd = udp_socket();
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, ip, &to.sin_addr);
    static uint8_t payload[BATCH][DGRAM];
    struct iovec iov[BATCH];
    struct mmsghdr msgs[BATCH];
    for (uint32_t i = 0; i < BATCH; i++) {
        iov[i] = (struct iovec) {payload[i], DGRAM};
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_name = &to;
        msgs[i].msg_hdr.msg_namelen = sizeof(to);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    // One datagram first, and a pause: the neighbour entry is resolved before the burst, so ARP's unresolved queue
    // drops nothing the receiver would count as lost.
    uint64_t zero = 0;
    memcpy(payload[0], &zero, sizeof(zero));
    (void)sendto(fd, payload[0], DGRAM, 0, (struct sockaddr*)&to, sizeof(to));
    struct timespec pause = {0, 200000000};
    nanosleep(&pause, NULL);
    uint64_t seq = 1;
    uint64_t end = now_ns() + (uint64_t)(seconds * (double)NS_PER_S);
    while (now_ns() < end) {
        for (uint32_t i = 0; i < BATCH; i++) {
            uint64_t s = seq + i;
            memcpy(payload[i], &s, sizeof(s));
        }
        int sent = sendmmsg(fd, msgs, BATCH, 0);
        if (sent < 0) {
            perror("sendmmsg");
            return 1;
        }
        seq += (uint64_t)sent;
    }
    printf("RESULT: send sent=%llu\n", (unsigned long long)seq);
    return 0;
}

// --- receiving: what every mode shares --------------------------------------------------------------------------------

struct rx_state {
    uint64_t received;
    uint64_t lost;
    uint64_t next_seq;
    uint64_t first_seq;
    uint64_t since_spin;
    uint32_t per;
    uint64_t spin;
    uint64_t last_ns;
};

static void got(struct rx_state* st, const uint8_t* data, uint32_t len) {
    if (len < sizeof(uint64_t)) {
        return;
    }
    uint64_t seq;
    memcpy(&seq, data, sizeof(seq));
    if (st->received == 0) {
        st->first_seq = seq;
    }
    if (seq + 1 > st->next_seq) {
        st->next_seq = seq + 1; // one past the highest seen: lost is what the range holds that did not arrive
    }
    st->received++;
    st->lost = st->next_seq - st->first_seq - st->received;
    st->last_ns = now_ns();
    if (++st->since_spin >= st->per) {
        st->since_spin = 0;
        spin_us(st->spin);
    }
}

static uint32_t socket_drops(int fd, uint32_t* rcvbuf) {
    uint32_t info[SK_MEMINFO_VARS];
    socklen_t len = sizeof(info);
    memset(info, 0, sizeof(info));
    if (getsockopt(fd, SOL_SOCKET, SO_MEMINFO, info, &len) < 0) {
        perror("SO_MEMINFO");
        return UINT32_MAX;
    }
    *rcvbuf = info[SK_MEMINFO_RCVBUF];
    return info[SK_MEMINFO_DROPS];
}


static int bound_socket(int port) {
    int fd = udp_socket();
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    return fd;
}

// Waits until the first datagram, then runs until `seconds` after it, or TAIL_S after the last one.
static bool rx_done(const struct rx_state* st, uint64_t first_ns, double seconds) {
    uint64_t now = now_ns();
    if (st->received == 0) {
        return false;
    }
    return now - first_ns > (uint64_t)(seconds * (double)NS_PER_S) || now - st->last_ns > TAIL_S * NS_PER_S;
}

static void recv_mmsg(int fd, struct rx_state* st, double seconds) {
    static uint8_t bufs[BATCH][DGRAM];
    struct iovec iov[BATCH];
    struct mmsghdr msgs[BATCH];
    for (uint32_t i = 0; i < BATCH; i++) {
        iov[i] = (struct iovec) {bufs[i], DGRAM};
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    uint64_t first = 0;
    struct pollfd pfd = {fd, POLLIN, 0};
    while (!rx_done(st, first, seconds)) {
        if (poll(&pfd, 1, 100) <= 0) {
            continue;
        }
        int got_n = recvmmsg(fd, msgs, BATCH, MSG_DONTWAIT, NULL);
        for (int i = 0; i < got_n; i++) {
            if (st->received == 0) {
                first = now_ns();
            }
            got(st, bufs[i], msgs[i].msg_len);
        }
    }
}

// --- io_uring, raw syscalls as hal_linux.c does it --------------------------------------------------------------------

struct uring {
    int fd;
    uint32_t *sq_tail, *sq_mask, *sq_array, *cq_head, *cq_tail, *cq_mask;
    struct io_uring_sqe* sqes;
    struct io_uring_cqe* cqes;
    struct io_uring_buf_ring* br;
    uint32_t br_mask;
    uint8_t* buffers;
    uint32_t nbufs;
};

static void uring_init(struct uring* u, uint32_t flags, uint32_t nbufs) {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    // The completion ring holds every buffer's completion: a multishot request ends (IORING_CQE_F_MORE clear) when it
    // cannot post one, and the datagrams then wait in the socket again.
    p.flags = flags | IORING_SETUP_CQSIZE;
    p.cq_entries = 2 * nbufs;
    u->fd = (int)syscall(__NR_io_uring_setup, 64, &p);
    if (u->fd < 0) {
        perror("io_uring_setup");
        exit(1);
    }
    size_t sq_len = p.sq_off.array + (p.sq_entries * sizeof(uint32_t));
    size_t cq_len = p.cq_off.cqes + (p.cq_entries * sizeof(struct io_uring_cqe));
    if (cq_len > sq_len) {
        sq_len = cq_len; // IORING_FEAT_SINGLE_MMAP, every kernel this probe runs on
    }
    uint8_t* sq = mmap(NULL, sq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, u->fd, IORING_OFF_SQ_RING);
    u->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                   u->fd, IORING_OFF_SQES);
    if (sq == MAP_FAILED || u->sqes == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    u->sq_tail = (uint32_t*)(sq + p.sq_off.tail);
    u->sq_mask = (uint32_t*)(sq + p.sq_off.ring_mask);
    u->sq_array = (uint32_t*)(sq + p.sq_off.array);
    u->cq_head = (uint32_t*)(sq + p.cq_off.head);
    u->cq_tail = (uint32_t*)(sq + p.cq_off.tail);
    u->cq_mask = (uint32_t*)(sq + p.cq_off.ring_mask);
    u->cqes = (struct io_uring_cqe*)(sq + p.cq_off.cqes);

    u->nbufs = nbufs; // a power of two
    size_t ring_len = nbufs * sizeof(struct io_uring_buf);
    u->br = mmap(NULL, ring_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    u->buffers = mmap(NULL, (size_t)nbufs * BUF_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (u->br == MAP_FAILED || u->buffers == MAP_FAILED) {
        perror("mmap buffers");
        exit(1);
    }
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = (uint64_t)(uintptr_t)u->br;
    reg.ring_entries = nbufs;
    reg.bgid = BGID;
    if (syscall(__NR_io_uring_register, u->fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) {
        perror("IORING_REGISTER_PBUF_RING");
        exit(1);
    }
    u->br_mask = nbufs - 1;
    for (uint32_t i = 0; i < nbufs; i++) {
        struct io_uring_buf* b = &u->br->bufs[i];
        b->addr = (uint64_t)(uintptr_t)(u->buffers + ((size_t)i * BUF_SIZE));
        b->len = BUF_SIZE;
        b->bid = (uint16_t)i;
    }
    __atomic_store_n(&u->br->tail, (uint16_t)nbufs, __ATOMIC_RELEASE);
}

static uint16_t br_tail_local;

static void buf_recycle(struct uring* u, uint16_t bid) {
    struct io_uring_buf* b = &u->br->bufs[br_tail_local & u->br_mask];
    b->addr = (uint64_t)(uintptr_t)(u->buffers + ((size_t)bid * BUF_SIZE));
    b->len = BUF_SIZE;
    b->bid = bid;
    br_tail_local++;
}

static struct msghdr rx_msghdr; // multishot recvmsg's layout: no name, no control

static void arm(struct uring* u, int fd) {
    uint32_t tail = *u->sq_tail;
    uint32_t idx = tail & *u->sq_mask;
    struct io_uring_sqe* sqe = &u->sqes[idx];
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)&rx_msghdr;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->buf_group = BGID;
    u->sq_array[idx] = idx;
    __atomic_store_n(u->sq_tail, tail + 1, __ATOMIC_RELEASE);
    if (syscall(__NR_io_uring_enter, u->fd, 1, 0, 0, NULL, 0) < 0) {
        perror("io_uring_enter submit");
        exit(1);
    }
}

static uint64_t rearms;

static void recv_uring(int fd, struct rx_state* st, double seconds, uint32_t flags, uint32_t nbufs) {
    struct uring u;
    uring_init(&u, flags, nbufs);
    br_tail_local = (uint16_t)nbufs;
    arm(&u, fd);
    uint64_t first = 0;
    while (!rx_done(st, first, seconds)) {
        uint32_t head = *u.cq_head;
        uint32_t tail = __atomic_load_n(u.cq_tail, __ATOMIC_ACQUIRE);
        if (head == tail) {
            struct timespec ts = {0, 100000000};
            struct {
                uint64_t sigmask;
                uint32_t sigmask_sz, pad;
                uint64_t ts;
            } arg = {0, 0, 0, (uint64_t)(uintptr_t)&ts};
            (void)syscall(__NR_io_uring_enter, u.fd, 0, 1, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg,
                          sizeof(arg));
            continue;
        }
        bool rearm = false;
        for (; head != tail; head++) {
            const struct io_uring_cqe* cqe = &u.cqes[head & *u.cq_mask];
            if (!(cqe->flags & IORING_CQE_F_MORE)) {
                rearm = true;
            }
            if (cqe->res < 0 || !(cqe->flags & IORING_CQE_F_BUFFER)) {
                continue;
            }
            uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
            const uint8_t* buf = u.buffers + ((size_t)bid * BUF_SIZE);
            const struct io_uring_recvmsg_out* out = (const struct io_uring_recvmsg_out*)buf;
            if (st->received == 0) {
                first = now_ns();
            }
            uint32_t len = out->payloadlen < BUF_SIZE - sizeof(*out) ? out->payloadlen : 0;
            got(st, buf + sizeof(*out), len);
            buf_recycle(&u, bid);
            // Hand the buffer back now, as a datagram's processing ends, not after the batch: a spin inside got()
            // must not hold buffers the kernel could be filling.
            __atomic_store_n(&u.br->tail, br_tail_local, __ATOMIC_RELEASE);
            __atomic_store_n(u.cq_head, head + 1, __ATOMIC_RELEASE);
        }
        __atomic_store_n(u.cq_head, head, __ATOMIC_RELEASE);
        if (rearm) {
            rearms++;
            arm(&u, fd);
        }
    }
}

int main(int argc, char** argv) {
    if (argc >= 5 && strcmp(argv[1], "send") == 0) {
        return do_send(argv[2], atoi(argv[3]), atof(argv[4]));
    }
    if (argc < 6 || strcmp(argv[1], "recv") != 0) {
        fprintf(stderr, "usage: %s send IP PORT SECONDS | recv PORT SECONDS mmsg|uring|coop SPIN_US [PER] [BUFS]\n",
                argv[0]);
        return 2;
    }
    int fd = bound_socket(atoi(argv[2]));
    double seconds = atof(argv[3]);
    const char* mode = argv[4];
    struct rx_state st = {0};
    st.spin = strtoull(argv[5], NULL, 10);
    st.per = argc > 6 ? (uint32_t)strtoul(argv[6], NULL, 10) : DEFAULT_PER;
    uint32_t nbufs = argc > 7 ? (uint32_t)strtoul(argv[7], NULL, 10) : DEFAULT_BUFS;
    if (strcmp(mode, "mmsg") == 0) {
        nbufs = 0;
        recv_mmsg(fd, &st, seconds);
    } else if (strcmp(mode, "uring") == 0) {
        recv_uring(fd, &st, seconds, 0, nbufs);
    } else if (strcmp(mode, "coop") == 0) {
        recv_uring(fd, &st, seconds, IORING_SETUP_COOP_TASKRUN, nbufs);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }
    uint32_t rcvbuf = 0;
    uint32_t drops = socket_drops(fd, &rcvbuf);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("RESULT: mode=%s spin_us=%llu per=%u bufs=%u received=%llu lost=%llu socket_drops=%u rcvbuf=%u rearms=%llu "
           "cpu_user_ms=%ld cpu_sys_ms=%ld\n",
           mode, (unsigned long long)st.spin, st.per, nbufs, (unsigned long long)st.received,
           (unsigned long long)st.lost, drops, rcvbuf, (unsigned long long)rearms,
           (long)(ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000),
           (long)(ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000));
    return 0;
}
