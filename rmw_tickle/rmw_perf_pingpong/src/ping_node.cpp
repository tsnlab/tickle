// rmw_tickle/COMPARISON.MD's "Design: rmw_perf_pingpong" - ping role. Publisher on "ping",
// subscriber on "pong" - mirrors examples/perf_hil/cyclonedds/best_effort_latency/client.c's own
// shape exactly (same CLI flags, same blocking send-then-wait-for-echo pattern, same RESULT line)
// so the two tracks (native no-rmw HIL, this rmw-layer tool) stay directly comparable.
//
// RTT is computed entirely against this process's own now_ns() - never a timestamp read on a
// different machine - so it needs no cross-host clock agreement at all (COMPARISON.MD's own
// retracted item 5 attempt mixed two independently-clocked machines' timestamps; this tool never
// does that).
//
// -m <bench|array1k|struct16> (payload-size expansion, COMPARISON.MD's own priority list): `Bench`
// stays the default (64-byte payload, matches examples/perf_hil/idl/p1/Bench.idl exactly, for the
// native-HIL-vs-rmw-layer comparison this tool was originally built for); `array1k`/`struct16`
// give this same single-clock-RTT-correct methodology the same message shapes buildfarm_perf_
// tests' own Array1k.msg/Struct16.msg already use (msg/Array1k.msg's own header comment explains
// why they're a deliberate copy here, not a real dependency on that upstream package) - directly
// comparable against COMPARISON.MD's own existing "Performance comparison" section's numbers,
// same wire layout. Templated on the message type (BenchTraits<T> below) rather than three near-
// duplicate copies of this whole file - pong_node.cpp needs no type-specific logic at all (a pure
// echo), only ping_node.cpp's own sequence-id/send-timestamp field access differs per type.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <thread>

#include <rclcpp/rclcpp.hpp>

#include "pingpong_common.hpp"
#include "rmw_perf_pingpong/msg/array1k.hpp"
#include "rmw_perf_pingpong/msg/bench.hpp"
#include "rmw_perf_pingpong/msg/struct16.hpp"

namespace {

    using pingpong::BenchTraits;
    using pingpong::now_ns;

    // Same conversion hal_linux.c's own now_ns() uses (its SEC_NS) - kept as two named constants
    // here (seconds and milliseconds) rather than one, since this file needs both.
    constexpr uint64_t ns_per_s = 1000000000ULL;
    constexpr uint64_t ns_per_ms = 1000000ULL;

    constexpr int discovery_poll_ms = 20;
    constexpr uint64_t reply_wait_ms = 500; // matching the native client's own reply timeout
    constexpr int spin_poll_us = 100;
    constexpr double default_duration_s = 10.0;

    // Every rclcpp::Node on ROS 2 Jazzy unconditionally subscribes to /parameter_events (its own
    // internal NodeTimeSource, for use_sim_time monitoring - no NodeOptions flag disables it) and
    // creates a ~/get_type_description service - neither of which this tool needs. The type
    // description service is opted out of via a default parameter override here (so this binary
    // works standalone, without needing --ros-args on every invocation); /parameter_events itself
    // needs rmw_tickle to actually have typesupport for rcl_interfaces/msg/ParameterEvent, which is
    // a separate, real compatibility gap (COMPARISON.MD's own item 5 write-up) - not something this
    // tool's own code can work around.
    auto default_node_options() -> rclcpp::NodeOptions {
        return rclcpp::NodeOptions()
            .start_parameter_services(false)
            .start_parameter_event_publisher(false)
            .enable_rosout(false)
            .parameter_overrides({rclcpp::Parameter("start_type_description_service", false)});
    }

    // Round-trip statistics over the replies that came back. Split out of run_ping() to keep it under
    // clang-tidy's cognitive-complexity threshold.
    struct rtt_stats {
        uint64_t received = 0;
        double min_ms = -1.0;
        double max_ms = 0.0;
        double sum_ms = 0.0;
    };

    auto add_rtt(rtt_stats& stats, double rtt_ms) -> void {
        stats.received++;
        if (stats.min_ms < 0.0 || rtt_ms < stats.min_ms) {
            stats.min_ms = rtt_ms;
        }
        stats.max_ms = std::max(rtt_ms, stats.max_ms);
        stats.sum_ms += rtt_ms;
    }

    // What poll mode spends (2026-09-26, RMW_PERF_PLAN §7): the wait loop's iterations, and how long its
    // spin_some() calls and its sleeps really took - the sleep asks for 100 us and the kernel's timer slack
    // decides the rest. Printed per round trip on a LOOP: line after RESULT.
    constexpr double ns_per_us = 1000.0;

    struct loop_stats {
        uint64_t iterations = 0;
        uint64_t spin_ns = 0;
        uint64_t sleep_ns = 0;
    };

