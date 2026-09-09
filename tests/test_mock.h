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

// Fake HAL + clock so tests can #include "../src/tickle.c" directly (to reach its static
// functions) and link without pulling in the real hal_linux.c - no socket, no real time.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <tickle/config.h>
#include <tickle/tickle.h>

// Defined in exactly one place per test binary (see test_common.h's DEFINE_STORAGE convention).
#ifdef TEST_MOCK_DEFINE_STORAGE
struct _tt_Config _tt_CONFIG = {
    .addr = _tt_NODE_ADDRESS,
    .port = _tt_NODE_PORT,
    .broadcast = _tt_NODE_BROADCAST,
    .node_id = tt_NODE_ID_INVALID, // unused by the mock HAL (test_mock_node_id drives it instead)
};

// Defaults: a quiet node - no incoming packets, sends "succeed", clock starts at 0.
uint64_t test_mock_now = 0;
int32_t test_mock_node_id = 1;
tt_ret_t test_mock_bind_return = tt_RET_OK;
int32_t test_mock_receive_return = -1;
bool test_mock_send_return_override = false;
int32_t test_mock_send_return = 0;
int test_mock_send_call_count = 0;
#else
extern uint64_t test_mock_now;
extern int32_t test_mock_node_id;
extern tt_ret_t test_mock_bind_return;
extern int32_t test_mock_receive_return;
extern bool test_mock_send_return_override;
extern int32_t test_mock_send_return;
extern int test_mock_send_call_count;
#endif

// Call at the start of each test case so one test's overrides can't leak into the next.
static inline void test_mock_reset(void) {
    test_mock_now = 0;
    test_mock_node_id = 1;
    test_mock_bind_return = tt_RET_OK;
    test_mock_receive_return = -1;
    test_mock_send_return_override = false;
    test_mock_send_return = 0;
    test_mock_send_call_count = 0;
}

#ifdef TEST_MOCK_DEFINE_STORAGE
// These replace the real platform HAL symbols (normally hal_linux.c) in a test binary.
uint64_t tt_get_ns(void) {
    return test_mock_now;
}

int32_t tt_get_node_id(void) {
    return test_mock_node_id;
}

tt_ret_t tt_bind(struct tt_Node* node) {
    (void)node;
    return test_mock_bind_return;
}

void tt_close(struct tt_Node* node) {
    (void)node;
}

int32_t tt_send(struct tt_Node* node, const void* buf, size_t len) {
    (void)node;
    (void)buf;

    test_mock_send_call_count++;

    if (test_mock_send_return_override) {
        return test_mock_send_return;
    }

    return (int32_t)len;
}

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    (void)node;
    (void)buf;
    (void)len;
    (void)timeout;

    *ip = 0;
    *port = 0;

    return test_mock_receive_return;
}
#endif
