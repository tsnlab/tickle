// The L4 round trip, sender side (examples/perf_hil/experiments/l4_rtt_rig.sh; docs/RESULTS.md "rmw large messages").
//
// The user's decisions (2026-10-19): cross-host latency is measured by round trip only, never one way over two clocks,
// and for a large message the reply is a small message. So this node publishes the large message (-m array1m|array4m)
// at a fixed rate on "l4_large", large_rtt_rx answers each sample it takes with an Ack (the id alone, 8 bytes) on
// "l4_ack", RELIABLE KEEP_LAST 10, through the same rmw, and the round trip is
//
//     reply_ns - due_ns,   both read here, on this process's CLOCK_MONOTONIC
//
// where due_ns is the sample's creation time: the time its publish was scheduled for (start + k / rate), kept here and
// also carried in the message's `time` field. A publish that starts late (a timer wake, or a DDS publish of the sample
// before that blocked past its slot) counts against the framework, as in the L4 creation-time cells. -m ack sends the
// Ack itself as the ping: the control, a small ping and a small reply on the same rmw, link and code.
//
// Only rclcpp and rmw public API, so every RMW_IMPLEMENTATION runs this same binary. One thread publishes on the
// schedule; the executor runs on another and stamps each reply as its callback starts.
//
// Output: a RESULT line (identity, counts, percentiles - for a person; the reader recomputes everything from the
// samples) and, with --samples <file>, one line per scheduled sample:
//     seq due_ns pub_start_ns pub_return_ns reply_ns        (reply_ns 0: no reply came)
// Exit 0 on a complete run, 2 when no match, 3 when no warm-up round trip, 1 on any other failure.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rmw/rmw.h>

#include "pingpong_common.hpp"
#include "rmw_perf_pingpong/msg/ack.hpp"
#include "rmw_perf_pingpong/msg/array1k.hpp"
#include "rmw_perf_pingpong/msg/array1m.hpp"
#include "rmw_perf_pingpong/msg/array4m.hpp"

namespace {

    using pingpong::now_ns;

    constexpr uint64_t ns_per_s = 1000000000ULL;
    constexpr double ns_per_s_d = 1e9;
    constexpr double ns_per_ms_d = 1e6;
    constexpr double ns_per_us_d = 1e3;
    constexpr uint64_t match_limit_s = 30;
    constexpr uint64_t warmup_limit_s = 20;
    constexpr int poll_ms = 20;
    constexpr size_t ack_depth = 10;
    constexpr uint64_t warmup_id = 0;
    constexpr double p50 = 0.50;
    constexpr double p99 = 0.99;
    constexpr double default_rate_hz = 30.0;
    constexpr double default_duration_s = 30.0;
    constexpr double default_drain_s = 3.0;

    struct options {
        const char* message = "array1m";
        double rate_hz = default_rate_hz;
        double duration_s = default_duration_s;
        double drain_s = default_drain_s;
        bool reliable = true;
        size_t depth = ack_depth;
        const char* samples_path = nullptr;
    };

    // Written by the executor thread (reply callback), read by the publishing thread only after the executor is
    // joined, apart from the counters it polls.
    struct replies {
        std::unique_ptr<std::atomic<uint64_t>[]> reply_ns; // NOLINT(modernize-avoid-c-arrays) - atomics, sized once
        uint64_t count = 0;                                // ids 1..count
        std::atomic<uint64_t> replied {0};
        std::atomic<uint64_t> dup {0};
        std::atomic<uint64_t> unknown {0};
        std::atomic<bool> warmed {false};
        std::atomic<uint64_t> warmup_late {0};
    };

    auto on_reply(replies& rep, uint64_t id, uint64_t arrival_ns) -> void {
        if (id == warmup_id) {
            if (rep.warmed.exchange(true)) {
                rep.warmup_late++;
            }
            return;
        }
        if (id > rep.count) {
            rep.unknown++;
            return;
        }
        uint64_t expected = 0;
        if (rep.reply_ns[id].compare_exchange_strong(expected, arrival_ns)) {
            rep.replied++;
        } else {
            rep.dup++;
        }
    }

    // Per scheduled sample, written by the publishing thread only.
    struct schedule {
        std::vector<uint64_t> due_ns;
        std::vector<uint64_t> pub_start_ns;
        std::vector<uint64_t> pub_return_ns;
    };

