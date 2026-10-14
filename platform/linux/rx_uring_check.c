// Copyright (c) 2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// tt_receive()'s wait on io_uring (hal_linux.c "One wait that returns data") against real sockets on loopback, with
// its control: every case runs twice in one process, on a context made with TT_RX_URING_WAIT=0 (the epoll path, the
// A/B control) and on one made without it (the ring), from the same inputs - and each case's outcome, recorded as a
// line of text, must be the same in both arms. Beside that, each arm must report its own treatment: the ring arm's
// waits entered the kernel on the ring and its datagrams came from recvmsg completions, the control's did not.
// The wait is on the ring only while large samples arrive: each arm first receives a datagram shaped as a large
// sample's run (single-form FRAG_RUN_L), which puts the ring arm's wait on the ring; before it, a wait uses epoll.
//   0. on       - before that datagram, a datagram is received with no wait on the ring; after it, the ring is on.
//   1. wait     - a datagram sent while the context sleeps in tt_receive() wakes it, whole, from the right sender; on
//   the
//                 ring it is read where the caller asked (no copy), by one wait, with no epoll_pwait2() for it.
//   2. order    - five datagrams queued before the wait: the wait and the drain after it hand them out in order.
//   3. timeout  - a wait with nothing sent times out no sooner than asked; on the ring its recvmsg stays armed, and the
//                 drain then asks nothing of that socket (no cancellation, no read); a datagram sent next is handed
//                 out by the drain from the recvmsg's completion.
//   4. wake     - tt_wake_signal() from another thread ends a wait (-3) through the epoll set, and the datagram sent
//                 after it is not lost.
//   5. moved    - a recvmsg armed into one buffer (a wait that timed out) and the next receive asked with another (a
//                 landing that moved): nothing is written to the first buffer after that call, and the next datagram
//                 arrives in the second, whole.
//   6. yield    - a datagram on the well-known socket handed out into the buffer the data socket's recvmsg was armed
//                 into: a datagram sent to the data socket next does not overwrite it, and is handed out after.
//   7. drain    - a drain started after a wait left its recvmsgs armed (a large-sample build): the datagrams sent
//                 next are handed out in the order they were sent.
//   6b. both    - a datagram on each socket taken by the ring while the context is away (both held, the data socket's
//                 in the caller's buffer): both handed out whole, whichever goes first - twice, so each goes first
//                 once.
//   8. hint     - tt_rx_maybe_ready() after a wait left both recvmsgs armed: on the ring "nothing arrived" at
//                 once, with no poll armed (a recvmsg covers its socket as a poll does); "may be" once a datagram
//                 came, which the drain then returns.
//  10. merged   - receive offload on (as a large sample's MTU fragment turns it on) and two datagrams sent as one
//                 UDP_SEGMENT send, which the UDP_GRO socket takes as one merged read: both handed out whole, in order;
//                 on the ring, by the wait's recvmsg (its segment-size cmsg), with no readiness fallback.
//   9. quiet    - 128 small datagrams in a row put the wait back on epoll (the next wait is not on the ring, and
//                 receives), and the next large-sample datagram puts it on the ring again.
// With the argument "refused" (run under tests/support/refuse_io_uring.so): no ring, uring_wait=-1, and case 1 still
// receives its datagram, by epoll.
// Exit 0 pass, 1 fail, 3 setup failed. Run by test.sh (test-linux) in a private namespace.

// Built as rmw_tickle builds core (platform/linux/Makefile passes the same), so a lint of this file alone sees the same
// build: the wait on the ring is compiled only where large samples are.
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507
#endif
#ifndef tt_LARGE_SAMPLES
#define tt_LARGE_SAMPLES 1
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> // setenv(), unsetenv()
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/udp.h> // UDP_GRO, UDP_SEGMENT: case 10
#include <sys/socket.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h>
#include <tickle/tickle.h>

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_SETUP_FAILED = 3 };

