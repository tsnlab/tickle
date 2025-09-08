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
rmw_init_publisher_allocation(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds,
  rmw_publisher_allocation_t * allocation)
{
  (void)type_support;
  (void)message_bounds;
  (void)allocation;
  RCUTILS_LOG_DEBUG("function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_fini_publisher_allocation(
  rmw_publisher_allocation_t * allocation)
{
  (void)allocation;
  RCUTILS_LOG_DEBUG("function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_publisher_t *
rmw_create_publisher(
  const rmw_node_t * node,
  const rosidl_message_type_support_t * type_support,
  const char * topic_name,
  const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options)
{
  (void)node;
  (void)type_support;
  (void)topic_name;
  (void)qos_policies;
  (void)publisher_options;
  
  RCUTILS_LOG_DEBUG("rmw_create_publisher: function not implemented for TickLE");
  return NULL;
}

rmw_ret_t
rmw_destroy_publisher(
  rmw_node_t * node,
  rmw_publisher_t * publisher)
{
  (void)node;
  (void)publisher;
  
  RCUTILS_LOG_DEBUG("rmw_destroy_publisher: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_publish(
  const rmw_publisher_t * publisher,
  const void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  (void)publisher;
  (void)ros_message;
  (void)allocation;
  
  RCUTILS_LOG_DEBUG("rmw_publish: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_publish_loaned_message(
  const rmw_publisher_t * publisher,
  void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  (void)publisher;
  (void)ros_message;
  (void)allocation;
  
  RCUTILS_LOG_DEBUG("rmw_publish_loaned_message: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_publish_serialized_message(
  const rmw_publisher_t * publisher,
  const rmw_serialized_message_t * serialized_message,
  rmw_publisher_allocation_t * allocation)
{
  (void)publisher;
  (void)serialized_message;
  (void)allocation;
  
  RCUTILS_LOG_DEBUG("rmw_publish_serialized_message: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_borrow_loaned_message(
  const rmw_publisher_t * publisher,
  const rosidl_message_type_support_t * type_support,
  void ** ros_message)
{
  (void)publisher;
  (void)type_support;
  (void)ros_message;
  
  RCUTILS_LOG_DEBUG("rmw_borrow_loaned_message: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_return_loaned_message_from_publisher(
  const rmw_publisher_t * publisher,
  void * loaned_message)
{
  (void)publisher;
  (void)loaned_message;
  
  RCUTILS_LOG_DEBUG("rmw_return_loaned_message_from_publisher: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_publisher_count_matched_subscriptions(
  const rmw_publisher_t * publisher,
  size_t * subscription_count)
{
  (void)publisher;
  (void)subscription_count;
  
  RCUTILS_LOG_DEBUG("rmw_publisher_count_matched_subscriptions: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_publisher_event_init(
  rmw_event_t * event,
  const rmw_publisher_t * publisher,
  rmw_event_type_t event_type)
{
  (void)event;
  (void)publisher;
  (void)event_type;
  
  RCUTILS_LOG_DEBUG("rmw_publisher_event_init: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_publisher_assert_liveliness(
  const rmw_publisher_t * publisher)
{
  (void)publisher;
  
  RCUTILS_LOG_DEBUG("rmw_publisher_assert_liveliness: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_publisher_wait_for_all_acked(
  const rmw_publisher_t * publisher,
  rmw_time_t wait_timeout)
{
  (void)publisher;
  (void)wait_timeout;
  
  RCUTILS_LOG_DEBUG("rmw_publisher_wait_for_all_acked: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}
