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

#include "rmw_tickle_c/rmw_tickle.h"
#include "rmw/rmw.h"
#include "rmw/error_handling.h"
#include "rcutils/error_handling.h"
#include "rcutils/logging_macros.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

rmw_node_t *
rmw_create_node(
  rmw_context_t * context,
  const char * name,
  const char * namespace_)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, NULL);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(name, NULL);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(namespace_, NULL);

  if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    RCUTILS_LOG_ERROR("Implementation identifier mismatch. Expected: %s, Got: %s", 
                      RMW_TICKLE_IDENTIFIER, context->implementation_identifier);
    return NULL;
  }

  if (strlen(name) == 0) {
    RMW_SET_ERROR_MSG("Node name cannot be empty");
    RCUTILS_LOG_ERROR("Node name is empty");
    return NULL;
  }

  if (strlen(namespace_) == 0) {
    RMW_SET_ERROR_MSG("Node namespace cannot be empty");
    RCUTILS_LOG_ERROR("Node namespace is empty");
    return NULL;
  }

  // Allocate memory for the node
  rmw_tickle_node_t * tickle_node = malloc(sizeof(rmw_tickle_node_t));
  if (tickle_node == NULL) {
    RMW_SET_ERROR_MSG("Failed to allocate memory for node");
    RCUTILS_LOG_ERROR("Failed to allocate memory for TickLE node");
    return NULL;
  }

  // Initialize the TickLE node structure
  memset(tickle_node, 0, sizeof(rmw_tickle_node_t));
  
  // Set up the RMW node structure
  rmw_node_t * rmw_node = malloc(sizeof(rmw_node_t));
  if (rmw_node == NULL) {
    free(tickle_node);
    RMW_SET_ERROR_MSG("Failed to allocate memory for RMW node");
    RCUTILS_LOG_ERROR("Failed to allocate memory for RMW node");
    return NULL;
  }

  rmw_node->implementation_identifier = RMW_TICKLE_IDENTIFIER;
  rmw_node->data = tickle_node;
  rmw_node->name = name;
  rmw_node->namespace_ = namespace_;

  // Initialize TickLE node (placeholder - actual implementation would go here)
  // tt_Node_init(&tickle_node->tickle_node, ...);

  RCUTILS_LOG_INFO("Created TickLE node: %s%s", namespace_, name);

  return rmw_node;
}

rmw_ret_t
rmw_destroy_node(rmw_node_t * node)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_node_t * tickle_node = (rmw_tickle_node_t *)node->data;
  if (tickle_node != NULL) {
    // Cleanup TickLE node (placeholder - actual implementation would go here)
    // tt_Node_fini(&tickle_node->tickle_node);
    free(tickle_node);
  }

  free(node);

  RCUTILS_LOG_INFO("Destroyed TickLE node: %s%s", node->namespace_, node->name);

  return RMW_RET_OK;
}

const rmw_guard_condition_t *
rmw_node_get_graph_guard_condition(
  const rmw_node_t * node)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, NULL);

  if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return NULL;
  }

  // For now, we don't support graph guard conditions in TickLE
  // This is a placeholder implementation that returns NULL
  RCUTILS_LOG_DEBUG("rmw_node_get_graph_guard_condition: function not implemented for TickLE");
  return NULL;
}
