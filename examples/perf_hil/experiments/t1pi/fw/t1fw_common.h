/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Shared pieces of the T1 framework-comparison nodes (cyclonedds_node.c, fastdds_node.cpp, zenohpico_node.c): the
// command line, the 16-byte sample header, first-copy scoring, the quantiles and the RESULT line. Plain C so both the C
// and the C++ node include it. The roles mirror wifi_link_node.c (small cells) and large_sample_bench.c (large cell):
//
//   ping  publishes SIZE-byte samples at HZ for -d seconds once matched; the first -r samples (one second at -r HZ) are
//         warm-up and not scored; times each sample's round trip from its own send stamp to the first pong naming it;
//         then drains DRAIN_S.
//   echo  receives ping samples, scores one-way delay (first copy only) and answers the first copy with a 16-byte pong
//         from inside the receive callback / the receive loop. Runs -d seconds or until the -q stop file exists.
//   pub   publishes floor(d*HZ) samples of SIZE bytes at HZ (large cell), then stays until -d + -Q seconds after its
//   first
//         publish (-Q 3, large_sample_bench's DRAIN_NS) so resends are answered.
//   sub   receives them, scores every first copy (no warm-up), one-way latency from the send stamp. -d seconds or -q.
//
// Sample = 16 header bytes (send_ns u64, seq u32, reserved u32) then zeros up to SIZE; the pong is the 16 header bytes.
// Times are CLOCK_MONOTONIC (every namespace shares the host's clock). One RESULT line per role, on stdout.
#ifndef T1FW_COMMON_H
#define T1FW_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define T1_HDR 16U
#define T1_MAX_SEQ 65536U
#define T1_MATCH_WAIT_S 15.0
#define T1_DRAIN_S 3.0

enum t1_role { T1_PING, T1_ECHO, T1_PUB, T1_SUB };

struct t1_opts {
    enum t1_role role;
    double seconds;    // -d
    int reliable;      // -m rel
    uint32_t size;     // -s
    double rate_hz;    // -r
    uint32_t depth;    // -K
    double drain_s;    // -Q (pub: wait after the last publish)
    double block_s;    // -B DDS max_blocking_time, seconds
    const char* ip;    // -i own address (interface selection)
    const char* iface; // -I interface name
    const char* stop;  // -q stop file
    const char* lat;   // -l latency file (us, sorted, one per line)
};

static inline uint64_t t1_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static inline int t1_parse(int argc, char** argv, struct t1_opts* o) {
    memset(o, 0, sizeof(*o));
    o->seconds = 60.0;
    o->size = 100;
    o->rate_hz = 100.0;
    o->depth = 100;
    o->drain_s = 3.0;
    o->block_s = 1.0;
    if (argc < 2) {
        return -1;
    }
    if (strcmp(argv[1], "ping") == 0) {
        o->role = T1_PING;
    } else if (strcmp(argv[1], "echo") == 0) {
        o->role = T1_ECHO;
    } else if (strcmp(argv[1], "pub") == 0) {
        o->role = T1_PUB;
    } else if (strcmp(argv[1], "sub") == 0) {
        o->role = T1_SUB;
    } else {
        return -1;
    }
    int c;
    optind = 2;
    while ((c = getopt(argc, argv, "d:m:s:r:K:Q:B:i:I:q:l:")) != -1) {
        switch (c) {
        case 'd':
            o->seconds = atof(optarg);
            break;
        case 'm':
            o->reliable = strcmp(optarg, "rel") == 0;
            break;
        case 's':
            o->size = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'r':
            o->rate_hz = atof(optarg);
            break;
        case 'K':
            o->depth = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'Q':
            o->drain_s = atof(optarg);
            break;
        case 'B':
            o->block_s = atof(optarg);
            break;
        case 'i':
            o->ip = optarg;
            break;
        case 'I':
            o->iface = optarg;
            break;
        case 'q':
            o->stop = optarg;
            break;
        case 'l':
            o->lat = optarg;
            break;
        default:
            return -1;
        }
    }
    if (o->size < T1_HDR) {
        o->size = T1_HDR;
    }
    return 0;
}

static inline int t1_stop_asked(const struct t1_opts* o) {
    return o->stop != NULL && access(o->stop, F_OK) == 0;
}