#define CHECK_CONTEXT_ID 63 // any valid id; this context never meets another
#define MAGIC0 'U'          // this check's datagrams start "UR" then their number: none of TickLE's own looks like it
#define MAGIC1 'R'
#define LEN_BASE 100U   // datagram k is LEN_BASE + k bytes
#define PATTERN_STEP 7U // its bytes after the header
#define SETTLE_US 5000  // a datagram sent is queued, and a recvmsg's task work run, within this
#define LATE_SEND_US 20000
#define WAIT_LONG_NS 2000000000LL // a wait that should end for what was sent, not for this
#define WAIT_SHORT_NS 50000000LL  // a wait that should time out: 50 ms
#define WAIT_SLACK_NS 2000000LL   // the clock's and the timer's slack against it
#define WAIT_BRIEF_NS 20000000LL  // a wait that leaves the recvmsgs armed: 20 ms
#define ORDER_COUNT 5U
#define MAX_TRIES 16
#define LINE_BYTES 256U
#define TRANSCRIPT_LINES 32U
#define UNTOUCHED 0xEE
#define NS_PER_S 1000000000LL
#define DRAIN_DATAGRAM_BYTES 65507U
#define LARGE_SHAPED_BYTES 200U // a datagram that reads as a large sample's run to the HAL (bytes 0 and 3)
#define QUIET_DATAGRAMS 128U    // hal_linux.c WAIT_QUIET_DATAGRAMS
#define DRAINED_COUNT 3U        // case 7's datagrams
#define HEADER_BYTES 3U         // "UR" and the number
#define DATAGRAM_ROOM 64U       // the largest number a datagram is given, and then some

// Each case's datagrams, by number.
enum datagram_number {
    DG_WAIT = 1,
    DG_TIMEOUT = 2,
    DG_WAKE = 3,
    DG_MOVED = 4,
    DG_YIELD_WELL_KNOWN = 5,
    DG_YIELD_DATA = 6,
    DG_DRAIN_FIRST = 7,
    DG_BOTH_DATA = 11,
    DG_BOTH_WELL_KNOWN = 12,
    DG_HINT = 10,
    DG_ON = 13,
    DG_QUIET = 14,
    DG_AFTER_QUIET = 15,
    DG_MERGED = 16, // and 17, the second datagram of the merged read
};

struct arm {
    const char* name;
    bool ring;
    char lines[TRANSCRIPT_LINES][LINE_BYTES]; // each case's outcome, compared between the arms
    unsigned count;
};

static struct tt_Context node;
static int sender = -1;
static uint8_t datagram[LEN_BASE + DATAGRAM_ROOM];
static uint8_t buf[tt_MAX_BUFFER_LENGTH];
static uint8_t other[tt_MAX_BUFFER_LENGTH];
static int failures;
static bool setup_failed;

static void verdict(const struct arm* arm, bool passed, const char* line) {
    printf("rx_uring_check: %s - [%s] %s\n", passed ? "PASS" : "FAIL", arm->name, line);
    failures += passed ? 0 : 1;
}

static void record(struct arm* arm, const char* line) {
    if (arm->count < TRANSCRIPT_LINES) {
        (void)snprintf(arm->lines[arm->count++], LINE_BYTES, "%s", line);
    }
}

static int64_t now_ns(void) {
    struct timespec now;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_MONOTONIC: <time.h>
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((int64_t)now.tv_sec * NS_PER_S) + now.tv_nsec;
}

// Datagram number k: "UR", k, then a pattern; LEN_BASE + k bytes.
static uint32_t build(uint8_t k) {
    const uint32_t len = LEN_BASE + k;
    datagram[0] = MAGIC0;
    datagram[1] = MAGIC1;
    datagram[2] = k;
    for (uint32_t i = HEADER_BYTES; i < len; i++) {
        datagram[i] = (uint8_t)((i * PATTERN_STEP) + k);
    }
    return len;
}

// The number of the datagram `got` bytes long in `where`, or -1 when it is not one of this check's, whole.
static int which(const uint8_t* where, int32_t got) {
    if (got < (int32_t)LEN_BASE || where[0] != MAGIC0 || where[1] != MAGIC1) {
        return -1;
    }
    const uint8_t k = where[2];
    if ((uint32_t)got != LEN_BASE + k) {
        return -1;
    }
    for (uint32_t i = HEADER_BYTES; i < (uint32_t)got; i++) {
        if (where[i] != (uint8_t)((i * PATTERN_STEP) + k)) {
            return -1;
        }
    }
    return k;
}

