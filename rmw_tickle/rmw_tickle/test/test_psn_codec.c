/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle's per-message psn header since tt_VERSION 10 (WIRE_PLAN.md W3): one 32-bit word below 2^31,
// two past it, in the sender's byte order. Every psn round-trips exactly, from either byte order - across
// the 16-bit wrap that 43d49fa8 fixed, at the edge of each form, and at the top of the range - and a
// header cut short is refused rather than misread.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rmw_tickle_c/rmw_tickle.h"

static void swap_words(uint8_t* bytes, uint32_t length) {
    for (uint32_t offset = 0; offset + sizeof(uint32_t) <= length; offset += sizeof(uint32_t)) {
        uint32_t word = 0;
        memcpy(&word, bytes + offset, sizeof(word));
        word = __builtin_bswap32(word);
        memcpy(bytes + offset, &word, sizeof(word));
    }
}

int main(void) {
    static const uint64_t psns[] = {1ULL,
                                    65535ULL,
                                    65536ULL,
                                    65537ULL,
                                    (uint64_t)RMW_TICKLE_PSN_LONG_FLAG - 1ULL,
                                    (uint64_t)RMW_TICKLE_PSN_LONG_FLAG,
                                    (1ULL << 32U) + 5ULL,
                                    (1ULL << 63U) - 1ULL};
    for (size_t i = 0; i < sizeof(psns) / sizeof(psns[0]); i++) {
        uint8_t bytes[RMW_TICKLE_PSN_BYTES] = {0};
        uint32_t written = rmw_tickle_psn_write(psns[i], bytes);
        assert(written == rmw_tickle_psn_bytes(psns[i]));
        assert(written == (psns[i] < RMW_TICKLE_PSN_LONG_FLAG ? RMW_TICKLE_PSN_SHORT_BYTES : RMW_TICKLE_PSN_BYTES));
        uint64_t read = 0;
        assert(written == rmw_tickle_psn_read(bytes, sizeof(bytes), true, &read));
        assert(read == psns[i]);
        swap_words(bytes, written); // as a sender of the other byte order wrote it
        read = 0;
        assert(written == rmw_tickle_psn_read(bytes, sizeof(bytes), false, &read));
        assert(read == psns[i]);
        swap_words(bytes, written);
        assert(0 == rmw_tickle_psn_read(bytes, written - 1U, true, &read)); // cut short: refused
    }
    printf("psn header codec: PASS\n");
    return 0;
}
