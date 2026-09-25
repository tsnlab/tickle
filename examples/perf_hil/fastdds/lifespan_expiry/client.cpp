/*
 * HIL 3-way QoS-matrix comparison, scenario "lifespan_expiry" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) publisher role. See the CycloneDDS twin's own doc comment for the full
 * mechanism this mirrors exactly - writes a fixed count with LIFESPAN and a generous
 * `KEEP_ALL` history, deliberately *not* the shallow depth scenario 6 tests, so any loss observed
 * is attributable to LIFESPAN's own age-based expiry, not queue-depth eviction.
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

    constexpr double default_interval_s = 0.02;
    constexpr double default_lifespan_s = 0.1;
    constexpr uint32_t default_count = 100;
    constexpr int32_t max_samples = 500;
    constexpr double match_timeout_s = 10.0;
    constexpr int32_t ack_wait_s = 10;

    struct client_options {
        double interval_s = default_interval_s;
        double lifespan_s = default_lifespan_s;
        uint32_t count = default_count;
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
                opts.lifespan_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
                opts.count = static_cast<uint32_t>(atoi(argv[++i]));
            }
        }
        return opts;
    }

    auto write_schedule(DataWriter* writer, const client_options& opts) -> uint64_t {
        uint64_t sent = 0;
        uint32_t seq = 0;
        const uint64_t interval_ns = harness::seconds_to_ns(opts.interval_s);
        while (!harness::interrupted() && seq < opts.count) {
            Bench msg;
            msg.seq(++seq);
            msg.send_ns(harness::now_ns());
            writer->write(&msg);
            sent++;
            harness::sleep_ns(interval_ns);
        }
        return sent;
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
    wqos.history().kind = KEEP_ALL_HISTORY_QOS;
    wqos.resource_limits().max_samples = max_samples;
    wqos.lifespan().duration = eprosima::fastrtps::Duration_t(opts.lifespan_s);

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

    const uint64_t sent = write_schedule(writer, opts);

    writer->wait_for_acknowledgments(eprosima::fastrtps::Duration_t(ack_wait_s, 0));

    printf("RESULT: framework=fastdds scenario=lifespan_expiry role=client sent=%lu\n",
           static_cast<unsigned long>(sent));

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
