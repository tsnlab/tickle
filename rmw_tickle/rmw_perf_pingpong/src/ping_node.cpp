// rmw_tickle/COMPARISON.md's "Design: rmw_perf_pingpong" - ping role. Publisher on "ping",
// subscriber on "pong" - mirrors examples/perf_hil/cyclonedds/best_effort_latency/client.c's own
// shape exactly (same CLI flags, same blocking send-then-wait-for-echo pattern, same RESULT line)
// so the two tracks (native no-rmw HIL, this rmw-layer tool) stay directly comparable.
//
// RTT is computed entirely against this process's own now_ns() - never a timestamp read on a
// different machine - so it needs no cross-host clock agreement at all (COMPARISON.md's own
// retracted item 5 attempt mixed two independently-clocked machines' timestamps; this tool never
// does that).
//
// -m <bench|array1k|struct16> (payload-size expansion, COMPARISON.md's own priority list): `Bench`
// stays the default (64-byte payload, matches examples/perf_hil/idl/p1/Bench.idl exactly, for the
// native-HIL-vs-rmw-layer comparison this tool was originally built for); `array1k`/`struct16`
// give this same single-clock-RTT-correct methodology the same message shapes buildfarm_perf_
// tests' own Array1k.msg/Struct16.msg already use (msg/Array1k.msg's own header comment explains
// why they're a deliberate copy here, not a real dependency on that upstream package) - directly
// comparable against COMPARISON.md's own existing "Performance comparison" section's numbers,
// same wire layout. Templated on the message type (BenchTraits<T> below) rather than three near-
// duplicate copies of this whole file - pong_node.cpp needs no type-specific logic at all (a pure
// echo), only ping_node.cpp's own sequence-id/send-timestamp field access differs per type.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "pingpong_common.hpp"
#include "rmw_perf_pingpong/msg/array1k.hpp"
#include "rmw_perf_pingpong/msg/bench.hpp"
#include "rmw_perf_pingpong/msg/struct16.hpp"

namespace {

    using pingpong::BenchTraits;
    using pingpong::now_ns;
    using pingpong::phase_probe;

    // Same conversion hal_linux.c's own now_ns() uses (its SEC_NS) - kept as two named constants
    // here (seconds and milliseconds) rather than one, since this file needs both.
    constexpr uint64_t ns_per_s = 1000000000ULL;
    constexpr uint64_t ns_per_ms = 1000000ULL;

    constexpr int discovery_poll_ms = 20;
    constexpr uint64_t reply_wait_ms = 500;    // matching the native client's own reply timeout
    constexpr int default_poll_sleep_us = 100; // --poll-sleep-us overrides it
    constexpr double default_duration_s = 10.0;

    // Every rclcpp::Node on ROS 2 Jazzy unconditionally subscribes to /parameter_events (its own
    // internal NodeTimeSource, for use_sim_time monitoring - no NodeOptions flag disables it) and
    // creates a ~/get_type_description service - neither of which this tool needs. The type
    // description service is opted out of via a default parameter override here (so this binary
    // works standalone, without needing --ros-args on every invocation); /parameter_events itself
    // needs rmw_tickle to actually have typesupport for rcl_interfaces/msg/ParameterEvent, which is
    // a separate, real compatibility gap (COMPARISON.md's own item 5 write-up) - not something this
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

    // How the ping waits for each reply (--wait, --poll-sleep-us): blocking in spin_once(), or polling with
    // spin_some() and a sleep of poll_sleep_us between spins. The user's decision (2026-09-26): both modes
    // are test cases, and poll is swept over its sleep rather than measured at one arbitrary grid.
    //
    // Two poll-mode options (RMW_PERF_PLAN 10.1's proposals (a) and (b), branch only, pending the user's decision);
    // both default to today's behaviour, so every earlier row stays reproducible:
    //   --phase locked|random  locked: publish, then poll until the reply (today). random: the poll loop runs on its
    //                          own and a second thread publishes on its own clock, so the reply lands at a random
    //                          phase of the poll cycle (pingpong::phase_probe checks that it does).
    //   --rtt-at loop|callback loop: the round trip ends after the poll cycle that took the reply, its sleep
    //                          included (today). callback: it ends at the reply's callback, as block mode's does.
    struct wait_mode {
        bool blocking = false;
        int poll_sleep_us = default_poll_sleep_us;
        bool random_phase = false;
        bool rtt_at_callback = false;
    };

