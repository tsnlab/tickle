// RMW_PERF_PLAN.md 12: to_tickle / from_tickle of a large primitive sequence (through the tickle_cpp typesupport
// handle, as rmw_tickle calls them), a memcpy of the same bytes, and rclcpp::Serialization under whatever
// RMW_IMPLEMENTATION is set. Prints the converter library actually loaded (dladdr), which is the identity check
// between the -O0 and -O2 overlays.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp/serialization.hpp>
#include <rosidl_typesupport_cpp/message_type_support.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <std_msgs/msg/byte_multi_array.hpp>

#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_cpp/identifier.h"

static double now_us() {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <typename T> static void run(const char* name, T& msg, size_t bytes, int rounds, int calls) {
    const rosidl_message_type_support_t* ts = rosidl_typesupport_cpp::get_message_type_support_handle<T>();
    const rosidl_message_type_support_t* h =
        get_message_typesupport_handle(ts, rosidl_typesupport_tickle_cpp__identifier);
    auto* cb = static_cast<const rosidl_typesupport_tickle_c_message_callbacks_t*>(h->data);
    Dl_info info {};
    dladdr(reinterpret_cast<void*>(cb->to_tickle), &info);
    void* tickle = std::aligned_alloc(8, (cb->tickle_struct_size + 7) / 8 * 8);
    T back;
    std::vector<uint8_t> a(bytes, 0x5a), b(bytes);
    rclcpp::Serialization<T> ser;
    std::vector<double> to_v, from_v, cp_v, ser_v, des_v;
    for (int r = 0; r < rounds; r++) {
        double t0 = now_us();
        for (int i = 0; i < calls; i++)
            if (!cb->to_tickle(&msg, tickle)) {
                std::printf("to_tickle FAILED\n");
                return;
            }
        double t1 = now_us();
        for (int i = 0; i < calls; i++)
            if (!cb->from_tickle(tickle, &back)) {
                std::printf("from_tickle FAILED\n");
                return;
            }
        double t2 = now_us();
        for (int i = 0; i < calls; i++) {
            std::memcpy(b.data(), a.data(), bytes);
            asm volatile("" : : "r"(b.data()) : "memory");
        }
        double t3 = now_us();
        rclcpp::SerializedMessage sm;
        double t4 = now_us();
        for (int i = 0; i < calls; i++)
            ser.serialize_message(&msg, &sm);
        double t5 = now_us();
        for (int i = 0; i < calls; i++)
            ser.deserialize_message(&sm, &back);
        double t6 = now_us();
        to_v.push_back((t1 - t0) / calls);
        from_v.push_back((t2 - t1) / calls);
        cp_v.push_back((t3 - t2) / calls);
        ser_v.push_back((t5 - t4) / calls);
        des_v.push_back((t6 - t5) / calls);
    }
    bool same =
        std::memcmp(back.data.data(), msg.data.data(), msg.data.size()) == 0 && back.data.size() == msg.data.size();
    std::printf(
        "RESULT: type=%s bytes=%zu rmw=%s to_tickle_us=%.3f from_tickle_us=%.3f memcpy_us=%.3f serialize_us=%.3f "
        "deserialize_us=%.3f roundtrip_ok=%d lib=%s\n",
        name, bytes, std::getenv("RMW_IMPLEMENTATION") ? std::getenv("RMW_IMPLEMENTATION") : "default", median(to_v),
        median(from_v), median(cp_v), median(ser_v), median(des_v), same ? 1 : 0,
        info.dli_fname ? info.dli_fname : "?");
    std::free(tickle);
}

int main(int argc, char** argv) {
    int rounds = argc > 1 ? std::atoi(argv[1]) : 20, calls = argc > 2 ? std::atoi(argv[2]) : 2000;
    sensor_msgs::msg::Image img;
    img.height = 1;
    img.width = 64000;
    img.encoding = "mono8";
    img.step = 64000;
    img.data.assign(64000, 0x42);
    for (size_t i = 0; i < img.data.size(); i++)
        img.data[i] = static_cast<uint8_t>(i * 7);
    run("Image", img, img.data.size(), rounds, calls);
    std_msgs::msg::ByteMultiArray bma;
    bma.data.assign(16384, 0x24);
    for (size_t i = 0; i < bma.data.size(); i++)
        bma.data[i] = static_cast<uint8_t>(i * 3);
    run("ByteMultiArray", bma, bma.data.size(), rounds, calls);
    return 0;
}
