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
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct rosidl_runtime_c__uint16__Sequence {
    uint16_t* data;
    size_t size;
    size_t capacity;
} rosidl_runtime_c__uint16__Sequence;

static inline bool rosidl_runtime_c__uint16__Sequence__init(rosidl_runtime_c__uint16__Sequence* seq, size_t size) {
    seq->data = (uint16_t*)malloc(size * sizeof(uint16_t));
    if (seq->data == NULL && size > 0) {
        return false;
    }
    seq->size = size;
    seq->capacity = size;
    return true;
}

typedef struct rosidl_runtime_c__float32__Sequence {
    float* data;
    size_t size;
    size_t capacity;
} rosidl_runtime_c__float32__Sequence;

static inline bool rosidl_runtime_c__float32__Sequence__init(rosidl_runtime_c__float32__Sequence* seq, size_t size) {
    seq->data = (float*)malloc(size * sizeof(float));
    if (seq->data == NULL && size > 0) {
        return false;
    }
    seq->size = size;
    seq->capacity = size;
    return true;
}

typedef struct rosidl_runtime_c__uint8__Sequence {
    uint8_t* data;
    size_t size;
    size_t capacity;
} rosidl_runtime_c__uint8__Sequence;

static inline bool rosidl_runtime_c__uint8__Sequence__init(rosidl_runtime_c__uint8__Sequence* seq, size_t size) {
    seq->data = (uint8_t*)malloc(size * sizeof(uint8_t));
    if (seq->data == NULL && size > 0) {
        return false;
    }
    seq->size = size;
    seq->capacity = size;
    return true;
}

/* Milestone 55: a real ROS 2 install declares this too (rosidl_runtime_c's own generic per-
 * primitive-type Sequence macro instantiates it for `char` the same as every other primitive) -
 * deliberately a *decoy* here, never the type a real IDL `char[]`/`char[<=N]` field's own struct
 * member actually is (rosidl_generator_c represents that as rosidl_runtime_c__uint8__Sequence
 * instead - see ros2_adapter._ros2_sequence_scalar_type()'s own doc comment for the full story).
 * Declaring both here, exactly like a real ROS 2 install's own headers do, is what lets this
 * fixture catch a regression the same way the real bug was actually found: calling the wrong one
 * is a hard pointer-type-mismatch compile error, not a silently-accepted no-op. */
typedef struct rosidl_runtime_c__char__Sequence {
    char* data;
    size_t size;
    size_t capacity;
} rosidl_runtime_c__char__Sequence;

static inline bool rosidl_runtime_c__char__Sequence__init(rosidl_runtime_c__char__Sequence* seq, size_t size) {
    seq->data = (char*)malloc(size * sizeof(char));
    if (seq->data == NULL && size > 0) {
        return false;
    }
    seq->size = size;
    seq->capacity = size;
    return true;
}