    // A line of its own after RESULT, so RESULT's fields stay as every parser knows them: iterations of the
    // wait loop per round trip, and in poll mode the mean spin_some() and the mean real sleep, in us.
    auto print_loop_stats(const loop_stats& loop, uint64_t transmitted) -> void {
        const double per_rtt =
            transmitted > 0 ? static_cast<double>(loop.iterations) / static_cast<double>(transmitted) : 0.0;
        const double iterations = loop.iterations > 0 ? static_cast<double>(loop.iterations) : 1.0;
        std::printf("LOOP: iterations_per_rtt=%.2f spin_some_us=%.1f sleep_us=%.1f\n", per_rtt,
                    static_cast<double>(loop.spin_ns) / iterations / ns_per_us,
                    static_cast<double>(loop.sleep_ns) / iterations / ns_per_us);
    }

    // Spins until the reply has arrived or the deadline passes.
    //
    // poll (the default, and every row before 2026-09-26): spin_some() and a 100 us sleep, with the round
    // trip read after the loop - so it includes the sleep that follows the spin that took the reply, and a
    // reply is only seen at the loop's own ~150 us cadence (the sleep plus the kernel's timer slack).
    // Measured on a veth pair, that is 230-255 us of every round trip, for all three rmw implementations
    // alike (examples/perf_hil/experiments/rmw_ping_wait_mode.sh). block waits in spin_once() until the
    // reply wakes the executor, as the native client waits in tt_Node_poll(); the caller then reads the
    // round trip at the callback.
    auto wait_for_reply(rclcpp::executors::SingleThreadedExecutor& executor, const std::atomic<bool>& got_reply,
                        uint64_t wait_deadline, bool blocking, loop_stats& loop) -> void {
        while (rclcpp::ok() && !got_reply && now_ns() < wait_deadline) {
            loop.iterations++;
            if (blocking) {
                executor.spin_once(std::chrono::nanoseconds(wait_deadline - now_ns()));
            } else {
                const uint64_t spin_start = now_ns();
                executor.spin_some();
                const uint64_t sleep_start = now_ns();
                std::this_thread::sleep_for(std::chrono::microseconds(spin_poll_us));
                loop.spin_ns += sleep_start - spin_start;
                loop.sleep_ns += now_ns() - sleep_start;
            }
        }
    }

    template <typename T>
    auto run_ping(const rclcpp::Node::SharedPtr& node, double interval_s, double duration_s, bool reliable,
                  bool blocking, pingpong::stamp_log& stamps) -> int {
        using Traits = BenchTraits<T>;

        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);

        rclcpp::QoS qos(8);
        if (reliable) {
            qos.reliable().keep_last(8);
        } else {
            qos.best_effort();
        }

        auto pub = node->create_publisher<T>("ping", qos);

        std::atomic<bool> got_reply {false};
        T reply_msg;
        uint64_t reply_ns = 0; // when the reply reached the callback - what --wait block measures to
        auto sub = node->create_subscription<T>("pong", qos, [&](const typename T::ConstSharedPtr& msg) -> void {
            reply_ns = now_ns();
            reply_msg = *msg;
            got_reply = true;
        });

        // Give discovery a moment - a send before both sides have matched would just be lost,
        // undercounting "sent" for no real reason (same reasoning as the native client's own
        // wait_for_writer_match()/wait_for_reader_match()).
        {
            const uint64_t match_deadline = now_ns() + (10ULL * ns_per_s);
            while (rclcpp::ok() && now_ns() < match_deadline &&
                   (pub->get_subscription_count() == 0 || sub->get_publisher_count() == 0)) {
                executor.spin_some();
                std::this_thread::sleep_for(std::chrono::milliseconds(discovery_poll_ms));
            }
            if (pub->get_subscription_count() == 0 || sub->get_publisher_count() == 0) {
                std::fprintf(stderr, "timed out waiting for a match\n");
                return 1;
            }
        }

        uint64_t transmitted = 0;
        rtt_stats rtt;
        loop_stats loop;

        uint64_t seq = 0;
        const uint64_t start = now_ns();
        const uint64_t deadline = start + static_cast<uint64_t>(duration_s * static_cast<double>(ns_per_s));
        const auto interval_ns = static_cast<uint64_t>(interval_s * static_cast<double>(ns_per_s));

