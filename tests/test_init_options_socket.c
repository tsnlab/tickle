/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions, bundle 6: the socket group (struct tt_SocketOptions) on the REAL Linux HAL, built as
// rmw_tickle builds core (a 65507-byte datagram, large samples: the build offload, checksum 0, the direct read and the
// drain are compiled into).
//   - the defaults are the constants and switches the HAL had, and a zeroed struct reads the same;
//   - every field, set to a value other than its default, changes what tt_bind() sets up: the buffer size the kernel
//     grants, each switch's state, whether a refused io_uring fails creation, the drain's buffers, its growth and its
//     hold-off. The poll group's receive_timeout is read here too (the idle-socket recheck);
//   - each switch's environment variable decides over the option when set, both ways, and the option decides when
//     it is unset;
//   - every check refuses a bad value with tt_RET_INVALID_ARGUMENT and accepts the boundary beside it
//     (tests/mutants_init_options_socket.sh removes each check and expects this test to fail).
// io_uring's refusal is made here (this file's syscall(), which the HAL's io_uring_setup() reaches), not needed from
// the system. A system that refuses it anyway skips the drain's checks and says so - never a silent pass.
//
// Loopback only: contexts are created and destroyed without a poll, their broadcast is 127.255.255.255, the id
// registry is off, and the only datagrams are this test's own, sent to its own data socket over 127.0.0.1.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier) - the real HAL needs ppoll(); RTLD_NEXT
#endif
#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#ifndef HAL_LINUX_C
#define HAL_LINUX_C "../src/hal_linux.c"
#endif
#include HAL_LINUX_C // NOLINT(bugprone-suspicious-include) - the real HAL, deliberately; or a mutant of it
#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_socket.sh passes a mutant
#include "test_common.h"

#define PORT 47331
#define LOOPBACK_BROADCAST "127.255.255.255"
#define CONTEXT_ID 23
#define SMALL_BUFFER 16384U               // the kernel grants twice what it is asked, above its minimum
#define FEW_BUFFERS 64U                   // drain_buffers
#define SMALL_DRAIN (256U * 1024U)        // drain_bytes
#define SHORT_HOLDOFF (2ULL * 1000000ULL) // drain_holdoff_ns, 2 ms
#define CUT_BYTES 3000                    // longer than a drain buffer for 1,472-byte datagrams
#define RECHECK_SHORT (50ULL * 1000ULL)   // poll.receive_timeout, 50 us

static struct tt_Context ctx;

// ---- io_uring refused on demand: the HAL's io_uring_setup() is a syscall() call, which this definition answers.
static bool refuse_io_uring = false;

long syscall(long number, ...) { // NOLINT(readability-inconsistent-declaration-parameter-name, cert-dcl37-c)
    va_list ap;
    va_start(ap, number);
    long a[6];
    for (int i = 0; i < 6; i++) {
        a[i] = va_arg(ap, long);
    }
    va_end(ap);
    if (refuse_io_uring && number == __NR_io_uring_setup) {
        errno = EPERM;
        return -1;
    }
    long (*real)(long, ...) = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
    return real(number, a[0], a[1], a[2], a[3], a[4], a[5]);
}

static const char* const SWITCH_ENV[] = {"TT_UDP_OFFLOAD",   "TT_UDP_CSUM0",   "TT_RX_DIRECT",    "TT_RX_LAZY_TIMER",
                                         "TT_RX_LAZY_ADAPT", "TT_RX_NONBLOCK", "TT_RX_URING_WAIT"};
#define SWITCH_COUNT 7
#define SWITCH_URING_WAIT 6 // the one switch whose default is the build's (tt_RX_URING_WAIT), not on

static void clear_switch_env(void) {
    for (int i = 0; i < SWITCH_COUNT; i++) {
        (void)unsetenv(SWITCH_ENV[i]);
    }
}

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    opts.net.broadcast = LOOPBACK_BROADCAST;
    opts.net.port = PORT;
    opts.net.context_id = CONTEXT_ID;
    return opts;
}

static tt_ret_t create(const struct tt_InitOptions* opts) {
    memset(&ctx, 0, sizeof(ctx));
    return tt_Context_create_with(&ctx, opts);
}

static void destroy(void) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
}

// ---- The defaults.

