/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// copy_count_shim - rmw_tickle/RMW_PERF_PLAN.md 11 M-a's second measurement (2026-09-27): an LD_PRELOAD wrapper
// around memcpy/memmove that counts calls and bytes per call site (the caller's return address, as an offset into
// its own object), for copies of COPY_COUNT_MIN bytes and up. At exit it writes one line per site to
// $COPY_COUNT_OUT (default stderr): "SITE object offset calls bytes". Resolve offsets with
// `addr2line -f -e <object> <offset>`. It cannot see a copy the compiler inlined - small fixed-size ones.
//
// Build: gcc -O2 -fno-builtin -fno-tree-loop-distribute-patterns -shared -fPIC -o copy_count_shim.so copy_count_shim.c
// -ldl (the byte loop below must stay a loop: turned back into a memcpy call it would recurse into this one)
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SITES 512
#define COPY_COUNT_MIN 64U

struct site {
    void* caller;
    uint64_t calls;
    uint64_t bytes;
};

static struct site sites[SITES];
static void* (*real_memcpy)(void*, const void*, size_t);
static void* (*real_memmove)(void*, const void*, size_t);

// Until the real functions are resolved - and while dlsym() itself runs, which may copy - a plain byte loop. Resolved
// once, in a constructor: resolving from inside memcpy re-entered dlsym and hung a ROS process (2026-09-27).
static void* byte_copy(void* dst, const void* src, size_t len) {
    volatile uint8_t* dst_bytes = dst;
    const volatile uint8_t* src_bytes = src;
    if (dst_bytes < src_bytes) {
        for (size_t i = 0; i < len; i++) {
            dst_bytes[i] = src_bytes[i];
        }
    } else {
        for (size_t i = len; i > 0; i--) {
            dst_bytes[i - 1] = src_bytes[i - 1];
        }
    }
    return dst;
}

__attribute__((constructor)) static void resolve(void) {
    void* copy = dlsym(RTLD_NEXT, "memcpy");
    void* move = dlsym(RTLD_NEXT, "memmove");
    __atomic_store_n(&real_memmove, (void* (*)(void*, const void*, size_t))move, __ATOMIC_RELEASE);
    __atomic_store_n(&real_memcpy, (void* (*)(void*, const void*, size_t))copy, __ATOMIC_RELEASE);
}

static void record(void* caller, size_t len) {
    if (len < COPY_COUNT_MIN) {
        return;
    }
    uintptr_t slot = ((uintptr_t)caller >> 4U) % SITES;
    for (unsigned probe = 0; probe < SITES; probe++, slot = (slot + 1) % SITES) {
        void* seen = __atomic_load_n(&sites[slot].caller, __ATOMIC_RELAXED);
        if (seen == NULL && __atomic_compare_exchange_n(&sites[slot].caller, &seen, caller, false, __ATOMIC_RELAXED,
                                                        __ATOMIC_RELAXED)) {
            seen = caller; // claimed; on a lost race `seen` now holds the winner
        }
        if (seen == caller) {
            __atomic_fetch_add(&sites[slot].calls, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&sites[slot].bytes, len, __ATOMIC_RELAXED);
            return;
        }
    }
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - interposes libc's, by design
void* memcpy(void* dst, const void* src, size_t len) {
    void* (*real)(void*, const void*, size_t) = __atomic_load_n(&real_memcpy, __ATOMIC_ACQUIRE);
    if (real == NULL) {
        return byte_copy(dst, src, len);
    }
    record(__builtin_return_address(0), len);
    return real(dst, src, len);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - interposes libc's, by design
void* memmove(void* dst, const void* src, size_t len) {
    void* (*real)(void*, const void*, size_t) = __atomic_load_n(&real_memmove, __ATOMIC_ACQUIRE);
    if (real == NULL) {
        return byte_copy(dst, src, len);
    }
    record(__builtin_return_address(0), len);
    return real(dst, src, len);
}

__attribute__((destructor)) static void dump(void) {
    const char* path = getenv("COPY_COUNT_OUT");
    FILE* out = path != NULL ? fopen(path, "a") : stderr;
    if (out == NULL) {
        return;
    }
    for (unsigned i = 0; i < SITES; i++) {
        if (sites[i].caller == NULL) {
            continue;
        }
        Dl_info info;
        if (dladdr(sites[i].caller, &info) != 0 && info.dli_fname != NULL) {
            fprintf(out, "SITE %s 0x%lx %llu %llu\n", info.dli_fname,
                    (unsigned long)((uintptr_t)sites[i].caller - (uintptr_t)info.dli_fbase),
                    (unsigned long long)sites[i].calls, (unsigned long long)sites[i].bytes);
        }
    }
    if (out != stderr) {
        fclose(out);
    }
}
