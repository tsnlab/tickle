/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_unsupported.c: every rmw entry point exists (none left for rmw_implementation to report as a
// missing symbol), rmw_set_log_severity sets TickLE's log level, and the rest answer
// RMW_RET_UNSUPPORTED with a reason - the answer rmw_cyclonedds_cpp/rmw_fastrtps_cpp give for a
// feature they lack, not a crash or a silent RMW_RET_OK.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <tickle/log.h>

#include "rcutils/error_handling.h"
#include "rmw/error_handling.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/types.h"

extern tt_LogLevel tt_current_log_level; // src/log.c - the level rmw_set_log_severity sets

// An UNSUPPORTED answer must carry a reason naming the function, and leave none behind.
static void expect_unsupported(rmw_ret_t ret, const char* name) {
    assert(RMW_RET_UNSUPPORTED == ret);
    assert(rmw_error_is_set());
    assert(NULL != strstr(rmw_get_error_string().str, name));
    rmw_reset_error();
}

int main(void) {
    const struct {
        rmw_log_severity_t severity;
        tt_LogLevel level;
    } mapping[] = {
        {RMW_LOG_SEVERITY_DEBUG, TT_LOG_DEBUG},  {RMW_LOG_SEVERITY_INFO, TT_LOG_INFO},
        {RMW_LOG_SEVERITY_WARN, TT_LOG_WARNING}, {RMW_LOG_SEVERITY_ERROR, TT_LOG_ERROR},
        {RMW_LOG_SEVERITY_FATAL, TT_LOG_ERROR}, // TickLE has nothing above ERROR
    };
    for (size_t i = 0; i < sizeof(mapping) / sizeof(mapping[0]); i++) {
        assert(RMW_RET_OK == rmw_set_log_severity(mapping[i].severity));
        assert(mapping[i].level == tt_current_log_level);
    }
    const int invalid_severity = 7;
    assert(RMW_RET_INVALID_ARGUMENT == rmw_set_log_severity((rmw_log_severity_t)invalid_severity));
    assert(TT_LOG_ERROR == tt_current_log_level); // unchanged by the refusal
    rmw_reset_error();
    assert(RMW_RET_OK == rmw_set_log_severity(RMW_LOG_SEVERITY_INFO));

    // One of each kind: a feature TickLE has no counterpart for, and a real gap.
    rmw_publisher_allocation_t allocation;
    expect_unsupported(rmw_fini_publisher_allocation(&allocation), "rmw_fini_publisher_allocation");
    size_t size = 0;
    expect_unsupported(rmw_get_serialized_message_size(NULL, NULL, &size), "rmw_get_serialized_message_size");
    expect_unsupported(rmw_publish_serialized_message(NULL, NULL, NULL), "rmw_publish_serialized_message");
    expect_unsupported(rmw_subscription_set_on_new_message_callback(NULL, NULL, NULL),
                       "rmw_subscription_set_on_new_message_callback");

    printf("unsupported entry points: PASS\n");
    return 0;
}
