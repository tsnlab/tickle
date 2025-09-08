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
  (void)node;
  (void)type_support;
  (void)service_name;
  (void)qos_policies;
  
  RCUTILS_LOG_DEBUG("rmw_create_client: function not implemented for TickLE");
  return NULL;
}

rmw_ret_t
rmw_destroy_client(
  rmw_node_t * node,
  rmw_client_t * client)
{
  (void)node;
  (void)client;
  
  RCUTILS_LOG_DEBUG("rmw_destroy_client: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_send_request(
  const rmw_client_t * client,
  const void * ros_request,
  int64_t * sequence_id)
{
  (void)client;
  (void)ros_request;
  (void)sequence_id;
  
  RCUTILS_LOG_DEBUG("rmw_send_request: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_take_response(
  const rmw_client_t * client,
  rmw_service_info_t * request_header,
  void * ros_response,
  bool * taken)
{
  (void)client;
  (void)request_header;
  (void)ros_response;
  (void)taken;
  
  RCUTILS_LOG_DEBUG("rmw_take_response: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}
