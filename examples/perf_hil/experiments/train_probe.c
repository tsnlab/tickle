/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// train_probe - what a TickLE reader can see of a packet train's spacing (2026-11-27, adaptive_pacing/DESIGN.md 27.6).
//
// The packet-train rate estimate divides the bytes of a back-to-back train by the time between its first and last
// datagram as the reader's host received them. The Linux receive path batches (recvmmsg() inside the HAL, the io_uring
// drain, UDP_GRO merging same-flow datagrams into one read), which can hide that spacing. This measures it.
//
//   train_probe send --to IP --port P [--n 20] [--size 1468] [--trains 200] [--period-ms 100] [--how hal|plain]
//   train_probe recv [--mode MODE] [--port P] [--n 20] [--size 1468] [--trains 200] [--rate-kbit R] [--overhead 42]
//
// The sender sends TRAINS trains of N datagrams back to back, one train every PERIOD ms. `hal` sends them with
// tt_send_batch() (sendmmsg(), and UDP_SEGMENT when the HAL turns it on: how core sends a large sample's fragments);
// `plain` with one sendto() each. The datagrams are a large sample's FRAG_CONT_L fragments in the single form (the
// HAL's receive offload turns on at the first of those), size 1468 by default (a fragment on the wire), with the train
// id in seq_no and the datagram's index in frag_index.
//
// The receiver reads them by one of
//   hal         the real path: tt_receive(), then tt_try_receive() until it finds nothing, each datagram stamped with
//               the clock right after it was handed out (what core could stamp); `hal` takes the HAL as it is, so run
//               it with TT_UDP_OFFLOAD=0 and 1 (receive offload) and with --drain (tt_rx_drain_start(), as core does
//               while a large sample arrives); it prints the HAL's own counters
//   each        a plain UDP socket, one recvmsg() per datagram, a user stamp after each, and the kernel's receive time
//               stamp (SO_TIMESTAMPING, software) of each
//   batch       the same socket read with recvmmsg() (32 at a time), a user stamp after each call
//   each-gro    `each` with UDP_GRO on: the kernel hands same-flow datagrams over merged, with their size in a cmsg
//               (the HAL's offload does this)
// and says, per train, the span between the first and last datagram (the user stamp's, the kernel's) against the ideal
// (N - 1) x (size + overhead) x 8 / rate for a link of --rate-kbit: the spans' error, the number of distinct stamps
// (N = every datagram has its own stamp, 1 = the whole train under one stamp: the batching hides the spacing), and
// the lag of the user stamp behind the kernel's. Percentiles over the trains that arrived whole.
//
// The receiver's own port is printed first ("port=N"; the HAL's data socket is ephemeral).
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h>
#include <tickle/tickle.h>

// NOLINTBEGIN(misc-include-cleaner) -- SOL_SOCKET, SO_*, SCM_*, CLOCK_*, clockid_t, iovec: glibc-private headers, as
// hal_linux.c
#define MAGIC 0x54525042U // "TRPB"
#define NS_PER_US 1000LL
#define NS_PER_S 1000000000LL
#define NS_PER_MS 1000000LL
#define MAX_N 512U
#define MAX_TRAINS 4096U
#define MAX_DGRAM 1500U
#define BATCH 32U
#define GROUP_GAP_NS (10 * NS_PER_US) // stamps further apart than this are separate reads
#define TRAIN_TIMEOUT_NS (50 * NS_PER_MS)
#define FIRST_TIMEOUT_NS (3 * NS_PER_S)
#define RECV_CONTEXT_ID 61
#define SEND_CONTEXT_ID 62
#define DEFAULT_N 20U
#define DEFAULT_SIZE 1468U
#define DEFAULT_TRAINS 200U
#define DEFAULT_PERIOD_MS 100U
#define DEFAULT_OVERHEAD 42U
#define CMSG_BYTES 256U
#define PERCENTILES 5U
#define BASE_10 10
#define FILL_BYTE 0xA5U
#define MS_PER_S 1000U
#define BITS_PER_BYTE 8LL
#define KILO_NS 1000000LL // ns per bit at 1 kbit/s: bytes x 8 x this / kbit = ns
#define RATIO_SCALE 1000
#define READ_BUFFER_BYTES 65536U
#define SOCKET_BUFFER_BYTES (4 * 1024 * 1024)
#define POLL_SLEEP_US 20U
#define RECEIVE_WAIT_NS (100 * NS_PER_MS)
#define LATER_TIMEOUT_NS (4 * NS_PER_S)
#define PERCENT 100U

