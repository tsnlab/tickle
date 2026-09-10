/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// libFuzzer harness for the packet parser - the one place TickLE reads fully untrusted input
// (broadcast UDP, no authentication). Build/run via `make fuzz` (needs clang):
//   make fuzz FUZZ_ARGS='-max_total_time=60'
// or point it at a corpus directory. Whitebox, same as the tests/test_*.c files.

#include <stdint.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: reach process_packet() and its static helpers directly.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include)

// A couple of registered endpoints so the fuzzer can reach process_data() /
// process_callrequest() / process_callresponse() bodies, not just the header/length checks.
static int32_t stub_size(struct tt_Data* d) {
    (void)d;
    return 4;
}
static int32_t stub_enc(struct tt_Data* d, uint8_t* p, uint32_t n) {
    (void)d;
    (void)p;
    (void)n;
    return 4;
}
static int32_t stub_dec(struct tt_Data* d, const uint8_t* p, uint32_t n, bool native) {
    (void)p;
    (void)native;
    if (n < 4) {
        return -1;
    }
    memset(d, 0, 4);
    return 4;
}
static void stub_free(struct tt_Data* d) {
    (void)d;
}
static void sub_cb(struct tt_Subscriber* s, uint64_t t, uint16_t q, struct tt_Data* d) {
    (void)s;
    (void)t;
    (void)q;
    (void)d;
}

int LLVMFuzzerInitialize(int* argc, char*** argv) {
    (void)argc;
    (void)argv;
    tt_current_log_level = TT_LOG_NONE; // don't flood the fuzzer's output with rejection messages
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > tt_MAX_BUFFER_LENGTH) {
        return 0; // the real receive path never hands process_packet() more than one datagram
    }

    static struct tt_Topic topic = {.name = "fuzz",
                                    .data_size = 4,
                                    .data_encode_size = stub_size,
                                    .data_encode = stub_enc,
                                    .data_decode = stub_dec,
                                    .data_free = stub_free};
    static struct tt_Subscriber sub;
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;

    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = 0x11223344;
    sub.topic = &topic;
    sub.callback = sub_cb;
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&sub;

    memcpy(node.rx_buffer, data, size);
    process_packet(&node, node.rx_buffer, 0, (uint32_t)size, 0x0a000002, 8282);

    // process_update() is the one path that allocates; free anything it recorded so a long fuzz
    // run doesn't just leak-report on the first UPDATE.
    for (uint32_t i = 0; i < tt_MAX_ENDPOINT_COUNT; i++) {
        _tt_free(node.updates[i]);
    }
    return 0;
}
