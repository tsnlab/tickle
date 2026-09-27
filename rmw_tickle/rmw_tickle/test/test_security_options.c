/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// (g7, CONTEXT_NODE_PLAN.md roadmap, 2026-09-27) rmw_tickle implements no ROS 2 security. With
// ROS_SECURITY_ENFORCEMENT=Enforce - rcl's enforce_security - rmw_init() must refuse to start rather than run
// unsecured without a word, as it did; with security enabled but permissive it starts and says once that security is
// not applied; by default it starts and says nothing.

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rcutils/allocator.h"
#include "rcutils/logging.h"
#include "rcutils/strdup.h"
#include "rcutils/time.h"
#include "rcutils/types/rcutils_ret.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/ret_types.h"
#include "rmw/security_options.h"

static int security_notices;

static void count_security_notice(const rcutils_log_location_t* location, int severity, const char* name,
                                  rcutils_time_point_value_t timestamp, const char* format, va_list* args) {
    (void)location;
    (void)severity;
    (void)timestamp;
    (void)args;
    if (NULL != name && 0 == strcmp(name, "rmw_tickle") && NULL != strstr(format, "does not implement it")) {
        security_notices++;
    }
}

// rmw_init() with the given enforcement and keystore (NULL: security not enabled); its result.
static rmw_ret_t init_with_security(enum rmw_security_enforcement_policy_e enforce, const char* keystore) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    options.security_options.enforce_security = enforce;
    if (NULL != keystore) {
        options.security_options.security_root_path = rcutils_strdup(keystore, allocator);
    }
    rmw_context_t context = rmw_get_zero_initialized_context();
    rmw_ret_t ret = rmw_init(&options, &context);
    if (RMW_RET_OK == ret) {
        assert(RMW_RET_OK == rmw_shutdown(&context));
        assert(RMW_RET_OK == rmw_context_fini(&context));
    } else {
        assert(NULL == context.impl); // refused before anything was set up
        rmw_reset_error();
    }
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    return ret;
}

int main(void) {
    assert(RCUTILS_RET_OK == rcutils_logging_initialize());
    rcutils_logging_set_output_handler(count_security_notice);

    // Default: no security asked for - starts, says nothing.
    assert(RMW_RET_OK == init_with_security(RMW_SECURITY_ENFORCEMENT_PERMISSIVE, NULL));
    assert(0 == security_notices);

    // Enforce: refused, whether or not a keystore was found.
    assert(RMW_RET_OK != init_with_security(RMW_SECURITY_ENFORCEMENT_ENFORCE, "/keystore"));
    assert(RMW_RET_OK != init_with_security(RMW_SECURITY_ENFORCEMENT_ENFORCE, NULL));

    // Enabled but permissive: starts, and says so once - not on every rmw_init().
    assert(RMW_RET_OK == init_with_security(RMW_SECURITY_ENFORCEMENT_PERMISSIVE, "/keystore"));
    assert(RMW_RET_OK == init_with_security(RMW_SECURITY_ENFORCEMENT_PERMISSIVE, "/keystore"));
    assert(1 == security_notices);

    printf("test_security_options: PASS\n");
    return 0;
}