static bool send_k(uint8_t k, bool well_known) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof(dest);
    if (getsockname(well_known ? node.hal.sock : node.hal.data_sock, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const uint32_t len = build(k);
    return sendto(sender, datagram, len, 0, (struct sockaddr*)&dest, sizeof(dest)) == (ssize_t)len;
}

static void send_or_fail(uint8_t k, bool well_known) {
    if (!send_k(k, well_known)) {
        printf("rx_uring_check: SETUP FAILED - cannot send datagram %u\n", (unsigned)k);
        setup_failed = true;
    }
}

// Receives with tt_receive() into `into` until one of this check's datagrams comes (the context's own announces may be
// read first): its number, or -1 (nothing), -3 (interrupted), -4 (an error).
static int receive_k(uint8_t* into, int64_t timeout) {
    uint32_t ip = 0;
    uint16_t port = 0;
    for (int i = 0; i < MAX_TRIES; i++) {
        int32_t got = tt_receive(&node, into, tt_MAX_BUFFER_LENGTH, &ip, &port, timeout);
        if (got == -1 || got == -3) {
            return got;
        }
        if (got < -1) {
            return -4;
        }
        int k = which(into, got);
        if (k >= 0) {
            return k;
        }
    }
    return -1;
}

// tt_try_receive() into `into` until one of this check's datagrams or nothing.
static int try_k(uint8_t* into) {
    uint32_t ip = 0;
    uint16_t port = 0;
    for (int i = 0; i < MAX_TRIES; i++) {
        int32_t got = tt_try_receive(&node, into, tt_MAX_BUFFER_LENGTH, &ip, &port);
        if (got < 0) {
            return (int)got;
        }
        int k = which(into, got);
        if (k >= 0) {
            return k;
        }
    }
    return -1;
}

// Whatever is waiting, read and dropped: each case starts from empty sockets.
static void drain_all(void) {
    usleep(SETTLE_US);
    uint32_t ip = 0;
    uint16_t port = 0;
    while (tt_try_receive(&node, buf, sizeof(buf), &ip, &port) >= 0) {
    }
}

static void* send_late(void* arg) {
    usleep(LATE_SEND_US);
    (void)send_k(*(const uint8_t*)arg, false);
    return NULL;
}

static void* wake_late(void* arg) {
    (void)arg;
    usleep(LATE_SEND_US);
    (void)tt_wake_signal(&node);
    return NULL;
}

static void check_wait(struct arm* arm) {
    drain_all();
    const uint64_t enters = node.hal.wait_enters;
    const uint64_t recvs = node.hal.wait_recvs;
    const uint64_t copies = node.hal.wait_copies;
    const uint64_t epolls = node.hal.wait_epolls;
    static uint8_t late = DG_WAIT;
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    if (pthread_create(&thread, NULL, send_late, &late) != 0) {
        setup_failed = true;
        return;
    }
    memset(buf, 0, sizeof(buf));
    int k = receive_k(buf, WAIT_LONG_NS);
    (void)pthread_join(thread, NULL);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "wait: received %d", k);
    record(arm, line);
    verdict(arm, k == DG_WAIT, line);
    if (arm->ring) {
        const bool engaged = node.hal.wait_enters > enters && node.hal.wait_recvs == recvs + 1U &&
                             node.hal.wait_copies == copies && node.hal.wait_epolls == epolls;
        (void)snprintf(
            line, sizeof(line),
            "wait: on the ring - waits +%llu, recvs +%llu, copies +%llu, epoll_pwait2 +%llu (want >0, 1, 0, 0)",
            (unsigned long long)(node.hal.wait_enters - enters), (unsigned long long)(node.hal.wait_recvs - recvs),
            (unsigned long long)(node.hal.wait_copies - copies), (unsigned long long)(node.hal.wait_epolls - epolls));
        verdict(arm, engaged, line);
    }
}

