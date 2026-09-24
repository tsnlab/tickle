/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// action_check server|client SECONDS: a ROS 2 action - example_interfaces/action/Fibonacci -
// between two processes through rmw_tickle, with rclcpp_action on both ends and default nodes.
//
// rmw has no action API: rcl_action builds an action from three services (SendGoal, GetResult,
// action_msgs' CancelGoal) and two topics (FeedbackMessage, action_msgs' GoalStatusArray), so this
// exercises the implicit interfaces rosidl_typesupport_tickle_c generates for a .action, the
// status topic's TRANSIENT_LOCAL QoS, and a service answered after a delay (GetResult is only
// answered once the goal finishes).
//
// server: accepts any goal, sends the sequence so far as feedback after each step, succeeds with
// the whole sequence - or, if cancelled, ends CANCELED with what it had; runs for SECONDS. client:
// sends a goal of order 10 and checks every feedback is a prefix of the true sequence and the result
// is exactly it; then sends one of order 40, cancels it partway, and checks the cancel is accepted
// and the goal ends CANCELED with a prefix. Exit 0 on success; 1 on a wrong
// or missing answer; 3 if rclcpp refused to start, with the reason.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "action_msgs/srv/cancel_goal.hpp"
#include "example_interfaces/action/fibonacci.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/client.hpp"
#include "rclcpp_action/client_goal_handle.hpp"
#include "rclcpp_action/create_client.hpp"
#include "rclcpp_action/create_server.hpp"
#include "rclcpp_action/server.hpp"
#include "rclcpp_action/server_goal_handle.hpp"
#include "rclcpp_action/types.hpp"

namespace {
    using Fibonacci = example_interfaces::action::Fibonacci;
    using GoalHandle = rclcpp_action::ServerGoalHandle<Fibonacci>;

    constexpr int32_t goal_order = 10;
    constexpr int32_t cancelled_order = 40; // 2 s of steps - long enough to cancel partway
    constexpr std::chrono::milliseconds before_cancel {300};
    constexpr std::chrono::milliseconds step {50};
    constexpr std::chrono::seconds server_wait {10};

    // The first order + 1 Fibonacci numbers: 0, 1, 1, 2, 3, ...
    auto fibonacci(int32_t order) -> std::vector<int32_t> {
        std::vector<int32_t> sequence {0, 1};
        while (static_cast<int32_t>(sequence.size()) < order + 1) {
            sequence.push_back(sequence[sequence.size() - 1] + sequence[sequence.size() - 2]);
        }
        sequence.resize(static_cast<size_t>(order) + 1);
        return sequence;
    }

    void execute(const std::shared_ptr<GoalHandle>& goal_handle) {
        const auto sequence = fibonacci(goal_handle->get_goal()->order);
        auto feedback = std::make_shared<Fibonacci::Feedback>();
        auto result = std::make_shared<Fibonacci::Result>();
        for (const int32_t value: sequence) {
            if (goal_handle->is_canceling()) {
                result->sequence = feedback->sequence; // what was computed before the cancel
                goal_handle->canceled(result);
                return;
            }
            feedback->sequence.push_back(value);
            goal_handle->publish_feedback(feedback);
            std::this_thread::sleep_for(step);
        }
        result->sequence = sequence;
        goal_handle->succeed(result);
    }

