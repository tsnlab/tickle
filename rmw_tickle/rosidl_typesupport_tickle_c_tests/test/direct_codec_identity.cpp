/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Pass 1 of rmw_tickle/LARGE_MESSAGE_PLAN.md ("Pass 1 harness"): the direct codec against today's
// path (to_tickle, then the TickLE struct codec), over any type whose package built
// rosidl_typesupport_tickle_c. One program, not one generated per type: each type's handles are
// found by symbol name, rosidl_typesupport_introspection_c builds and compares its messages and
// knows its IDL bounds, and the tickle handle supplies both paths.
//
// Per type, 200 random messages. Three in four aim to be byte-compared, shrinking until the old
// path accepts; the fourth fills unbounded fields past a TickLE capacity, into the capacity-only
// class. For each message:
//   1. encode: when the old path accepts, the new one gives the same bytes and direct_encode_size
//      says how many. When the old path refuses at to_tickle - a TickLE struct capacity, not a
//      limit of the ROS type - the new one may accept if the message is within its IDL bounds and
//      65,535; that is the capacity-only class. Any other disagreement fails.
//      1b. whatever the new path accepts decodes back to the original (floats bitwise, strings up
//      to their first NUL, which is all the wire carries), into a shell holding the last sample.
//   2. over-bound inputs, built on purpose: both paths refuse.
//   3. differential decode of truncated and bit-flipped bytes, and
//   4. the same with is_native_endian false: both decoders accept or refuse alike and agree on
//      what they accept, except the capacity-only class (the new result is one to_tickle refuses,
//      within the IDL bounds).
// A type needs half its samples byte-compared (check 1's identical branch).
//
// Usage: direct_codec_identity [-s seed] [-n samples] [-d declined_list] (--all | pkg...)

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include "rosidl_runtime_c/message_initialization.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/string.h"
#include "rosidl_runtime_c/string_functions.h"
#include "rosidl_typesupport_introspection_c/field_types.h"
#include "rosidl_typesupport_introspection_c/message_introspection.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "tickle/tickle.h" // tt_Data, the struct codec's own opaque message pointer

namespace {

    using message_members = rosidl_typesupport_introspection_c__MessageMembers;
    using message_member = rosidl_typesupport_introspection_c__MessageMember;
    using tickle_callbacks = rosidl_typesupport_tickle_c_message_callbacks_t;

    constexpr uint32_t buffer_length = 65507; // TickLE's per-sample limit at rmw's build
    constexpr size_t unbounded_fill = 300;    // "bound-filling" for a field with no IDL bound
    constexpr size_t max_count = 65535;       // a uint16 count or string length: the wire's own limit
    constexpr int default_samples = 200;
    constexpr size_t small_message = 256; // up to this size, every truncation is tried
    constexpr size_t random_truncations = 32;
    constexpr size_t bit_flips = 32;
    constexpr int reported_failures = 5; // per type, before the rest are only counted
    constexpr int from_tickle_failed = -100;
    constexpr size_t letters = 26; // the alphabet a random string is drawn from

    auto members_of(const rosidl_message_type_support_t* support) -> const message_members* {
        return static_cast<const message_members*>(support->data);
    }

    auto is_sequence(const message_member& member) -> bool {
        return member.is_array_ && (member.array_size_ == 0 || member.is_upper_bound_);
    }

    // rosidl gives a message with no fields of its own a single `uint8_t
    // structure_needs_at_least_one_member`, so that its C struct is valid C. TickLE's generator has
    // no such field, and neither codec carries it, so it is not data: the harness neither fills it
    // nor compares it, and an empty message is genuinely 0 bytes on both paths.
    auto is_placeholder(const message_member& member) -> bool {
        return std::strcmp(member.name_, "structure_needs_at_least_one_member") == 0;
    }

    auto scalar_size(uint8_t type_id) -> size_t {
        switch (type_id) {
        case rosidl_typesupport_introspection_c__ROS_TYPE_FLOAT:
            return 4;
        case rosidl_typesupport_introspection_c__ROS_TYPE_DOUBLE:
            return 8;
        case rosidl_typesupport_introspection_c__ROS_TYPE_CHAR:
        case rosidl_typesupport_introspection_c__ROS_TYPE_BOOLEAN:
        case rosidl_typesupport_introspection_c__ROS_TYPE_OCTET:
        case rosidl_typesupport_introspection_c__ROS_TYPE_UINT8:
        case rosidl_typesupport_introspection_c__ROS_TYPE_INT8:
            return 1;
        case rosidl_typesupport_introspection_c__ROS_TYPE_UINT16:
        case rosidl_typesupport_introspection_c__ROS_TYPE_INT16:
            return 2;
        case rosidl_typesupport_introspection_c__ROS_TYPE_UINT32:
        case rosidl_typesupport_introspection_c__ROS_TYPE_INT32:
            return 4;
        case rosidl_typesupport_introspection_c__ROS_TYPE_UINT64:
        case rosidl_typesupport_introspection_c__ROS_TYPE_INT64:
            return 8;
        default:
            return 0; // string, message, or a type TickLE declines (long double, wchar, wstring)
        }
    }

