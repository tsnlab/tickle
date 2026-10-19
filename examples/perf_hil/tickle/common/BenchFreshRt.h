/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// KEEP_LAST freshness by ROUND TRIP (the user's rule, 2026-10-19: across hosts only round trips are timed; the hosts'
// clocks drift and Ethernet PTP's error is large, so no one-way delay is read across hosts). Every time difference
// here is taken on ONE clock, the SENDER's CLOCK_MONOTONIC:
//
//   round-trip age = t_echo_rx - t_create
//     t_create  : the creation time BenchFreshness.h stamps (the burst's due time on the sender's schedule), carried in
//                 the stamp's created_mono_ns and handed back verbatim by the reader - never read on the reader's
//                 clock;
//     t_echo_rx : when the sender's side-channel thread received the reader's ECHO for that sample.
//   The reader sends the ECHO the moment its application takes the sample (TickLE: in the subscriber callback; DDS:
//   after dds_take()/take_next_sample() returned it), with a non-blocking sendto() on a plain UDP socket that is NOT
//   the middleware. The code is this header for all three frameworks, so the return path is a common term, not a
//   framework difference.
//
//   THE CONTROL: the sender's thread also sends a PING every --fresh-rt-ping-ms (default 20) over the same socket pair,
//   same 32-byte datagram, and the reader's thread answers with a PONG. side RTT = t_pong_rx - t_ping_tx, again on the
//   sender's clock. Its p50/p99 are printed (fresh_rt_side_*): the side channel's own delay, measured through the run,
//   framework-independent (no middleware on its path). round-trip age - side RTT p50 / 2 is a DIAGNOSTIC estimate of
//   the one-way age and is never scored (freshness_rt_summary.py prints it labelled as such).
//
//   NEWEST SHARE, unchanged definition: the stamp says which seq is the newest of its burst, the echo carries the
//   stamp's flags back, and the sender counts the distinct newest seqs echoed (fresh_rt_newest) against the bursts it
//   scheduled (fresh_bursts, BenchFreshness.h).
//
//   ECHO ACCOUNTING: each ECHO carries the reader's running echo number; the reader prints how many it sent and how
//   many sendto() calls failed (fresh_rt_echo_sent, fresh_rt_echo_fail), the sender how many it received
//   (fresh_rt_echoes). freshness_rt_summary.py voids a run whose echoes were lost beyond its pre-registered threshold.
//
// Opt-in: `--fresh-rt-listen PORT` on the sender, `--fresh-rt HOST:PORT` on the reader (run_scenario.sh gives both
// sides the same arguments, so each side accepts and ignores the other's flag). Either implies --fresh. Without them
// nothing here runs, no socket or thread exists, and no field is printed: the RESULT lines are byte for byte as before.
// Linux only (POSIX sockets and a pthread per side), like every perf_hil bench. The two helper threads block SIGINT and
// SIGTERM, so the benches' own handlers keep receiving them on the main thread.

#pragma once

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>

#include "BenchFreshness.h"

#define BENCH_RT_MAGIC 0x52544652U // "RFTR" little-endian
#define BENCH_RT_KIND_HELLO 1U
#define BENCH_RT_KIND_ECHO 2U
#define BENCH_RT_KIND_PING 3U
#define BENCH_RT_KIND_PONG 4U
#define BENCH_RT_FIELDS_MAX 768
#define BENCH_RT_DEFAULT_PING_MS 20U
// After the sender's last write the side channel stays open until no echo has come for BENCH_RT_LINGER_QUIET_MS, at
// most BENCH_RT_LINGER_MAX_MS: the reader takes the last samples after the last write.
#define BENCH_RT_LINGER_QUIET_MS 300U
#define BENCH_RT_LINGER_MAX_MS 3000U
// The reader's thread wakes at least this often to see the stop flag and, until the first PING, to say HELLO.
#define BENCH_RT_READER_WAKE_MS 200

// One datagram, 32 bytes, every kind. ECHO: seq = the sample's seq, flags = its stamp's flags, number = the reader's
// echo count before this one, t_ns = the stamp's created_mono_ns (the sender's clock, handed back). PING/PONG: seq =
// the ping's number, t_ns = the sender's CLOCK_MONOTONIC when it sent the ping (the PONG returns it unchanged).
struct bench_rt_msg {
    uint32_t magic;
    uint16_t kind;
    uint16_t flags;
    uint32_t seq;
    uint32_t number;
    int64_t t_ns;
    int64_t aux;
};

