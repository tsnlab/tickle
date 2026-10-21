/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions in a build without tt_DISCOVERY_OPTIONS (core's default): a discovery range other than SUBNET
// and a peer link are options this build cannot honour, so tt_Context_create_with() refuses them rather than running
// without them - a setting silently ignored reads like one that had no effect. tests/test_init_options.c is the build
// that has them. Nor the hybrid transport: its options set are refused too (test_init_options_hybrid.c has it).

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options.sh passes a mutant

#if tt_DISCOVERY_OPTIONS
#error "this test is about a build without tt_DISCOVERY_OPTIONS"
#endif
#if tt_HYBRID_TRANSPORT
#error "this test is about a build without the hybrid transport"
#endif

static struct tt_Context ctx;

static tt_ret_t create_with(const struct tt_InitOptions* opts) {
    test_mock_reset();
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    memset(&ctx, 0, sizeof(ctx));
    tt_ret_t ret = tt_Context_create_with(&ctx, opts);
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
    }
    return ret;
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    struct tt_InitOptions opts;

    tt_InitOptions_default(&opts);
    EXPECT_EQ_INT(tt_RET_OK, create_with(&opts)); // the control: the defaults are honoured

    opts.net.discovery_range = tt_DISCOVERY_RANGE_LOCALHOST;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create_with(&opts));
    opts.net.discovery_range = tt_DISCOVERY_RANGE_OFF;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create_with(&opts));

    tt_InitOptions_default(&opts);
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255"};
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.0.9"};
    opts.net.link_count = 2;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&opts)); // the control: the same links, neither a peer
    opts.net.links[1].peer = true;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create_with(&opts));

    // Nor has it the hybrid transport (no fragmentation): its options, all 0 by default, are refused when set.
    tt_InitOptions_default(&opts);
    EXPECT_EQ_INT(0, opts.hybrid_transport.n_max);
    opts.hybrid_transport.n_max = 1;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create_with(&opts));
    tt_InitOptions_default(&opts);
    opts.hybrid_transport.path_alpha = 1e-3;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create_with(&opts));

    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_noflag: all tests passed\n");
    return 0;
}
