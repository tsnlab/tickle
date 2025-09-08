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
#include <rosidl_runtime_c/message_type_support_struct.h>

#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t
rmw_init_subscription_allocation(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds,
  rmw_subscription_allocation_t * allocation)
{
  (void)type_support;
  (void)message_bounds;
  (void)allocation;
  RCUTILS_LOG_DEBUG("function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_fini_subscription_allocation(
  rmw_subscription_allocation_t * allocation)
{
  (void)allocation;
  RCUTILS_LOG_DEBUG("function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_subscription_t *
rmw_create_subscription(
  const rmw_node_t * node,
  const rosidl_message_type_support_t * type_support,
  const char * topic_name,
  const rmw_qos_profile_t * qos_policies,
  const rmw_subscription_options_t * subscription_options)
{
  (void)node;
  (void)type_support;
  (void)topic_name;
  (void)qos_policies;
  (void)subscription_options;
  
  RCUTILS_LOG_DEBUG("rmw_create_subscription: function not implemented for TickLE");
  return NULL;
}

rmw_ret_t
rmw_destroy_subscription(
  rmw_node_t * node,
  rmw_subscription_t * subscription)
{
  (void)node;
  (void)subscription;
  
  RCUTILS_LOG_DEBUG("rmw_destroy_subscription: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_take(
  const rmw_subscription_t * subscription,
  void * ros_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  (void)subscription;
  (void)ros_message;
  (void)taken;
  (void)allocation;
  
  RCUTILS_LOG_DEBUG("rmw_take: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_take_with_info(
  const rmw_subscription_t * subscription,
  void * ros_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  (void)subscription;
  (void)ros_message;
  (void)taken;
  (void)message_info;
  (void)allocation;
  
  RCUTILS_LOG_DEBUG("rmw_take_with_info: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_take_serialized_message(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  (void)subscription;
  (void)serialized_message;
  (void)allocation;
  
  if (taken != NULL) {
    *taken = false;
  }
  
  RCUTILS_LOG_DEBUG("rmw_take_serialized_message: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_take_serialized_message_with_info(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  (void)subscription;
  (void)serialized_message;
  (void)taken;
  (void)message_info;
  (void)allocation;
  
  RCUTILS_LOG_DEBUG("rmw_take_serialized_message_with_info: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_take_loaned_message(
  const rmw_subscription_t * subscription,
  void ** ros_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  (void)subscription;
  (void)ros_message;
  (void)taken;
  (void)allocation;
  
  RCUTILS_LOG_DEBUG("rmw_take_loaned_message: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_take_loaned_message_with_info(
  const rmw_subscription_t * subscription,
  void ** ros_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  (void)subscription;
  (void)ros_message;
  (void)taken;
  (void)message_info;
  (void)allocation;
  
  RCUTILS_LOG_DEBUG("rmw_take_loaned_message_with_info: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_return_loaned_message_from_subscription(
  const rmw_subscription_t * subscription,
  void * loaned_message)
{
  (void)subscription;
  (void)loaned_message;
  
  RCUTILS_LOG_DEBUG("rmw_return_loaned_message_from_subscription: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_subscription_count_matched_publishers(
  const rmw_subscription_t * subscription,
  size_t * publisher_count)
{
  (void)subscription;
  (void)publisher_count;
  
  RCUTILS_LOG_DEBUG("rmw_subscription_count_matched_publishers: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_subscription_event_init(
  rmw_event_t * event,
  const rmw_subscription_t * subscription,
  rmw_event_type_t event_type)
{
  (void)event;
  (void)subscription;
  (void)event_type;
  
  RCUTILS_LOG_DEBUG("rmw_subscription_event_init: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_subscription_set_content_filter(
  rmw_subscription_t * subscription,
  const rmw_subscription_content_filter_options_t * options)
{
  (void)subscription;
  (void)options;
  
  RCUTILS_LOG_DEBUG("rmw_subscription_set_content_filter: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_subscription_get_content_filter(
  const rmw_subscription_t * subscription,
  rcutils_allocator_t * allocator,
  rmw_subscription_content_filter_options_t * options)
{
  (void)subscription;
  (void)allocator;
  (void)options;
  
  RCUTILS_LOG_DEBUG("rmw_subscription_get_content_filter: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}
