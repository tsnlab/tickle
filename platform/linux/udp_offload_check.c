// Copyright (c) 2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// UDP offload (hal_linux.c "UDP offload") through the real Linux HAL, run by udp_offload_check.sh in two private
// network namespaces joined by a veth pair. Built against core with rmw's datagram (tt_MAX_BUFFER_LENGTH 65507), the
// one build that compiles receive offload in.
//
//   udp_offload_check send IP PORT BROADCAST [REPEAT]   tt_send_batch() of the fixed patterns below to IP:PORT,
//                                              REPEAT times over (1), then "UEND"
//   udp_offload_check plain PORT                a plain socket, no UDP_GRO: one line per datagram the wire delivered
//   udp_offload_check hal PORT BROADCAST        a context's well-known socket on PORT, read with tt_receive()
//
// Every receiver prints one line per test datagram, "pattern index size hash", so two runs compare with diff: the
// datagrams the wire carried, their sizes and their bytes, in order. The plain receiver reads each datagram as the
// kernel cut it, so with send offload on the sender it shows what the segmentation produced; the hal receiver checks
// each datagram's bytes against the pattern where tt_receive() put it (buffer + rx_offset, 4-aligned) and so shows
// what receive offload handed core. The last line of each role is a "RESULT:" with its counters, always printed.
//
// Exit: 0 ran (the script compares), 1 a datagram that is not what was sent, an offset off 4, or a size above 1472
// (hal/plain), 2 setup failed.
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507 // rmw_tickle's datagram: the build with receive offload in it
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> // NOLINT(misc-include-cleaner) - nanosleep(), struct timespec

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h> // NOLINT(misc-include-cleaner) - SOL_SOCKET, SO_*: glibc-private headers behind it
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h>
#include <tickle/tickle.h>

#define MAGIC_LEN 4U
#define HEADER_LEN 12U      // magic (4), pattern (4: its number << 8, and the low byte FULL_TYPE_AT), index (4)
#define SPLIT_AT 16U        // head/body split of a two-piece datagram
#define END_LEN 4U          // "UEND"
#define SEND_REPEAT_ARG 5   // argv index of send's optional REPEAT
#define BATCH 64U           // fragments per tt_send_batch() call, as core sends a large sample
#define MAX_RUN 128U        // datagrams in the largest single call below
#define PAUSE_NS 20000000L  // between patterns: the receiver drains
#define IDLE_POLLS 50       // 100 ms waits without a datagram before a receiver gives up (5 s)
#define WAIT_NS 100000000LL // 100 ms
#define NS_PER_US 1000LL
// The flapping build's receiver is slower than the sender on pattern 3 (udp_offload_check.sh, arm E): 1.5 ms per
// datagram, 45 ms for its 30 against 20 ms pauses between patterns, so pattern 4's merged runs are already queued when
// receive offload, quiet after pattern 3's last small datagrams, turns to go back to plain reads - the moment its
// switch must not read one of them without the option on. It catches up on patterns 4 and 5 and goes back in the
// pause before the next pass.
#ifndef CHECK_READ_DELAY_NS
#define CHECK_READ_DELAY_NS 0L
#endif
#define RCVBUF (8 << 20)    // root in the namespace: SO_RCVBUFFORCE
#define CHECK_CONTEXT_ID 61 // any valid id; the two contexts here never discover each other
#define WIRE_MAX 1472U      // the most any datagram may carry (DESIGN.md section 8: no IP fragmentation)
#define FNV_OFFSET 2166136261U
#define FNV_PRIME 16777619U
#define FILL_INDEX_MUL 31U
#define FILL_OFFSET_MUL 7U

static const char k_magic[MAGIC_LEN] = {'U', 'O', 'F', 'T'};
// Pattern 1's datagrams begin as a large sample's fragment does (a single-form header naming FRAG_CONT_L), because that
// is what turns receive offload on (hal_linux.c, "When receive offload is on"); the other patterns are small traffic.
static const char k_frag_magic[MAGIC_LEN] = {tt_SINGLE_MARKER_LE, 'U', 'O', tt_SUBMESSAGE_TYPE_FRAG_CONT_L};
// Pattern 7's begin as a large fragment retransmitted to one node does: the full form, the magic "KT" and then the
// submessage header, whose type is byte 4 - the low byte of the pattern word (HEADER_LEN), FRAG_CONT_L for this
// pattern and 0 for every other.
static const char k_full_magic[MAGIC_LEN] = {'K', 'T', 'U', 'O'};
#define FULL_PATTERN 6U
static const char* magic_of(uint32_t number) {
    if (number == 0 || number == 5) {
        return k_frag_magic;
    }
    return number == FULL_PATTERN ? k_full_magic : k_magic;
}
static uint32_t pattern_word(uint32_t number) {
    return (number << 8) | (number == FULL_PATTERN ? (uint32_t)tt_SUBMESSAGE_TYPE_FRAG_CONT_L : 0U);
}

