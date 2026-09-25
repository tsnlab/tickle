/*
 * HIL 3-way QoS-matrix comparison, scenario "best_effort_throughput" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) server/receiver role. The authoritative side for loss/throughput - only
 * it can see what actually arrived (see client.cpp's own doc comment).
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the
 * receive loop, its loss accounting, its QoS and the RESULT line are unchanged.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/subscriber/qos/SubscriberQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>
#include <fastdds/rtps/common/Time_t.h>

#include "../../tickle/common/BenchStats.h" // shared instrumentation - see its own header
#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr double default_safety_cap_s = 40.0;
    constexpr double safety_cap_buffer_s = 15.0;

    struct stream_stats {
        uint64_t received = 0;
        uint64_t lost = 0;
        uint32_t last_seq = 0;
        bool first = true;
        uint64_t first_recv_ns = 0;
        uint64_t last_recv_ns = 0;
    };

    auto parse_safety_cap(int argc, char** argv) -> double {
        double safety_cap_s = default_safety_cap_s;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                safety_cap_s = atof(argv[++i]);
            }
        }
        // +15s buffer (2026-09-21, real bug found the hard way - see the identical fix on the TickLE
        // twin's own server.c for the full story): run_scenario.sh forwards the same -d to both
        // sides, but it means "the client's own send duration" there vs. "this side's own don't-hang-
        // forever cap" here - taken verbatim, this side could exit and stop counting before the
        // client (which starts several seconds later, run_scenario.sh's own pre-client sleep) had
        // even finished sending.
        return safety_cap_s + safety_cap_buffer_s;
    }

    void count_sample(stream_stats& stats, uint32_t seq) {
        if (stats.first) {
            stats.first = false;
            stats.first_recv_ns = harness::now_ns();
        } else if (seq > stats.last_seq + 1) {
            stats.lost += (seq - stats.last_seq - 1);
        }
        stats.last_seq = seq;
        stats.last_recv_ns = harness::now_ns();
        stats.received++;
    }

    void receive_until(DataReader* reader, uint64_t deadline, stream_stats& stats) {
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            const eprosima::fastrtps::Duration_t timeout {1, 0};
            if (!reader->wait_for_unread_message(timeout)) {
                continue;
            }
            Bench sample;
            SampleInfo info;
            while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
                if (info.valid_data) {
                    count_sample(stats, sample.seq());
                }
            }
        }
    }

    void report(const stream_stats& stats) {
        const double elapsed_s =
            stats.received > 0 ? static_cast<double>(stats.last_recv_ns - stats.first_recv_ns) / harness::ns_per_s_real
                               : 0.0;
        const uint64_t total = stats.received + stats.lost;
        const double loss_pct =
            total > 0 ? (harness::percent * static_cast<double>(stats.lost) / static_cast<double>(total)) : 0.0;
        const double mbps = elapsed_s > 0.0
                                ? ((static_cast<double>(stats.received) * sizeof(Bench) * harness::bits_per_byte) /
                                   harness::bits_per_megabit / elapsed_s)
                                : 0.0;

        bench_stats_end(&harness::g_bench_stats);

        printf("RESULT: framework=fastdds scenario=best_effort_throughput role=server recv=%lu lost=%lu "
               "loss_pct=%.1f elapsed_s=%.3f recv_mbps=%.3f %s\n",
               static_cast<unsigned long>(stats.received), static_cast<unsigned long>(stats.lost), loss_pct, elapsed_s,
               mbps, harness::bench_fields(BENCH_ROLE_RECEIVER, stats.received));
    }

} // namespace

auto main(int argc, char** argv) -> int {
    // Armed at the very top, before any middleware setup, so the counters cover discovery
    // too - identically for all three frameworks, which is what makes them comparable.
    bench_stats_begin(&harness::g_bench_stats);
    const double safety_cap_s = parse_safety_cap(argc, argv);

    harness::install_sigint_handler();

    DomainParticipant* const participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (participant == nullptr) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }

    const TypeSupport type(new BenchPubSubType());
    type.register_type(participant);

    Topic* const topic = participant->create_topic("stream", "Bench", TOPIC_QOS_DEFAULT);

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;

    Subscriber* const subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* const reader = subscriber->create_datareader(topic, rqos);
    if (reader == nullptr) {
        fprintf(stderr, "create_datareader failed\n");
        return 1;
    }

    stream_stats stats;
    receive_until(reader, harness::now_ns() + harness::seconds_to_ns(safety_cap_s), stats);
    report(stats);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
