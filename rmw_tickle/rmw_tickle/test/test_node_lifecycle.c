/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 10: the first real end-to-end exercise of rmw_init()/rmw_create_
// node()/rmw_destroy_node()/rmw_shutdown()/rmw_context_fini() together - every prior milestone
// proved its own piece compiles and links against real rmw/rosidl headers (check-all.yml), but
// none actually *ran* rmw_init() until this test (which is what surfaced rmw_get_zero_initialized_
// context() missing entirely - see rmw_init.c). A real struct tt_Node is created and destroyed
// here (binding a real UDP socket, starting and stopping a real poll thread) - no second node/peer
// is needed for this, unlike a publish/subscribe/service round trip (out of scope - see this
// milestone's own "not solved here" note in PLAN.md).

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h" // rmw_enclave_options_copy()
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    // rmw_init_options_init() itself leaves enclave NULL (a real caller, e.g. rcl_init(), always
    // sets one before calling rmw_init() - rmw_init() rejects a NULL enclave, matching test_rmw_
    // implementation's own test_init_shutdown.cpp). A real heap allocation via rmw_enclave_options_
    // copy() (a plain string-copy utility rmw itself implements, not something rmw_tickle needs to)
    // - not a string literal: rmw_init_options_fini() below does free it, matching a real
    // rcl_init()'s own enclave ownership.
    assert(RMW_RET_OK == rmw_enclave_options_copy("/", &allocator, &options.enclave));

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_node_lifecycle", "/");
    assert(NULL != node);

    const rmw_guard_condition_t* graph_guard_condition = rmw_node_get_graph_guard_condition(node);
    assert(NULL != graph_guard_condition);

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("rmw node lifecycle: PASS\n");
    return 0;
}
