/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// LARGE_MESSAGE_PLAN.md pass 5, the memory half: what one rmw_tickle endpoint of a large type
// costs, measured as the slope over many of them rather than as a total - a fixed per-node cost
// would otherwise be read as a per-endpoint one.
//
// Allocator bytes (glibc's mallinfo2().uordblks), not VmHWM: the plan says so, and the reason is
// that VmHWM is whole-process and misses pages a calloc never touched, which is most of a large
// reserved arena. `n` endpoints are created, the bytes in use are read before and after, and the
// difference is divided by `n`. std_msgs/Empty is the control: whatever a pair of endpoints costs
// that has nothing to do with the message, it costs for Empty too, so Image minus Empty is what the
// type itself brings.
//
// Before stage 1 a publisher held one TickLE struct (publish_scratch_buf) and a subscription
// another (decode_scratch), so the slope carried two struct sizes per pair that it should not carry
// now. The struct size is printed beside the slope, which is what makes that readable.

#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <string>
#include <vector>

#include <rosidl_runtime_c/message_type_support_struct.h>
#include <rosidl_typesupport_cpp/message_type_support.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <std_msgs/msg/empty.hpp>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/types.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_cpp/identifier.h"

namespace {

    constexpr int default_endpoints = 100;
    constexpr size_t queue_depth = 10;

    auto bytes_in_use() -> size_t {
        const struct mallinfo2 info = mallinfo2();
        return info.uordblks;
    }

    auto tickle_callbacks_of(const rosidl_message_type_support_t* top)
        -> const rosidl_typesupport_tickle_c_message_callbacks_t* {
        const rosidl_message_type_support_t* ours =
            get_message_typesupport_handle(top, rosidl_typesupport_tickle_cpp__identifier);
        return ours != nullptr ? static_cast<const rosidl_typesupport_tickle_c_message_callbacks_t*>(ours->data)
                               : nullptr;
    }

    auto endpoint_qos() -> rmw_qos_profile_t {
        rmw_qos_profile_t qos = rmw_qos_profile_default;
        qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
        qos.depth = queue_depth;
        qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
        qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
        return qos;
    }

    // One publisher and one subscription per topic, n topics, on one node. Returns the bytes in use
    // that the n pairs added, or 0 if anything refused.
    auto measure(const char* name, const rosidl_message_type_support_t* type_support, int count) -> void {
        const auto* callbacks = tickle_callbacks_of(type_support);
        if (callbacks == nullptr) {
            std::printf("%s: no rosidl_typesupport_tickle_c handle - is the interface overlay sourced?\n", name);
            return;
        }

        const rcutils_allocator_t allocator = rcutils_get_default_allocator();
        rmw_init_options_t options = rmw_get_zero_initialized_init_options();
        if (rmw_init_options_init(&options, allocator) != RMW_RET_OK) {
            std::printf("%s: rmw_init_options_init failed\n", name);
            return;
        }
        options.enclave = rcutils_strdup("/", allocator);
        rmw_context_t context = rmw_get_zero_initialized_context();
        if (rmw_init(&options, &context) != RMW_RET_OK) {
            std::printf("%s: rmw_init failed\n", name);
            return;
        }
        rmw_node_t* node = rmw_create_node(&context, "endpoint_memory", "/");
        if (node == nullptr) {
            std::printf("%s: rmw_create_node failed\n", name);
            return;
        }

        const rmw_qos_profile_t qos = endpoint_qos();
        const rmw_publisher_options_t publisher_options = rmw_get_default_publisher_options();
        const rmw_subscription_options_t subscription_options = rmw_get_default_subscription_options();
        std::vector<rmw_publisher_t*> publishers;
        std::vector<rmw_subscription_t*> subscriptions;

        // The node, its context and the first endpoint pair carry fixed costs; the slope is taken
        // from the pairs after them, which is the whole point of measuring a slope.
        rmw_publisher_t* warm_publisher = rmw_create_publisher(node, type_support, "/warm", &qos, &publisher_options);
        rmw_subscription_t* warm_subscription =
            rmw_create_subscription(node, type_support, "/warm", &qos, &subscription_options);
        if (warm_publisher == nullptr || warm_subscription == nullptr) {
            std::printf("%s: the first endpoint pair failed\n", name);
            return;
        }

        const size_t before = bytes_in_use();
        for (int index = 0; index < count; index++) {
            const std::string topic = "/endpoint_" + std::to_string(index);
            rmw_publisher_t* publisher =
                rmw_create_publisher(node, type_support, topic.c_str(), &qos, &publisher_options);
            rmw_subscription_t* subscription =
                rmw_create_subscription(node, type_support, topic.c_str(), &qos, &subscription_options);
            if (publisher == nullptr || subscription == nullptr) {
                std::printf("%s: endpoint %d failed\n", name, index);
                return;
            }
            publishers.push_back(publisher);
            subscriptions.push_back(subscription);
        }
        const size_t after = bytes_in_use();

        for (rmw_subscription_t* subscription: subscriptions) {
            (void)rmw_destroy_subscription(node, subscription);
        }
        for (rmw_publisher_t* publisher: publishers) {
            (void)rmw_destroy_publisher(node, publisher);
        }
        const size_t freed = bytes_in_use();
        (void)rmw_destroy_subscription(node, warm_subscription);
        (void)rmw_destroy_publisher(node, warm_publisher);
        (void)rmw_destroy_node(node);
        (void)rmw_shutdown(&context);
        (void)rmw_context_fini(&context);
        (void)rmw_init_options_fini(&options);

        const double per_pair = static_cast<double>(after - before) / count;
        std::printf("RESULT: type=%s pairs=%d bytes_per_pair=%.1f tickle_struct_size=%zu "
                    "max_encoded_size=%zu leaked_bytes=%zu direct_codec=%d\n",
                    name, count, per_pair, callbacks->tickle_struct_size, callbacks->tickle_max_encoded_size,
                    freed > before ? freed - before : 0, callbacks->direct_encode != nullptr ? 1 : 0);
    }

} // namespace

auto main(int argc, char** argv) -> int {
    const std::vector<std::string> args(argv, argv + argc);
    const int count = args.size() > 1 ? std::stoi(args[1]) : default_endpoints;
    measure("Image", rosidl_typesupport_cpp::get_message_type_support_handle<sensor_msgs::msg::Image>(), count);
    measure("Empty", rosidl_typesupport_cpp::get_message_type_support_handle<std_msgs::msg::Empty>(), count);
    return 0;
}
