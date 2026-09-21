// rmw_tickle/comparison.md's "Design: rmw_perf_pingpong" - ping role. Publisher on "ping",
// subscriber on "pong" - mirrors examples/perf_hil/cyclonedds/best_effort_latency/client.c's own
// shape exactly (same CLI flags, same blocking send-then-wait-for-echo pattern, same RESULT line)
// so the two tracks (native no-rmw HIL, this rmw-layer tool) stay directly comparable.
//
// RTT is computed entirely against this process's own now_ns() - never a timestamp read on a
// different machine - so it needs no cross-host clock agreement at all (comparison.md's own
// retracted item 5 attempt mixed two independently-clocked machines' timestamps; this tool never
// does that).
//
// -m <bench|array1k|struct16> (payload-size expansion, comparison.md's own priority list): `Bench`
// stays the default (64-byte payload, matches examples/perf_hil/idl/Bench.idl exactly, for the
// native-HIL-vs-rmw-layer comparison this tool was originally built for); `array1k`/`struct16`
// give this same single-clock-RTT-correct methodology the same message shapes buildfarm_perf_
// tests' own Array1k.msg/Struct16.msg already use (msg/Array1k.msg's own header comment explains
// why they're a deliberate copy here, not a real dependency on that upstream package) - directly
// comparable against comparison.md's own existing "Performance comparison" section's numbers,
// same wire layout. Templated on the message type (BenchTraits<T> below) rather than three near-
// duplicate copies of this whole file - pong_node.cpp needs no type-specific logic at all (a pure
// echo), only ping_node.cpp's own sequence-id/send-timestamp field access differs per type.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <rclcpp/rclcpp.hpp>

#include "rmw_perf_pingpong/msg/array1k.hpp"
#include "rmw_perf_pingpong/msg/bench.hpp"
#include "rmw_perf_pingpong/msg/struct16.hpp"

namespace {

