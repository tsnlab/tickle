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
#include <stdint.h>

// Basic encoding/decoding utility functions
void* tt_encode_buffer(void* buffer, uint32_t* tail, uint32_t len);
void* tt_decode_buffer(void* buffer, uint32_t* head, uint32_t tail, uint32_t length);

// String encoding/decoding functions
bool tt_encode_string(void* buffer, uint32_t* tail, uint32_t buffer_size, const char* str);
bool tt_decode_string(void* buffer, uint32_t* head, uint32_t tail, uint16_t* str_len, char** str);