struct options {
    const char* mode;
    const char* how;
    const char* to;
    uint16_t port;
    uint32_t n;
    uint32_t size;
    uint32_t trains;
    uint32_t period_ms;
    uint32_t rate_kbit;
    uint32_t overhead;
    bool drain;
    const char* csv;
};

struct stamp { // one datagram as the receiver saw it
    int64_t user_ns;
    int64_t kernel_ns; // 0: none
};

struct train {
    uint32_t id;
    uint32_t got;
    struct stamp at[MAX_N];
    bool seen[MAX_N];
};

// The wire layout of a datagram: tt_SingleHeader, tt_FragContLHeader, the probe's own bytes, filler.
struct probe_dgram {
    uint8_t marker, version, source, type;
    uint32_t entity_id;
    uint32_t seq_no; // the train id
    uint16_t frag_index;
    uint16_t frag_count;
    uint32_t magic;
} __attribute__((packed));

static int64_t now_ns(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return ((int64_t)ts.tv_sec * NS_PER_S) + ts.tv_nsec;
}

static uint8_t native_marker(void) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return tt_SINGLE_MARKER_LE;
#else
    return tt_SINGLE_MARKER_BE;
#endif
}

static bool uarg(const char* text, uint32_t* out) {
    char* end = NULL;
    unsigned long value = strtoul(text, &end, BASE_10);
    if (end == text || *end != 0 || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool parse_text(struct options* opt, const char* key, const char* value) {
    if (strcmp(key, "--mode") == 0) {
        opt->mode = value;
    } else if (strcmp(key, "--how") == 0) {
        opt->how = value;
    } else if (strcmp(key, "--to") == 0) {
        opt->to = value;
    } else if (strcmp(key, "--csv") == 0) {
        opt->csv = value;
    } else {
        return false;
    }
    return true;
}

static bool parse_number(struct options* opt, const char* key, const char* value) {
    uint32_t number = 0;
    if (!uarg(value, &number)) {
        return false;
    }
    if (strcmp(key, "--port") == 0) {
        opt->port = (uint16_t)number;
    } else if (strcmp(key, "--n") == 0) {
        opt->n = number;
    } else if (strcmp(key, "--size") == 0) {
        opt->size = number;
    } else if (strcmp(key, "--trains") == 0) {
        opt->trains = number;
    } else if (strcmp(key, "--period-ms") == 0) {
        opt->period_ms = number;
    } else if (strcmp(key, "--rate-kbit") == 0) {
        opt->rate_kbit = number;
    } else if (strcmp(key, "--overhead") == 0) {
        opt->overhead = number;
    } else {
        return false;
    }
    return true;
}

static bool parse_one(struct options* opt, const char* key, const char* value) {
    return parse_text(opt, key, value) || parse_number(opt, key, value);
}

static bool parse(int argc, char** argv, struct options* opt) {
    *opt = (struct options) {.mode = "hal",
                             .how = "hal",
                             .n = DEFAULT_N,
                             .size = DEFAULT_SIZE,
                             .trains = DEFAULT_TRAINS,
                             .period_ms = DEFAULT_PERIOD_MS,
                             .overhead = DEFAULT_OVERHEAD};
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--drain") == 0) {
            opt->drain = true;
        } else if (i + 1 >= argc || !parse_one(opt, argv[i], argv[i + 1])) {
            fprintf(stderr, "train_probe: bad option %s\n", argv[i]);
            return false;
        } else {
            i++;
        }
    }
    bool sane = opt->n >= 2U && opt->n <= MAX_N && opt->trains <= MAX_TRAINS &&
                opt->size >= sizeof(struct probe_dgram) && opt->size <= MAX_DGRAM;
    if (!sane) {
        fprintf(stderr, "train_probe: --n 2..%u, --trains <= %u, --size %zu..%u\n", MAX_N, MAX_TRAINS,
                sizeof(struct probe_dgram), MAX_DGRAM);
    }
    return sane;
}

