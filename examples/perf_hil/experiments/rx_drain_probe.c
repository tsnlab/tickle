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

#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier,readability-identifier-naming) - recvmmsg(), sendmmsg()
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// NOLINTBEGIN(misc-include-cleaner) - socket, poll and clock names live in glibc-private headers, and the io_uring
// syscall numbers in an architecture's own; these public headers are the portable spelling (as hal_linux.c's)
#include <arpa/inet.h>
#include <linux/io_uring.h>
#include <linux/sock_diag.h> // SK_MEMINFO_*
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>

#define DGRAM_MAX 65507U
#define BATCH 64U
#define SOCK_BUF 212992
#define BGID 7U
#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000ULL
#define DEFAULT_PER 2889U
#define DEFAULT_DGRAM 1472U
#define DEFAULT_BUF_SIZE 2048U
#define ARP_PAUSE_NS 200000000L
#define POLL_MS 100
#define RING_ENTRIES 64U
#define WAIT_NS 100000000L
#define LINE_BYTES 1024
#define DEFAULT_BUFS 4096U
#define TAIL_S 1ULL

static uint32_t dgram = DEFAULT_DGRAM; // datagram length: 1472 (TickLE's fragments) or up to 65507 (IP fragmentation)
static uint32_t buf_size = DEFAULT_BUF_SIZE; // a provided buffer: the kernel's header, then the datagram

static uint64_t now_ns(void) {
    struct timespec wait;
    clock_gettime(CLOCK_MONOTONIC, &wait);
    return ((uint64_t)wait.tv_sec * NS_PER_S) + (uint64_t)wait.tv_nsec;
}

static void spin_us(uint64_t micros) {
    uint64_t end = now_ns() + (micros * NS_PER_US);
    while (now_ns() < end) {
    }
}

static int udp_socket(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }
    int size = SOCK_BUF;
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    return sock;
}

static int do_send(const char* ip, int port, double seconds) {
    int sock = udp_socket();
    struct sockaddr_in dest = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, ip, &dest.sin_addr);
    static uint8_t payload[BATCH][DGRAM_MAX];
    struct iovec iov[BATCH];
    struct mmsghdr msgs[BATCH];
    for (uint32_t i = 0; i < BATCH; i++) {
        iov[i] = (struct iovec) {payload[i], dgram};
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_name = &dest;
        msgs[i].msg_hdr.msg_namelen = sizeof(dest);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    // One datagram first, and a pause: the neighbour entry is resolved before the burst, so ARP's unresolved queue
    // drops nothing the receiver would count as lost.
    uint64_t zero = 0;
    memcpy(payload[0], &zero, sizeof(zero));
    (void)sendto(sock, payload[0], dgram, 0, (struct sockaddr*)&dest, sizeof(dest));
    struct timespec pause = {0, ARP_PAUSE_NS};
    nanosleep(&pause, NULL);
    uint64_t seq = 1;
    uint64_t end = now_ns() + (uint64_t)(seconds * (double)NS_PER_S);
    while (now_ns() < end) {
        for (uint32_t i = 0; i < BATCH; i++) {
            uint64_t numbered = seq + i;
            memcpy(payload[i], &numbered, sizeof(numbered));
        }
        int sent = sendmmsg(sock, msgs, BATCH, 0);
        if (sent < 0) {
            perror("sendmmsg");
            return 1;
        }
        seq += (uint64_t)sent;
    }
    printf("RESULT: send sent=%llu\n", (unsigned long long)seq);
    return 0;
}

// --- receiving: what every mode shares
// --------------------------------------------------------------------------------

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

static void got(struct rx_state* state, const uint8_t* data, uint32_t len) {
    if (len < sizeof(uint64_t)) {
        return;
    }
    uint64_t seq;
    memcpy(&seq, data, sizeof(seq));
    if (state->received == 0) {
        state->first_seq = seq;
    }
    if (seq + 1 > state->next_seq) {
        state->next_seq = seq + 1; // one past the highest seen: lost is what the range holds that did not arrive
    }
    state->received++;
    state->lost = state->next_seq - state->first_seq - state->received;
    state->last_ns = now_ns();
    if (++state->since_spin >= state->per) {
        state->since_spin = 0;
        spin_us(state->spin);
    }
}

static uint32_t socket_drops(int sock, uint32_t* rcvbuf) {
    uint32_t info[SK_MEMINFO_VARS];
    socklen_t len = sizeof(info);
    memset(info, 0, sizeof(info));
    if (getsockopt(sock, SOL_SOCKET, SO_MEMINFO, info, &len) < 0) {
        perror("SO_MEMINFO");
        return UINT32_MAX;
    }
    *rcvbuf = info[SK_MEMINFO_RCVBUF];
    return info[SK_MEMINFO_DROPS];
}

