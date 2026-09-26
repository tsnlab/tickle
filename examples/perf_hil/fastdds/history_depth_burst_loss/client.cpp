/*
 * HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (rmw_tickle/COMPARISON.md) -
 * FastDDS native (no rmw) publisher role. See the CycloneDDS twin's own doc comment for the full
 * mechanism this mirrors exactly - writes on a fixed schedule regardless of whether the subscriber
 * is consuming, with a short (200ms) max_blocking_time so a stalled reader produces real, timely
 * write failures once depth=8's own bounded write-history-cache fills and stays unacknowledged.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the fixed-count
 * write schedule, the QoS, the ack wait and the RESULT line are unchanged.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
#include <fastdds/rtps/common/Time_t.h>

#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr double default_interval_s = 0.05;
    // Fixed sample COUNT, not a fixed duration - see the CycloneDDS twin's own doc comment for the
    // real bug this avoids (a duration-based loop tearing the writer down mid-catch-up).
    constexpr uint32_t default_count = 160;
    constexpr int32_t history_depth = 8;
    constexpr uint32_t max_blocking_ns = 200U * 1000U * 1000U; // 200ms
    constexpr double match_timeout_s = 10.0;
    constexpr int32_t ack_wait_s = 10;

    struct client_options {
        double interval_s = default_interval_s;
        uint32_t count = default_count;
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
                opts.count = static_cast<uint32_t>(atoi(argv[++i]));
            }
        }
        return opts;
    }

    struct write_result {
        uint64_t sent = 0;
        uint64_t dropped = 0;
    };

    auto write_schedule(DataWriter* writer, const client_options& opts) -> write_result {
        write_result result;
        uint32_t seq = 0;
        const uint64_t interval_ns = harness::seconds_to_ns(opts.interval_s);
        while (!harness::interrupted() && seq < opts.count) {
            Bench msg;
            msg.seq(++seq);
            msg.send_ns(harness::now_ns());
            if (writer->write(&msg)) {
                result.sent++;
            } else {
                result.dropped++;
            }
            harness::sleep_ns(interval_ns);
        }
        return result;
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

    Topic* const topic = participant->create_topic("stream", "Bench", TOPIC_QOS_DEFAULT);

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.reliability().max_blocking_time = eprosima::fastrtps::Duration_t(0, max_blocking_ns);
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = history_depth;

    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* const writer = publisher->create_datawriter(topic, wqos);
    if (writer == nullptr) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    // Match-wait, this exercise's own established FastDDS idiom.
    const bool matched = harness::wait_until(match_timeout_s, [writer] {
        PublicationMatchedStatus status;
        writer->get_publication_matched_status(status);
        return status.current_count > 0;
    });
    if (!matched) {
        fprintf(stderr, "timed out waiting for a matched reader\n");
        return 1;
    }

    const write_result result = write_schedule(writer, opts);

    // Wait for real acks before tearing down - see the CycloneDDS twin's own doc comment for the
    // real bug this avoids.
    writer->wait_for_acknowledgments(eprosima::fastrtps::Duration_t(ack_wait_s, 0));

    printf("RESULT: framework=fastdds scenario=history_depth_burst_loss role=client sent=%lu "
           "write_dropped=%lu\n",
           static_cast<unsigned long>(result.sent), static_cast<unsigned long>(result.dropped));

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
