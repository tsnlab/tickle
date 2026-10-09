/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Large-message stage 2 (docs/DESIGN.md section 8): the buffers core publishes samples above tt_MAX_SAMPLE_LENGTH
// from, and puts them back together in - one per sample, from malloc() with a small free list, so a steady stream of
// same-sized images reuses the same few buffers instead of faulting a fresh megabyte in each time.
//
// RMW_TICKLE_MAX_SAMPLE_BYTES (default 8 MiB, the user's decision of 2026-10-09) bounds them: a buffer for more than
// that is refused, so a publish of a larger message fails with an error naming the limit and a larger one arriving is
// dropped and counted (large.no_buffer). Core's own ceiling, one reader window of datagrams (about 11.3 MiB), caps the
// setting.

#include <pthread.h> // NOLINT(misc-include-cleaner) - pthread_mutex_*: see rmw_tickle.h's own <pthread.h> comment
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> // malloc()/free(), getenv()/strtoull()
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "rcutils/logging_macros.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

// Buffers kept for reuse, and how much larger than the request a kept one may be and still be handed out: twice, so
// a buffer once used for an 8 MiB sample is not spent on every 64 KiB one.
#define RMW_TICKLE_LARGE_FREE_LIST 4U
#define RMW_TICKLE_LARGE_REUSE_FACTOR 2U
// Ahead of each buffer, its size; 16 keeps what follows aligned for anything malloc() aligns for.
#define RMW_TICKLE_LARGE_HEADER 16U
// How far past the limit a request may be and still be one for a message within it: the 20 bytes of framing ahead of
// a sample, and a reassembly's room for a last fragment it may not fill (one datagram).
#define RMW_TICKLE_LARGE_SLACK 2048U

struct rmw_tickle_large_pool {
    pthread_mutex_t mutex; // NOLINT(misc-include-cleaner) - acquire runs on the publishing or polling thread,
                           // release on whichever thread returns a loan
    uint8_t* free_list[RMW_TICKLE_LARGE_FREE_LIST];
    uint32_t free_count;
    uint32_t limit;      // RMW_TICKLE_MAX_SAMPLE_BYTES, capped at core's ceiling
    atomic_uint refused; // acquires refused for passing the limit
};

// The largest message core can carry as a large sample: tt_LARGE_MAX_FRAGMENTS datagrams of payload, fragment 0 short
// by tt_FRAG_FIRST_L_SHORTFALL - 1,446 + 1,452 x 8,191 bytes at the default datagram (DESIGN.md section 8).
static uint32_t core_ceiling(void) {
    const uint32_t framing = (uint32_t)(sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
    const uint32_t cont = (uint32_t)tt_CONTROL_MAX_LENGTH - framing - (uint32_t)sizeof(struct tt_FragContLHeader);
    return ((uint32_t)tt_LARGE_MAX_FRAGMENTS * cont) - (uint32_t)tt_FRAG_FIRST_L_SHORTFALL;
}

uint32_t rmw_tickle_max_sample_bytes(void) {
    uint32_t ceiling = core_ceiling();
    const char* env = getenv("RMW_TICKLE_MAX_SAMPLE_BYTES");
    if (NULL == env || '\0' == env[0]) {
        return RMW_TICKLE_MAX_SAMPLE_BYTES_DEFAULT < ceiling ? RMW_TICKLE_MAX_SAMPLE_BYTES_DEFAULT : ceiling;
    }
    char* end = NULL;
    unsigned long long value = strtoull(env, &end, 10);
    if (end == env || (end != NULL && '\0' != *end) || value == 0) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle", "RMW_TICKLE_MAX_SAMPLE_BYTES='%s' is not a byte count - using %u", env,
                               (unsigned)RMW_TICKLE_MAX_SAMPLE_BYTES_DEFAULT);
        return RMW_TICKLE_MAX_SAMPLE_BYTES_DEFAULT;
    }
    if (value > ceiling) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                               "RMW_TICKLE_MAX_SAMPLE_BYTES=%llu is above the %u bytes one reader window of datagrams "
                               "carries - using %u",
                               value, (unsigned)ceiling, (unsigned)ceiling);
        return ceiling;
    }
    return (uint32_t)value;
}