static int bound_socket(int port) {
    int sock = udp_socket();
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    return sock;
}

// Waits until the first datagram, then runs until `seconds` after it, or TAIL_S after the last one.
static bool rx_done(const struct rx_state* state, uint64_t first_ns, double seconds) {
    uint64_t now = now_ns();
    if (state->received == 0) {
        return false;
    }
    return now - first_ns > (uint64_t)(seconds * (double)NS_PER_S) || now - state->last_ns > TAIL_S * NS_PER_S;
}

static void recv_mmsg(int sock, struct rx_state* state, double seconds) {
    static uint8_t bufs[BATCH][DGRAM_MAX];
    struct iovec iov[BATCH];
    struct mmsghdr msgs[BATCH];
    for (uint32_t i = 0; i < BATCH; i++) {
        iov[i] = (struct iovec) {bufs[i], DGRAM_MAX};
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    uint64_t first = 0;
    struct pollfd pfd = {sock, POLLIN, 0};
    while (!rx_done(state, first, seconds)) {
        if (poll(&pfd, 1, POLL_MS) <= 0) {
            continue;
        }
        int got_n = recvmmsg(sock, msgs, BATCH, MSG_DONTWAIT, NULL);
        for (int i = 0; i < got_n; i++) {
            if (state->received == 0) {
                first = now_ns();
            }
            got(state, bufs[i], msgs[i].msg_len);
        }
    }
}

// --- io_uring, raw syscalls as hal_linux.c does it
// --------------------------------------------------------------------

struct uring {
    int sock;
    uint32_t *sq_tail, *sq_mask, *sq_array, *cq_head, *cq_tail, *cq_mask;
    struct io_uring_sqe* sqes;
    struct io_uring_cqe* cqes;
    struct io_uring_buf_ring* br;
    uint32_t br_mask;
    uint8_t* buffers;
    uint32_t nbufs;
};

static void uring_init(struct uring* ring, uint32_t flags, uint32_t nbufs) {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    // The completion ring holds every buffer's completion: a multishot request ends (IORING_CQE_F_MORE clear) when it
    // cannot post one, and the datagrams then wait in the socket again.
    params.flags = flags | IORING_SETUP_CQSIZE;
    params.cq_entries = 2 * nbufs;
    ring->sock = (int)syscall(__NR_io_uring_setup, RING_ENTRIES, &params);
    if (ring->sock < 0) {
        perror("io_uring_setup");
        exit(1);
    }
    size_t sq_len = params.sq_off.array + (params.sq_entries * sizeof(uint32_t));
    size_t cq_len = params.cq_off.cqes + (params.cq_entries * sizeof(struct io_uring_cqe));
    if (cq_len > sq_len) {
        sq_len = cq_len; // IORING_FEAT_SINGLE_MMAP, every kernel this probe runs on
    }
    uint8_t* sq_map =
        mmap(NULL, sq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring->sock, IORING_OFF_SQ_RING);
    ring->sqes = mmap(NULL, params.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, ring->sock, IORING_OFF_SQES);
    if (sq_map == MAP_FAILED || ring->sqes == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    ring->sq_tail = (uint32_t*)(sq_map + params.sq_off.tail);
    ring->sq_mask = (uint32_t*)(sq_map + params.sq_off.ring_mask);
    ring->sq_array = (uint32_t*)(sq_map + params.sq_off.array);
    ring->cq_head = (uint32_t*)(sq_map + params.cq_off.head);
    ring->cq_tail = (uint32_t*)(sq_map + params.cq_off.tail);
    ring->cq_mask = (uint32_t*)(sq_map + params.cq_off.ring_mask);
    ring->cqes = (struct io_uring_cqe*)(sq_map + params.cq_off.cqes);

    ring->nbufs = nbufs; // a power of two
    size_t ring_len = nbufs * sizeof(struct io_uring_buf);
    ring->br = mmap(NULL, ring_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    ring->buffers =
        mmap(NULL, (size_t)nbufs * buf_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (ring->br == MAP_FAILED || ring->buffers == MAP_FAILED) {
        perror("mmap buffers");
        exit(1);
    }
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = (uint64_t)(uintptr_t)ring->br;
    reg.ring_entries = nbufs;
    reg.bgid = BGID;
    if (syscall(__NR_io_uring_register, ring->sock, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) {
        perror("IORING_REGISTER_PBUF_RING");
        exit(1);
    }
    ring->br_mask = nbufs - 1;
    for (uint32_t i = 0; i < nbufs; i++) {
        struct io_uring_buf* entry = &ring->br->bufs[i];
        entry->addr = (uint64_t)(uintptr_t)(ring->buffers + ((size_t)i * buf_size));
        entry->len = buf_size;
        entry->bid = (uint16_t)i;
    }
    __atomic_store_n(&ring->br->tail, (uint16_t)nbufs, __ATOMIC_RELEASE);
}

static uint16_t br_tail_local;

static void buf_recycle(struct uring* ring, uint16_t bid) {
    struct io_uring_buf* entry = &ring->br->bufs[br_tail_local & ring->br_mask];
    entry->addr = (uint64_t)(uintptr_t)(ring->buffers + ((size_t)bid * buf_size));
    entry->len = buf_size;
    entry->bid = bid;
    br_tail_local++;
}

static struct msghdr rx_msghdr; // multishot recvmsg's layout: no name, no control

static void arm(struct uring* ring, int sock) {
    uint32_t tail = *ring->sq_tail;
    uint32_t idx = tail & *ring->sq_mask;
    struct io_uring_sqe* sqe = &ring->sqes[idx];
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd = sock;
    sqe->addr = (uint64_t)(uintptr_t)&rx_msghdr;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->buf_group = BGID;
    ring->sq_array[idx] = idx;
    __atomic_store_n(ring->sq_tail, tail + 1, __ATOMIC_RELEASE);
    if (syscall(__NR_io_uring_enter, ring->sock, 1, 0, 0, NULL, 0) < 0) {
        perror("io_uring_enter submit");
        exit(1);
    }
}

static uint64_t rearms;

static void recv_uring(int sock, struct rx_state* state, double seconds, uint32_t flags, uint32_t nbufs) {
    struct uring ring;
    uring_init(&ring, flags, nbufs);
    br_tail_local = (uint16_t)nbufs;
    arm(&ring, sock);
    uint64_t first = 0;
    while (!rx_done(state, first, seconds)) {
        uint32_t head = *ring.cq_head;
        uint32_t tail = __atomic_load_n(ring.cq_tail, __ATOMIC_ACQUIRE);
        if (head == tail) {
            struct timespec wait = {0, WAIT_NS};
            struct {
                uint64_t sigmask;
                uint32_t sigmask_sz, pad;
                uint64_t wait;
            } arg = {0, 0, 0, (uint64_t)(uintptr_t)&wait};
            (void)syscall(__NR_io_uring_enter, ring.sock, 0, 1, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg,
                          sizeof(arg));
            continue;
        }
        bool rearm = false;
        for (; head != tail; head++) {
            const struct io_uring_cqe* cqe = &ring.cqes[head & *ring.cq_mask];
            if (!(cqe->flags & IORING_CQE_F_MORE)) {
                rearm = true;
            }
            if (cqe->res < 0 || !(cqe->flags & IORING_CQE_F_BUFFER)) {
                continue;
            }
            uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
            const uint8_t* buf = ring.buffers + ((size_t)bid * buf_size);
            const struct io_uring_recvmsg_out* out = (const struct io_uring_recvmsg_out*)buf;
            if (state->received == 0) {
                first = now_ns();
            }
            uint32_t len = out->payloadlen < buf_size - sizeof(*out) ? out->payloadlen : 0;
            got(state, buf + sizeof(*out), len);
            buf_recycle(&ring, bid);
            // Hand the buffer back now, as a datagram's processing ends, not after the batch: a spin inside got()
            // must not hold buffers the kernel could be filling.
            __atomic_store_n(&ring.br->tail, br_tail_local, __ATOMIC_RELEASE);
            __atomic_store_n(ring.cq_head, head + 1, __ATOMIC_RELEASE);
        }
        __atomic_store_n(ring.cq_head, head, __ATOMIC_RELEASE);
        if (rearm) {
            rearms++;
            arm(&ring, sock);
        }
    }
}

// This namespace's IP reassembly failures and timeouts (/proc/net/snmp, the "Ip:" rows): a datagram above the MTU that
// the kernel could not put together never reaches a socket, so neither lost nor socket_drops would name it alone.
static void ip_reasm(uint64_t* fails, uint64_t* timeouts) {
    FILE* snmp = fopen("/proc/net/snmp", "r");
    if (snmp == NULL) {
        return;
    }
    char names[LINE_BYTES];
    char values[LINE_BYTES];
    while (fgets(names, sizeof(names), snmp) != NULL) {
        if (strncmp(names, "Ip:", 3) != 0 || fgets(values, sizeof(values), snmp) == NULL) {
            continue;
        }
        char* name_save = NULL;
        char* value_save = NULL;
        char* name = strtok_r(names, " \n", &name_save);
        char* value = strtok_r(values, " \n", &value_save);
        while (name != NULL && value != NULL) {
            if (strcmp(name, "ReasmFails") == 0) {
                *fails = strtoull(value, NULL, 10);
            } else if (strcmp(name, "ReasmTimeout") == 0) {
                *timeouts = strtoull(value, NULL, 10);
            }
            name = strtok_r(NULL, " \n", &name_save);
            value = strtok_r(NULL, " \n", &value_save);
        }
        break;
    }
    fclose(snmp);
}

// Positions of recv's optional arguments, and the ms in a second / us in a ms for the CPU times.
enum {
    ARG_SPIN = 5,
    ARG_PER = 6,
    ARG_BUFS = 7,
    ARG_BUF_SIZE = 8,
    ARG_SEND_SIZE = 5,
    MS_PER_S = 1000,
    US_PER_MS = 1000
};

static long cpu_ms(struct timeval time) { // NOLINT(misc-include-cleaner) - struct timeval: <sys/resource.h>
    return ((long)time.tv_sec * MS_PER_S) + ((long)time.tv_usec / US_PER_MS);
}

int main(int argc, char** argv) {
    if (argc >= ARG_SEND_SIZE && strcmp(argv[1], "send") == 0) {
        if (argc > ARG_SEND_SIZE) {
            dgram = (uint32_t)strtoul(argv[ARG_SEND_SIZE], NULL, 10);
        }
        return do_send(argv[2], atoi(argv[3]), atof(argv[4]));
    }
    if (argc <= ARG_SPIN || strcmp(argv[1], "recv") != 0) {
        fprintf(stderr,
                "usage: %s send IP PORT SECONDS [SIZE] | recv PORT SECONDS mmsg|uring|coop SPIN_US [PER] [BUFS] "
                "[BUF_SIZE]\n",
                argv[0]);
        return 2;
    }
    int sock = bound_socket(atoi(argv[2]));
    double seconds = atof(argv[3]);
    const char* mode = argv[4];
    struct rx_state state = {0};
    state.spin = strtoull(argv[ARG_SPIN], NULL, 10);
    state.per = argc > ARG_PER ? (uint32_t)strtoul(argv[ARG_PER], NULL, 10) : DEFAULT_PER;
    uint32_t nbufs = argc > ARG_BUFS ? (uint32_t)strtoul(argv[ARG_BUFS], NULL, 10) : DEFAULT_BUFS;
    if (argc > ARG_BUF_SIZE) {
        buf_size = (uint32_t)strtoul(argv[ARG_BUF_SIZE], NULL, 10);
    }
    uint64_t reasm_fails = 0;
    uint64_t reasm_timeout = 0;
    ip_reasm(&reasm_fails, &reasm_timeout);
    if (strcmp(mode, "mmsg") == 0) {
        nbufs = 0;
        recv_mmsg(sock, &state, seconds);
    } else if (strcmp(mode, "uring") == 0) {
        recv_uring(sock, &state, seconds, 0, nbufs);
    } else if (strcmp(mode, "coop") == 0) {
        recv_uring(sock, &state, seconds, IORING_SETUP_COOP_TASKRUN, nbufs);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }
    uint32_t rcvbuf = 0;
    uint32_t drops = socket_drops(sock, &rcvbuf);
    struct rusage usage; // NOLINT(misc-include-cleaner) - struct rusage: <sys/resource.h>
    getrusage(RUSAGE_SELF, &usage);
    uint64_t fails_after = 0;
    uint64_t timeout_after = 0;
    ip_reasm(&fails_after, &timeout_after);
    printf("RESULT: mode=%s spin_us=%llu per=%u bufs=%u buf_size=%u received=%llu lost=%llu "
           "socket_drops=%u rcvbuf=%u "
           "rearms=%llu reasm_fails=%llu reasm_timeout=%llu cpu_user_ms=%ld cpu_sys_ms=%ld\n",
           mode, (unsigned long long)state.spin, state.per, nbufs, buf_size, (unsigned long long)state.received,
           (unsigned long long)state.lost, drops, rcvbuf, (unsigned long long)rearms,
           (unsigned long long)(fails_after - reasm_fails), (unsigned long long)(timeout_after - reasm_timeout),
           cpu_ms(usage.ru_utime), cpu_ms(usage.ru_stime));
    return 0;
}
// NOLINTEND(misc-include-cleaner)
