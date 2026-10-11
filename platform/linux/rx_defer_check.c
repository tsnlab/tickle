// Copyright (c) 2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// A deferred wait (hal_linux.c rx_defer(), struct tt_Context.rx_defer_ns) against the real Linux HAL, on loopback in a
// private network namespace (rx_defer_check.sh, from test.sh). Core asks for a deferral while a large sample arrives
// (tests/test_rx_defer.c checks when); this checks what the HAL then does with one:
//   defers   - a datagram already queued is not read until the deferral has passed, and the deferral is counted;
//              control: the same with no deferral asked for comes back at once;
//   once     - the request is used once: the next wait, asked for nothing, reads at once;
//   wake     - tt_wake_signal() (tt_Context_interrupt()) ends a deferral at once: the wait reports the interrupt;
//   bell     - a ring of the segment's bell ends a deferral at once: the wait reports the bell;
//   deadline - a deferral longer than the caller's timeout ends at the timeout, with nothing queued: a timeout;
//   rest     - the wait after a deferral has only what is left of the caller's timeout;
//   budget   - the budget is a quarter of the data socket's granted receive buffer in datagrams of twice the
//              largest fragment's size, computed here from getsockopt() on the context's own socket; with
//              TT_RX_DEFER=0 in the environment (argument "off") it is 0.
// Run once more under tests/support/refuse_epoll.c, which leaves the HAL no epoll: every wait, the deferral's
// included, then goes through ppoll(), and the same checks hold.
// Built as rmw_tickle builds core (65507-byte datagram, large samples), with the segment and its bell. Each check's
// mutant is in tests/mutants_rx_defer.py. Timing bounds are an order of magnitude wide: a loaded machine makes a
// wait late, never early, and every bound that a late wait could break is a lower one.
//
// Exit 0 pass, 1 fail, 3 setup failed. Usage: rx_defer_check [off]
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>   // NOLINT(misc-include-cleaner) - nanosleep(), struct timespec
#include <unistd.h> // NOLINT(misc-include-cleaner) - close(), write(), mkdtemp() via <stdlib.h>

#include <netinet/in.h>
#include <sys/socket.h> // NOLINT(misc-include-cleaner) - SOL_SOCKET, SO_*: glibc-private headers behind it
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h>
#include <tickle/tickle.h>

#define CHECK_CONTEXT_ID 57 // any valid id; this context never meets another
#define NS_PER_MS 1000000ULL
#define DEFER_MS 200ULL // a deferral long enough to tell from a wait that did not defer
#define LONG_DEFER_MS 2000ULL
#define PROMPT_MS 100ULL // "at once": well under any deferral asked for here
#define SIGNAL_AFTER_MS 20ULL
#define DEADLINE_MS 300ULL // the mutant that ignores it reaches the next bound only at twice this
#define REST_DEFER_MS 200ULL
#define REST_TIMEOUT_MS 400ULL
#define REST_SLACK_MS 100ULL // the rest check fails at REST_TIMEOUT + this; the mutant takes REST_DEFER more
#define READ_BUFFER_BYTES 2048
#define REPORT_BYTES 256
#define MS_PER_S 1000ULL
#define WATCHDOG_MS 1000ULL
#define WIRE_FRAGMENT 1472U // tt_CONTROL_MAX_LENGTH, the datagram a budget counts
#define BUDGET_SHARE 4U     // of the buffer, as hal_linux.c RX_DEFER_SHARE
#define BELL_PATH_BYTES 64

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_SETUP_FAILED = 3 };

static struct tt_Context node;
static int sender = -1;
static int failures;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - <time.h>
    return ((uint64_t)ts.tv_sec * MS_PER_S) + ((uint64_t)ts.tv_nsec / NS_PER_MS);
}

static void sleep_ms(uint64_t millis) {
    struct timespec ts = {.tv_sec = (time_t)(millis / MS_PER_S), .tv_nsec = (long)((millis % MS_PER_S) * NS_PER_MS)};
    nanosleep(&ts, NULL);
}

static void report(bool passed, const char* name, const char* what) {
    printf("rx_defer_check: %s %s - %s\n", passed ? "PASS" : "FAIL", name, what);
    if (!passed) {
        failures++;
    }
}

