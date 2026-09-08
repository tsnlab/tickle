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

#include <stdint.h>

#include <tickle/tickle.h>

struct SetBoolRequest {
    bool data;
};

struct SetBoolResponse {
    bool success;
    char* message;
};

extern struct tt_Service SetBoolService;

int32_t SetBoolRequest_encode_size(struct SetBoolRequest* request);
int32_t SetBoolRequest_encode(struct SetBoolRequest* request, uint8_t* payload, uint32_t len);
int32_t SetBoolRequest_decode(struct SetBoolRequest* request, const uint8_t* payload, uint32_t len,
                              bool is_native_endian);
void SetBoolRequest_free(struct SetBoolRequest* request);
int32_t SetBoolResponse_encode_size(struct SetBoolResponse* response);
int32_t SetBoolResponse_encode(struct SetBoolResponse* response, uint8_t* payload, uint32_t len);
int32_t SetBoolResponse_decode(struct SetBoolResponse* response, const uint8_t* payload, uint32_t len,
                               bool is_native_endian);
void SetBoolResponse_free(struct SetBoolResponse* response);