static void test_defaults_are_the_compiled_settings(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_U32(tt_SOCKET_BUFFER_SIZE, opts.socket.buffer_size);
    EXPECT_EQ_U32(tt_SWITCH_ON, opts.socket.udp_offload);
    EXPECT_EQ_U32(tt_SWITCH_ON, opts.socket.udp_csum0);
    EXPECT_EQ_U32(tt_SWITCH_ON, opts.socket.rx_direct);
    EXPECT_EQ_U32(tt_SWITCH_ON, opts.socket.rx_lazy_timer);
    EXPECT_EQ_U32(tt_SWITCH_ON, opts.socket.rx_lazy_adapt);
    EXPECT_EQ_U32(tt_SWITCH_ON, opts.socket.rx_nonblock);
    EXPECT_EQ_U32(tt_RX_URING_WAIT ? tt_SWITCH_ON : tt_SWITCH_OFF, opts.socket.rx_uring_wait);
    EXPECT_EQ_U32(tt_SOCKET_RX_HINT_AUTO, opts.socket.rx_hint); // this build's tt_HAL_RX_HINT
    EXPECT_EQ_U32(tt_RX_DRAIN_BYTES, opts.socket.drain_bytes);
    EXPECT_EQ_U32(tt_RX_DRAIN_BUFFERS, opts.socket.drain_buffers);
    EXPECT_EQ_U32(tt_RX_DRAIN_BYTES_MAX, opts.socket.drain_bytes_max);
    EXPECT_EQ_U64(tt_RX_DRAIN_HOLDOFF_NS, opts.socket.drain_holdoff_ns);
    // drain_bytes_max left 0 is four times the context's own start.
    struct tt_InitOptions zero;
    memset(&zero, 0, sizeof(zero));
    zero.socket.drain_bytes = SMALL_DRAIN;
    tt_InitOptions_default(&zero); // clears it: the documented use is "zero, then set"
    EXPECT_EQ_U32(tt_RX_DRAIN_BYTES_MAX, zero.socket.drain_bytes_max);
    opts = defaults();
    opts.socket.drain_bytes = SMALL_DRAIN;
    opts.socket.drain_bytes_max = 0;
    clear_switch_env();
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_U32(4U * SMALL_DRAIN, ctx.opts.socket.drain_bytes_max);
    destroy();
}

// ---- buffer_size: what the kernel grants both sockets, both directions.

static int granted(int sock, int which) {
    int value = 0;
    socklen_t length = sizeof(value);
    if (getsockopt(sock, SOL_SOCKET, which, &value, &length) != 0) {
        return -1;
    }
    return value;
}

static void test_buffer_size_is_what_both_sockets_ask(void) {
    clear_switch_env();
    struct tt_InitOptions opts = defaults();
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    const int standard = granted(ctx.hal.data_sock, SO_RCVBUF);
    destroy();
    opts.socket.buffer_size = SMALL_BUFFER;
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT((int)(2U * SMALL_BUFFER), granted(ctx.hal.sock, SO_RCVBUF));
    EXPECT_EQ_INT((int)(2U * SMALL_BUFFER), granted(ctx.hal.sock, SO_SNDBUF));
    EXPECT_EQ_INT((int)(2U * SMALL_BUFFER), granted(ctx.hal.data_sock, SO_RCVBUF));
    EXPECT_EQ_INT((int)(2U * SMALL_BUFFER), granted(ctx.hal.data_sock, SO_SNDBUF));
    destroy();
    EXPECT_TRUE(standard > (int)(2U * SMALL_BUFFER)); // the default asks 1 MiB; any rmem_max above 16 KB grants more
}

// ---- The switches: each one's state after tt_bind(), under the option and under its environment variable.

static bool switch_state(int which) {
    switch (which) {
    case 0:
        return ctx.udp_offload == (TT_UDP_OFFLOAD_GRO | TT_UDP_OFFLOAD_GSO);
    case 1:
        return ctx.hal.csum0 == 1;
    case 2:
        return ctx.large.direct_allowed;
    case 3:
        return ctx.hal.timer_fd_plus1 != 0;
    case 4:
        return ctx.hal.timer_adapt;
    case SWITCH_URING_WAIT:
        return ctx.hal.wr_fd_plus1 > 0;
    default: {
        int flags = fcntl(ctx.hal.data_sock, F_GETFL);
        return ctx.hal.nonblocking && flags >= 0 && (flags & O_NONBLOCK) != 0;
    }
    }
}