static void check_order(struct arm* arm) {
    drain_all();
    for (uint8_t k = 1; k <= ORDER_COUNT; k++) {
        send_or_fail(k, false);
    }
    usleep(SETTLE_US);
    char line[LINE_BYTES];
    int used = snprintf(line, sizeof(line), "order:");
    int k = receive_k(buf, WAIT_LONG_NS);
    for (unsigned taken = 0; k >= 0 && taken <= ORDER_COUNT && used > 0 && (size_t)used < sizeof(line); taken++) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, " %d", k);
        k = try_k(buf);
    }
    record(arm, line);
    verdict(arm, strcmp(line, "order: 1 2 3 4 5") == 0, line);
}

static void check_timeout(struct arm* arm) {
    drain_all();
    const int64_t start = now_ns();
    int k = receive_k(buf, WAIT_SHORT_NS);
    const int64_t took = now_ns() - start;
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "timeout: %d, not early %d", k, took >= WAIT_SHORT_NS - WAIT_SLACK_NS ? 1 : 0);
    record(arm, line);
    verdict(arm, k == -1 && took >= WAIT_SHORT_NS - WAIT_SLACK_NS, line);
    if (arm->ring) {
        const bool armed = (node.hal.wait_recv_armed & 2U) != 0;
        const uint64_t cancels = node.hal.wait_cancels;
        int none = try_k(buf);
        none = none == -1 ? try_k(buf) : none;
        (void)snprintf(line, sizeof(line),
                       "timeout: the data socket's recvmsg left armed %d; the drain asked nothing of it: %d, "
                       "cancellations +%llu",
                       armed ? 1 : 0, none, (unsigned long long)(node.hal.wait_cancels - cancels));
        verdict(arm, armed && none == -1 && node.hal.wait_cancels == cancels, line);
    }
    const uint64_t recvs = node.hal.wait_recvs;
    send_or_fail(DG_TIMEOUT, false);
    usleep(SETTLE_US);
    k = try_k(buf);
    (void)snprintf(line, sizeof(line), "timeout: the next datagram, by the drain: %d", k);
    record(arm, line);
    verdict(arm, k == DG_TIMEOUT, line);
    if (arm->ring) {
        (void)snprintf(line, sizeof(line), "timeout: it came from the recvmsg's completion: recvs +%llu",
                       (unsigned long long)(node.hal.wait_recvs - recvs));
        verdict(arm, node.hal.wait_recvs == recvs + 1U, line);
    }
}

static void check_wake(struct arm* arm) {
    drain_all();
    const uint64_t epolls = node.hal.wait_epolls;
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    if (pthread_create(&thread, NULL, wake_late, NULL) != 0) {
        setup_failed = true;
        return;
    }
    int k = receive_k(buf, WAIT_LONG_NS);
    (void)pthread_join(thread, NULL);
    send_or_fail(DG_WAKE, false);
    int after = receive_k(buf, WAIT_LONG_NS);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "wake: %d, then %d", k, after);
    record(arm, line);
    verdict(arm, k == -3 && after == DG_WAKE, line);
    if (arm->ring) {
        (void)snprintf(line, sizeof(line), "wake: named by epoll_pwait2() after the epoll fd's poll: +%llu",
                       (unsigned long long)(node.hal.wait_epolls - epolls));
        verdict(arm, node.hal.wait_epolls > epolls, line);
    }
}

static bool untouched(const uint8_t* where) {
    for (size_t i = 0; i < sizeof(other); i++) {
        if (where[i] != UNTOUCHED) {
            return false;
        }
    }
    return true;
}

static void check_moved(struct arm* arm) {
    drain_all();
    int k = receive_k(other, WAIT_BRIEF_NS); // times out: on the ring, the data socket's recvmsg is armed into `other`
    memset(other, UNTOUCHED, sizeof(other));
    int none = try_k(buf); // the landing moved: another buffer asked
    send_or_fail(DG_MOVED, false);
    usleep(SETTLE_US);
    const bool kept = untouched(other);
    int after = receive_k(buf, WAIT_LONG_NS);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "moved: %d, %d; the first buffer untouched %d; the next datagram %d", k, none,
                   kept ? 1 : 0, after);
    record(arm, line);
    verdict(arm, k == -1 && none == -1 && kept && after == DG_MOVED, line);
}

