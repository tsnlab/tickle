/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// default_node pub|sub SECONDS: the first thing a ROS 2 user runs - an rclcpp::Node with default
// options - on either end of a round trip through rmw_tickle. Default options mean everything
// rclcpp starts on its own is started: the type description service (type_description_interfaces),
// the parameter services and /parameter_events (rcl_interfaces), /rosout (rcl_interfaces/Log). Each
// needs its interface package built with TickLE typesupport, and GetTypeDescription's response
// needs large-message support to fit at all.
//
// pub publishes, every 100 ms for SECONDS: std_msgs/String and std_msgs/Header (on the topics
// interfaces_check's C subscriber reads too), std_msgs/UInt8MultiArray (a sequence, and a nested
// message holding one) and sensor_msgs/JointState (string[] plus three float64[], one left empty).
// sub takes them as C++ messages and compares every field. Those two types are the ones rclcpp
// could not send at all while rmw_tickle converted C++ messages with the C converters, and a take
// then wrote C allocations into C++ objects.
//
// Exit 0: pub published for SECONDS; sub received each of the four intact. 1: sub saw a wrong
// sample or ran out of time. 3: rclcpp refused to start, with the reason.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/joint_state.hpp"
#include "std_msgs/msg/header.hpp"
#include "std_msgs/msg/multi_array_dimension.hpp"
#include "std_msgs/msg/string.hpp"
#include "std_msgs/msg/u_int8_multi_array.hpp"

namespace {
    constexpr int32_t stamp_sec = 1234;
    constexpr uint32_t stamp_nsec = 567890U;
    constexpr long rounds_per_second = 10;
    constexpr std::chrono::milliseconds period {100};
    constexpr size_t qos_depth = 10; // RELIABLE, KEEP_LAST 10 - ROS 2's default, as interfaces_check
    constexpr uint8_t byte_seven = 7;
    constexpr uint8_t byte_max = 255;
    constexpr uint32_t dim_size = 3;
    constexpr double joint_a_position = 1.5;
    constexpr double joint_b_position = -2.25;
    constexpr double joint_a_velocity = 0.5;

    auto expected_text() -> std_msgs::msg::String {
        std_msgs::msg::String text;
        text.data = "hello from rmw_tickle";
        return text;
    }

    auto expected_header() -> std_msgs::msg::Header {
        std_msgs::msg::Header header;
        header.stamp.sec = stamp_sec;
        header.stamp.nanosec = stamp_nsec;
        header.frame_id = "tickle_frame";
        return header;
    }

    auto expected_bytes() -> std_msgs::msg::UInt8MultiArray {
        std_msgs::msg::UInt8MultiArray bytes;
        std_msgs::msg::MultiArrayDimension dim;
        dim.label = "x";
        dim.size = dim_size;
        dim.stride = dim_size;
        bytes.layout.dim.push_back(dim);
        bytes.layout.data_offset = 0;
        bytes.data = {byte_seven, 0, byte_max};
        return bytes;
    }

    auto expected_joints() -> sensor_msgs::msg::JointState {
        sensor_msgs::msg::JointState joints;
        joints.header = expected_header();
        joints.name = {"joint_a", "joint_b"};
        joints.position = {joint_a_position, joint_b_position};
        joints.velocity = {joint_a_velocity, 0.0};
        // effort deliberately left empty - an empty sequence has to come back empty, not absent
        return joints;
    }

