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
#include <rosidl_runtime_c/message_type_support_struct.h>
#include <rosidl_typesupport_c/message_type_support_dispatch.h>

#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t
rmw_serialize(
  const void * ros_message,
  const rosidl_message_type_support_t * type_support,
  rmw_serialized_message_t * serialized_message)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(type_support, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(serialized_message, RMW_RET_INVALID_ARGUMENT);

  // For now, we'll skip the type support check
  // In a real implementation, we would use proper type support validation
  (void)type_support; // Suppress unused parameter warning

  // For now, we'll use a simplified serialization approach
  // In a real implementation, we would use the proper ROS2 CDR serialization
  
  // Calculate the size needed for serialization
  // This is a placeholder - in reality, we would use the type support to get the actual size
  size_t message_size = 1024; // Placeholder size
  
  // Allocate memory for the serialized message
  serialized_message->buffer = rmw_allocate(message_size);
  if (serialized_message->buffer == NULL) {
    RMW_SET_ERROR_MSG("Failed to allocate memory for serialized message");
    return RMW_RET_ERROR;
  }
  
  serialized_message->buffer_length = message_size;
  serialized_message->buffer_capacity = message_size;
  
  // For now, we'll just copy the message data as-is
  // In a real implementation, we would use proper CDR serialization
  memcpy(serialized_message->buffer, ros_message, message_size);
  
  RCUTILS_LOG_DEBUG("Successfully serialized message (simplified implementation)");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_deserialize(
  const rmw_serialized_message_t * serialized_message,
  const rosidl_message_type_support_t * type_support,
  void * ros_message)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(serialized_message, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(type_support, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);

  // Check if the serialized message has valid data
  if (serialized_message->buffer == NULL || serialized_message->buffer_length == 0) {
    RMW_SET_ERROR_MSG("Serialized message buffer is NULL or empty");
    return RMW_RET_ERROR;
  }

  // For now, we'll use a simplified deserialization approach
  // In a real implementation, we would use the proper ROS2 CDR deserialization
  
  // Copy the serialized data back to the message
  // This is a placeholder - in reality, we would use proper CDR deserialization
  size_t copy_size = serialized_message->buffer_length;
  if (copy_size > 1024) { // Safety check
    copy_size = 1024;
  }
  
  memcpy(ros_message, serialized_message->buffer, copy_size);
  
  RCUTILS_LOG_DEBUG("Successfully deserialized message (simplified implementation)");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_get_serialized_message_size(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds,
  size_t * size)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(type_support, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(size, RMW_RET_INVALID_ARGUMENT);
  (void)message_bounds; // Not used in this simplified implementation

  // For now, we'll return a placeholder size
  // In a real implementation, we would use the type support to calculate the actual size
  *size = 1024; // Placeholder size
  
  RCUTILS_LOG_DEBUG("Returned serialized message size: %zu (simplified implementation)", *size);
  return RMW_RET_OK;
}
