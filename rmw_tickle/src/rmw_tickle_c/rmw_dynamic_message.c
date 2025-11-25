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

#include <rcutils/logging_macros.h>
#include <rmw/allocators.h>
#include <rmw/error_handling.h>
#include <rmw/rmw.h>

#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t rmw_take_dynamic_message(const rmw_subscription_t* subscription,
                                   rosidl_dynamic_typesupport_dynamic_data_t* dynamic_message, bool* taken,
                                   rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)dynamic_message;
    (void)allocation;

    if (taken != NULL) {
        *taken = false;
    }

    RCUTILS_LOG_DEBUG("rmw_take_dynamic_message: Dynamic messages not supported in TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_take_dynamic_message_with_info(const rmw_subscription_t* subscription,
                                             rosidl_dynamic_typesupport_dynamic_data_t* dynamic_message, bool* taken,
                                             rmw_message_info_t* message_info,
                                             rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)dynamic_message;
    (void)message_info;
    (void)allocation;

    if (taken != NULL) {
        *taken = false;
    }

    RCUTILS_LOG_DEBUG("rmw_take_dynamic_message_with_info: Dynamic messages not supported in TickLE");
    return RMW_RET_UNSUPPORTED;
}