static void* large_acquire(void* user, uint32_t bytes) {
    struct rmw_tickle_large_pool* pool = (struct rmw_tickle_large_pool*)user;
    // The request is the sample and its 20 bytes of framing; a reassembly asks for a whole last fragment it may not
    // fill. Either way more than a datagram over the limit is a message over it.
    if (bytes > pool->limit + RMW_TICKLE_LARGE_SLACK) {
        atomic_fetch_add(&pool->refused, 1U);
        return NULL;
    }
    pthread_mutex_lock(&pool->mutex);
    for (uint32_t i = pool->free_count; i-- > 0;) {
        uint8_t* kept = pool->free_list[i];
        uint32_t size = 0;
        memcpy(&size, kept, sizeof(size));
        if (size >= bytes && size / RMW_TICKLE_LARGE_REUSE_FACTOR <= bytes) {
            pool->free_list[i] = pool->free_list[--pool->free_count];
            pthread_mutex_unlock(&pool->mutex);
            return kept + RMW_TICKLE_LARGE_HEADER;
        }
    }
    pthread_mutex_unlock(&pool->mutex);
    uint8_t* block = (uint8_t*)malloc((size_t)bytes + RMW_TICKLE_LARGE_HEADER);
    if (NULL == block) {
        return NULL;
    }
    memcpy(block, &bytes, sizeof(bytes));
    return block + RMW_TICKLE_LARGE_HEADER;
}

static void large_release(void* user, void* buffer) {
    struct rmw_tickle_large_pool* pool = (struct rmw_tickle_large_pool*)user;
    uint8_t* block = (uint8_t*)buffer - RMW_TICKLE_LARGE_HEADER;
    pthread_mutex_lock(&pool->mutex);
    if (pool->free_count < RMW_TICKLE_LARGE_FREE_LIST) {
        pool->free_list[pool->free_count++] = block;
        block = NULL;
    }
    pthread_mutex_unlock(&pool->mutex);
    free(block);
}

bool rmw_tickle_large_attach(rmw_tickle_context_impl_t* context_impl) {
    struct rmw_tickle_large_pool* pool = (struct rmw_tickle_large_pool*)calloc(1, sizeof(*pool));
    if (NULL == pool) {
        return false;
    }
    pthread_mutex_init(&pool->mutex, NULL);
    pool->limit = rmw_tickle_max_sample_bytes();
    atomic_init(&pool->refused, 0U);
    if (tt_RET_OK != tt_Context_set_large_buffers(&context_impl->tickle_context, large_acquire, large_release, pool)) {
        pthread_mutex_destroy(&pool->mutex);
        free(pool);
        return false;
    }
    context_impl->large_pool = pool;
    return true;
}

void rmw_tickle_large_detach(rmw_tickle_context_impl_t* context_impl) {
    struct rmw_tickle_large_pool* pool = context_impl->large_pool;
    if (NULL == pool) {
        return;
    }
    for (uint32_t i = 0; i < pool->free_count; i++) {
        free(pool->free_list[i]);
    }
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
    context_impl->large_pool = NULL;
}

uint32_t rmw_tickle_large_limit(const rmw_tickle_context_impl_t* context_impl) {
    return NULL != context_impl->large_pool ? context_impl->large_pool->limit : 0;
}

uint16_t rmw_tickle_tracking_words_for(const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks) {
    if (NULL != callbacks &&
        (ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED == callbacks->tickle_max_encoded_size ||
         callbacks->tickle_max_encoded_size > (size_t)tt_MAX_SAMPLE_LENGTH)) {
        return (uint16_t)tt_RELIABLE_BITMAP_MAX_WORDS;
    }
    return (uint16_t)RMW_TICKLE_TRACKING_WORDS;
}
