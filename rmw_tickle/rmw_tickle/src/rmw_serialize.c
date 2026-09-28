/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 3: rmw_serialize()/rmw_deserialize() - the same ros<->tickle
// conversion and TickLE CDR-4 codec rmw_publish()/the subscriber callback use (rmw_publisher.c/
// rmw_subscription.c), just writing to/reading from a caller-owned byte buffer instead of the
// network.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/types/rcutils_ret.h" // RCUTILS_RET_OK
#include "rmw/error_handling.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/serialized_message.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/sequence_bound.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

// (g1) The direct codec where the type has one: the same bytes, straight from the ROS message.
// test/golden/rmw_serialize_bytes.txt holds what the struct path produced for two fixed messages
// before this change, which is what test_serialized checks these against - comparing the new
// encoder with rmw_serialize of the same build would compare it with itself.
rmw_ret_t rmw_serialize(const void* ros_message, const rosidl_message_type_support_t* type_support,
                        rmw_serialized_message_t* serialized_message) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(serialized_message, RMW_RET_INVALID_ARGUMENT);

    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = rmw_tickle_get_message_callbacks(type_support);
    if (NULL == callbacks) {
        return RMW_RET_ERROR; // error message already set
    }

    if (NULL != callbacks->direct_encode) {
        int32_t direct_size = callbacks->direct_encode_size(ros_message);
        if (direct_size < 0) {
            RMW_SET_ERROR_MSG("this message cannot be serialized: a string or sequence is past its bound");
            return RMW_RET_ERROR;
        }
        if (serialized_message->buffer_capacity < (size_t)direct_size &&
            rmw_serialized_message_resize(serialized_message, (size_t)direct_size) != RCUTILS_RET_OK) {
            RMW_SET_ERROR_MSG("failed to resize serialized_message");
            return RMW_RET_BAD_ALLOC;
        }
        int32_t written = callbacks->direct_encode(ros_message, serialized_message->buffer,
                                                   (uint32_t)serialized_message->buffer_capacity);
        if (written < 0) {
            RMW_SET_ERROR_MSG("direct_encode() failed");
            return RMW_RET_ERROR;
        }
        serialized_message->buffer_length = (size_t)written;
        return RMW_RET_OK;
    }

    void* tickle_buf =
        serialized_message->allocator.allocate(callbacks->tickle_struct_size, serialized_message->allocator.state);
    if (NULL == tickle_buf) {
        RMW_SET_ERROR_MSG("failed to allocate scratch TickLE struct");
        return RMW_RET_BAD_ALLOC;
    }

    if (!callbacks->to_tickle(ros_message, tickle_buf)) {
        RMW_SET_ERROR_MSG("failed to convert ROS message to TickLE wire struct (capacity exceeded?)");
        serialized_message->allocator.deallocate(tickle_buf, serialized_message->allocator.state);
        return RMW_RET_ERROR;
    }

    int32_t size = callbacks->tickle_encode_size(tickle_buf);
    if (size < 0) {
        RMW_SET_ERROR_MSG("failed to compute TickLE encode size");
        serialized_message->allocator.deallocate(tickle_buf, serialized_message->allocator.state);
        return RMW_RET_ERROR;
    }

    if (serialized_message->buffer_capacity < (size_t)size) {
        if (rmw_serialized_message_resize(serialized_message, (size_t)size) != RCUTILS_RET_OK) {
            RMW_SET_ERROR_MSG("failed to resize serialized_message");
            serialized_message->allocator.deallocate(tickle_buf, serialized_message->allocator.state);
            return RMW_RET_BAD_ALLOC;
        }
    }

    int32_t encoded =
        callbacks->tickle_encode(tickle_buf, serialized_message->buffer, (uint32_t)serialized_message->buffer_capacity);
    serialized_message->allocator.deallocate(tickle_buf, serialized_message->allocator.state);
    if (encoded != size) {
        RMW_SET_ERROR_MSG("tickle_encode() did not produce the expected size");
        return RMW_RET_ERROR;
    }
    serialized_message->buffer_length = (size_t)encoded;
    return RMW_RET_OK;
}

