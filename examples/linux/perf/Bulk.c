/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Generated code

#include "Bulk.h"

#include <stdint.h>
#include <string.h>

#include <tickle/hal.h>
#include <tickle/tickle.h>

struct tt_Topic BulkTopic = {
    .name = "BulkTopic",
    .data_size = sizeof(struct BulkData),
    .data_encode_size = (tt_DATA_ENCODE_SIZE)BulkData_encode_size,
    .data_encode = (tt_DATA_ENCODE)BulkData_encode,
    .data_decode = (tt_DATA_DECODE)BulkData_decode,
    .data_decode_inplace = (tt_DATA_DECODE_INPLACE)BulkData_decode_inplace,
    .data_free = (tt_DATA_FREE)BulkData_free,
};

int32_t BulkData_encode_size(struct BulkData* data) {
    return (int32_t)(sizeof(uint32_t) + sizeof(uint32_t) + data->size); // seq + size + bytes
}

int32_t BulkData_encode(struct BulkData* data, uint8_t* payload, uint32_t len) {
    int32_t encoded = 0;

    if (encoded + sizeof(uint32_t) > len) {
        return -1;
    }

    *(uint32_t*)payload = data->seq;

    encoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    if (encoded + sizeof(uint32_t) > len) {
        return -1;
    }

    *(uint32_t*)payload = data->size;

    encoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    if (encoded + data->size > len) {
        return -1;
    }

    memcpy(payload, data->bytes, data->size);

    encoded += (int32_t)data->size;
    payload += data->size;

    return encoded;
}

int32_t BulkData_decode(struct BulkData* data, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    int32_t decoded = 0;

    if (decoded + sizeof(uint32_t) > len) {
        return -1;
    }

    uint32_t seq = *(uint32_t*)payload;
    if (!is_native_endian) {
        seq = _tt_bswap_32(seq);
    }
    data->seq = seq;

    decoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    if (decoded + sizeof(uint32_t) > len) {
        return -1;
    }

    uint32_t size = *(uint32_t*)payload;
    if (!is_native_endian) {
        size = _tt_bswap_32(size);
    }

    decoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    // Malformed or from a peer with a larger BULK_MAX_PAYLOAD_SIZE than ours - refuse rather
    // than overflow `bytes`.
    if (size > BULK_MAX_PAYLOAD_SIZE || decoded + size > len) {
        return -1;
    }

    memcpy(data->bytes, payload, size);
    data->size = size;

    decoded += (int32_t)size;
    payload += size;

    return decoded;
}

struct BulkData* BulkData_decode_inplace(const uint8_t* payload, uint32_t len, bool is_native_endian) {
    // BulkData is { u32 seq; u32 size; u8 bytes[]; } with no padding, so a native-endian wire
    // payload [seq][size][bytes] already *is* a BulkData - just bounds-check and alias it. The
    // payload sits 24 bytes into rx_buffer (past tt_Header + SubmessageHeader + DataHeader),
    // which keeps the u32s 4-byte aligned. Byte-swapped wire can't be aliased -> NULL (copy path).
    if (!is_native_endian || len < 2 * sizeof(uint32_t)) {
        return NULL;
    }

    uint32_t size = ((const uint32_t*)payload)[1];
    if (size > BULK_MAX_PAYLOAD_SIZE || 2 * sizeof(uint32_t) + size > len) {
        return NULL;
    }

    return (struct BulkData*)(uintptr_t)payload;
}

void BulkData_free(struct BulkData* data) {
    (void)data;
    // Do nothing
}
