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
#include "rmw/names_and_types.h"
#include "rmw/error_handling.h"
#include "rcutils/error_handling.h"
#include "rcutils/allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

rmw_ret_t
rmw_get_topic_names_and_types(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  bool no_demangle,
  rmw_names_and_types_t * topic_names_and_types)
{
  (void)no_demangle;   // TickLE doesn't support demangling

  // Perform RMW checks
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "Allocator argument is invalid",
    return RMW_RET_INVALID_ARGUMENT);

  if (RMW_RET_OK != rmw_names_and_types_check_zero(topic_names_and_types)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  rmw_tickle_node_t * tickle_node = (rmw_tickle_node_t *)node->data;
  if (tickle_node == NULL) {
    RMW_SET_ERROR_MSG("TickLE node data is NULL");
    return RMW_RET_ERROR;
  }

  // For now, return empty topic list
  // In a real implementation, we would enumerate topics from TickLE discovery
  if (RCUTILS_RET_OK != rmw_names_and_types_init(
      topic_names_and_types, 0, allocator))
  {
    RMW_SET_ERROR_MSG("Failed to initialize topic_names_and_types string array");
    return RMW_RET_ERROR;
  }

  printf("rmw_get_topic_names_and_types: function not fully implemented for TickLE\n");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_get_service_names_and_types(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  rmw_names_and_types_t * service_names_and_types)
{
  // Perform RMW checks
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "Allocator argument is invalid",
    return RMW_RET_INVALID_ARGUMENT);

  if (RMW_RET_OK != rmw_names_and_types_check_zero(service_names_and_types)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  rmw_tickle_node_t * tickle_node = (rmw_tickle_node_t *)node->data;
  if (tickle_node == NULL) {
    RMW_SET_ERROR_MSG("TickLE node data is NULL");
    return RMW_RET_ERROR;
  }

  // For now, return empty service list
  // In a real implementation, we would enumerate services from TickLE discovery
  if (RCUTILS_RET_OK != rmw_names_and_types_init(
      service_names_and_types, 0, allocator))
  {
    RMW_SET_ERROR_MSG("Failed to initialize service_names_and_types string array");
    return RMW_RET_ERROR;
  }

  printf("rmw_get_service_names_and_types: function not fully implemented for TickLE\n");
  return RMW_RET_OK;
}