static uint8_t* switch_field(struct tt_InitOptions* opts, int which) {
    uint8_t* fields[SWITCH_COUNT] = {
        &opts->socket.udp_offload,   &opts->socket.udp_csum0,   &opts->socket.rx_direct,    &opts->socket.rx_lazy_timer,
        &opts->socket.rx_lazy_adapt, &opts->socket.rx_nonblock, &opts->socket.rx_uring_wait};
    return fields[which];
}

// The state with the option at `option` and the environment variable at `env` (NULL: unset).
static bool state_with(int which, uint8_t option, const char* env) {
    clear_switch_env();
    if (env != NULL) {
        (void)setenv(SWITCH_ENV[which], env, 1);
    }
    struct tt_InitOptions opts = defaults();
    *switch_field(&opts, which) = option;
    bool state = false;
    if (create(&opts) == tt_RET_OK) {
        state = switch_state(which);
        destroy();
    } else {
        test_failures++;
        fprintf(stderr, "%s: creation failed\n", SWITCH_ENV[which]);
    }
    clear_switch_env();
    return state;
}

static void expect_state(int which, uint8_t option, const char* env, bool expected) {
    bool got = state_with(which, option, env);
    if (got != expected) {
        fprintf(stderr, "%s: option %u, environment %s: %s, expected %s\n", SWITCH_ENV[which], (unsigned)option,
                env != NULL ? env : "unset", got ? "on" : "off", expected ? "on" : "off");
        test_failures++;
    }
}

static void test_each_switch_and_its_environment_variable(void) {
    for (int which = 0; which < SWITCH_COUNT; which++) {
        // The default: on, but for the wait ring, whose default is the build's.
        expect_state(which, tt_SWITCH_DEFAULT, NULL, which != SWITCH_URING_WAIT || tt_RX_URING_WAIT);
        expect_state(which, tt_SWITCH_ON, NULL, true);
        expect_state(which, tt_SWITCH_OFF, NULL, false);  // the option, unset environment
        expect_state(which, tt_SWITCH_ON, "0", false);    // the environment decides: off over on
        expect_state(which, tt_SWITCH_OFF, "1", true);    // and on over off
        expect_state(which, tt_SWITCH_DEFAULT, "", true); // set but not "0": on, as it always was
    }
}

// ---- rx_hint: whether a refused io_uring fails creation.

static void test_rx_hint_decides_what_a_refusal_does(void) {
    clear_switch_env();
    struct tt_InitOptions opts = defaults();
    refuse_io_uring = true;
    opts.socket.rx_hint = 0; // this build's: AUTO
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(-1, ctx.hal.uring_fd);
    destroy();
    opts.socket.rx_hint = tt_SOCKET_RX_HINT_AUTO;
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    destroy();
    opts.socket.rx_hint = tt_SOCKET_RX_HINT_URING;
    EXPECT_EQ_INT(tt_RET_UNSUPPORTED, create(&opts));
    refuse_io_uring = false;
    // Allowed, URING is an ordinary context - where the system allows io_uring at all.
    tt_ret_t allowed = create(&opts);
    if (allowed == tt_RET_OK) {
        EXPECT_TRUE(ctx.hal.uring_fd >= 0);
        destroy();
    } else {
        EXPECT_EQ_INT(tt_RET_UNSUPPORTED, allowed);
        printf("test_init_options_socket: SKIP rx_hint URING allowed - this system refuses io_uring itself\n");
    }
}

// ---- The drain: buffers, growth and hold-off.

// The count tt_rx_drain_start() sets up: the most buffers, halved until they fit the budget.
static uint32_t expected_count(uint32_t buffers, uint32_t budget, uint32_t size) {
    uint32_t count = buffers;
    while (count > 1U && (uint64_t)count * size > budget) {
        count /= 2U;
    }
    return count;
}

// Starts a drain on a fresh context; false (and a printed SKIP) when this system gives none.
static bool drain_on(const struct tt_InitOptions* opts) {
    EXPECT_EQ_INT(tt_RET_OK, create(opts));
    if (!tt_rx_drain_start(&ctx, tt_CONTROL_MAX_LENGTH)) {
        destroy();
        return false;
    }
    return true;
}