static inline int64_t bench_rt_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - POSIX <time.h>; glibc: bits/time.h
    return bench_fresh_ts_ns(&ts);
}

// A quantile store, as BenchFreshness.h's: past BENCH_FRESH_MAX values it halves itself and keeps every 2nd, 4th, ...
// value, so what it keeps stays an even sample of the run; min and max are exact.
struct bench_rt_store {
    uint64_t seen;
    uint64_t stride;
    uint32_t kept;
    double min_us;
    double max_us;
    double val_us[BENCH_FRESH_MAX];
};

static inline void bench_rt_store_add(struct bench_rt_store* store, double value_us) {
    if (store->stride == 0) {
        store->stride = 1;
    }
    if (store->seen == 0 || value_us < store->min_us) {
        store->min_us = value_us;
    }
    if (store->seen == 0 || value_us > store->max_us) {
        store->max_us = value_us;
    }
    if (store->seen++ % store->stride != 0) {
        return;
    }
    if (store->kept == BENCH_FRESH_MAX) {
        for (uint32_t i = 0; i < BENCH_FRESH_MAX / 2; i++) {
            store->val_us[i] = store->val_us[(size_t)2 * i];
        }
        store->kept = BENCH_FRESH_MAX / 2;
        store->stride *= 2;
        if ((store->seen - 1) % store->stride != 0) {
            return;
        }
    }
    store->val_us[store->kept++] = value_us;
}

// Nearest rank, as BenchFreshness.h. Sorts in place: call after the run.
static inline double bench_rt_store_quantile(struct bench_rt_store* store, double q) {
    const double round_up = 0.999999;
    qsort(store->val_us, store->kept, sizeof(store->val_us[0]), bench_fresh_compare);
    uint32_t rank = (uint32_t)((q * (double)store->kept) + round_up);
    rank = rank < 1 ? 1 : rank;
    rank = rank > store->kept ? store->kept : rank;
    return store->val_us[rank - 1];
}

// "<name>_kept=N <name>_p50_us=.. <name>_p99_us=.. <name>_min_us=.. <name>_max_us=.." (or =none), with a leading space.
static inline int bench_rt_store_fields(struct bench_rt_store* store, const char* name, char* buf, size_t len) {
    const double p50 = 0.50;
    const double p99 = 0.99;
    if (store->kept == 0) {
        return snprintf(buf, len, " %s_kept=0 %s_p50_us=none %s_p99_us=none %s_min_us=none %s_max_us=none", name, name,
                        name, name, name);
    }
    double q50 = bench_rt_store_quantile(store, p50);
    double q99 = bench_rt_store_quantile(store, p99);
    return snprintf(buf, len, " %s_kept=%u %s_p50_us=%.1f %s_p99_us=%.1f %s_min_us=%.1f %s_max_us=%.1f", name,
                    (unsigned)store->kept, name, q50, name, q99, name, store->min_us, name, store->max_us);
}

