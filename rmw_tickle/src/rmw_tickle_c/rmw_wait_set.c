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

#include "rmw_tickle_c/rmw_tickle.h"

rmw_wait_set_t *
rmw_create_wait_set(rmw_context_t * context, size_t max_conditions)
{
  (void)context;
  (void)max_conditions;
  
  RCUTILS_LOG_DEBUG("rmw_create_wait_set: function not implemented for TickLE");
  return NULL;
}

rmw_ret_t
rmw_destroy_wait_set(rmw_wait_set_t * wait_set)
{
  (void)wait_set;
  
  RCUTILS_LOG_DEBUG("rmw_destroy_wait_set: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_wait(
  rmw_subscriptions_t * subscriptions,
  rmw_guard_conditions_t * guard_conditions,
  rmw_services_t * services,
  rmw_clients_t * clients,
  rmw_events_t * events,
  rmw_wait_set_t * wait_set,
  const rmw_time_t * wait_timeout)
{
  (void)subscriptions;
  (void)guard_conditions;
  (void)services;
  (void)clients;
  (void)events;
  (void)wait_set;
  (void)wait_timeout;
  
  RCUTILS_LOG_DEBUG("rmw_wait: function not implemented for TickLE");
  return RMW_RET_UNSUPPORTED;
}