    auto is_text(const message_member& member) -> bool {
        return member.type_id_ == rosidl_typesupport_introspection_c__ROS_TYPE_STRING;
    }

    auto is_nested(const message_member& member) -> bool {
        return member.type_id_ == rosidl_typesupport_introspection_c__ROS_TYPE_MESSAGE;
    }

    auto element_size(const message_member& member) -> size_t {
        if (is_text(member)) {
            return sizeof(rosidl_runtime_c__String);
        }
        if (is_nested(member)) {
            return members_of(member.members_)->size_of_;
        }
        return scalar_size(member.type_id_);
    }

    auto count_of(const message_member& member, const void* field) -> size_t {
        return is_sequence(member) ? member.size_function(field) : member.array_size_;
    }

    auto element_at(const message_member& member, void* field, size_t index) -> void* {
        if (member.get_function != nullptr) {
            return member.get_function(field, index);
        }
        return static_cast<char*>(field) + (index * element_size(member));
    }

    auto element_at(const message_member& member, const void* field, size_t index) -> const void* {
        if (member.get_const_function != nullptr) {
            return member.get_const_function(field, index);
        }
        return static_cast<const char*>(field) + (index * element_size(member));
    }

    auto first_nul_length(const rosidl_runtime_c__String* str) -> size_t {
        if (str->data == nullptr) {
            return 0;
        }
        const void* nul = std::memchr(str->data, 0, str->size);
        return nul != nullptr ? static_cast<size_t>(static_cast<const char*>(nul) - str->data) : str->size;
    }

    // An initialised message of one type, freed on scope exit.
    class Message {
      public:
        explicit Message(const message_members* type) : type(type), storage(std::calloc(1, type->size_of_)) {
            type->init_function(storage, ROSIDL_RUNTIME_C_MSG_INIT_ALL);
        }
        ~Message() {
            type->fini_function(storage);
            std::free(storage);
        }
        Message(const Message&) = delete;
        Message(Message&&) = delete;
        auto operator=(const Message&) -> Message& = delete;
        auto operator=(Message&&) -> Message& = delete;
        [[nodiscard]] auto get() const -> void* {
            return storage;
        }

      private:
        const message_members* type;
        void* storage;
    };

    // A buffer the codecs may read and write as aligned words, as tt_Context's own do.
    struct aligned_buffer {
        std::vector<uint64_t> words;
    };

    auto make_buffer(size_t bytes) -> aligned_buffer {
        return aligned_buffer {std::vector<uint64_t>((bytes / 8) + 2)};
    }

    auto bytes_of(aligned_buffer& buffer) -> uint8_t* {
        return reinterpret_cast<uint8_t*>(buffer.words.data());
    }

    // --- random messages ------------------------------------------------------------------------

    struct gen_state {
        std::mt19937_64 rng;
        bool fits = false;     // no unbounded_fill: lengths a TickLE capacity profile normally holds
        size_t cap = SIZE_MAX; // an upper bound on every length, lowered to shrink a sample
    };

    auto random_below(gen_state& gen, size_t bound) -> size_t {
        return bound == 0 ? 0 : static_cast<size_t>(gen.rng() % bound);
    }

    // 0, 1, 2-4 or bound-filling, a quarter each - never past an IDL bound, which check 2 covers.
    auto random_length(gen_state& gen, size_t bound, int depth) -> size_t {
        size_t length = 0;
        switch (random_below(gen, 4)) {
        case 0:
            break;
        case 1:
            length = 1;
            break;
        case 2:
            length = 2 + random_below(gen, 3);
            break;
        default:
            if (bound != 0) {
                length = bound;
            } else if (gen.fits || depth > 0) {
                length = 2 + random_below(gen, 3);
            } else {
                length = unbounded_fill;
            }
        }
        return std::min(bound != 0 ? std::min(length, bound) : length, gen.cap);
    }

    auto fill_scalar(gen_state& gen, uint8_t type_id, void* dst) -> void {
        size_t const size = scalar_size(type_id);
        if (type_id == rosidl_typesupport_introspection_c__ROS_TYPE_BOOLEAN) {
            *static_cast<bool*>(dst) = (gen.rng() & 1U) != 0;
            return;
        }
        if (type_id == rosidl_typesupport_introspection_c__ROS_TYPE_FLOAT && random_below(gen, 8) == 0) {
            static const std::array<float, 4> special = {NAN, -0.0F, INFINITY, -INFINITY};
            float chosen = special.at(random_below(gen, 4));
            std::memcpy(dst, &chosen, 4);
            return;
        }
        if (type_id == rosidl_typesupport_introspection_c__ROS_TYPE_DOUBLE && random_below(gen, 8) == 0) {
            static const std::array<double, 4> special = {static_cast<double>(NAN), -0.0, static_cast<double>(INFINITY),
                                                          -static_cast<double>(INFINITY)};
            double chosen = special.at(random_below(gen, 4));
            std::memcpy(dst, &chosen, 8);
            return;
        }
        uint64_t bits = gen.rng();
        std::memcpy(dst, &bits, size);
    }