    struct loop_stats {
        uint64_t iterations = 0;
        uint64_t spin_ns = 0;
        uint64_t sleep_ns = 0;
    };

    // A line of its own after RESULT, so RESULT's fields stay as every parser knows them: iterations of the
    // wait loop per round trip, and in poll mode the mean spin_some() and the mean real sleep, in us.
    auto print_loop_stats(const loop_stats& loop, uint64_t transmitted, const wait_mode& wait) -> void {
        const double per_rtt =
            transmitted > 0 ? static_cast<double>(loop.iterations) / static_cast<double>(transmitted) : 0.0;
        const double iterations = loop.iterations > 0 ? static_cast<double>(loop.iterations) : 1.0;
        std::printf("LOOP: poll_sleep_us=%d iterations_per_rtt=%.2f spin_some_us=%.1f sleep_us=%.1f\n",
                    wait.poll_sleep_us, per_rtt, static_cast<double>(loop.spin_ns) / iterations / ns_per_us,
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
                        uint64_t wait_deadline, const wait_mode& wait, loop_stats& loop, phase_probe& probe) -> void {
        while (rclcpp::ok() && !got_reply && now_ns() < wait_deadline) {
            loop.iterations++;
            if (wait.blocking) {
                executor.spin_once(std::chrono::nanoseconds(wait_deadline - now_ns()));
            } else {
                const uint64_t spin_start = now_ns();
                probe.on_check(spin_start);
                executor.spin_some();
                const uint64_t sleep_start = now_ns();
                std::this_thread::sleep_for(std::chrono::microseconds(wait.poll_sleep_us));
                loop.spin_ns += sleep_start - spin_start;
                loop.sleep_ns += now_ns() - sleep_start;
            }
        }
    }

    // The PHASE: line (poll mode): the options in force, where the pings fell in the poll cycle, and how long after
    // each publish its reply reached the callback - spread over about one cycle when the phase is random, and
    // narrow when it is locked.
    auto print_phase(const phase_probe& probe, std::vector<uint64_t>& callback_after_send, const wait_mode& wait)
        -> void {
        uint64_t placed = 0;
        const double chi2 = pingpong::phase_chi2(probe, placed);
        std::printf("PHASE: phase=%s rtt_at=%s placed=%lu unplaced=%lu bins=", wait.random_phase ? "random" : "locked",
                    wait.rtt_at_callback ? "callback" : "loop", static_cast<unsigned long>(placed),
                    static_cast<unsigned long>(probe.unplaced));
        for (size_t i = 0; i < probe.counts.size(); i++) {
            std::printf("%s%lu", i == 0 ? "" : "/", static_cast<unsigned long>(probe.counts[i]));
        }
        std::printf(" chi2=%.2f", chi2);
        std::sort(callback_after_send.begin(), callback_after_send.end());
        const size_t count = callback_after_send.size();
        if (count > 0) {
            constexpr size_t p10 = 10;
            constexpr size_t p50 = 50;
            constexpr size_t p90 = 90;
            constexpr size_t hundred = 100;
            std::printf(" callback_p10_us=%.1f callback_p50_us=%.1f callback_p90_us=%.1f",
                        static_cast<double>(callback_after_send[count * p10 / hundred]) / ns_per_us,
                        static_cast<double>(callback_after_send[count * p50 / hundred]) / ns_per_us,
                        static_cast<double>(callback_after_send[count * p90 / hundred]) / ns_per_us);
        }
        std::printf("\n");
    }

    auto print_summary(const rtt_stats& rtt, uint64_t transmitted, bool reliable, const wait_mode& wait) -> void {
        const uint64_t received = rtt.received;
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
            std::printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", rtt.min_ms, avg, rtt.max_ms);
        }
        std::printf("RESULT: framework=%s scenario=pingpong qos=%s wait=%s sent=%lu recv=%lu loss_pct=%.0f "
                    "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
                    rmw_impl, reliable ? "reliable" : "best_effort", wait.blocking ? "block" : "poll",
                    static_cast<unsigned long>(transmitted), static_cast<unsigned long>(received), loss_pct, rtt.min_ms,
                    avg, rtt.max_ms);
    }

