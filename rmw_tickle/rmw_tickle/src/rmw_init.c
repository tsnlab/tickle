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
    // Initializing an already-initialized init_options (implementation_identifier already set by
    // a prior rmw_init_options_init() call that was never rmw_init_options_fini()'d) is a real
    // caller mistake rmw's own contract rejects (test_rmw_implementation's own test_init_options.
    // cpp's "Initializing twice fails"), not something to silently re-stamp over.
    if (NULL != init_options->implementation_identifier) {
        RMW_SET_ERROR_MSG("init_options is already initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }

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
    // implementation_identifier is NULL both on a zero-initialized (never rmw_init_options_init()'d)
    // init_options, and - now that this function itself clears it below - on an already-finalized
    // one (test_rmw_implementation's own test_init_options.cpp's "Finalizing twice fails"). Either
    // way that's a plain invalid argument, not "wrong rmw" (which needs a real, non-null, just
    // incorrect identifier to mean anything) - and not something a bare strcmp() can safely be
    // handed regardless.
    if (NULL == init_options->implementation_identifier) {
        RMW_SET_ERROR_MSG("init_options has already been finalized, or was never initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (strcmp(init_options->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    // rmw_init_options_copy() below is the only place that ever heap-allocates enclave (via
    // rcutils_strdup()) - free it here so a copied (not just a directly-init()'d) init_options
    // doesn't leak it.
    if (init_options->enclave != NULL) {
        allocator->deallocate(init_options->enclave, allocator->state);
        init_options->enclave = NULL;
    }
    init_options->implementation_identifier = NULL;
    return RMW_RET_OK;
}

rmw_ret_t rmw_init_options_copy(const rmw_init_options_t* src, rmw_init_options_t* const dst) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(src, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(dst, RMW_RET_INVALID_ARGUMENT);

    // src->implementation_identifier being NULL means src itself was never initialized - a plain
    // invalid argument (test_rmw_implementation's test_init_options.cpp's copy_with_bad_arguments
    // exercises exactly this, distinct from a real-but-wrong identifier below) - and, same as
    // everywhere else in this file, not safe to hand a bare strcmp() regardless.
    if (NULL == src->implementation_identifier) {
        RMW_SET_ERROR_MSG("src has not been initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
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

    // options->implementation_identifier is NULL if `options` was never passed through
    // rmw_init_options_init() (still zero-initialized) - rmw's own contract (test_rmw_
    // implementation's test_init_shutdown.cpp) treats that as a plain invalid argument, distinct
    // from a non-null identifier that just names the wrong rmw (INCORRECT_RMW_IMPLEMENTATION).
    if (NULL == options->implementation_identifier) {
        RMW_SET_ERROR_MSG("options has not been initialized (implementation_identifier is null)");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (strcmp(options->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    if (NULL == options->enclave) {
        RMW_SET_ERROR_MSG("options->enclave is null");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (NULL != context->impl) {
        RMW_SET_ERROR_MSG("context has already been initialized");
        return RMW_RET_INVALID_ARGUMENT;
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
    impl->allocator = options->allocator;

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
    // Milestone 34 - guards node_count/nodes[] (rmw_tickle_context_impl_t's own doc comment,
    // rmw_tickle.h) - a separate, dedicated mutex from wait_mutex above (a different lock-nesting
    // contract, guarding a different concern, not worth conflating just because both happen to be
    // context-level).
    if (pthread_mutex_init(&impl->registry_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize context registry_mutex");
        pthread_cond_destroy(&impl->wait_cond);
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

    // Same reasoning as TICKLE_BROADCAST_ADDR just above, for _tt_CONFIG.node_id (config.h's own
    // doc comment on that field spells out exactly this scenario): tt_get_node_id()'s auto-detect
    // derives a node's id from the last octet of its own address on the broadcast subnet, which
    // silently collides whenever two rmw_tickle processes share one host/interface (e.g.
    // buildfarm_perf_tests' own "two_process" test topology, both sides on the same CI runner) -
    // each then discards the other's every packet as "self sent" (process_datagram(), tickle.c).
    // Real separate hosts need no override at all; examples/linux/*'s own standalone binaries
    // already expose this as a `-I` CLI flag for exactly the same reason - an rmw plugin has no
    // CLI of its own to extend, so this is the equivalent for anything that launches multiple
    // rmw_tickle nodes on one host (e.g. rmw-perf.yml's own launch template).
    char* node_id = getenv("TICKLE_NODE_ID");
    if (node_id != NULL) {
        _tt_CONFIG.node_id = atoi(node_id);
    }

    return RMW_RET_OK;
}

rmw_ret_t rmw_shutdown(rmw_context_t* context) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

    // context->implementation_identifier is NULL on a zero-initialized (never rmw_init()'d)
    // context - see rmw_init()'s own comment on the same NULL-vs-wrong-identifier distinction.
    if (NULL == context->implementation_identifier) {
        RMW_SET_ERROR_MSG("context has not been initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    // NULL here means either never-initialized-past-the-identifier-check (shouldn't happen - the
    // identifier is only ever set together with impl in rmw_init()) or already-finalized
    // (rmw_context_fini() nulls it) - either way, nothing left to shut down.
    if (NULL == context->impl) {
        RMW_SET_ERROR_MSG("context has already been finalized");
        return RMW_RET_INVALID_ARGUMENT;
    }

    // Idempotent by rmw's own contract (test_init_shutdown.cpp's "Shutdown twice should succeed")
    // - just (re-)mark it, no different work needed the second time.
    ((rmw_tickle_context_impl_t*)context->impl)->shutdown = true;

    return RMW_RET_OK;
}

rmw_ret_t rmw_context_fini(rmw_context_t* const context) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

    // See rmw_init()'s own comment on the same NULL-vs-wrong-identifier distinction -
    // context->implementation_identifier is NULL on a zero-initialized (never rmw_init()'d)
    // context.
    if (NULL == context->implementation_identifier) {
        RMW_SET_ERROR_MSG("context has not been initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    // NULL here means already finalized (this function itself is what nulls it, below) - rmw's
    // own contract requires a second finalization to fail, not silently double-free/no-op
    // (test_init_shutdown.cpp's "Finalization twice should fail").
    if (NULL == context->impl) {
        RMW_SET_ERROR_MSG("context has already been finalized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    // rmw's own contract: shutdown must precede finalization (test_init_shutdown.cpp's
    // "Finalization w/o shutdown should fail") - shutdown itself doesn't null context->impl (it's
    // idempotent and this function is still the one that owns freeing it), so this needs its own
    // flag rather than reusing the impl-null check above.
    if (!((rmw_tickle_context_impl_t*)context->impl)->shutdown) {
        RMW_SET_ERROR_MSG("context must be shut down (rmw_shutdown()) before finalization");
        return RMW_RET_INVALID_ARGUMENT;
    }

    // Free the context implementation. nodes[] itself (Milestone 34) is expected to already be
    // empty/NULL here - the real rmw/rcl lifecycle contract requires every node (and every
    // publisher/subscription/etc. on it) to be destroyed before its own context is finalized, the
    // same pre-existing assumption every other per-entity teardown in this package already relies
    // on - so this just frees whatever allocation nodes[] itself still holds (the array, not its
    // now-empty contents), not a defensive walk-and-destroy of anything still registered.
    rmw_tickle_context_impl_t* impl = (rmw_tickle_context_impl_t*)context->impl;
    if (impl->nodes != NULL) {
        impl->allocator.deallocate((void*)impl->nodes, impl->allocator.state);
    }
    pthread_mutex_destroy(&impl->registry_mutex);
    pthread_cond_destroy(&impl->wait_cond);
    pthread_mutex_destroy(&impl->wait_mutex);
    context->options.allocator.deallocate(context->impl, context->options.allocator.state);
    context->impl = NULL;

    // context->options is only ever a shallow copy (`context->options = *options;`, rmw_init()
    // above) of whatever the caller's own rmw_init_options_t still is - it does NOT own enclave
    // (or anything else heap-allocated in it), so finalizing it here is not this function's job at
    // all: the caller who originally rmw_init_options_init()'d that struct is the one who must
    // rmw_init_options_fini() it, separately, whenever *they're* done with it (which may be well
    // after this context itself is finalized - test_rmw_implementation's own test_init_shutdown.cpp
    // does exactly that in its fixture's TearDown()). This function calling rmw_init_options_fini()
    // on its own borrowed copy used to look harmless only because that function was a no-op past
    // its own identifier check - once it started actually freeing enclave, doing so here as well
    // produced a real double-free the moment a caller (correctly) finalized their own options too.
    context->implementation_identifier = NULL;

    return RMW_RET_OK;
}
