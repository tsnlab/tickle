// bundle10_rss: one role of a scenario, in one process that hosts one rmw_tickle context. The residency of the
// context's tables is read by rss_tables_preload.so (LD_PRELOAD), not here; this node only produces the traffic.
//
//   rss_node ROLE MSG QOS RATE_HZ DUR_S
//     ROLE  idle | pub | sub | server | client
//     MSG   small (72 B) | mid (32 KiB, the fragment path) | large (1 MiB, the large-sample path); ignored by
//           idle/server/client (Echo.srv, 72 B request)
//     QOS   reliable | best_effort (KEEP_LAST 10)
// At the end it prints one line: "RSS_NODE role=... sent=... recv=..." - the scenario's reader voids a run whose
// receiving side counted nothing.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>

#include "rss_tables_rmw/msg/large.hpp"
#include "rss_tables_rmw/msg/mid.hpp"
#include "rss_tables_rmw/msg/small.hpp"
#include "rss_tables_rmw/srv/echo.hpp"

namespace {

    uint64_t g_sent = 0;
    uint64_t g_recv = 0;

    auto make_qos(bool reliable) -> rclcpp::QoS {
        rclcpp::QoS qos(10);
        qos.keep_last(10);
        if (reliable) {
            qos.reliable();
        } else {
            qos.best_effort();
        }
        return qos;
    }

    template <typename T>
    auto run_pub(const rclcpp::Node::SharedPtr& node, bool reliable, double rate, double dur) -> void {
        auto pub = node->create_publisher<T>("rss_topic", make_qos(reliable));
        auto msg = std::make_unique<T>();
        const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / rate));
        auto timer = node->create_wall_timer(period, [&]() -> void {
            msg->id = ++g_sent;
            pub->publish(*msg);
        });
        rclcpp::spin_until_future_complete(node, std::promise<void>().get_future(),
                                           std::chrono::milliseconds(static_cast<int64_t>(dur * 1000)));
    }

    template <typename T> auto run_sub(const rclcpp::Node::SharedPtr& node, bool reliable, double dur) -> void {
        auto sub = node->create_subscription<T>("rss_topic", make_qos(reliable),
                                                [](const typename T::ConstSharedPtr& /*msg*/) -> void {
                                                    g_recv++;
                                                });
        rclcpp::spin_until_future_complete(node, std::promise<void>().get_future(),
                                           std::chrono::milliseconds(static_cast<int64_t>(dur * 1000)));
    }

    auto run_server(const rclcpp::Node::SharedPtr& node, double dur) -> void {
        using Echo = rss_tables_rmw::srv::Echo;
        auto srv = node->create_service<Echo>(
            "rss_echo",
            [](const std::shared_ptr<Echo::Request>& req, const std::shared_ptr<Echo::Response>& res) -> void {
                res->id = req->id;
                g_recv++;
            });
        rclcpp::spin_until_future_complete(node, std::promise<void>().get_future(),
                                           std::chrono::milliseconds(static_cast<int64_t>(dur * 1000)));
    }

    auto run_client(const rclcpp::Node::SharedPtr& node, double rate, double dur) -> void {
        using Echo = rss_tables_rmw::srv::Echo;
        auto cli = node->create_client<Echo>("rss_echo");
        const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / rate));
        auto timer = node->create_wall_timer(period, [&]() -> void {
            if (!cli->service_is_ready()) {
                return;
            }
            auto req = std::make_shared<Echo::Request>();
            req->id = ++g_sent;
            cli->async_send_request(req, [](rclcpp::Client<Echo>::SharedFuture /*f*/) -> void {
                g_recv++;
            });
        });
        rclcpp::spin_until_future_complete(node, std::promise<void>().get_future(),
                                           std::chrono::milliseconds(static_cast<int64_t>(dur * 1000)));
    }

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: rss_node idle|pub|sub|server|client small|mid|large reliable|best_effort RATE DUR\n");
        return 2;
    }
    const std::string role = argv[1];
    const std::string msg = argv[2];
    const bool reliable = std::strcmp(argv[3], "reliable") == 0;
    const double rate = std::atof(argv[4]);
    const double dur = std::atof(argv[5]);
    rclcpp::init(1, argv);
    auto node = std::make_shared<rclcpp::Node>(
        "rss_" + role, rclcpp::NodeOptions()
                           .start_parameter_services(false)
                           .start_parameter_event_publisher(false)
                           .enable_rosout(false)
                           .parameter_overrides({rclcpp::Parameter("start_type_description_service", false)}));
    using rss_tables_rmw::msg::Large;
    using rss_tables_rmw::msg::Mid;
    using rss_tables_rmw::msg::Small;
    if (role == "idle") {
        rclcpp::spin_until_future_complete(node, std::promise<void>().get_future(),
                                           std::chrono::milliseconds(static_cast<int64_t>(dur * 1000)));
    } else if (role == "pub") {
        if (msg == "large") {
            run_pub<Large>(node, reliable, rate, dur);
        } else if (msg == "mid") {
            run_pub<Mid>(node, reliable, rate, dur);
        } else {
            run_pub<Small>(node, reliable, rate, dur);
        }
    } else if (role == "sub") {
        if (msg == "large") {
            run_sub<Large>(node, reliable, dur);
        } else if (msg == "mid") {
            run_sub<Mid>(node, reliable, dur);
        } else {
            run_sub<Small>(node, reliable, dur);
        }
    } else if (role == "server") {
        run_server(node, dur);
    } else if (role == "client") {
        run_client(node, rate, dur);
    } else {
        std::fprintf(stderr, "unknown role %s\n", role.c_str());
        return 2;
    }
    std::printf("RSS_NODE role=%s msg=%s sent=%llu recv=%llu\n", role.c_str(), msg.c_str(),
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_recv));
    std::fflush(stdout);
    node.reset();
    rclcpp::shutdown();
    return 0;
}
