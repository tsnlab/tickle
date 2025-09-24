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

#include <stdio.h>

#include "rcutils/error_handling.h"
#include "rmw/error_handling.h"
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t rmw_get_publishers_info_by_topic(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                           const char* topic_name, bool no_mangle __attribute__((unused)),
                                           rmw_topic_endpoint_info_array_t* publishers_info) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publishers_info, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    // For now, we don't have topic info support in TickLE
    publishers_info->size = 0;
    publishers_info->info_array = NULL;

    printf("rmw_get_publishers_info_by_topic: returning empty array (not implemented for TickLE)\n");
    return RMW_RET_OK;
}

rmw_ret_t rmw_get_subscriptions_info_by_topic(const rmw_node_t* node, rcutils_allocator_t* allocator,
                                              const char* topic_name, bool no_mangle __attribute__((unused)),
                                              rmw_topic_endpoint_info_array_t* subscriptions_info) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscriptions_info, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    // For now, we don't have topic info support in TickLE
    subscriptions_info->size = 0;
    subscriptions_info->info_array = NULL;

    printf("rmw_get_subscriptions_info_by_topic: returning empty array (not implemented for TickLE)\n");
    return RMW_RET_OK;
}