static bool test_drain_sizes(void) {
    clear_switch_env();
    struct tt_InitOptions opts = defaults();
    if (!drain_on(&opts)) {
        printf("test_init_options_socket: SKIP drain - tt_rx_drain_start() refused (io_uring refused, Linux < 6.0)\n");
        return false;
    }
    const uint32_t size = ctx.hal.drain_size;
    EXPECT_EQ_U32(expected_count(tt_RX_DRAIN_BUFFERS, tt_RX_DRAIN_BYTES, size), ctx.hal.drain_count);
    destroy();

    opts.socket.drain_buffers = FEW_BUFFERS;
    EXPECT_TRUE(drain_on(&opts));
    EXPECT_EQ_U32(FEW_BUFFERS, ctx.hal.drain_count);
    destroy();

    opts = defaults();
    opts.socket.drain_bytes = SMALL_DRAIN;
    EXPECT_TRUE(drain_on(&opts));
    EXPECT_EQ_U32(SMALL_DRAIN, ctx.hal.drain_budget);
    EXPECT_EQ_U32(expected_count(tt_RX_DRAIN_BUFFERS, SMALL_DRAIN, size), ctx.hal.drain_count);
    EXPECT_TRUE(ctx.hal.drain_count < expected_count(tt_RX_DRAIN_BUFFERS, tt_RX_DRAIN_BYTES, size));
    destroy();
    return true;
}

// An overrun earns the next start twice the bytes, up to drain_bytes_max. The overrun is set by hand: what makes one
// is rx_drain_check.c's (7. rearm).
static uint32_t budget_after_an_overrun(uint32_t bytes_max) {
    struct tt_InitOptions opts = defaults();
    opts.socket.drain_bytes = SMALL_DRAIN;
    opts.socket.drain_bytes_max = bytes_max;
    EXPECT_TRUE(drain_on(&opts));
    ctx.hal.drain_overran = true;
    (void)tt_rx_drain_start(&ctx, tt_CONTROL_MAX_LENGTH);
    uint32_t budget = ctx.hal.drain_budget;
    destroy();
    return budget;
}

static void test_drain_bytes_max_caps_the_growth(void) {
    EXPECT_EQ_U32(2U * SMALL_DRAIN, budget_after_an_overrun(0)); // four times the start: room to double
    EXPECT_EQ_U32(SMALL_DRAIN, budget_after_an_overrun(SMALL_DRAIN));
    EXPECT_EQ_U32(SMALL_DRAIN + 1000U, budget_after_an_overrun(SMALL_DRAIN + 1000U)); // doubled, then capped
}

// A datagram too long for the drain's buffers is cut, and no drain starts for drain_holdoff_ns after.
static uint64_t holdoff_after_a_cut(uint64_t holdoff_ns) {
    struct tt_InitOptions opts = defaults();
    opts.socket.drain_holdoff_ns = holdoff_ns;
    EXPECT_TRUE(drain_on(&opts));
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof(dest);
    EXPECT_TRUE(sender >= 0 && getsockname(ctx.hal.data_sock, (struct sockaddr*)&dest, &dest_len) == 0);
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static uint8_t payload[CUT_BYTES];
    memset(payload, 0xD5, sizeof(payload));
    EXPECT_TRUE(sendto(sender, payload, sizeof(payload), 0, (struct sockaddr*)&dest, sizeof(dest)) ==
                (ssize_t)sizeof(payload));
    (void)close(sender);
    static uint8_t buf[CUT_BYTES + 16];
    uint32_t ip = 0;
    uint16_t port = 0;
    const uint64_t before = tt_get_ns();
    uint64_t cut = ctx.hal.drain_truncated;
    for (int i = 0; i < 1000 && ctx.hal.drain_truncated == cut; i++) {
        (void)tt_try_receive(&ctx, buf, sizeof(buf), &ip, &port);
    }
    EXPECT_EQ_U64(cut + 1U, ctx.hal.drain_truncated);
    const uint64_t holdoff = ctx.hal.drain_holdoff_until - before;
    destroy();
    return holdoff;
}

static void test_drain_holdoff_is_the_wait_after_a_cut(void) {
    const uint64_t standard = holdoff_after_a_cut(0);
    const uint64_t shorter = holdoff_after_a_cut(SHORT_HOLDOFF);
    EXPECT_TRUE(standard >= tt_RX_DRAIN_HOLDOFF_NS && standard < tt_RX_DRAIN_HOLDOFF_NS + (50ULL * 1000000ULL));
    EXPECT_TRUE(shorter >= SHORT_HOLDOFF && shorter < SHORT_HOLDOFF + (50ULL * 1000000ULL));
}

// ---- poll.receive_timeout in the HAL: a socket skipped as idle is asked again after it.