static void fill(uint8_t* buf, const struct options* opt, uint32_t train, uint32_t index) {
    memset(buf, FILL_BYTE, opt->size);
    struct probe_dgram head = {.marker = native_marker(),
                               .version = tt_VERSION,
                               .source = SEND_CONTEXT_ID,
                               .type = tt_SUBMESSAGE_TYPE_FRAG_CONT_L,
                               .seq_no = train,
                               .frag_index = (uint16_t)index,
                               .frag_count = (uint16_t)opt->n,
                               .magic = MAGIC};
    memcpy(buf, &head, sizeof(head));
}

// ---- the HAL context both ends of `hal` use
// ---------------------------------------------------------------------------

static bool open_context(struct tt_Context* node, uint8_t context_id) {
    static char broadcast[INET_ADDRSTRLEN];
    const char* given = getenv("BENCH_BROADCAST");
    snprintf(broadcast, sizeof(broadcast), "%s", given != NULL ? given : "127.255.255.255");
    _tt_CONFIG.broadcast = broadcast;
    _tt_CONFIG.context_id = context_id;
    if (tt_Context_create(node) != tt_RET_OK) {
        fprintf(stderr, "train_probe: cannot create a context (BENCH_BROADCAST must be an interface's broadcast)\n");
        return false;
    }
    return true;
}

// ---- sender
// -----------------------------------------------------------------------------------------------------------

static bool send_train_hal(struct tt_Context* node, const struct options* opt, uint32_t train, uint32_t ip) {
    static uint8_t bufs[MAX_N][MAX_DGRAM];
    struct tt_OutDatagram out[MAX_N];
    for (uint32_t i = 0; i < opt->n; i++) {
        fill(bufs[i], opt, train, i);
        out[i] = (struct tt_OutDatagram) {.head = bufs[i], .head_len = opt->size, .ip = ip, .port = opt->port};
    }
    return tt_send_batch(node, out, opt->n) == (int32_t)opt->n;
}

static bool send_train_plain(int sock, const struct options* opt, uint32_t train, const struct sockaddr_in* dest) {
    uint8_t buf[MAX_DGRAM];
    for (uint32_t i = 0; i < opt->n; i++) {
        fill(buf, opt, train, i);
        if (sendto(sock, buf, opt->size, 0, (const struct sockaddr*)dest, sizeof(*dest)) != (ssize_t)opt->size) {
            return false;
        }
    }
    return true;
}

static int run_send(const struct options* opt) {
    struct sockaddr_in dest = {.sin_family = AF_INET, .sin_port = htons(opt->port)};
    if (opt->to == NULL || opt->port == 0 || inet_pton(AF_INET, opt->to, &dest.sin_addr) != 1) {
        fprintf(stderr, "train_probe send: --to IP and --port are required\n");
        return 2;
    }
    bool use_hal = strcmp(opt->how, "hal") == 0;
    struct tt_Context node;
    int sock = use_hal ? -1 : socket(AF_INET, SOCK_DGRAM, 0);
    if (use_hal ? !open_context(&node, SEND_CONTEXT_ID) : sock < 0) {
        return 2;
    }
    uint32_t ip = ntohl(dest.sin_addr.s_addr);
    struct timespec gap = {.tv_sec = opt->period_ms / MS_PER_S,
                           .tv_nsec = (long)(opt->period_ms % MS_PER_S) * NS_PER_MS};
    for (uint32_t train = 0; train < opt->trains; train++) {
        bool sent = use_hal ? send_train_hal(&node, opt, train, ip) : send_train_plain(sock, opt, train, &dest);
        if (!sent) {
            fprintf(stderr, "train_probe send: train %u failed: %s\n", train, strerror(errno));
            return 1;
        }
        nanosleep(&gap, NULL);
    }
    return 0;
}

