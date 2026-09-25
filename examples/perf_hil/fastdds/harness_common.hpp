/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// What every FastDDS harness client and server shares (2026-09-25): the SIGINT flag, a monotonic
// clock, a sleep, the unit constants, and the storage for the shared instrumentation.
//
// It exists because the FastDDS harness was brought up to the repository's own C++ clang-tidy checks
// that day - the checks the rmw_tickle C++ code already meets - and each of the eighteen files had its
// own copy of these few helpers, each tripping the same findings. One copy, written once to the checks,
// is less to get wrong than eighteen. Behaviour is exactly what those copies did.
//
// Every program here is one translation unit, so `inline` variables are simply the program's globals.

#pragma once

#include <array>
#include <csignal>
#include <cstdint>
#include <time.h> // NOLINT(modernize-deprecated-headers) - clock_gettime()/nanosleep() are POSIX, not in <ctime>

#include "../tickle/common/BenchStats.h"

namespace harness {

    constexpr uint64_t ns_per_s = 1000000000ULL;
    constexpr double ns_per_s_real = 1e9;
    constexpr double ns_per_ms = 1e6;
    constexpr double percent = 100.0;
    constexpr double bits_per_byte = 8.0;
    constexpr double bits_per_megabit = 1e6;

    inline volatile sig_atomic_t g_interrupted = 0;

    inline void handle_sigint(int signum) {
        (void)signum;
        g_interrupted = 1;
    }

    inline void install_sigint_handler() {
        std::signal(SIGINT, handle_sigint);
    }

    inline auto interrupted() -> bool {
        return g_interrupted != 0;
    }

    inline auto now_ns() -> uint64_t {
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - glibc defines it in bits/time.h
        return (static_cast<uint64_t>(ts.tv_sec) * ns_per_s) + static_cast<uint64_t>(ts.tv_nsec);
    }

    inline void sleep_ns(uint64_t duration_ns) {
        const struct timespec ts = {static_cast<time_t>(duration_ns / ns_per_s),
                                    static_cast<long>(duration_ns % ns_per_s)};
        nanosleep(&ts, nullptr);
    }

    inline auto seconds_to_ns(double seconds) -> uint64_t {
        return static_cast<uint64_t>(seconds * ns_per_s_real);
    }

    // The shared instrumentation's storage (BenchStats.h). Armed at the very top of each main(), before
    // any middleware setup, so the counters cover discovery too - identically for all three frameworks.
    inline struct BenchStats g_bench_stats;
    inline std::array<char, BENCH_STATS_FIELDS_MAX> g_bench_fields {};

    inline auto bench_fields(int role, uint64_t samples) -> const char* {
        return bench_stats_fields(&g_bench_stats, role, samples, BENCH_SAMPLE_BYTES, g_bench_fields.data(),
                                  g_bench_fields.size());
    }

} // namespace harness
