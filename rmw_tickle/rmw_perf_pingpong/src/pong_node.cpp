// rmw_tickle/COMPARISON.MD's "Design: rmw_perf_pingpong" - pong role. Subscriber on "ping",
// publisher on "pong" - republishes each received sample unmodified (mirrors examples/perf_hil's
// own native server.c: an echo, not a transform, so ping_node's own RTT calculation is measuring
// this round trip and nothing else).
//
// -m <bench|array1k|struct16> (payload-size expansion, see ping_node.cpp's own header comment for
// the full 'why') - a pure echo needs no type-specific field access at all, so this stays a plain
// function template instantiated per message type rather than needing ping_node.cpp's own
// BenchTraits<T>.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>

#include <rclcpp/rclcpp.hpp>

#include "pingpong_common.hpp"
#include "rmw_perf_pingpong/msg/array1k.hpp"
#include "rmw_perf_pingpong/msg/bench.hpp"
#include "rmw_perf_pingpong/msg/struct16.hpp"

namespace {

    // See ping_node.cpp's own identical helper for why these three are needed - the /parameter_events
    // gap this doesn't work around still applies here too (any rclcpp::Node using rmw_tickle needs
    // the same rcl_interfaces overlay workaround this tool's own README/COMPARISON.MD entry
    // documents).
    auto default_node_options() -> rclcpp::NodeOptions {
        return rclcpp::NodeOptions()
            .start_parameter_services(false)
            .start_parameter_event_publisher(false)
            .enable_rosout(false)
            .parameter_overrides({rclcpp::Parameter("start_type_description_service", false)});
    }

    // --stamps: per sample, when the ping reached this callback and when publish() of the echo returned.
    template <typename T>
    auto run_pong(const rclcpp::Node::SharedPtr& node, bool reliable, pingpong::stamp_log& stamps) -> void {
        rclcpp::QoS qos(8);
        if (reliable) {
            qos.reliable().keep_last(8);
        } else {
            qos.best_effort();
        }

        auto pub = node->create_publisher<T>("pong", qos);
        auto sub =
            node->create_subscription<T>("ping", qos, [pub, &stamps](const typename T::ConstSharedPtr& msg) -> void {
                const uint64_t callback_ns = pingpong::now_ns();
                pub->publish(*msg);
                pingpong::stamp_log_add(stamps, pingpong::BenchTraits<T>::seq(*msg), callback_ns, pingpong::now_ns());
            });

        rclcpp::spin(node);
    }

} // namespace

auto main(int argc, char** argv) -> int {
    bool reliable = false;
    const char* message = "bench";
    const char* stamps_path = nullptr; // --stamps <file>, see pingpong::stamp_log
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--reliable") == 0) {
            reliable = true;
        } else if (std::strcmp(argv[i], "--stamps") == 0 && i + 1 < argc) {
            stamps_path = argv[++i];
        } else if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            message = argv[++i];
        }
    }

    // See ping_node.cpp's own identical try/catch comment - the same rclcpp-internals-can-throw
    // reasoning applies here (create_publisher()'s own QoS-override parameter declaration).
    try {
        rclcpp::init(argc, argv);
        auto node = std::make_shared<rclcpp::Node>("pong_node", default_node_options());
        pingpong::stamp_log stamps;
        pingpong::stamp_log_open(stamps, stamps_path, "pong: seq callback_ns publish_return_ns (CLOCK_MONOTONIC ns)");

        if (std::strcmp(message, "bench") == 0) {
            run_pong<rmw_perf_pingpong::msg::Bench>(node, reliable, stamps);
        } else if (std::strcmp(message, "array1k") == 0) {
            run_pong<rmw_perf_pingpong::msg::Array1k>(node, reliable, stamps);
        } else if (std::strcmp(message, "struct16") == 0) {
            run_pong<rmw_perf_pingpong::msg::Struct16>(node, reliable, stamps);
        } else {
            std::fprintf(stderr, "unknown -m '%s' (expected bench|array1k|struct16)\n", message);
            rclcpp::shutdown();
            return 1;
        }

        // spin() returned: the run is over (SIGINT), so the stamps go to disk now, off the timed path.
        const bool written = pingpong::stamp_log_write(stamps);
        if (!written) {
            std::fprintf(stderr, "cannot write --stamps file %s\n", stamps_path);
        }
        rclcpp::shutdown();
        return written ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unhandled exception: %s\n", e.what());
        return 1;
    }
}