static bool recheck_after(uint64_t receive_timeout, uint64_t elapsed) {
    clear_switch_env();
    struct tt_InitOptions opts = defaults();
    opts.poll.receive_timeout = receive_timeout;
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    ctx.hal.rx_idle = TT_RX_IDLE_DATA;
    ctx.hal.rx_idle_since_ns = 0;
    ctx.rx_clock_ns = tt_SECOND;
    rx_idle_count_return(&ctx); // the first return while skipping: the time noted
    ctx.rx_clock_ns = tt_SECOND + elapsed;
    rx_idle_count_return(&ctx);
    bool asked_again = ctx.hal.rx_idle == 0;
    ctx.rx_clock_ns = 0;
    destroy();
    return asked_again;
}

static void test_receive_timeout_is_the_idle_recheck(void) {
    EXPECT_TRUE(!recheck_after(0, 60ULL * 1000ULL)); // the default, 100 us: not yet
    EXPECT_TRUE(recheck_after(0, tt_RECEIVE_TIMEOUT));
    EXPECT_TRUE(recheck_after(RECHECK_SHORT, 60ULL * 1000ULL)); // 50 us: past it
    EXPECT_TRUE(!recheck_after(RECHECK_SHORT, 40ULL * 1000ULL));
}

// ---- The checks.

static void expect_created(const struct tt_InitOptions* opts, tt_ret_t expected, const char* what) {
    clear_switch_env();
    tt_ret_t ret = create(opts);
    if (ret != expected) {
        fprintf(stderr, "%s: returned %d, expected %d\n", what, (int)ret, (int)expected);
        test_failures++;
    }
    if (ret == tt_RET_OK) {
        destroy();
    }
}

static void test_validation(void) {
    struct tt_InitOptions opts = defaults();
    opts.socket.buffer_size = (uint32_t)INT32_MAX + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "socket.buffer_size above INT_MAX");
    opts.socket.buffer_size = (uint32_t)INT32_MAX;
    expect_created(&opts, tt_RET_OK, "socket.buffer_size INT_MAX");

    for (int which = 0; which < SWITCH_COUNT; which++) {
        opts = defaults();
        *switch_field(&opts, which) = tt_SWITCH_OFF + 1U;
        expect_created(&opts, tt_RET_INVALID_ARGUMENT, SWITCH_ENV[which]);
        *switch_field(&opts, which) = tt_SWITCH_OFF;
        expect_created(&opts, tt_RET_OK, SWITCH_ENV[which]);
    }

    opts = defaults();
    opts.socket.rx_hint = tt_SOCKET_RX_HINT_URING + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "socket.rx_hint unknown");

    opts = defaults();
    opts.socket.drain_buffers = 3;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "socket.drain_buffers not a power of two");
    opts.socket.drain_buffers = 2U * tt_SOCKET_DRAIN_BUFFERS_MAX;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "socket.drain_buffers above the limit");
    opts.socket.drain_buffers = tt_SOCKET_DRAIN_BUFFERS_MAX;
    expect_created(&opts, tt_RET_OK, "socket.drain_buffers at the limit");

    opts = defaults();
    opts.socket.drain_bytes = SMALL_DRAIN;
    opts.socket.drain_bytes_max = SMALL_DRAIN - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "socket.drain_bytes_max below drain_bytes");
    opts.socket.drain_bytes_max = SMALL_DRAIN;
    expect_created(&opts, tt_RET_OK, "socket.drain_bytes_max = drain_bytes");

    opts = defaults();
    opts.socket.drain_holdoff_ns = tt_SOCKET_DRAIN_HOLDOFF_MAX + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "socket.drain_holdoff_ns above the limit");
    opts.socket.drain_holdoff_ns = tt_SOCKET_DRAIN_HOLDOFF_MAX;
    expect_created(&opts, tt_RET_OK, "socket.drain_holdoff_ns at the limit");
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    (void)setenv("TICKLE_ID_REGISTRY", "off", 1);
    test_defaults_are_the_compiled_settings();
    test_buffer_size_is_what_both_sockets_ask();
    test_each_switch_and_its_environment_variable();
    test_rx_hint_decides_what_a_refusal_does();
    if (test_drain_sizes()) {
        test_drain_bytes_max_caps_the_growth();
        test_drain_holdoff_is_the_wait_after_a_cut();
    }
    test_receive_timeout_is_the_idle_recheck();
    test_validation();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_socket: all tests passed\n");
    return 0;
}
