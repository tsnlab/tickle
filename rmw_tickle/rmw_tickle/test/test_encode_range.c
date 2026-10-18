/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The publisher's ranged encode (core DESIGN.md section 8, "Encoding while sending"; tt_DATA_ENCODE_RANGE): core asks
// for a large sample a range at a time and requires the ranges to be exactly the bytes data_encode writes. Checked here
// for both kinds rmw_tickle encodes by range - a type whose wire bytes are its message (inplace_bytes) and a serialized
// message - at a short and a long publication sequence number, cut into ranges of every width that straddles the psn
// header differently. A type that is not in place declines (core then encodes it whole), and so does a range past the
// end. data_encode_size is the exact length, which core relies on when it sends ranges.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define BLOB_BYTES 5003U
#define LONG_PSN ((uint64_t)RMW_TICKLE_PSN_LONG_FLAG + 7U) // takes the 8-byte header
#define UNWRITTEN 0xA5U                                    // what a byte no range wrote holds
#define FILL_STEP 7U
#define FILL_SHIFT 8U
#define MESSAGE_TIME (-12345)
#define MESSAGE_ID 0x0102030405060708ULL

// A plain type whose wire bytes are its struct: an odd-length byte array, then two 8-byte fields.
struct blob {
    uint8_t bytes[BLOB_BYTES];
    uint8_t pad[5]; // explicit, so the struct has no hole: its wire bytes are all its memory
    int64_t time;
    uint64_t id;
};

static int32_t blob_encode_size(const void* ros_message) {
    (void)ros_message;
    return (int32_t)sizeof(struct blob);
}

static int32_t blob_encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    if (len < sizeof(struct blob)) {
        return -1;
    }
    memcpy(payload, ros_message, sizeof(struct blob));
    return (int32_t)sizeof(struct blob);
}

static int32_t blob_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct blob)) {
        return -1;
    }
    memcpy(ros_message, payload, sizeof(struct blob));
    return (int32_t)sizeof(struct blob);
}

static int32_t refuse_encode_size(struct tt_Data* data) {
    (void)data;
    return -1;
}

static rosidl_typesupport_tickle_c_message_callbacks_t blob_callbacks = {
    .ros_type_name = "test_encode_range/msg/Blob",
    .tickle_struct_size = sizeof(struct blob),
    .ros_struct_size = sizeof(struct blob),
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&refuse_encode_size,
    .tickle_max_encoded_size = sizeof(struct blob),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
    .direct_encode_size = &blob_encode_size,
    .direct_encode = &blob_encode,
    .direct_decode = &blob_decode,
    .inplace_bytes = sizeof(struct blob),
    .ros_struct_align = _Alignof(struct blob),
};

static rosidl_message_type_support_t blob_handle = {.data = &blob_callbacks,
                                                    .func = get_message_typesupport_handle_function};

// Cuts [0, size) into ranges of `width` (the last shorter) and asks for each in order, as core does; the result must be
// data_encode's bytes exactly.
static void expect_ranges_match(const struct tt_TypeSupport* topic, rmw_tickle_outgoing_message_t* message,
                                uint32_t width) {
    int32_t size = topic->data_encode_size((struct tt_Data*)message);
    assert(size > 0);
    uint8_t* whole = (uint8_t*)malloc((size_t)size);
    uint8_t* by_range = (uint8_t*)malloc((size_t)size);
    assert(NULL != whole && NULL != by_range);
    assert(size == topic->data_encode((struct tt_Data*)message, whole, (uint32_t)size));
    memset(by_range, UNWRITTEN, (size_t)size);
    for (uint32_t offset = 0; offset < (uint32_t)size; offset += width) {
        uint32_t len = (uint32_t)size - offset < width ? (uint32_t)size - offset : width;
        assert((int32_t)len == topic->data_encode_range((struct tt_Data*)message, by_range + offset, offset, len));
    }
    assert(0 == memcmp(whole, by_range, (size_t)size));
    // One byte past the end is declined, and so is a range that runs over it.
    assert(-1 == topic->data_encode_range((struct tt_Data*)message, by_range, (uint32_t)size, 1));
    assert(-1 == topic->data_encode_range((struct tt_Data*)message, by_range, (uint32_t)size - 2U, 3));
    free(whole);
    free(by_range);
}

static void expect_every_width(const struct tt_TypeSupport* topic, rmw_tickle_outgoing_message_t* message) {
    static const uint32_t widths[] = {1, 3, 4, 5, 7, 8, 9, 1446, 1452, 92928, 1U << 30U};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        expect_ranges_match(topic, message, widths[i]);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    blob_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;

    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "encode_range", "/");
    assert(NULL != node);
    rmw_publisher_options_t publisher_options = rmw_get_default_publisher_options();
    rmw_publisher_t* publisher =
        rmw_create_publisher(node, &blob_handle, "/encode_range", &rmw_qos_profile_default, &publisher_options);
    assert(NULL != publisher);
    const struct tt_TypeSupport* topic = &((rmw_tickle_publisher_t*)publisher->data)->topic;
    assert(NULL != topic->data_encode_range);

    struct blob* message = (struct blob*)calloc(1, sizeof(struct blob));
    assert(NULL != message);
    for (uint32_t i = 0; i < BLOB_BYTES; i++) {
        message->bytes[i] = (uint8_t)((i * FILL_STEP) + (i >> FILL_SHIFT) + 1U);
    }
    message->time = MESSAGE_TIME;
    message->id = MESSAGE_ID;

    // In place, at both psn widths: the length is exact, and every cut gives data_encode's bytes.
    static const uint64_t psns[] = {5, LONG_PSN};
    for (size_t which = 0; which < sizeof(psns) / sizeof(psns[0]); which++) {
        rmw_tickle_outgoing_message_t outgoing = {.publication_sequence_number = psns[which],
                                                  .callbacks = &blob_callbacks,
                                                  .ros_message = message};
        assert((int32_t)(rmw_tickle_psn_bytes(psns[which]) + sizeof(struct blob)) ==
               topic->data_encode_size((struct tt_Data*)&outgoing));
        expect_every_width(topic, &outgoing);

        // A serialized message: its own bytes behind the psn, whatever they are.
        rmw_tickle_outgoing_message_t serialized = {.publication_sequence_number = psns[which],
                                                    .callbacks = &blob_callbacks,
                                                    .serialized = message->bytes,
                                                    .serialized_len = BLOB_BYTES - 2U};
        expect_every_width(topic, &serialized);
    }

    // Control: a type that is not in place declines from the first range - core then encodes it whole - while its
    // data_encode still works.
    blob_callbacks.inplace_bytes = 0;
    rmw_tickle_outgoing_message_t not_in_place = {.publication_sequence_number = 5,
                                                  .callbacks = &blob_callbacks,
                                                  .ros_message = message};
    uint8_t first[16];
    assert(-1 == topic->data_encode_range((struct tt_Data*)&not_in_place, first, 0, sizeof(first)));
    blob_callbacks.inplace_bytes = sizeof(struct blob);
    assert((int32_t)sizeof(first) ==
           topic->data_encode_range((struct tt_Data*)&not_in_place, first, 0, sizeof(first))); // and the switch decided

    free(message);
    assert(RMW_RET_OK == rmw_destroy_publisher(node, publisher));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_encode_range: PASS\n");
    return 0;
}