static void check_yield(struct arm* arm) {
    drain_all();
    int k = receive_k(buf, WAIT_BRIEF_NS); // times out: the data socket's recvmsg is armed into `buf`
    send_or_fail(DG_YIELD_WELL_KNOWN, true);
    usleep(SETTLE_US);
    int well_known = receive_k(buf, WAIT_LONG_NS);
    send_or_fail(DG_YIELD_DATA, false); // while core would still be processing the well-known one in `buf`
    usleep(SETTLE_US);
    const bool kept = which(buf, (int32_t)(LEN_BASE + DG_YIELD_WELL_KNOWN)) == DG_YIELD_WELL_KNOWN;
    int data = receive_k(buf, WAIT_LONG_NS);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "yield: %d, well-known %d kept %d, then data %d", k, well_known, kept ? 1 : 0,
                   data);
    record(arm, line);
    verdict(arm, k == -1 && well_known == DG_YIELD_WELL_KNOWN && kept && data == DG_YIELD_DATA, line);
}

// One round of 6b: the numbers received, lowest first ("11 12" when both came whole).
static void both_round(char* out, size_t size, bool data_first) {
    int k = receive_k(buf, WAIT_BRIEF_NS); // times out: both recvmsgs armed (on the ring)
    send_or_fail(DG_BOTH_DATA, false);
    send_or_fail(DG_BOTH_WELL_KNOWN, true);
    usleep(SETTLE_US);                    // both taken by the ring meanwhile
    node.hal.rx_prefer_data = data_first; // which of two ready sockets the receive hands out first
    int first = receive_k(buf, WAIT_LONG_NS);
    int second = try_k(buf);
    if (second < 0) {
        second = receive_k(buf, WAIT_BRIEF_NS);
    }
    (void)snprintf(out, size, "%d: %d %d", k, first < second ? first : second, first < second ? second : first);
}

static void check_both(struct arm* arm) {
    drain_all();
    char round1[LINE_BYTES / 4];
    char round2[LINE_BYTES / 4];
    both_round(round1, sizeof(round1), false);
    both_round(round2, sizeof(round2), true);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "both: %s; %s", round1, round2);
    record(arm, line);
    verdict(arm, strcmp(line, "both: -1: 11 12; -1: 11 12") == 0, line);
}

static void check_drain(struct arm* arm) {
#if tt_RX_DRAIN
    drain_all();
    int k = receive_k(buf, WAIT_BRIEF_NS); // times out with the recvmsgs armed
    const uint64_t taken = node.hal.drain_datagrams;
    const bool started = tt_rx_drain_start(&node, DRAIN_DATAGRAM_BYTES);
    for (uint32_t number = DG_DRAIN_FIRST; number < DG_DRAIN_FIRST + DRAINED_COUNT; number++) {
        send_or_fail((uint8_t)number, false);
    }
    usleep(SETTLE_US);
    char line[LINE_BYTES];
    int used = snprintf(line, sizeof(line), "drain: %d started %d, then", k, started ? 1 : 0);
    int got = receive_k(buf, WAIT_LONG_NS);
    for (unsigned taken = 0; got >= 0 && taken <= DRAINED_COUNT && used > 0 && (size_t)used < sizeof(line); taken++) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, " %d", got);
        got = try_k(buf);
    }
    record(arm, line);
    const bool drained = node.hal.drain_datagrams >= taken + DRAINED_COUNT;
    verdict(arm, strstr(line, "started 1, then 7 8 9") != NULL && drained, line);
#else
    (void)arm;
#endif
}

