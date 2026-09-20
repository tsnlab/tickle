// rmw_tickle/comparison.md's "Design: rmw_perf_pingpong" - pong role. Subscriber on "ping",
// publisher on "pong" - republishes each received sample unmodified (mirrors examples/perf_hil's
// own native server.c: an echo, not a transform, so ping_node's own RTT calculation is measuring
// this round trip and nothing else).
#include <cstring>

#include <rclcpp/rclcpp.hpp>

#include "rmw_perf_pingpong/msg/bench.hpp"

using rmw_perf_pingpong::msg::Bench;

namespace {

    // See ping_node.cpp's own identical helper for why these three are needed - the /parameter_events
    // gap this doesn't work around still applies here too (any rclcpp::Node using rmw_tickle needs
    // the same rcl_interfaces overlay workaround this tool's own README/comparison.md entry
    // documents).
    rclcpp::NodeOptions default_node_options() {
        return rclcpp::NodeOptions()
            .start_parameter_services(false)
            .start_parameter_event_publisher(false)
            .enable_rosout(false)
            .parameter_overrides({rclcpp::Parameter("start_type_description_service", false)});
    }

} // namespace

int main(int argc, char** argv) {
    bool reliable = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--reliable") == 0) {
            reliable = true;
        }
    }

    rclcpp::init(argc, argv);
    auto node = std::make_shared<rclcpp::Node>("pong_node", default_node_options());

    rclcpp::QoS qos(8);
    if (reliable) {
        qos.reliable().keep_last(8);
    } else {
        qos.best_effort();
    }

    auto pub = node->create_publisher<Bench>("pong", qos);
    auto sub = node->create_subscription<Bench>("ping", qos, [pub](const Bench::SharedPtr msg) {
        pub->publish(*msg);
    });

    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
