// Copyright 2024 TickLE RMW Implementation
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw_tickle_c/rmw_tickle.h"

rmw_node_t* rmw_create_node(rmw_context_t* context, const char* name, const char* namespace_) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(name, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(namespace_, NULL);

    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        RCUTILS_LOG_ERROR("Implementation identifier mismatch. Expected: %s, Got: %s", RMW_TICKLE_IDENTIFIER,
                          context->implementation_identifier);
        return NULL;
    }

    if (!name || strlen(name) == 0) {
        RMW_SET_ERROR_MSG("Node name cannot be null or empty");
        RCUTILS_LOG_ERROR("Node name is null or empty");
        return NULL;
    }

    if (!namespace_ || strlen(namespace_) == 0) {
        RMW_SET_ERROR_MSG("Node namespace cannot be null or empty");
        RCUTILS_LOG_ERROR("Node namespace is null or empty");
        return NULL;
    }

    // Allocate memory for the node
    rmw_tickle_node_t* tickle_node = malloc(sizeof(rmw_tickle_node_t));
    if (tickle_node == NULL) {
        RMW_SET_ERROR_MSG("Failed to allocate memory for node");
        RCUTILS_LOG_ERROR("Failed to allocate memory for TickLE node");
        return NULL;
    }

    // Initialize the TickLE node structure
    memset(tickle_node, 0, sizeof(rmw_tickle_node_t));

    // Initialize allocator
    tickle_node->allocator = rcutils_get_default_allocator();

    // Store context reference (cast to impl type)
    tickle_node->context = (const rmw_context_t*)context;

    // Set up the RMW node structure (embedded in tickle_node)
    rmw_node_t* rmw_node = &tickle_node->rmw_node;

    rmw_node->implementation_identifier = RMW_TICKLE_IDENTIFIER;
    rmw_node->data = tickle_node;
    rmw_node->name = rcutils_strdup(name, tickle_node->allocator);

    // Ensure namespace starts with '/'
    if (namespace_[0] != '/') {
        // This should not happen as ROS2 should provide absolute namespaces
        RCUTILS_LOG_WARN("Namespace '%s' does not start with '/', this may cause issues", namespace_);
    }
    rmw_node->namespace_ = rcutils_strdup(namespace_, tickle_node->allocator);

    // Initialize TickLE node
    int32_t result = tt_Node_create(&tickle_node->tickle_node);
    if (result != 0) {
        free(tickle_node);
        RMW_SET_ERROR_MSG("Failed to create TickLE node");
        RCUTILS_LOG_ERROR("Failed to create TickLE node, error code: %d", result);
        return NULL;
    }

    RCUTILS_LOG_INFO("Created TickLE node: %s%s", namespace_, name);

    return rmw_node;
}

rmw_ret_t rmw_destroy_node(rmw_node_t* node) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_node_t* tickle_node = (rmw_tickle_node_t*)node->data;
    if (tickle_node != NULL) {
        // Destroy TickLE node
        int32_t result = tt_Node_destroy(&tickle_node->tickle_node);
        if (result != 0) {
            RCUTILS_LOG_WARN("Failed to destroy TickLE node, error code: %d", result);
        }

        tickle_node->allocator.deallocate(node->name, tickle_node->allocator.state);
        tickle_node->allocator.deallocate(node->namespace_, tickle_node->allocator.state);

        free(tickle_node);
    }

    RCUTILS_LOG_INFO("Destroyed TickLE node: %s%s", node->namespace_ ? node->namespace_ : "",
                     node->name ? node->name : "");

    return RMW_RET_OK;
}

const rmw_guard_condition_t* rmw_node_get_graph_guard_condition(const rmw_node_t* node) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, NULL);

    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return NULL;
    }

    rmw_tickle_node_t* tickle_node = (rmw_tickle_node_t*)node->data;
    if (tickle_node == NULL) {
        RMW_SET_ERROR_MSG("Node data is null");
        return NULL;
    }

    const rmw_context_t* context = tickle_node->context;
    if (context == NULL) {
        RMW_SET_ERROR_MSG("Context is null");
        return NULL;
    }

    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)context->impl;
    return &context_impl->graph_guard_condition;
}
