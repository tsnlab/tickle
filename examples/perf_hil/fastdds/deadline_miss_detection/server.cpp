/*
 * HIL 3-way QoS-matrix comparison, scenario "deadline_miss_detection" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) subscriber role. See the CycloneDDS twin's own doc comment for the full
 * mechanism - mirrors client.cpp's own DEADLINE QoS exactly and independently monitors this side's
 * own RequestedDeadlineMissedStatus.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fastdds/dds/core/status/DeadlineMissedStatus.hpp>
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
    double deadline_s = 0.05;
    double safety_cap_s = 30.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            deadline_s = atof(argv[++i]);
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
    rqos.deadline().period = eprosima::fastrtps::Duration_t(deadline_s);

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

    uint64_t received = 0;

    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(safety_cap_s * 1e9);
    while (!g_interrupted && now_ns() < deadline) {
        eprosima::fastrtps::Duration_t timeout {0, 200 * 1000 * 1000};
        if (reader->wait_for_unread_message(timeout)) {
            Bench sample;
            SampleInfo info;
            while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
                if (info.valid_data) {
                    received++;
                }
            }
        }
    }

    RequestedDeadlineMissedStatus final_status;
    reader->get_requested_deadline_missed_status(final_status);

    printf("RESULT: framework=fastdds scenario=deadline_miss_detection role=server recv=%lu "
           "requested_missed_total=%u\n",
           (unsigned long)received, final_status.total_count);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
