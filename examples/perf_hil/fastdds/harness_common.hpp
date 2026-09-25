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
#include <cstddef>
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

    // Sleeps `seconds` split exactly as the throughput clients' -i pacing and the stalling servers' pause
    // always split it: whole seconds, then the fraction in nanoseconds - not seconds_to_ns(), whose rounding
    // could differ from it by a nanosecond.
    inline void sleep_seconds(double seconds) {
        const auto whole_s = static_cast<time_t>(seconds);
        const struct timespec ts = {whole_s,
                                    static_cast<long>((seconds - static_cast<double>(whole_s)) * ns_per_s_real)};
        nanosleep(&ts, nullptr);
    }

    constexpr long match_poll_ns = 50L * 1000L * 1000L; // 50ms

    // The match-wait every scenario that cannot rely on a blind discovery sleep uses: checks `matched`
    // every 50ms until it holds (true) or `timeout_s` has passed (false). See best_effort_throughput/
    // client.cpp for why a blind sleep was not enough.
    template <typename Pred> inline auto wait_until(double timeout_s, Pred matched) -> bool {
        const uint64_t start = now_ns();
        for (;;) {
            if (matched()) {
                return true;
            }
            const double elapsed = static_cast<double>(now_ns() - start) / ns_per_s_real;
            if (elapsed >= timeout_s) {
                return false;
            }
            const struct timespec poll_interval = {0, match_poll_ns};
            nanosleep(&poll_interval, nullptr);
        }
    }

    // Megabits per second of `samples` samples of `sample_size` bytes. The throughput harnesses have always
    // passed sizeof(Bench) here, not BENCH_SAMPLE_BYTES; kept so their numbers stay comparable with every
    // earlier run.
    inline auto mbps(uint64_t samples, size_t sample_size, double elapsed_s) -> double {
        return elapsed_s > 0.0 ? ((static_cast<double>(samples) * static_cast<double>(sample_size) * bits_per_byte) /
                                  bits_per_megabit / elapsed_s)
                               : 0.0;
    }

    // What a throughput server counts: every seq gap is loss, relative to the previous sample taken.
    struct stream_stats {
        uint64_t received = 0;
        uint64_t lost = 0;
        uint32_t last_seq = 0;
        bool first = true;
        uint64_t first_recv_ns = 0;
        uint64_t last_recv_ns = 0;
    };

    inline void count_sample(stream_stats& stats, uint32_t seq) {
        if (stats.first) {
            stats.first = false;
            stats.first_recv_ns = now_ns();
        } else if (seq > stats.last_seq + 1) {
            stats.lost += (seq - stats.last_seq - 1);
        }
        stats.last_seq = seq;
        stats.last_recv_ns = now_ns();
        stats.received++;
    }

    inline auto stream_elapsed_s(const stream_stats& stats) -> double {
        return stats.received > 0 ? static_cast<double>(stats.last_recv_ns - stats.first_recv_ns) / ns_per_s_real : 0.0;
    }

    // What the two stalling servers (history_depth_burst_loss, lifespan_expiry) count. Unlike stream_stats,
    // last_seq starts at 0 rather than at the first sample's own seq, so samples lost before the first one
    // that arrives count too - see history_depth_burst_loss/server.cpp for the bug that choice fixed.
    struct seq_gaps {
        uint64_t received = 0;
        uint64_t lost = 0;
        uint32_t last_seq = 0;
    };

    inline void count_seq(seq_gaps& gaps, uint32_t seq) {
        if (seq > gaps.last_seq + 1) {
            gaps.lost += (seq - gaps.last_seq - 1);
        }
        gaps.last_seq = seq;
        gaps.received++;
    }

    inline auto seq_gaps_loss_pct(const seq_gaps& gaps) -> double {
        const uint64_t total = gaps.received + gaps.lost;
        return total > 0 ? (percent * static_cast<double>(gaps.lost) / static_cast<double>(total)) : 0.0;
    }

    inline auto stream_loss_pct(const stream_stats& stats) -> double {
        const uint64_t total = stats.received + stats.lost;
        return total > 0 ? (percent * static_cast<double>(stats.lost) / static_cast<double>(total)) : 0.0;
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
