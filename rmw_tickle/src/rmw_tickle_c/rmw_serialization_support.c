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
#include <rmw/dynamic_message_type_support.h>
#include <rcutils/logging_macros.h>

#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t
rmw_serialization_support_init(
  const char * serialization_lib_name,
  rcutils_allocator_t * allocator,
  rosidl_dynamic_typesupport_serialization_support_t * serialization_support)
{
  (void)serialization_lib_name;
  (void)allocator;
  (void)serialization_support;

  RCUTILS_LOG_DEBUG("rmw_serialization_support_init: Serialization support not implemented in TickLE");
  return RMW_RET_UNSUPPORTED;
}

rmw_ret_t
rmw_serialization_support_fini(
  rosidl_dynamic_typesupport_serialization_support_t * serialization_support,
  rcutils_allocator_t * allocator)
{
  (void)serialization_support;
  (void)allocator;

  RCUTILS_LOG_DEBUG("rmw_serialization_support_fini: Serialization support not implemented in TickLE");
  return RMW_RET_UNSUPPORTED;
}
