/*
 * HIL 3-way QoS-matrix comparison, scenario "durability_late_join" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) subscriber role, deliberately started *after* the publisher has already
 * sent its whole backlog (run_scenario.sh's own orchestration sleeps before starting this side) -
 * the actual thing under test: does TRANSIENT_LOCAL (-D) deliver that backlog anyway, while
 * VOLATILE (default) delivers none of it. See server.cpp's own doc comment for why this side also
 * has a writer (the ack), not just a reader.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the match-wait, the
 * receive window, the QoS and the RESULT line are unchanged.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
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

    constexpr int32_t history_depth = 20; // matches server.cpp's own backlog_count
    constexpr double match_timeout_s = 15.0;
    // 15s, not a short window - see the CycloneDDS twin's own doc comment: RELIABLE redelivery of a
    // durability backlog goes through real ACKNACK round-trips per sample, not an instant replay.
    constexpr uint64_t backlog_window_ns = 15ULL * harness::ns_per_s;

    auto parse_durable(int argc, char** argv) -> bool {
        bool durable = false;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-D") == 0) {
                durable = true;
            }
        }
        return durable;
    }

    auto receive_backlog(DataReader* reader, uint64_t deadline) -> uint32_t {
        uint32_t received = 0;
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            const eprosima::fastrtps::Duration_t timeout {1, 0};
            if (!reader->wait_for_unread_message(timeout)) {
                continue;
            }
            Bench sample;
            SampleInfo info;
            while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
                if (info.valid_data) {
                    received++;
                }
            }
        }
        return received;
    }

} // namespace

auto main(int argc, char** argv) -> int {
    const bool durable = parse_durable(argc, argv);

    harness::install_sigint_handler();

    DomainParticipant* const participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (participant == nullptr) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }

    const TypeSupport type(new BenchPubSubType());
    type.register_type(participant);

    Topic* const data_topic = participant->create_topic("ping", "Bench", TOPIC_QOS_DEFAULT);
    Topic* const ack_topic = participant->create_topic("pong", "Bench", TOPIC_QOS_DEFAULT);

    // Matches server.cpp's own backlog_count (20) - see its own doc comment for the real bug this
    // avoids.
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = history_depth;
    if (durable) {
        rqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    }

    Subscriber* const subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* const reader = subscriber->create_datareader(data_topic, rqos);
    if (reader == nullptr) {
        fprintf(stderr, "create_datareader failed\n");
        return 1;
    }

    DataWriterQos ack_qos = DATAWRITER_QOS_DEFAULT;
    ack_qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* const ack_writer = publisher->create_datawriter(ack_topic, ack_qos);
    if (ack_writer == nullptr) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    // Actively wait for a real match on both, matching the CycloneDDS twin's own approach - the
    // real, confirmed reason a plain sleep isn't good enough is documented on the CycloneDDS side's
    // own common.h/client.c doc comments.
    const bool matched = harness::wait_until(match_timeout_s, [reader, ack_writer] {
        SubscriptionMatchedStatus sub_status;
        reader->get_subscription_matched_status(sub_status);
        const bool reader_matched = sub_status.current_count > 0;
        PublicationMatchedStatus pub_status;
        ack_writer->get_publication_matched_status(pub_status);
        const bool writer_matched = pub_status.current_count > 0;
        return reader_matched && writer_matched;
    });
    if (!matched) {
        fprintf(stderr, "timed out waiting for a match\n");
        return 1;
    }

    const uint64_t start = harness::now_ns();
    const uint32_t received = receive_backlog(reader, start + backlog_window_ns);
    const double backlog_delivery_ms =
        received > 0 ? static_cast<double>(harness::now_ns() - start) / harness::ns_per_ms : -1.0;

    Bench ack;
    ack.seq(1);
    ack.send_ns(harness::now_ns());
    ack_writer->write(&ack);

    printf("RESULT: framework=fastdds scenario=durability_late_join role=client durable=%d "
           "received=%u backlog_delivery_ms=%.3f\n",
           static_cast<int>(durable), received, backlog_delivery_ms);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
