/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// What an application may put in rmw_publisher_options_t.rmw_specific_publisher_payload to size one
// publisher's retained-sample storage, instead of the RMW_TICKLE_* environment variables that set it
// for the whole process (2026-09-25). The env vars stay, and remain the default; a payload field
// that is 0 leaves that knob to them.
//
// Why per publisher: the env vars are process-wide, so a node with one large-sample topic among
// forty had to raise the budget for all forty publishers to give that one what it needs.
//
// Through rclcpp, an application reaches this field by subclassing
// rclcpp::detail::RMWImplementationSpecificPublisherPayload and setting the pointer in
// modify_rmw_publisher_options(); the object must outlive every publisher created with it, which is
// rmw's own rule for this field (rmw/types.h).
//
// Standalone: include only this header - it pulls in nothing of rmw_tickle's internals.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// rmw_specific_publisher_payload is a bare void* that every rmw implementation shares, so a payload
// meant for another one can arrive here. This says "mine" - rmw_tickle reads no further without it,
// and warns instead. Spelled 'tTPL' in ASCII.
#define RMW_TICKLE_PUBLISHER_PAYLOAD_MAGIC 0x7454504CU

/// Per-publisher storage sizing for rmw_tickle.
typedef struct rmw_tickle_publisher_payload_t {
    /// RMW_TICKLE_PUBLISHER_PAYLOAD_MAGIC. Anything else and the payload is ignored.
    uint32_t magic;
    /// sizeof(rmw_tickle_publisher_payload_t), so a payload built against a different version of
    /// this header is refused rather than misread - the same marker rosidl_typesupport_tickle_c's
    /// callbacks struct carries, and for the same reason.
    uint32_t struct_size;
    /// Byte budget for what this publisher holds, or 0 for the environment's default. What reaching
    /// it means depends on the history policy:
    ///   - KEEP_LAST: retained samples, 0 = RMW_TICKLE_CACHE_BYTES. Past it the oldest are evicted.
    ///   - KEEP_ALL, VOLATILE: unacknowledged samples, 0 = RMW_TICKLE_KEEP_ALL_BYTES (since
    ///     2026-09-25). Past it the writer blocks - nothing is ever dropped.
    ///   - KEEP_ALL, TRANSIENT_LOCAL: not budgeted; its depth is the history a late joiner replays.
    /// Either way the arena is never smaller than one sample of this type, nor larger than
    /// (depth + 1) records - this caps it, it does not set it.
    uint32_t cache_bytes;
    /// How large this publisher's samples actually get, or 0 for
    /// RMW_TICKLE_KEEP_ALL_MAX_SAMPLE_BYTES and the derived default. This is what the cache reserves
    /// per retained sample, under either history policy.
    ///
    /// Clamped upward by what the type can actually produce - the generator's bound when it has
    /// one, the datagram otherwise - since reserving past an exact maximum is waste with no
    /// benefit. Setting it LOWER than that bound is the point: a type bounded at 64 KB whose
    /// samples are really 8 KB costs 11 x 64 KB at depth 10, and saying 8192 here gets the same
    /// ten samples for an eighth of the memory.
    ///
    /// Under-reserving is safe under both policies, and costs only retention or throughput: a
    /// KEEP_LAST publisher keeps fewer than its depth, and a KEEP_ALL one blocks sooner. Neither
    /// drops an unacknowledged sample.
    uint32_t max_sample_bytes;
} rmw_tickle_publisher_payload_t;

/// Initialiser that sets the two markers and leaves every knob to the environment:
///   rmw_tickle_publisher_payload_t payload = RMW_TICKLE_PUBLISHER_PAYLOAD_INIT;
///   payload.cache_bytes = 64U * 1024U * 1024U;
#define RMW_TICKLE_PUBLISHER_PAYLOAD_INIT \
    {RMW_TICKLE_PUBLISHER_PAYLOAD_MAGIC, (uint32_t)sizeof(rmw_tickle_publisher_payload_t), 0U, 0U}

#ifdef __cplusplus
}
#endif
