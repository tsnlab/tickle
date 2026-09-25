/*
 * HIL 3-way QoS-matrix comparison, scenario "history_depth_burst_loss" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) subscriber role. See the CycloneDDS twin's own doc comment for the full
 * mechanism - this mirrors it exactly: deliberately stalls its own consumption for `-p` seconds
 * right after matching, before ever creating the receive loop, while the writer (client.cpp) keeps
 * writing on a fixed schedule regardless.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the stall, the
 * loss accounting, the QoS and the RESULT line are unchanged.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
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

    constexpr double default_pause_s = 3.0;
    constexpr double default_safety_cap_s = 30.0;
    constexpr int32_t history_depth = 8;
    constexpr double match_timeout_s = 10.0;

    struct server_options {
        double pause_s = default_pause_s;
        double safety_cap_s = default_safety_cap_s;
    };

    auto parse_options(int argc, char** argv) -> server_options {
        server_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
                opts.pause_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.safety_cap_s = atof(argv[++i]);
            }
        }
        return opts;
    }

    // Takes everything that arrives until `deadline`, counting seq gaps from zero.
    void receive_until(DataReader* reader, uint64_t deadline, harness::seq_gaps& gaps) {
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            const eprosima::fastrtps::Duration_t timeout {1, 0};
            if (!reader->wait_for_unread_message(timeout)) {
                continue;
            }
            Bench sample;
            SampleInfo info;
            while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
                if (info.valid_data) {
                    harness::count_seq(gaps, sample.seq());
                }
            }
        }
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
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = history_depth;

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

    printf("Subscriber: matched, now deliberately stalling for %.1fs before consuming anything\n", opts.pause_s);
    harness::sleep_seconds(opts.pause_s);

    // last_seq starts at 0, not the first received sample's own seq (2026-09-20, real measurement
    // bug - see the CycloneDDS twin's own doc comment for the full story: it silently hid this
    // scenario's own real reader-side HISTORY eviction loss during the pause window).
    harness::seq_gaps gaps;
    receive_until(reader, harness::now_ns() + harness::seconds_to_ns(opts.safety_cap_s), gaps);

    printf("RESULT: framework=fastdds scenario=history_depth_burst_loss role=server pause_s=%.1f "
           "recv=%lu lost=%lu loss_pct=%.1f\n",
           opts.pause_s, static_cast<unsigned long>(gaps.received), static_cast<unsigned long>(gaps.lost),
           harness::seq_gaps_loss_pct(gaps));

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
