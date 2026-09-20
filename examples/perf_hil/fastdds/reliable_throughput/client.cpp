/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/comparison.md) -
 * FastDDS native (no rmw) client/sender role. Mirrors the CycloneDDS scenario pair exactly - a
 * one-way stream (no pong), the receiver (server.cpp) is the authoritative side for loss/
 * throughput since only it can see what actually arrived.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

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
    double duration_s = 10.0;
    double interval_s = 0.0; // 0 = as fast as possible
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
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

    // scenario "reliable_throughput" - BEST_EFFORT, matched exactly across frameworks
    // (comparison.md's design principle 3).
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = 8;

    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* writer = publisher->create_datawriter(topic, wqos);
    if (!writer) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    // Give discovery a moment - see the latency scenarios' own identical comment.
    struct timespec discovery_wait = {2, 0};
    nanosleep(&discovery_wait, nullptr);

    uint32_t seq = 0;
    uint64_t sent = 0;
    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(duration_s * 1e9);

    while (!g_interrupted && now_ns() < deadline) {
        Bench msg;
        msg.seq(++seq);
        msg.send_ns(now_ns());
        if (writer->write(&msg)) {
            sent++;
        }
        if (interval_s > 0.0) {
            struct timespec pace = {(time_t)interval_s, (long)((interval_s - (time_t)interval_s) * 1e9)};
            nanosleep(&pace, nullptr);
        }
    }

    double elapsed_s = (double)(now_ns() - start) / 1e9;
    double mbps = elapsed_s > 0.0 ? ((double)sent * sizeof(Bench) * 8.0) / 1e6 / elapsed_s : 0.0;
    printf("RESULT: framework=fastdds scenario=reliable_throughput role=client sent=%lu elapsed_s=%.3f "
           "send_mbps=%.3f\n",
           (unsigned long)sent, elapsed_s, mbps);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
