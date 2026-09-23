/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/COMPARISON.MD) -
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
    double interval_s = 0.0;       // 0 = as fast as possible
    double drain_s = 3.0;          // cap on the teardown wait-for-acknowledgements below
    double max_blocking_ms = -1.0; // -B: RELIABILITY max_blocking_time; <0 keeps FastDDS's default (100ms)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-B") == 0 && i + 1 < argc) {
            max_blocking_ms = atof(argv[++i]);
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

    // scenario "reliable_throughput" - RELIABLE, matched exactly across frameworks
    // (COMPARISON.MD's design principle 3). KEEP_ALL + generous resource_limits (2026-09-21, real
    // bug found the hard way - see the CycloneDDS twin's own identical fix, which this file never
    // got the matching update for): the shallow KEEP_LAST(8) this file used before real-CI-caught
    // reliable_throughput's own RELIABLE never recovering any real tc/netem-injected loss at all
    // (COMPARISON.MD §3/§6 - CycloneDDS's own KEEP_ALL+resource_limits(4000) fully recovers 1%/5%
    // injected loss every time, this file's own shallow depth=8 recovered none of it) - a shallow
    // writer history queue evicts a lost sample long before a NACK-driven retry can land, the exact
    // same root cause CycloneDDS's own client.c doc comment already names.
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.history().kind = KEEP_ALL_HISTORY_QOS;
    wqos.resource_limits().max_samples = 4000;
    if (max_blocking_ms >= 0.0) {
        // Field-wise, not a Duration_t constructor: the type lives in eprosima::fastrtps on FastDDS
        // 2.x (the rig's jazzy) but in eprosima::fastdds on 3.x, while seconds/nanosec exist in both.
        double blocking_s = max_blocking_ms / 1000.0;
        wqos.reliability().max_blocking_time.seconds = (int32_t)blocking_s;
        wqos.reliability().max_blocking_time.nanosec = (uint32_t)((blocking_s - (int32_t)blocking_s) * 1e9);
    }

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
    // Writes the DataWriter refused (e.g. a timeout once KEEP_ALL's resource_limits stay full past
    // max_blocking_time). seq is still consumed, so the server counts each one as lost too;
    // write_fail lets the two be told apart (rmw_tickle/PLAN.md Phase 3, item 5).
    uint64_t write_fail = 0;
    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(duration_s * 1e9);

    while (!g_interrupted && now_ns() < deadline) {
        Bench msg;
        msg.seq(++seq);
        msg.send_ns(now_ns());
        if (writer->write(&msg)) {
            sent++;
        } else {
            write_fail++;
        }
        if (interval_s > 0.0) {
            struct timespec pace = {(time_t)interval_s, (long)((interval_s - (time_t)interval_s) * 1e9)};
            nanosleep(&pace, nullptr);
        }
    }

    // Teardown drain, matching the TickLE and CycloneDDS harnesses so all three are measured the
    // same way at the end of a run - see the CycloneDDS twin's own comment for why a tail sample
    // is otherwise invisible. wait_for_acknowledgments() is FastDDS's own equivalent.
    eprosima::fastrtps::Duration_t drain {(int32_t)drain_s, 0};
    // == RETCODE_OK, not a bool test: on FastDDS 2.x (the rig's jazzy) this returns
    // fastrtps::types::ReturnCode_t, whose operator bool() is deleted; 3.x returns the
    // fastdds::dds enum. Comparing against the 2.x constant is what compiles where we measure.
    const char* drained = writer->wait_for_acknowledgments(drain) == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK
                              ? "acked"
                              : "timeout";

    double elapsed_s = (double)(now_ns() - start) / 1e9;
    double mbps = elapsed_s > 0.0 ? ((double)sent * sizeof(Bench) * 8.0) / 1e6 / elapsed_s : 0.0;
    printf("RESULT: framework=fastdds scenario=reliable_throughput role=client sent=%lu write_fail=%lu "
           "elapsed_s=%.3f send_mbps=%.3f max_blocking_ms=%.3f drained=%s\n",
           (unsigned long)sent, (unsigned long)write_fail, elapsed_s, mbps, max_blocking_ms, drained);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
