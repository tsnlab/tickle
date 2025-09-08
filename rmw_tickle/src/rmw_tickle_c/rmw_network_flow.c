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
#include "rmw/network_flow_endpoint.h"
#include "rmw/get_network_flow_endpoints.h"
#include "rcutils/error_handling.h"

#include <stdio.h>

rmw_ret_t
rmw_publisher_get_network_flow_endpoints(
  const rmw_publisher_t * publisher,
  rcutils_allocator_t * allocator,
  rmw_network_flow_endpoint_array_t * network_flow_endpoint_array)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(network_flow_endpoint_array, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(publisher->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have network flow endpoint support in TickLE
  network_flow_endpoint_array->size = 0;
//   network_flow_endpoint_array->endpoints = NULL;
  
  printf("rmw_publisher_get_network_flow_endpoints: returning empty array (not implemented for TickLE)\n");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_subscription_get_network_flow_endpoints(
  const rmw_subscription_t * subscription,
  rcutils_allocator_t * allocator,
  rmw_network_flow_endpoint_array_t * network_flow_endpoint_array)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(network_flow_endpoint_array, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(subscription->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have network flow endpoint support in TickLE
  network_flow_endpoint_array->size = 0;
//   network_flow_endpoint_array->endpoints = NULL;
  
  printf("rmw_subscription_get_network_flow_endpoints: returning empty array (not implemented for TickLE)\n");
  return RMW_RET_OK;
}
