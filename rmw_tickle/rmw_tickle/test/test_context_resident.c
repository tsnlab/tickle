/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The context implementation (rmw_tickle_context_impl_t, 2.2 MB, sized for the worst case) must not be resident
// beyond what is written. rmw_init() used to allocate() it and memset() it, which wrote every page: 2.2 MB of every
// rmw_tickle process's RSS, most of it tt_Discovery's 2048-entity table and the context's fragment slots, never used
// by a process with a handful of peers (rmw_rss_breakdown.sh, 2026-10-09). zero_allocate() gets pages the kernel
// zeroed and leaves them untouched.
//
// Measured with mincore(), which reports which pages of a mapping are resident. Two controls make "few pages
// resident" mean something:
//   - mincore() sees a write: one page written in a separate zero_allocate()d block of the same size is reported
//     resident, and that block's other pages are not.
//   - the context's own mapping is visible to it: core's tt_Context_init() writes tx_buffer, so tx_buffer's pages are
//     resident. If core stops writing it this control fails, and the test needs a new one, not a looser bound.
// The checks: tt_Discovery's entity table, which nothing writes with no peer, is under 10% resident, and the whole
// struct under 50%. memset() put both at 100%.

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"

enum {
    MAX_PAGES = 4096, // the most pages one resident() call reads: 16 MB, 7x the context
    SETTLE_MS = 300,  // a few context cycles
    BYTES_PER_KB = 1024,
    NS_PER_MS = 1000 * 1000,
};

// Pages wholly inside [start, start + len), and how many of them are resident.
static void resident(const void* start, size_t len, size_t* pages, size_t* in_core) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const unsigned char* base = (const unsigned char*)start;
    size_t lead = (page - ((uintptr_t)base % page)) % page; // bytes up to the first page boundary
    *pages = 0;
    *in_core = 0;
    if (len < lead + page) {
        return;
    }
    size_t count = (len - lead) / page;
    unsigned char vec[MAX_PAGES];
    assert(count <= sizeof(vec));
    int ret = mincore((void*)(base + lead), count * page, vec);
    assert(0 == ret);
    *pages = count;
    for (size_t i = 0; i < count; i++) {
        *in_core += (size_t)(vec[i] & 1U);
    }
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);

    // Control 1: mincore() reports a page written in a zero_allocate()d block this size, and not the others.
    uint8_t* probe = (uint8_t*)allocator.zero_allocate(1, sizeof(rmw_tickle_context_impl_t), allocator.state);
    assert(NULL != probe);
    probe[sizeof(rmw_tickle_context_impl_t) / 2] = 1;
    size_t probe_pages = 0;
    size_t probe_in = 0;
    resident(probe, sizeof(rmw_tickle_context_impl_t), &probe_pages, &probe_in);
    printf("control: a %zu-page zero_allocate()d block with one page written: %zu resident\n", probe_pages, probe_in);
    assert(probe_pages > 500);
    assert(probe_in >= 1 && probe_in <= 3);
    allocator.deallocate(probe, allocator.state);

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_context_resident", "/");
    assert(NULL != node);
    // A few context cycles, so the poll thread's announcements and periodic tasks have run.
    struct timespec pause = {.tv_sec = 0, .tv_nsec = (long)SETTLE_MS * NS_PER_MS};
    nanosleep(&pause, NULL);

    const rmw_tickle_context_impl_t* impl = (const rmw_tickle_context_impl_t*)context.impl;
    size_t all_pages = 0;
    size_t all_in = 0;
    size_t tx_pages = 0;
    size_t tx_in = 0;
    size_t ent_pages = 0;
    size_t ent_in = 0;
    resident(impl, sizeof(*impl), &all_pages, &all_in);
    resident(impl->tickle_context.tx_buffer, sizeof(impl->tickle_context.tx_buffer), &tx_pages, &tx_in);
    resident(impl->discovery.entities, sizeof(impl->discovery.entities), &ent_pages, &ent_in);
    printf("context impl: %zu of %zu pages resident (%zu kB of %zu kB)\n", all_in, all_pages,
           all_in * page / BYTES_PER_KB, all_pages * page / BYTES_PER_KB);
    printf("  tickle_context.tx_buffer (written by tt_Context_init, the control): %zu of %zu\n", tx_in, tx_pages);
    printf("  discovery.entities (nothing writes it with no peer): %zu of %zu\n", ent_in, ent_pages);

    // Control 2: the context's mapping is visible - core wrote tx_buffer, and every page of it reads resident.
    assert(tx_pages > 16);
    assert(tx_in == tx_pages);
    assert(ent_pages > 200);
    assert(ent_in * 10U < ent_pages);
    assert(all_in * 2U < all_pages);

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("context impl resident only where written: PASS\n");
    return 0;
}