static inline void bench_rt_block_signals(void) {
    sigset_t set; // NOLINT(misc-include-cleaner) - POSIX <signal.h>; glibc: bits/types/sigset_t.h
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

static inline bool bench_rt_msg_read(const uint8_t* buf, ssize_t got, struct bench_rt_msg* msg) {
    if (got != (ssize_t)sizeof(*msg)) {
        return false;
    }
    memcpy(msg, buf, sizeof(*msg));
    return msg->magic == BENCH_RT_MAGIC;
}

// ---------------------------------------------------------------------------------------------------- sender ----

struct bench_rt_sender {
    bool on;
    bool started;
    bool bind_failed;
    int port;
    int fd;
    int stop; // read and written with __atomic builtins (C and C++ alike)
    uint32_t ping_ms;
    pthread_t thread; // NOLINT(misc-include-cleaner) - POSIX <pthread.h>; glibc: bits/pthreadtypes.h
    // Written by the side-channel thread only, read by the main thread after it joined.
    bool have_peer;
    struct sockaddr_in peer;
    uint64_t hellos;
    uint64_t echoes;
    uint64_t in_window;
    uint64_t newest;
    uint64_t newest_late;
    uint64_t bad;         // datagrams that were not ours, or came from somewhere else than the peer
    uint64_t echo_no_max; // the highest echo number seen + 1: how many echoes the reader had sent by its last one
    uint32_t last_newest_seq;
    uint64_t pings;
    uint64_t ping_fail;
    uint64_t pongs;
    int64_t last_echo_ns; // the last echo's arrival, read by the main thread during the linger (atomic)
    uint32_t next_ping;
    struct bench_rt_store age;
    struct bench_rt_store side;
};

// --fresh-rt-listen PORT (on), --fresh-rt HOST:PORT (the reader's, ignored here), --fresh-rt-ping-ms N. Returns true if
// argv[*idx] was one of them, its value consumed. --fresh-rt-listen implies --fresh.
static inline bool bench_rt_sender_parse_arg(struct bench_rt_sender* side, struct bench_fresh_sender* fresh, int argc,
                                             char** argv, int* idx) {
    const long port_max = 65535;
    const long ping_ms_max = 10000;
    if (*idx + 1 >= argc) {
        return false;
    }
    if (strcmp(argv[*idx], "--fresh-rt-listen") == 0) {
        long port = strtol(argv[++*idx], NULL, 10);
        side->port = port > 0 && port <= port_max ? (int)port : 0;
        side->on = side->port != 0;
        fresh->on = fresh->on || side->on;
        return true;
    }
    if (strcmp(argv[*idx], "--fresh-rt") == 0) {
        ++*idx;
        return true;
    }
    if (strcmp(argv[*idx], "--fresh-rt-ping-ms") == 0) {
        long ping_ms = strtol(argv[++*idx], NULL, 10);
        side->ping_ms = ping_ms >= 1 && ping_ms <= ping_ms_max ? (uint32_t)ping_ms : side->ping_ms;
        return true;
    }
    return false;
}

static inline void bench_rt_sender_on_msg(struct bench_rt_sender* side, const struct bench_rt_msg* msg,
                                          const struct sockaddr_in* from, int64_t rx_ns) {
    const double ns_per_us = 1e3;
    if (side->have_peer &&
        (from->sin_addr.s_addr != side->peer.sin_addr.s_addr || from->sin_port != side->peer.sin_port)) {
        side->bad++;
        return;
    }
    if (!side->have_peer) {
        side->peer = *from;
        side->have_peer = true;
    }
    switch (msg->kind) {
    case BENCH_RT_KIND_HELLO:
        side->hellos++;
        break;
    case BENCH_RT_KIND_ECHO:
        side->echoes++;
        __atomic_store_n(&side->last_echo_ns, rx_ns, __ATOMIC_RELAXED);
        if ((uint64_t)msg->number + 1 > side->echo_no_max) {
            side->echo_no_max = (uint64_t)msg->number + 1;
        }
        if ((msg->flags & BENCH_FRESH_FLAG_IN_WINDOW) == 0) {
            break;
        }
        side->in_window++;
        if ((msg->flags & BENCH_FRESH_FLAG_NEWEST) != 0) {
            if (msg->seq > side->last_newest_seq) {
                side->newest++;
                side->last_newest_seq = msg->seq;
            } else {
                side->newest_late++;
            }
        }
        bench_rt_store_add(&side->age, (double)(rx_ns - msg->t_ns) / ns_per_us);
        break;
    case BENCH_RT_KIND_PONG:
        side->pongs++;
        bench_rt_store_add(&side->side, (double)(rx_ns - msg->t_ns) / ns_per_us);
        break;
    default:
        side->bad++;
        break;
    }
}

static inline void bench_rt_sender_ping(struct bench_rt_sender* side) {
    struct bench_rt_msg ping;
    memset(&ping, 0, sizeof(ping));
    ping.magic = BENCH_RT_MAGIC;
    ping.kind = BENCH_RT_KIND_PING;
    ping.seq = side->next_ping++;
    ping.t_ns = bench_rt_mono_ns();
    side->pings++;
    if (sendto(side->fd, &ping, sizeof(ping), MSG_DONTWAIT, (const struct sockaddr*)&side->peer, sizeof(side->peer)) !=
        (ssize_t)sizeof(ping)) {
        side->ping_fail++;
    }
}

static inline void* bench_rt_sender_thread(void* arg) {
    struct bench_rt_sender* side = (struct bench_rt_sender*)arg;
    const int64_t ns_per_ms = 1000000;
    bench_rt_block_signals();
    int64_t next_ping_ns = bench_rt_mono_ns() + ((int64_t)side->ping_ms * ns_per_ms);
    while (__atomic_load_n(&side->stop, __ATOMIC_ACQUIRE) == 0) {
        int64_t now = bench_rt_mono_ns();
        if (now >= next_ping_ns) {
            if (side->have_peer) {
                bench_rt_sender_ping(side);
            }
            next_ping_ns += (int64_t)side->ping_ms * ns_per_ms;
            if (next_ping_ns <= now) { // a stall: do not burst pings to catch up
                next_ping_ns = now + ((int64_t)side->ping_ms * ns_per_ms);
            }
            continue;
        }
        struct pollfd pfd;
        pfd.fd = side->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int wait_ms = (int)((next_ping_ns - now + ns_per_ms - 1) / ns_per_ms);
        if (poll(&pfd, 1, wait_ms) <= 0) {
            continue;
        }
        uint8_t buf[2 * sizeof(struct bench_rt_msg)];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        for (;;) { // drain everything queued, each stamped as it is read
            ssize_t got = recvfrom(side->fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&from, &from_len);
            if (got < 0) {
                break;
            }
            int64_t rx_ns = bench_rt_mono_ns();
            struct bench_rt_msg msg;
            if (!bench_rt_msg_read(buf, got, &msg)) {
                side->bad++;
            } else {
                bench_rt_sender_on_msg(side, &msg, &from, rx_ns);
            }
            from_len = sizeof(from);
        }
    }
    return NULL;
}

// Binds 0.0.0.0:PORT and starts the side-channel thread. Call after the arguments are parsed, before the first write.
static inline void bench_rt_sender_start(struct bench_rt_sender* side) {
    if (!side->on) {
        return;
    }
    if (side->ping_ms == 0) {
        side->ping_ms = BENCH_RT_DEFAULT_PING_MS;
    }
    side->fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)side->port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (side->fd < 0 || bind(side->fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        pthread_create(&side->thread, NULL, bench_rt_sender_thread, side) != 0) {
        (void)fprintf(stderr, "fresh-rt: cannot listen on UDP port %d: %s\n", side->port, strerror(errno));
        side->bind_failed = true;
        return;
    }
    side->started = true;
}

// After the last write: keep the side channel open until no echo has come for BENCH_RT_LINGER_QUIET_MS (at most
// BENCH_RT_LINGER_MAX_MS), then stop the thread. Call before printing the RESULT line.
static inline void bench_rt_sender_finish(struct bench_rt_sender* side) {
    const int64_t ns_per_ms = 1000000;
    const long nap_ns = 10 * 1000000L;
    struct timespec nap;
    nap.tv_sec = 0;
    nap.tv_nsec = nap_ns;
    if (!side->started) {
        return;
    }
    int64_t begin = bench_rt_mono_ns();
    for (;;) {
        int64_t now = bench_rt_mono_ns();
        int64_t last = __atomic_load_n(&side->last_echo_ns, __ATOMIC_RELAXED);
        int64_t quiet_since = last > begin ? last : begin;
        if (now - quiet_since >= (int64_t)BENCH_RT_LINGER_QUIET_MS * ns_per_ms ||
            now - begin >= (int64_t)BENCH_RT_LINGER_MAX_MS * ns_per_ms) {
            break;
        }
        nanosleep(&nap, NULL);
    }
    __atomic_store_n(&side->stop, 1, __ATOMIC_RELEASE);
    pthread_join(side->thread, NULL);
    close(side->fd);
    side->started = false;
}

// The sender's round-trip fields, with a leading space; "" without --fresh-rt-listen (print as "...%s%s\n" after the
// fresh fields). Sorts the stores: after bench_rt_sender_finish().
static inline const char* bench_rt_sender_fields(struct bench_rt_sender* side, char* buf, size_t len) {
    buf[0] = '\0';
    if (!side->on) {
        return buf;
    }
    if (side->bind_failed) {
        (void)snprintf(buf, len, " fresh_rt=bindfail fresh_rt_port=%d", side->port);
        return buf;
    }
    int used = snprintf(
        buf, len,
        " fresh_rt=on fresh_rt_port=%d fresh_rt_ping_ms=%u fresh_rt_peer=%s fresh_rt_hellos=%llu "
        "fresh_rt_echoes=%llu fresh_rt_echo_no_max=%llu fresh_rt_in_window=%llu fresh_rt_newest=%llu "
        "fresh_rt_newest_late=%llu fresh_rt_bad=%llu",
        side->port, (unsigned)side->ping_ms, side->have_peer ? "yes" : "no", (unsigned long long)side->hellos,
        (unsigned long long)side->echoes, (unsigned long long)side->echo_no_max, (unsigned long long)side->in_window,
        (unsigned long long)side->newest, (unsigned long long)side->newest_late, (unsigned long long)side->bad);
    if (used < 0 || (size_t)used >= len) {
        return buf;
    }
    int more = bench_rt_store_fields(&side->age, "fresh_rt_age", buf + used, len - (size_t)used);
    used += more > 0 ? more : 0;
    if ((size_t)used >= len) {
        return buf;
    }
    more =
        snprintf(buf + used, len - (size_t)used, " fresh_rt_pings=%llu fresh_rt_ping_fail=%llu fresh_rt_pongs=%llu",
                 (unsigned long long)side->pings, (unsigned long long)side->ping_fail, (unsigned long long)side->pongs);
    used += more > 0 ? more : 0;
    if ((size_t)used >= len) {
        return buf;
    }
    (void)bench_rt_store_fields(&side->side, "fresh_rt_side", buf + used, len - (size_t)used);
    return buf;
}

// -------------------------------------------------------------------------------------------------- reader ----

struct bench_rt_reader {
    bool on;
    bool started;
    bool setup_failed;
    int fd;
    int stop;
    pthread_t thread; // NOLINT(misc-include-cleaner) - POSIX <pthread.h>; glibc: bits/pthreadtypes.h
    struct sockaddr_in to;
    char target[64];
    // The application thread's (the take path's) counters.
    uint32_t echo_sent;
    uint64_t echo_fail;
    uint64_t echo_skipped; // samples without a stamp: nothing to hand back
    // The helper thread's counters.
    uint64_t hellos;
    uint64_t pings;
    uint64_t pongs;
    uint64_t pong_fail;
};

// --fresh-rt HOST:PORT (on), --fresh-rt-listen PORT and --fresh-rt-ping-ms N (the sender's, ignored here). Returns true
// if argv[*idx] was one of them, its value consumed. --fresh-rt implies --fresh.
static inline bool bench_rt_reader_parse_arg(struct bench_rt_reader* side, struct bench_fresh_receiver* fresh, int argc,
                                             char** argv, int* idx) {
    const long port_max = 65535;
    if (*idx + 1 >= argc) {
        return false;
    }
    if (strcmp(argv[*idx], "--fresh-rt-listen") == 0 || strcmp(argv[*idx], "--fresh-rt-ping-ms") == 0) {
        ++*idx;
        return true;
    }
    if (strcmp(argv[*idx], "--fresh-rt") != 0) {
        return false;
    }
    const char* value = argv[++*idx];
    (void)snprintf(side->target, sizeof(side->target), "%s", value);
    char* colon = strrchr(side->target, ':');
    memset(&side->to, 0, sizeof(side->to));
    side->to.sin_family = AF_INET;
    long port = colon != NULL ? strtol(colon + 1, NULL, 10) : 0;
    if (colon != NULL) {
        *colon = '\0';
    }
    side->setup_failed =
        colon == NULL || port <= 0 || port > port_max || inet_pton(AF_INET, side->target, &side->to.sin_addr) != 1;
    side->to.sin_port = htons((uint16_t)(port > 0 && port <= port_max ? port : 0));
    if (colon != NULL) {
        *colon = ':';
    }
    side->on = true;
    fresh->on = true;
    return true;
}

static inline bool bench_rt_reader_from_sender(const struct bench_rt_reader* side, const struct sockaddr_in* from) {
    return from->sin_addr.s_addr == side->to.sin_addr.s_addr && from->sin_port == side->to.sin_port;
}

static inline void bench_rt_reader_hello(struct bench_rt_reader* side) {
    struct bench_rt_msg hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = BENCH_RT_MAGIC;
    hello.kind = BENCH_RT_KIND_HELLO;
    hello.t_ns = bench_rt_mono_ns();
    side->hellos++;
    (void)sendto(side->fd, &hello, sizeof(hello), MSG_DONTWAIT, (const struct sockaddr*)&side->to, sizeof(side->to));
}

static inline void* bench_rt_reader_thread(void* arg) {
    struct bench_rt_reader* side = (struct bench_rt_reader*)arg;
    bench_rt_block_signals();
    bench_rt_reader_hello(side);
    while (__atomic_load_n(&side->stop, __ATOMIC_ACQUIRE) == 0) {
        struct pollfd pfd;
        pfd.fd = side->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, BENCH_RT_READER_WAKE_MS) <= 0) {
            if (side->pings == 0) { // the sender may have started after us: say where we are until it pings
                bench_rt_reader_hello(side);
            }
            continue;
        }
        uint8_t buf[2 * sizeof(struct bench_rt_msg)];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        for (;;) {
            ssize_t got = recvfrom(side->fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&from, &from_len);
            if (got < 0) {
                break;
            }
            from_len = sizeof(from);
            struct bench_rt_msg msg;
            if (!bench_rt_msg_read(buf, got, &msg) || msg.kind != BENCH_RT_KIND_PING ||
                !bench_rt_reader_from_sender(side, &from)) {
                continue;
            }
            side->pings++;
            msg.kind = BENCH_RT_KIND_PONG;
            if (sendto(side->fd, &msg, sizeof(msg), MSG_DONTWAIT, (const struct sockaddr*)&side->to,
                       sizeof(side->to)) == (ssize_t)sizeof(msg)) {
                side->pongs++;
            } else {
                side->pong_fail++;
            }
        }
    }
    return NULL;
}