// The patterns, each a list of datagram sizes sent in the calls given. Chosen for what each makes gso_run() decide -
// which since 2026-10-11 sends a run of large fragments only (hal_linux.c, "Which runs go as one"), so patterns 2-5,
// small traffic, never form one (udp_offload_check.sh checks the count) and reach the receiver one datagram each:
//   1 a 1 MB large sample: 722 x 1468 (4 + 12 + 1452, the FRAG_CONT_L datagram) and a short last one, in calls of 64
//     (runs of 44, the 65507-byte bound, and 20; the last call one run of 19)
//   2 full 1472-byte datagrams, 88 in one call: same size, same destination, but not fragments - no run
//   3 mixed sizes in one call; 1002 is 2 mod 4 (copied by receive offload), 1468 4 mod 8 - and it ends on three
//     growing sizes: three small reads in a row, after which receive offload turns to go back to plain reads (arm E)
//     while pattern 4's datagrams may already be queued
//   4 head only (body_len 0) datagrams
//   5 alternating destinations (PORT, PORT + 1); only PORT's half is received
//   6 large fragments of mixed sizes in one call: a run ends at a longer one, takes a shorter one as its last - runs of
//     5, 2, 3, 7, 2, 3, 2 (the second cycle continues the first one's last run)
//   7 large fragments in the full form (a retransmission to one node), 20 in one call: one run
struct pattern {
    uint32_t count;
    uint32_t sizes[32]; // repeated cyclically when count is larger
    uint32_t size_count;
    uint32_t call; // datagrams per tt_send_batch() call
    bool head_only;
    bool alternate;
};
static const struct pattern k_patterns[] = {
    {723, {1468}, 1, BATCH, false, false},
    {88, {1472}, 1, 88, false, false},
    {30,
     {1000, 1000, 1000, 1472, 200, 200, 200, 200,  200,  1002, 1002, 1002, 1002, 13,  1468,
      1468, 1468, 1472, 1472, 64,  64,  64,  1472, 1472, 1472, 1472, 1472, 100,  300, 700},
     30,
     30,
     false,
     false},
    {64, {600}, 1, 64, true, false},
    {40, {1472}, 1, 40, false, true},
    {24, {1468, 1468, 1468, 1468, 1000, 1000, 1000, 1468, 1468, 404, 1468, 1468}, 12, 24, false, false},
    {20, {1468}, 1, 20, false, false},
};
// The datagrams a sender with send offload puts in runs, per pass, and the runs: pattern 1's 723 in 11 x 2 + 1, pattern
// 6's 24 in 7, pattern 7's 20 in 1.
#define GSO_DATAGRAMS_PER_PASS (723U + 24U + 20U)
#define GSO_SENDS_PER_PASS (23U + 7U + 1U)
#define PATTERN_COUNT (sizeof(k_patterns) / sizeof(k_patterns[0]))
#define LARGE_LAST 404U // pattern 1's last datagram

static uint32_t pattern_size(uint32_t p, uint32_t i) {
    const struct pattern* pat = &k_patterns[p];
    if (p == 0 && i == pat->count - 1) {
        return LARGE_LAST;
    }
    return pat->sizes[i % pat->size_count];
}

static void fill(uint8_t* out, uint32_t p, uint32_t i, uint32_t size) {
    memcpy(out, magic_of(p), MAGIC_LEN);
    const uint32_t word = pattern_word(p);
    memcpy(out + MAGIC_LEN, &word, sizeof(word));
    memcpy(out + MAGIC_LEN + sizeof(p), &i, sizeof(i));
    for (uint32_t k = HEADER_LEN; k < size; k++) {
        out[k] = (uint8_t)((i * FILL_INDEX_MUL) + (k * FILL_OFFSET_MUL) + p);
    }
}

static uint32_t fnv(const uint8_t* data, uint32_t len) {
    uint32_t hash = FNV_OFFSET;
    for (uint32_t k = 0; k < len; k++) {
        hash = (hash ^ data[k]) * FNV_PRIME;
    }
    return hash;
}

