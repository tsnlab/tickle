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

rmw_ret_t
rmw_qos_profile_check_compatible(
  const rmw_qos_profile_t publisher_profile __attribute__((unused)),
  const rmw_qos_profile_t subscription_profile __attribute__((unused)),
  rmw_qos_compatibility_type_t * compatibility,
  char * reason,
  size_t reason_size)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(compatibility, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(reason, RMW_RET_INVALID_ARGUMENT);

  // For now, we don't have QoS compatibility checking in TickLE
  // Assume they are compatible
  *compatibility = RMW_QOS_COMPATIBILITY_OK;
  snprintf(reason, reason_size, "QoS compatibility check not implemented for TickLE");

  printf("rmw_qos_profile_check_compatible: returning compatible (not implemented for TickLE)\n");
  return RMW_RET_OK;
}
