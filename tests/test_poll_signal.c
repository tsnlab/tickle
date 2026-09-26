/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The two things that end an indefinite wait, on the REAL Linux HAL rather than the mock (2026-09-25).
// A negative-timeout tt_Node_poll() with nothing scheduled now asks tt_receive() for no deadline at all,
// so the only ways out are a datagram, tt_wake_signal() - the route the user named for new work arriving
// mid-wait - and a signal, which is how Ctrl-C reaches a caller's loop. test_poll_wait.c proves what
// TickLE does with each answer; this proves the HAL gives those answers, with real file descriptors, a
// real ppoll() and a real signal.
//
// No network is needed: an unbound UDP socket is never readable, so the wait is genuinely indefinite
// until something ends it. That also keeps this runnable in a namespace with no loopback.

// ppoll() is a GNU extension, and the real HAL included below needs it declared by the system headers -
// which, once included here first, would otherwise have been parsed without it.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier)
#endif
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "../src/hal_linux.c" // NOLINT(bugprone-suspicious-include) - the real HAL, deliberately
#include "test_common.h"

#define SHORT_WAIT_NS (30ULL * 1000ULL * 1000ULL) // the timed control's own deadline
#define ACT_AFTER_NS (50ULL * 1000ULL * 1000ULL)  // how long the blocked thread waits before being woken
#define GIVE_UP_NS (2000ULL * 1000ULL * 1000ULL)  // a wake that has not landed by now never will

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static void sleep_ns(uint64_t ns) {
    struct timespec ts = {.tv_sec = (time_t)(ns / 1000000000ULL), .tv_nsec = (long)(ns % 1000000000ULL)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static void on_signal(int signo) {
    (void)signo; // the point is only that it interrupts ppoll()
}

static struct tt_Node node;

static void open_node(void) {
    memset(&node, 0, sizeof(node));
    node.hal.sock = socket(AF_INET, SOCK_DGRAM, 0);
    node.hal.data_sock = socket(AF_INET, SOCK_DGRAM, 0);
    node.hal.wake_fd = eventfd(0, EFD_NONBLOCK);
    EXPECT_TRUE(node.hal.sock >= 0 && node.hal.data_sock >= 0 && node.hal.wake_fd >= 0);
}

static void close_node(void) {
    close(node.hal.sock);
    close(node.hal.data_sock);
    close(node.hal.wake_fd);
}

struct blocked {
    int32_t result;
    uint64_t returned_at;
    volatile int done;
};

static void* block_indefinitely(void* param) {
    struct blocked* blocked = (struct blocked*)param;
    uint8_t buf[64];
    uint32_t ip = 0;
    uint16_t port = 0;
    blocked->result = tt_receive(&node, buf, sizeof(buf), &ip, &port, 0); // 0: no deadline at all
    blocked->returned_at = now_ns();
    blocked->done = 1;
    return NULL;
}

// Waits for the blocked thread, up to GIVE_UP_NS. Returns whether it came back.
static int wait_for(struct blocked* blocked) {
    uint64_t give_up = now_ns() + GIVE_UP_NS;
    while (!blocked->done && now_ns() < give_up) {
        sleep_ns(1000000ULL);
    }
    return blocked->done;
}

// The control: a finite wait with nothing arriving times out after its deadline and not before. It is
// what makes the two cases below mean something - they return far sooner than they would have waited.
static void test_timed_wait_times_out(void) {
    open_node();
    uint8_t buf[64];
    uint32_t ip = 0;
    uint16_t port = 0;
    uint64_t start = now_ns();
    int32_t len = tt_receive(&node, buf, sizeof(buf), &ip, &port, (int64_t)SHORT_WAIT_NS);
    uint64_t elapsed = now_ns() - start;
    EXPECT_EQ_INT(-1, len);
    EXPECT_TRUE(elapsed >= SHORT_WAIT_NS);
    close_node();
}

// New work from another thread: tt_wake_signal() - what tt_Node_interrupt() calls - ends an indefinite
// wait, reported as -3 so tt_Node_poll() returns tt_RET_INTERRUPTED.
static void test_wake_signal_ends_an_indefinite_wait(void) {
    open_node();
    struct blocked blocked = {.result = 0, .returned_at = 0, .done = 0};
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> above; glibc declares it privately
    EXPECT_TRUE(pthread_create(&thread, NULL, block_indefinitely, &blocked) == 0);

    sleep_ns(ACT_AFTER_NS);
    EXPECT_EQ_INT(0, blocked.done); // still waiting: nothing else could have ended it
    uint64_t woke_at = now_ns();
    EXPECT_EQ_INT(tt_RET_OK, tt_wake_signal(&node));

    EXPECT_TRUE(wait_for(&blocked));
    EXPECT_EQ_INT(-3, blocked.result);
    EXPECT_TRUE(blocked.returned_at - woke_at < GIVE_UP_NS);
    pthread_join(thread, NULL);
    close_node();
}

// Ctrl-C: a signal ends an indefinite wait, reported as a timeout (-1) - which, before its entry was due,
// tt_Node_poll() hands straight back to the caller's loop.
static void test_signal_ends_an_indefinite_wait(void) {
    open_node();
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    action.sa_flags = 0; // no SA_RESTART: the interrupted ppoll() must come back, as it does for SIGINT
    sigemptyset(&action.sa_mask);
    EXPECT_TRUE(sigaction(SIGUSR1, &action, NULL) == 0);

    struct blocked blocked = {.result = 0, .returned_at = 0, .done = 0};
    pthread_t thread; // NOLINT(misc-include-cleaner)
    EXPECT_TRUE(pthread_create(&thread, NULL, block_indefinitely, &blocked) == 0);

    sleep_ns(ACT_AFTER_NS);
    EXPECT_EQ_INT(0, blocked.done);
    EXPECT_TRUE(pthread_kill(thread, SIGUSR1) == 0);

    EXPECT_TRUE(wait_for(&blocked));
    EXPECT_EQ_INT(-1, blocked.result);
    pthread_join(thread, NULL);
    close_node();
}

// ---- tt_try_receive() reads only sockets that can have data (2026-09-26). Two Unix-domain datagram
// socketpairs stand in for the well-known and data sockets: the HAL reads them with the same recvfrom()
// calls, and they need no network.

static int pair_wk[2];
static int pair_data[2];

static void open_pair_node(void) {
    memset(&node, 0, sizeof(node));
    EXPECT_TRUE(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair_wk) == 0);
    EXPECT_TRUE(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair_data) == 0);
    node.hal.sock = pair_wk[0];
    node.hal.data_sock = pair_data[0];
    node.hal.wake_fd = eventfd(0, EFD_NONBLOCK);
}

