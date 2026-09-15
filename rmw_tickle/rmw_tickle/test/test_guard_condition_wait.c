/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 10: exercises Milestone 5's own guard condition + wait_set design
// for real - rmw_trigger_guard_condition()/rmw_wait() together, including the edge-triggered
// "consumed once observed ready" behavior rmw_wait_set.c's own check_guard_conditions() implements
// (atomic_load while peeking across loop iterations, atomic_exchange only in the one finalizing
// pass right before returning - see that file's own module doc comment). No second node/peer is
// needed for this: a guard condition is entirely local to the process that owns it.

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h" // rmw_enclave_options_copy()
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/time.h" // rmw_time_t
#include "rmw/types.h"

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    // See test_node_lifecycle.c's own comment on this same line - rmw_init() requires a non-NULL
    // enclave (matching test_rmw_implementation's own test_init_shutdown.cpp contract), and it
    // must be a real heap allocation (rmw_init_options_fini() below frees it), not a string
    // literal.
    assert(RMW_RET_OK == rmw_enclave_options_copy("/", &allocator, &options.enclave));

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_guard_condition_wait", "/");
    assert(NULL != node);

    rmw_guard_condition_t* guard_condition = rmw_create_guard_condition(&context);
    assert(NULL != guard_condition);

    rmw_wait_set_t* wait_set = rmw_create_wait_set(&context, 0);
    assert(NULL != wait_set);

    void* guard_conditions_storage[1] = {guard_condition};
    rmw_guard_conditions_t guard_conditions = {
        .guard_condition_count = 1,
        .guard_conditions = guard_conditions_storage,
    };
    rmw_time_t zero_timeout = {0, 0};

    // Not yet triggered: a zero-timeout rmw_wait() must time out and null the entry.
    rmw_ret_t ret = rmw_wait(NULL, &guard_conditions, NULL, NULL, NULL, wait_set, &zero_timeout);
    assert(RMW_RET_TIMEOUT == ret);
    assert(NULL == guard_conditions.guard_conditions[0]);

    // Triggered: rmw_wait() must return OK, keeping the entry non-NULL.
    guard_conditions.guard_conditions[0] = guard_condition;
    assert(RMW_RET_OK == rmw_trigger_guard_condition(guard_condition));
    ret = rmw_wait(NULL, &guard_conditions, NULL, NULL, NULL, wait_set, &zero_timeout);
    assert(RMW_RET_OK == ret);
    assert(NULL != guard_conditions.guard_conditions[0]);

    // Edge-triggered: consumed by the rmw_wait() call above - without a fresh trigger, the very
    // next call must find nothing ready again.
    guard_conditions.guard_conditions[0] = guard_condition;
    ret = rmw_wait(NULL, &guard_conditions, NULL, NULL, NULL, wait_set, &zero_timeout);
    assert(RMW_RET_TIMEOUT == ret);
    assert(NULL == guard_conditions.guard_conditions[0]);

    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_guard_condition(guard_condition));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("guard condition + wait_set: PASS\n");
    return 0;
}
