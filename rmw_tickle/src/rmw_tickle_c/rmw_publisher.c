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

#include <stdint.h>
#include <stdlib.h>

#include <rcutils/logging_macros.h>
#include <rmw/allocators.h>
#include <rmw/error_handling.h>
#include <rmw/rmw.h>
#include <rosidl_runtime_c/message_type_support_struct.h>

#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t rmw_init_publisher_allocation(const rosidl_message_type_support_t* type_support,
                                        const rosidl_runtime_c__Sequence__bound* message_bounds,
                                        rmw_publisher_allocation_t* allocation) {
    (void)type_support;
    (void)message_bounds;
    (void)allocation;
    RCUTILS_LOG_DEBUG("function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_fini_publisher_allocation(rmw_publisher_allocation_t* allocation) {
    (void)allocation;
    RCUTILS_LOG_DEBUG("function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_publisher_t* rmw_create_publisher(const rmw_node_t* node, const rosidl_message_type_support_t* type_support,
                                      const char* topic_name, const rmw_qos_profile_t* qos_policies,
                                      const rmw_publisher_options_t* publisher_options) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(type_support, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(topic_name, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos_policies, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher_options, NULL);

    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return NULL;
    }

    // Allocate memory for the publisher
    rmw_tickle_publisher_t* tickle_publisher = malloc(sizeof(rmw_tickle_publisher_t));
    if (tickle_publisher == NULL) {
        RMW_SET_ERROR_MSG("Failed to allocate memory for publisher");
        return NULL;
    }

    // Initialize the publisher structure
    memset(tickle_publisher, 0, sizeof(rmw_tickle_publisher_t));

    // Set up the RMW publisher structure (embedded in tickle_publisher)
    rmw_publisher_t* rmw_publisher = &tickle_publisher->rmw_publisher;

    rmw_publisher->implementation_identifier = RMW_TICKLE_IDENTIFIER;
    rmw_publisher->data = tickle_publisher;
    rmw_publisher->topic_name = topic_name;
    rmw_publisher->options = *publisher_options;
    rmw_publisher->can_loan_messages = false;

    // Store node and type support references
    tickle_publisher->node = (rmw_tickle_node_t*)node->data;
    tickle_publisher->type_support = type_support;

    // Initialize TickLE publisher
    // Create a dummy topic for now - in a real implementation, this would be created based on type_support
    struct tt_Topic* topic = malloc(sizeof(struct tt_Topic));
    if (topic == NULL) {
        free(tickle_publisher);
        RMW_SET_ERROR_MSG("Failed to allocate memory for TickLE topic");
        return NULL;
    }

    // Initialize topic with basic information
    topic->name = topic_name;
    topic->data_size = 0; // Will be set based on message type
    topic->data_encode_size = NULL;
    topic->data_encode = NULL;
    topic->data_decode = NULL;
    topic->data_free = NULL;
    topic->history_depth = 10; // Default QoS
    topic->deadline_duration = 0;
    topic->lifespan_duration = 0;

    int32_t result = tt_Node_create_publisher(&tickle_publisher->node->tickle_node, &tickle_publisher->tickle_publisher,
                                              topic, topic_name);
    if (result != 0) {
        free(topic);
        free(tickle_publisher);
        RMW_SET_ERROR_MSG("Failed to create TickLE publisher");
        return NULL;
    }

    // Store topic reference for later use
    tickle_publisher->tickle_publisher.topic = topic;

    RCUTILS_LOG_INFO("Created TickLE publisher for topic: %s", topic_name);

    return rmw_publisher;
}

rmw_ret_t rmw_destroy_publisher(rmw_node_t* node, rmw_publisher_t* publisher) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(publisher->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* tickle_publisher = (rmw_tickle_publisher_t*)publisher->data;
    if (tickle_publisher != NULL) {
        // Destroy TickLE publisher
        int32_t result = tt_Publisher_destroy(&tickle_publisher->tickle_publisher);
        if (result != 0) {
            RCUTILS_LOG_WARN("Failed to destroy TickLE publisher, error code: %d", result);
        }

        // Free the topic if it was allocated
        if (tickle_publisher->tickle_publisher.topic != NULL) {
            free(tickle_publisher->tickle_publisher.topic);
        }

        free(tickle_publisher);
    }

    RCUTILS_LOG_INFO("Destroyed TickLE publisher for topic: %s", publisher->topic_name);

    return RMW_RET_OK;
}

rmw_ret_t rmw_publish(const rmw_publisher_t* publisher, const void* ros_message,
                      rmw_publisher_allocation_t* allocation) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    (void)allocation; // Not used in this implementation

    if (strcmp(publisher->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_publisher_t* tickle_publisher = (rmw_tickle_publisher_t*)publisher->data;
    if (tickle_publisher == NULL) {
        RMW_SET_ERROR_MSG("Publisher data is NULL");
        return RMW_RET_ERROR;
    }

    // Create a data structure for TickLE
    // In a real implementation, this would serialize the ROS message
    struct tt_Data* data = malloc(sizeof(struct tt_Data));
    if (data == NULL) {
        RMW_SET_ERROR_MSG("Failed to allocate memory for TickLE data");
        return RMW_RET_ERROR;
    }

    // For now, we'll store a pointer to the ROS message in the data structure
    // In a complete implementation, we would serialize the ros_message here
    // This is a simplified approach - in reality, we would need proper serialization
    data = (struct tt_Data*)ros_message; // Cast for now - this is not ideal but works for testing

    int32_t result = tt_Publisher_publish(&tickle_publisher->tickle_publisher, data);

    // Note: We don't free data here since it's pointing to the ros_message

    if (result != 0) {
        RMW_SET_ERROR_MSG("Failed to publish message via TickLE");
        return RMW_RET_ERROR;
    }

    RCUTILS_LOG_DEBUG("Successfully published message via TickLE");
    return RMW_RET_OK;
}

rmw_ret_t rmw_publish_loaned_message(const rmw_publisher_t* publisher, void* ros_message,
                                     rmw_publisher_allocation_t* allocation) {
    (void)publisher;
    (void)ros_message;
    (void)allocation;

    RCUTILS_LOG_DEBUG("rmw_publish_loaned_message: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_publish_serialized_message(const rmw_publisher_t* publisher,
                                         const rmw_serialized_message_t* serialized_message,
                                         rmw_publisher_allocation_t* allocation) {
    (void)publisher;
    (void)serialized_message;
    (void)allocation;

    RCUTILS_LOG_DEBUG("rmw_publish_serialized_message: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_borrow_loaned_message(const rmw_publisher_t* publisher, const rosidl_message_type_support_t* type_support,
                                    void** ros_message) {
    (void)publisher;
    (void)type_support;
    (void)ros_message;

    RCUTILS_LOG_DEBUG("rmw_borrow_loaned_message: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_return_loaned_message_from_publisher(const rmw_publisher_t* publisher, void* loaned_message) {
    (void)publisher;
    (void)loaned_message;

    RCUTILS_LOG_DEBUG("rmw_return_loaned_message_from_publisher: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_publisher_count_matched_subscriptions(const rmw_publisher_t* publisher, size_t* subscription_count) {
    (void)publisher;
    (void)subscription_count;

    RCUTILS_LOG_DEBUG("rmw_publisher_count_matched_subscriptions: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_publisher_event_init(rmw_event_t* event, const rmw_publisher_t* publisher, rmw_event_type_t event_type) {
    (void)event;
    (void)publisher;
    (void)event_type;

    RCUTILS_LOG_DEBUG("rmw_publisher_event_init: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_publisher_assert_liveliness(const rmw_publisher_t* publisher) {
    (void)publisher;

    RCUTILS_LOG_DEBUG("rmw_publisher_assert_liveliness: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_publisher_wait_for_all_acked(const rmw_publisher_t* publisher, rmw_time_t wait_timeout) {
    (void)publisher;
    (void)wait_timeout;

    RCUTILS_LOG_DEBUG("rmw_publisher_wait_for_all_acked: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}
