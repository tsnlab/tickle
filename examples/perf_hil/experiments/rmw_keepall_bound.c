/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_keepall_bound.c - what one rmw_tickle KEEP_ALL sample of a bounded type costs, in the units its bound is
// counted in, from core's own sizing functions (tt_sample_datagrams(), tt_sample_cache_bytes()) rather than a copy
// of their arithmetic. rmw_keepall_rig.sh's EQUAL_BOUND mode compiles it on the client Pi against the tree it
// built, so the numbers are that build's.
//
// Build (one line): gcc -I include -Dtt_MAX_BUFFER_LENGTH=<the typesupport's tickle_max_buffer_length>
//                   -o rmw_keepall_bound examples/perf_hil/experiments/rmw_keepall_bound.c
// Usage:  rmw_keepall_bound <encoded bytes> <psn bytes sent> <psn bytes reserved> <tracking words>
//   encoded bytes   the type's tickle_max_encoded_size (a bounded type's every sample is that size)
//   psn bytes sent  RMW_TICKLE_PSN_SHORT_BYTES: the header every sample of a run below 2^31 samples carries
//   psn reserved    RMW_TICKLE_PSN_BYTES: what clamp_record_bytes() (rmw_publisher.c) reserves per sample
//   tracking words  RMW_TICKLE_TRACKING_WORDS: a subscription's announced window, in 64-bit words
// Prints one line of key=value:
//   datagrams      datagrams (and so seq_no) one sample takes
//   footprint      arena bytes one sample actually takes (tt_sample_cache_bytes of what is sent)
//   reserved       clamp_record_bytes(): the record rmw_tickle sizes the arena with (never below one of these)
//   window_samples the subscription's window in samples: the count bound keep_all_bound() (tickle.c) applies
//   depth_samples  the publisher's KEEP_ALL ring (RMW_TICKLE_KEEP_ALL_DEPTH_VOLATILE) in samples

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <tickle/config.h>
#include <tickle/tickle.h>

static int parse(const char* text, unsigned long* out) {
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > (unsigned long)tt_MAX_SAMPLE_LENGTH) {
        return 0;
    }
    *out = value;
    return 1;
}

int main(int argc, char** argv) {
    unsigned long encoded = 0;
    unsigned long psn_sent = 0;
    unsigned long psn_reserved = 0;
    unsigned long words = 0;
    if (argc != 5 || !parse(argv[1], &encoded) || !parse(argv[2], &psn_sent) || !parse(argv[3], &psn_reserved) ||
        !parse(argv[4], &words)) {
        fprintf(stderr, "usage: %s <encoded bytes> <psn bytes sent> <psn bytes reserved> <tracking words>\n", argv[0]);
        return 2;
    }
    const uint32_t sent = (uint32_t)(encoded + psn_sent);
    const uint32_t datagrams = tt_sample_datagrams(sent);
    const uint32_t footprint = tt_sample_cache_bytes(sent);
    const uint32_t reserved = tt_sample_cache_bytes((uint32_t)(encoded + psn_reserved));
    const uint32_t window = (uint32_t)words * tt_RELIABLE_BITMAP_WORD_BITS;
    const uint32_t ring = 2U * window; // RMW_TICKLE_KEEP_ALL_DEPTH_VOLATILE (rmw_publisher.c)
    printf("datagrams=%u footprint=%u reserved=%u window_samples=%u depth_samples=%u control_max=%u\n",
           (unsigned)datagrams, (unsigned)footprint, (unsigned)reserved, (unsigned)(window / datagrams),
           (unsigned)(ring / datagrams), (unsigned)tt_CONTROL_MAX_LENGTH);
    return 0;
}
