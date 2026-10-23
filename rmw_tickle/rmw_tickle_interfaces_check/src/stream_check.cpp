/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// stream_check pub TYPE SECONDS | sub TYPE MODE SECONDS: a live two-process stream through rclcpp and
// rmw_tickle, checked sample by sample for ordering. check_ros2_interfaces.sh -S runs one pub/sub pair
// per cell (TYPE x MODE) and decides the verdict from what each subscriber prints.
//
// This is what .github/workflows/rmw-perf.yml (retired 2026-10-23) checked through buildfarm_perf_tests'
// two_process_rmw_ matrix and nothing per-push did: the shape of that matrix is kept - BEST_EFFORT,
// KEEP_LAST 10, 1000 Hz from a default rclcpp::Node, a 1 KiB array topic and a small nested struct
// topic - and so is perf_test's per-sample check, in the same order:
//   - a sample whose publish time is not strictly newer than the previous one delivered is a timestamp
//     violation. perf_test threw "Data consistency violated" on it and the process died in
//     std::terminate(); rmw-perf.yml's patch (f) made it a counter so the run kept its evidence, and
//     failed the job on any. Here too it is counted, never thrown, and the harness fails on any.
//   - a sample whose publish time is newer but whose id is not higher is an out-of-order arrival:
//     counted and reported, not gated (patch (f)'s id variant: BEST_EFFORT promises no order).
// pub: TYPE in array1k|header, publishes for SECONDS at 1000 Hz. sub: MODE block (the executor waits in
// rmw_wait until a sample arrives) or poll (spin_some with a zero timeout, then a short sleep) - the
// two receive paths rmw_tickle has. sub listens for SECONDS and prints one line:
//   stream_check cell TYPE MODE: received=N lost=L out_of_order=J timestamp_not_newer=K
// and exits 0; the harness reads the counters. Exit 2: usage. Exit 3: rclcpp refused to start. Exit 4: an
// exception reached main, printed.
//
// STREAM_CHECK_INJECT, for the harness's own controls and never set in a real run:
//   timestamp_regress (pub): every 100th sample carries the previous sample's time - the violation.
//   drop_result (sub): exits 0 without printing its cell line - a cell that "passed" having run nothing.
//   abort (sub): prints perf_test's "Data consistency violated" from the first callback and aborts, as
//     perf_test's uncaught throw did through std::terminate - the abort class rmw-perf.yml found.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#include "identity.h"
#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/header.hpp"
#include "std_msgs/msg/u_int8_multi_array.hpp"

namespace {
    constexpr size_t qos_depth = 10; // perf_test's --keep_last --history_depth 10, as buildfarm passed it
    constexpr std::chrono::microseconds publish_period {1000}; // perf_test's default --rate 1000
    constexpr std::chrono::microseconds poll_sleep {200};
    constexpr std::chrono::milliseconds block_slice {100};
    constexpr size_t array_bytes = 1024; // perf_test's Array1k
    constexpr uint64_t regress_every = 100;
    constexpr int64_t ns_per_second = 1000000000;
    constexpr int exit_usage = 2;
    constexpr int exit_refused = 3;
    constexpr int exit_exception = 4;

    struct stamp {
        int64_t time;
        uint64_t id;
    };

    auto make_stamp(int64_t time, uint64_t id) -> stamp {
        stamp value {};
        value.time = time;
        value.id = id;
        return value;
    }

    auto inject(const char* what) -> bool {
        const char* value = std::getenv("STREAM_CHECK_INJECT");
        return value != nullptr && std::strcmp(value, what) == 0;
    }

    auto topic_of(const std::string& type) -> std::string {
        return "/tickle_stream_" + type;
    }

    // array1k carries the time and the id in its first 16 bytes; header in stamp and frame_id.
    void fill(std_msgs::msg::UInt8MultiArray& msg, const stamp& value) {
        msg.data.resize(array_bytes);
        std::memcpy(msg.data.data(), &value.time, sizeof(value.time));
        std::memcpy(msg.data.data() + sizeof(value.time), &value.id, sizeof(value.id));
    }

    void fill(std_msgs::msg::Header& msg, const stamp& value) {
        msg.stamp.sec = static_cast<int32_t>(value.time / ns_per_second);
        msg.stamp.nanosec = static_cast<uint32_t>(value.time % ns_per_second);
        msg.frame_id = std::to_string(value.id);
    }

    auto read(const std_msgs::msg::UInt8MultiArray& msg) -> stamp {
        stamp value {};
        if (msg.data.size() >= sizeof(value.time) + sizeof(value.id)) {
            std::memcpy(&value.time, msg.data.data(), sizeof(value.time));
            std::memcpy(&value.id, msg.data.data() + sizeof(value.time), sizeof(value.id));
        }
        return value;
    }

    auto read(const std_msgs::msg::Header& msg) -> stamp {
        return make_stamp((static_cast<int64_t>(msg.stamp.sec) * ns_per_second) + msg.stamp.nanosec,
                          std::strtoull(msg.frame_id.c_str(), nullptr, 10));
    }

    auto best_effort() -> rclcpp::QoS {
        return rclcpp::QoS(rclcpp::KeepLast(qos_depth)).best_effort();
    }

