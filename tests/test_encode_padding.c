/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// end_encode() pads every submessage to 4 bytes, and the padding is counted in the submessage's length, so it goes
// on the wire. It must be zero (2026-09-27). It used to be whatever an earlier datagram had left in tx_buffer: a
// receiver saw a payload ending in stale bytes - test_thread_safety's response codec, which requires its string's
// terminator last, then failed on every retry of that call, and the client never completed it - and bytes of an
// earlier datagram left the host.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: builds submessages with tickle.c's own start_encode() / encode() / end_encode().
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
// A generated codec (examples/set_bool, from tools/typesupport), to see what a real decoder makes of the padding.
#include "SetBool.c" // NOLINT(bugprone-suspicious-include) -- the generated codec, compiled into this test

#define STALE 0xEE
#define PAYLOAD 0x11

static struct tt_Context node;

static void setup(void) {
    test_mock_reset();
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    memset(node.tx_buffer + sizeof(struct tt_Header), STALE, sizeof(node.tx_buffer) - sizeof(struct tt_Header));
}

// Every payload length that leaves 0, 1, 2 or 3 bytes of padding: the submessage's length covers the padding, and
// every padding byte is zero although tx_buffer held non-zero bytes there before.
static void test_padding_is_zero_for_every_remainder(void) {
    for (uint32_t payload_length = 1; payload_length <= 8; payload_length++) {
        setup();
        struct tt_SubmessageHeader* header = start_encode(&node, tt_SUBMESSAGE_TYPE_CALLRESPONSE, 2);
        EXPECT_TRUE(header != NULL);
        if (header == NULL) {
            return;
        }
        uint8_t* payload = encode(&node, payload_length);
        EXPECT_TRUE(payload != NULL);
        if (payload == NULL) {
            return;
        }
        memset(payload, PAYLOAD, payload_length);
        uint32_t unpadded = (uint32_t)(payload + payload_length - (uint8_t*)header);
        EXPECT_TRUE(end_encode(&node, header, false, NULL, 0));
        EXPECT_EQ_U32((uint32_t)ROUNDUP(unpadded), (uint32_t)header->length);
        for (uint32_t i = unpadded; i < header->length; i++) {
            EXPECT_EQ_U32(0, ((uint8_t*)header)[i]);
        }
        EXPECT_EQ_U32(PAYLOAD, payload[payload_length - 1]); // the payload itself is left alone
    }
}

// Does a generated decoder read the padding? It reads a string through its length prefix, checks the terminator at the
// prefix's end and skips to the next 4-byte boundary, so it never reads the tail of the payload: a message ending
// in one bool (3 bytes of end_encode() padding) and one ending in a string both decode as the receiver sees them -
// the payload through the padded submessage end - with the padding stale (as before the fix) or zeroed.
static int32_t decode_as_received(bool stale_padding, bool response) {
    setup();
    struct tt_SubmessageHeader* header = start_encode(&node, tt_SUBMESSAGE_TYPE_CALLRESPONSE, 2);
    struct SetBoolRequest request = {.data = true};
    struct SetBoolResponse answer = {.success = true, .message = "a message past fifteen bytes"};
    int32_t size = response ? SetBoolResponse_encode_size(&answer) : SetBoolRequest_encode_size(&request);
    uint8_t* payload = encode(&node, (uint32_t)size);
    if (header == NULL || payload == NULL) {
        return -100;
    }
    int32_t encoded = response ? SetBoolResponse_encode(&answer, payload, (uint32_t)size)
                               : SetBoolRequest_encode(&request, payload, (uint32_t)size);
    uint32_t unpadded = (uint32_t)(payload + encoded - (uint8_t*)header);
    EXPECT_TRUE(end_encode(&node, header, false, NULL, 0));
    if (!response) {
        EXPECT_TRUE(header->length > unpadded); // the bool leaves padding to read, or this case would prove nothing
    }
    if (stale_padding) {
        memset((uint8_t*)header + unpadded, STALE, header->length - unpadded); // what the padding held before the fix
    }
    uint32_t received = header->length - (uint32_t)(payload - (uint8_t*)header);
    if (response) {
        struct SetBoolResponse decoded;
        int32_t result = SetBoolResponse_decode(&decoded, payload, received, true);
        return result >= 0 && strcmp(decoded.message, answer.message) == 0 ? result : -101;
    }
    struct SetBoolRequest decoded;
    int32_t result = SetBoolRequest_decode(&decoded, payload, received, true);
    return result >= 0 && decoded.data ? result : -101;
}

static void test_a_generated_decoder_ignores_the_padding(void) {
    EXPECT_TRUE(decode_as_received(true, false) >= 0);
    EXPECT_TRUE(decode_as_received(false, false) >= 0);
    EXPECT_TRUE(decode_as_received(true, true) >= 0);
    EXPECT_TRUE(decode_as_received(false, true) >= 0);
}

int main(void) {
    test_padding_is_zero_for_every_remainder();
    test_a_generated_decoder_ignores_the_padding();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_encode_padding: all tests passed\n");
    return 0;
}
