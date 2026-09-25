/*
 * HIL 3-way QoS-matrix comparison, scenario "liveliness_loss_detection" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) publisher role. See the CycloneDDS twin's own doc comment for the full
 * mechanism this mirrors exactly - publishes at a steady cadence with LIVELINESS AUTOMATIC and a
 * matched lease duration until it either runs out its own generous safety cap or gets killed
 * (`kill -9`) by the orchestrating test script partway through.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the publish
 * cadence, the QoS, the deliberately absent SIGINT handler and the RESULT line are unchanged.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

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

    constexpr double default_interval_s = 0.1;
    constexpr double default_lease_s = 2.0;
    constexpr double default_duration_s = 60.0;
    constexpr double announcements_per_lease = 3.0;
    constexpr double match_timeout_s = 10.0;

    struct client_options {
        double interval_s = default_interval_s;
        double lease_s = default_lease_s;
        double duration_s = default_duration_s;
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
                opts.lease_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.duration_s = atof(argv[++i]);
            }
        }
        return opts;
    }

    auto writer_qos(double lease_s) -> DataWriterQos {
        DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
        wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        wqos.liveliness().kind = AUTOMATIC_LIVELINESS_QOS;
        wqos.liveliness().lease_duration = eprosima::fastrtps::Duration_t(lease_s);
        // Real, required (2026-09-20, not a tuning choice) - FastDDS's own dds_create_writer
        // equivalent rejected this QoS outright ("LeaseDuration <= announcement period") with the
        // default announcement_period, which sits too close to lease_s to satisfy FastDDS's own
        // internal RTPS_QOS_CHECK. lease/3, matching this exercise's own established "3x within the
        // lease window" convention (rmw_tickle's own tt_LIVELINESS_MISS_THRESHOLD does the same).
        wqos.liveliness().announcement_period = eprosima::fastrtps::Duration_t(lease_s / announcements_per_lease);
        return wqos;
    }

    // Publishes until the safety cap - or, the case under test, until the orchestrator kills it. The
    // interrupted() check is inert here: no SIGINT handler is installed (see main()).
    auto publish_until(DataWriter* writer, const client_options& opts) -> uint32_t {
        uint32_t seq = 0;
        const uint64_t interval_ns = harness::seconds_to_ns(opts.interval_s);
        const uint64_t deadline = harness::now_ns() + harness::seconds_to_ns(opts.duration_s);
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            Bench msg;
            msg.seq(++seq);
            msg.send_ns(harness::now_ns());
            writer->write(&msg);
            harness::sleep_ns(interval_ns);
        }
        return seq;
    }

} // namespace

auto main(int argc, char** argv) -> int {
    const client_options opts = parse_options(argc, argv);

    // A liveliness_loss_detection client must die the way a crashed process dies, so this
    // scenario deliberately does NOT install a SIGINT handler. Measured 2026-09-23, lease 2.0s, 2
    // reps each: under `kill -INT` TickLE was detected in 88/92ms (its own goodbye broadcast) while
    // BOTH DDS vendors raised no liveliness event at all (clean unregister), against 1900-2332ms
    // and ~1999-2000ms under `kill -9`. A run with the wrong signal would not merely flatter one
    // framework - it would make the other two look broken, which is the kind of table nobody
    // double-checks because it confirms what they wanted. Leaving SIGINT at its default
    // disposition (terminate, no cleanup) makes the correct measurement the only obtainable one,
    // whichever signal the orchestrator sends and whoever runs it.
    //
    // Verified on the rig after the change (TickLE, lease 2.0s): `kill -9` still detects normally
    // (2325/2325ms), while `kill -INT` now leaves the client RUNNING and the run ends
    // departed=0/detect_latency_ms=-1. That is the intended outcome and worth understanding: the
    // orchestrator launches this client with `nohup ... &`, and a non-interactive shell sets
    // SIGINT to ignore for a background job, which the explicit handler used to override. So a
    // wrong-signal run now fails loudly instead of producing a plausible wrong number - and all
    // three frameworks fail it the same way, since the DDS twins raise no liveliness event under
    // -INT either.

    DomainParticipant* const participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (participant == nullptr) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }

    const TypeSupport type(new BenchPubSubType());
    type.register_type(participant);

    Topic* const topic = participant->create_topic("stream", "Bench", TOPIC_QOS_DEFAULT);

    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* const writer = publisher->create_datawriter(topic, writer_qos(opts.lease_s));
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

    printf("Publisher: matched (pid=%d), publishing every %.3fs until killed or %.1fs safety cap\n", getpid(),
           opts.interval_s, opts.duration_s);

    const uint32_t seq = publish_until(writer, opts);

    printf("RESULT: framework=fastdds scenario=liveliness_loss_detection role=client sent=%u "
           "(ran to completion, not killed)\n",
           seq);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
