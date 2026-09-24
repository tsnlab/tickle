/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// default_node SECONDS: the first thing a ROS 2 user runs - an rclcpp::Node with default options -
// publishing std_msgs/String and std_msgs/Header on the topics interfaces_check's subscriber reads,
// every 100 ms for SECONDS. Default options mean everything rclcpp starts on its own is started:
// the type description service (type_description_interfaces), the parameter services and
// /parameter_events (rcl_interfaces), /rosout (rcl_interfaces/Log). Each needs its interface
// package built with TickLE typesupport, and GetTypeDescription's response needs large-message
// support to fit at all. Exit 0 once it has published for SECONDS; 3, with the reason, if rclcpp
// refuses to start.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/header.hpp"
#include "std_msgs/msg/string.hpp"

namespace {
    constexpr int32_t stamp_sec = 1234;
    constexpr uint32_t stamp_nsec = 567890U;
    constexpr long rounds_per_second = 10;
    constexpr std::chrono::milliseconds period {100};
    constexpr size_t qos_depth = 10; // RELIABLE, KEEP_LAST 10 - ROS 2's default, as interfaces_check

    auto publish_for(long seconds) -> int {
        auto node = std::make_shared<rclcpp::Node>("tickle_default_node");
        auto string_pub = node->create_publisher<std_msgs::msg::String>("/tickle_check_string", qos_depth);
        auto header_pub = node->create_publisher<std_msgs::msg::Header>("/tickle_check_header", qos_depth);
        std_msgs::msg::String text;
        text.data = "hello from rmw_tickle";
        std_msgs::msg::Header header;
        header.stamp.sec = stamp_sec;
        header.stamp.nanosec = stamp_nsec;
        header.frame_id = "tickle_frame";
        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);
        const long rounds = seconds * rounds_per_second;
        for (long i = 0; i < rounds && rclcpp::ok(); i++) {
            string_pub->publish(text);
            header_pub->publish(header);
            executor.spin_some(); // serves the type description and parameter services meanwhile
            std::this_thread::sleep_for(period);
        }
        std::printf("default_node: published %ld of each from a default rclcpp::Node\n", rounds);
        return 0;
    }
} // namespace

auto main(int argc, char** argv) -> int {
    rclcpp::init(argc, argv);
    int status = 0;
    try {
        status = publish_for(argc > 1 ? std::stol(argv[1]) : rounds_per_second);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "default_node: rclcpp refused to start: %s\n", error.what());
        status = 3;
    }
    std::fflush(stdout);
    rclcpp::shutdown();
    return status;
}