static void check_hint(struct arm* arm) {
    drain_all();
    int k = receive_k(buf, WAIT_BRIEF_NS); // times out with both recvmsgs armed (on the ring)
    char line[LINE_BYTES];
    if (arm->ring) {
        const uint64_t arms = node.hal.uring_arms;
        const bool quiet = !tt_rx_maybe_ready(&node);
        (void)snprintf(line, sizeof(line), "hint: nothing arrived, said at once %d, polls armed +%llu", quiet ? 1 : 0,
                       (unsigned long long)(node.hal.uring_arms - arms));
        verdict(arm, quiet && node.hal.uring_arms == arms, line);
    }
    send_or_fail(DG_HINT, false);
    usleep(SETTLE_US);
    const bool may_be = tt_rx_maybe_ready(&node);
    int got = try_k(buf);
    (void)snprintf(line, sizeof(line), "hint: %d; after a datagram 'may be' %d, then %d", k, may_be ? 1 : 0, got);
    record(arm, line);
    verdict(arm, k == -1 && may_be && got == DG_HINT, line);
}

// A datagram the HAL takes for a large sample's run: single-form framing, FRAG_RUN_L (what wait_note() looks at).
static bool send_large_shaped(void) {
    uint8_t shaped[LARGE_SHAPED_BYTES];
    memset(shaped, 0, sizeof(shaped));
    shaped[0] = (uint8_t)tt_SINGLE_MARKER_LE;
    shaped[1] = tt_VERSION;
    shaped[3] = tt_SUBMESSAGE_TYPE_FRAG_RUN_L;
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof(dest);
    if (getsockname(node.hal.data_sock, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sendto(sender, shaped, sizeof(shaped), 0, (struct sockaddr*)&dest, sizeof(dest)) == (ssize_t)sizeof(shaped);
}

// Receives the large-shaped datagram: true when it came.
static bool receive_large_shaped(void) {
    uint32_t ip = 0;
    uint16_t port = 0;
    for (int i = 0; i < MAX_TRIES; i++) {
        int32_t got = tt_receive(&node, buf, sizeof(buf), &ip, &port, WAIT_LONG_NS);
        if (got == (int32_t)LARGE_SHAPED_BYTES && buf[3] == tt_SUBMESSAGE_TYPE_FRAG_RUN_L) {
            return true;
        }
        if (got < 0) {
            return false;
        }
    }
    return false;
}

static void check_on(struct arm* arm) {
    drain_all();
    const uint64_t enters = node.hal.wait_enters;
    send_or_fail(DG_ON, false);
    int k = receive_k(buf, WAIT_LONG_NS);
    const bool epoll_first = node.hal.wait_enters == enters && !node.hal.wait_on;
    if (!send_large_shaped()) {
        setup_failed = true;
        return;
    }
    const bool large = receive_large_shaped();
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "on: %d, then a large sample's datagram %d", k, large ? 1 : 0);
    record(arm, line);
    verdict(arm, k == DG_ON && large, line);
    (void)snprintf(line, sizeof(line), "on: before it no wait on the ring %d; after it the ring on %d (want 1, %d)",
                   epoll_first ? 1 : 0, node.hal.wait_on ? 1 : 0, arm->ring ? 1 : 0);
    verdict(arm, epoll_first && node.hal.wait_on == arm->ring, line);
}

// Receive offload on, as the HAL turns it on for a large sample's MTU fragment (gro_turn_on()), or off.
static bool offload(bool enable) {
    int value = enable ? 1 : 0;
    bool set = setsockopt(node.hal.sock, SOL_UDP, UDP_GRO, &value, sizeof(value)) == 0 &&
               setsockopt(node.hal.data_sock, SOL_UDP, UDP_GRO, &value, sizeof(value)) == 0;
    node.hal.gro_state = (uint8_t)(enable && set ? TT_GRO_ON : TT_GRO_PLAIN);
    node.hal.gro_quiet = 0;
    return set;
}

// The two datagrams of case 10, one length (a merged read's segments are), side by side: DG_MERGED, then the same bytes
// numbered DG_MERGED + 1.
static uint8_t merged_pair[2U * (LEN_BASE + DATAGRAM_ROOM)];

static uint32_t build_merged_pair(void) {
    const uint32_t len = build(DG_MERGED);
    memcpy(merged_pair, datagram, len);
    memcpy(merged_pair + len, datagram, len);
    merged_pair[len + 2U] = DG_MERGED + 1U;
    return len;
}

// Sent as one UDP_SEGMENT send to the data socket, which a UDP_GRO socket takes as one merged read.
static bool send_merged(uint32_t len) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof(dest);
    if (getsockname(node.hal.data_sock, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int gso = socket(AF_INET, SOCK_DGRAM, 0);
    int size = (int)len;
    const size_t both = (size_t)2U * len;
    bool sent = gso >= 0 && setsockopt(gso, SOL_UDP, UDP_SEGMENT, &size, sizeof(size)) == 0 &&
                sendto(gso, merged_pair, both, 0, (struct sockaddr*)&dest, sizeof(dest)) == (ssize_t)both;
    if (gso >= 0) {
        (void)close(gso);
    }
    return sent;
}

// Whether a receive handed out datagram `index` (0 or 1) of the pair whole: `got` bytes at buf + rx_offset.
static bool merged_whole(int32_t got, uint32_t len, uint32_t index) {
    return got == (int32_t)len && memcmp(buf + node.rx_offset, merged_pair + ((size_t)index * len), len) == 0;
}

static void check_merged(struct arm* arm) {
    drain_all();
    if (!offload(true)) {
        printf("rx_uring_check: SKIP - merged: the kernel refuses UDP_GRO\n");
        (void)offload(false);
        return;
    }
    int k = receive_k(buf, WAIT_BRIEF_NS); // times out: the recvmsgs armed with receive offload on
    const uint64_t fallbacks = node.hal.wait_fallbacks;
    const uint64_t recvs = node.hal.wait_recvs;
    const uint64_t merged = node.udp_gro_reads;
    const uint32_t len = build_merged_pair();
    if (!send_merged(len)) {
        printf("rx_uring_check: SKIP - merged: no UDP_SEGMENT send on this kernel\n");
        (void)offload(false);
        return;
    }
    usleep(SETTLE_US);
    uint32_t ip = 0;
    uint16_t port = 0;
    const bool first = merged_whole(tt_receive(&node, buf, sizeof(buf), &ip, &port, WAIT_LONG_NS), len, 0);
    const bool second = merged_whole(tt_try_receive(&node, buf, sizeof(buf), &ip, &port), len, 1);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "merged: %d, then the first whole %d, the second whole %d, one merged read %d",
                   k, first ? 1 : 0, second ? 1 : 0, node.udp_gro_reads == merged + 1U ? 1 : 0);
    record(arm, line);
    verdict(arm, k == -1 && first && second && node.udp_gro_reads == merged + 1U, line);
    if (arm->ring) {
        (void)snprintf(line, sizeof(line), "merged: by the ring's recvmsg, recvs +%llu, fallbacks +%llu (want 1, 0)",
                       (unsigned long long)(node.hal.wait_recvs - recvs),
                       (unsigned long long)(node.hal.wait_fallbacks - fallbacks));
        verdict(arm, node.hal.wait_recvs == recvs + 1U && node.hal.wait_fallbacks == fallbacks, line);
    }
    (void)offload(false);
}

static void check_quiet(struct arm* arm) {
    drain_all();
    int last = -1;
    for (uint32_t i = 0; i < QUIET_DATAGRAMS; i++) {
        send_or_fail(DG_QUIET, false);
        last = receive_k(buf, WAIT_LONG_NS);
        if (last != DG_QUIET) {
            break;
        }
    }
    const uint64_t enters = node.hal.wait_enters;
    send_or_fail(DG_AFTER_QUIET, false);
    int after = receive_k(buf, WAIT_LONG_NS);
    const bool off = !node.hal.wait_on && node.hal.wait_enters == enters;
    const bool large = send_large_shaped() && receive_large_shaped();
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "quiet: %u small ones %d, the next %d, then a large one %d", QUIET_DATAGRAMS,
                   last, after, large ? 1 : 0);
    record(arm, line);
    verdict(arm, last == DG_QUIET && after == DG_AFTER_QUIET && large, line);
    (void)snprintf(line, sizeof(line), "quiet: off the ring after them %d; on again after the large one %d (want %d)",
                   off ? 1 : 0, node.hal.wait_on ? 1 : 0, arm->ring ? 1 : 0);
    verdict(arm, off && node.hal.wait_on == arm->ring, line);
}

