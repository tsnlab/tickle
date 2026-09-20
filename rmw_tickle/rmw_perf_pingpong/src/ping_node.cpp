// rmw_tickle/comparison.md's "Design: rmw_perf_pingpong" - ping role. Publisher on "ping",
// subscriber on "pong" - mirrors examples/perf_hil/cyclonedds/best_effort_latency/client.c's own
// shape exactly (same CLI flags, same blocking send-then-wait-for-echo pattern, same RESULT line)
// so the two tracks (native no-rmw HIL, this rmw-layer tool) stay directly comparable.
//
// RTT is computed entirely against this process's own now_ns() - never a timestamp read on a
// different machine - so it needs no cross-host clock agreement at all (comparison.md's own
// retracted item 5 attempt mixed two independently-clocked machines' timestamps; this tool never
// does that).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <rclcpp/rclcpp.hpp>

#include "rmw_perf_pingpong/msg/bench.hpp"

using rmw_perf_pingpong::msg::Bench;

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

} // namespace

int main(int argc, char** argv) {
    double interval_s = 1.0;
    double duration_s = 10.0;
    bool reliable = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--reliable") == 0) {
            reliable = true;
        }
    }

    rclcpp::init(argc, argv);
    auto node = std::make_shared<rclcpp::Node>("ping_node", default_node_options());
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);

    rclcpp::QoS qos(8);
    if (reliable) {
        qos.reliable().keep_last(8);
    } else {
        qos.best_effort();
    }

    auto pub = node->create_publisher<Bench>("ping", qos);

    std::atomic<bool> got_reply {false};
    Bench reply_msg;
    auto sub = node->create_subscription<Bench>("pong", qos, [&](const Bench::SharedPtr msg) {
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
            rclcpp::shutdown();
            return 1;
        }
    }

    uint64_t transmitted = 0, received = 0;
    double rtt_min_ms = -1.0, rtt_max_ms = 0.0, rtt_sum_ms = 0.0;

    uint32_t seq = 0;
    uint64_t start = now_ns();
    uint64_t deadline = start + static_cast<uint64_t>(duration_s * 1e9);
    uint64_t interval_ns = static_cast<uint64_t>(interval_s * 1e9);

    while (rclcpp::ok() && now_ns() < deadline) {
        Bench req;
        req.seq = ++seq;
        req.send_ns = now_ns();
        got_reply = false;
        pub->publish(req);
        transmitted++;

        uint64_t wait_deadline = now_ns() + 500ULL * 1000000ULL; // 500ms, matching the native client
        while (rclcpp::ok() && !got_reply && now_ns() < wait_deadline) {
            executor.spin_some();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        if (got_reply && reply_msg.seq == req.seq) {
            double rtt_ms = static_cast<double>(now_ns() - reply_msg.send_ns) / 1e6;
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
    double loss_pct = transmitted > 0 ? (100.0 * static_cast<double>(lost) / static_cast<double>(transmitted)) : 0.0;
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

    rclcpp::shutdown();
    return 0;
}