    // Waits until both sides have matched. Give discovery a moment - a send before both sides have matched would
    // just be lost, undercounting "sent" for no real reason (same reasoning as the native client's own
    // wait_for_writer_match()/wait_for_reader_match()).
    template <typename Pub, typename Sub>
    auto wait_for_match(rclcpp::executors::SingleThreadedExecutor& executor, const Pub& pub, const Sub& sub) -> bool {
        const uint64_t match_deadline = now_ns() + (10ULL * ns_per_s);
        while (rclcpp::ok() && now_ns() < match_deadline &&
               (pub->get_subscription_count() == 0 || sub->get_publisher_count() == 0)) {
            executor.spin_some();
            std::this_thread::sleep_for(std::chrono::milliseconds(discovery_poll_ms));
        }
        if (pub->get_subscription_count() == 0 || sub->get_publisher_count() == 0) {
            std::fprintf(stderr, "timed out waiting for a match\n");
            return false;
        }
        return true;
    }

    // --phase random: the reply as the sender thread sees it. The callback fills it on the poll thread; the poll
    // thread stamps seen_ns when the cycle that took it has ended, and wakes the sender.
    struct reply_slot {
        std::mutex mutex;
        std::condition_variable ready;
        uint64_t seq = 0;
        uint64_t send_ns = 0;  // the reply's own copy of its request's send time
        uint64_t reply_ns = 0; // at the callback
        uint64_t seen_ns = 0;  // at the end of the poll cycle that took it, 0 until then
    };