// Opens the side socket (unconnected, so an ICMP "port unreachable" from a sender not yet listening never turns into
// an error on a later echo) and starts the PONG thread. Call after the arguments are parsed, before the first take.
static inline void bench_rt_reader_start(struct bench_rt_reader* side) {
    if (!side->on || side->setup_failed) {
        return;
    }
    side->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (side->fd < 0 || pthread_create(&side->thread, NULL, bench_rt_reader_thread, side) != 0) {
        (void)fprintf(stderr, "fresh-rt: cannot open the side socket to %s: %s\n", side->target, strerror(errno));
        side->setup_failed = true;
        return;
    }
    side->started = true;
}

// The take path: one ECHO for the sample the application has just taken, non-blocking. payload is the sample's
// payload, whose first bytes are BenchFreshness.h's stamp.
static inline void bench_rt_echo(struct bench_rt_reader* side, uint32_t seq, const uint8_t* payload, size_t len) {
    if (!side->started) {
        return;
    }
    struct bench_fresh_stamp stamp;
    if (len < sizeof(stamp)) {
        side->echo_skipped++;
        return;
    }
    memcpy(&stamp, payload, sizeof(stamp));
    if (stamp.magic != BENCH_FRESH_MAGIC) {
        side->echo_skipped++;
        return;
    }
    struct bench_rt_msg echo;
    echo.magic = BENCH_RT_MAGIC;
    echo.kind = BENCH_RT_KIND_ECHO;
    echo.flags = stamp.flags;
    echo.seq = seq;
    echo.number = side->echo_sent;
    echo.t_ns = stamp.created_mono_ns;
    echo.aux = 0;
    if (sendto(side->fd, &echo, sizeof(echo), MSG_DONTWAIT, (const struct sockaddr*)&side->to, sizeof(side->to)) ==
        (ssize_t)sizeof(echo)) {
        side->echo_sent++;
    } else {
        side->echo_fail++;
    }
}

