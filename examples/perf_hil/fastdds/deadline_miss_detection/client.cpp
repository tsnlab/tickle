/*
 * HIL 3-way QoS-matrix comparison, scenario "deadline_miss_detection" (rmw_tickle/comparison.md) -
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
 */
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fastdds/dds/core/status/DeadlineMissedStatus.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastrtps/utils/TimeConversion.h>

#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int) {
    g_interrupted = 1;
}

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static std::atomic<uint32_t> g_miss_count {0};
static std::atomic<uint64_t> g_first_miss_detect_ns {0};

class DeadlineListener : public DataWriterListener {
  public:
    void on_offered_deadline_missed(DataWriter* writer, const OfferedDeadlineMissedStatus& status) override {
        (void)writer;
        uint64_t now = now_ns();
        uint64_t expected = 0;
        g_first_miss_detect_ns.compare_exchange_strong(expected, now);
        g_miss_count.store((uint32_t)status.total_count);
    }
};

int main(int argc, char** argv) {
    double interval_s = 0.02;
    double deadline_s = 0.05;
    uint32_t count = 100;
    uint32_t miss_at = 50;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            deadline_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            miss_at = (uint32_t)atoi(argv[++i]);
        }
    }

    std::signal(SIGINT, handle_sigint);

    DomainParticipant* participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (!participant) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }

    TypeSupport type(new BenchPubSubType());
    type.register_type(participant);

    Topic* topic = participant->create_topic("stream", "Bench", TOPIC_QOS_DEFAULT);

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.deadline().period = eprosima::fastrtps::Duration_t(deadline_s);

    static DeadlineListener listener;
    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* writer = publisher->create_datawriter(topic, wqos, &listener);
    if (!writer) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    // Match-wait, this exercise's own established FastDDS idiom.
    {
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        bool matched = false;
        for (;;) {
            PublicationMatchedStatus status;
            writer->get_publication_matched_status(status);
            if (status.current_count > 0) {
                matched = true;
                break;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
            if (elapsed >= 10.0) {
                break;
            }
            struct timespec poll_interval = {0, 50 * 1000 * 1000};
            nanosleep(&poll_interval, nullptr);
        }
        if (!matched) {
            fprintf(stderr, "timed out waiting for a matched reader\n");
            return 1;
        }
    }

    uint32_t seq = 0;
    uint64_t sent = 0;
    uint64_t expected_miss_boundary_ns = 0;

    while (!g_interrupted && seq < count) {
        Bench msg;
        msg.seq(++seq);
        msg.send_ns(now_ns());
        writer->write(&msg);
        sent++;

        double this_interval_s = interval_s;
        if (seq == miss_at) {
            this_interval_s = deadline_s * 3.0;
            expected_miss_boundary_ns = now_ns() + (uint64_t)(deadline_s * 1e9);
            printf("Publisher: deliberately skipping the interval after seq=%u (sleeping %.3fs, deadline=%.3fs)\n", seq,
                   this_interval_s, deadline_s);
        }
        uint64_t sleep_ns = (uint64_t)(this_interval_s * 1e9);
        struct timespec pace = {(time_t)(sleep_ns / 1000000000ULL), (long)(sleep_ns % 1000000000ULL)};
        nanosleep(&pace, nullptr);
    }

    // The listener runs asynchronously - give it a brief real window to have fired for the very
    // last period boundary before reading final counts.
    struct timespec settle = {0, 200 * 1000 * 1000};
    nanosleep(&settle, nullptr);

    uint32_t miss_count = g_miss_count.load();
    uint64_t first_detect_ns = g_first_miss_detect_ns.load();
    double detect_latency_ms = (first_detect_ns > 0 && expected_miss_boundary_ns > 0)
                                   ? (double)((int64_t)first_detect_ns - (int64_t)expected_miss_boundary_ns) / 1e6
                                   : -1.0;

    printf("RESULT: framework=fastdds scenario=deadline_miss_detection role=client sent=%lu "
           "offered_missed_total=%u detect_latency_ms=%.3f\n",
           (unsigned long)sent, miss_count, detect_latency_ms);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