static bool run_arm(struct arm* arm) {
    if (arm->ring) {
        unsetenv("TT_RX_URING_WAIT");
    } else {
        setenv("TT_RX_URING_WAIT", "0", 1);
    }
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_uring_check: SETUP FAILED - cannot create context\n");
        return false;
    }
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "treatment: uring_wait=%d (want %d)", (int)node.hal.wait_state,
                   arm->ring ? 1 : 0);
    verdict(arm, node.hal.wait_state == (arm->ring ? 1 : 0), line);
    check_on(arm);
    check_wait(arm);
    check_order(arm);
    check_timeout(arm);
    check_wake(arm);
    check_moved(arm);
    check_yield(arm);
    check_both(arm);
    check_drain(arm);
    check_hint(arm);
    check_merged(arm);
    check_quiet(arm);
    printf("rx_uring_check: [%s] uring_wait=%d uring_switches=%llu uring_waits=%llu uring_recvs=%llu uring_copies=%llu "
           "uring_fallbacks=%llu uring_cancels=%llu uring_epoll=%llu\n",
           arm->name, (int)node.hal.wait_state, (unsigned long long)node.hal.wait_switches,
           (unsigned long long)node.hal.wait_enters, (unsigned long long)node.hal.wait_recvs,
           (unsigned long long)node.hal.wait_copies, (unsigned long long)node.hal.wait_fallbacks,
           (unsigned long long)node.hal.wait_cancels, (unsigned long long)node.hal.wait_epolls);
    if (!arm->ring) {
        verdict(arm, node.hal.wait_enters == 0 && node.hal.wait_recvs == 0,
                "treatment: no wait was made on the ring, no datagram came from it");
    }
    tt_Context_destroy(&node);
    return true;
}