// ---- receiver: the trains and their statistics
// -------------------------------------------------------------------------

static struct train trains[MAX_TRAINS];
static uint32_t train_count;

static struct train* train_of(uint32_t id) {
    for (uint32_t i = train_count; i > 0; i--) {
        if (trains[i - 1U].id == id) {
            return &trains[i - 1U];
        }
    }
    if (train_count >= MAX_TRAINS) {
        return NULL;
    }
    trains[train_count] = (struct train) {.id = id};
    return &trains[train_count++];
}

// Notes one datagram read at user_ns (kernel_ns 0: none); false when it is not one of this probe's.
static bool note(const uint8_t* buf, size_t len, int64_t user_ns, int64_t kernel_ns, uint32_t max_n) {
    struct probe_dgram head;
    if (len < sizeof(head)) {
        return false;
    }
    memcpy(&head, buf, sizeof(head));
    struct train* train = head.magic == MAGIC ? train_of(head.seq_no) : NULL;
    if (train == NULL || head.frag_index >= max_n || train->seen[head.frag_index]) {
        return false;
    }
    train->seen[head.frag_index] = true;
    train->at[head.frag_index] = (struct stamp) {user_ns, kernel_ns};
    train->got++;
    return true;
}

static int cmp_i64(const void* a, const void* b) {
    int64_t left = *(const int64_t*)a;
    int64_t right = *(const int64_t*)b;
    return (left > right) - (left < right);
}

static int64_t pick(const int64_t* sorted, uint32_t count, uint32_t percent) {
    return count != 0 ? sorted[((uint64_t)(count - 1U) * percent) / PERCENT] : 0;
}

static void print_percentiles(const char* name, int64_t* values, uint32_t count, const char* unit) {
    qsort(values, count, sizeof(*values), cmp_i64);
    static const uint32_t points[PERCENTILES] = {5, 25, 50, 75, 95};
    printf("  %-26s", name);
    for (uint32_t i = 0; i < PERCENTILES; i++) {
        printf(" p%02u %9lld", points[i], (long long)pick(values, count, points[i]));
    }
    printf(" %s\n", unit);
}

// The number of distinct stamps: stamps within GROUP_GAP_NS of each other are one read.
static uint32_t groups_of(const struct train* train, uint32_t count) {
    int64_t sorted[MAX_N];
    for (uint32_t i = 0; i < count; i++) {
        sorted[i] = train->at[i].user_ns;
    }
    qsort(sorted, count, sizeof(*sorted), cmp_i64);
    uint32_t groups = 1;
    for (uint32_t i = 1; i < count; i++) {
        groups += sorted[i] - sorted[i - 1U] > GROUP_GAP_NS ? 1U : 0U;
    }
    return groups;
}

static void span_of(const struct train* train, uint32_t count, int64_t* user, int64_t* kernel) {
    int64_t u_lo = INT64_MAX;
    int64_t u_hi = 0;
    int64_t k_lo = INT64_MAX;
    int64_t k_hi = 0;
    for (uint32_t i = 0; i < count; i++) {
        u_lo = train->at[i].user_ns < u_lo ? train->at[i].user_ns : u_lo;
        u_hi = train->at[i].user_ns > u_hi ? train->at[i].user_ns : u_hi;
        if (train->at[i].kernel_ns != 0) {
            k_lo = train->at[i].kernel_ns < k_lo ? train->at[i].kernel_ns : k_lo;
            k_hi = train->at[i].kernel_ns > k_hi ? train->at[i].kernel_ns : k_hi;
        }
    }
    *user = u_hi - u_lo;
    *kernel = k_hi >= k_lo && k_hi != 0 ? k_hi - k_lo : -1;
}

