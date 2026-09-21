/*
 * HIL 3-way QoS-matrix comparison, scenario "best_effort_latency" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) client/ping role. Mirrors the CycloneDDS scenario pair exactly - same
 * RESULT line shape as TickLE's own examples/linux/ping_pong/ping.c, framework field added for
 * this exercise's own dashboard parsing.
 */
#include <cmath>
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
    double interval_s = 1.0;
    double duration_s = 10.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
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

    Topic* ping_topic = participant->create_topic("ping", "Bench", TOPIC_QOS_DEFAULT);
    Topic* pong_topic = participant->create_topic("pong", "Bench", TOPIC_QOS_DEFAULT);

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;

    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataWriter* writer = publisher->create_datawriter(ping_topic, wqos);
    DataReader* reader = subscriber->create_datareader(pong_topic, rqos);
    if (!writer || !reader) {
        fprintf(stderr, "create_datawriter/datareader failed\n");
        return 1;
    }

    // Give discovery a moment - see the CycloneDDS client's own identical comment.
    struct timespec discovery_wait = {2, 0};
    nanosleep(&discovery_wait, nullptr);

    uint64_t transmitted = 0, received = 0;
    double rtt_min_ms = -1.0, rtt_max_ms = 0.0, rtt_sum_ms = 0.0;

    uint32_t seq = 0;
    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(duration_s * 1e9);
    uint64_t interval_ns = (uint64_t)(interval_s * 1e9);

    while (!g_interrupted && now_ns() < deadline) {
        Bench req;
        req.seq(++seq);
        req.send_ns(now_ns());
        writer->write(&req);
        transmitted++;

        eprosima::fastrtps::Duration_t timeout {0, 500 * 1000 * 1000}; // 500ms
        if (reader->wait_for_unread_message(timeout)) {
            // Drain every currently-buffered sample, keeping only the newest - a real, confirmed
            // FastDDS behavior found while debugging this exact loop: DataReader's own default
            // history can hold more than one not-yet-taken sample, and take_next_sample() is FIFO
            // (oldest first), so a single take() here would return an already-stale response
            // (matching an *older* request) whenever the reader had more than one queued, not the
            // fresh one this request's own RTT should be measured against.
            Bench resp;
            SampleInfo info;
            bool have_fresh = false;
            while (reader->take_next_sample(&resp, &info) == ReturnCode_t::RETCODE_OK) {
                if (info.valid_data) {
                    have_fresh = true;
                }
            }
            if (have_fresh && resp.seq() == req.seq()) {
                double rtt_ms = (double)(now_ns() - resp.send_ns()) / 1e6;
                received++;
                if (rtt_min_ms < 0.0 || rtt_ms < rtt_min_ms) {
                    rtt_min_ms = rtt_ms;
                }
                if (rtt_ms > rtt_max_ms) {
                    rtt_max_ms = rtt_ms;
                }
                rtt_sum_ms += rtt_ms;
            }
        }

        struct timespec sleep_ts = {(time_t)(interval_ns / 1000000000ULL), (long)(interval_ns % 1000000000ULL)};
        nanosleep(&sleep_ts, nullptr);
    }

    uint64_t lost = transmitted - received;
    double loss_pct = transmitted > 0 ? (100.0 * (double)lost / (double)transmitted) : 0.0;
    double avg = received > 0 ? rtt_sum_ms / (double)received : 0.0;

    printf("\n--- fastdds best_effort_latency statistics ---\n");
    printf("%lu sent, %lu received, %.0f%% loss\n", (unsigned long)transmitted, (unsigned long)received, loss_pct);
    if (received > 0) {
        printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", rtt_min_ms, avg, rtt_max_ms);
    }
    printf("RESULT: framework=fastdds scenario=best_effort_latency sent=%lu recv=%lu loss_pct=%.0f "
           "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
           (unsigned long)transmitted, (unsigned long)received, loss_pct, rtt_min_ms, avg, rtt_max_ms);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
