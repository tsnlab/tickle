/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) server/receiver role. The authoritative side for loss/throughput - only
 * it can see what actually arrived (see client.cpp's own doc comment).
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the
 * receive loop, its lifetime rule, its loss accounting, its QoS and the RESULT line are unchanged.
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
    constexpr double idle_cap_s = 15.0;
    constexpr int32_t default_keepall_samples = 4000; // resource_limits - matches client.cpp, and -N

    struct server_options {
        double safety_cap_s = default_safety_cap_s;
        // -K <depth>: KEEP_LAST at that depth instead of the KEEP_ALL default (2026-09-25). The
        // campaign's Q0 baseline uses KEEP_ALL for all three because that is the only configuration
        // where all three make the same promise, but KEEP_LAST is what rclcpp and TickLE actually
        // default to, so without this there is no cross-vendor cell for the configuration users get.
        // Depth 64, not 8: this file's own notes record KEEP_LAST(8) as the bisected cause of a real
        // 53% loss, so a shallow depth would re-measure that finding rather than the default.
        int keep_last_depth = 0; // 0 = KEEP_ALL, unchanged default
        // -N <samples>: KEEP_ALL's history bound, RESOURCE_LIMITS max_samples and max_samples_per_instance,
        // 4000 by default (2026-09-26, the fairness audit, COMPARISON.MD 4.4). The bound differed between
        // the three frameworks by default; the campaign now passes all three the same number of samples,
        // and every RESULT line's keepall_samples= says which it was. The same letter in all three.
        int32_t keepall_samples = default_keepall_samples;
    };

    auto parse_options(int argc, char** argv) -> server_options {
        server_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-K") == 0 && i + 1 < argc) {
                opts.keep_last_depth = atoi(argv[++i]);
            } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.safety_cap_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc) {
                opts.keepall_samples = static_cast<int32_t>(atoi(argv[++i]));
            }
        }
        // +15s buffer - see best_effort_throughput/server.cpp's own doc comment (this same directory)
        // for the real bug this avoids.
        opts.safety_cap_s += safety_cap_buffer_s;
        return opts;
    }

    // KEEP_ALL + generous resource_limits - matches client.cpp's own identical fix, see its doc
    // comment for the real bug this avoids.
    auto reader_qos(const server_options& opts) -> DataReaderQos {
        DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
        rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        if (opts.keep_last_depth > 0) {
            rqos.history().kind = KEEP_LAST_HISTORY_QOS;
            rqos.history().depth = opts.keep_last_depth;
        } else {
            rqos.history().kind = KEEP_ALL_HISTORY_QOS;
        }
        rqos.resource_limits().max_samples = opts.keepall_samples;
        rqos.resource_limits().max_samples_per_instance = opts.keepall_samples;
        return rqos;
    }

    // Lifetime (2026-09-25): an absolute cap only until the first sample arrives, then an idle
    // cap. It was absolute throughout - -d + 15 s from start - and that truncated the one cell that
    // needed longer: P4 under 5% loss needs at least 16.8 s of wire time, so the server exited while
    // the client was still retransmitting, leaving 200 samples undelivered and peer_acks_end=0 in
    // what read as a protocol result. Any fixed figure only moves that cliff to a worse condition.
    // "Don't hang forever" means "stop when nothing is arriving", so that is what it now checks.
    // run_scenario.sh still ends the normal case with SIGINT as soon as the client finishes; these
    // are only the backstops. Identical in all three frameworks' servers - a lifetime rule that
    // differed would hand whichever lived longest the samples the other was cut off from.
    void receive_until_idle(DataReader* reader, uint64_t deadline, harness::stream_stats& stats) {
        const uint64_t idle_cap_ns = harness::seconds_to_ns(idle_cap_s);
        while (!harness::interrupted()) {
            const uint64_t now = harness::now_ns();
            if (stats.first ? now >= deadline : now - stats.last_recv_ns >= idle_cap_ns) {
                break;
            }
            const eprosima::fastrtps::Duration_t timeout {1, 0};
            if (!reader->wait_for_unread_message(timeout)) {
                continue;
            }
            Bench sample;
            SampleInfo info;
            while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
                if (info.valid_data) {
                    harness::count_sample(stats, sample.seq());
                }
            }
        }
    }

} // namespace

auto main(int argc, char** argv) -> int {
    // Armed at the very top, before any middleware setup, so the counters cover discovery
    // too - identically for all three frameworks, which is what makes them comparable.
    bench_stats_begin(&harness::g_bench_stats);
    const server_options opts = parse_options(argc, argv);

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

    Subscriber* const subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* const reader = subscriber->create_datareader(topic, reader_qos(opts));
    if (reader == nullptr) {
        fprintf(stderr, "create_datareader failed\n");
        return 1;
    }

    harness::stream_stats stats;
    receive_until_idle(reader, harness::now_ns() + harness::seconds_to_ns(opts.safety_cap_s), stats);

    const double elapsed_s = harness::stream_elapsed_s(stats);
    const double loss_pct = harness::stream_loss_pct(stats);
    const double mbps = harness::mbps(stats.received, sizeof(Bench), elapsed_s);

    bench_stats_end(&harness::g_bench_stats);

    printf("RESULT: framework=fastdds scenario=reliable_throughput role=server recv=%lu lost=%lu "
           "loss_pct=%.1f elapsed_s=%.3f recv_mbps=%.3f keep_all=%d keep_last_depth=%d keepall_samples=%d %s\n",
           static_cast<unsigned long>(stats.received), static_cast<unsigned long>(stats.lost), loss_pct, elapsed_s,
           mbps, opts.keep_last_depth > 0 ? 0 : 1, opts.keep_last_depth, static_cast<int>(opts.keepall_samples),
           harness::bench_fields(BENCH_ROLE_RECEIVER, stats.received));

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
