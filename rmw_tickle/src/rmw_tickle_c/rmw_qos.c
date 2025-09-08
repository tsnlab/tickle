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

#include <stdio.h>

rmw_ret_t
rmw_client_request_publisher_get_actual_qos(
  const rmw_client_t * client,
  rmw_qos_profile_t * qos)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have actual QoS information in TickLE
  // Return default QoS profile
  qos->reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  qos->durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
  qos->history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  qos->depth = 10;
  qos->avoid_ros_namespace_conventions = false;

  printf("rmw_client_request_publisher_get_actual_qos: returning default QoS\n");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_client_response_subscription_get_actual_qos(
  const rmw_client_t * client,
  rmw_qos_profile_t * qos)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have actual QoS information in TickLE
  // Return default QoS profile
  qos->reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  qos->durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
  qos->history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  qos->depth = 10;
  qos->avoid_ros_namespace_conventions = false;

  printf("rmw_client_response_subscription_get_actual_qos: returning default QoS\n");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_publisher_get_actual_qos(
  const rmw_publisher_t * publisher,
  rmw_qos_profile_t * qos)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(publisher->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have actual QoS information in TickLE
  // Return default QoS profile
  qos->reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  qos->durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
  qos->history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  qos->depth = 10;
  qos->avoid_ros_namespace_conventions = false;

  printf("rmw_publisher_get_actual_qos: returning default QoS\n");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_subscription_get_actual_qos(
  const rmw_subscription_t * subscription,
  rmw_qos_profile_t * qos)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(subscription->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have actual QoS information in TickLE
  // Return default QoS profile
  qos->reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  qos->durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
  qos->history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  qos->depth = 10;
  qos->avoid_ros_namespace_conventions = false;

  printf("rmw_subscription_get_actual_qos: returning default QoS\n");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_service_request_subscription_get_actual_qos(
  const rmw_service_t * service,
  rmw_qos_profile_t * qos)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(service->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have actual QoS information in TickLE
  // Return default QoS profile
  qos->reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  qos->durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
  qos->history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  qos->depth = 10;
  qos->avoid_ros_namespace_conventions = false;

  printf("rmw_service_request_subscription_get_actual_qos: returning default QoS\n");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_service_response_publisher_get_actual_qos(
  const rmw_service_t * service,
  rmw_qos_profile_t * qos)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(service->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have actual QoS information in TickLE
  // Return default QoS profile
  qos->reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  qos->durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
  qos->history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  qos->depth = 10;
  qos->avoid_ros_namespace_conventions = false;

  printf("rmw_service_response_publisher_get_actual_qos: returning default QoS\n");
  return RMW_RET_OK;
}
