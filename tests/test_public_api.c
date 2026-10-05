/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Every function tickle.h declares must also be defined. Taking a function's address needs its
// definition at link time, so a declaration that nothing implements fails this binary's own link -
// which is the whole test; there is nothing to assert at runtime.
//
// Real failure this exists for (rmw_tickle/PLAN.md, Phase 2 -> 3): tt_Publisher_unacked_bound() was
// declared in tickle.h and described in the CHANGELOG, but its definition never landed (a scripted
// edit that applied the header half and dropped the source half). Nothing called it, so every
// build, every test and all of CI passed while main shipped a public API that could not link. An
// external caller would have found out instead of us.
//
// Maintenance: add a line here whenever tickle.h gains a function. The list is hand-maintained on
// purpose - a generated one would need a C parser to be right, and a stale list still catches the
// failure mode above for everything already on it. tests/test_common.h's own
// `find . -name '*.[ch]'` lint pass keeps this file formatted like the rest.
//
// HAL entry points (tt_send(), tt_get_ns(), ... - hal.h) are deliberately absent: those are
// implemented per platform, and in a unit test by tests/test_mock.h, so listing them would prove
// only that the mock exists.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// clang-format off
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox include, after the mock HAL above
// clang-format on

// Deliberately void* rather than a function-pointer type: these have different signatures, and the
// only thing being checked is that each symbol resolves.
static void* const public_api[] = {
    // Node lifecycle and the poll loop
    (void*)tt_Context_create,
    (void*)tt_Context_destroy,
    (void*)tt_Context_poll,
    (void*)tt_Context_interrupt,
    (void*)tt_Context_schedule,
    (void*)tt_Context_unschedule,
    (void*)tt_Context_set_discovery,
    (void*)tt_Context_entity_alive,
    // Endpoint creation
    (void*)tt_Context_create_publisher,
    (void*)tt_Context_create_subscriber,
    (void*)tt_Context_create_client,
    (void*)tt_Context_create_server,
    // Publisher
    (void*)tt_Publisher_publish,
    (void*)tt_Publisher_destroy,
    (void*)tt_Publisher_set_heartbeat_period,
    (void*)tt_Publisher_set_ack_solicit_period,
    (void*)tt_Publisher_request_ack,
    (void*)tt_Publisher_is_acked_by_all_peers,
    (void*)tt_Publisher_min_acked_seq_no,
    (void*)tt_Publisher_unacked_bound, // the one that was missing
    (void*)tt_Publisher_writable,
    // Subscriber, Client, Server
    (void*)tt_Subscriber_destroy,
    (void*)tt_Client_call,
    (void*)tt_Client_destroy,
    (void*)tt_Server_send_response,
    (void*)tt_Server_destroy,
    // Caller-owned storage and discovery
    (void*)tt_ReliableCache_init,
    (void*)tt_ReliableCache_grow,
    (void*)tt_Discovery_count,
    (void*)tt_Discovery_find,
    // Framing helpers tickle.h exposes
    (void*)tt_hash_id,
    (void*)tt_is_native_endian,
    (void*)tt_is_reverse_endian,
};

// Every tt_ret_t value must be distinct: a switch with one case per code fails to compile on a
// duplicate. Real failure: tt_RET_UNSUPPORTED was added as -15, the value tt_RET_BUSY already had, so
// an io_uring-only build refused by its container read as "another thread is polling". Add a case
// whenever hal.h gains a code.
static const char* ret_name(tt_ret_t ret) {
    switch (ret) {
    case tt_RET_OK:
        return "OK";
    case tt_RET_TIMEOUT:
        return "TIMEOUT";
    case tt_RET_IO_ERROR:
        return "IO_ERROR";
    case tt_RET_PROTOCOL_ERROR:
        return "PROTOCOL_ERROR";
    case tt_RET_OUT_OF_MEMORY:
        return "OUT_OF_MEMORY";
    case tt_RET_OUT_OF_BUFFER:
        return "OUT_OF_BUFFER";
    case tt_RET_OUT_OF_SCHEDULE:
        return "OUT_OF_SCHEDULE";
    case tt_RET_IILEGAL_NODE_ID:
        return "IILEGAL_NODE_ID";
    case tt_RET_IILEGAL_ENDPOINT_ID:
        return "IILEGAL_ENDPOINT_ID";
    case tt_RET_ILLEGAL_STATUS:
        return "ILLEGAL_STATUS";
    case tt_RET_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case tt_RET_INTERRUPTED:
        return "INTERRUPTED";
    case tt_RET_NOT_FOUND:
        return "NOT_FOUND";
    case tt_RET_WOULD_BLOCK:
        return "WOULD_BLOCK";
    case tt_RET_NO_SUCH_LINK:
        return "NO_SUCH_LINK";
    case tt_RET_UNSUPPORTED:
        return "UNSUPPORTED";
    case tt_RET_BUSY:
        return "BUSY";
    }
    return NULL;
}

int main(void) {
    // Nothing is called - resolving the addresses above is the test. The count is printed so a
    // silently-emptied table is visible rather than passing vacuously.
    size_t count = sizeof(public_api) / sizeof(public_api[0]);
    for (size_t i = 0; i < count; i++) {
        if (public_api[i] == NULL) {
            fprintf(stderr, "public_api[%zu] is NULL\n", i);
            test_failures++;
        }
    }

    if (ret_name(tt_RET_BUSY) == NULL || strcmp(ret_name(tt_RET_UNSUPPORTED), "UNSUPPORTED") != 0) {
        fprintf(stderr, "ret_name() does not name every code\n");
        test_failures++;
    }

    printf("test_public_api: %zu public functions link; %s\n", count,
           test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