    auto serve_for(const rclcpp::Node::SharedPtr& node, long seconds) -> int {
        auto server = rclcpp_action::create_server<Fibonacci>(
            node, "/tickle_check_fibonacci",
            [](const rclcpp_action::GoalUUID&, const std::shared_ptr<const Fibonacci::Goal>& goal) {
                std::printf("action server: goal of order %d accepted\n", goal->order);
                return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
            },
            [](const std::shared_ptr<GoalHandle>&) {
                return rclcpp_action::CancelResponse::ACCEPT;
            },
            [](const std::shared_ptr<GoalHandle>& goal_handle) {
                std::thread(execute, goal_handle).detach();
            });
        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);
        executor.spin_until_future_complete(std::promise<void>().get_future(), std::chrono::seconds(seconds));
        std::printf("action server: done\n");
        return 0;
    }

    auto is_prefix(const std::vector<int32_t>& part, const std::vector<int32_t>& whole) -> bool {
        return part.size() <= whole.size() && std::equal(part.begin(), part.end(), whole.begin());
    }

    // A second goal, cancelled partway: action_msgs' CancelGoal service, the server seeing the
    // request, and the goal ending CANCELED with the part computed so far.
    auto cancel_one(const rclcpp::Node::SharedPtr& node, const rclcpp_action::Client<Fibonacci>::SharedPtr& client,
                    std::chrono::seconds timeout) -> bool {
        Fibonacci::Goal goal;
        goal.order = cancelled_order;
        auto goal_future = client->async_send_goal(goal);
        if (rclcpp::spin_until_future_complete(node, goal_future, timeout) != rclcpp::FutureReturnCode::SUCCESS ||
            !goal_future.get()) {
            std::printf("action client: FAIL - the goal to cancel was not accepted\n");
            return false;
        }
        const auto& handle = goal_future.get();
        auto result_future = client->async_get_result(handle);
        rclcpp::spin_until_future_complete(node, result_future, before_cancel); // let it run a little
        auto cancel_future = client->async_cancel_goal(handle);
        if (rclcpp::spin_until_future_complete(node, cancel_future, timeout) != rclcpp::FutureReturnCode::SUCCESS) {
            std::printf("action client: FAIL - no answer to the cancel request\n");
            return false;
        }
        const auto& cancel_response = cancel_future.get();
        const bool cancel_accepted =
            cancel_response->return_code == action_msgs::srv::CancelGoal::Response::ERROR_NONE &&
            cancel_response->goals_canceling.size() == 1;
        if (rclcpp::spin_until_future_complete(node, result_future, timeout) != rclcpp::FutureReturnCode::SUCCESS) {
            std::printf("action client: FAIL - no result for the cancelled goal\n");
            return false;
        }
        const auto& wrapped = result_future.get();
        const auto& partial = wrapped.result->sequence;
        const bool cancelled = wrapped.code == rclcpp_action::ResultCode::CANCELED &&
                               partial.size() < static_cast<size_t>(cancelled_order) + 1 &&
                               is_prefix(partial, fibonacci(cancelled_order));
        std::printf("action client: cancel %s, goal ended %s with %zu of %d values\n",
                    cancel_accepted ? "accepted" : "REFUSED", cancelled ? "CANCELED" : "WRONG", partial.size(),
                    cancelled_order + 1);
        return cancel_accepted && cancelled;
    }

    auto request(const rclcpp::Node::SharedPtr& node, long seconds) -> int {
        auto client = rclcpp_action::create_client<Fibonacci>(node, "/tickle_check_fibonacci");
        if (!client->wait_for_action_server(server_wait)) {
            std::printf("action client: FAIL - no action server within %ld s\n",
                        static_cast<long>(server_wait.count()));
            return 1;
        }
        const auto expected = fibonacci(goal_order);
        size_t feedbacks = 0;
        bool feedback_ok = true;
        auto options = rclcpp_action::Client<Fibonacci>::SendGoalOptions();
        options.feedback_callback = [&](const std::shared_ptr<rclcpp_action::ClientGoalHandle<Fibonacci>>&,
                                        const std::shared_ptr<const Fibonacci::Feedback>& feedback) {
            feedbacks++;
            feedback_ok = feedback_ok && is_prefix(feedback->sequence, expected);
        };
        Fibonacci::Goal goal;
        goal.order = goal_order;
        auto goal_future = client->async_send_goal(goal, options);
        const auto timeout = std::chrono::seconds(seconds);
        if (rclcpp::spin_until_future_complete(node, goal_future, timeout) != rclcpp::FutureReturnCode::SUCCESS ||
            !goal_future.get()) {
            std::printf("action client: FAIL - the goal was not accepted\n");
            return 1;
        }
        auto result_future = client->async_get_result(goal_future.get());
        if (rclcpp::spin_until_future_complete(node, result_future, timeout) != rclcpp::FutureReturnCode::SUCCESS) {
            std::printf("action client: FAIL - no result\n");
            return 1;
        }
        const auto& wrapped = result_future.get();
        const bool result_ok =
            wrapped.code == rclcpp_action::ResultCode::SUCCEEDED && wrapped.result->sequence == expected;
        std::printf("action client: %zu feedback(s) %s, result %s (%zu values)\n", feedbacks,
                    feedback_ok ? "all prefixes of the sequence" : "WRONG", result_ok ? "exact" : "WRONG",
                    wrapped.result->sequence.size());
        const bool cancel_ok = cancel_one(node, client, timeout);
        const bool pass = result_ok && feedback_ok && feedbacks > 0 && cancel_ok;
        std::printf("action client: %s\n",
                    pass ? "PASS - Fibonacci round-tripped as an action, and a second goal was cancelled" : "FAIL");
        return pass ? 0 : 1;
    }
} // namespace

auto main(int argc, char** argv) -> int {
    const bool usage_ok = argc == 3 && (std::strcmp(argv[1], "server") == 0 || std::strcmp(argv[1], "client") == 0);
    if (!usage_ok) {
        std::fprintf(stderr, "usage: action_check server|client SECONDS\n");
        return 2;
    }
    const bool server = std::strcmp(argv[1], "server") == 0;
    rclcpp::init(1, argv);
    int status = 0;
    try {
        auto node = std::make_shared<rclcpp::Node>(server ? "tickle_action_server" : "tickle_action_client");
        const long seconds = std::stol(argv[2]);
        status = server ? serve_for(node, seconds) : request(node, seconds);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "action_check: rclcpp refused to start: %s\n", error.what());
        status = 3;
    } catch (...) {
        std::fprintf(stderr, "action_check: failed with a non-standard exception\n");
        status = 3;
    }
    std::fflush(stdout);
    rclcpp::shutdown();
    return status;
}
