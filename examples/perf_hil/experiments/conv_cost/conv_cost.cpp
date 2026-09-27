/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// RMW_PERF_PLAN.md 12: to_tickle / from_tickle of a large primitive sequence (through the tickle_cpp typesupport
// handle, as rmw_tickle calls them), a memcpy of the same bytes, and rclcpp::Serialization under whatever
// RMW_IMPLEMENTATION is set. Prints the converter library actually loaded (dladdr), which is the identity check
// between the -O0 and -O2 overlays.
//
// A colcon package (2026-09-27) so that CI's clang-tidy has a compile database entry for it: built by hand, it had
// none, and turned af54d45e's Check all red on "'rclcpp/rclcpp.hpp' file not found".
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <memory>
#include <ratio>
#include <string>
#include <vector>

#include <rclcpp/serialization.hpp>
#include <rclcpp/serialized_message.hpp>
#include <rosidl_runtime_c/message_type_support_struct.h>
#include <rosidl_typesupport_cpp/message_type_support.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <std_msgs/msg/byte_multi_array.hpp>

#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_cpp/identifier.h"

namespace {

    constexpr int default_rounds = 20;
    constexpr int default_calls = 2000;
    constexpr size_t image_bytes = 64000;
    constexpr size_t byte_array_bytes = 16384;
    constexpr size_t tickle_alignment = 8;
    constexpr unsigned char copy_fill = 0x5a;
    constexpr size_t image_step = 7;
    constexpr size_t byte_array_step = 3;

    auto now_us() -> double {
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    auto median(std::vector<double> values) -> double {
        std::ranges::sort(values);
        return values[values.size() / 2];
    }

    struct free_deleter {
        auto operator()(void* pointer) const -> void {
            std::free(pointer);
        } // NOLINT(cppcoreguidelines-no-malloc)
    };

    template <typename Message> auto run(const char* name, Message& msg, size_t bytes, int rounds, int calls) -> void {
        const rosidl_message_type_support_t* top = rosidl_typesupport_cpp::get_message_type_support_handle<Message>();
        const rosidl_message_type_support_t* ours =
            get_message_typesupport_handle(top, rosidl_typesupport_tickle_cpp__identifier);
        if (ours == nullptr) {
            std::printf("%s: no rosidl_typesupport_tickle_cpp handle - is the interface overlay sourced?\n", name);
            return;
        }
        const auto* callbacks = static_cast<const rosidl_typesupport_tickle_c_message_callbacks_t*>(ours->data);
        Dl_info info {};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - dladdr takes the function's address
        dladdr(reinterpret_cast<void*>(callbacks->to_tickle), &info);
        const size_t tickle_size =
            (callbacks->tickle_struct_size + tickle_alignment - 1) / tickle_alignment * tickle_alignment;
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) - the TickLE struct needs its alignment, as rmw_tickle's does
        const std::unique_ptr<void, free_deleter> tickle(std::aligned_alloc(tickle_alignment, tickle_size));
        Message back;
        std::vector<uint8_t> source(bytes, copy_fill);
        std::vector<uint8_t> target(bytes);
        const rclcpp::Serialization<Message> serialization;
        std::vector<double> to_us;
        std::vector<double> from_us;
        std::vector<double> copy_us;
        std::vector<double> serialize_us;
        std::vector<double> deserialize_us;
        for (int round = 0; round < rounds; round++) {
            const double start = now_us();
            for (int call = 0; call < calls; call++) {
                if (!callbacks->to_tickle(&msg, tickle.get())) {
                    std::printf("to_tickle FAILED\n");
                    return;
                }
            }
            const double converted = now_us();
            for (int call = 0; call < calls; call++) {
                if (!callbacks->from_tickle(tickle.get(), &back)) {
                    std::printf("from_tickle FAILED\n");
                    return;
                }
            }
            const double restored = now_us();
            for (int call = 0; call < calls; call++) {
                std::memcpy(target.data(), source.data(), bytes);
                asm volatile("" : : "r"(target.data()) : "memory"); // the copy must not be optimised away
            }
            const double copied = now_us();
            rclcpp::SerializedMessage serialized;
            const double serialize_start = now_us();
            for (int call = 0; call < calls; call++) {
                serialization.serialize_message(&msg, &serialized);
            }
            const double serialize_end = now_us();
            for (int call = 0; call < calls; call++) {
                serialization.deserialize_message(&serialized, &back);
            }
            const double deserialize_end = now_us();
            to_us.push_back((converted - start) / calls);
            from_us.push_back((restored - converted) / calls);
            copy_us.push_back((copied - restored) / calls);
            serialize_us.push_back((serialize_end - serialize_start) / calls);
            deserialize_us.push_back((deserialize_end - serialize_end) / calls);
        }
        const bool same = back.data == msg.data;
        const char* rmw = std::getenv("RMW_IMPLEMENTATION"); // NOLINT(concurrency-mt-unsafe) - one thread
        std::printf(
            "RESULT: type=%s bytes=%zu rmw=%s to_tickle_us=%.3f from_tickle_us=%.3f memcpy_us=%.3f serialize_us=%.3f "
            "deserialize_us=%.3f roundtrip_ok=%d lib=%s\n",
            name, bytes, rmw != nullptr ? rmw : "default", median(to_us), median(from_us), median(copy_us),
            median(serialize_us), median(deserialize_us), same ? 1 : 0,
            info.dli_fname != nullptr ? info.dli_fname : "?");
    }

    auto run_all(int rounds, int calls) -> void {
        sensor_msgs::msg::Image image;
        image.height = 1;
        image.width = image_bytes;
        image.encoding = "mono8";
        image.step = image_bytes;
        image.data.resize(image_bytes);
        for (size_t index = 0; index < image.data.size(); index++) {
            image.data[index] = static_cast<uint8_t>(index * image_step);
        }
        run("Image", image, image.data.size(), rounds, calls);
        std_msgs::msg::ByteMultiArray byte_array;
        byte_array.data.resize(byte_array_bytes);
        for (size_t index = 0; index < byte_array.data.size(); index++) {
            byte_array.data[index] = static_cast<uint8_t>(index * byte_array_step);
        }
        run("ByteMultiArray", byte_array, byte_array.data.size(), rounds, calls);
    }

} // namespace

auto main(int argc, char** argv) -> int {
    try {
        const std::vector<std::string> args(argv, argv + argc);
        const int rounds = args.size() > 1 ? std::stoi(args[1]) : default_rounds;
        const int calls = args.size() > 2 ? std::stoi(args[2]) : default_calls;
        run_all(rounds, calls);
        return 0;
    } catch (const std::exception& error) {
        std::printf("conv_cost: %s\n", error.what());
        return 1;
    }
}
