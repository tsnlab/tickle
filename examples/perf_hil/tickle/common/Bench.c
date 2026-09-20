/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Hand-written, matching examples/uint64/UInt64.c's own generated-code pattern exactly - see
// Bench.h's own doc comment for why this one isn't run through tools/typesupport.

#include "Bench.h"

#include <stdint.h>
#include <string.h> // memcpy

#include <tickle/hal.h>
#include <tickle/tickle.h>

// Matches struct BenchData's own asserted wire size and payload field size (Bench.h).
static const int32_t bench_data_wire_size = 76;
static const uint32_t bench_payload_size = 64;

struct tt_Topic BenchTopic = {
    .name = "BenchTopic",
    .data_size = sizeof(struct BenchData),
    .data_encode_size = (tt_DATA_ENCODE_SIZE)BenchData_encode_size,
    .data_encode = (tt_DATA_ENCODE)BenchData_encode,
    .data_encode_inplace = (tt_DATA_ENCODE_INPLACE)BenchData_encode_inplace,
    .data_decode = (tt_DATA_DECODE)BenchData_decode,
    .data_decode_inplace = (tt_DATA_DECODE_INPLACE)BenchData_decode_inplace,
    .data_free = (tt_DATA_FREE)BenchData_free,
};

int32_t BenchData_encode_size(struct BenchData* data) {
    (void)data;
    return bench_data_wire_size;
}

int32_t BenchData_encode(struct BenchData* data, uint8_t* payload, uint32_t len) {
    int32_t encoded = 0;
    if ((uint32_t)encoded + 4 > len) {
        return -1;
    }
    *(uint32_t*)(payload + encoded) = data->seq;
    encoded += 4;

    if ((uint32_t)encoded + 8 > len) {
        return -1;
    }
    *(uint64_t*)(payload + encoded) = data->send_ns;
    encoded += 8;

    if ((uint32_t)encoded + bench_payload_size > len) {
        return -1;
    }
    memcpy(payload + encoded, data->payload, bench_payload_size);
    encoded += (int32_t)bench_payload_size;

    return encoded;
}

int32_t BenchData_decode(struct BenchData* data, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    int32_t decoded = 0;

    if ((uint32_t)decoded + 4 > len) {
        return -1;
    }
    uint32_t seq_raw = *(const uint32_t*)(payload + decoded);
    if (!is_native_endian) {
        seq_raw = _tt_bswap_32(seq_raw);
    }
    data->seq = seq_raw;
    decoded += 4;

    if ((uint32_t)decoded + 8 > len) {
        return -1;
    }
    uint64_t send_ns_raw = *(const uint64_t*)(payload + decoded);
    if (!is_native_endian) {
        send_ns_raw = _tt_bswap_64(send_ns_raw);
    }
    data->send_ns = send_ns_raw;
    decoded += 8;

    if ((uint32_t)decoded + bench_payload_size > len) {
        return -1;
    }
    memcpy(data->payload, payload + decoded, bench_payload_size);
    decoded += (int32_t)bench_payload_size;

    return decoded;
}

int32_t BenchData_encode_inplace(struct BenchData* data, const uint8_t** payload_out) {
    *payload_out = (const uint8_t*)data;
    return bench_data_wire_size;
}

struct BenchData* BenchData_decode_inplace(const uint8_t* payload, uint32_t len, bool is_native_endian) {
    if (!is_native_endian || len < (uint32_t)bench_data_wire_size) {
        return NULL;
    }
    return (struct BenchData*)payload;
}

void BenchData_free(struct BenchData* data) {
    (void)data;
}
