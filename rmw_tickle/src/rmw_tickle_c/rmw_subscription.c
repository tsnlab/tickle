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

#include <rcutils/allocator.h>
#include <rcutils/logging_macros.h>
#include <rcutils/strdup.h>
#include <rmw/allocators.h>
#include <rmw/error_handling.h>
#include <rmw/rmw.h>
#include <rosidl_runtime_c/message_type_support_struct.h>

#include "rmw_tickle_c/rmw_tickle.h"

#include "__TEMP__messages.h"

rmw_ret_t rmw_init_subscription_allocation(const rosidl_message_type_support_t* type_support,
                                           const rosidl_runtime_c__Sequence__bound* message_bounds,
                                           rmw_subscription_allocation_t* allocation) {
    (void)type_support;
    (void)message_bounds;
    (void)allocation;
    RCUTILS_LOG_DEBUG("function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_fini_subscription_allocation(rmw_subscription_allocation_t* allocation) {
    (void)allocation;
    RCUTILS_LOG_DEBUG("function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_subscription_t* rmw_create_subscription(const rmw_node_t* node, const rosidl_message_type_support_t* type_support,
                                            const char* topic_name, const rmw_qos_profile_t* qos_policies,
                                            const rmw_subscription_options_t* subscription_options) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(type_support, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(topic_name, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos_policies, NULL);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription_options, NULL);

    if (strcmp(node->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return NULL;
    }

    rmw_tickle_node_t* tickle_node = (rmw_tickle_node_t*)node->data;

    // Allocate memory for the subscription
    rmw_tickle_subscriber_t* tickle_subscriber = malloc(sizeof(rmw_tickle_subscriber_t));
    if (tickle_subscriber == NULL) {
        RMW_SET_ERROR_MSG("Failed to allocate memory for subscription");
        return NULL;
    }

    // Initialize the subscription structure
    memset(tickle_subscriber, 0, sizeof(rmw_tickle_subscriber_t));

    // Set up the RMW subscription structure (embedded in tickle_subscriber)
    rmw_subscription_t* rmw_subscription = &tickle_subscriber->rmw_subscription;

    rmw_subscription->implementation_identifier = RMW_TICKLE_IDENTIFIER;
    rmw_subscription->data = tickle_subscriber;
    rmw_subscription->topic_name = rcutils_strdup(topic_name, tickle_node->allocator);
    rmw_subscription->options = *subscription_options;
    rmw_subscription->can_loan_messages = false;

    // Store node and type support references
    tickle_subscriber->node = tickle_node;
    tickle_subscriber->type_support = type_support;

    // Initialize TickLE subscriber
    // TODO: Create a dummy topic for now - in a real implementation, this would be created based on type_support
    struct tt_Topic* topic = malloc(sizeof(struct tt_Topic));
    if (topic == NULL) {
        free(tickle_subscriber);
        RMW_SET_ERROR_MSG("Failed to allocate memory for TickLE topic");
        return NULL;
    }

    // Initialize topic with basic information
    topic->name = rcutils_strdup(topic_name, tickle_node->allocator);
    topic->history_depth = 10; // Default QoS
    topic->deadline_duration = 0;
    topic->lifespan_duration = 0;

    if ((strcmp(topic_name, "/microROS/ping") == 0) || (strcmp(topic_name, "/microROS/pong") == 0)) {
        topic->data_size = sizeof(struct HeaderData);
        topic->data_encode_size = (tt_DATA_ENCODE_SIZE)HeaderData_encode_size;
        topic->data_encode = (tt_DATA_ENCODE)HeaderData_encode;
        topic->data_decode = (tt_DATA_DECODE)HeaderData_decode;
        topic->data_free = (tt_DATA_FREE)HeaderData_free;
    } else if (strcmp(topic_name, "/chatter") == 0) {
        topic->data_size = sizeof(struct StringData);
        topic->data_encode_size = (tt_DATA_ENCODE_SIZE)StringData_encode_size;
        topic->data_encode = (tt_DATA_ENCODE)StringData_encode;
        topic->data_decode = (tt_DATA_DECODE)StringData_decode;
        topic->data_free = (tt_DATA_FREE)StringData_free;
    }

    // Create a dummy callback for now
    // In a real implementation, this would handle incoming messages
    tt_SUBSCRIBER_CALLBACK callback = NULL; // We'll handle messages in rmw_take instead

    int32_t result = tt_Node_create_subscriber(&tickle_subscriber->node->tickle_node,
                                               &tickle_subscriber->tickle_subscriber, topic, topic_name, callback);
    if (result != 0) {
        free(topic);
        free(tickle_subscriber);
        RMW_SET_ERROR_MSG("Failed to create TickLE subscriber");
        return NULL;
    }

    // Store topic reference for later use
    tickle_subscriber->tickle_subscriber.topic = topic;

    RCUTILS_LOG_INFO("Created TickLE subscription for topic: %s", topic_name);

    return rmw_subscription;
}

rmw_ret_t rmw_destroy_subscription(rmw_node_t* node, rmw_subscription_t* subscription) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);

    if (strcmp(subscription->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_node_t* tickle_node = (rmw_tickle_node_t*)node->data;
    rmw_tickle_subscriber_t* tickle_subscriber = (rmw_tickle_subscriber_t*)subscription->data;
    if (tickle_subscriber != NULL) {
        // Destroy TickLE subscriber
        int32_t result = tt_Subscriber_destroy(&tickle_subscriber->tickle_subscriber);
        if (result != 0) {
            RCUTILS_LOG_WARN("Failed to destroy TickLE subscriber, error code: %d", result);
        }

        tickle_node->allocator.deallocate(subscription->topic_name, tickle_node->allocator.state);

        // Free the topic if it was allocated
        if (tickle_subscriber->tickle_subscriber.topic != NULL) {
            tickle_node->allocator.deallocate(tickle_subscriber->tickle_subscriber.topic->name, tickle_node->allocator.state);
            free(tickle_subscriber->tickle_subscriber.topic);
        }

        free(tickle_subscriber);
    }

    RCUTILS_LOG_INFO("Destroyed TickLE subscription for topic: %s", subscription->topic_name);

    return RMW_RET_OK;
}

rmw_ret_t rmw_take(const rmw_subscription_t* subscription, void* ros_message, bool* taken,
                   rmw_subscription_allocation_t* allocation) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    (void)allocation; // Not used in this implementation

    uint8_t buffer[tt_MAX_BUFFER_LENGTH];
    uint32_t ip = 0;
    uint16_t port = 0;
    if (strcmp(subscription->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* tickle_subscriber = (rmw_tickle_subscriber_t*)subscription->data;
    if (tickle_subscriber == NULL) {
        RMW_SET_ERROR_MSG("Subscription data is NULL");
        return RMW_RET_ERROR;
    }

    if (strcmp(subscription->topic_name, "/parameter_events") == 0) {
        *taken = false;
        return RMW_RET_OK;
    }

    // Poll the TickLE node for incoming messages
    // In a real implementation, this would check for new messages from the network
    *taken = false;
    int32_t len = __TEMP__tt_receive_packet(&tickle_subscriber->node->tickle_node, buffer, tt_MAX_BUFFER_LENGTH);
    if (len == -1) {
        // Timeout
        return RMW_RET_OK;
    } else if (len < 0) {
        // I/O Error
        return RMW_RET_ERROR;
    }
    memcpy(ros_message, buffer, len);

    // For now, we'll simulate message reception
    // In a real implementation, we would:
    // 1. Check if any messages match this subscription's topic
    // 2. Deserialize the message data into ros_message
    // 3. Handle message ordering and QoS

    // This is a simplified implementation - we'll just return no message available for now
    // In a complete implementation, we would need to:
    // - Implement proper message queuing
    // - Handle message deserialization
    // - Support different QoS policies

    *taken = true;

    return RMW_RET_OK;
}

rmw_ret_t rmw_take_with_info(const rmw_subscription_t* subscription, void* ros_message, bool* taken,
                             rmw_message_info_t* message_info, rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)ros_message;
    (void)taken;
    (void)message_info;
    (void)allocation;

    // TODO: We should fill in the message_info, but the demo still works without it
    return rmw_take(subscription, ros_message, taken, allocation);
}

rmw_ret_t rmw_take_serialized_message(const rmw_subscription_t* subscription,
                                      rmw_serialized_message_t* serialized_message, bool* taken,
                                      rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)serialized_message;
    (void)allocation;

    if (taken != NULL) {
        *taken = false;
    }

    RCUTILS_LOG_DEBUG("rmw_take_serialized_message: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_take_serialized_message_with_info(const rmw_subscription_t* subscription,
                                                rmw_serialized_message_t* serialized_message, bool* taken,
                                                rmw_message_info_t* message_info,
                                                rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)serialized_message;
    (void)taken;
    (void)message_info;
    (void)allocation;

    RCUTILS_LOG_DEBUG("rmw_take_serialized_message_with_info: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_take_loaned_message(const rmw_subscription_t* subscription, void** ros_message, bool* taken,
                                  rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)ros_message;
    (void)taken;
    (void)allocation;

    RCUTILS_LOG_DEBUG("rmw_take_loaned_message: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_take_loaned_message_with_info(const rmw_subscription_t* subscription, void** ros_message, bool* taken,
                                            rmw_message_info_t* message_info,
                                            rmw_subscription_allocation_t* allocation) {
    (void)subscription;
    (void)ros_message;
    (void)taken;
    (void)message_info;
    (void)allocation;

    RCUTILS_LOG_DEBUG("rmw_take_loaned_message_with_info: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_return_loaned_message_from_subscription(const rmw_subscription_t* subscription, void* loaned_message) {
    (void)subscription;
    (void)loaned_message;

    RCUTILS_LOG_DEBUG("rmw_return_loaned_message_from_subscription: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_subscription_count_matched_publishers(const rmw_subscription_t* subscription, size_t* publisher_count) {
    (void)subscription;
    (void)publisher_count;

    RCUTILS_LOG_DEBUG("rmw_subscription_count_matched_publishers: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_subscription_event_init(rmw_event_t* event, const rmw_subscription_t* subscription,
                                      rmw_event_type_t event_type) {
    (void)event;
    (void)subscription;
    (void)event_type;

    RCUTILS_LOG_DEBUG("rmw_subscription_event_init: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_subscription_set_content_filter(rmw_subscription_t* subscription,
                                              const rmw_subscription_content_filter_options_t* options) {
    (void)subscription;
    (void)options;

    RCUTILS_LOG_DEBUG("rmw_subscription_set_content_filter: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}

rmw_ret_t rmw_subscription_get_content_filter(const rmw_subscription_t* subscription, rcutils_allocator_t* allocator,
                                              rmw_subscription_content_filter_options_t* options) {
    (void)subscription;
    (void)allocator;
    (void)options;

    RCUTILS_LOG_DEBUG("rmw_subscription_get_content_filter: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}
