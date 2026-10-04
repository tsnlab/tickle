// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// tt_rx_maybe_ready() against the real Linux HAL (README.md, "io_uring"), run by test_samehost.sh.
//
// The unit tests link the mock HAL by design, so nothing there can say whether the io_uring hint is right. And the
// one way it can be wrong is silent: a hint that answers "nothing arrived" after something did would leave a busy
// publisher deaf to its ACKNACKs and to discovery, and the benchmark would still look fine at BEST_EFFORT. So this
// asks the HAL directly, on loopback only:
//
//   1. quiet       - with nothing sent, the hint settles to "no" (and costs no read: uring_skipped rises)
//   2. arrival     - one datagram sent to the context's data socket turns it to "yes" within 200 ms
//   3. re-arm      - after the datagram is read, the hint returns to "no"
//
// Under the refusal shim (tests/support/refuse_io_uring.c) the HAL has no ring, and the only correct answer is
// "yes" every time - checked as its own case, because a fallback that said "no" would be the same deafness.
//
// Exit 0 pass, 1 fail, 2 io_uring compiled out (tt_HAL_RX_HINT=tt_RX_HINT_READ), 3 setup failed.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000U) + ((uint64_t)ts.tv_nsec / 1000000U);
}

// Reads whatever is waiting, so the hint's next answer is about what arrives after this.
static void drain(struct tt_Context* node) {
    uint8_t buf[2048];
    uint32_t ip = 0;
    uint16_t port = 0;
    while (tt_try_receive(node, buf, sizeof buf, &ip, &port) >= 0) {
    }
}

// The hint must answer "no" for `hold_ms` straight, reading whatever a "yes" turns out to be in between - the
// context's own announce comes back to it over loopback broadcast, and that is an arrival like any other.
static bool settles_quiet(struct tt_Context* node, uint64_t within_ms, uint64_t hold_ms) {
    uint64_t start = now_ms();
    uint64_t quiet_since = start;
    while (now_ms() - start < within_ms) {
        if (tt_rx_maybe_ready(node)) {
            drain(node);
            quiet_since = now_ms();
        } else if (now_ms() - quiet_since >= hold_ms) {
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    const bool refused_case = argc > 1 && strcmp(argv[1], "refused") == 0;
#if !tt_HAL_IO_URING
    (void)refused_case;
    printf("rx_hint_check: io_uring compiled out - nothing to check\n");
    return 2;
#else
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = 57;
    struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_hint_check: FAIL - cannot create context\n");
        return 3;
    }
    int rc = 1;
    if (refused_case) {
        if (node.hal.uring_fd >= 0) {
            printf("rx_hint_check: FAIL - the refusal shim did not reach the HAL (uring_fd=%d)\n", node.hal.uring_fd);
            goto out;
        }
        for (int i = 0; i < 1000; i++) {
            if (!tt_rx_maybe_ready(&node)) {
                printf("rx_hint_check: FAIL - no ring, yet the hint said nothing arrived (call %d)\n", i);
                goto out;
            }
        }
        printf("rx_hint_check: PASS [refused] no ring, every answer 'may be'\n");
        rc = 0;
        goto out;
    }
    if (node.hal.uring_fd < 0) {
        printf("rx_hint_check: FAIL - this host refused io_uring, so the hint cannot be checked here\n");
        goto out;
    }

    // 1. quiet
    if (!settles_quiet(&node, 2000, 100)) {
        printf("rx_hint_check: FAIL - the hint never held 'no' for 100 ms with nothing sent\n");
        goto out;
    }
    uint64_t skipped_before = node.hal.uring_skipped;
    (void)tt_rx_maybe_ready(&node);
    if (node.hal.uring_skipped != skipped_before + 1) {
        printf("rx_hint_check: FAIL - a quiet 'no' did not count as a skipped read\n");
        goto out;
    }

    // 2. arrival
    struct sockaddr_in to;
    socklen_t to_len = sizeof to;
    if (getsockname(node.hal.data_sock, (struct sockaddr*)&to, &to_len) != 0) {
        printf("rx_hint_check: FAIL - getsockname\n");
        goto out;
    }
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    const char payload[] = "rx_hint_check";
    if (sender < 0 || sendto(sender, payload, sizeof payload, 0, (struct sockaddr*)&to, sizeof to) < 0) {
        printf("rx_hint_check: FAIL - cannot send the probe datagram\n");
        goto out;
    }
    (void)close(sender);
    uint64_t sent_at = now_ms();
    bool seen = false;
    while (now_ms() - sent_at < 200) {
        if (tt_rx_maybe_ready(&node)) {
            seen = true;
            break;
        }
    }
    if (!seen) {
        printf("rx_hint_check: FAIL - a datagram arrived and the hint still said nothing had, for 200 ms\n");
        goto out;
    }
    // The probe, not just "something": the context's own announce may be read first.
    uint8_t buf[2048];
    uint32_t ip = 0;
    uint16_t port = 0;
    bool probe_read = false;
    for (int32_t len = 0; (len = tt_try_receive(&node, buf, sizeof buf, &ip, &port)) >= 0;) {
        if (len == (int32_t)sizeof payload && memcmp(buf, payload, sizeof payload) == 0) {
            probe_read = true;
        }
    }
    if (!probe_read) {
        printf("rx_hint_check: FAIL - the hint said yes but no read returned the probe\n");
        goto out;
    }

    // 3. re-arm
    drain(&node);
    if (!settles_quiet(&node, 2000, 100)) {
        printf("rx_hint_check: FAIL - after the probe was read the hint did not return to 'no'\n");
        goto out;
    }
    printf("rx_hint_check: PASS [uring] quiet -> arrival seen -> quiet again (arms=%llu skipped=%llu)\n",
           (unsigned long long)node.hal.uring_arms, (unsigned long long)node.hal.uring_skipped);
    rc = 0;
out:
    tt_Context_destroy(&node);
    return rc;
#endif
}