    auto set_fields(rmw_perf_pingpong::msg::Ack& msg, uint64_t id, uint64_t /*due*/) -> void {
        msg.id = id;
    }
    auto set_fields(rmw_perf_pingpong::msg::Array1k& msg, uint64_t id, uint64_t due) -> void {
        msg.id = id;
        msg.time = static_cast<int64_t>(due);
    }
    auto set_fields(rmw_perf_pingpong::msg::Array1m& msg, uint64_t id, uint64_t due) -> void {
        msg.id = id;
        msg.time = static_cast<int64_t>(due);
    }
    auto set_fields(rmw_perf_pingpong::msg::Array4m& msg, uint64_t id, uint64_t due) -> void {
        msg.id = id;
        msg.time = static_cast<int64_t>(due);
    }

    auto sleep_until(uint64_t when_ns) -> void {
        const struct timespec until = {.tv_sec = static_cast<time_t>(when_ns / ns_per_s),
                                       .tv_nsec = static_cast<long>(when_ns % ns_per_s)};
        // CLOCK_MONOTONIC and clock_nanosleep come through a private glibc header (see pingpong::now_ns()).
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) != 0) { // NOLINT(misc-include-cleaner)
        }
    }

    auto percentile(std::vector<int64_t> values, double quantile) -> int64_t {
        if (values.empty()) {
            return -1;
        }
        const auto rank = static_cast<size_t>(std::ceil(quantile * static_cast<double>(values.size()))) - 1;
        const size_t index = std::min(rank, values.size() - 1);
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
        return values[index];
    }

    template <typename Pub, typename Sub> auto wait_for_match(const Pub& pub, const Sub& sub) -> bool {
        const uint64_t deadline = now_ns() + (match_limit_s * ns_per_s);
        while (rclcpp::ok() && now_ns() < deadline &&
               (pub->get_subscription_count() == 0 || sub->get_publisher_count() == 0)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        }
        return pub->get_subscription_count() > 0 && sub->get_publisher_count() > 0;
    }

    // Matching is seen from this side only: the receiver's reply writer may not have matched this node's reader yet,
    // and a writer with no matched reader sends nothing. So id 0 goes out once per period until one comes back, and
    // only then does the measured schedule start (ping_node's warm-up, 2026-10-18).
    template <typename T>
    auto warm_up(const typename rclcpp::Publisher<T>::SharedPtr& pub, T& msg, const replies& rep, uint64_t period_ns,
                 uint64_t& sent) -> uint64_t {
        const uint64_t start = now_ns();
        const uint64_t limit = start + (warmup_limit_s * ns_per_s);
        uint64_t next = start;
        while (rclcpp::ok() && !rep.warmed && now_ns() < limit) {
            if (now_ns() >= next) {
                set_fields(msg, warmup_id, now_ns());
                pub->publish(msg);
                sent++;
                next += period_ns;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return now_ns() - start;
    }

    auto write_samples(const char* path, const schedule& sched, const replies& rep) -> bool {
        FILE* out = std::fopen(path, "w");
        if (out == nullptr) {
            return false;
        }
        std::fprintf(out, "# seq due_ns pub_start_ns pub_return_ns reply_ns (sender CLOCK_MONOTONIC ns; reply_ns 0 = "
                          "no reply)\n");
        for (uint64_t seq = 1; seq <= rep.count; seq++) {
            std::fprintf(out, "%llu %llu %llu %llu %llu\n", static_cast<unsigned long long>(seq),
                         static_cast<unsigned long long>(sched.due_ns[seq]),
                         static_cast<unsigned long long>(sched.pub_start_ns[seq]),
                         static_cast<unsigned long long>(sched.pub_return_ns[seq]),
                         static_cast<unsigned long long>(rep.reply_ns[seq].load()));
        }
        std::fprintf(out, "# end sent=%llu replied=%llu\n", static_cast<unsigned long long>(rep.count),
                     static_cast<unsigned long long>(rep.replied.load()));
        return std::fclose(out) == 0;
    }

    struct summary_in {
        const options* opt = nullptr;
        uint64_t warmup_sent = 0;
        uint64_t warmup_ns = 0;
        uint64_t drain_ns = 0;
    };

    auto print_result(const summary_in& info, const schedule& sched, const replies& rep) -> void {
        std::vector<int64_t> rtt;
        std::vector<int64_t> send_rtt;
        std::vector<int64_t> gap;
        std::vector<int64_t> pub;
        for (uint64_t seq = 1; seq <= rep.count; seq++) {
            const auto due = static_cast<int64_t>(sched.due_ns[seq]);
            const auto start = static_cast<int64_t>(sched.pub_start_ns[seq]);
            gap.push_back(start - due);
            pub.push_back(static_cast<int64_t>(sched.pub_return_ns[seq]) - start);
            const auto reply = static_cast<int64_t>(rep.reply_ns[seq].load());
            if (reply != 0) {
                rtt.push_back(reply - due);
                send_rtt.push_back(reply - start);
            }
        }
        const auto to_ms = [](int64_t value) -> double {
            return static_cast<double>(value) / ns_per_ms_d;
        };
        const auto to_us = [](int64_t value) -> double {
            return static_cast<double>(value) / ns_per_us_d;
        };
        const uint64_t replied = rep.replied.load();
        std::printf(
            "RESULT: framework=%s msg=%s qos=%s depth=%zu rate_hz=%.3f sent=%llu replied=%llu missing=%llu "
            "dup=%llu unknown=%llu",
            rmw_get_implementation_identifier(), info.opt->message, info.opt->reliable ? "reliable" : "best_effort",
            info.opt->depth, info.opt->rate_hz, static_cast<unsigned long long>(rep.count),
            static_cast<unsigned long long>(replied), static_cast<unsigned long long>(rep.count - replied),
            static_cast<unsigned long long>(rep.dup.load()), static_cast<unsigned long long>(rep.unknown.load()));
        std::printf(" rtt_p50_ms=%.4f rtt_p99_ms=%.4f rtt_max_ms=%.4f send_rtt_p50_ms=%.4f",
                    to_ms(percentile(rtt, p50)), to_ms(percentile(rtt, p99)), to_ms(percentile(rtt, 1.0)),
                    to_ms(percentile(send_rtt, p50)));
        std::printf(" gap_p50_us=%.1f gap_p99_us=%.1f gap_max_us=%.1f pub_p50_us=%.1f pub_p99_us=%.1f pub_max_us=%.1f",
                    to_us(percentile(gap, p50)), to_us(percentile(gap, p99)), to_us(percentile(gap, 1.0)),
                    to_us(percentile(pub, p50)), to_us(percentile(pub, p99)), to_us(percentile(pub, 1.0)));
        std::printf(" warmup_sent=%llu warmup_ms=%.1f warmup_late=%llu drain_ms=%.1f\n",
                    static_cast<unsigned long long>(info.warmup_sent), to_ms(static_cast<int64_t>(info.warmup_ns)),
                    static_cast<unsigned long long>(rep.warmup_late.load()),
                    to_ms(static_cast<int64_t>(info.drain_ns)));
        std::fflush(stdout);
    }

    // The measured schedule: sample k (1-based) is due at start + (k - 1) * period. A publish that returns past the
    // next due time is followed at once by the next one (no skipping): its lateness is the framework's.
    template <typename T>
    auto run_schedule(const typename rclcpp::Publisher<T>::SharedPtr& pub, T& msg, schedule& sched, uint64_t count,
                      uint64_t period_ns) -> void {
        const uint64_t start = now_ns() + period_ns;
        for (uint64_t seq = 1; seq <= count && rclcpp::ok(); seq++) {
            const uint64_t due = start + ((seq - 1) * period_ns);
            sleep_until(due);
            set_fields(msg, seq, due);
            sched.due_ns[seq] = due;
            sched.pub_start_ns[seq] = now_ns();
            pub->publish(msg);
            sched.pub_return_ns[seq] = now_ns();
        }
    }

    template <typename T> auto run_tx(const rclcpp::Node::SharedPtr& node, const options& opt) -> int {
        rclcpp::QoS qos(opt.depth);
        qos.keep_last(opt.depth);
        if (opt.reliable) {
            qos.reliable();
        } else {
            qos.best_effort();
        }
        rclcpp::QoS ack_qos(ack_depth);
        ack_qos.reliable().keep_last(ack_depth);

        const auto count = static_cast<uint64_t>(std::llround(opt.rate_hz * opt.duration_s));
        const auto period_ns = static_cast<uint64_t>(std::llround(ns_per_s_d / opt.rate_hz));
        replies rep;
        rep.count = count;
        rep.reply_ns = std::make_unique<std::atomic<uint64_t>[]>(count + 1); // NOLINT(modernize-avoid-c-arrays)
        for (uint64_t i = 0; i <= count; i++) {
            rep.reply_ns[i].store(0);
        }
        schedule sched;
        sched.due_ns.assign(count + 1, 0);
        sched.pub_start_ns.assign(count + 1, 0);
        sched.pub_return_ns.assign(count + 1, 0);

        auto pub = node->create_publisher<T>("l4_large", qos);
        auto sub = node->create_subscription<rmw_perf_pingpong::msg::Ack>(
            "l4_ack", ack_qos, [&rep](const rmw_perf_pingpong::msg::Ack::ConstSharedPtr& ack) -> void {
                on_reply(rep, ack->id, now_ns());
            });
        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);
        std::thread spinner([&executor]() -> void {
            executor.spin();
        });
        const auto stop = [&executor, &spinner]() -> void {
            executor.cancel();
            spinner.join();
        };

        auto msg = std::make_unique<T>(); // a large message lives on the heap; its payload stays zero
        if (!wait_for_match(pub, sub)) {
            std::fprintf(stderr, "timed out waiting for a match (%llu s)\n",
                         static_cast<unsigned long long>(match_limit_s));
            stop();
            return 2;
        }
        uint64_t warmup_sent = 0;
        const uint64_t warmup_ns = warm_up<T>(pub, *msg, rep, period_ns, warmup_sent);
        if (!rep.warmed) {
            std::fprintf(stderr, "no warm-up round trip in %llu s (%llu warm-up samples)\n",
                         static_cast<unsigned long long>(warmup_limit_s), static_cast<unsigned long long>(warmup_sent));
            stop();
            return 3;
        }
        run_schedule<T>(pub, *msg, sched, count, period_ns);
        const uint64_t drain_start = now_ns();
        const auto drain_end = drain_start + static_cast<uint64_t>(std::llround(opt.drain_s * ns_per_s_d));
        while (rep.replied.load() < count && now_ns() < drain_end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const uint64_t drain_ns = now_ns() - drain_start;
        stop();

        summary_in info;
        info.opt = &opt;
        info.warmup_sent = warmup_sent;
        info.warmup_ns = warmup_ns;
        info.drain_ns = drain_ns;
        print_result(info, sched, rep);
        if (opt.samples_path != nullptr && !write_samples(opt.samples_path, sched, rep)) {
            std::fprintf(stderr, "cannot write --samples file %s\n", opt.samples_path);
            return 1;
        }
        return 0;
    }

    auto default_node_options() -> rclcpp::NodeOptions {
        return rclcpp::NodeOptions()
            .start_parameter_services(false)
            .start_parameter_event_publisher(false)
            .enable_rosout(false)
            .parameter_overrides({rclcpp::Parameter("start_type_description_service", false)});
    }

    auto run_message(const rclcpp::Node::SharedPtr& node, const options& opt) -> int {
        if (std::strcmp(opt.message, "array1m") == 0) {
            return run_tx<rmw_perf_pingpong::msg::Array1m>(node, opt);
        }
        if (std::strcmp(opt.message, "array4m") == 0) {
            return run_tx<rmw_perf_pingpong::msg::Array4m>(node, opt);
        }
        if (std::strcmp(opt.message, "array1k") == 0) {
            return run_tx<rmw_perf_pingpong::msg::Array1k>(node, opt);
        }
        if (std::strcmp(opt.message, "ack") == 0) {
            return run_tx<rmw_perf_pingpong::msg::Ack>(node, opt);
        }
        std::fprintf(stderr, "unknown -m '%s' (expected array1m|array4m|array1k|ack)\n", opt.message);
        return 1;
    }

    auto parse(int argc, char** argv, options& opt) -> bool {
        for (int i = 1; i < argc; i++) {
            const bool has_value = i + 1 < argc;
            if (std::strcmp(argv[i], "-m") == 0 && has_value) {
                opt.message = argv[++i];
            } else if (std::strcmp(argv[i], "--rate") == 0 && has_value) {
                opt.rate_hz = std::atof(argv[++i]);
            } else if (std::strcmp(argv[i], "-d") == 0 && has_value) {
                opt.duration_s = std::atof(argv[++i]);
            } else if (std::strcmp(argv[i], "--drain") == 0 && has_value) {
                opt.drain_s = std::atof(argv[++i]);
            } else if (std::strcmp(argv[i], "--qos") == 0 && has_value) {
                opt.reliable = std::strcmp(argv[++i], "reliable") == 0;
            } else if (std::strcmp(argv[i], "--depth") == 0 && has_value) {
                opt.depth = static_cast<size_t>(std::atoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--samples") == 0 && has_value) {
                opt.samples_path = argv[++i];
            }
        }
        return opt.rate_hz > 0.0 && opt.duration_s > 0.0 && opt.depth > 0;
    }

} // namespace

auto main(int argc, char** argv) -> int {
    options opt;
    if (!parse(argc, argv, opt)) {
        std::fprintf(stderr, "usage: large_rtt_tx -m array1m|array4m|array1k|ack --rate HZ -d S --qos "
                             "reliable|best_effort --depth N [--drain S] [--samples FILE]\n");
        return 1;
    }
    try {
        rclcpp::init(argc, argv);
        auto node = std::make_shared<rclcpp::Node>("large_rtt_tx", default_node_options());
        const int ret = run_message(node, opt);
        rclcpp::shutdown();
        return ret;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unhandled exception: %s\n", e.what());
        return 1;
    }
}
