/*
 * HIL 3-way QoS-matrix comparison, scenario "deadline_miss_detection" (rmw_tickle/COMPARISON.md) -
 * FastDDS native (no rmw) subscriber role. See the CycloneDDS twin's own doc comment for the full
 * mechanism - mirrors client.cpp's own DEADLINE QoS exactly and independently monitors this side's
 * own RequestedDeadlineMissedStatus.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the
 * receive loop, the QoS and the RESULT line are unchanged.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/core/status/DeadlineMissedStatus.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
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

#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr double default_deadline_s = 0.05;
    constexpr double default_safety_cap_s = 30.0;
    constexpr double match_timeout_s = 10.0;
    constexpr uint32_t receive_wait_ns = 200U * 1000U * 1000U; // 200ms

    struct server_options {
        double deadline_s = default_deadline_s;
        double safety_cap_s = default_safety_cap_s;
    };

    auto parse_options(int argc, char** argv) -> server_options {
        server_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
                opts.deadline_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.safety_cap_s = atof(argv[++i]);
            }
        }
        return opts;
    }

    auto receive_until(DataReader* reader, uint64_t deadline) -> uint64_t {
        uint64_t received = 0;
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            const eprosima::fastrtps::Duration_t timeout {0, receive_wait_ns};
            if (reader->wait_for_unread_message(timeout)) {
                Bench sample;
                SampleInfo info;
                while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
                    if (info.valid_data) {
                        received++;
                    }
                }
            }
        }
        return received;
    }

} // namespace

auto main(int argc, char** argv) -> int {
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

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.deadline().period = eprosima::fastrtps::Duration_t(opts.deadline_s);

    Subscriber* const subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* const reader = subscriber->create_datareader(topic, rqos);
    if (reader == nullptr) {
        fprintf(stderr, "create_datareader failed\n");
        return 1;
    }

    // Match-wait, this exercise's own established FastDDS idiom.
    const bool matched = harness::wait_until(match_timeout_s, [reader] {
        SubscriptionMatchedStatus status;
        reader->get_subscription_matched_status(status);
        return status.current_count > 0;
    });
    if (!matched) {
        fprintf(stderr, "timed out waiting for a matched writer\n");
        return 1;
    }

    const uint64_t received = receive_until(reader, harness::now_ns() + harness::seconds_to_ns(opts.safety_cap_s));

    RequestedDeadlineMissedStatus final_status;
    reader->get_requested_deadline_missed_status(final_status);

    printf("RESULT: framework=fastdds scenario=deadline_miss_detection role=server recv=%lu "
           "requested_missed_total=%u\n",
           static_cast<unsigned long>(received), final_status.total_count);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
