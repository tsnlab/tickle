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

#include <rmw/allocators.h>
#include <rmw/rmw.h>
#include <rmw/error_handling.h>
#include <rcutils/logging_macros.h>

#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t
rmw_compare_gids(const rmw_gid_t * gid1, const rmw_gid_t * gid2, int * result)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid1, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid2, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(result, RMW_RET_INVALID_ARGUMENT);

  // For TickLE, we'll use a simple byte-by-byte comparison
  *result = memcmp(gid1->data, gid2->data, RMW_GID_STORAGE_SIZE);

  RCUTILS_LOG_DEBUG("rmw_compare_gids: Compared GIDs");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_compare_gids_equal(const rmw_gid_t * gid1, const rmw_gid_t * gid2, bool * result)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid1, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid2, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(result, RMW_RET_INVALID_ARGUMENT);

  int comparison_result;
  rmw_ret_t ret = rmw_compare_gids(gid1, gid2, &comparison_result);
  if (ret != RMW_RET_OK) {
    return ret;
  }

  *result = (comparison_result == 0);

  RCUTILS_LOG_DEBUG("rmw_compare_gids_equal: GIDs are %s", *result ? "equal" : "different");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_get_gid_for_publisher(const rmw_publisher_t * publisher, rmw_gid_t * gid)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(publisher->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_publisher_t * tickle_publisher = (rmw_tickle_publisher_t *)publisher->data;
  if (tickle_publisher == NULL) {
    RMW_SET_ERROR_MSG("Publisher data is NULL");
    return RMW_RET_ERROR;
  }

  // Generate a simple GID based on publisher endpoint ID
  memset(gid->data, 0, RMW_GID_STORAGE_SIZE);
  uint32_t endpoint_id = tickle_publisher->tickle_publisher.super.id;
  memcpy(gid->data, &endpoint_id, sizeof(endpoint_id));

  RCUTILS_LOG_DEBUG("rmw_get_gid_for_publisher: Generated GID for publisher");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_get_gid_for_subscription(const rmw_subscription_t * subscription, rmw_gid_t * gid)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(subscription->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_subscriber_t * tickle_subscriber = (rmw_tickle_subscriber_t *)subscription->data;
  if (tickle_subscriber == NULL) {
    RMW_SET_ERROR_MSG("Subscription data is NULL");
    return RMW_RET_ERROR;
  }

  // Generate a simple GID based on subscriber endpoint ID
  memset(gid->data, 0, RMW_GID_STORAGE_SIZE);
  uint32_t endpoint_id = tickle_subscriber->tickle_subscriber.super.id;
  memcpy(gid->data, &endpoint_id, sizeof(endpoint_id));

  RCUTILS_LOG_DEBUG("rmw_get_gid_for_subscription: Generated GID for subscription");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_get_gid_for_client(const rmw_client_t * client, rmw_gid_t * gid)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_client_t * tickle_client = (rmw_tickle_client_t *)client->data;
  if (tickle_client == NULL) {
    RMW_SET_ERROR_MSG("Client data is NULL");
    return RMW_RET_ERROR;
  }

  // Generate a simple GID based on client endpoint ID
  memset(gid->data, 0, RMW_GID_STORAGE_SIZE);
  uint32_t endpoint_id = tickle_client->tickle_client.super.id;
  memcpy(gid->data, &endpoint_id, sizeof(endpoint_id));

  RCUTILS_LOG_DEBUG("rmw_get_gid_for_client: Generated GID for client");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_get_gid_for_service(const rmw_service_t * service, rmw_gid_t * gid)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(service->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_service_t * tickle_service = (rmw_tickle_service_t *)service->data;
  if (tickle_service == NULL) {
    RMW_SET_ERROR_MSG("Service data is NULL");
    return RMW_RET_ERROR;
  }

  // Generate a simple GID based on service endpoint ID
  memset(gid->data, 0, RMW_GID_STORAGE_SIZE);
  uint32_t endpoint_id = tickle_service->tickle_server.super.id;
  memcpy(gid->data, &endpoint_id, sizeof(endpoint_id));

  RCUTILS_LOG_DEBUG("rmw_get_gid_for_service: Generated GID for service");
  return RMW_RET_OK;
}
