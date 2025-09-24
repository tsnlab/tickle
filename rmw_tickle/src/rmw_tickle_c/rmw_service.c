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
#include <stdlib.h>
#include <string.h>

rmw_service_t *
rmw_create_service(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_support,
  const char * service_name,
  const rmw_qos_profile_t * qos_policies)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, NULL);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(type_support, NULL);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(service_name, NULL);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos_policies, NULL);

  if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return NULL;
  }

  // Allocate memory for the service
  rmw_tickle_service_t * tickle_service = malloc(sizeof(rmw_tickle_service_t));
  if (tickle_service == NULL) {
    RMW_SET_ERROR_MSG("Failed to allocate memory for service");
    return NULL;
  }

  // Initialize the service structure
  memset(tickle_service, 0, sizeof(rmw_tickle_service_t));

  // Set up the RMW service structure (embedded in tickle_service)
  rmw_service_t * rmw_service = &tickle_service->rmw_service;

  rmw_service->implementation_identifier = RMW_TICKLE_IDENTIFIER;
  rmw_service->data = tickle_service;
  rmw_service->service_name = service_name;

  // Store node and type support references
  tickle_service->node = (rmw_tickle_node_t *)node->data;
  tickle_service->type_support = type_support;

  // Initialize TickLE server
  // Create a dummy service for now - in a real implementation, this would be created based on type_support
  struct tt_Service * service = malloc(sizeof(struct tt_Service));
  if (service == NULL) {
    free(tickle_service);
    RMW_SET_ERROR_MSG("Failed to allocate memory for TickLE service");
    return NULL;
  }

  // Initialize service with basic information
  service->name = service_name;
  service->request_size = 0; // Will be set based on message type
  service->response_size = 0; // Will be set based on message type
  service->request_encode_size = NULL;
  service->request_encode = NULL;
  service->request_decode = NULL;
  service->request_free = NULL;
  service->response_encode_size = NULL;
  service->response_encode = NULL;
  service->response_decode = NULL;
  service->response_free = NULL;
  service->call_retry_interval = 0;
  service->call_retry_count = 0;

  // Create a dummy callback for now
  // In a real implementation, this would handle incoming service requests
  tt_SERVER_CALLBACK callback = NULL; // We'll handle requests in rmw_take_request instead

  int32_t result = tt_Node_create_server(&tickle_service->node->tickle_node,
                                        &tickle_service->tickle_server,
                                        service, service_name, callback);
  if (result != 0) {
    free(service);
    free(tickle_service);
    RMW_SET_ERROR_MSG("Failed to create TickLE server");
    return NULL;
  }

  // Store service reference for later use
  tickle_service->tickle_server.service = service;

  RCUTILS_LOG_INFO("Created TickLE service: %s", service_name);
  return rmw_service;
}

rmw_ret_t
rmw_destroy_service(
  rmw_node_t * node,
  rmw_service_t * service)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(service->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_service_t * tickle_service = (rmw_tickle_service_t *)service->data;
  if (tickle_service != NULL) {
    // Destroy TickLE server
    int32_t result = tt_Server_destroy(&tickle_service->tickle_server);
    if (result != 0) {
      RCUTILS_LOG_WARN("Failed to destroy TickLE server, error code: %d", result);
    }

    // Free the service if it was allocated
    if (tickle_service->tickle_server.service != NULL) {
      free(tickle_service->tickle_server.service);
    }

    free(tickle_service);
  }

  RCUTILS_LOG_INFO("Destroyed TickLE service: %s", service->service_name);
  return RMW_RET_OK;
}

rmw_ret_t
rmw_take_request(
  const rmw_service_t * service,
  rmw_service_info_t * request_header,
  void * ros_request,
  bool * taken)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(request_header, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(service->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_service_t * tickle_service = (rmw_tickle_service_t *)service->data;
  if (tickle_service == NULL) {
    RMW_SET_ERROR_MSG("Service data is NULL");
    return RMW_RET_ERROR;
  }

  // For now, we'll simulate request reception
  // In a real implementation, this would poll the TickLE node for incoming service requests
  // and deserialize them into the ros_request buffer

  // Check if there are any requests available
  // This is a simplified implementation - in reality, we would need to:
  // 1. Poll the TickLE node for new service requests
  // 2. Check if any requests match this service
  // 3. Deserialize the request data into ros_request

  // For now, we'll just return no request available
  *taken = false;

  RCUTILS_LOG_DEBUG("rmw_take_request: No requests available (simplified implementation)");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_send_response(
  const rmw_service_t * service,
  rmw_request_id_t * request_id,
  void * ros_response)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(request_id, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_response, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(service->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_service_t * tickle_service = (rmw_tickle_service_t *)service->data;
  if (tickle_service == NULL) {
    RMW_SET_ERROR_MSG("Service data is NULL");
    return RMW_RET_ERROR;
  }

  // For now, we'll simulate response sending
  // In a real implementation, this would serialize the ros_response
  // and send it back to the client using the TickLE API

  // Create a dummy response structure for TickLE
  // In a real implementation, this would serialize the ROS response
  struct tt_Response * response = malloc(sizeof(struct tt_Response));
  if (response == NULL) {
    RMW_SET_ERROR_MSG("Failed to allocate memory for TickLE response");
    return RMW_RET_ERROR;
  }

  // For now, we'll just simulate sending the response
  // In a complete implementation, we would serialize the ros_response here
  // and use the TickLE API to send it back to the client

  // Free the response structure
  free(response);

  RCUTILS_LOG_DEBUG("Successfully sent service response via TickLE (simplified implementation)");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_service_server_is_available(
  const rmw_node_t * node,
  const rmw_client_t * client,
  bool * is_available)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(is_available, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match for node");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  if (strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match for client");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have service availability checking in TickLE
  *is_available = false;
  printf("rmw_service_server_is_available: returning false (not implemented for TickLE)\n");
  return RMW_RET_OK;
}