static inline void t1_hdr_put(uint8_t* buf, uint64_t send_ns, uint32_t seq) {
    memcpy(buf, &send_ns, 8);
    memcpy(buf + 8, &seq, 4);
    uint32_t zero = 0;
    memcpy(buf + 12, &zero, 4);
}

static inline void t1_hdr_get(const uint8_t* buf, uint64_t* send_ns, uint32_t* seq) {
    memcpy(send_ns, buf, 8);
    memcpy(seq, buf + 8, 4);
}

// First-copy scoring state of one role.
struct t1_score {
    uint8_t seen[T1_MAX_SEQ];
    uint64_t lat_ns[T1_MAX_SEQ];
    uint32_t scored, duplicates, below_warmup, max_seq, received;
    uint32_t warmup;
};

// Returns 1 for a first copy (to answer), 0 for a duplicate or an out-of-range seq.
static inline int t1_record(struct t1_score* s, uint32_t seq, uint64_t send_ns, uint64_t now) {
    if (seq >= T1_MAX_SEQ) {
        return 0;
    }
    s->received++;
    if (s->seen[seq] != 0) {
        s->duplicates++;
        return 0;
    }
    s->seen[seq] = 1;
    if (seq > s->max_seq) {
        s->max_seq = seq;
    }
    if (seq < s->warmup) {
        s->below_warmup++;
        return 1;
    }
    s->lat_ns[s->scored++] = now - send_ns;
    return 1;
}

static inline int t1_cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static inline void t1_sort(struct t1_score* s) {
    qsort(s->lat_ns, s->scored, sizeof(uint64_t), t1_cmp_u64);
}

// Same index rule as wifi_link_node.c: floor(n * percent / 100), clamped.
static inline double t1_q_us(const struct t1_score* s, uint32_t percent) {
    if (s->scored == 0) {
        return 0.0;
    }
    uint32_t i = (uint32_t)(((uint64_t)s->scored * percent) / 100U);
    i = i >= s->scored ? s->scored - 1U : i;
    return (double)s->lat_ns[i] / 1000.0;
}

static inline void t1_write_lat(const struct t1_opts* o, const struct t1_score* s) {
    if (o->lat == NULL) {
        return;
    }
    FILE* f = fopen(o->lat, "we");
    if (f == NULL) {
        return;
    }
    for (uint32_t i = 0; i < s->scored; i++) {
        fprintf(f, "%.1f\n", (double)s->lat_ns[i] / 1000.0);
    }
    fclose(f);
}

static inline double t1_cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1e6);
}

static inline void t1_sleep_until(uint64_t until_ns) {
    uint64_t n = t1_now_ns();
    if (until_ns <= n) {
        return;
    }
    uint64_t d = until_ns - n;
    struct timespec ts = {.tv_sec = (time_t)(d / 1000000000ULL), .tv_nsec = (long)(d % 1000000000ULL)};
    nanosleep(&ts, NULL);
}

// One RESULT line. matched: 1 if the role's endpoints matched; published/failed only for ping and pub.
static inline void t1_result(const char* fw, const struct t1_opts* o, struct t1_score* s, int matched,
                             uint32_t published, uint32_t failed, uint64_t start_ns, uint64_t end_ns,
                             const char* extra) {
    static const char* names[] = {"ping", "echo", "pub", "sub"};
    t1_sort(s);
    t1_write_lat(o, s);
    printf("RESULT fw=%s role=%s reliable=%d size=%u rate_hz=%.3f depth=%u matched=%d start_ns=%llu end_ns=%llu "
           "published=%u "
           "publish_failed=%u warmup=%u scored=%u received=%u duplicates=%u below_warmup=%u max_seq=%u p50_us=%.1f "
           "p90_us=%.1f "
           "p99_us=%.1f max_us=%.1f cpu_ms=%.1f %s\n",
           fw, names[o->role], o->reliable, o->size, o->rate_hz, o->depth, matched, (unsigned long long)start_ns,
           (unsigned long long)end_ns, published, failed, s->warmup, s->scored, s->received, s->duplicates,
           s->below_warmup, s->max_seq, t1_q_us(s, 50), t1_q_us(s, 90), t1_q_us(s, 99),
           s->scored > 0 ? (double)s->lat_ns[s->scored - 1U] / 1000.0 : 0.0, t1_cpu_ms(), extra != NULL ? extra : "");
    fflush(stdout);
}

#endif
