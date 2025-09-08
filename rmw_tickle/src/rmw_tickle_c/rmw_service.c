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

rmw_service_t *
rmw_create_service(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_support,
  const char * service_name,
  const rmw_qos_profile_t * qos_policies)
{
  (void)node;
  (void)type_support;
  (void)service_name;
  (void)qos_policies;
  
  printf("rmw_create_service: function not implemented for TickLE\n");
  return NULL;
}

rmw_ret_t
rmw_destroy_service(
  rmw_node_t * node,
  rmw_service_t * service)
{
  (void)node;
  (void)service;
  
  printf("rmw_destroy_service: function not implemented for TickLE\n");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_take_request(
  const rmw_service_t * service,
  rmw_service_info_t * request_header,
  void * ros_request,
  bool * taken)
{
  (void)service;
  (void)request_header;
  (void)ros_request;
  (void)taken;
  
  printf("rmw_take_request: function not implemented for TickLE\n");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_send_response(
  const rmw_service_t * service,
  rmw_request_id_t * request_id,
  void * ros_response)
{
  (void)service;
  (void)request_id;
  (void)ros_response;
  
  printf("rmw_send_response: function not implemented for TickLE\n");
  return RMW_RET_UNSUPPORTED;
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
