/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Every primitive type, as a fixed array, a bounded sequence and an unbounded sequence, round-tripped through the
// C++ converters rmw_tickle calls (to_tickle, then from_tickle) and compared with operator== (2026-09-27). The
// converters copy these with std::copy / assign() since RMW_PERF_PLAN.md 12.2; this pins that every element of every
// type survives - bool's std::vector<bool> included - full, and at length 0.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_cpp/message_type_support.hpp"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c_tests/msg/primitives.hpp"
#include "rosidl_typesupport_tickle_cpp/identifier.h"

namespace {

    using Primitives = rosidl_typesupport_tickle_c_tests::msg::Primitives;

    constexpr size_t full_length = 5;    // the unbounded sequences' capacity (capacities/*.capacities)
    constexpr size_t bounded_length = 3; // Primitives.msg's [<=3]
    constexpr int seed_step = 7;
    constexpr int element_step = 3;
    constexpr unsigned char poison = 0xa5;

    template <typename Container> auto fill(Container& container, size_t count, int seed) -> void {
        container.clear();
        for (size_t index = 0; index < count; index++) {
            container.push_back(static_cast<typename Container::value_type>((static_cast<size_t>(seed) * seed_step) +
                                                                            (index * element_step) + 1));
        }
    }

    template <typename Array> auto fill_array(Array& array, int seed) -> void {
        for (size_t index = 0; index < array.size(); index++) {
            array[index] = static_cast<typename Array::value_type>((static_cast<size_t>(seed) * seed_step) + index + 1);
        }
    }

    template <typename Array, typename Bounded, typename Sequence>
    auto fill_all(Array& array, Bounded& bounded, Sequence& sequence, int seed, size_t count) -> void {
        fill_array(array, seed);
        fill(bounded, count < bounded_length ? count : bounded_length, seed);
        fill(sequence, count, seed);
    }

    auto make(size_t count) -> Primitives {
        Primitives msg;
        // No bool[<=3] in Primitives.msg (see its header): bool has only its array and its unbounded sequence.
        fill_array(msg.bool_array, 1);
        fill(msg.bool_sequence, count, 1);
        int seed = 2;
        fill_all(msg.byte_array, msg.byte_bounded, msg.byte_sequence, seed++, count);
        fill_all(msg.char_array, msg.char_bounded, msg.char_sequence, seed++, count);
        fill_all(msg.float32_array, msg.float32_bounded, msg.float32_sequence, seed++, count);
        fill_all(msg.float64_array, msg.float64_bounded, msg.float64_sequence, seed++, count);
        fill_all(msg.int8_array, msg.int8_bounded, msg.int8_sequence, seed++, count);
        fill_all(msg.uint8_array, msg.uint8_bounded, msg.uint8_sequence, seed++, count);
        fill_all(msg.int16_array, msg.int16_bounded, msg.int16_sequence, seed++, count);
        fill_all(msg.uint16_array, msg.uint16_bounded, msg.uint16_sequence, seed++, count);
        fill_all(msg.int32_array, msg.int32_bounded, msg.int32_sequence, seed++, count);
        fill_all(msg.uint32_array, msg.uint32_bounded, msg.uint32_sequence, seed++, count);
        fill_all(msg.int64_array, msg.int64_bounded, msg.int64_sequence, seed++, count);
        fill_all(msg.uint64_array, msg.uint64_bounded, msg.uint64_sequence, seed, count);
        return msg;
    }

    auto run() -> int {
        const rosidl_message_type_support_t* top =
            rosidl_typesupport_cpp::get_message_type_support_handle<Primitives>();
        // NOLINTNEXTLINE(misc-include-cleaner) - declared in rosidl_runtime_c/message_type_support_struct.h
        const rosidl_message_type_support_t* ours =
            get_message_typesupport_handle(top, rosidl_typesupport_tickle_cpp__identifier);
        if (ours == nullptr) { // not assert(): a Release build compiles that out, and this would crash instead
            std::printf(
                "no rosidl_typesupport_tickle_cpp handle for Primitives - is this package's install sourced?\n");
            return 1;
        }
        const auto* callbacks = static_cast<const rosidl_typesupport_tickle_c_message_callbacks_t*>(ours->data);
        std::vector<unsigned char> tickle(callbacks->tickle_struct_size);
        int failures = 0;
        for (const size_t count: {full_length, size_t {0}}) {
            const Primitives sent = make(count);
            Primitives received;
            std::memset(tickle.data(), poison, tickle.size()); // nothing may pass through from before
            if (!callbacks->to_tickle(&sent, tickle.data()) || !callbacks->from_tickle(tickle.data(), &received)) {
                std::printf("length %zu: conversion refused\n", count);
                failures++;
                continue;
            }
            if (!(sent == received)) {
                std::printf("length %zu: round trip differs\n", count);
                failures++;
            }
            if (count == full_length && (received.bool_sequence.size() != full_length ||
                                         received.uint64_sequence.back() != sent.uint64_sequence.back())) {
                std::printf("length %zu: sequence length or last element lost\n", count);
                failures++;
            }
        }
        if (failures != 0) {
            return 1;
        }
        std::printf("test_primitives_cpp: all primitive arrays and sequences round-trip\n");
        return 0;
    }

} // namespace

auto main() -> int {
    try {
        return run();
    } catch (const std::exception& error) {
        std::printf("test_primitives_cpp: %s\n", error.what());
        return 1;
    }
}