static void write_csv(const struct options* opt, int64_t ideal_ns) {
    FILE* file = opt->csv != NULL ? fopen(opt->csv, "w") : NULL;
    if (file == NULL) {
        return;
    }
    fprintf(file, "train,got,user_span_ns,kernel_span_ns,ideal_ns,groups\n");
    for (uint32_t idx = 0; idx < train_count; idx++) {
        int64_t user = 0;
        int64_t kernel = 0;
        span_of(&trains[idx], opt->n, &user, &kernel);
        fprintf(file, "%u,%u,%lld,%lld,%lld,%u\n", trains[idx].id, trains[idx].got, (long long)user, (long long)kernel,
                (long long)ideal_ns, trains[idx].got == opt->n ? groups_of(&trains[idx], opt->n) : 0U);
    }
    (void)fclose(file);
}

static void report(const struct options* opt, uint32_t sent_hint) {
    int64_t ideal_ns = 0;
    if (opt->rate_kbit != 0) {
        ideal_ns =
            (int64_t)(opt->n - 1U) * ((int64_t)opt->size + opt->overhead) * BITS_PER_BYTE * KILO_NS / opt->rate_kbit;
    }
    static int64_t user_err[MAX_TRAINS];
    static int64_t kernel_err[MAX_TRAINS];
    static int64_t user_ratio[MAX_TRAINS];
    static int64_t lag[MAX_TRAINS];
    static int64_t groups[MAX_TRAINS];
    uint32_t whole = 0;
    uint32_t kernel_whole = 0;
    for (uint32_t idx = 0; idx < train_count; idx++) {
        if (trains[idx].got != opt->n) {
            continue;
        }
        int64_t user = 0;
        int64_t kernel = 0;
        span_of(&trains[idx], opt->n, &user, &kernel);
        user_err[whole] = user - ideal_ns;
        user_ratio[whole] = ideal_ns != 0 ? user * RATIO_SCALE / ideal_ns : 0;
        groups[whole] = groups_of(&trains[idx], opt->n);
        if (kernel >= 0) {
            kernel_err[kernel_whole] = kernel - ideal_ns;
            lag[kernel_whole] = trains[idx].at[opt->n - 1U].user_ns - trains[idx].at[opt->n - 1U].kernel_ns;
            kernel_whole++;
        }
        whole++;
    }
    printf(
        "mode=%s n=%u size=%u rate_kbit=%u overhead=%u drain=%d trains_seen=%u whole=%u (sent %u) ideal_span_us=%lld\n",
        opt->mode, opt->n, opt->size, opt->rate_kbit, opt->overhead, opt->drain ? 1 : 0, train_count, whole, sent_hint,
        (long long)(ideal_ns / NS_PER_US));
    if (whole == 0) {
        printf("  no train arrived whole\n");
        return;
    }
    print_percentiles("user span - ideal (ns)", user_err, whole, "ns");
    print_percentiles("user span / ideal (x1000)", user_ratio, whole, "");
    print_percentiles("distinct stamps per train", groups, whole,
                      "(n = every datagram has its own stamp; 1 = all under one)");
    if (kernel_whole != 0) {
        print_percentiles("kernel span - ideal (ns)", kernel_err, kernel_whole, "ns");
        print_percentiles("user stamp - kernel stamp (ns)", lag, kernel_whole, "ns (last datagram)");
    }
    write_csv(opt, ideal_ns);
}

// ---- receiver: the readers
// ----------------------------------------------------------------------------------------------

static int64_t wait_limit(void) {
    return train_count == 0 ? FIRST_TIMEOUT_NS : LATER_TIMEOUT_NS;
}

static bool train_done(const struct options* opt) {
    return train_count != 0 && trains[train_count - 1U].got == opt->n && train_count >= opt->trains;
}

static uint32_t landing_offset(const struct tt_Context* node) {
#if TT_HAL_UDP_GRO
    return node->rx_offset;
#else
    (void)node;
    return 0;
#endif
}