rmw_ret_t rmw_deserialize(const rmw_serialized_message_t* serialized_message,
                          const rosidl_message_type_support_t* type_support, void* ros_message) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(serialized_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);

    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = rmw_tickle_get_message_callbacks(type_support);
    if (NULL == callbacks) {
        return RMW_RET_ERROR; // error message already set
    }

    if (NULL != callbacks->direct_decode) {
        // true: rmw_serialize() always encodes in this machine's own native byte order - there is
        // no framing here to negotiate endianness against, unlike the real wire path.
        if (callbacks->direct_decode(ros_message, serialized_message->buffer,
                                     (uint32_t)serialized_message->buffer_length, true) < 0) {
            RMW_SET_ERROR_MSG("direct_decode() failed");
            return RMW_RET_ERROR;
        }
        return RMW_RET_OK;
    }

    rcutils_allocator_t allocator = serialized_message->allocator;
    void* tickle_buf = allocator.zero_allocate(1, callbacks->tickle_struct_size, allocator.state);
    if (NULL == tickle_buf) {
        RMW_SET_ERROR_MSG("failed to allocate scratch TickLE struct");
        return RMW_RET_BAD_ALLOC;
    }

    // true: rmw_serialize() (above) always encodes in the local machine's own native byte order -
    // there's no network framing/tt_Header magic to negotiate endianness against here, unlike the
    // real wire path (tickle.c's process_data(), which derives this from tt_is_native_endian()).
    int32_t decoded = callbacks->tickle_decode(tickle_buf, serialized_message->buffer,
                                               (uint32_t)serialized_message->buffer_length, true);
    if (decoded < 0) {
        RMW_SET_ERROR_MSG("tickle_decode() failed");
        allocator.deallocate(tickle_buf, allocator.state);
        return RMW_RET_ERROR;
    }

    // Same "ros_message assumed fresh/blank" caveat as rmw_subscription.c's own rmw_take_with_
    // info() - see its doc comment there.
    bool converted = callbacks->from_tickle(tickle_buf, ros_message);
    callbacks->tickle_free(tickle_buf);
    allocator.deallocate(tickle_buf, allocator.state);
    if (!converted) {
        RMW_SET_ERROR_MSG("failed to convert TickLE wire struct to ROS message");
        return RMW_RET_ERROR;
    }
    return RMW_RET_OK;
}

// (g1) What rmw_serialize() would write for a message of this type. rcl asks before serializing so
// it can size a buffer once. A type carrying a plain string, or a sequence of them, has no answer
// that does not depend on the message, and the generator says so by leaving tickle_max_encoded_size
// at its "no bound" marker - this refuses for those rather than guessing, which is what a caller
// wants to know. `message_bounds` would be the way to ask about a bounded instance; rcl passes NULL.
rmw_ret_t rmw_get_serialized_message_size(const rosidl_message_type_support_t* type_support,
                                          const rosidl_runtime_c__Sequence__bound* message_bounds,
                                          size_t* size) { // NOLINT(readability-non-const-parameter)
    (void)message_bounds;
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(size, RMW_RET_INVALID_ARGUMENT);
    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = rmw_tickle_get_message_callbacks(type_support);
    if (NULL == callbacks) {
        return RMW_RET_ERROR; // error message already set
    }
    if (ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED == callbacks->tickle_max_encoded_size) {
        RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
            "%s has no upper bound on its encoded size - it carries a string, or a sequence of them, so how many "
            "bytes it takes depends on the message. Serialize it and read the length instead.",
            callbacks->ros_type_name);
        return RMW_RET_UNSUPPORTED;
    }
    *size = callbacks->tickle_max_encoded_size;
    return RMW_RET_OK;
}
