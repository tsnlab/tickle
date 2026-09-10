/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include "encoding.h"

#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "log.h"

// Endian checking functions
bool tt_is_native_endian(struct tt_Header* header) {
    return header->magic_value == NATIVE_MAGIC_VALUE;
}

bool tt_is_reverse_endian(struct tt_Header* header) {
    return header->magic_value == REVERSE_MAGIC_VALUE;
}

// Endpoint id = FNV-1a over the bytes of `type` then a '/' separator then `name`. Byte-oriented
// on purpose: the result is identical on any host regardless of byte order (two nodes of opposite
// endianness must agree on the id for the same names, or discovery never matches them) and it
// needs no aligned word access (the previous version cast the string to uint32_t*, which faults
// on strict-alignment targets). The '/' keeps "ab"+"c" from colliding with "a"+"bc".
uint32_t tt_hash_id(const char* type, const char* name) {
    size_t type_len = _tt_strnlen(type, tt_MAX_NAME_LENGTH + 1);
    size_t name_len = _tt_strnlen(name, tt_MAX_NAME_LENGTH + 1);

    if (type_len > tt_MAX_NAME_LENGTH) {
        TT_LOG_WARNING("Length of \"%s\" exceeds maximum string length %u", type, tt_MAX_NAME_LENGTH);
        type_len = tt_MAX_NAME_LENGTH;
    }
    if (name_len > tt_MAX_NAME_LENGTH) {
        TT_LOG_WARNING("Length of \"%s\" exceeds maximum string length %u", name, tt_MAX_NAME_LENGTH);
        name_len = tt_MAX_NAME_LENGTH;
    }

    uint32_t hash = 2166136261u; // FNV-1a 32-bit offset basis
    for (size_t i = 0; i < type_len; i++) {
        hash = (hash ^ (uint8_t)type[i]) * 16777619u;
    }
    hash = (hash ^ (uint8_t)'/') * 16777619u;
    for (size_t i = 0; i < name_len; i++) {
        hash = (hash ^ (uint8_t)name[i]) * 16777619u;
    }
    return hash;
}

// Basic encoding/decoding utility functions
void* tt_encode_buffer(void* buffer, uint32_t* tail, uint32_t len) {
    uint8_t* buf = (uint8_t*)buffer + *tail;
    *tail += len;
    return buf;
}

void* tt_decode_buffer(void* buffer, uint32_t* head, uint32_t tail, uint32_t length) {
    if (*head + length > tail) {
        return NULL;
    }

    void* p = (uint8_t*)buffer + *head;
    *head += length;
    return p;
}

// String encoding/decoding functions
bool tt_encode_string(void* buffer, uint32_t* tail, uint32_t buffer_size, const char* str) {
    if (str == NULL) {
        return false; // a caller-supplied name/type string should never be NULL; fail, don't deref
    }
    size_t str_len = _tt_strnlen(str, tt_MAX_STRING_LENGTH) + 1; // including '\0'

    // str_len always >= 0

    if (*tail + sizeof(uint16_t) + str_len >= buffer_size) {
        return false;
    }

    uint16_t len16 = (uint16_t)str_len; // native byte order - the peer swaps if it reads reverse
    _tt_memcpy((uint8_t*)buffer + *tail, &len16, sizeof(len16));
    *tail += sizeof(uint16_t);

    _tt_memcpy((uint8_t*)buffer + *tail, str, str_len);
    *tail += str_len;

    return true;
}

bool tt_decode_string(void* buffer, uint32_t* head, uint32_t tail, uint16_t* str_len, char** str, bool reverse) {
    if (*head + sizeof(uint16_t) > tail) {
        return false;
    }

    uint16_t raw_len = 0;
    _tt_memcpy(&raw_len, (uint8_t*)buffer + *head, sizeof(raw_len));
    *str_len = reverse ? _tt_bswap_16(raw_len) : raw_len; // length was written in the sender's byte order
    *head += sizeof(uint16_t);

    if (*head + *str_len > tail) {
        return false;
    }

    // str_len is defined to include the terminating '\0' (see tt_encode_string); reject any
    // string that doesn't actually end with one so callers can safely treat *str as a C string.
    if (*str_len == 0 || ((const char*)buffer)[*head + *str_len - 1] != '\0') {
        return false;
    }

    *str = (char*)((uint8_t*)buffer + *head);
    *head += *str_len;

    return true;
}
