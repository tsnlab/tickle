/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROS_DOMAIN_ID (2026-09-27, DDS parity): rmw_init() puts each domain on its own well-known port,
// _tt_NODE_PORT + the domain id, so two domains on one network never discover each other. A PC's unit
// tests (domain 0) were heard by the rig's nodes (domain 73) over a shared LAN before this.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <tickle/config.h> // _tt_CONFIG, _tt_NODE_PORT

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/domain_id.h" // RMW_DEFAULT_DOMAIN_ID
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw_tickle_c/rmw_tickle.h" // RMW_TICKLE_MAX_DOMAIN_ID

#define DOMAIN_SEVEN 7U
#define DOMAIN_FROM_ENV "9"
#define DOMAIN_FROM_ENV_VALUE 9

// rmw_init() with `domain_id`; returns its result and, on success, the well-known port it chose.
static rmw_ret_t init_with_domain(size_t domain_id, int* port) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    options.domain_id = domain_id;
    rmw_context_t context = rmw_get_zero_initialized_context();
    rmw_ret_t ret = rmw_init(&options, &context);
    if (RMW_RET_OK == ret) {
        *port = _tt_CONFIG.port;
        assert(RMW_RET_OK == rmw_shutdown(&context));
        assert(RMW_RET_OK == rmw_context_fini(&context));
    }
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    return ret;
}

int main(void) {
    int port = 0;
    assert(RMW_RET_OK == init_with_domain(0, &port));
    assert(_tt_NODE_PORT == port); // domain 0 keeps the port every TickLE node has always used
    assert(RMW_RET_OK == init_with_domain(DOMAIN_SEVEN, &port));
    assert(_tt_NODE_PORT + (int)DOMAIN_SEVEN == port);
    assert(0 == setenv("ROS_DOMAIN_ID", DOMAIN_FROM_ENV, 1));
    assert(RMW_RET_OK == init_with_domain(RMW_DEFAULT_DOMAIN_ID, &port));
    assert(_tt_NODE_PORT + DOMAIN_FROM_ENV_VALUE == port);
    assert(RMW_RET_INVALID_ARGUMENT == init_with_domain(RMW_TICKLE_MAX_DOMAIN_ID + 1U, &port));
    printf("ROS_DOMAIN_ID as a port offset: PASS\n");
    return 0;
}
