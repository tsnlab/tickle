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
#include <rosidl_runtime_c/service_type_support_struct.h>

#include "rmw_tickle_c/rmw_tickle.h"

rmw_client_t *
rmw_create_client(
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

  // Allocate memory for the client
  rmw_tickle_client_t * tickle_client = malloc(sizeof(rmw_tickle_client_t));
  if (tickle_client == NULL) {
    RMW_SET_ERROR_MSG("Failed to allocate memory for client");
    return NULL;
  }

  // Initialize the client structure
  memset(tickle_client, 0, sizeof(rmw_tickle_client_t));

  // Set up the RMW client structure (embedded in tickle_client)
  rmw_client_t * rmw_client = &tickle_client->rmw_client;

  rmw_client->implementation_identifier = RMW_TICKLE_IDENTIFIER;
  rmw_client->data = tickle_client;
  rmw_client->service_name = service_name;

  // Store node and type support references
  tickle_client->node = (rmw_tickle_node_t *)node->data;
  tickle_client->type_support = type_support;

  // Initialize TickLE client
  // Create a dummy service for now - in a real implementation, this would be created based on type_support
  struct tt_Service * service = malloc(sizeof(struct tt_Service));
  if (service == NULL) {
    free(tickle_client);
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
  // In a real implementation, this would handle incoming service responses
  tt_CLIENT_CALLBACK callback = NULL; // We'll handle responses in rmw_take_response instead

  int32_t result = tt_Node_create_client(&tickle_client->node->tickle_node,
                                        &tickle_client->tickle_client,
                                        service, service_name, callback);
  if (result != 0) {
    free(service);
    free(tickle_client);
    RMW_SET_ERROR_MSG("Failed to create TickLE client");
    return NULL;
  }

  // Store service reference for later use
  tickle_client->tickle_client.service = service;

  RCUTILS_LOG_INFO("Created TickLE client: %s", service_name);
  return rmw_client;
}

rmw_ret_t
rmw_destroy_client(
  rmw_node_t * node,
  rmw_client_t * client)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_client_t * tickle_client = (rmw_tickle_client_t *)client->data;
  if (tickle_client != NULL) {
    // Destroy TickLE client
    int32_t result = tt_Client_destroy(&tickle_client->tickle_client);
    if (result != 0) {
      RCUTILS_LOG_WARN("Failed to destroy TickLE client, error code: %d", result);
    }

    // Free the service if it was allocated
    if (tickle_client->tickle_client.service != NULL) {
      free(tickle_client->tickle_client.service);
    }

    free(tickle_client);
  }

  RCUTILS_LOG_INFO("Destroyed TickLE client: %s", client->service_name);
  return RMW_RET_OK;
}

rmw_ret_t
rmw_send_request(
  const rmw_client_t * client,
  const void * ros_request,
  int64_t * sequence_id)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(sequence_id, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_client_t * tickle_client = (rmw_tickle_client_t *)client->data;
  if (tickle_client == NULL) {
    RMW_SET_ERROR_MSG("Client data is NULL");
    return RMW_RET_ERROR;
  }

  // For now, we'll simulate request sending
  // In a real implementation, this would serialize the ros_request
  // and send it to the service using the TickLE API

  // Create a dummy request structure for TickLE
  // In a real implementation, this would serialize the ROS request
  struct tt_Request * request = malloc(sizeof(struct tt_Request));
  if (request == NULL) {
    RMW_SET_ERROR_MSG("Failed to allocate memory for TickLE request");
    return RMW_RET_ERROR;
  }

  // For now, we'll just simulate sending the request
  // In a complete implementation, we would serialize the ros_request here
  // and use the TickLE API to send it to the service

  // Set a dummy sequence ID
  *sequence_id = 1;

  // Free the request structure
  free(request);

  RCUTILS_LOG_DEBUG("Successfully sent service request via TickLE (simplified implementation)");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_take_response(
  const rmw_client_t * client,
  rmw_service_info_t * request_header,
  void * ros_response,
  bool * taken)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(request_header, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_response, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(client->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  rmw_tickle_client_t * tickle_client = (rmw_tickle_client_t *)client->data;
  if (tickle_client == NULL) {
    RMW_SET_ERROR_MSG("Client data is NULL");
    return RMW_RET_ERROR;
  }

  // For now, we'll simulate response reception
  // In a real implementation, this would poll the TickLE node for incoming service responses
  // and deserialize them into the ros_response buffer

  // Check if there are any responses available
  // This is a simplified implementation - in reality, we would need to:
  // 1. Poll the TickLE node for new service responses
  // 2. Check if any responses match this client
  // 3. Deserialize the response data into ros_response

  // For now, we'll just return no response available
  *taken = false;

  RCUTILS_LOG_DEBUG("rmw_take_response: No responses available (simplified implementation)");
  return RMW_RET_OK;
}
