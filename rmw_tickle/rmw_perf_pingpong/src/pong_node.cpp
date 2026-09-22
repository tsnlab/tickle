// rmw_tickle/COMPARISON.MD's "Design: rmw_perf_pingpong" - pong role. Subscriber on "ping",
// publisher on "pong" - republishes each received sample unmodified (mirrors examples/perf_hil's
// own native server.c: an echo, not a transform, so ping_node's own RTT calculation is measuring
// this round trip and nothing else).
//
// -m <bench|array1k|struct16> (payload-size expansion, see ping_node.cpp's own header comment for
// the full 'why') - a pure echo needs no type-specific field access at all, so this stays a plain
// function template instantiated per message type rather than needing ping_node.cpp's own
// BenchTraits<T>.
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>

#include <rclcpp/rclcpp.hpp>

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

    template <typename T> auto run_pong(const rclcpp::Node::SharedPtr& node, bool reliable) -> void {
        rclcpp::QoS qos(8);
        if (reliable) {
            qos.reliable().keep_last(8);
        } else {
            qos.best_effort();
        }

        auto pub = node->create_publisher<T>("pong", qos);
        auto sub = node->create_subscription<T>("ping", qos, [pub](const typename T::ConstSharedPtr& msg) -> void {
            pub->publish(*msg);
        });

        rclcpp::spin(node);
    }

} // namespace

auto main(int argc, char** argv) -> int {
    bool reliable = false;
    const char* message = "bench";
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--reliable") == 0) {
            reliable = true;
        } else if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            message = argv[++i];
        }
    }

    // See ping_node.cpp's own identical try/catch comment - the same rclcpp-internals-can-throw
    // reasoning applies here (create_publisher()'s own QoS-override parameter declaration).
    try {
        rclcpp::init(argc, argv);
        auto node = std::make_shared<rclcpp::Node>("pong_node", default_node_options());

        if (std::strcmp(message, "bench") == 0) {
            run_pong<rmw_perf_pingpong::msg::Bench>(node, reliable);
        } else if (std::strcmp(message, "array1k") == 0) {
            run_pong<rmw_perf_pingpong::msg::Array1k>(node, reliable);
        } else if (std::strcmp(message, "struct16") == 0) {
            run_pong<rmw_perf_pingpong::msg::Struct16>(node, reliable);
        } else {
            std::fprintf(stderr, "unknown -m '%s' (expected bench|array1k|struct16)\n", message);
            rclcpp::shutdown();
            return 1;
        }

        rclcpp::shutdown();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unhandled exception: %s\n", e.what());
        return 1;
    }
}
