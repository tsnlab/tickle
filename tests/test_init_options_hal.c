/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions on the REAL Linux HAL (bundle 1): the net options the HAL reads - addr, broadcast and port -
// come from each context's own options, so two contexts in one process bind two different well-known ports and two
// different data addresses at once. With the process-wide _tt_CONFIG both would have bound whatever it held when each
// was created, and every later read (the registry path, the broadcast destination) the value it held then.
//
// Loopback only, and nothing is sent: both contexts are created and destroyed without a poll, their broadcast address
// is 127.255.255.255, and the id registry is off (TICKLE_ID_REGISTRY=off).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier) - the real HAL below needs ppoll() declared
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "../src/hal_linux.c" // NOLINT(bugprone-suspicious-include) - the real HAL, deliberately
#include "../src/tickle.c"    // NOLINT(bugprone-suspicious-include) -- whitebox, as the other unit tests
#include "test_common.h"

#define PORT_A 47311
#define PORT_B 47312
#define LOOPBACK_BROADCAST "127.255.255.255"
#define LOOPBACK_IP 0x7F000001U

static struct tt_Context a;
static struct tt_Context b;

static bool bound_to(int sock, uint32_t* ip, uint16_t* port) {
    struct sockaddr_in addr;
    socklen_t length = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    if (sock < 0 || getsockname(sock, (struct sockaddr*)&addr, &length) != 0) {
        return false;
    }
    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);
    return true;
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    (void)setenv("TICKLE_ID_REGISTRY", "off", 1);

    struct tt_InitOptions opts_a;
    tt_InitOptions_default(&opts_a);
    opts_a.net.addr = "127.0.0.1";
    opts_a.net.broadcast = LOOPBACK_BROADCAST;
    opts_a.net.port = PORT_A;
    opts_a.net.context_id = 21;
    struct tt_InitOptions opts_b;
    tt_InitOptions_default(&opts_b); // addr stays the default, any address
    opts_b.net.broadcast = LOOPBACK_BROADCAST;
    opts_b.net.port = PORT_B;
    opts_b.net.context_id = 22;

    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&a, &opts_a));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&b, &opts_b));

    uint32_t ip = 0;
    uint16_t port = 0;
    // port: each context's well-known socket on its own port.
    EXPECT_TRUE(bound_to(a.hal.sock, &ip, &port));
    EXPECT_EQ_U32(PORT_A, port);
    EXPECT_EQ_U32(0, ip); // the well-known socket always binds the wildcard (hal_linux.c's tt_bind())
    EXPECT_TRUE(bound_to(b.hal.sock, &ip, &port));
    EXPECT_EQ_U32(PORT_B, port);
    // addr: the data socket bound to the context's own address - loopback for one, any for the other.
    EXPECT_TRUE(bound_to(a.hal.data_sock, &ip, &port));
    EXPECT_EQ_U32(LOOPBACK_IP, ip);
    EXPECT_TRUE(bound_to(b.hal.data_sock, &ip, &port));
    EXPECT_EQ_U32(0, ip);
    // broadcast and port: where each one's broadcast goes.
    EXPECT_EQ_U32(inet_addr(LOOPBACK_BROADCAST), a.hal.broadcast_addr.sin_addr.s_addr);
    EXPECT_EQ_U32(PORT_A, ntohs(a.hal.broadcast_addr.sin_port));
    EXPECT_EQ_U32(PORT_B, ntohs(b.hal.broadcast_addr.sin_port));
    // And the deprecated global was not what either read: it still holds its defaults.
    EXPECT_EQ_INT(_tt_CONTEXT_PORT, _tt_CONFIG.port);
    EXPECT_TRUE(strcmp(_tt_CONFIG.broadcast, _tt_CONTEXT_BROADCAST) == 0);

    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&a));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&b));

    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_hal: two contexts bound ports %d and %d and data addresses 127.0.0.1 and 0.0.0.0 at "
           "once, each from its own options\n",
           PORT_A, PORT_B);
    return 0;
}
