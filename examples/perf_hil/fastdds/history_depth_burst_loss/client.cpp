/*
 * HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) publisher role. See the CycloneDDS twin's own doc comment for the full
 * mechanism this mirrors exactly - writes on a fixed schedule regardless of whether the subscriber
 * is consuming, with a short (200ms) max_blocking_time so a stalled reader produces real, timely
 * write failures once depth=8's own bounded write-history-cache fills and stays unacknowledged.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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
    double interval_s = 0.05;
    // Fixed sample COUNT, not a fixed duration - see the CycloneDDS twin's own doc comment for the
    // real bug this avoids (a duration-based loop tearing the writer down mid-catch-up).
    uint32_t count = 160;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = (uint32_t)atoi(argv[++i]);
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
    wqos.reliability().max_blocking_time = eprosima::fastrtps::Duration_t(0, 200 * 1000 * 1000);
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = 8;

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

    uint32_t seq = 0;
    uint64_t sent = 0, dropped = 0;
    uint64_t interval_ns = (uint64_t)(interval_s * 1e9);

    while (!g_interrupted && seq < count) {
        Bench msg;
        msg.seq(++seq);
        msg.send_ns(now_ns());
        if (writer->write(&msg)) {
            sent++;
        } else {
            dropped++;
        }
        struct timespec pace = {(time_t)(interval_ns / 1000000000ULL), (long)(interval_ns % 1000000000ULL)};
        nanosleep(&pace, nullptr);
    }

    // Wait for real acks before tearing down - see the CycloneDDS twin's own doc comment for the
    // real bug this avoids.
    writer->wait_for_acknowledgments(eprosima::fastrtps::Duration_t(10, 0));

    printf("RESULT: framework=fastdds scenario=history_depth_burst_loss role=client sent=%lu "
           "write_dropped=%lu\n",
           (unsigned long)sent, (unsigned long)dropped);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