    auto fill_text(gen_state& gen, rosidl_runtime_c__String* str, size_t bound, int depth) -> void {
        size_t const length = random_length(gen, bound, depth);
        std::string text(length, 'a');
        for (char& character: text) {
            character = static_cast<char>('a' + random_below(gen, letters));
        }
        if (length > 0 && random_below(gen, 10) == 0) {
            text[random_below(gen, length)] = '\0'; // the wire carries what comes before it
        }
        rosidl_runtime_c__String__assignn(str, text.data(), text.size());
    }

    auto fill_message(gen_state& gen, const message_members* members, void* msg, int depth) -> bool;

    // NOLINTNEXTLINE(misc-no-recursion) - a nested field is described by its own type's members
    auto fill_one(gen_state& gen, const message_member& member, void* value, int depth) -> bool {
        if (is_nested(member)) {
            return fill_message(gen, members_of(member.members_), value, depth + 1);
        }
        if (is_text(member)) {
            fill_text(gen, static_cast<rosidl_runtime_c__String*>(value), member.string_upper_bound_, depth);
            return true;
        }
        fill_scalar(gen, member.type_id_, value);
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion) - a nested field is described by its own type's members
    auto fill_member(gen_state& gen, const message_member& member, void* field, int depth) -> bool {
        if (!member.is_array_) {
            return fill_one(gen, member, field, depth);
        }
        size_t count = member.array_size_;
        if (is_sequence(member)) {
            count = random_length(gen, member.is_upper_bound_ ? member.array_size_ : 0, depth);
            if (!member.resize_function(field, count)) {
                return false;
            }
        }
        for (size_t index = 0; index < count; index++) {
            if (!fill_one(gen, member, element_at(member, field, index), depth + 1)) {
                return false;
            }
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion) - a nested field is described by its own type's members
    auto fill_message(gen_state& gen, const message_members* members, void* msg, int depth) -> bool {
        for (uint32_t i = 0; i < members->member_count_; i++) {
            const message_member& member = members->members_[i];
            if (is_placeholder(member)) {
                continue;
            }
            if (!fill_member(gen, member, static_cast<char*>(msg) + member.offset_, depth)) {
                return false;
            }
        }
        return true;
    }

    // --- comparison and bounds -------------------------------------------------------------------

    auto equal_message(const message_members* members, const void* a, const void* b) -> bool;

    // NOLINTNEXTLINE(misc-no-recursion) - a nested field is described by its own type's members
    auto equal_value(const message_member& member, const void* a, const void* b) -> bool {
        if (is_nested(member)) {
            return equal_message(members_of(member.members_), a, b);
        }
        if (is_text(member)) {
            const auto* left = static_cast<const rosidl_runtime_c__String*>(a);
            const auto* right = static_cast<const rosidl_runtime_c__String*>(b);
            size_t const length = first_nul_length(left);
            return length == first_nul_length(right) &&
                   (length == 0 || std::memcmp(left->data, right->data, length) == 0);
        }
        return std::memcmp(a, b, scalar_size(member.type_id_)) == 0; // bitwise: NaN equals itself
    }

    // NOLINTNEXTLINE(misc-no-recursion) - a nested field is described by its own type's members
    auto equal_member(const message_member& member, const void* a, const void* b) -> bool {
        if (!member.is_array_) {
            return equal_value(member, a, b);
        }
        size_t const count = count_of(member, a);
        if (count != count_of(member, b)) {
            return false;
        }
        for (size_t index = 0; index < count; index++) {
            if (!equal_value(member, element_at(member, a, index), element_at(member, b, index))) {
                return false;
            }
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion) - a nested field is described by its own type's members
    auto equal_message(const message_members* members, const void* a, const void* b) -> bool {
        for (uint32_t i = 0; i < members->member_count_; i++) {
            const message_member& member = members->members_[i];
            if (is_placeholder(member)) {
                continue;
            }
            if (!equal_member(member, static_cast<const char*>(a) + member.offset_,
                              static_cast<const char*>(b) + member.offset_)) {
                return false;
            }
        }
        return true;
    }

    // Within every IDL bound and the wire's own limits - what the direct codec alone checks.
    auto within_idl(const message_members* members, const void* msg) -> bool;

    auto text_within(const message_member& member, const void* value) -> bool {
        const auto* str = static_cast<const rosidl_runtime_c__String*>(value);
        if (member.string_upper_bound_ != 0 && str->size > member.string_upper_bound_) {
            return false;
        }
        return first_nul_length(str) + 1 <= max_count;
    }

    // NOLINTNEXTLINE(misc-no-recursion) - a nested field is described by its own type's members
    auto member_within_idl(const message_member& member, const void* field) -> bool {
        size_t const count = member.is_array_ ? count_of(member, field) : 1;
        if (is_sequence(member) && (count > max_count || (member.is_upper_bound_ && count > member.array_size_))) {
            return false;
        }
        for (size_t index = 0; index < count; index++) {
            const void* value = member.is_array_ ? element_at(member, field, index) : field;
            if (is_text(member) && !text_within(member, value)) {
                return false;
            }
            if (is_nested(member) && !within_idl(members_of(member.members_), value)) {
                return false;
            }
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion) - a nested field is described by its own type's members
    auto within_idl(const message_members* members, const void* msg) -> bool {
        for (uint32_t i = 0; i < members->member_count_; i++) {
            const message_member& member = members->members_[i];
            if (is_placeholder(member)) {
                continue;
            }
            if (!member_within_idl(member, static_cast<const char*>(msg) + member.offset_)) {
                return false;
            }
        }
        return true;
    }

    // --- the two paths ---------------------------------------------------------------------------

    enum old_outcome : uint8_t { OLD_ACCEPTED, OLD_CAPACITY, OLD_REFUSED };

    struct old_encoding {
        old_outcome outcome;
        int32_t size;
    };

    struct counts {
        int identical = 0;
        int capacity_only = 0;
        int both_refused = 0;
        int regenerated = 0;
        int shrunk = 0;
        int overbound = 0;
        int decode_inputs = 0;
        int decode_capacity_only = 0;
        int failures = 0;
    };

    // Everything one type's checks work on. Plain data: the checks are the free functions below.
    struct checker {
        std::string name; // pkg/subfolder/Type
        const message_members* members;
        const tickle_callbacks* callbacks;
        int samples;
        gen_state gen;
        aligned_buffer tickle_storage; // the TickLE struct the old path fills
        aligned_buffer out_old;
        aligned_buffer out_new;
        aligned_buffer scratch;
        counts tally;
    };

    auto tickle_data(checker& check) -> tt_Data* {
        return reinterpret_cast<tt_Data*>(bytes_of(check.tickle_storage));
    }

    // Whether to_tickle accepts: false is exactly "some TickLE capacity is exceeded".
    auto fits_struct(checker& check, const void* msg) -> bool {
        std::ranges::fill(check.tickle_storage.words, 0);
        return check.callbacks->to_tickle(msg, bytes_of(check.tickle_storage));
    }

    auto old_encode(checker& check, const void* msg) -> old_encoding {
        if (!fits_struct(check, msg)) {
            return {.outcome = OLD_CAPACITY, .size = -1};
        }
        int32_t const size = check.callbacks->tickle_encode(tickle_data(check), bytes_of(check.out_old), buffer_length);
        return {.outcome = size >= 0 ? OLD_ACCEPTED : OLD_REFUSED, .size = size};
    }

    auto old_decode(checker& check, uint32_t len, bool is_native_endian, void* out) -> int32_t {
        std::ranges::fill(check.tickle_storage.words, 0);
        int32_t const size =
            check.callbacks->tickle_decode(tickle_data(check), bytes_of(check.scratch), len, is_native_endian);
        if (size < 0) {
            return size;
        }
        bool const converted = check.callbacks->from_tickle(bytes_of(check.tickle_storage), out);
        check.callbacks->tickle_free(tickle_data(check));
        return converted ? size : from_tickle_failed;
    }

    // --- the checks -------------------------------------------------------------------------------

    auto report(checker& check, const char* which, int sample, const std::string& detail) -> void {
        check.tally.failures++;
        if (check.tally.failures <= reported_failures) {
            std::printf("FAIL %s check %s sample %d: %s\n", check.name.c_str(), which, sample, detail.c_str());
        }
    }

    auto first_difference(checker& check, int32_t a, int32_t b) -> std::string {
        int32_t const shorter = std::min(a, b);
        for (int32_t index = 0; index < shorter; index++) {
            if (bytes_of(check.out_old)[index] != bytes_of(check.out_new)[index]) {
                return ", first difference at byte " + std::to_string(index);
            }
        }
        return "";
    }

    // 3 and 4: both decoders on the same bytes, in both byte orders. They must accept or refuse
    // alike, and agree on what they accept. The one allowed disagreement is the capacity-only
    // class: the old path refuses a message the ROS type itself permits.
    auto decode_pair(checker& check, const std::vector<uint8_t>& input, bool is_native_endian, int sample,
                     const Message& shell) -> void {
        check.tally.decode_inputs++;
        std::ranges::copy(input, bytes_of(check.scratch));
        auto len = static_cast<uint32_t>(input.size());
        Message const old_msg(check.members);
        int32_t const old_size = old_decode(check, len, is_native_endian, old_msg.get());
        int32_t const new_size =
            check.callbacks->direct_decode(shell.get(), bytes_of(check.scratch), len, is_native_endian);
        const char* which = is_native_endian ? "3" : "4";
        std::string const where =
            std::string(is_native_endian ? "native" : "foreign") + ", " + std::to_string(len) + " B";
        if (old_size >= 0 && new_size >= 0) {
            if (old_size != new_size || !equal_message(check.members, old_msg.get(), shell.get())) {
                report(check, which, sample, "decoders disagree on what they accept (" + where + ")");
            }
            return;
        }
        if (old_size >= 0) {
            report(check, which, sample,
                   "old accepts, new refuses with " + std::to_string(new_size) + " (" + where + ")");
            return;
        }
        if (new_size < 0) {
            return;
        }
        if (old_size == -2 && !fits_struct(check, shell.get()) && within_idl(check.members, shell.get())) {
            check.tally.decode_capacity_only++;
            return;
        }
        report(check, which, sample, "old refuses with " + std::to_string(old_size) + ", new accepts (" + where + ")");
    }

    auto damaged_copies(checker& check, const uint8_t* payload, uint32_t len) -> std::vector<std::vector<uint8_t>> {
        std::vector<std::vector<uint8_t>> inputs;
        inputs.emplace_back(payload, payload + len);
        if (len <= small_message) {
            for (uint32_t index = 0; index < len; index++) {
                inputs.emplace_back(payload, payload + index);
            }
        } else {
            for (size_t k = 0; k < random_truncations; k++) {
                inputs.emplace_back(payload, payload + random_below(check.gen, len));
            }
        }
        for (size_t k = 0; k < bit_flips && len > 0; k++) {
            std::vector<uint8_t> flipped(payload, payload + len);
            size_t const bit = random_below(check.gen, static_cast<size_t>(len) * 8);
            flipped[bit / 8] ^= static_cast<uint8_t>(1U << (bit % 8));
            inputs.push_back(std::move(flipped));
        }
        return inputs;
    }

    auto differential(checker& check, uint32_t len, int sample, const Message& shell) -> void {
        for (const auto& input: damaged_copies(check, bytes_of(check.out_new), len)) {
            decode_pair(check, input, true, sample, shell);
            decode_pair(check, input, false, sample, shell);
        }
    }

    // 1b: the bytes the new path produced decode back to the message they came from, into a shell
    // that still holds the previous sample.
    auto decodes_back(checker& check, const void* msg, int32_t size, int sample, const Message& shell) -> bool {
        int32_t const back =
            check.callbacks->direct_decode(shell.get(), bytes_of(check.out_new), static_cast<uint32_t>(size), true);
        if (back == size && equal_message(check.members, msg, shell.get())) {
            return true;
        }
        report(check, "1b", sample,
               "direct_decode returned " + std::to_string(back) + " of " + std::to_string(size) +
                   (back == size ? ", message differs" : ""));
        return false;
    }

    // 1: the two encoders on one message. Returns false when they disagreed.
    auto encodings_agree(checker& check, void* msg, int32_t new_size, int sample) -> bool {
        int32_t const sized = check.callbacks->direct_encode_size(msg);
        if (new_size >= 0 && sized != new_size) {
            report(check, "1", sample,
                   "direct_encode_size " + std::to_string(sized) + " != direct_encode " + std::to_string(new_size));
        }
        old_encoding const old = old_encode(check, msg);
        if (old.outcome == OLD_ACCEPTED) {
            if (new_size != old.size ||
                std::memcmp(bytes_of(check.out_old), bytes_of(check.out_new), static_cast<size_t>(old.size)) != 0) {
                report(check, "1", sample,
                       "bytes differ: old " + std::to_string(old.size) + " B, new " + std::to_string(new_size) + " B" +
                           first_difference(check, old.size, new_size));
                return false;
            }
            check.tally.identical++;
            return true;
        }
        if (new_size < 0) {
            check.tally.both_refused++;
            return true;
        }
        if (old.outcome != OLD_CAPACITY) {
            report(check, "1", sample, "old encode refuses (" + std::to_string(old.size) + "), new accepts");
            return false;
        }
        if (!within_idl(check.members, msg)) {
            report(check, "1", sample, "new accepts a message outside its IDL bounds");
            return false;
        }
        check.tally.capacity_only++;
        return true;
    }

    auto check_sample(checker& check, void* msg, int32_t new_size, int sample, const Message& shell) -> void {
        if (!encodings_agree(check, msg, new_size, sample) || new_size < 0) {
            return;
        }
        if (decodes_back(check, msg, new_size, sample, shell)) {
            differential(check, static_cast<uint32_t>(new_size), sample, shell);
        }
    }

    // One sample. Every fourth fills unbounded fields to unbounded_fill, which a TickLE capacity
    // normally refuses - the capacity-only class. The other three aim to be byte-compared, so when
    // the old path refuses one for a capacity, it is regenerated shorter and shorter rather than
    // counted as capacity-only: whether a type gets compared at all must not depend on how small
    // its profile capacities happen to be. A sample too large for one datagram is likewise
    // regenerated (a whole message must fit, which stage 1 does not change).
    auto one_sample(checker& check, int sample, const Message& shell) -> void {
        static const std::array<size_t, 4> caps = {SIZE_MAX, 2, 1, 0};
        const bool free_sample = (sample % 4) == 0;
        for (size_t attempt = 0; attempt < caps.size(); attempt++) {
            const bool last = attempt + 1 == caps.size();
            Message const msg(check.members);
            check.gen.fits = !free_sample || attempt > 0;
            check.gen.cap = caps.at(attempt);
            fill_message(check.gen, check.members, msg.get(), 0);
            int32_t const new_size = check.callbacks->direct_encode(msg.get(), bytes_of(check.out_new), buffer_length);
            if (!last && new_size == -1) {
                check.tally.regenerated++;
                continue;
            }
            if (!last && !free_sample && !fits_struct(check, msg.get())) {
                check.tally.shrunk++;
                continue;
            }
            check_sample(check, msg.get(), new_size, sample, shell);
            return;
        }
    }

    // Plants over-bound input `kind` in the first field that can take it, depth first through
    // nested messages and the first element of fixed arrays of them.
    // NOLINTNEXTLINE(misc-no-recursion) - a nested field is described by its own type's members
    auto plant(const message_members* members, void* msg, int kind) -> bool {
        for (uint32_t i = 0; i < members->member_count_; i++) {
            const message_member& member = members->members_[i];
            void* field = static_cast<char*>(msg) + member.offset_;
            if (kind == 0 && is_sequence(member) && member.is_upper_bound_) {
                return member.resize_function(field, member.array_size_ + 1);
            }
            if (kind == 2 && is_sequence(member) && !member.is_upper_bound_) {
                return member.resize_function(field, max_count + 1);
            }
            if (!member.is_array_ && is_text(member)) {
                auto* str = static_cast<rosidl_runtime_c__String*>(field);
                if (kind == 1 && member.string_upper_bound_ != 0) {
                    std::string const text(member.string_upper_bound_ + 1, 'x');
                    return rosidl_runtime_c__String__assign(str, text.c_str());
                }
                if (kind == 3 && member.string_upper_bound_ == 0) {
                    std::string const text(max_count, 'y');
                    return rosidl_runtime_c__String__assign(str, text.c_str());
                }
            }
            if (is_nested(member) && !is_sequence(member) &&
                plant(members_of(member.members_), member.is_array_ ? element_at(member, field, 0) : field, kind)) {
                return true;
            }
        }
        return false;
    }

    // 2: a bounded sequence at bound+1, a bounded string at bound+1, a count of 65,536 and a string
    // of 65,535 characters, one each where the type has such a field. Both paths must refuse.
    auto overbound(checker& check) -> void {
        static const std::array<const char*, 4> names = {"bounded sequence +1", "bounded string +1", "count 65536",
                                                         "string of 65535"};
        for (size_t kind = 0; kind < names.size(); kind++) {
            Message const msg(check.members);
            check.gen.fits = true;
            check.gen.cap = 1;
            fill_message(check.gen, check.members, msg.get(), 1);
            if (!plant(check.members, msg.get(), static_cast<int>(kind))) {
                continue;
            }
            check.tally.overbound++;
            old_encoding const old = old_encode(check, msg.get());
            int32_t const size = check.callbacks->direct_encode(msg.get(), bytes_of(check.out_new), buffer_length);
            int32_t const sized = check.callbacks->direct_encode_size(msg.get());
            if (old.outcome == OLD_ACCEPTED || size >= 0 || sized >= 0) {
                report(check, "2", -1,
                       std::string(names.at(kind)) + ": old " + (old.outcome == OLD_ACCEPTED ? "accepts" : "refuses") +
                           ", new encode " + std::to_string(size) + ", size " + std::to_string(sized));
            }
        }
    }

    auto run_type(const std::string& name, const message_members* members, const tickle_callbacks* callbacks,
                  uint64_t seed, int samples) -> counts {
        checker check {.name = name,
                       .members = members,
                       .callbacks = callbacks,
                       .samples = samples,
                       .gen = gen_state {},
                       .tickle_storage = make_buffer(callbacks->tickle_struct_size),
                       .out_old = make_buffer(buffer_length),
                       .out_new = make_buffer(buffer_length),
                       .scratch = make_buffer(buffer_length),
                       .tally = counts {}};
        check.gen.rng.seed(seed);
        Message const shell(members);
        for (int sample = 0; sample < samples; sample++) {
            one_sample(check, sample, shell);
        }
        overbound(check);
        return check.tally;
    }

    // --- finding types ---------------------------------------------------------------------------

    auto split(const std::string& text, char separator) -> std::vector<std::string> {
        std::vector<std::string> parts;
        std::stringstream stream(text);
        std::string item;
        while (std::getline(stream, item, separator)) {
            if (!item.empty()) {
                parts.push_back(item);
            }
        }
        return parts;
    }

    auto prefix_path() -> std::vector<std::string> {
        const char* value = std::getenv("AMENT_PREFIX_PATH");
        return split(value != nullptr ? value : "", ':');
    }

    // The messages one .idl line declares: a .msg is itself, a .srv two, an .action eight.
    auto messages_of(const std::string& subfolder, const std::string& name) -> std::vector<std::string> {
        if (subfolder == "msg") {
            return {name};
        }
        if (subfolder == "srv") {
            return {name + "_Request", name + "_Response"};
        }
        if (subfolder == "action") {
            return {name + "_Goal",
                    name + "_Result",
                    name + "_Feedback",
                    name + "_FeedbackMessage",
                    name + "_SendGoal_Request",
                    name + "_SendGoal_Response",
                    name + "_GetResult_Request",
                    name + "_GetResult_Response"};
        }
        return {};
    }

    // Every interface message of a package, from its ament index entry: "pkg/subfolder/Type".
    auto interface_messages(const std::string& package) -> std::vector<std::string> {
        std::vector<std::string> types;
        for (const std::string& prefix: prefix_path()) {
            std::string path = prefix;
            path.append("/share/ament_index/resource_index/rosidl_interfaces/").append(package);
            std::ifstream index(path);
            if (!index) {
                continue;
            }
            std::set<std::string> seen;
            std::string line;
            while (std::getline(index, line)) {
                size_t const slash = line.find('/');
                size_t const dot = line.rfind('.');
                if (slash == std::string::npos || dot == std::string::npos || line.substr(dot) != ".idl") {
                    continue;
                }
                std::string const subfolder = line.substr(0, slash);
                std::string const name = line.substr(slash + 1, dot - slash - 1);
                for (const auto& message: messages_of(subfolder, name)) {
                    std::string key = subfolder;
                    key.append("/").append(message);
                    if (seen.insert(key).second) {
                        std::string type = package;
                        type.append("/").append(key);
                        types.push_back(type);
                    }
                }
            }
            break;
        }
        return types;
    }

    // Packages that built rosidl_typesupport_tickle_c: those with its library in their prefix.
    auto tickle_packages() -> std::vector<std::string> {
        std::set<std::string> packages;
        for (const std::string& prefix: prefix_path()) {
            std::string const dir = prefix + "/share/ament_index/resource_index/rosidl_interfaces";
            DIR* handle = opendir(dir.c_str());
            if (handle == nullptr) {
                continue;
            }
            while (const dirent* entry = readdir(handle)) {
                std::string package = entry->d_name;
                struct stat info {};
                std::string library = prefix;
                library.append("/lib/lib").append(package).append("__rosidl_typesupport_tickle_c.so");
                if (package[0] != '.' && stat(library.c_str(), &info) == 0) {
                    packages.insert(package);
                }
            }
            closedir(handle);
        }
        return {packages.begin(), packages.end()};
    }

    auto open_library(const std::string& package, const char* typesupport) -> void* {
        std::string name = "lib";
        name.append(package).append("__").append(typesupport).append(".so");
        void* handle = dlopen(name.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (handle == nullptr) {
            std::printf("ERROR %s: %s\n", name.c_str(), dlerror());
        }
        return handle;
    }

    using handle_function = const rosidl_message_type_support_t* (*)();

    auto type_handle(void* library, const char* typesupport, const std::string& type)
        -> const rosidl_message_type_support_t* {
        std::vector<std::string> parts = split(type, '/');
        std::string symbol = typesupport;
        symbol.append("__get_message_type_support_handle__")
            .append(parts[0])
            .append("__")
            .append(parts[1])
            .append("__")
            .append(parts[2]);
        auto function = reinterpret_cast<handle_function>(dlsym(library, symbol.c_str()));
        return function != nullptr ? function() : nullptr;
    }

    auto read_list(const std::string& path) -> std::set<std::string> {
        std::set<std::string> lines;
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line[0] != '#') {
                lines.insert(line);
            }
        }
        return lines;
    }

    struct options {
        uint64_t seed;
        int samples = default_samples;
        std::string declined_path;
        std::vector<std::string> packages;
        bool all = false;
    };

    auto parse(int argc, char** argv) -> options {
        options opts {.seed = std::random_device {}()};
        for (int i = 1; i < argc; i++) {
            std::string const arg = argv[i];
            if (arg == "-s" && i + 1 < argc) {
                opts.seed = std::strtoull(argv[++i], nullptr, 10);
            } else if (arg == "-n" && i + 1 < argc) {
                opts.samples = std::atoi(argv[++i]);
            } else if (arg == "-d" && i + 1 < argc) {
                opts.declined_path = argv[++i];
            } else if (arg == "--all") {
                opts.all = true;
            } else {
                opts.packages.push_back(arg);
            }
        }
        if (opts.all) {
            opts.packages = tickle_packages();
        }
        return opts;
    }

    struct run_state {
        std::set<std::string> declined;
        int types = 0;
        int failed = 0;
    };

    auto check_one_type(const std::string& type, const rosidl_message_type_support_t* tickle,
                        const rosidl_message_type_support_t* introspection, const options& opts, run_state& state)
        -> void {
        if (introspection == nullptr) {
            std::printf("FAIL %s: no introspection handle\n", type.c_str());
            state.failed++;
            return;
        }
        const auto* callbacks = static_cast<const tickle_callbacks*>(tickle->data);
        if (callbacks->struct_size != sizeof(tickle_callbacks) || callbacks->direct_encode == nullptr) {
            std::printf("FAIL %s: no direct codec in its callbacks (rebuild it)\n", type.c_str());
            state.failed++;
            return;
        }
        state.types++;
        uint64_t const seed = opts.seed ^ std::hash<std::string> {}(type);
        counts const tally = run_type(type, members_of(introspection), callbacks, seed, opts.samples);
        bool const below_floor = tally.identical < opts.samples / 2;
        std::printf("%s %s identical=%d capacity_only=%d both_refused=%d regenerated=%d shrunk=%d overbound=%d "
                    "decode_inputs=%d decode_capacity_only=%d failures=%d seed=%" PRIu64 "\n",
                    (tally.failures > 0 || below_floor) ? "FAIL" : "ok", type.c_str(), tally.identical,
                    tally.capacity_only, tally.both_refused, tally.regenerated, tally.shrunk, tally.overbound,
                    tally.decode_inputs, tally.decode_capacity_only, tally.failures, seed);
        if (below_floor) {
            std::printf("FAIL %s: %d of %d samples byte-compared, below the floor of %d\n", type.c_str(),
                        tally.identical, opts.samples, opts.samples / 2);
        }
        if (tally.failures > 0 || below_floor) {
            state.failed++;
        }
    }

    auto check_package(const std::string& package, const options& opts, run_state& state) -> void {
        std::vector<std::string> const types = interface_messages(package);
        void* tickle_library = open_library(package, "rosidl_typesupport_tickle_c");
        void* introspection_library = open_library(package, "rosidl_typesupport_introspection_c");
        if (tickle_library == nullptr || introspection_library == nullptr || types.empty()) {
            std::printf("FAIL %s: no libraries or no interfaces\n", package.c_str());
            state.failed++;
            return;
        }
        for (const std::string& type: types) {
            const rosidl_message_type_support_t* tickle =
                type_handle(tickle_library, "rosidl_typesupport_tickle_c", type);
            if (tickle == nullptr) {
                state.declined.insert(type);
                continue;
            }
            check_one_type(type, tickle, type_handle(introspection_library, "rosidl_typesupport_introspection_c", type),
                           opts, state);
        }
    }

    // The declined list is checked both ways: a type this build declines and the list does not
    // name, and a type the list names and this build supports, are both failures.
    auto check_declined(const options& opts, run_state& state) -> void {
        if (opts.declined_path.empty()) {
            for (const auto& type: state.declined) {
                std::printf("declined %s\n", type.c_str());
            }
            return;
        }
        std::set<std::string> const expected = read_list(opts.declined_path);
        for (const auto& type: state.declined) {
            if (!expected.contains(type)) {
                std::printf("FAIL %s: declined, but not on the declined list\n", type.c_str());
                state.failed++;
            }
        }
        for (const auto& type: expected) {
            std::string const package = split(type, '/')[0];
            bool const checked = std::ranges::find(opts.packages, package) != opts.packages.end();
            if (checked && !state.declined.contains(type)) {
                std::printf("FAIL %s: on the declined list, but supported\n", type.c_str());
                state.failed++;
            }
        }
    }

} // namespace

auto main(int argc, char** argv) -> int {
    options const opts = parse(argc, argv);
    std::printf("direct_codec_identity: seed %" PRIu64 ", %d samples per type, %zu package(s)\n", opts.seed,
                opts.samples, opts.packages.size());
    run_state state;
    for (const std::string& package: opts.packages) {
        check_package(package, opts, state);
    }
    check_declined(opts, state);
    std::printf("direct_codec_identity: %d type(s) checked, %zu declined, %d failed, seed %" PRIu64 "\n", state.types,
                state.declined.size(), state.failed, opts.seed);
    return state.failed == 0 && state.types > 0 ? 0 : 1;
}