static void close_pair_node(void) {
    close(pair_wk[0]);
    close(pair_wk[1]);
    close(pair_data[0]);
    close(pair_data[1]);
    close(node.hal.wake_fd);
}

static int32_t try_one(uint8_t* tag) {
    uint8_t buf[16] = {0};
    uint32_t ip = 0;
    uint16_t port = 0;
    int32_t len = tt_try_receive(&node, buf, sizeof(buf), &ip, &port);
    *tag = buf[0];
    return len;
}

// Once a socket has come up empty in a drain it is not asked again until the drain ends - so data that
// lands on it meanwhile waits for the next session - and it is not lost: the next session reads it.
static void test_drain_skips_a_socket_found_empty(void) {
    open_pair_node();
    uint8_t tag = 0;
    EXPECT_EQ_INT(1, (int)send(pair_data[1], "a", 1, 0));
    EXPECT_EQ_INT(1, (int)send(pair_data[1], "b", 1, 0));

    EXPECT_EQ_INT(1, try_one(&tag)); // well-known asked first, empty: marked idle; data read
    EXPECT_EQ_INT('a', tag);
    EXPECT_EQ_INT(1, try_one(&tag));
    EXPECT_EQ_INT('b', tag);

    EXPECT_EQ_INT(1, (int)send(pair_wk[1], "w", 1, 0)); // lands on the socket already found empty
    EXPECT_EQ_INT(-1, try_one(&tag));                   // skipped: the drain ends on the data socket's own empty read
    EXPECT_EQ_INT(1, try_one(&tag));                    // the next session asks both again
    EXPECT_EQ_INT('w', tag);
    close_pair_node();
}

// After a wait, a socket ppoll() did not report ready is not asked at all by the drain that follows.
static void test_drain_after_a_wait_reads_only_ready_sockets(void) {
    open_pair_node();
    uint8_t buf[16] = {0};
    uint32_t ip = 0;
    uint16_t port = 0;
    EXPECT_EQ_INT(1, (int)send(pair_data[1], "a", 1, 0));
    EXPECT_EQ_INT(1, (int)send(pair_data[1], "b", 1, 0));

    EXPECT_EQ_INT(1, tt_receive(&node, buf, sizeof(buf), &ip, &port, (int64_t)SHORT_WAIT_NS)); // reads 'a'
    EXPECT_EQ_INT('a', buf[0]);
    // ppoll saw nothing on the well-known socket. With batching, the recvmmsg() that read 'a' took 'b' as
    // well and came back short, so the data socket is known drained too.
    EXPECT_EQ_INT(tt_RX_BATCH > 1 ? (TT_RX_IDLE_WELL_KNOWN | TT_RX_IDLE_DATA) : TT_RX_IDLE_WELL_KNOWN,
                  node.hal.rx_idle);

    EXPECT_EQ_INT(1, (int)send(pair_wk[1], "w", 1, 0));
    uint8_t tag = 0;
    EXPECT_EQ_INT(1, try_one(&tag)); // the data socket, whatever the alternation says
    EXPECT_EQ_INT('b', tag);
    EXPECT_EQ_INT(-1, try_one(&tag)); // drained - the well-known socket was never asked
    EXPECT_EQ_INT(1, try_one(&tag));  // and the next session finds 'w'
    EXPECT_EQ_INT('w', tag);
    close_pair_node();
}

