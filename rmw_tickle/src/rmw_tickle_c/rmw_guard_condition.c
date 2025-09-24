// Copyright 2024 TickLE Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <rcutils/logging_macros.h>
#include <rmw/allocators.h>
#include <rmw/error_handling.h>
#include <rmw/rmw.h>

#include "rmw_tickle_c/rmw_tickle.h"

rmw_guard_condition_t* rmw_create_guard_condition(rmw_context_t* context) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, NULL);

    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return NULL;
    }

    // Allocate memory for the guard condition
    rmw_tickle_guard_condition_t* tickle_guard_condition = malloc(sizeof(rmw_tickle_guard_condition_t));
    if (tickle_guard_condition == NULL) {
        RMW_SET_ERROR_MSG("Failed to allocate memory for guard condition");
        return NULL;
    }

    // Initialize the guard condition structure
    memset(tickle_guard_condition, 0, sizeof(rmw_tickle_guard_condition_t));

    // Set up the RMW guard condition structure (embedded in tickle_guard_condition)
    rmw_guard_condition_t* rmw_guard_condition = &tickle_guard_condition->rmw_guard_condition;

    rmw_guard_condition->implementation_identifier = RMW_TICKLE_IDENTIFIER;
    rmw_guard_condition->data = tickle_guard_condition;

    // Initialize allocator
    tickle_guard_condition->allocator = rcutils_get_default_allocator();
    tickle_guard_condition->has_triggered = false;

    RCUTILS_LOG_INFO("Created TickLE guard condition");

    return rmw_guard_condition;
}

rmw_ret_t rmw_destroy_guard_condition(rmw_guard_condition_t* guard_condition) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(guard_condition, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(guard_condition->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_guard_condition_t* tickle_guard_condition = (rmw_tickle_guard_condition_t*)guard_condition->data;
    if (tickle_guard_condition != NULL) {
        free(tickle_guard_condition);
    }

    RCUTILS_LOG_INFO("Destroyed TickLE guard condition");

    return RMW_RET_OK;
}

rmw_ret_t rmw_trigger_guard_condition(const rmw_guard_condition_t* guard_condition) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(guard_condition, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(guard_condition->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_guard_condition_t* tickle_guard_condition = (rmw_tickle_guard_condition_t*)guard_condition->data;
    if (tickle_guard_condition != NULL) {
        tickle_guard_condition->has_triggered = true;
    }

    return RMW_RET_OK;
}