    template <typename Message>
    auto publish_for(const rclcpp::Node::SharedPtr& node, const std::string& type, long seconds) -> int {
        auto pub = node->create_publisher<Message>(topic_of(type), best_effort());
        report_identity("stream_check pub");
        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);
        const bool regress = inject("timestamp_regress");
        const auto start = std::chrono::steady_clock::now();
        const auto end = start + std::chrono::seconds(seconds);
        auto next = start;
        uint64_t id = 0;
        int64_t previous_time = 0;
        Message msg;
        while (rclcpp::ok() && std::chrono::steady_clock::now() < end) {
            ++id; // never 0, as perf_test, so a default-constructed sample cannot pass as the first
            int64_t time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
            if (regress && id % regress_every == 0) {
                time = previous_time;
            }
            previous_time = time;
            fill(msg, make_stamp(time, id));
            pub->publish(msg);
            executor.spin_some(std::chrono::nanoseconds(0)); // serves rclcpp's own services meanwhile
            next += publish_period;
            std::this_thread::sleep_until(next);
        }
        std::printf("stream_check pub %s: published %llu\n", type.c_str(), static_cast<unsigned long long>(id));
        return 0;
    }

    struct counters {
        uint64_t received = 0;
        uint64_t lost = 0;
        uint64_t out_of_order = 0;
        uint64_t timestamp_not_newer = 0;
        int64_t prev_time = 0;
        uint64_t prev_id = 0;
    };

    // perf_test's order: the timestamp first, and a sample failing it is not counted further.
    void check(counters& seen, const stamp& value) {
        if (seen.prev_time >= value.time) {
            ++seen.timestamp_not_newer;
            std::fprintf(stderr, "timestamp_not_newer: received time %lld after %lld (total so far: %llu)\n",
                         static_cast<long long>(value.time), static_cast<long long>(seen.prev_time),
                         static_cast<unsigned long long>(seen.timestamp_not_newer));
            return;
        }
        seen.prev_time = value.time;
        ++seen.received;
        if (value.id <= seen.prev_id) {
            ++seen.out_of_order;
            std::fprintf(stderr, "out_of_order: received id %llu after %llu\n",
                         static_cast<unsigned long long>(value.id), static_cast<unsigned long long>(seen.prev_id));
            return;
        }
        seen.lost += value.id - seen.prev_id - 1;
        seen.prev_id = value.id;
    }

    template <typename Message>
    auto subscribe_for(const rclcpp::Node::SharedPtr& node, const std::string& type, const std::string& mode,
                       long seconds) -> int {
        counters seen;
        const bool abort_now = inject("abort");
        auto sub =
            node->create_subscription<Message>(topic_of(type), best_effort(), [&seen, abort_now](const Message& msg) {
                if (abort_now) {
                    std::fprintf(stderr, "stream_check: Data consistency violated (injected)\n");
                    std::abort();
                }
                check(seen, read(msg));
            });
        report_identity("stream_check sub");
        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (rclcpp::ok() && std::chrono::steady_clock::now() < deadline) {
            if (mode == "block") {
                executor.spin_once(block_slice);
            } else {
                executor.spin_some(std::chrono::nanoseconds(0));
                std::this_thread::sleep_for(poll_sleep);
            }
        }
        if (inject("drop_result")) {
            return 0;
        }
        std::printf("stream_check cell %s %s: received=%llu lost=%llu out_of_order=%llu timestamp_not_newer=%llu\n",
                    type.c_str(), mode.c_str(), static_cast<unsigned long long>(seen.received),
                    static_cast<unsigned long long>(seen.lost), static_cast<unsigned long long>(seen.out_of_order),
                    static_cast<unsigned long long>(seen.timestamp_not_newer));
        return 0;
    }

    template <typename Message>
    auto run(const rclcpp::Node::SharedPtr& node, bool publisher, const std::string& type, const std::string& mode,
             long seconds) -> int {
        return publisher ? publish_for<Message>(node, type, seconds)
                         : subscribe_for<Message>(node, type, mode, seconds);
    }
} // namespace

auto main(int argc, char** argv) -> int {
    const bool publisher = argc == 4 && std::strcmp(argv[1], "pub") == 0;
    const bool subscriber = argc == 5 && std::strcmp(argv[1], "sub") == 0;
    const bool type_ok =
        (publisher || subscriber) && (std::strcmp(argv[2], "array1k") == 0 || std::strcmp(argv[2], "header") == 0);
    const bool mode_ok =
        publisher || (subscriber && (std::strcmp(argv[3], "block") == 0 || std::strcmp(argv[3], "poll") == 0));
    if (!type_ok || !mode_ok) {
        std::fprintf(stderr,
                     "usage: stream_check pub array1k|header SECONDS | sub array1k|header block|poll SECONDS\n");
        return exit_usage;
    }
    rclcpp::init(1, argv);
    int status = 0;
    try {
        const std::string type = argv[2];
        const std::string mode = subscriber ? argv[3] : "";
        const long seconds = std::stol(argv[argc - 1]);
        rclcpp::Node::SharedPtr node;
        try {
            node = std::make_shared<rclcpp::Node>(publisher ? "tickle_stream_pub" : "tickle_stream_sub");
        } catch (const std::exception& error) {
            std::fprintf(stderr, "stream_check: rclcpp refused to start: %s\n", error.what());
            rclcpp::shutdown();
            return exit_refused;
        }
        status = type == "array1k" ? run<std_msgs::msg::UInt8MultiArray>(node, publisher, type, mode, seconds)
                                   : run<std_msgs::msg::Header>(node, publisher, type, mode, seconds);
    } catch (const std::exception& error) {
        // Not swallowed: a non-zero exit fails the cell, and the message names what was thrown.
        std::fprintf(stderr, "stream_check: exception: %s\n", error.what());
        status = exit_exception;
    }
    std::fflush(stdout);
    rclcpp::shutdown();
    return status;
}
