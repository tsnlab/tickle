/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions, bundle 6: socket.rx_hint in a build with no io_uring (tt_HAL_RX_HINT=tt_RX_HINT_READ). The
// option chooses only between AUTO and URING, so here the default resolves to AUTO - what such a build does, a read
// per check - and URING, which it cannot honour, is refused. Through the mock HAL;
// tests/test_init_options_socket.c has the rest of the group on the real one.

#define tt_HAL_RX_HINT 1 // tt_RX_HINT_READ, which hal_linux.h defines after this

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_socket.sh passes a mutant

static tt_ret_t create(const struct tt_InitOptions* opts) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    struct tt_Context node;
    memset(&node, 0, sizeof(node));
    tt_ret_t ret = tt_Context_create_with(&node, opts);
    if (ret == tt_RET_OK) {
        EXPECT_EQ_U32(tt_SOCKET_RX_HINT_AUTO, node.opts.socket.rx_hint);
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
    }
    return ret;
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    EXPECT_EQ_INT(0, tt_HAL_IO_URING); // the build this test is about

    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    EXPECT_EQ_U32(tt_SOCKET_RX_HINT_AUTO, opts.socket.rx_hint);
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    opts.socket.rx_hint = tt_SOCKET_RX_HINT_URING;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create(&opts));

    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_socket_noring: a build without io_uring resolves rx_hint to AUTO and refuses URING\n");
    return 0;
}