    uint64_t now_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
    }

    // Every rclcpp::Node on ROS 2 Jazzy unconditionally subscribes to /parameter_events (its own
    // internal NodeTimeSource, for use_sim_time monitoring - no NodeOptions flag disables it) and
    // creates a ~/get_type_description service - neither of which this tool needs. The type
    // description service is opted out of via a default parameter override here (so this binary
    // works standalone, without needing --ros-args on every invocation); /parameter_events itself
    // needs rmw_tickle to actually have typesupport for rcl_interfaces/msg/ParameterEvent, which is
    // a separate, real compatibility gap (comparison.md's own item 5 write-up) - not something this
    // tool's own code can work around.
    rclcpp::NodeOptions default_node_options() {
        return rclcpp::NodeOptions()
            .start_parameter_services(false)
            .start_parameter_event_publisher(false)
            .enable_rosout(false)
            .parameter_overrides({rclcpp::Parameter("start_type_description_service", false)});
    }

    // Per-message-type field access: Bench's own (seq, send_ns) versus Array1k's/Struct16's own
    // (id, time) - same role, different names/types (performance_test's own convention, matched
    // verbatim - see msg/Array1k.msg's own header comment), unified here so run_ping() below is
    // written once against BenchTraits<T> instead of three times against three field names.
    template <typename T> struct BenchTraits;

    template <> struct BenchTraits<rmw_perf_pingpong::msg::Bench> {
        static void set_seq(rmw_perf_pingpong::msg::Bench& m, uint64_t v) {
            m.seq = static_cast<uint32_t>(v);
        }
        static uint64_t seq(const rmw_perf_pingpong::msg::Bench& m) {
            return m.seq;
        }
        static void set_send_ns(rmw_perf_pingpong::msg::Bench& m, uint64_t v) {
            m.send_ns = v;
        }
        static uint64_t send_ns(const rmw_perf_pingpong::msg::Bench& m) {
            return m.send_ns;
        }
    };

    template <> struct BenchTraits<rmw_perf_pingpong::msg::Array1k> {
        static void set_seq(rmw_perf_pingpong::msg::Array1k& m, uint64_t v) {
            m.id = v;
        }
        static uint64_t seq(const rmw_perf_pingpong::msg::Array1k& m) {
            return m.id;
        }
        static void set_send_ns(rmw_perf_pingpong::msg::Array1k& m, uint64_t v) {
            m.time = static_cast<int64_t>(v);
        }
        static uint64_t send_ns(const rmw_perf_pingpong::msg::Array1k& m) {
            return static_cast<uint64_t>(m.time);
        }
    };

    template <> struct BenchTraits<rmw_perf_pingpong::msg::Struct16> {
        static void set_seq(rmw_perf_pingpong::msg::Struct16& m, uint64_t v) {
            m.id = v;
        }
        static uint64_t seq(const rmw_perf_pingpong::msg::Struct16& m) {
            return m.id;
        }
        static void set_send_ns(rmw_perf_pingpong::msg::Struct16& m, uint64_t v) {
            m.time = static_cast<int64_t>(v);
        }
        static uint64_t send_ns(const rmw_perf_pingpong::msg::Struct16& m) {
            return static_cast<uint64_t>(m.time);
        }
    };

    template <typename T>
    int run_ping(rclcpp::Node::SharedPtr node, double interval_s, double duration_s, bool reliable) {
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
        auto sub = node->create_subscription<T>("pong", qos, [&](const typename T::SharedPtr msg) {
            reply_msg = *msg;
            got_reply = true;
        });

        // Give discovery a moment - a send before both sides have matched would just be lost,
        // undercounting "sent" for no real reason (same reasoning as the native client's own
        // wait_for_writer_match()/wait_for_reader_match()).
        {
            uint64_t match_deadline = now_ns() + 10ULL * 1000000000ULL;
            while (rclcpp::ok() && now_ns() < match_deadline &&
                   (pub->get_subscription_count() == 0 || sub->get_publisher_count() == 0)) {
                executor.spin_some();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (pub->get_subscription_count() == 0 || sub->get_publisher_count() == 0) {
                std::fprintf(stderr, "timed out waiting for a match\n");
                return 1;
            }
        }

        uint64_t transmitted = 0, received = 0;
        double rtt_min_ms = -1.0, rtt_max_ms = 0.0, rtt_sum_ms = 0.0;

        uint64_t seq = 0;
        uint64_t start = now_ns();
        uint64_t deadline = start + static_cast<uint64_t>(duration_s * 1e9);
        uint64_t interval_ns = static_cast<uint64_t>(interval_s * 1e9);

        while (rclcpp::ok() && now_ns() < deadline) {
            T req;
            Traits::set_seq(req, ++seq);
            Traits::set_send_ns(req, now_ns());
            got_reply = false;
            pub->publish(req);
            transmitted++;

            uint64_t wait_deadline = now_ns() + 500ULL * 1000000ULL; // 500ms, matching the native client
            while (rclcpp::ok() && !got_reply && now_ns() < wait_deadline) {
                executor.spin_some();
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if (got_reply && Traits::seq(reply_msg) == seq) {
                double rtt_ms = static_cast<double>(now_ns() - Traits::send_ns(reply_msg)) / 1e6;
                received++;
                if (rtt_min_ms < 0.0 || rtt_ms < rtt_min_ms) {
                    rtt_min_ms = rtt_ms;
                }
                if (rtt_ms > rtt_max_ms) {
                    rtt_max_ms = rtt_ms;
                }
                rtt_sum_ms += rtt_ms;
            }

            struct timespec sleep_ts = {static_cast<time_t>(interval_ns / 1000000000ULL),
                                        static_cast<long>(interval_ns % 1000000000ULL)};
            nanosleep(&sleep_ts, nullptr);
        }

        uint64_t lost = transmitted - received;
        double loss_pct =
            transmitted > 0 ? (100.0 * static_cast<double>(lost) / static_cast<double>(transmitted)) : 0.0;
        double avg = received > 0 ? rtt_sum_ms / static_cast<double>(received) : 0.0;

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
        std::printf("RESULT: framework=%s scenario=pingpong qos=%s sent=%lu recv=%lu loss_pct=%.0f "
                    "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
                    rmw_impl, reliable ? "reliable" : "best_effort", static_cast<unsigned long>(transmitted),
                    static_cast<unsigned long>(received), loss_pct, rtt_min_ms, avg, rtt_max_ms);

        return 0;
    }

} // namespace

int main(int argc, char** argv) {
    double interval_s = 1.0;
    double duration_s = 10.0;
    bool reliable = false;
    const char* message = "bench";
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--reliable") == 0) {
            reliable = true;
        } else if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            message = argv[++i];
        }
    }

    rclcpp::init(argc, argv);
    auto node = std::make_shared<rclcpp::Node>("ping_node", default_node_options());

    int rc;
    if (std::strcmp(message, "bench") == 0) {
        rc = run_ping<rmw_perf_pingpong::msg::Bench>(node, interval_s, duration_s, reliable);
    } else if (std::strcmp(message, "array1k") == 0) {
        rc = run_ping<rmw_perf_pingpong::msg::Array1k>(node, interval_s, duration_s, reliable);
    } else if (std::strcmp(message, "struct16") == 0) {
        rc = run_ping<rmw_perf_pingpong::msg::Struct16>(node, interval_s, duration_s, reliable);
    } else {
        std::fprintf(stderr, "unknown -m '%s' (expected bench|array1k|struct16)\n", message);
        rc = 1;
    }

    rclcpp::shutdown();
    return rc;
}
