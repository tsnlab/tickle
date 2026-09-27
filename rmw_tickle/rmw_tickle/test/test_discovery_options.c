/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g6 (RMW_GAPS_PLAN.md): rmw_init's reading of rmw_discovery_options_t, as rmw_cyclonedds reads it. Checked:
// rmw_init_options_init's default is LOCALHOST (as the vendors'); LOCALHOST binds and broadcasts on loopback;
// static peers become peer links - a host name looked up, a subnet kept, IPv6 skipped; OFF and SYSTEM_DEFAULT ignore
// the peers; NOT_SET lets rmw_init through and refuses rmw_create_node, as the vendors do.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <tickle/config.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/discovery_options.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"

static rmw_init_options_t options;
static rmw_context_t context;

static void start(rmw_automatic_discovery_range_t range, const char* const* peers, size_t peer_count) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    assert(RMW_AUTOMATIC_DISCOVERY_RANGE_LOCALHOST == options.discovery_options.automatic_discovery_range);
    options.enclave = rcutils_strdup("/", allocator);
    options.discovery_options.automatic_discovery_range = range;
    if (peer_count > 0) {
        assert(RMW_RET_OK == rmw_discovery_options_fini(&options.discovery_options));
        assert(RMW_RET_OK == rmw_discovery_options_init(&options.discovery_options, peer_count, &allocator));
        options.discovery_options.automatic_discovery_range = range;
        for (size_t i = 0; i < peer_count; i++) {
            (void)snprintf(options.discovery_options.static_peers[i].peer_address,
                           sizeof(options.discovery_options.static_peers[i].peer_address), "%s", peers[i]);
        }
    }
    context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
}

static void stop(void) {
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
}

int main(void) {
    static const char* const peers[] = {"localhost", "10.9.9.0/24", "::1"};

    // LOCALHOST with peers: loopback, the data socket open to the peers, two peer links (IPv6 skipped).
    start(RMW_AUTOMATIC_DISCOVERY_RANGE_LOCALHOST, peers, 3);
    assert(tt_DISCOVERY_RANGE_LOCALHOST == _tt_CONFIG.discovery_range);
    assert(0 == strcmp("127.255.255.255", _tt_CONFIG.broadcast) && 0 == strcmp("0.0.0.0", _tt_CONFIG.addr));
    assert(3 == _tt_CONFIG.link_count && !_tt_CONFIG.links[0].peer);
    assert(_tt_CONFIG.links[1].peer && 0 == strcmp("127.0.0.1", _tt_CONFIG.links[1].broadcast));
    assert(_tt_CONFIG.links[2].peer && 0 == strcmp("10.9.9.0/24", _tt_CONFIG.links[2].broadcast));
    stop();

    // LOCALHOST alone: bound to loopback.
    start(RMW_AUTOMATIC_DISCOVERY_RANGE_LOCALHOST, NULL, 0);
    assert(0 == strcmp("127.0.0.1", _tt_CONFIG.addr) && 0 == _tt_CONFIG.link_count);
    stop();

    // OFF and SYSTEM_DEFAULT: the peers are ignored.
    start(RMW_AUTOMATIC_DISCOVERY_RANGE_OFF, peers, 3);
    assert(tt_DISCOVERY_RANGE_OFF == _tt_CONFIG.discovery_range && 0 == _tt_CONFIG.link_count);
    stop();
    start(RMW_AUTOMATIC_DISCOVERY_RANGE_SYSTEM_DEFAULT, peers, 3);
    assert(tt_DISCOVERY_RANGE_SUBNET == _tt_CONFIG.discovery_range && 0 == _tt_CONFIG.link_count);
    stop();

    // SUBNET with peers: the links, plus the peers.
    start(RMW_AUTOMATIC_DISCOVERY_RANGE_SUBNET, peers, 3);
    assert(tt_DISCOVERY_RANGE_SUBNET == _tt_CONFIG.discovery_range && 3 == _tt_CONFIG.link_count);
    stop();

    // NOT_SET: rmw_init passes, rmw_create_node refuses - what the vendors do.
    start(RMW_AUTOMATIC_DISCOVERY_RANGE_NOT_SET, NULL, 0);
    assert(NULL == rmw_create_node(&context, "g6_node", "/"));
    assert(NULL != strstr(rmw_get_error_string().str, "no automatic discovery range was given"));
    rmw_reset_error();
    stop();

    printf("test_discovery_options: PASS\n");
    return 0;
}