static inline void bench_rt_reader_finish(struct bench_rt_reader* side) {
    if (!side->started) {
        return;
    }
    __atomic_store_n(&side->stop, 1, __ATOMIC_RELEASE);
    pthread_join(side->thread, NULL);
    close(side->fd);
    side->started = false;
}

// The reader's fields, with a leading space; "" without --fresh-rt. After bench_rt_reader_finish().
static inline const char* bench_rt_reader_fields(const struct bench_rt_reader* side, char* buf, size_t len) {
    buf[0] = '\0';
    if (!side->on) {
        return buf;
    }
    (void)snprintf(buf, len,
                   " fresh_rt=%s fresh_rt_target=%s fresh_rt_echo_sent=%u fresh_rt_echo_fail=%llu "
                   "fresh_rt_echo_skipped=%llu fresh_rt_hellos=%llu fresh_rt_pings=%llu fresh_rt_pongs=%llu "
                   "fresh_rt_pong_fail=%llu",
                   side->setup_failed ? "setupfail" : "on", side->target, (unsigned)side->echo_sent,
                   (unsigned long long)side->echo_fail, (unsigned long long)side->echo_skipped,
                   (unsigned long long)side->hellos, (unsigned long long)side->pings, (unsigned long long)side->pongs,
                   (unsigned long long)side->pong_fail);
    return buf;
}
