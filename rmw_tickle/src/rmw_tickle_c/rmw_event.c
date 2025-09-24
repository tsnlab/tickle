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

bool
rmw_event_type_is_supported(rmw_event_type_t event_type)
{
  (void)event_type; // Suppress unused parameter warning

  // For now, TickLE doesn't support events
  // In a real implementation, we would check which event types are supported
  RCUTILS_LOG_DEBUG("rmw_event_type_is_supported: Events not supported in TickLE");
  return false;
}

rmw_ret_t
rmw_init_event(rmw_event_t * event, const char * implementation_identifier)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(event, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(implementation_identifier, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // Initialize event structure
  event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
  event->data = NULL;
  event->event_type = RMW_EVENT_INVALID;

  RCUTILS_LOG_DEBUG("rmw_init_event: Events not supported in TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_fini_event(rmw_event_t * event)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(event, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(event->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // Clean up event structure
  event->implementation_identifier = NULL;
  event->data = NULL;
  event->event_type = RMW_EVENT_INVALID;

  RCUTILS_LOG_DEBUG("rmw_fini_event: Events not supported in TickLE");
  return RMW_RET_OK;
}
