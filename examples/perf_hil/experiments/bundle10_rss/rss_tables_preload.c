/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// bundle10_rss: rss_tables.c as an LD_PRELOAD for an unmodified rmw_tickle process. An object is found by a calloc()
// of exactly the sizeof of a TYPE in the layout (rmw_tickle zero_allocate()s its context implementation, which holds
// the core tt_Context and tt_Discovery); its fields are read every RSS_TABLES_PERIOD_MS (default 200) and once more
// just before it is freed, and the peaks are written to RSS_TABLES_OUT at every sample (so a killed process still
// leaves its last reading). A process that never allocated a matching object writes "RT_NONE" - the reader voids it
// rather than reading an absence as "nothing resident".
//
//   gcc -O2 -shared -fPIC -o rss_tables_preload.so rss_tables_preload.c -lpthread
//   RSS_TABLES_LAYOUT=layout.txt RSS_TABLES_OUT=out.txt LD_PRELOAD=$PWD/rss_tables_preload.so rss_node ...
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// One instrument, two uses: core_host.c links it into a host, this file preloads it - both from the same source.
#include "rss_tables.c" // NOLINT(bugprone-suspicious-include)

// glibc's own allocator entry points: the calloc() and free() below interpose libc's and must reach the real ones
// without dlsym(), which itself calls calloc().
// NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming)
extern void* __libc_calloc(size_t nmemb, size_t size);
extern void __libc_free(void* ptr);
// NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming)

enum {
    RT_OUT_PATH_BYTES = 512,
    RT_TMP_PATH_BYTES = 600, // RT_OUT_PATH_BYTES + ".tmp", with room to spare
    RT_DEFAULT_PERIOD_MS = 200,
    RT_MS_PER_S = 1000,
    RT_NS_PER_MS = 1000000,
};

static int rt_ready;
static pthread_mutex_t rt_lock = PTHREAD_MUTEX_INITIALIZER; // NOLINT(misc-include-cleaner) - <pthread.h> above
static char rt_out_path[RT_OUT_PATH_BYTES];
static long rt_period_ms = RT_DEFAULT_PERIOD_MS;
static unsigned long rt_samples;

// The instrument's own calloc()/free() calls come back through the interposed ones below (misc-no-recursion); an
// allocation the instrument makes is never one it registers, unless its size matches a layout TYPE's sizeof.
static void rt_write(const char* label) { // NOLINT(misc-no-recursion)
    char tmp[RT_TMP_PATH_BYTES];
    snprintf(tmp, sizeof(tmp), "%s.tmp", rt_out_path);
    FILE* file = fopen(tmp, "w");
    if (file == NULL) {
        return;
    }
    if (__atomic_load_n(&rt_nobjs, __ATOMIC_ACQUIRE) == 0) {
        size_t rss = 0;
        size_t anon = 0;
        size_t hwm = 0;
        rt_proc_kb(&rss, &anon, &hwm);
        fprintf(file, "RT_NONE pid=%d rss_kb=%zu hwm_kb=%zu\n", (int)getpid(), rss, hwm);
    } else {
        fprintf(file, "RT_SAMPLES n=%lu\n", rt_samples);
        rt_snapshot(file, label);
    }
    fclose(file);
    rename(tmp, rt_out_path);
}

static void* rt_sampler(void* arg) {
    (void)arg;
    for (;;) {
        struct timespec ts = {.tv_sec = rt_period_ms / RT_MS_PER_S,
                              .tv_nsec = (rt_period_ms % RT_MS_PER_S) * (long)RT_NS_PER_MS};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&rt_lock);
        rt_samples++;
        rt_write("periodic");
        pthread_mutex_unlock(&rt_lock);
    }
    return NULL;
}

__attribute__((constructor)) static void rt_init(void) {
    const char* layout = getenv("RSS_TABLES_LAYOUT");
    const char* out = getenv("RSS_TABLES_OUT");
    const char* period = getenv("RSS_TABLES_PERIOD_MS");
    if (layout == NULL || out == NULL || rt_load_layout(layout) != 0) {
        return;
    }
    snprintf(rt_out_path, sizeof(rt_out_path), "%s", out);
    if (period != NULL && atol(period) > 0) {
        rt_period_ms = atol(period);
    }
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> above
    if (pthread_create(&thread, NULL, rt_sampler, NULL) == 0) {
        pthread_detach(thread);
    }
    __atomic_store_n(&rt_ready, 1, __ATOMIC_RELEASE);
}

__attribute__((destructor)) static void rt_fini(void) {
    if (!__atomic_load_n(&rt_ready, __ATOMIC_ACQUIRE)) {
        return;
    }
    pthread_mutex_lock(&rt_lock);
    rt_write("exit");
    pthread_mutex_unlock(&rt_lock);
}

void* calloc(size_t nmemb, size_t size) { // NOLINT(misc-no-recursion) - see rt_write()
    void* p = __libc_calloc(nmemb, size);
    if (p != NULL && __atomic_load_n(&rt_ready, __ATOMIC_ACQUIRE) && size != 0 && nmemb <= SIZE_MAX / size) {
        size_t bytes = nmemb * size;
        for (int type_idx = 0; type_idx < rt_ntypes; type_idx++) {
            if (rt_types[type_idx].size == bytes) {
                pthread_mutex_lock(&rt_lock);
                rt_add(type_idx, rt_types[type_idx].name, (uintptr_t)p, bytes);
                pthread_mutex_unlock(&rt_lock);
                break;
            }
        }
    }
    return p;
}

void free(void* ptr) { // NOLINT(misc-no-recursion) - see rt_write()
    if (ptr != NULL) {
        int nobjs = __atomic_load_n(&rt_nobjs, __ATOMIC_ACQUIRE);
        for (int i = 0; i < nobjs; i++) {
            if (rt_objs[i].base == (uintptr_t)ptr && rt_objs[i].live) {
                pthread_mutex_lock(&rt_lock);
                rt_write("before_free"); // raises the peaks with the object still mapped
                rt_objs[i].live = 0;
                rt_write("after_free");
                pthread_mutex_unlock(&rt_lock);
                break;
            }
        }
    }
    __libc_free(ptr);
}
