/*
 * HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (rmw_tickle/comparison.md) -
 * FastDDS native (no rmw) subscriber role. See the CycloneDDS twin's own doc comment for the full
 * mechanism - this mirrors it exactly: deliberately stalls its own consumption for `-p` seconds
 * right after matching, before ever creating the receive loop, while the writer (client.cpp) keeps
 * writing on a fixed schedule regardless.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
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

int main(int argc, char** argv) {
    double pause_s = 3.0;
    double safety_cap_s = 30.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pause_s = atof(argv[++i]);
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
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = 8;

    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* reader = subscriber->create_datareader(topic, rqos);
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
            SubscriptionMatchedStatus status;
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

    printf("Subscriber: matched, now deliberately stalling for %.1fs before consuming anything\n", pause_s);
    struct timespec pause_ts = {(time_t)pause_s, (long)((pause_s - (time_t)pause_s) * 1e9)};
    nanosleep(&pause_ts, nullptr);

    // last_seq starts at 0, not the first received sample's own seq (2026-09-20, real measurement
    // bug - see the CycloneDDS twin's own doc comment for the full story: it silently hid this
    // scenario's own real reader-side HISTORY eviction loss during the pause window).
    uint64_t received = 0, lost = 0;
    uint32_t last_seq = 0;

    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(safety_cap_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        eprosima::fastrtps::Duration_t timeout{1, 0};
        if (!reader->wait_for_unread_message(timeout)) {
            continue;
        }
        Bench sample;
        SampleInfo info;
        while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
            if (!info.valid_data) {
                continue;
            }
            uint32_t seq = sample.seq();
            if (seq > last_seq + 1) {
                lost += (seq - last_seq - 1);
                last_seq = seq;
            } else {
                last_seq = seq;
            }
            received++;
        }
    }

    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;

    printf("RESULT: framework=fastdds scenario=history_depth_burst_loss role=server pause_s=%.1f "
           "recv=%lu lost=%lu loss_pct=%.1f\n",
           pause_s, (unsigned long)received, (unsigned long)lost, loss_pct);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