static void pause_between(void) {
    struct timespec pause = {0, PAUSE_NS};
    nanosleep(&pause, NULL);
}

static struct tt_Context g_node;
static uint8_t g_storage[MAX_RUN][WIRE_MAX];
static _Alignas(8) uint8_t g_rx[tt_RX_POOL_BUFFER_BYTES];

static bool context_up(const char* broadcast, uint16_t port) {
    _tt_CONFIG.broadcast = (char*)broadcast; // argv: lives as long as the process
    _tt_CONFIG.port = port;
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    return tt_Context_create(&g_node) == tt_RET_OK;
}

// One call's datagrams of pattern `number`, from `first`: `count` of them, built in g_storage, described in `out`.
static void build_call(struct tt_OutDatagram* out, uint32_t number, uint32_t first, uint32_t count, uint32_t ip,
                       uint16_t port) {
    const struct pattern* pat = &k_patterns[number];
    for (uint32_t k = 0; k < count; k++) {
        uint32_t index = first + k;
        uint32_t size = pattern_size(number, index);
        fill(g_storage[k], number, index, size);
        bool split = !pat->head_only && size > SPLIT_AT;
        out[k] = (struct tt_OutDatagram) {.head = g_storage[k],
                                          .head_len = split ? SPLIT_AT : size,
                                          .body = split ? g_storage[k] + SPLIT_AT : NULL,
                                          .body_len = split ? size - SPLIT_AT : 0,
                                          .ip = ip,
                                          .port = (uint16_t)(pat->alternate && (index % 2U) == 1U ? port + 1 : port)};
    }
}

static int run_send(uint32_t ip, uint16_t port, const char* broadcast, uint32_t repeat) {
    // The sender's well-known port is not the receivers': its own announces must not reach the hal receiver.
    if (!context_up(broadcast, (uint16_t)(port + 2))) {
        fprintf(stderr, "cannot create the context\n");
        return 2;
    }
    uint32_t sent = 0;
    for (uint32_t pass = 0; pass < repeat * PATTERN_COUNT; pass++) {
        const uint32_t number = pass % PATTERN_COUNT;
        const struct pattern* pat = &k_patterns[number];
        for (uint32_t first = 0; first < pat->count; first += pat->call) {
            struct tt_OutDatagram out[MAX_RUN];
            uint32_t count = pat->count - first < pat->call ? pat->count - first : pat->call;
            build_call(out, number, first, count, ip, port);
            if (tt_send_batch(&g_node, out, count) != (int32_t)count) {
                perror("tt_send_batch");
                return 2;
            }
            sent += count;
        }
        pause_between();
    }
    struct tt_OutDatagram end = {.head = "UEND", .head_len = END_LEN, .ip = ip, .port = port};
    (void)tt_send_batch(&g_node, &end, 1);
    printf("RESULT: role=send datagrams=%u udp_offload=%u gso_sends=%lu gso_datagrams=%lu gso_sends_expected=%u "
           "gso_datagrams_expected=%u\n",
           sent, (unsigned)g_node.udp_offload, (unsigned long)g_node.udp_gso_sends,
           (unsigned long)g_node.udp_gso_datagrams, GSO_SENDS_PER_PASS * repeat, GSO_DATAGRAMS_PER_PASS * repeat);
    tt_Context_destroy(&g_node);
    return 0;
}

// One received datagram: checked against the pattern it names, printed. False when it is not what was sent.
static bool record(const uint8_t* data, uint32_t len, uint32_t* records, uint32_t* largest, bool* ended) {
    if (len == END_LEN && memcmp(data, "UEND", END_LEN) == 0) {
        *ended = true;
        return true;
    }
    uint32_t number = 0;
    uint32_t index = 0;
    if (len < HEADER_LEN) {
        return true; // not ours
    }
    memcpy(&number, data + MAGIC_LEN, sizeof(number));
    number >>= 8;
    if (number >= PATTERN_COUNT || memcmp(data, magic_of(number), MAGIC_LEN) != 0) {
        return true; // not ours (an announce)
    }
    memcpy(&index, data + MAGIC_LEN + sizeof(number), sizeof(index));
    *largest = len > *largest ? len : *largest;
    (*records)++;
    printf("%u %u %u %08x\n", number, index, len, fnv(data, len));
    if (number >= PATTERN_COUNT || index >= k_patterns[number].count || len != pattern_size(number, index) ||
        len > WIRE_MAX) {
        fprintf(stderr, "datagram %u/%u: %u bytes, not what was sent\n", number, index, len);
        return false;
    }
    static uint8_t expect[WIRE_MAX];
    fill(expect, number, index, len);
    if (memcmp(expect, data, len) != 0) {
        fprintf(stderr, "datagram %u/%u: bytes differ from what was sent\n", number, index);
        return false;
    }
    return true;
}

