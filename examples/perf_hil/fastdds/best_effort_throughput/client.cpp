/*
 * HIL 3-way QoS-matrix comparison, scenario "best_effort_throughput" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) client/sender role. Mirrors the CycloneDDS scenario pair exactly - a
 * one-way stream (no pong), the receiver (server.cpp) is the authoritative side for loss/
 * throughput since only it can see what actually arrived.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the
 * send loop, its pacing, its QoS and the RESULT line are unchanged.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h> // NOLINT(modernize-deprecated-headers) - nanosleep() is POSIX, not in <ctime>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/publisher/qos/PublisherQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>

#include "../../tickle/common/BenchStats.h" // shared instrumentation - see its own header
#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr double default_duration_s = 10.0;
    constexpr double match_timeout_s = 10.0;
    constexpr long match_poll_ns = 50L * 1000L * 1000L; // 50ms

    struct client_options {
        double duration_s = default_duration_s;
        double interval_s = 0.0; // 0 = as fast as possible
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.duration_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            }
        }
        return opts;
    }

    // Actively waits for a real match instead of a blind sleep - a real, confirmed CycloneDDS
    // discovery bug (COMPARISON.MD's own "Blocked" section) made this necessary there; adding it here
    // too on the first real failure of this exact scenario on FastDDS - a blind sleep alone isn't
    // reliable for a write-only participant on a topic other than the two latency scenarios' own
    // "ping"/"pong", confirmed the hard way (0 received, 100% loss, reproduced twice).
    auto wait_for_writer_match(DataWriter* writer, double timeout_s) -> bool {
        const uint64_t start = harness::now_ns();
        for (;;) {
            PublicationMatchedStatus status;
            writer->get_publication_matched_status(status);
            if (status.current_count > 0) {
                return true;
            }
            const double elapsed = static_cast<double>(harness::now_ns() - start) / harness::ns_per_s_real;
            if (elapsed >= timeout_s) {
                return false;
            }
            const struct timespec poll_interval = {0, match_poll_ns};
            nanosleep(&poll_interval, nullptr);
        }
    }

    auto send_until(DataWriter* writer, uint64_t deadline, double interval_s) -> uint64_t {
        uint32_t seq = 0;
        uint64_t sent = 0;
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            Bench msg;
            msg.seq(++seq);
            msg.send_ns(harness::now_ns());
            if (writer->write(&msg)) {
                sent++;
            }
            if (interval_s > 0.0) {
                harness::pace_seconds(interval_s);
            }
        }
        return sent;
    }

} // namespace

auto main(int argc, char** argv) -> int {
    // Armed at the very top, before any middleware setup, so the counters cover discovery
    // too - identically for all three frameworks, which is what makes them comparable.
    bench_stats_begin(&harness::g_bench_stats);
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

    Topic* const topic = participant->create_topic("stream", "Bench", TOPIC_QOS_DEFAULT);

    // scenario "best_effort_throughput" - BEST_EFFORT, matched exactly across frameworks
    // (COMPARISON.MD's design principle 3).
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;

    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* const writer = publisher->create_datawriter(topic, wqos);
    if (writer == nullptr) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    if (!wait_for_writer_match(writer, match_timeout_s)) {
        fprintf(stderr, "timed out waiting for a matched reader\n");
        return 1;
    }

    const uint64_t start = harness::now_ns();
    const uint64_t sent = send_until(writer, start + harness::seconds_to_ns(opts.duration_s), opts.interval_s);

    const double elapsed_s = static_cast<double>(harness::now_ns() - start) / harness::ns_per_s_real;
    const double mbps = harness::mbps(sent, sizeof(Bench), elapsed_s);
    bench_stats_end(&harness::g_bench_stats);
    printf("RESULT: framework=fastdds scenario=best_effort_throughput role=client sent=%lu elapsed_s=%.3f "
           "send_mbps=%.3f %s\n",
           static_cast<unsigned long>(sent), elapsed_s, mbps, harness::bench_fields(BENCH_ROLE_SENDER, sent));

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
