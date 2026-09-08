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
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <tickle/tickle.h>

struct UInt64Data {
    uint64_t data;
};

extern struct tt_Topic UInt64Topic;

int32_t UInt64Data_encode_size(struct UInt64Data* data);
int32_t UInt64Data_encode(struct UInt64Data* data, uint8_t* payload, uint32_t len);
int32_t UInt64Data_decode(struct UInt64Data* data, const uint8_t* payload, uint32_t len, bool is_native_endian);
void UInt64Data_free(struct UInt64Data* data);
