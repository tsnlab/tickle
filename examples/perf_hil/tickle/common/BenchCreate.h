/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Endpoint creation for the TickLE rig benches, with the QoS given at creation (docs/DESIGN.md section 9): the
// Publisher or Subscriber goes on the context's default node, as the deprecated tt_Context_create_*() put it, with
// caller storage of exactly the size the profile needs. Header-only, like the other Bench*.h helpers.
//
// The storage comes from calloc() and is never freed: an endpoint keeps it until it is destroyed, and the benches
// destroy theirs with the context at the end of main(), as the static arrays this replaced lived to exit. calloc()
// is 16-byte aligned on Linux, past the 8 the creates require.
//
// Every bench spells its profile out field by field, and a Subscriber's history is KEEP_ALL: the deprecated creates
// left tt_Subscriber.keep_last_depth at 0 ("never skip"), and a KEEP_LAST profile would set it to the depth and turn
// on the segment drain's skip path on a same-host run. KEEP_ALL is what keeps 0 there; on a Subscriber it changes
// nothing else.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <tickle/hal.h> // tt_ret_t
#include <tickle/tickle.h>

#include "Bench.h"

// Creates `pub` on topic `topic_name` of BenchTopic with `qos` and `options` (NULL: none).
static inline tt_ret_t BenchCreate_publisher(struct tt_Context* context, struct tt_Publisher* pub,
                                             const char* topic_name, const struct tt_QosProfile* qos,
                                             const struct tt_PublisherOptions* options) {
    const size_t size = tt_Publisher_storage_size(&BenchTopic, qos);
    void* storage = NULL;
    if (size != 0) {
        storage = calloc(1, size);
        if (storage == NULL) {
            return tt_RET_OUT_OF_BUFFER;
        }
    }
    return tt_Node_create_publisher(tt_Context_default_node(context), pub, &BenchTopic, topic_name, qos, options,
                                    storage, size);
}

// Creates `sub` on topic `topic_name` of BenchTopic with `qos`, delivering to `callback`.
static inline tt_ret_t BenchCreate_subscriber(struct tt_Context* context, struct tt_Subscriber* sub,
                                              const char* topic_name, const struct tt_QosProfile* qos,
                                              tt_SUBSCRIBER_CALLBACK callback) {
    const struct tt_SubscriberOptions options = {.callback = callback};
    const size_t size = tt_Subscriber_storage_size(&BenchTopic, qos);
    void* storage = NULL;
    if (size != 0) {
        storage = calloc(1, size);
        if (storage == NULL) {
            return tt_RET_OUT_OF_BUFFER;
        }
    }
    return tt_Node_create_subscriber(tt_Context_default_node(context), sub, &BenchTopic, topic_name, qos, &options,
                                     storage, size);
}

// history.bytes for a Publisher that retains `depth` samples and used to have an arena of `old_arena` bytes: no less
// than the old arena, and no less than `depth` samples of BenchTopic's largest size need, so the cap never binds
// below the depth (which would also log a warning at creation).
static inline uint32_t BenchCreate_history_bytes(uint32_t depth, uint32_t old_arena) {
    const uint64_t full = ((uint64_t)depth + 1U) * tt_sample_cache_bytes(BenchTopic.max_encoded_size);
    const uint64_t bytes = full > old_arena ? full : old_arena;
    return bytes < UINT32_MAX ? (uint32_t)bytes : UINT32_MAX;
}