    // --phase random (poll only): this thread polls without pause - spin_some() and the sleep, as a real
    // application's loop would - while a sender thread publishes on its own clock: the k-th ping at start + k * -i
    // plus a random offset of up to phase_jitter_max_ns (at most half of -i), so the rate is -i's on average.
    //
    // The offset is what makes the phase random. Without it (the first sketch, 2026-09-27), the sender slept -i
    // after the poll thread woke it at the end of a cycle; 20 ms later the publish fell at the same place in the
    // loop's ~157 us cycle each time, and rmw_phase_check.sh measured chi2 287 against uniform. An offset spread
    // over ~30 cycles leaves each phase bin within a few percent of the others.
    constexpr uint64_t phase_jitter_max_ns = 5000000ULL; // 5 ms
    template <typename T>
    auto run_ping_random(const rclcpp::Node::SharedPtr& node, rclcpp::executors::SingleThreadedExecutor& executor,
                         const rclcpp::QoS& qos, double interval_s, double duration_s, bool reliable,
                         const wait_mode& wait, pingpong::stamp_log& stamps) -> int {
        using Traits = BenchTraits<T>;
        auto pub = node->create_publisher<T>("ping", qos);
        reply_slot slot;
        bool took_reply = false; // the poll thread's own: a reply came in this cycle's spin_some()
        auto sub = node->create_subscription<T>("pong", qos, [&](const typename T::ConstSharedPtr& msg) -> void {
            const uint64_t callback_ns = now_ns();
            const std::lock_guard<std::mutex> lock(slot.mutex);
            slot.seq = Traits::seq(*msg);
            slot.send_ns = Traits::send_ns(*msg);
            slot.reply_ns = callback_ns;
            slot.seen_ns = 0;
            took_reply = true;
        });
        if (!wait_for_match(executor, pub, sub)) {
            return 1;
        }

        uint64_t transmitted = 0;
        rtt_stats rtt;
        loop_stats loop;
        phase_probe probe;
        std::vector<uint64_t> callback_after_send;
        std::atomic<bool> done {false};
        const uint64_t start = now_ns();
        const uint64_t deadline = start + static_cast<uint64_t>(duration_s * static_cast<double>(ns_per_s));
        const auto interval_ns = static_cast<uint64_t>(interval_s * static_cast<double>(ns_per_s));
        const uint64_t jitter_ns = std::min(phase_jitter_max_ns, interval_ns / 2);

        std::thread sender([&]() -> void {
            std::mt19937_64 random(start);
            std::uniform_int_distribution<uint64_t> offset(0, jitter_ns);
            uint64_t seq = 0;
            while (rclcpp::ok() && now_ns() < deadline) {
                // The k-th ping's own time, whatever the last round trip took; one already past goes at once.
                const uint64_t due = start + (seq * interval_ns) + offset(random);
                const struct timespec due_ts = {.tv_sec = static_cast<time_t>(due / ns_per_s),
                                                .tv_nsec = static_cast<long>(due % ns_per_s)};
                // NOLINTNEXTLINE(misc-include-cleaner) - <ctime>; now_ns() is CLOCK_MONOTONIC too
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due_ts, nullptr);
                T req;
                Traits::set_seq(req, ++seq);
                const uint64_t prev_check = probe.last_check_ns.load(std::memory_order_relaxed);
                const uint64_t sent_at = now_ns();
                Traits::set_send_ns(req, sent_at);
                probe.on_send(prev_check, sent_at);
                pub->publish(req);
                transmitted++;
                {
                    std::unique_lock<std::mutex> lock(slot.mutex);
                    const bool got = slot.ready.wait_for(lock, std::chrono::milliseconds(reply_wait_ms), [&]() -> bool {
                        return slot.seq == seq && slot.seen_ns != 0;
                    });
                    if (got) {
                        pingpong::stamp_log_add(stamps, seq, sent_at, slot.reply_ns);
                        callback_after_send.push_back(slot.reply_ns - sent_at);
                        const uint64_t end_ns = wait.rtt_at_callback ? slot.reply_ns : slot.seen_ns;
                        add_rtt(rtt, static_cast<double>(end_ns - slot.send_ns) / static_cast<double>(ns_per_ms));
                    }
                }
            }
            done = true;
        });

        while (!done) {
            loop.iterations++;
            const uint64_t spin_start = now_ns();
            probe.on_check(spin_start);
            executor.spin_some();
            const uint64_t sleep_start = now_ns();
            std::this_thread::sleep_for(std::chrono::microseconds(wait.poll_sleep_us));
            loop.spin_ns += sleep_start - spin_start;
            loop.sleep_ns += now_ns() - sleep_start;
            if (took_reply) {
                took_reply = false;
                const std::lock_guard<std::mutex> lock(slot.mutex);
                slot.seen_ns = now_ns();
                slot.ready.notify_all();
            }
        }
        sender.join();

