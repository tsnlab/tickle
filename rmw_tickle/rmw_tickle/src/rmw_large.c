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
#include <stdio.h>  // fprintf(), fopen(): the pool's line at detach (RMW_TICKLE_LARGE_HUGEPAGE)
#include <stdlib.h> // malloc()/free(), getenv()/strtoull(), posix_memalign()
#include <string.h>

#include <sys/mman.h> // madvise(): RMW_TICKLE_LARGE_HUGEPAGE
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
// RMW_TICKLE_LARGE_HUGEPAGE=1 (A/B switch, off by default; branch ab/i4be-cpu-candidates): a buffer of at least one
// transparent huge page is allocated aligned to one and its whole huge pages advised MADV_HUGEPAGE, so the kernel's
// copy of each datagram run into it (the in-place read) and the take's copy out of it walk 2 MiB pages, not 4 KiB ones.
// Only the buffer's whole huge pages are advised: rounding it up would fault a further huge page in for its last few
// hundred bytes. 2 MiB is the PMD size with 4 KiB pages (x86-64, and arm64 as the rig Pis run it); THP "madvise" or
// "always" honours the advice, "never" ignores it.
#define RMW_TICKLE_LARGE_HUGEPAGE_BYTES (2U * 1024U * 1024U)
// Longer than any /proc/self/smaps_rollup line (anon_huge_kb()).
#define RMW_TICKLE_SMAPS_LINE 128

struct rmw_tickle_large_pool {
    pthread_mutex_t mutex; // NOLINT(misc-include-cleaner) - acquire runs on the publishing or polling thread,
                           // release on whichever thread returns a loan
    uint8_t* free_list[RMW_TICKLE_LARGE_FREE_LIST];
    uint32_t free_count;
    uint32_t limit;      // RMW_TICKLE_MAX_SAMPLE_BYTES, capped at core's ceiling
    atomic_uint refused; // acquires refused for passing the limit
    bool hugepage;       // RMW_TICKLE_LARGE_HUGEPAGE=1
    uint32_t allocated;  // buffers allocated rather than reused, under mutex
    uint32_t advised;    // of them, advised MADV_HUGEPAGE without error, under mutex
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
    size_t total = (size_t)bytes + RMW_TICKLE_LARGE_HEADER;
    uint8_t* block = NULL;
    bool advised = false;
    if (pool->hugepage && total >= (size_t)RMW_TICKLE_LARGE_HUGEPAGE_BYTES) {
        void* aligned = NULL;
        if (0 == posix_memalign(&aligned, (size_t)RMW_TICKLE_LARGE_HUGEPAGE_BYTES, total)) {
            block = (uint8_t*)aligned;
            size_t whole = total - (total % (size_t)RMW_TICKLE_LARGE_HUGEPAGE_BYTES);
            advised = 0 == madvise(block, whole, MADV_HUGEPAGE);
        }
    } else {
        block = (uint8_t*)malloc(total);
    }
    if (NULL == block) {
        return NULL;
    }
    pthread_mutex_lock(&pool->mutex);
    pool->allocated++;
    pool->advised += advised ? 1U : 0U;
    pthread_mutex_unlock(&pool->mutex);
    memcpy(block, &bytes, sizeof(bytes));
    return block + RMW_TICKLE_LARGE_HEADER;
}

// This process's AnonHugePages in kB (/proc/self/smaps_rollup), -1 when it cannot be read: the realized treatment of
// RMW_TICKLE_LARGE_HUGEPAGE, which a madvise() that succeeded does not show by itself.
static long anon_huge_kb(void) {
    FILE* file = fopen("/proc/self/smaps_rollup", "re");
    if (NULL == file) {
        return -1;
    }
    static const char key[] = "AnonHugePages:";
    char line[RMW_TICKLE_SMAPS_LINE];
    long huge_kb = -1;
    while (NULL != fgets(line, (int)sizeof(line), file)) {
        if (0 == strncmp(line, key, sizeof(key) - 1U)) {
            huge_kb = strtol(line + sizeof(key) - 1U, NULL, 10);
            break;
        }
    }
    (void)fclose(file);
    return huge_kb;
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
    const char* hugepage = getenv("RMW_TICKLE_LARGE_HUGEPAGE");
    pool->hugepage = NULL != hugepage && 0 == strcmp(hugepage, "1");
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
    // One line per context that allocated a large buffer, read while the kept ones are still mapped: the A/B's
    // treatment check (hugepage=1 with AnonHugePages covering the buffers' whole huge pages, or hugepage=0).
    if (pool->allocated > 0) {
        (void)fprintf(stderr, "rmw_tickle: large pool hugepage=%d allocated=%u advised=%u anon_huge_kb=%ld\n",
                      pool->hugepage ? 1 : 0, (unsigned)pool->allocated, (unsigned)pool->advised, anon_huge_kb());
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

void rmw_tickle_large_region_attach(struct tt_Publisher* pub,
                                    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks, size_t depth,
                                    bool keep_all) {
#if !tt_LARGE_SHM
    (void)pub;
    (void)callbacks;
    (void)depth;
    (void)keep_all;
#else
    if (NULL == callbacks || rmw_tickle_tracking_words_for(callbacks) != (uint16_t)tt_RELIABLE_BITMAP_MAX_WORDS) {
        return; // its messages always fit a datagram: never large
    }
    const char* env = getenv("RMW_TICKLE_LARGE_REGION");
    if (NULL != env && 0 == strcmp(env, "0")) {
        return;
    }
    uint32_t limit = rmw_tickle_max_sample_bytes();
    uint32_t largest = limit;
    if (ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED != callbacks->tickle_max_encoded_size &&
        callbacks->tickle_max_encoded_size < (size_t)limit) {
        largest = (uint32_t)callbacks->tickle_max_encoded_size;
    }
    // The psn ahead of the message, and the same slack the large buffers allow for framing.
    uint32_t chunk_bytes = tt_LARGE_CHUNK_BYTES(largest + RMW_TICKLE_LARGE_SLACK);
    size_t retained = keep_all || 0 == depth || depth > (size_t)tt_LARGE_RETAINED ? (size_t)tt_LARGE_RETAINED : depth;
    size_t chunks = retained + (size_t)tt_LARGE_REGION_SPARE;
    if (chunks > (size_t)tt_LARGE_REGION_CHUNKS_MAX) {
        chunks = (size_t)tt_LARGE_REGION_CHUNKS_MAX;
    }
    if (tt_RET_OK != tt_Publisher_set_large_region(pub, (uint32_t)chunks, chunk_bytes)) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle", "could not give a large-message publisher its shared region: its "
                                             "same-host subscribers get fragments");
    }
#endif
}