    auto publish_for(const rclcpp::Node::SharedPtr& node, long seconds) -> int {
        auto text_pub = node->create_publisher<std_msgs::msg::String>("/tickle_check_string", qos_depth);
        auto header_pub = node->create_publisher<std_msgs::msg::Header>("/tickle_check_header", qos_depth);
        auto bytes_pub = node->create_publisher<std_msgs::msg::UInt8MultiArray>("/tickle_check_bytes", qos_depth);
        auto joints_pub = node->create_publisher<sensor_msgs::msg::JointState>("/tickle_check_joints", qos_depth);
        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);
        const long rounds = seconds * rounds_per_second;
        for (long i = 0; i < rounds && rclcpp::ok(); i++) {
            text_pub->publish(expected_text());
            header_pub->publish(expected_header());
            bytes_pub->publish(expected_bytes());
            joints_pub->publish(expected_joints());
            executor.spin_some(); // serves the type description and parameter services meanwhile
            std::this_thread::sleep_for(period);
        }
        std::printf("default_node pub: published %ld of each from a default rclcpp::Node\n", rounds);
        return 0;
    }

    // Whether `received` is exactly what pub sent, compared with rosidl_generator_cpp's own
    // field-by-field operator==. Printed on first arrival; a type only counts once it arrives intact.
    template <typename Message> auto expected_for() -> Message {
        if constexpr (std::is_same_v<Message, std_msgs::msg::String>) {
            return expected_text();
        } else if constexpr (std::is_same_v<Message, std_msgs::msg::Header>) {
            return expected_header();
        } else if constexpr (std::is_same_v<Message, std_msgs::msg::UInt8MultiArray>) {
            return expected_bytes();
        } else {
            return expected_joints();
        }
    }

    template <typename Message> void report(const char* label, const Message& received, bool& got) {
        const bool intact = received == expected_for<Message>();
        if (!got) {
            std::printf("%s: %s\n", label, intact ? "intact" : "WRONG");
        }
        got = got || intact;
    }

    auto subscribe_for(const rclcpp::Node::SharedPtr& node, long seconds) -> int {
        std::array<bool, 4> got {};
        auto text_sub = node->create_subscription<std_msgs::msg::String>("/tickle_check_string", qos_depth,
                                                                         [&got](const std_msgs::msg::String& msg) {
                                                                             report("std_msgs/String", msg, got[0]);
                                                                         });
        auto header_sub = node->create_subscription<std_msgs::msg::Header>("/tickle_check_header", qos_depth,
                                                                           [&got](const std_msgs::msg::Header& msg) {
                                                                               report("std_msgs/Header", msg, got[1]);
                                                                           });
        auto bytes_sub = node->create_subscription<std_msgs::msg::UInt8MultiArray>(
            "/tickle_check_bytes", qos_depth, [&got](const std_msgs::msg::UInt8MultiArray& msg) {
                report("std_msgs/UInt8MultiArray", msg, got[2]);
            });
        auto joints_sub = node->create_subscription<sensor_msgs::msg::JointState>(
            "/tickle_check_joints", qos_depth, [&got](const sensor_msgs::msg::JointState& msg) {
                report("sensor_msgs/JointState", msg, got[3]);
            });
        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        auto all = [&got] {
            return got[0] && got[1] && got[2] && got[3];
        };
        while (!all() && rclcpp::ok() && std::chrono::steady_clock::now() < deadline) {
            executor.spin_some(period);
        }
        std::printf("default_node sub: %s\n", all() ? "PASS - all four arrived intact as C++ messages" : "FAIL");
        return all() ? 0 : 1;
    }
} // namespace

auto main(int argc, char** argv) -> int {
    const bool usage_ok = argc == 3 && (std::strcmp(argv[1], "pub") == 0 || std::strcmp(argv[1], "sub") == 0);
    if (!usage_ok) {
        std::fprintf(stderr, "usage: default_node pub|sub SECONDS\n");
        return 2;
    }
    const bool publisher = std::strcmp(argv[1], "pub") == 0;
    rclcpp::init(1, argv);
    int status = 0;
    try {
        auto node = std::make_shared<rclcpp::Node>(publisher ? "tickle_default_pub" : "tickle_default_sub");
        const long seconds = std::stol(argv[2]);
        status = publisher ? publish_for(node, seconds) : subscribe_for(node, seconds);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "default_node: rclcpp refused to start: %s\n", error.what());
        status = 3;
    }
    std::fflush(stdout);
    rclcpp::shutdown();
    return status;
}
