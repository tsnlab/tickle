#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <chrono>

using namespace std::chrono_literals;

class TickLETestNode : public rclcpp::Node
{
public:
    TickLETestNode() : Node("tickle_test_node")
    {
        // Create publisher
        publisher_ = this->create_publisher<std_msgs::msg::String>("test_topic", 10);

        // Create subscription
        subscription_ = this->create_subscription<std_msgs::msg::String>(
            "test_topic", 10,
            std::bind(&TickLETestNode::topic_callback, this, std::placeholders::_1));

        // Create timer to publish messages
        timer_ = this->create_wall_timer(
            1s, std::bind(&TickLETestNode::timer_callback, this));

        RCLCPP_INFO(this->get_logger(), "TickLE test node started");
    }

private:
    void timer_callback()
    {
        auto message = std_msgs::msg::String();
        message.data = "Hello from TickLE! " + std::to_string(counter_++);
        RCLCPP_INFO(this->get_logger(), "Publishing: '%s'", message.data.c_str());
        publisher_->publish(message);
    }

    void topic_callback(const std_msgs::msg::String::SharedPtr msg)
    {
        RCLCPP_INFO(this->get_logger(), "Received: '%s'", msg->data.c_str());
    }

    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr publisher_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr subscription_;
    rclcpp::TimerBase::SharedPtr timer_;
    int counter_ = 0;
};

int main(int argc, char * argv[])
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<TickLETestNode>());
    rclcpp::shutdown();
    return 0;
}