static bool queue_datagram(void) {
    static const char probe[] = "rx_defer_check probe";
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof dest;
    if (getsockname(node.hal.data_sock, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool sent = sendto(sender, probe, sizeof probe, 0, (struct sockaddr*)&dest, sizeof dest) == (ssize_t)sizeof probe;
    sleep_ms(1); // loopback delivery is immediate in practice; this only removes the doubt
    return sent;
}

// tt_receive() with `defer_ms` asked for and `timeout_ms` (0: no limit); its result, and in *took how long it took.
static int32_t receive(uint64_t defer_ms, uint64_t timeout_ms, uint64_t* took) {
    uint8_t buf[READ_BUFFER_BYTES];
    uint32_t ip = 0;
    uint16_t port = 0;
    node.rx_defer_ns = defer_ms * NS_PER_MS;
    uint64_t start = now_ms();
    int32_t got = tt_receive(&node, buf, sizeof buf, &ip, &port, (int64_t)(timeout_ms * NS_PER_MS));
    *took = now_ms() - start;
    return got;
}

static void* ring_later(void* arg) {
    sleep_ms(SIGNAL_AFTER_MS);
    if (arg == NULL) {
        (void)tt_wake_signal(&node);
    } else {
        const uint8_t one = 1;
        (void)write(*(int*)arg, &one, sizeof one); // the owner holds its bell read-write: a ring, as a peer's
    }
    return NULL;
}

static void check_defers_and_once(void) {
    uint64_t took = 0;
    char what[REPORT_BYTES];
    // Control: no deferral, a queued datagram comes back at once.
    bool queued = queue_datagram();
    int32_t got = receive(0, 0, &took);
    snprintf(what, sizeof what, "no deferral asked: read %d B after %llu ms (must be > 0 B within %llu ms)", got,
             (unsigned long long)took, (unsigned long long)PROMPT_MS);
    report(queued && got > 0 && took < PROMPT_MS, "control", what);

    uint64_t defers = node.rx_defers;
    queued = queue_datagram();
    got = receive(DEFER_MS, 0, &took);
    snprintf(what, sizeof what,
             "%llu ms asked: read %d B after %llu ms, rx_defers +%llu (must be > 0 B, >= %llu ms, +1)",
             (unsigned long long)DEFER_MS, got, (unsigned long long)took, (unsigned long long)(node.rx_defers - defers),
             (unsigned long long)DEFER_MS - 1ULL);
    report(queued && got > 0 && took >= DEFER_MS - 1ULL && node.rx_defers == defers + 1U, "defers", what);

    // Once: core sets the request before every wait, so the HAL clearing it is what keeps a request from outliving
    // its wait - set it, wait once, and wait again without setting it.
    node.rx_defer_ns = DEFER_MS * NS_PER_MS;
    queued = queue_datagram();
    uint8_t buf[READ_BUFFER_BYTES];
    uint32_t ip = 0;
    uint16_t port = 0;
    (void)tt_receive(&node, buf, sizeof buf, &ip, &port, 0); // uses the request
    uint64_t left = node.rx_defer_ns;
    queued = queue_datagram() && queued;
    uint64_t start = now_ms();
    got = tt_receive(&node, buf, sizeof buf, &ip, &port, 0); // asked for nothing new
    took = now_ms() - start;
    snprintf(what, sizeof what,
             "after a deferred wait rx_defer_ns is %llu and the next wait read %d B after %llu ms "
             "(must be 0, > 0 B, within %llu ms)",
             (unsigned long long)left, got, (unsigned long long)took, (unsigned long long)PROMPT_MS);
    report(queued && left == 0 && got > 0 && took < PROMPT_MS, "once", what);
}

static void check_ends(const char* name, int* bell, int32_t expect) {
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h>
    uint64_t took = 0;
    char what[REPORT_BYTES];
    if (pthread_create(&thread, NULL, ring_later, bell) != 0) {
        report(false, name, "cannot start the thread that rings");
        return;
    }
    int32_t got = receive(LONG_DEFER_MS, 0, &took);
    pthread_join(thread, NULL);
    snprintf(what, sizeof what,
             "%llu ms asked, rung after %llu ms: returned %d after %llu ms (must be %d within %llu ms)",
             (unsigned long long)LONG_DEFER_MS, (unsigned long long)SIGNAL_AFTER_MS, got, (unsigned long long)took,
             expect, SIGNAL_AFTER_MS + PROMPT_MS);
    report(got == expect && took < SIGNAL_AFTER_MS + PROMPT_MS, name, what);
}

// A deferral longer than the timeout, nothing queued: a timeout at the timeout. A watchdog interrupt after a second
// keeps a wait that has lost its deadline from hanging the check (it then fails with -3, not -1).
static void* watchdog(void* arg) {
    (void)arg;
    sleep_ms(WATCHDOG_MS);
    (void)tt_wake_signal(&node);
    return NULL;
}

static void check_deadline_and_rest(void) {
    uint64_t took = 0;
    char what[REPORT_BYTES];
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h>
    if (pthread_create(&thread, NULL, watchdog, NULL) != 0) {
        report(false, "deadline", "cannot start the watchdog");
        return;
    }
    int32_t got = receive(LONG_DEFER_MS, DEADLINE_MS, &took);
    pthread_join(thread, NULL);
    uint8_t buf[READ_BUFFER_BYTES];
    uint32_t ip = 0;
    uint16_t port = 0;
    (void)tt_receive(&node, buf, sizeof buf, &ip, &port, (int64_t)NS_PER_MS); // takes the watchdog's interrupt
    snprintf(what, sizeof what,
             "%llu ms asked, timeout %llu ms, nothing queued: returned %d after %llu ms (must be -1 "
             "within %llu ms)",
             (unsigned long long)LONG_DEFER_MS, (unsigned long long)DEADLINE_MS, got, (unsigned long long)took,
             DEADLINE_MS + PROMPT_MS);
    report(got == -1 && took >= DEADLINE_MS - 1ULL && took < DEADLINE_MS + PROMPT_MS, "deadline", what);

    got = receive(REST_DEFER_MS, REST_TIMEOUT_MS, &took);
    snprintf(what, sizeof what,
             "%llu ms asked, timeout %llu ms, nothing queued: returned %d after %llu ms (must be -1 "
             "in [%llu, %llu) ms - the deferral comes out of the timeout)",
             (unsigned long long)REST_DEFER_MS, (unsigned long long)REST_TIMEOUT_MS, got, (unsigned long long)took,
             REST_TIMEOUT_MS - 1ULL, REST_TIMEOUT_MS + REST_SLACK_MS);
    report(got == -1 && took >= REST_TIMEOUT_MS - 1ULL && took < REST_TIMEOUT_MS + REST_SLACK_MS, "rest", what);
}

static uint32_t granted(int socket_fd) {
    int value = 0;
    socklen_t length = sizeof value;
    // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET, SO_RCVBUF: glibc-private headers behind <sys/socket.h>
    return getsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &value, &length) == 0 ? (uint32_t)value : 0U;
}

static void check_budget(bool off) {
    char what[REPORT_BYTES];
    uint32_t data = granted(node.hal.data_sock);
    uint32_t expect = off ? 0U : data / (2U * WIRE_FRAGMENT) / BUDGET_SHARE;
    snprintf(what, sizeof what, "granted %u B, budget %u (must be %u%s), epoll %s", data, node.rx_defer_budget, expect,
             off ? ": TT_RX_DEFER=0" : "", node.hal.epoll_fd_plus1 > 0 ? "on" : "off");
    report(node.rx_defer_budget == expect && (off || expect > 0U), "budget", what);
}

int main(int argc, char** argv) {
    bool off = argc > 1 && strcmp(argv[1], "off") == 0;
    if (off) {
        setenv("TT_RX_DEFER", "0", 1);
    }
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_defer_check: SETUP FAILED - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    check_budget(off);
    if (off) {
        tt_Context_destroy(&node);
        return failures == 0 ? CHECK_PASS : CHECK_FAIL;
    }
    sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender < 0) {
        printf("rx_defer_check: SETUP FAILED - cannot open a sender\n");
        tt_Context_destroy(&node);
        return CHECK_SETUP_FAILED;
    }
    char dir[] = "/tmp/rx_defer_check.XXXXXX";
    char bell_path[BELL_PATH_BYTES] = "";
    if (node.hal.bell_fd_plus1 == 0 && mkdtemp(dir) != NULL) {
        // No segment of its own yet (it is made when a same-host peer appears): a bell of its own, as the segment
        // would make one - the HAL call puts it in both wait sets.
        snprintf(bell_path, sizeof bell_path, "%s/bell", dir);
        (void)tt_segment_bell_create(&node, bell_path);
    }
    check_defers_and_once();
    check_ends("wake", NULL, -3);
    int bell = node.hal.bell_fd_plus1 - 1;
    if (bell >= 0) {
        check_ends("bell", &bell, 0);
    } else {
        report(false, "bell", "no bell to ring (tt_segment_bell_create() failed)");
    }
    check_deadline_and_rest();
    printf("rx_defer_check: RESULT failures=%d rx_defers=%llu budget=%u\n", failures,
           (unsigned long long)node.rx_defers, node.rx_defer_budget);
    if (bell_path[0] != '\0') {
        tt_segment_bell_destroy(&node, bell_path);
        (void)rmdir(dir);
    }
    (void)close(sender);
    tt_Context_destroy(&node);
    return failures == 0 ? CHECK_PASS : CHECK_FAIL;
}
