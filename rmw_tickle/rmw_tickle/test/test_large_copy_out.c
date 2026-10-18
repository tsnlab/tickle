/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// RMW_TICKLE_TAKE_COPY's copies (rmw_tickle_large_copy_out(), rmw_large.c): every mode puts exactly the source's bytes
// at the destination and writes nothing outside it, at every alignment of either end and at lengths around the
// 64-byte line and the 64 KiB reverse block - including the Array4m sample's own 4,194,304 + 8 bytes. And the switch
// parses as documented: only "nt", "rev" and "ntrev" choose a copy; anything else is the decoder.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rmw_tickle_c/rmw_tickle.h"

#define GUARD ((size_t)128)
#define GUARD_BYTE 0xA5U
#define PATTERN_MUL ((size_t)131) // a byte pattern with no short period, so a shifted copy cannot match
#define PATTERN_SHIFT 9U
#define DST_SKEW ((size_t)17) // destination starts 0, 17 and 34 bytes past an aligned one

static void check_one(uint8_t mode, size_t total, size_t src_off, size_t dst_off) {
    size_t span = total + (GUARD * 2U);
    uint8_t* src = (uint8_t*)malloc(span);
    uint8_t* dst = (uint8_t*)malloc(span);
    assert(NULL != src && NULL != dst);
    for (size_t i = 0; i < span; i++) {
        src[i] = (uint8_t)((i * PATTERN_MUL) ^ (i >> PATTERN_SHIFT));
    }
    memset(dst, (int)GUARD_BYTE, span);
    size_t len = total - src_off - dst_off;
    rmw_tickle_large_copy_out(dst + GUARD + dst_off, src + GUARD + src_off, len, mode);
    assert(0 == memcmp(dst + GUARD + dst_off, src + GUARD + src_off, len));
    for (size_t i = 0; i < GUARD + dst_off; i++) {
        assert(GUARD_BYTE == dst[i]);
    }
    for (size_t i = GUARD + dst_off + len; i < span; i++) {
        assert(GUARD_BYTE == dst[i]);
    }
    free(src);
    free(dst);
}

int main(void) {
    const uint8_t modes[] = {RMW_TICKLE_TAKE_COPY_NT, RMW_TICKLE_TAKE_COPY_REV, RMW_TICKLE_TAKE_COPY_NTREV};
    const size_t lengths[] = {
        64, 65, 127, 128, 200, 4099, 65536, 65537, 196685, 1048589, 4194312,
    };
    for (size_t mi = 0; mi < sizeof(modes) / sizeof(modes[0]); mi++) {
        for (size_t li = 0; li < sizeof(lengths) / sizeof(lengths[0]); li++) {
            for (size_t src_off = 0; src_off < 3; src_off++) {
                for (size_t skew = 0; skew < 3; skew++) {
                    size_t dst_off = skew * DST_SKEW;
                    check_one(modes[mi], lengths[li] + src_off + dst_off, src_off, dst_off);
                }
            }
        }
        check_one(modes[mi], 0, 0, 0); // nothing to copy: nothing written
        check_one(modes[mi], 1, 0, 0);
    }

    assert(RMW_TICKLE_TAKE_COPY_DECODE == rmw_tickle_take_copy_mode()); // unset
    (void)setenv("RMW_TICKLE_TAKE_COPY", "nt", 1);
    assert(RMW_TICKLE_TAKE_COPY_NT == rmw_tickle_take_copy_mode());
    (void)setenv("RMW_TICKLE_TAKE_COPY", "rev", 1);
    assert(RMW_TICKLE_TAKE_COPY_REV == rmw_tickle_take_copy_mode());
    (void)setenv("RMW_TICKLE_TAKE_COPY", "ntrev", 1);
    assert(RMW_TICKLE_TAKE_COPY_NTREV == rmw_tickle_take_copy_mode());
    (void)setenv("RMW_TICKLE_TAKE_COPY", "1", 1);
    assert(RMW_TICKLE_TAKE_COPY_DECODE == rmw_tickle_take_copy_mode());
    (void)setenv("RMW_TICKLE_TAKE_COPY", "", 1);
    assert(RMW_TICKLE_TAKE_COPY_DECODE == rmw_tickle_take_copy_mode());
    (void)printf("test_large_copy_out: every mode, length and alignment copied exactly, nothing outside written\n");
    return 0;
}
