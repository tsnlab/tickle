/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// -K <depth> for the BEST_EFFORT throughput benches of all three frameworks: the KEEP_LAST depth of the DDS writer
// AND reader (docs/TESTING.md section 4: every QoS value passed explicitly, the same on every framework). Header-only
// and plain C, included by all three frameworks' clients and servers like BenchWindow.h, so the parse and the echo
// cannot drift apart between them.
//
// WHY IT EXISTS (ROADMAP "Fill COMPARISON 1a's empty cells", 2026-10-06). The BEST_EFFORT benches never set a
// history, so both DDS vendors ran their default, KEEP_LAST 1, on writer and reader alike. On one host FastDDS's
// writer then overwrites what its reader has not taken: in mixed_delivery.sh's LOCAL_ONLY arm the subscriber took 154
// of 3.1 million samples. TickLE has no history cache at all - its BEST_EFFORT subscriber is a callback, and what
// queues between the two processes is the receiver's segment ring (tt_SEGMENT_SLOTS datagrams; the writer drops at a
// full ring, shm_full_dropped=). Which depth is the fair one for the vendors is the user's decision, so this is a flag
// and its default is what the benches did before it existed:
//   no -K      each product's own default history (KEEP_LAST 1 on both DDS vendors), as every published S1-S3 row
//   -K <d>     KEEP_LAST d on the vendors' writer and reader; TickLE accepts and echoes it and has nothing to apply
// Every RESULT line echoes history_arg= (what was asked) and history= (what the process actually runs: a vendor reads
// it back from the entity's own QoS, TickLE prints none and its ring size), so a row that ran something other than
// what it was asked for can be refused by the harness rather than averaged in.

#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 0: -K was not given and nothing is set. -1: -K was given and is not a positive integer; a bench then applies
// nothing and echoes history_arg=invalid, which no harness accepts.
#define BENCH_HISTORY_DEFAULT 0
#define BENCH_HISTORY_INVALID (-1)
#define BENCH_HISTORY_FIELD_MAX 64
#define BENCH_HISTORY_DEPTH_MAX 1000000L

// Returns true if argv[*idx] was -K, having consumed its value.
static inline bool BenchHistory_parse_arg(int* depth, int argc, char** argv, int* idx) {
    if (*idx + 1 >= argc || strcmp(argv[*idx], "-K") != 0) {
        return false;
    }
    const char* text = argv[++*idx];
    char* end = NULL;
    long value = strtol(text, &end, 10);
    *depth = (end != text && *end == '\0' && value >= 1 && value <= BENCH_HISTORY_DEPTH_MAX) ? (int)value
                                                                                             : BENCH_HISTORY_INVALID;
    return true;
}

// "history_arg=default", "history_arg=<d>" or "history_arg=invalid".
static inline const char* BenchHistory_arg_field(int depth, char* out, size_t out_len) {
    if (depth == BENCH_HISTORY_DEFAULT) {
        snprintf(out, out_len, "history_arg=default");
    } else if (depth == BENCH_HISTORY_INVALID) {
        snprintf(out, out_len, "history_arg=invalid");
    } else {
        snprintf(out, out_len, "history_arg=%d", depth);
    }
    return out;
}
