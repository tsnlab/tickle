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
#include "rcutils/allocator.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// Constants
#define TICKLE_MAX_ENDPOINT_COUNT 256

rmw_ret_t
rmw_get_node_names(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces)
{
  // Perform RMW checks
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  if (node_names == NULL || node_namespaces == NULL) {
    RMW_SET_ERROR_MSG("node_names or node_namespaces is NULL");
    return RMW_RET_INVALID_ARGUMENT;
  }

  rmw_tickle_node_t * tickle_node = (rmw_tickle_node_t *)node->data;
  if (tickle_node == NULL) {
    RMW_SET_ERROR_MSG("TickLE node data is NULL");
    return RMW_RET_ERROR;
  }

  // Count discovered nodes from TickLE updates
  size_t discovered_node_count = 0;
  for (int i = 0; i < TICKLE_MAX_ENDPOINT_COUNT; i++) {
    if (tickle_node->tickle_node.updates[i] != NULL) {
      discovered_node_count++;
    }
  }

  // Initialize string arrays
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  if (RCUTILS_RET_OK != rcutils_string_array_init(
      node_names, discovered_node_count, &allocator))
  {
    RMW_SET_ERROR_MSG("Failed to initialize node_names string array");
    return RMW_RET_ERROR;
  }

  if (RCUTILS_RET_OK != rcutils_string_array_init(
      node_namespaces, discovered_node_count, &allocator))
  {
    rcutils_string_array_fini(node_names);
    RMW_SET_ERROR_MSG("Failed to initialize node_namespaces string array");
    return RMW_RET_ERROR;
  }

  // Extract node information from TickLE updates
  size_t node_index = 0;
  for (int i = 0; i < TICKLE_MAX_ENDPOINT_COUNT && node_index < discovered_node_count; i++) {
    struct tt_UpdateHeader * update = tickle_node->tickle_node.updates[i];
    if (update == NULL) {
      continue;
    }

    // For now, we'll use the node ID as both name and namespace
    // In a real implementation, we would parse the update data to extract
    // actual node names and namespaces from the discovered entities
    char node_name[32];
    char node_namespace[32];

    snprintf(node_name, sizeof(node_name), "tickle_node_%d", i);
    snprintf(node_namespace, sizeof(node_namespace), "/");

    // Allocate and copy node name
    size_t name_len = strlen(node_name) + 1;
    node_names->data[node_index] = allocator.zero_allocate(
      name_len, sizeof(char), allocator.state);
    if (node_names->data[node_index] == NULL) {
      // Clean up on error
      for (size_t j = 0; j < node_index; j++) {
        allocator.deallocate(node_names->data[j], allocator.state);
        allocator.deallocate(node_namespaces->data[j], allocator.state);
      }
      (void)rcutils_string_array_fini(node_names);
      (void)rcutils_string_array_fini(node_namespaces);
      RMW_SET_ERROR_MSG("Failed to allocate memory for node name");
      return RMW_RET_ERROR;
    }
    strncpy(node_names->data[node_index], node_name, name_len - 1);

    // Allocate and copy node namespace
    size_t namespace_len = strlen(node_namespace) + 1;
    node_namespaces->data[node_index] = allocator.zero_allocate(
      namespace_len, sizeof(char), allocator.state);
    if (node_namespaces->data[node_index] == NULL) {
      // Clean up on error
      allocator.deallocate(node_names->data[node_index], allocator.state);
      for (size_t j = 0; j < node_index; j++) {
        allocator.deallocate(node_names->data[j], allocator.state);
        allocator.deallocate(node_namespaces->data[j], allocator.state);
      }
      (void)rcutils_string_array_fini(node_names);
      (void)rcutils_string_array_fini(node_namespaces);
      RMW_SET_ERROR_MSG("Failed to allocate memory for node namespace");
      return RMW_RET_ERROR;
    }
    strncpy(node_namespaces->data[node_index], node_namespace, namespace_len - 1);

    node_index++;
  }

  printf("rmw_get_node_names: Found %zu discovered nodes\n", discovered_node_count);
  return RMW_RET_OK;
}

rmw_ret_t
rmw_get_node_names_with_enclaves(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces,
  rcutils_string_array_t * enclaves)
{
  (void)enclaves;   // TickLE doesn't support enclaves
  return rmw_get_node_names(node, node_names, node_namespaces);
}
