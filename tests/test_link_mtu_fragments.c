/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Link profiles (docs/DESIGN.md "Link profiles"): a link MTU is refused when the build's largest sample would need more
// fragments at it than a fragment's 8-bit index names (tt_FRAG_MAX_COUNT), and an interface MTU that small, taken by a
// T1S or TSN link, falls back to 1500. Built with a tt_MAX_SAMPLE_LENGTH large enough for that bound to lie above
// tt_LINK_MTU_MIN - at the other test binaries' sizes no MTU the minimum allows reaches it
// (tests/mutants_link_profiles.sh removes the check and expects this to fail).

#define tt_MAX_SAMPLE_LENGTH 60000

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_link_profiles.sh passes a mutant

static struct tt_Context created;

static tt_ret_t create(uint8_t profile, uint16_t mtu, int32_t interface_mtu) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    test_mock_link_mtu = interface_mtu;
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255", .profile = profile, .mtu = mtu};
    opts.net.link_count = 1;
    memset(&created, 0, sizeof(created));
    return tt_Context_create_with(&created, &opts);
}

static void test_an_mtu_too_small_to_fragment_every_sample(void) {
    uint32_t least = tt_LINK_MTU_MIN;
    while (!link_control_fragments(link_control_length(least))) {
        least++;
    }
    EXPECT_TRUE(least > tt_LINK_MTU_MIN); // the bound lies inside what the minimum allows, so the check decides
    // The cut at the least MTU takes tt_MAX_SAMPLE_LENGTH in tt_FRAG_MAX_COUNT fragments; one byte less, one more.
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create(tt_LINK_PROFILE_ETHERNET, (uint16_t)(least - 1U), -1));
    EXPECT_EQ_INT(tt_RET_OK, create(tt_LINK_PROFILE_ETHERNET, (uint16_t)least, -1));
    EXPECT_EQ_U32(least - 28U, created.control_max);
    EXPECT_TRUE(frag_count_for(&created, tt_MAX_SAMPLE_LENGTH + 3U) <= tt_FRAG_MAX_COUNT);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));

    // Taken from the interface: the same MTU falls back to 1500, with a warning, rather than refusing the context.
    EXPECT_EQ_INT(tt_RET_OK, create(tt_LINK_PROFILE_T1S, 0, (int32_t)least - 1));
    EXPECT_EQ_U32(1500, created.opts.net.links[0].mtu);
    EXPECT_EQ_U32(tt_CONTROL_MAX_LENGTH, created.control_max);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));
    EXPECT_EQ_INT(tt_RET_OK, create(tt_LINK_PROFILE_T1S, 0, (int32_t)least));
    EXPECT_EQ_U32(least, created.opts.net.links[0].mtu);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_an_mtu_too_small_to_fragment_every_sample();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_link_mtu_fragments: all tests passed\n");
    return 0;
}
