/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <pthread.h> // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own <pthread.h> comment
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"             // rmw_context_t, rmw_context_impl_t
#include "rmw/init_options.h"     // rmw_init_options_t
#include "rmw/ret_types.h"        // rmw_ret_t, RMW_RET_*
#include "rmw/rmw.h"              // rmw_init/_shutdown/_context_fini, rmw_get_implementation_identifier, ...
#include "rmw/security_options.h" // rmw_security_options_t
#include "rmw_tickle_c/rmw_tickle.h"

const char* const rmw_tickle_identifier = RMW_TICKLE_IDENTIFIER;
const char* const rmw_tickle_serialization_format = RMW_TICKLE_SERIALIZATION_FORMAT;

const char* rmw_get_implementation_identifier(void) {
    return RMW_TICKLE_IDENTIFIER;
}

const char* rmw_get_serialization_format(void) {
    return RMW_TICKLE_SERIALIZATION_FORMAT;
}

rmw_init_options_t rmw_get_zero_initialized_init_options(void) {
    rmw_init_options_t init_options;
    memset(&init_options, 0, sizeof(init_options));
    return init_options;
}

// Missing from the #20-era scaffold, same as rmw_get_serialization_format() was (Milestone 2) -
// rmw_init()'s own real contract (rmw/init.h) requires the caller to have zero-initialized
// `context` this way first; never actually exercised until Milestone 10's own rmw_init()-calling
// tests surfaced the gap.
rmw_context_t rmw_get_zero_initialized_context(void) {
    rmw_context_t context;
    memset(&context, 0, sizeof(context));
    return context;
}

rmw_ret_t rmw_init_options_init(rmw_init_options_t* const init_options, rcutils_allocator_t allocator) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator.allocate, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator.deallocate, RMW_RET_INVALID_ARGUMENT);

    init_options->instance_id = 0;
    init_options->domain_id = 0;
    // Initialize security options to zero
    memset(&init_options->security_options, 0, sizeof(rmw_security_options_t));
    init_options->enclave = NULL;
    init_options->allocator = allocator;
    init_options->impl = NULL;

    // Set the implementation identifier
    init_options->implementation_identifier = RMW_TICKLE_IDENTIFIER;

    return RMW_RET_OK;
}

rmw_ret_t rmw_init_options_fini(rmw_init_options_t* init_options) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
    rcutils_allocator_t* allocator = &init_options->allocator;
    RCUTILS_CHECK_ALLOCATOR(allocator, return RMW_RET_INVALID_ARGUMENT);
    if (strcmp(init_options->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_init_options_copy(const rmw_init_options_t* src, rmw_init_options_t* const dst) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(src, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(dst, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(src->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    if (NULL != dst->implementation_identifier) {
        RMW_SET_ERROR_MSG("expected zero-initialized dst");
        return RMW_RET_INVALID_ARGUMENT;
    }

    // Copy the basic structure
    memcpy(dst, src, sizeof(rmw_init_options_t));

    // Copy the enclave string if it exists
    if (src->enclave != NULL) {
        dst->enclave = rcutils_strdup(src->enclave, src->allocator);
        if (NULL == dst->enclave) {
            return RMW_RET_BAD_ALLOC;
        }
    } else {
        dst->enclave = NULL;
    }

    return RMW_RET_OK;
}

rmw_ret_t rmw_init(const rmw_init_options_t* options, rmw_context_t* const context) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(options, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(options->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    context->instance_id = 0;
    context->implementation_identifier = RMW_TICKLE_IDENTIFIER;
    context->options = *options;

    // Allocate and initialize context implementation
    rmw_tickle_context_impl_t* impl = (rmw_tickle_context_impl_t*)options->allocator.allocate(
        sizeof(rmw_tickle_context_impl_t), options->allocator.state);
    if (impl == NULL) {
        RMW_SET_ERROR_MSG("Failed to allocate context implementation");
        return RMW_RET_BAD_ALLOC;
    }

    // Initialize the context implementation
    memset(impl, 0, sizeof(rmw_tickle_context_impl_t));

    if (pthread_mutex_init(&impl->wait_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize context wait_mutex");
        options->allocator.deallocate(impl, options->allocator.state);
        return RMW_RET_ERROR;
    }
    if (pthread_cond_init(&impl->wait_cond, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize context wait_cond");
        pthread_mutex_destroy(&impl->wait_mutex);
        options->allocator.deallocate(impl, options->allocator.state);
        return RMW_RET_ERROR;
    }

    // rmw_tickle/PLAN.md's Milestone 5: safely waitable from the start (rmw_wait() never crashes
    // seeing it in a wait set); actually triggered on a real graph change is Milestone 6, not yet
    // wired up - see rmw_tickle_context_impl_t's own doc comment.
    impl->graph_guard_condition.rmw_guard_condition.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    impl->graph_guard_condition.rmw_guard_condition.data = &impl->graph_guard_condition;
    impl->graph_guard_condition.rmw_guard_condition.context = context;
    impl->graph_guard_condition.context_impl = impl;
    atomic_init(&impl->graph_guard_condition.has_triggered, false);
    impl->graph_guard_condition.allocator = options->allocator;

    context->impl = (rmw_context_impl_t*)impl;

    // _tt_CONFIG is one process-wide TickLE HAL config (include/tickle/config.h), not something
    // rmw_init_options_t carries a slot for - a hardcoded subnet here would silently break every
    // ROS 2 graph not on that exact network. TICKLE_BROADCAST_ADDR lets a deployment override the
    // HAL's own compiled-in default (see src/hal_linux.c / src/hal_freertos.c) the same way
    // examples/linux's drivers let `-b` do it for a CLI-driven process; unset, this leaves that
    // default untouched instead of guessing.
    char* broadcast_addr = getenv("TICKLE_BROADCAST_ADDR");
    if (broadcast_addr != NULL) {
        _tt_CONFIG.broadcast = broadcast_addr;
    }

    return RMW_RET_OK;
}

rmw_ret_t rmw_shutdown(rmw_context_t* context) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    return RMW_RET_OK;
}

rmw_ret_t rmw_context_fini(rmw_context_t* const context) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    // Free the context implementation
    if (context->impl != NULL) {
        rmw_tickle_context_impl_t* impl = (rmw_tickle_context_impl_t*)context->impl;
        pthread_cond_destroy(&impl->wait_cond);
        pthread_mutex_destroy(&impl->wait_mutex);
        context->options.allocator.deallocate(context->impl, context->options.allocator.state);
        context->impl = NULL;
    }

    // Finalize the init options
    rmw_ret_t ret = rmw_init_options_fini(&context->options);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    return RMW_RET_OK;
}
