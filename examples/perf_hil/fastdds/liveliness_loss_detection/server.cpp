/*
 * HIL 3-way QoS-matrix comparison, scenario "liveliness_loss_detection" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) subscriber role. See the CycloneDDS twin's own doc comment for the full
 * mechanism and, crucially, why both timestamps compared here (last received sample, loss
 * detected) are taken on this same host's own clock - never against the writer's own kill time on
 * a different host, the exact cross-host clock-sync pitfall this exercise's own earlier work
 * ("Item 5" in COMPARISON.MD) got burned by once already.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fastdds/dds/core/status/LivelinessChangedStatus.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastrtps/utils/TimeConversion.h>

#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int) {
    g_interrupted = 1;
}

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t g_last_recv_ns = 0;
static uint64_t g_loss_detect_ns = 0;
static bool g_loss_detected = false;

class LivelinessListener : public DataReaderListener {
  public:
    void on_liveliness_changed(DataReader* reader, const LivelinessChangedStatus& status) override {
        (void)reader;
        if (status.not_alive_count > 0 && !g_loss_detected) {
            g_loss_detected = true;
            g_loss_detect_ns = now_ns();
        }
    }
    void on_data_available(DataReader* reader) override {
        Bench sample;
        SampleInfo info;
        while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
            if (info.valid_data) {
                received++;
                g_last_recv_ns = now_ns();
            }
        }
    }
    uint64_t received = 0;
};

int main(int argc, char** argv) {
    double lease_s = 2.0;
    double safety_cap_s = 30.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            lease_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }

    std::signal(SIGINT, handle_sigint);

    DomainParticipant* participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (!participant) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }

    TypeSupport type(new BenchPubSubType());
    type.register_type(participant);

    Topic* topic = participant->create_topic("stream", "Bench", TOPIC_QOS_DEFAULT);

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.liveliness().kind = AUTOMATIC_LIVELINESS_QOS;
    rqos.liveliness().lease_duration = eprosima::fastrtps::Duration_t(lease_s);
    // See client.cpp's own doc comment for why this is required, not a tuning choice.
    rqos.liveliness().announcement_period = eprosima::fastrtps::Duration_t(lease_s / 3.0);

    static LivelinessListener listener;
    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* reader = subscriber->create_datareader(topic, rqos, &listener);
    if (!reader) {
        fprintf(stderr, "create_datareader failed\n");
        return 1;
    }

    // Match-wait, this exercise's own established FastDDS idiom.
    {
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        bool matched = false;
        for (;;) {
            eprosima::fastdds::dds::SubscriptionMatchedStatus status;
            reader->get_subscription_matched_status(status);
            if (status.current_count > 0) {
                matched = true;
                break;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
            if (elapsed >= 10.0) {
                break;
            }
            struct timespec poll_interval = {0, 50 * 1000 * 1000};
            nanosleep(&poll_interval, nullptr);
        }
        if (!matched) {
            fprintf(stderr, "timed out waiting for a matched writer\n");
            return 1;
        }
    }

    uint64_t deadline = now_ns() + (uint64_t)(safety_cap_s * 1e9);
    while (!g_interrupted && now_ns() < deadline && !g_loss_detected) {
        struct timespec poll_interval = {0, 50 * 1000 * 1000};
        nanosleep(&poll_interval, nullptr);
    }

    // A brief real settle window - the loss listener may fire a few ms after this poll loop's own
    // last check.
    struct timespec settle = {0, 300 * 1000 * 1000};
    nanosleep(&settle, nullptr);

    double detect_latency_ms =
        (g_loss_detected && g_last_recv_ns > 0) ? (double)(g_loss_detect_ns - g_last_recv_ns) / 1e6 : -1.0;

    printf("RESULT: framework=fastdds scenario=liveliness_loss_detection role=server recv=%lu "
           "loss_detected=%d detect_latency_ms=%.3f lease_s=%.3f\n",
           (unsigned long)listener.received, g_loss_detected, detect_latency_ms, lease_s);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
