/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A context finalized while a node on it was never destroyed must leave nothing running. rclcpp
// does exactly this when a Node constructor throws after rcl_node_init() succeeded, and rmw_tickle
// used to free the context under its still-running poll thread, which crashed the process on exit
// (rmw_tickle_stop_leaked_nodes(), rmw_node.c).
//
// Checked by counting this process's threads, not by waiting for a crash: a use-after-free need
// not fault, and a test that passes because it happened not to is no test. The count before
// creating the node is the baseline; creating it starts the poll and watchdog threads; after
// rmw_context_fini() the count must be back to the baseline.

#include <assert.h>
#include <dirent.h>
#include <stdio.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"

static int thread_count(void) {
    DIR* tasks = opendir("/proc/self/task");
    assert(NULL != tasks);
    int count = 0;
    for (const struct dirent* entry = readdir(tasks); entry != NULL; entry = readdir(tasks)) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    closedir(tasks);
    return count;
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    int baseline = thread_count();
    rmw_node_t* node = rmw_create_node(&context, "test_context_fini_leaked_node", "/");
    assert(NULL != node);
    int running = thread_count();
    // The control: the node really did start threads, so "back to baseline" below means something.
    assert(running > baseline);

    // The node is deliberately never destroyed.
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    int after = thread_count();
    printf("threads: %d before the node, %d with it, %d after finalizing its context\n", baseline, running, after);
    assert(after == baseline);

    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("context fini with a leaked node: PASS\n");
    return 0;
}