        while (rclcpp::ok() && now_ns() < deadline) {
            T req;
            Traits::set_seq(req, ++seq);
            const uint64_t sent_at = now_ns();
            Traits::set_send_ns(req, sent_at);
            got_reply = false;
            pub->publish(req);
            transmitted++;

            const uint64_t wait_deadline = now_ns() + (reply_wait_ms * ns_per_ms); // 500ms, matching the native client
            wait_for_reply(executor, got_reply, wait_deadline, blocking, loop);
            if (got_reply && Traits::seq(reply_msg) == seq) {
                pingpong::stamp_log_add(stamps, seq, sent_at, reply_ns); // --stamps, whichever the wait mode
                const uint64_t end_ns = blocking ? reply_ns : now_ns();
                add_rtt(rtt, static_cast<double>(end_ns - Traits::send_ns(reply_msg)) / static_cast<double>(ns_per_ms));
            }

            const struct timespec sleep_ts = {.tv_sec = static_cast<time_t>(interval_ns / ns_per_s),
                                              .tv_nsec = static_cast<long>(interval_ns % ns_per_s)};
            nanosleep(&sleep_ts, nullptr); // NOLINT(misc-include-cleaner) - see now_ns()'s own <ctime> comment
        }

        const uint64_t received = rtt.received;
        const double rtt_min_ms = rtt.min_ms;
        const double rtt_max_ms = rtt.max_ms;
        const uint64_t lost = transmitted - received;
        const double loss_pct =
            transmitted > 0 ? (100.0 * static_cast<double>(lost) / static_cast<double>(transmitted)) : 0.0;
        const double avg = received > 0 ? rtt.sum_ms / static_cast<double>(received) : 0.0;

        const char* rmw_impl = std::getenv("RMW_IMPLEMENTATION");
        if (rmw_impl == nullptr) {
            rmw_impl = "rmw_fastrtps_cpp"; // ROS 2's own real default when unset
        }

        std::printf("\n--- %s pingpong statistics (%s) ---\n", rmw_impl, reliable ? "reliable" : "best_effort");
        std::printf("%lu sent, %lu received, %.0f%% loss\n", static_cast<unsigned long>(transmitted),
                    static_cast<unsigned long>(received), loss_pct);
        if (received > 0) {
            std::printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", rtt_min_ms, avg, rtt_max_ms);
        }
        std::printf("RESULT: framework=%s scenario=pingpong qos=%s wait=%s sent=%lu recv=%lu loss_pct=%.0f "
                    "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
                    rmw_impl, reliable ? "reliable" : "best_effort", blocking ? "block" : "poll",
                    static_cast<unsigned long>(transmitted), static_cast<unsigned long>(received), loss_pct, rtt_min_ms,
                    avg, rtt_max_ms);
        print_loop_stats(loop, transmitted);

        return 0;
    }

} // namespace

auto main(int argc, char** argv) -> int {
    double interval_s = 1.0;
    double duration_s = default_duration_s;
    bool reliable = false;
    bool blocking = false;             // --wait block|poll, see run_ping()
    const char* stamps_path = nullptr; // --stamps <file>, see pingpong::stamp_log
    const char* message = "bench";
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
            blocking = std::strcmp(argv[++i], "block") == 0;
        } else if (std::strcmp(argv[i], "--stamps") == 0 && i + 1 < argc) {
            stamps_path = argv[++i];
        } else if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--reliable") == 0) {
            reliable = true;
        } else if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            message = argv[++i];
        }
    }

    // rclcpp internals (QoS-override parameter declaration, the subscription callback's own
    // std::variant storage) can throw past this point - e.g. a malformed --ros-args -p
    // qos_overrides...:=... value reaches rclcpp::exceptions::InvalidQosOverridesException here,
    // not at arg-parsing above. Reported the same way this file already reports every other
    // failure (stderr + non-zero exit), rather than letting it escape main() as an unhandled
    // exception.
    try {
        rclcpp::init(argc, argv);
        auto node = std::make_shared<rclcpp::Node>("ping_node", default_node_options());

        pingpong::stamp_log stamps;
        pingpong::stamp_log_open(stamps, stamps_path,
                                 "ping: seq send_ns reply_ns (CLOCK_MONOTONIC ns; replied samples only)");
        int ret;
        if (std::strcmp(message, "bench") == 0) {
            ret = run_ping<rmw_perf_pingpong::msg::Bench>(node, interval_s, duration_s, reliable, blocking, stamps);
        } else if (std::strcmp(message, "array1k") == 0) {
            ret = run_ping<rmw_perf_pingpong::msg::Array1k>(node, interval_s, duration_s, reliable, blocking, stamps);
        } else if (std::strcmp(message, "struct16") == 0) {
            ret = run_ping<rmw_perf_pingpong::msg::Struct16>(node, interval_s, duration_s, reliable, blocking, stamps);
        } else {
            std::fprintf(stderr, "unknown -m '%s' (expected bench|array1k|struct16)\n", message);
            ret = 1;
        }

        if (!pingpong::stamp_log_write(stamps)) {
            std::fprintf(stderr, "cannot write --stamps file %s\n", stamps_path);
            ret = 1;
        }
        rclcpp::shutdown();
        return ret;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unhandled exception: %s\n", e.what());
        return 1;
    }
}