static int check_refused(void) {
    struct arm arm = {.name = "refused", .ring = false, .count = 0}; // no ring treatment to check
    unsetenv("TT_RX_URING_WAIT");
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_uring_check: SETUP FAILED - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    verdict(&arm, node.hal.uring_fd < 0 && node.hal.wait_state == -1 && !node.hal.wait_ring,
            "no ring: uring_wait=-1, the epoll path");
    check_wait(&arm);
    tt_Context_destroy(&node);
    return failures == 0 ? CHECK_PASS : CHECK_FAIL;
}

int main(int argc, char** argv) {
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender < 0) {
        printf("rx_uring_check: SETUP FAILED - no sender socket\n");
        return CHECK_SETUP_FAILED;
    }
    if (argc > 1 && strcmp(argv[1], "refused") == 0) {
        int result = check_refused();
        (void)close(sender);
        printf("rx_uring_check: %s - refused\n", result == CHECK_PASS ? "PASS" : "FAIL");
        return result;
    }
    static struct arm control = {.name = "control", .ring = false, .count = 0};
    static struct arm ring = {.name = "ring", .ring = true, .count = 0};
    if (!run_arm(&control) || !run_arm(&ring)) {
        (void)close(sender);
        return CHECK_SETUP_FAILED;
    }
    (void)close(sender);
    bool same = control.count == ring.count && control.count > 0;
    for (unsigned i = 0; same && i < control.count; i++) {
        same = strcmp(control.lines[i], ring.lines[i]) == 0;
        if (!same) {
            printf("rx_uring_check: the arms differ: control '%s', ring '%s'\n", control.lines[i], ring.lines[i]);
        }
    }
    printf("rx_uring_check: %s - the same %u outcomes in both arms\n", same ? "PASS" : "FAIL", control.count);
    failures += same ? 0 : 1;
    if (setup_failed) {
        return CHECK_SETUP_FAILED;
    }
    if (failures != 0) {
        printf("rx_uring_check: FAIL - %d check(s)\n", failures);
        return CHECK_FAIL;
    }
    printf("rx_uring_check: PASS - all\n");
    return CHECK_PASS;
}
