/*
 * HIL 3-way QoS-matrix comparison, scenario "best_effort_latency" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) client/ping role. Mirrors the CycloneDDS scenario pair exactly - same
 * RESULT line shape as TickLE's own examples/linux/ping_pong/ping.c, framework field added for
 * this exercise's own dashboard parsing.
 *
 * Structured into small functions in an anonymous namespace (2026-09-25) so that it passes the
 * repository's own C++ clang-tidy checks, which the rmw_tickle C++ code already meets. It was never
 * linted before: until the CI step that builds the perf_hil harnesses stopped dying on the CycloneDDS
 * block, the FastDDS harness had no compile-database entries for clang-tidy to use. The logic, the
 * timing and the RESULT line are unchanged.
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h> // NOLINT(modernize-deprecated-headers) - nanosleep() is POSIX, not in <ctime>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/publisher/qos/PublisherQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/subscriber/qos/SubscriberQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>
#include <fastdds/rtps/common/Time_t.h>

#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr double default_interval_s = 1.0;
    constexpr double default_duration_s = 10.0;
    constexpr uint32_t response_wait_ns = 500U * 1000U * 1000U; // 500ms
    constexpr time_t discovery_wait_s = 2;                      // see the CycloneDDS client's identical comment

    struct rtt_stats {
        uint64_t transmitted = 0;
        uint64_t received = 0;
        double min_ms = -1.0;
        double max_ms = 0.0;
        double sum_ms = 0.0;
    };

    struct client_options {
        double interval_s = default_interval_s;
        double duration_s = default_duration_s;
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.duration_s = atof(argv[++i]);
            }
        }
        return opts;
    }

    void record_rtt(rtt_stats& stats, double rtt_ms) {
        stats.received++;
        if (stats.min_ms < 0.0 || rtt_ms < stats.min_ms) {
            stats.min_ms = rtt_ms;
        }
        stats.max_ms = std::max(stats.max_ms, rtt_ms);
        stats.sum_ms += rtt_ms;
    }

    // Drain every currently-buffered sample, keeping only the newest - a real, confirmed FastDDS behavior
    // found while debugging this exact loop: DataReader's own default history can hold more than one
    // not-yet-taken sample, and take_next_sample() is FIFO (oldest first), so a single take() here would
    // return an already-stale response (matching an *older* request) whenever the reader had more than one
    // queued, not the fresh one this request's own RTT should be measured against.
    auto take_newest(DataReader* reader, Bench& resp) -> bool {
        SampleInfo info;
        bool have_fresh = false;
        while (reader->take_next_sample(&resp, &info) == ReturnCode_t::RETCODE_OK) {
            if (info.valid_data) {
                have_fresh = true;
            }
        }
        return have_fresh;
    }

    void ping_once(DataWriter* writer, DataReader* reader, uint32_t seq, rtt_stats& stats) {
        Bench req;
        req.seq(seq);
        req.send_ns(harness::now_ns());
        writer->write(&req);
        stats.transmitted++;

        const eprosima::fastrtps::Duration_t timeout {0, response_wait_ns};
        if (!reader->wait_for_unread_message(timeout)) {
            return;
        }
        Bench resp;
        if (take_newest(reader, resp) && resp.seq() == req.seq()) {
            record_rtt(stats, static_cast<double>(harness::now_ns() - resp.send_ns()) / harness::ns_per_ms);
        }
    }

    void report(const rtt_stats& stats) {
        const uint64_t lost = stats.transmitted - stats.received;
        const double loss_pct =
            stats.transmitted > 0
                ? (harness::percent * static_cast<double>(lost) / static_cast<double>(stats.transmitted))
                : 0.0;
        const double avg = stats.received > 0 ? stats.sum_ms / static_cast<double>(stats.received) : 0.0;

        printf("\n--- fastdds best_effort_latency statistics ---\n");
        printf("%lu sent, %lu received, %.0f%% loss\n", static_cast<unsigned long>(stats.transmitted),
               static_cast<unsigned long>(stats.received), loss_pct);
        if (stats.received > 0) {
            printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", stats.min_ms, avg, stats.max_ms);
        }
        printf("RESULT: framework=fastdds scenario=best_effort_latency sent=%lu recv=%lu loss_pct=%.0f "
               "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
               static_cast<unsigned long>(stats.transmitted), static_cast<unsigned long>(stats.received), loss_pct,
               stats.min_ms, avg, stats.max_ms);
    }

} // namespace

auto main(int argc, char** argv) -> int {
    const client_options opts = parse_options(argc, argv);

    harness::install_sigint_handler();

    DomainParticipant* const participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (participant == nullptr) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }

    const TypeSupport type(new BenchPubSubType());
    type.register_type(participant);

    Topic* const ping_topic = participant->create_topic("ping", "Bench", TOPIC_QOS_DEFAULT);
    Topic* const pong_topic = participant->create_topic("pong", "Bench", TOPIC_QOS_DEFAULT);

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;

    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    Subscriber* const subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataWriter* const writer = publisher->create_datawriter(ping_topic, wqos);
    DataReader* const reader = subscriber->create_datareader(pong_topic, rqos);
    if (writer == nullptr || reader == nullptr) {
        fprintf(stderr, "create_datawriter/datareader failed\n");
        return 1;
    }

    const struct timespec discovery_wait = {discovery_wait_s, 0};
    nanosleep(&discovery_wait, nullptr);

    rtt_stats stats;
    uint32_t seq = 0;
    const uint64_t deadline = harness::now_ns() + harness::seconds_to_ns(opts.duration_s);
    const uint64_t interval_ns = harness::seconds_to_ns(opts.interval_s);
    while (!harness::interrupted() && harness::now_ns() < deadline) {
        ping_once(writer, reader, ++seq, stats);
        harness::sleep_ns(interval_ns);
    }

    report(stats);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
