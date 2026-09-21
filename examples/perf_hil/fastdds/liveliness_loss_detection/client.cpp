/*
 * HIL 3-way QoS-matrix comparison, scenario "liveliness_loss_detection" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) publisher role. See the CycloneDDS twin's own doc comment for the full
 * mechanism this mirrors exactly - publishes at a steady cadence with LIVELINESS AUTOMATIC and a
 * matched lease duration until it either runs out its own generous safety cap or gets killed
 * (`kill -9`) by the orchestrating test script partway through.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
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
    double interval_s = 0.1;
    double lease_s = 2.0;
    double duration_s = 60.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            lease_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
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

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.liveliness().kind = AUTOMATIC_LIVELINESS_QOS;
    wqos.liveliness().lease_duration = eprosima::fastrtps::Duration_t(lease_s);
    // Real, required (2026-09-20, not a tuning choice) - FastDDS's own dds_create_writer
    // equivalent rejected this QoS outright ("LeaseDuration <= announcement period") with the
    // default announcement_period, which sits too close to lease_s to satisfy FastDDS's own
    // internal RTPS_QOS_CHECK. lease/3, matching this exercise's own established "3x within the
    // lease window" convention (rmw_tickle's own tt_LIVELINESS_MISS_THRESHOLD does the same).
    wqos.liveliness().announcement_period = eprosima::fastrtps::Duration_t(lease_s / 3.0);

    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* writer = publisher->create_datawriter(topic, wqos);
    if (!writer) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    // Match-wait, this exercise's own established FastDDS idiom.
    {
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        bool matched = false;
        for (;;) {
            PublicationMatchedStatus status;
            writer->get_publication_matched_status(status);
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
            fprintf(stderr, "timed out waiting for a matched reader\n");
            return 1;
        }
    }

    printf("Publisher: matched (pid=%d), publishing every %.3fs until killed or %.1fs safety cap\n", getpid(),
           interval_s, duration_s);

    uint32_t seq = 0;
    uint64_t interval_ns = (uint64_t)(interval_s * 1e9);
    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(duration_s * 1e9);

    while (!g_interrupted && now_ns() < deadline) {
        Bench msg;
        msg.seq(++seq);
        msg.send_ns(now_ns());
        writer->write(&msg);
        struct timespec pace = {(time_t)(interval_ns / 1000000000ULL), (long)(interval_ns % 1000000000ULL)};
        nanosleep(&pace, nullptr);
    }

    printf("RESULT: framework=fastdds scenario=liveliness_loss_detection role=client sent=%u "
           "(ran to completion, not killed)\n",
           seq);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
