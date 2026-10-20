// The L4 round trip, receiver side - see large_rtt_tx.cpp. Subscribes to "l4_large" with the cell's QoS and, for every
// sample it takes, publishes an Ack carrying the sample's id on "l4_ack" (RELIABLE KEEP_LAST 10) through the same rmw.
// It reads no clock for the round trip, which is the sender's alone. At SIGINT it prints one RX line - identity,
// counts, and this process's CPU over the scheduled samples: cpu_window_ms from the first scheduled sample's callback
// to the last one's, cpu_window_takes the samples taken after the first (each one's take, callback and Ack lie inside),
// and maxrss_kb the peak RSS.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rmw/rmw.h>

#include "pingpong_common.hpp"
#include "rmw_perf_pingpong/msg/ack.hpp"
#include "rmw_perf_pingpong/msg/array1k.hpp"
#include "rmw_perf_pingpong/msg/array1m.hpp"
#include "rmw_perf_pingpong/msg/array4m.hpp"

namespace {

    constexpr size_t ack_depth = 10;
    constexpr double ns_per_ms = 1e6;
    constexpr uint64_t max_tracked_id = 1U << 20U; // ids above this are counted, not checked for duplicates

    struct rx_counts {
        uint64_t recv = 0;
        uint64_t warmup = 0;
        uint64_t dup = 0;
        uint64_t untracked = 0;
        uint64_t max_id = 0;
        std::vector<bool> seen;
        bool window_open = false; // the first scheduled sample (id >= 1) was taken
        uint64_t cpu_first_ns = 0;
        uint64_t cpu_last_ns = 0;
        uint64_t window_takes = 0; // scheduled samples taken after the first
    };

    // Called after a scheduled sample's Ack is published: process CPU time at the end of its callback.
    auto count_cpu(rx_counts& counts) -> void {
        const uint64_t cpu = pingpong::process_cpu_ns();
        if (!counts.window_open) {
            counts.window_open = true;
            counts.cpu_first_ns = cpu;
        } else {
            counts.window_takes++;
        }
        counts.cpu_last_ns = cpu;
    }

    auto count_take(rx_counts& counts, uint64_t id) -> void {
        counts.recv++;
        if (id == 0) {
            counts.warmup++;
            return;
        }
        counts.max_id = std::max(counts.max_id, id);
        if (id > max_tracked_id) {
            counts.untracked++;
            return;
        }
        if (counts.seen.size() <= id) {
            counts.seen.resize((id + 1) * 2, false);
        }
        if (counts.seen[id]) {
            counts.dup++;
        }
        counts.seen[id] = true;
    }

    template <typename T>
    auto run_rx(const rclcpp::Node::SharedPtr& node, bool reliable, size_t depth, rx_counts& counts) -> void {
        rclcpp::QoS qos(depth);
        qos.keep_last(depth);
        if (reliable) {
            qos.reliable();
        } else {
            qos.best_effort();
        }
        rclcpp::QoS ack_qos(ack_depth);
        ack_qos.reliable().keep_last(ack_depth);
        auto pub = node->create_publisher<rmw_perf_pingpong::msg::Ack>("l4_ack", ack_qos);
        auto sub = node->create_subscription<T>("l4_large", qos,
                                                [pub, &counts](const typename T::ConstSharedPtr& msg) -> void {
                                                    rmw_perf_pingpong::msg::Ack ack;
                                                    ack.id = msg->id;
                                                    pub->publish(ack);
                                                    count_take(counts, msg->id);
                                                    if (msg->id != 0) {
                                                        count_cpu(counts);
                                                    }
                                                });
        rclcpp::spin(node);
    }

    auto default_node_options() -> rclcpp::NodeOptions {
        return rclcpp::NodeOptions()
            .start_parameter_services(false)
            .start_parameter_event_publisher(false)
            .enable_rosout(false)
            .parameter_overrides({rclcpp::Parameter("start_type_description_service", false)});
    }

    auto run_message(const char* message, const rclcpp::Node::SharedPtr& node, bool reliable, size_t depth,
                     rx_counts& counts) -> bool {
        if (std::strcmp(message, "array1m") == 0) {
            run_rx<rmw_perf_pingpong::msg::Array1m>(node, reliable, depth, counts);
        } else if (std::strcmp(message, "array4m") == 0) {
            run_rx<rmw_perf_pingpong::msg::Array4m>(node, reliable, depth, counts);
        } else if (std::strcmp(message, "array1k") == 0) {
            run_rx<rmw_perf_pingpong::msg::Array1k>(node, reliable, depth, counts);
        } else if (std::strcmp(message, "ack") == 0) {
            run_rx<rmw_perf_pingpong::msg::Ack>(node, reliable, depth, counts);
        } else {
            std::fprintf(stderr, "unknown -m '%s' (expected array1m|array4m|array1k|ack)\n", message);
            return false;
        }
        return true;
    }

} // namespace

auto main(int argc, char** argv) -> int {
    const char* message = "array1m";
    bool reliable = true;
    size_t depth = ack_depth;
    for (int i = 1; i < argc; i++) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "-m") == 0 && has_value) {
            message = argv[++i];
        } else if (std::strcmp(argv[i], "--qos") == 0 && has_value) {
            reliable = std::strcmp(argv[++i], "reliable") == 0;
        } else if (std::strcmp(argv[i], "--depth") == 0 && has_value) {
            depth = static_cast<size_t>(std::atoi(argv[++i]));
        }
    }
    try {
        rclcpp::init(argc, argv);
        auto node = std::make_shared<rclcpp::Node>("large_rtt_rx", default_node_options());
        rx_counts counts;
        const bool known = run_message(message, node, reliable, depth, counts);
        // spin() returned: SIGINT ended the run.
        std::printf("RX: framework=%s msg=%s qos=%s depth=%zu recv=%llu warmup=%llu dup=%llu untracked=%llu "
                    "max_id=%llu cpu_window_ms=%.3f cpu_window_takes=%llu maxrss_kb=%ld\n",
                    rmw_get_implementation_identifier(), message, reliable ? "reliable" : "best_effort", depth,
                    static_cast<unsigned long long>(counts.recv), static_cast<unsigned long long>(counts.warmup),
                    static_cast<unsigned long long>(counts.dup), static_cast<unsigned long long>(counts.untracked),
                    static_cast<unsigned long long>(counts.max_id),
                    static_cast<double>(counts.cpu_last_ns - counts.cpu_first_ns) / ns_per_ms,
                    static_cast<unsigned long long>(counts.window_takes), pingpong::peak_rss_kb());
        std::fflush(stdout);
        rclcpp::shutdown();
        return known ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unhandled exception: %s\n", e.what());
        return 1;
    }
}
