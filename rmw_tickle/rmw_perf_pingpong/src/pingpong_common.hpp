// Shared by ping_node.cpp and pong_node.cpp: the clock both stamp with, per-message-type field access, and
// the per-sample stamp file (--stamps) both can write.
#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>

#include "rmw_perf_pingpong/msg/array1k.hpp"
#include "rmw_perf_pingpong/msg/bench.hpp"
#include "rmw_perf_pingpong/msg/struct16.hpp"

namespace pingpong {

    constexpr uint64_t stamp_ns_per_s = 1000000000ULL;

    // CLOCK_MONOTONIC, in ns - what send_ns in the message and every --stamps value are.
    inline auto now_ns() -> uint64_t {
        struct timespec ts;
        // clock_gettime()/CLOCK_MONOTONIC are declared through a private glibc header reached
        // transitively via <ctime> (misc-include-cleaner attributes them there instead of to
        // <ctime> itself) - same class of system-header quirk as rmw_tickle.h's own pthread.h
        // NOLINT.
        clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner)
        return (static_cast<uint64_t>(ts.tv_sec) * stamp_ns_per_s) + static_cast<uint64_t>(ts.tv_nsec);
    }

    // CLOCK_REALTIME - CLOCK_MONOTONIC, in ns: what turns a stamp here into the clock a packet capture or
    // a syscall trace (CLOCK_REALTIME) uses.
    inline auto realtime_offset_ns() -> int64_t {
        struct timespec real;
        struct timespec mono;
        clock_gettime(CLOCK_MONOTONIC, &mono); // NOLINT(misc-include-cleaner)
        clock_gettime(CLOCK_REALTIME, &real);  // NOLINT(misc-include-cleaner)
        const auto real_ns = (static_cast<int64_t>(real.tv_sec) * static_cast<int64_t>(stamp_ns_per_s)) + real.tv_nsec;
        const auto mono_ns = (static_cast<int64_t>(mono.tv_sec) * static_cast<int64_t>(stamp_ns_per_s)) + mono.tv_nsec;
        return real_ns - mono_ns;
    }

    // --stamps <file> (2026-09-26, RMW_PERF_PLAN §8): one line per sample, CLOCK_MONOTONIC ns, collected in
    // memory while the run goes and written at the end, so no file I/O lands on the path being timed.
    //   ping:  seq send_ns reply_ns     (reply_ns: the reply reaching the ping's callback, 0 if none came)
    //   pong:  seq callback_ns publish_return_ns
    // The file opens with the columns and the REALTIME - MONOTONIC offset read at the start, and closes
    // with it read again at the end.
    struct stamp_row {
        uint64_t seq;
        uint64_t first_ns;
        uint64_t second_ns;
    };

    struct stamp_log {
        const char* path = nullptr; // nullptr: --stamps not given, nothing recorded or written
        const char* columns = "";
        int64_t offset_start_ns = 0;
        std::vector<stamp_row> rows;
    };

    inline auto stamp_log_open(stamp_log& log, const char* path, const char* columns) -> void {
        log.path = path;
        log.columns = columns;
        if (path != nullptr) {
            constexpr size_t expected_rows = 1U << 16U;
            log.rows.reserve(expected_rows);
            log.offset_start_ns = realtime_offset_ns();
        }
    }

    inline auto stamp_log_add(stamp_log& log, uint64_t seq, uint64_t begin_ns, uint64_t end_ns) -> void {
        if (log.path != nullptr) {
            log.rows.push_back({seq, begin_ns, end_ns});
        }
    }

    // Returns false when the file could not be written; the caller reports it.
    inline auto stamp_log_write(const stamp_log& log) -> bool {
        if (log.path == nullptr) {
            return true;
        }
        FILE* out = std::fopen(log.path, "w");
        if (out == nullptr) {
            return false;
        }
        std::fprintf(out, "# %s\n# realtime_minus_monotonic_ns_start %lld\n", log.columns,
                     static_cast<long long>(log.offset_start_ns));
        for (const stamp_row& row: log.rows) {
            std::fprintf(out, "%llu %llu %llu\n", static_cast<unsigned long long>(row.seq),
                         static_cast<unsigned long long>(row.first_ns), static_cast<unsigned long long>(row.second_ns));
        }
        std::fprintf(out, "# realtime_minus_monotonic_ns_end %lld\n", static_cast<long long>(realtime_offset_ns()));
        return std::fclose(out) == 0;
    }

    // Per-message-type field access: Bench's own (seq, send_ns) versus Array1k's/Struct16's own
    // (id, time) - same role, different names/types (performance_test's own convention, matched
    // verbatim - see msg/Array1k.msg's own header comment), unified here so run_ping() below is
    // written once against BenchTraits<T> instead of three times against three field names.
    template <typename T> struct BenchTraits;

    template <> struct BenchTraits<rmw_perf_pingpong::msg::Bench> {
        static auto set_seq(rmw_perf_pingpong::msg::Bench& msg, uint64_t val) -> void {
            msg.seq = static_cast<uint32_t>(val);
        }
        static auto seq(const rmw_perf_pingpong::msg::Bench& msg) -> uint64_t {
            return msg.seq;
        }
        static auto set_send_ns(rmw_perf_pingpong::msg::Bench& msg, uint64_t val) -> void {
            msg.send_ns = val;
        }
        static auto send_ns(const rmw_perf_pingpong::msg::Bench& msg) -> uint64_t {
            return msg.send_ns;
        }
    };

    template <> struct BenchTraits<rmw_perf_pingpong::msg::Array1k> {
        static auto set_seq(rmw_perf_pingpong::msg::Array1k& msg, uint64_t val) -> void {
            msg.id = val;
        }
        static auto seq(const rmw_perf_pingpong::msg::Array1k& msg) -> uint64_t {
            return msg.id;
        }
        static auto set_send_ns(rmw_perf_pingpong::msg::Array1k& msg, uint64_t val) -> void {
            msg.time = static_cast<int64_t>(val);
        }
        static auto send_ns(const rmw_perf_pingpong::msg::Array1k& msg) -> uint64_t {
            return static_cast<uint64_t>(msg.time);
        }
    };

    template <> struct BenchTraits<rmw_perf_pingpong::msg::Struct16> {
        static auto set_seq(rmw_perf_pingpong::msg::Struct16& msg, uint64_t val) -> void {
            msg.id = val;
        }
        static auto seq(const rmw_perf_pingpong::msg::Struct16& msg) -> uint64_t {
            return msg.id;
        }
        static auto set_send_ns(rmw_perf_pingpong::msg::Struct16& msg, uint64_t val) -> void {
            msg.time = static_cast<int64_t>(val);
        }
        static auto send_ns(const rmw_perf_pingpong::msg::Struct16& msg) -> uint64_t {
            return static_cast<uint64_t>(msg.time);
        }
    };

} // namespace pingpong