        print_summary(rtt, transmitted, reliable, wait);
        print_loop_stats(loop, transmitted, wait);
        print_phase(probe, callback_after_send, wait);
        return 0;
    }

    template <typename T>
    auto run_ping(const rclcpp::Node::SharedPtr& node, double interval_s, double duration_s, bool reliable,
                  const wait_mode& wait, pingpong::stamp_log& stamps) -> int {
        using Traits = BenchTraits<T>;

        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);

        rclcpp::QoS qos(8);
        if (reliable) {
            qos.reliable().keep_last(8);
        } else {
            qos.best_effort();
        }
        if (wait.random_phase && !wait.blocking) {
            return run_ping_random<T>(node, executor, qos, interval_s, duration_s, reliable, wait, stamps);
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

        if (!wait_for_match(executor, pub, sub)) {
            return 1;
        }

        uint64_t transmitted = 0;
        rtt_stats rtt;
        loop_stats loop;
        phase_probe probe;
        std::vector<uint64_t> callback_after_send;

        uint64_t seq = 0;
        const uint64_t start = now_ns();
        const uint64_t deadline = start + static_cast<uint64_t>(duration_s * static_cast<double>(ns_per_s));
        const auto interval_ns = static_cast<uint64_t>(interval_s * static_cast<double>(ns_per_s));

        while (rclcpp::ok() && now_ns() < deadline) {
            T req;
            Traits::set_seq(req, ++seq);
            const uint64_t prev_check = probe.last_check_ns.load(std::memory_order_relaxed);
            const uint64_t sent_at = now_ns();
            Traits::set_send_ns(req, sent_at);
            probe.on_send(prev_check, sent_at);
            got_reply = false;
            pub->publish(req);
            transmitted++;

            const uint64_t wait_deadline = now_ns() + (reply_wait_ms * ns_per_ms); // 500ms, matching the native client
            wait_for_reply(executor, got_reply, wait_deadline, wait, loop, probe);
            if (got_reply && Traits::seq(reply_msg) == seq) {
                pingpong::stamp_log_add(stamps, seq, sent_at, reply_ns); // --stamps, whichever the wait mode
                callback_after_send.push_back(reply_ns - sent_at);
                const uint64_t end_ns = wait.blocking || wait.rtt_at_callback ? reply_ns : now_ns();
                add_rtt(rtt, static_cast<double>(end_ns - Traits::send_ns(reply_msg)) / static_cast<double>(ns_per_ms));
            }

            const struct timespec sleep_ts = {.tv_sec = static_cast<time_t>(interval_ns / ns_per_s),
                                              .tv_nsec = static_cast<long>(interval_ns % ns_per_s)};
            nanosleep(&sleep_ts, nullptr); // NOLINT(misc-include-cleaner) - see now_ns()'s own <ctime> comment
        }

        print_summary(rtt, transmitted, reliable, wait);
        print_loop_stats(loop, transmitted, wait);
        if (!wait.blocking) {
            print_phase(probe, callback_after_send, wait);
        }

        return 0;
    }

} // namespace

auto main(int argc, char** argv) -> int {
    double interval_s = 1.0;
    double duration_s = default_duration_s;
    bool reliable = false;
    wait_mode wait;                    // --wait block|poll, --poll-sleep-us N
    const char* stamps_path = nullptr; // --stamps <file>, see pingpong::stamp_log
    const char* message = "bench";
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
            wait.blocking = std::strcmp(argv[++i], "block") == 0;
        } else if (std::strcmp(argv[i], "--phase") == 0 && i + 1 < argc) {
            wait.random_phase = std::strcmp(argv[++i], "random") == 0;
        } else if (std::strcmp(argv[i], "--rtt-at") == 0 && i + 1 < argc) {
            wait.rtt_at_callback = std::strcmp(argv[++i], "callback") == 0;
        } else if (std::strcmp(argv[i], "--poll-sleep-us") == 0 && i + 1 < argc) {
            wait.poll_sleep_us = std::atoi(argv[++i]);
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
            ret = run_ping<rmw_perf_pingpong::msg::Bench>(node, interval_s, duration_s, reliable, wait, stamps);
        } else if (std::strcmp(message, "array1k") == 0) {
            ret = run_ping<rmw_perf_pingpong::msg::Array1k>(node, interval_s, duration_s, reliable, wait, stamps);
        } else if (std::strcmp(message, "struct16") == 0) {
            ret = run_ping<rmw_perf_pingpong::msg::Struct16>(node, interval_s, duration_s, reliable, wait, stamps);
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