#if tt_RX_BATCH > 1
static uint64_t elapsed_ns_since(const struct timespec* start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)(now.tv_sec - start->tv_sec) * 1000000000ULL) + (uint64_t)now.tv_nsec - (uint64_t)start->tv_nsec;
}

// Everything already queued on a socket comes in with one recvmmsg(): the first datagram returned, the
// rest held and handed out by tt_try_receive() without another syscall.
static void test_one_call_reads_every_queued_datagram(void) {
    open_pair_node();
    static const uint8_t tags[] = {'a', 'b', 'c', 'd', 'e'};
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ_INT(1, (int)send(pair_data[1], &tags[i], 1, 0));
    }
    uint8_t buf[16] = {0};
    uint32_t ip = 0;
    uint16_t port = 0;
    EXPECT_EQ_INT(1, tt_receive(&node, buf, sizeof(buf), &ip, &port, (int64_t)SHORT_WAIT_NS));
    EXPECT_EQ_INT('a', buf[0]);
    EXPECT_EQ_U64(1, node.hal.rx_batch_calls);
    EXPECT_EQ_U64(5, node.hal.rx_batch_datagrams);
    uint8_t tag = 0;
    for (int i = 1; i < 5; i++) {
        EXPECT_EQ_INT(1, try_one(&tag));
        EXPECT_EQ_INT(tags[i], tag);
    }
    EXPECT_EQ_U64(1, node.hal.rx_batch_calls); // all four came from the batch
    EXPECT_EQ_INT(-1, try_one(&tag));
    close_pair_node();
}

// A datagram a batch already holds is returned at once, never behind a wait - this is what keeps batching
// from adding latency. The socket is empty, so a tt_receive() that waited first would sit out its timeout.
static void test_a_held_datagram_is_returned_before_any_wait(void) {
    open_pair_node();
    EXPECT_EQ_INT(1, (int)send(pair_data[1], "a", 1, 0));
    EXPECT_EQ_INT(1, (int)send(pair_data[1], "b", 1, 0));
    uint8_t buf[16] = {0};
    uint32_t ip = 0;
    uint16_t port = 0;
    EXPECT_EQ_INT(1, tt_receive(&node, buf, sizeof(buf), &ip, &port, (int64_t)SHORT_WAIT_NS));
    EXPECT_EQ_INT('a', buf[0]);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    EXPECT_EQ_INT(1, tt_receive(&node, buf, sizeof(buf), &ip, &port, (int64_t)tt_SECOND));
    EXPECT_EQ_INT('b', buf[0]);
    EXPECT_TRUE(elapsed_ns_since(&start) < 50ULL * 1000000ULL);
    close_pair_node();
}

// A batch that came back full may have left more behind, so it does not mark its socket drained: the
// drain reads on, and the datagram past the batch is not stranded until the next wait.
static void test_a_full_batch_does_not_mark_the_socket_drained(void) {
    open_pair_node();
    for (int i = 0; i <= tt_RX_BATCH; i++) {
        uint8_t tag = (uint8_t)i;
        EXPECT_EQ_INT(1, (int)send(pair_data[1], &tag, 1, 0));
    }
    uint8_t buf[16] = {0};
    uint32_t ip = 0;
    uint16_t port = 0;
    EXPECT_EQ_INT(1, tt_receive(&node, buf, sizeof(buf), &ip, &port, (int64_t)SHORT_WAIT_NS));
    EXPECT_EQ_U64(1, node.hal.rx_batch_full);
    EXPECT_EQ_INT(0, node.hal.rx_idle & TT_RX_IDLE_DATA);
    uint8_t tag = 0;
    for (int i = 1; i <= tt_RX_BATCH; i++) {
        EXPECT_EQ_INT(1, try_one(&tag));
        EXPECT_EQ_INT(i, tag);
    }
    EXPECT_EQ_U64(2, node.hal.rx_batch_calls); // the last one needed a second call, in the same drain
    close_pair_node();
}
#endif

int main(void) {
#if tt_RX_BATCH > 1
    test_one_call_reads_every_queued_datagram();
    test_a_held_datagram_is_returned_before_any_wait();
    test_a_full_batch_does_not_mark_the_socket_drained();
#endif
    test_drain_skips_a_socket_found_empty();
    test_drain_after_a_wait_reads_only_ready_sockets();
    test_timed_wait_times_out();
    test_wake_signal_ends_an_indefinite_wait();
    test_signal_ends_an_indefinite_wait();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_poll_signal: all tests passed\n");
    return 0;
}