static int run_plain(uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int size = RCVBUF;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size)); // NOLINT(misc-include-cleaner)
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {htonl(INADDR_ANY)}};
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 2;
    }
    struct timeval wait = // NOLINT(misc-include-cleaner) - via <sys/socket.h>
        {0, WAIT_NS / NS_PER_US};
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait)); // NOLINT(misc-include-cleaner)
    uint32_t records = 0;
    uint32_t largest = 0;
    bool ended = false;
    bool good = true;
    int idle = 0;
    static uint8_t buf[tt_MAX_BUFFER_LENGTH];
    while (!ended && idle < IDLE_POLLS) {
        ssize_t got = recv(sock, buf, sizeof(buf), 0);
        if (got < 0) {
            idle++;
            continue;
        }
        idle = 0;
        good = record(buf, (uint32_t)got, &records, &largest, &ended) && good;
    }
    printf("RESULT: role=plain datagrams=%u largest=%u ended=%d\n", records, largest, ended);
    return good ? 0 : 1;
}

static int run_hal(uint16_t port, const char* broadcast) {
    if (!context_up(broadcast, port)) {
        fprintf(stderr, "cannot create the context\n");
        return 2;
    }
    int size = RCVBUF;
    // NOLINTNEXTLINE(misc-include-cleaner)
    (void)setsockopt(g_node.hal.sock, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size));
    uint32_t records = 0;
    uint32_t largest = 0;
    uint32_t misaligned = 0;
    bool ended = false;
    bool good = true;
    int idle = 0;
    while (!ended && idle < IDLE_POLLS) {
        uint32_t ip = 0;
        uint16_t from = 0;
        int32_t got = tt_receive(&g_node, g_rx, tt_MAX_BUFFER_LENGTH, &ip, &from, WAIT_NS);
        if (got < 0) {
            idle += got == -1 ? 1 : 0;
            continue;
        }
        idle = 0;
        uint32_t number = PATTERN_COUNT;
        if (got >= (int32_t)HEADER_LEN) {
            memcpy(&number, g_rx + g_node.rx_offset + MAGIC_LEN, sizeof(number));
            number >>= 8;
        }
        if (CHECK_READ_DELAY_NS > 0 && number == 2) {
            struct timespec delay = {0, CHECK_READ_DELAY_NS};
            nanosleep(&delay, NULL);
        }
        if (g_node.rx_offset % 4U != 0) {
            misaligned++;
            good = false;
        }
        good = record(g_rx + g_node.rx_offset, (uint32_t)got, &records, &largest, &ended) && good;
    }
    printf("RESULT: role=hal datagrams=%u largest=%u ended=%d misaligned=%u udp_offload=%u gro_reads=%lu "
           "gro_merged=%lu gro_copied=%lu gro_off8=%lu gro_enables=%lu gro_aborts=%lu gro_commits=%lu\n",
           records, largest, ended, misaligned, (unsigned)g_node.udp_offload, (unsigned long)g_node.udp_gro_reads,
           (unsigned long)g_node.udp_gro_merged, (unsigned long)g_node.udp_gro_copied,
           (unsigned long)g_node.udp_gro_off8, (unsigned long)g_node.hal.gro_enables,
           (unsigned long)g_node.hal.gro_aborts, (unsigned long)g_node.hal.gro_commits);
    tt_Context_destroy(&g_node);
    return good ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 5 && strcmp(argv[1], "send") == 0) {
        struct in_addr dest;
        if (inet_pton(AF_INET, argv[2], &dest) != 1) {
            return 2;
        }
        return run_send(ntohl(dest.s_addr), (uint16_t)atoi(argv[3]), argv[4],
                        argc > SEND_REPEAT_ARG ? (uint32_t)atoi(argv[SEND_REPEAT_ARG]) : 1U);
    }
    if (argc >= 3 && strcmp(argv[1], "plain") == 0) {
        return run_plain((uint16_t)atoi(argv[2]));
    }
    if (argc >= 4 && strcmp(argv[1], "hal") == 0) {
        return run_hal((uint16_t)atoi(argv[2]), argv[3]);
    }
    fprintf(stderr, "usage: %s send IP PORT BROADCAST | plain PORT | hal PORT BROADCAST\n", argv[0]);
    return 2;
}
