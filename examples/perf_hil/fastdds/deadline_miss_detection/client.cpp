/*
 * HIL 3-way QoS-matrix comparison, scenario "deadline_miss_detection" (rmw_tickle/COMPARISON.md) -
 * FastDDS native (no rmw) publisher role. See the CycloneDDS twin's own doc comment for the full
 * mechanism this mirrors exactly - publishes at a normal cadence well under the DEADLINE for `-n`
 * samples, except for exactly one deliberately-skipped interval partway through.
 *
 * A DataWriterListener, not a poll-after-sleep (2026-09-20, real methodology bug found on the rig -
 * see the CycloneDDS twin's own doc comment for the full story, same fix applied here): polling
 * right after each nanosleep() call can only ever observe the deadline miss once the deliberate
 * gap has *already* fully elapsed, not when it actually expires partway through - a real run showed
 * `detect_latency_ms=100` for a 150ms gap against a 50ms deadline, an artifact of the poll timing,
 * not a real detection delay. `on_offered_deadline_missed()` runs on FastDDS's own internal thread,
 * genuinely asynchronously to this loop's own blocking sleep.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the
 * publish cadence, the skipped interval, the QoS and the RESULT line are unchanged.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h> // NOLINT(modernize-deprecated-headers) - nanosleep() is POSIX, not in <ctime>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/core/status/DeadlineMissedStatus.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
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
    constexpr double default_deadline_s = 0.05;
    constexpr uint32_t default_count = 100;
    constexpr uint32_t default_miss_at = 50;
    constexpr double match_timeout_s = 10.0;
    constexpr double skipped_interval_deadlines = 3.0; // the deliberate gap, in deadline periods
    constexpr long settle_ns = 200L * 1000L * 1000L;   // 200ms

    std::atomic<uint32_t> g_miss_count {0};
    std::atomic<uint64_t> g_first_miss_detect_ns {0};

    class DeadlineListener : public DataWriterListener {
      public:
        void on_offered_deadline_missed(DataWriter* writer, const OfferedDeadlineMissedStatus& status) override {
            (void)writer;
            const uint64_t now = harness::now_ns();
            uint64_t expected = 0;
            g_first_miss_detect_ns.compare_exchange_strong(expected, now);
            g_miss_count.store(static_cast<uint32_t>(status.total_count));
        }
    };

    struct client_options {
        double interval_s = default_interval_s;
        double deadline_s = default_deadline_s;
        uint32_t count = default_count;
        uint32_t miss_at = default_miss_at;
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
                opts.deadline_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
                opts.count = static_cast<uint32_t>(atoi(argv[++i]));
            } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
                opts.miss_at = static_cast<uint32_t>(atoi(argv[++i]));
            }
        }
        return opts;
    }

    struct publish_result {
        uint64_t sent = 0;
        uint64_t expected_miss_boundary_ns = 0;
    };

    auto publish(DataWriter* writer, const client_options& opts) -> publish_result {
        publish_result result;
        uint32_t seq = 0;
        while (!harness::interrupted() && seq < opts.count) {
            Bench msg;
            msg.seq(++seq);
            msg.send_ns(harness::now_ns());
            writer->write(&msg);
            result.sent++;

            double this_interval_s = opts.interval_s;
            if (seq == opts.miss_at) {
                this_interval_s = opts.deadline_s * skipped_interval_deadlines;
                result.expected_miss_boundary_ns = harness::now_ns() + harness::seconds_to_ns(opts.deadline_s);
                printf("Publisher: deliberately skipping the interval after seq=%u (sleeping %.3fs, deadline=%.3fs)\n",
                       seq, this_interval_s, opts.deadline_s);
            }
            harness::sleep_ns(harness::seconds_to_ns(this_interval_s));
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
    wqos.deadline().period = eprosima::fastrtps::Duration_t(opts.deadline_s);

    static DeadlineListener listener;
    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* const writer = publisher->create_datawriter(topic, wqos, &listener);
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

    const publish_result result = publish(writer, opts);

    // The listener runs asynchronously - give it a brief real window to have fired for the very
    // last period boundary before reading final counts.
    const struct timespec settle = {0, settle_ns};
    nanosleep(&settle, nullptr);

    const uint32_t miss_count = g_miss_count.load();
    const uint64_t first_detect_ns = g_first_miss_detect_ns.load();
    const double detect_latency_ms = (first_detect_ns > 0 && result.expected_miss_boundary_ns > 0)
                                         ? static_cast<double>(static_cast<int64_t>(first_detect_ns) -
                                                               static_cast<int64_t>(result.expected_miss_boundary_ns)) /
                                               harness::ns_per_ms
                                         : -1.0;

    printf("RESULT: framework=fastdds scenario=deadline_miss_detection role=client sent=%lu "
           "offered_missed_total=%u detect_latency_ms=%.3f\n",
           static_cast<unsigned long>(result.sent), miss_count, detect_latency_ms);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
