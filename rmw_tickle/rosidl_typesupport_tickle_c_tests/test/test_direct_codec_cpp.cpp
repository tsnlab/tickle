/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The C++ direct codec's rules that its arm of the pass-1 harness cannot reach.
//
// That arm carries each sample across as a TickLE struct - a C message, to_tickle, then the C++
// from_tickle - and compares the two encodings byte for byte. It covers every type's layout, and it
// is how the C++ codec is held to the C one. What it cannot carry is a std::string with a NUL
// inside it: the struct holds a char*, so the C++ object it rebuilds ends at the first NUL and the
// question never arises. The rule still has to hold, because a C++ node can publish such a string
// directly, so it is asserted here against bytes written out in full rather than against another
// encoder. `cpp_string_size` (TICKLE_DIRECT_CODEC_MUTANT) is the mutant this exists for: it
// survives the harness and must fail here.
//
// std::vector<bool> is the other C++-only shape - it has no data(), so its elements are written one
// at a time where every other sequence is one memcpy - and it is asserted here for the same reason:
// a wrong element loop would still agree with itself.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rosidl_typesupport_tickle_c_tests/msg/dc_inner.hpp"
#include "rosidl_typesupport_tickle_c_tests/msg/dc_nested.hpp"
#include "rosidl_typesupport_tickle_c_tests__msg__DcInner__rosidl_typesupport_tickle_cpp.hpp"
#include "rosidl_typesupport_tickle_c_tests__msg__DcNested__rosidl_typesupport_tickle_cpp.hpp"

namespace {

    int failures = 0;

    auto check(bool condition, const char* what) -> void {
        if (!condition) {
            std::printf("FAIL %s\n", what);
            failures++;
        }
    }

    constexpr size_t buffer_bytes = 4096;
    // "ab" and its NUL take 2 + 3, padded to 8; the empty string that follows takes 2 + 1, padded
    // to 12.
    constexpr int32_t two_strings_bytes = 12;

    // "ab\0cd": the wire carries "ab" and its NUL, and nothing after it.
    auto test_a_string_ends_at_its_first_nul() -> void {
        rosidl_typesupport_tickle_c_tests::msg::DcInner message;
        message.a = std::string("ab\0cd", 5);
        message.b = "";

        std::vector<uint64_t> buffer(buffer_bytes / 8, 0);
        auto* payload = reinterpret_cast<uint8_t*>(buffer.data());
        namespace ts = rosidl_typesupport_tickle_c_tests::msg::rosidl_typesupport_tickle_cpp;
        int32_t const size = ts::direct_encode(message, payload, buffer_bytes);
        int32_t const sized = ts::direct_encode_size(message);

        check(size > 0, "an encodable message encodes");
        check(sized == size, "direct_encode_size agrees with direct_encode");
        if (size <= 0) {
            return;
        }
        uint16_t prefix = 0;
        std::memcpy(&prefix, payload, 2);
        check(prefix == 3, "the length prefix counts 'ab' and its NUL, not the characters after it");
        check(payload[2] == 'a' && payload[3] == 'b' && payload[4] == 0, "the bytes are 'a', 'b', NUL");
        check(size == two_strings_bytes, "and nothing after the NUL is on the wire");

        rosidl_typesupport_tickle_c_tests::msg::DcInner decoded;
        decoded.a = "stale";
        int32_t const read = ts::direct_decode(decoded, payload, static_cast<uint32_t>(size), true);
        check(read == size, "the bytes decode");
        check(decoded.a == "ab", "and give back the string the wire carried");
        check(decoded.b.empty(), "an empty string stays empty");
    }

    // A bounded string is refused above its bound, where the harness's C arm only ever sees one
    // refused by a TickLE capacity.
    auto test_a_bounded_string_is_refused_above_its_bound() -> void {
        rosidl_typesupport_tickle_c_tests::msg::DcNested message;
        message.short_name = "12345"; // string<=5: the bound itself is fine
        std::vector<uint64_t> buffer(buffer_bytes / 8, 0);
        auto* payload = reinterpret_cast<uint8_t*>(buffer.data());
        namespace ts = rosidl_typesupport_tickle_c_tests::msg::rosidl_typesupport_tickle_cpp;
        check(ts::direct_encode(message, payload, buffer_bytes) > 0, "a bounded string at its bound is encoded");

        message.short_name = "123456"; // one over
        check(ts::direct_encode(message, payload, buffer_bytes) == -2, "one character over its bound is refused");
        check(ts::direct_encode_size(message) == -2, "and its size is refused too");
    }

    // std::vector<bool>, whose elements are written one at a time.
    auto test_a_bool_sequence_round_trips() -> void {
        rosidl_typesupport_tickle_c_tests::msg::DcNested message;
        message.short_name = "x";
        message.flags = {true, false, true, true, false};

        std::vector<uint64_t> buffer(buffer_bytes / 8, 0);
        auto* payload = reinterpret_cast<uint8_t*>(buffer.data());
        namespace ts = rosidl_typesupport_tickle_c_tests::msg::rosidl_typesupport_tickle_cpp;
        int32_t const size = ts::direct_encode(message, payload, buffer_bytes);
        check(size > 0, "a message with a bool sequence encodes");
        if (size <= 0) {
            return;
        }

        rosidl_typesupport_tickle_c_tests::msg::DcNested decoded;
        decoded.flags = {false, false}; // a shell that already holds something
        check(ts::direct_decode(decoded, payload, static_cast<uint32_t>(size), true) == size, "the bytes decode");
        check(decoded.flags == message.flags, "and every bool comes back as it went");
    }

} // namespace

auto main() -> int {
    test_a_string_ends_at_its_first_nul();
    test_a_bounded_string_is_refused_above_its_bound();
    test_a_bool_sequence_round_trips();
    if (failures != 0) {
        std::printf("test_direct_codec_cpp: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_direct_codec_cpp: all tests passed\n");
    return 0;
}
