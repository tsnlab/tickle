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
// rmw_context_fini() the count of threads still LIVE must be back to the baseline.
//
// Live, not listed (2026-10-09). pthread_join() returns when the kernel clears the joined thread's
// tid, which it does inside do_exit() - after marking the task PF_EXITING, before the task leaves
// /proc/self/task. A joined thread therefore stays listed for a moment, and under CPU load that
// moment failed this test 10 runs in 200: the extra entry was the watchdog thread, PF_EXITING set,
// gone 1 ms later. A thread past pthread_join() can be in no other state, and a thread nobody
// stopped is never in it, so an exiting thread is not counted - and one still running still fails
// the check, at once and without waiting for anything.

#include <assert.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"

#define PF_EXITING_FLAG 0x00000004UL // include/linux/sched.h: the task has entered do_exit()
#define STAT_SPACES_TO_FLAGS 7       // after comm's ")": state ppid pgrp session tty_nr tpgid, then flags
#define STAT_LINE_BYTES 1024
#define PATH_BYTES 64
#define DECIMAL 10

// Whether /proc/self/task/<tid> is a thread that has entered do_exit(). One that vanished between the
// directory read and this one has exited too.
static bool exiting(const char* tid) {
    char path[PATH_BYTES];
    (void)snprintf(path, sizeof(path), "/proc/self/task/%s/stat", tid);
    FILE* stat = fopen(path, "r");
    if (NULL == stat) {
        return true;
    }
    char line[STAT_LINE_BYTES];
    bool read = NULL != fgets(line, sizeof(line), stat);
    (void)fclose(stat);
    assert(read);
    const char* field = strrchr(line, ')'); // comm may itself hold spaces and parentheses
    assert(NULL != field);
    for (int i = 0; i < STAT_SPACES_TO_FLAGS; i++) {
        field = strchr(field + 1, ' ');
        assert(NULL != field);
    }
    unsigned long flags = strtoul(field + 1, NULL, DECIMAL);
    return 0 != (flags & PF_EXITING_FLAG);
}

// This process's threads; with `live_only`, not counting those already exiting.
static int thread_count(bool live_only) {
    DIR* tasks = opendir("/proc/self/task");
    assert(NULL != tasks);
    int count = 0;
    for (const struct dirent* entry = readdir(tasks); entry != NULL; entry = readdir(tasks)) {
        if (entry->d_name[0] != '.' && !(live_only && exiting(entry->d_name))) {
            count++;
        }
    }
    closedir(tasks);
    return count;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0); // the counts line survives the assert after it
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    int baseline = thread_count(true);
    // Nothing has been stopped yet, so the exiting-thread rule must not be hiding anything here.
    assert(baseline == thread_count(false));
    rmw_node_t* node = rmw_create_node(&context, "test_context_fini_leaked_node", "/");
    assert(NULL != node);
    int running = thread_count(true);
    // The control: the node really did start threads, so "back to baseline" below means something.
    assert(running > baseline);

    // The node is deliberately never destroyed.
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    int listed = thread_count(false);
    int after = thread_count(true);
    printf("threads: %d before the node, %d with it, %d live after finalizing its context (%d listed, the rest "
           "exiting)\n",
           baseline, running, after, listed);
    assert(after == baseline);

    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("context fini with a leaked node: PASS\n");
    return 0;
}
