/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "rosidl_runtime_c/string.h"

// Minimal stand-in matching the real rosidl_runtime_c__String__assign() contract closely enough
// to exercise the generated adapter's own logic (allocate + copy + NUL-terminate, true on
// success) - not a claim that this is byte-for-byte what the real implementation does internally.
static inline bool rosidl_runtime_c__String__assign(rosidl_runtime_c__String* str, const char* value) {
    size_t len = strlen(value);
    char* buf = (char*)malloc(len + 1);
    if (buf == NULL) {
        return false;
    }
    memcpy(buf, value, len + 1);
    free(str->data);
    str->data = buf;
    str->size = len;
    str->capacity = len + 1;
    return true;
}

// M7's own array-of-string ros2_adapter conversion needs the Sequence shape too - unlike a
// primitive Sequence (rosidl_runtime_c/primitives_sequence_functions.h), String gets this in its
// own dedicated header rather than a generic per-type one (real rosidl_runtime_c convention).
typedef struct rosidl_runtime_c__String__Sequence {
    rosidl_runtime_c__String* data;
    size_t size;
    size_t capacity;
} rosidl_runtime_c__String__Sequence;

// Every element must start as a valid, empty rosidl_runtime_c__String (data=NULL, size=0,
// capacity=0) - the same state String__assign() itself expects to find (it free()s whatever
// .data already held before allocating), so calloc's own zero-fill is exactly right here, not
// just a convenient shortcut.
static inline bool rosidl_runtime_c__String__Sequence__init(rosidl_runtime_c__String__Sequence* seq, size_t size) {
    seq->data = (rosidl_runtime_c__String*)calloc(size, sizeof(rosidl_runtime_c__String));
    if (seq->data == NULL && size > 0) {
        return false;
    }
    seq->size = size;
    seq->capacity = size;
    return true;
}
