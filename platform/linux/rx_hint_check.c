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
//   2. arrival     - one datagram sent to the context's data socket turns it to "yes" within ARRIVAL_MS
//   3. re-arm      - after the datagram is read, the hint returns to "no"
//
// Under the refusal shim (tests/support/refuse_io_uring.c) the HAL has no ring, and the only correct answer is
// "yes" every time - checked as its own case, because a fallback that said "no" would be the same deafness.
//
// Exit 0 pass, 1 fail, 2 io_uring compiled out (tt_HAL_RX_HINT=tt_RX_HINT_READ), 3 setup failed.
#include <stdio.h>

#include <tickle/hal_linux.h> // tt_HAL_IO_URING, struct tt_hal

// Everything else is for the checks, which a build without io_uring does not compile.
#if tt_HAL_IO_URING
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>
#endif

#define MS_PER_S 1000U
#define NS_PER_MS 1000000U
#define SETTLE_WITHIN_MS 2000U // how long the hint may take to settle to "no"
#define QUIET_HOLD_MS 100U     // and how long it must then hold it
#define ARRIVAL_MS 200U        // how long an arrival may take to show
#define REFUSED_CALLS 1000     // answers checked when there is no ring
#define READ_BUFFER_BYTES 2048 // larger than any datagram the context could be sent here
#define CHECK_CONTEXT_ID 57    // any valid id; this context never meets another

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_COMPILED_OUT = 2, CHECK_SETUP_FAILED = 3 };

#if tt_HAL_IO_URING
static uint64_t now_ms(void) {
    struct timespec now;
    // CLOCK_MONOTONIC lives in a glibc-private header; <time.h> is the public one.
    // NOLINTNEXTLINE(misc-include-cleaner)
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * MS_PER_S) + ((uint64_t)now.tv_nsec / NS_PER_MS);
}

// Reads whatever is waiting, so the hint's next answer is about what arrives after this. Returns whether the probe
// was among it.
static bool drain(struct tt_Context* node, const char* probe, size_t probe_len) {
    uint8_t buf[READ_BUFFER_BYTES];
    uint32_t sender_ip = 0;
    uint16_t sender_port = 0;
    bool found = false;
    for (int32_t len = 0; (len = tt_try_receive(node, buf, sizeof buf, &sender_ip, &sender_port)) >= 0;) {
        if (probe != NULL && (size_t)len == probe_len && memcmp(buf, probe, probe_len) == 0) {
            found = true;
        }
    }
    return found;
}

// The hint must answer "no" for QUIET_HOLD_MS straight, reading whatever a "yes" turns out to be in between - the
// context's own announce comes back to it over loopback broadcast, and that is an arrival like any other.
static bool settles_quiet(struct tt_Context* node) {
    uint64_t start = now_ms();
    uint64_t quiet_since = start;
    while (now_ms() - start < SETTLE_WITHIN_MS) {
        if (tt_rx_maybe_ready(node)) {
            (void)drain(node, NULL, 0);
            quiet_since = now_ms();
        } else if (now_ms() - quiet_since >= QUIET_HOLD_MS) {
            return true;
        }
    }
    return false;
}

static enum check_exit check_refused(struct tt_Context* node) {
    if (node->hal.uring_fd >= 0) {
        printf("rx_hint_check: FAIL - the refusal shim did not reach the HAL (uring_fd=%d)\n", node->hal.uring_fd);
        return CHECK_FAIL;
    }
    for (int call = 0; call < REFUSED_CALLS; call++) {
        if (!tt_rx_maybe_ready(node)) {
            printf("rx_hint_check: FAIL - no ring, yet the hint said nothing arrived (call %d)\n", call);
            return CHECK_FAIL;
        }
    }
    printf("rx_hint_check: PASS [refused] no ring, every answer 'may be'\n");
    return CHECK_PASS;
}

static bool send_probe(struct tt_Context* node, const char* probe, size_t probe_len) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof dest;
    if (getsockname(node->hal.data_sock, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender < 0) {
        return false;
    }
    bool sent = sendto(sender, probe, probe_len, 0, (struct sockaddr*)&dest, sizeof dest) == (ssize_t)probe_len;
    (void)close(sender);
    return sent;
}

static bool arrival_seen(struct tt_Context* node) {
    uint64_t sent_at = now_ms();
    while (now_ms() - sent_at < ARRIVAL_MS) {
        if (tt_rx_maybe_ready(node)) {
            return true;
        }
    }
    return false;
}

static enum check_exit check_ring(struct tt_Context* node) {
    static const char probe[] = "rx_hint_check";
    if (node->hal.uring_fd < 0) {
        printf("rx_hint_check: FAIL - this host refused io_uring, so the hint cannot be checked here\n");
        return CHECK_FAIL;
    }
    // 1. quiet
    if (!settles_quiet(node)) {
        printf("rx_hint_check: FAIL - the hint never held 'no' for %u ms with nothing sent\n", QUIET_HOLD_MS);
        return CHECK_FAIL;
    }
    uint64_t skipped_before = node->hal.uring_skipped;
    (void)tt_rx_maybe_ready(node);
    if (node->hal.uring_skipped != skipped_before + 1) {
        printf("rx_hint_check: FAIL - a quiet 'no' did not count as a skipped read\n");
        return CHECK_FAIL;
    }
    // 2. arrival
    if (!send_probe(node, probe, sizeof probe)) {
        printf("rx_hint_check: FAIL - cannot send the probe datagram\n");
        return CHECK_SETUP_FAILED;
    }
    if (!arrival_seen(node)) {
        printf("rx_hint_check: FAIL - a datagram arrived and the hint still said nothing had, for %u ms\n", ARRIVAL_MS);
        return CHECK_FAIL;
    }
    // The probe, not just "something": the context's own announce may be read first.
    if (!drain(node, probe, sizeof probe)) {
        printf("rx_hint_check: FAIL - the hint said yes but no read returned the probe\n");
        return CHECK_FAIL;
    }
    // 3. re-arm
    if (!settles_quiet(node)) {
        printf("rx_hint_check: FAIL - after the probe was read the hint did not return to 'no'\n");
        return CHECK_FAIL;
    }
    printf("rx_hint_check: PASS [uring] quiet -> arrival seen -> quiet again (arms=%llu skipped=%llu)\n",
           (unsigned long long)node->hal.uring_arms, (unsigned long long)node->hal.uring_skipped);
    return CHECK_PASS;
}
#endif

int main(int argc, char** argv) {
#if !tt_HAL_IO_URING
    (void)argc;
    (void)argv;
    printf("rx_hint_check: io_uring compiled out - nothing to check\n");
    return CHECK_COMPILED_OUT;
#else
    const bool refused_case = argc > 1 && strcmp(argv[1], "refused") == 0;
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_hint_check: FAIL - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    enum check_exit result = refused_case ? check_refused(&node) : check_ring(&node);
    tt_Context_destroy(&node);
    return (int)result;
#endif
}