// `hal`: the real path.
static int read_hal(const struct options* opt, struct tt_Context* node) {
    uint8_t* landing =
        node->rx_buffer; // where the HAL reads, as core passes it; with UDP_GRO a datagram lies at rx_offset
    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t last = now_ns(CLOCK_MONOTONIC);
    uint64_t buffered_sum = 0;
    uint64_t reads = 0;
    while (!train_done(opt) && now_ns(CLOCK_MONOTONIC) - last < wait_limit()) {
        if (opt->drain) {
            (void)tt_rx_drain_start(node, tt_CONTROL_MAX_LENGTH);
        }
        int32_t got = tt_receive(node, landing, tt_MAX_BUFFER_LENGTH, &ip, &port, RECEIVE_WAIT_NS);
        while (got >= 0) {
            int64_t stamp = now_ns(CLOCK_MONOTONIC);
            buffered_sum += tt_rx_buffered(node);
            reads++;
            if (note(landing + landing_offset(node), (size_t)got, stamp, 0, opt->n)) {
                last = stamp;
            }
            got = tt_try_receive(node, landing, tt_MAX_BUFFER_LENGTH, &ip, &port);
        }
    }
    printf("hal counters: rx_batch_calls=%lu rx_batch_datagrams=%lu buffered_per_handout=%.1f socket_drops=%u\n",
           (unsigned long)node->hal.rx_batch_calls, (unsigned long)node->hal.rx_batch_datagrams,
           reads != 0 ? (double)buffered_sum / (double)reads : 0.0, (unsigned)tt_hal_rx_socket_drops(node));
#if tt_RX_DRAIN
    printf("hal drain: drain_datagrams=%lu\n", (unsigned long)node->hal.drain_datagrams);
#endif
#if TT_HAL_UDP_GRO
    printf("hal receive offload: gro_enables=%lu\n", (unsigned long)node->hal.gro_enables);
#endif
    return 0;
}

static int raw_socket(uint16_t port, bool gro, uint16_t* bound) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }
    int enable = 1;
    int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    int big = SOCKET_BUFFER_BYTES;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    socklen_t len = sizeof(addr);
    bool ready = setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) == 0 &&
                 (!gro || setsockopt(sock, SOL_UDP, UDP_GRO, &enable, sizeof(enable)) == 0) &&
                 bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
                 getsockname(sock, (struct sockaddr*)&addr, &len) == 0;
    if (!ready) {
        fprintf(stderr, "train_probe: raw socket setup: %s\n", strerror(errno));
        (void)close(sock);
        return -1;
    }
    *bound = ntohs(addr.sin_port);
    return sock;
}

static int64_t kernel_stamp(struct msghdr* msg) {
    for (struct cmsghdr* cm = CMSG_FIRSTHDR(msg); cm != NULL; cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPING) {
            struct timespec ts[3];
            memcpy(ts, CMSG_DATA(cm), sizeof(ts));
            return ((int64_t)ts[0].tv_sec * NS_PER_S) + ts[0].tv_nsec;
        }
    }
    return 0;
}

static uint32_t gro_size(struct msghdr* msg) {
    for (struct cmsghdr* cm = CMSG_FIRSTHDR(msg); cm != NULL; cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == SOL_UDP && cm->cmsg_type == UDP_GRO) {
            int size = 0;
            memcpy(&size, CMSG_DATA(cm), sizeof(size));
            return (uint32_t)size;
        }
    }
    return 0;
}

// The bytes of the datagram at `offset` in a read of `total` bytes whose segments are `segment` long (0: not merged).
static size_t piece_length(size_t total, size_t offset, uint32_t segment) {
    if (segment == 0) {
        return total;
    }
    return total - offset < segment ? total - offset : segment;
}

