/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Hand-written, not `tools/typesupport`-generated (unlike examples/uint64/UInt64.h) - mirrors
// examples/perf_hil/idl/Bench.idl's exact field shape (rmw_tickle/comparison.md's own HIL
// 3-way QoS-matrix comparison, "Planned" section) so this native-TickLE example, the
// FastDDS/CycloneDDS native examples, and rmw_perf_pingpong's own ROS 2 message can all be
// compared on literally the same wire payload, not just "a similar-sized one." Follows
// examples/uint64/UInt64.h's own established codec pattern exactly (one encode/decode block per
// field, explicit bounds checks, byteswap on decode when `!is_native_endian`).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <tickle/config.h> // tt_MAX_BUFFER_LENGTH
#include <tickle/tickle.h>

#pragma pack(push, 4)
struct BenchData {
    uint32_t seq;
    uint64_t send_ns;
    uint8_t payload[64];
};
#pragma pack(pop)

int32_t BenchData_encode_size(struct BenchData* data);
int32_t BenchData_encode(struct BenchData* data, uint8_t* payload, uint32_t len);
int32_t BenchData_decode(struct BenchData* data, const uint8_t* payload, uint32_t len, bool is_native_endian);
int32_t BenchData_encode_inplace(struct BenchData* data, const uint8_t** payload_out);
struct BenchData* BenchData_decode_inplace(const uint8_t* payload, uint32_t len, bool is_native_endian);
void BenchData_free(struct BenchData* data);

_Static_assert(sizeof(struct BenchData) == 76, "BenchData must match its CDR-4 wire size - ABI mismatch");
_Static_assert(76 <= tt_MAX_BUFFER_LENGTH, "BenchData's worst-case wire size exceeds a single datagram");

extern struct tt_Topic BenchTopic;