// One recvmsg() at a time (with UDP_GRO a read may hold several datagrams: each is noted at the one stamp).
static int read_each(const struct options* opt, int sock, bool gro) {
    static uint8_t buf[READ_BUFFER_BYTES];
    uint8_t control[CMSG_BYTES];
    int64_t last = now_ns(CLOCK_REALTIME);
    uint64_t reads = 0;
    uint64_t merged = 0;
    while (!train_done(opt) && now_ns(CLOCK_REALTIME) - last < wait_limit()) {
        struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
        struct msghdr msg = {.msg_iov = &iov,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof(control)};
        ssize_t got = recvmsg(sock, &msg, MSG_DONTWAIT);
        if (got < 0) {
            usleep(POLL_SLEEP_US);
            continue;
        }
        int64_t user = now_ns(CLOCK_REALTIME);
        int64_t kernel = kernel_stamp(&msg);
        uint32_t segment = gro ? gro_size(&msg) : 0;
        uint32_t pieces = segment != 0 ? ((uint32_t)got + segment - 1U) / segment : 1U;
        reads++;
        merged += pieces > 1U ? 1U : 0U;
        for (uint32_t i = 0; i < pieces; i++) {
            size_t offset = (size_t)i * segment;
            size_t len = piece_length((size_t)got, offset, segment);
            if (note(buf + offset, len, user, kernel, opt->n)) {
                last = user;
            }
        }
    }
    printf("raw reads=%lu merged_reads=%lu\n", (unsigned long)reads, (unsigned long)merged);
    return 0;
}

static int read_batch(const struct options* opt, int sock) {
    static uint8_t bufs[BATCH][MAX_DGRAM];
    struct mmsghdr msgs[BATCH];
    struct iovec iov[BATCH];
    int64_t last = now_ns(CLOCK_REALTIME);
    uint64_t calls = 0;
    while (!train_done(opt) && now_ns(CLOCK_REALTIME) - last < wait_limit()) {
        for (uint32_t i = 0; i < BATCH; i++) {
            iov[i] = (struct iovec) {.iov_base = bufs[i], .iov_len = MAX_DGRAM};
            msgs[i] = (struct mmsghdr) {.msg_hdr = {.msg_iov = &iov[i], .msg_iovlen = 1}};
        }
        int got = recvmmsg(sock, msgs, BATCH, MSG_DONTWAIT, NULL);
        if (got <= 0) {
            usleep(POLL_SLEEP_US);
            continue;
        }
        int64_t user = now_ns(CLOCK_REALTIME);
        calls++;
        for (int i = 0; i < got; i++) {
            if (note(bufs[i], msgs[i].msg_len, user, 0, opt->n)) {
                last = user;
            }
        }
    }
    printf("raw recvmmsg calls=%lu\n", (unsigned long)calls);
    return 0;
}

static int run_recv(const struct options* opt) {
    int result = 2;
    if (strcmp(opt->mode, "hal") == 0) {
        struct tt_Context node;
        if (!open_context(&node, RECV_CONTEXT_ID)) {
            return 2;
        }
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        if (getsockname(node.hal.data_sock, (struct sockaddr*)&addr, &len) == 0) {
            printf("port=%u\n", (unsigned)ntohs(addr.sin_port));
            (void)fflush(stdout);
        }
        result = read_hal(opt, &node);
    } else {
        bool gro = strcmp(opt->mode, "each-gro") == 0;
        uint16_t bound = 0;
        int sock = raw_socket(opt->port, gro, &bound);
        if (sock < 0) {
            return 2;
        }
        printf("port=%u\n", (unsigned)bound);
        (void)fflush(stdout);
        if (strcmp(opt->mode, "batch") == 0) {
            result = read_batch(opt, sock);
        } else if (strcmp(opt->mode, "each") == 0 || gro) {
            result = read_each(opt, sock, gro);
        } else {
            fprintf(stderr, "train_probe: unknown --mode %s\n", opt->mode);
        }
    }
    report(opt, opt->trains);
    return result;
}

int main(int argc, char** argv) {
    struct options opt;
    if (argc < 2 || !parse(argc, argv, &opt)) {
        fprintf(stderr, "usage: train_probe send|recv [options] (see the file's comment)\n");
        return 2;
    }
    if (strcmp(argv[1], "send") == 0) {
        return run_send(&opt);
    }
    return strcmp(argv[1], "recv") == 0 ? run_recv(&opt) : 2;
}
// NOLINTEND(misc-include-cleaner)
