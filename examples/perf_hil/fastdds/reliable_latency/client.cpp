/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_latency" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) client/ping role. Mirrors the CycloneDDS scenario pair exactly - same
 * RESULT line shape as TickLE's own examples/linux/ping_pong/ping.c, framework field added for
 * this exercise's own dashboard parsing.
 *
 * Structured into small functions in an anonymous namespace (2026-09-25) so that it passes the
 * repository's own C++ clang-tidy checks, which the rmw_tickle C++ code already meets. It was never
 * linted before: until the CI step that builds the perf_hil harnesses stopped dying on the CycloneDDS
 * block, the FastDDS harness had no compile-database entries for clang-tidy to use. The logic, the
 * timing and the RESULT line are unchanged.
 */
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h> // NOLINT(modernize-deprecated-headers) - clock_gettime()/nanosleep() are POSIX, not in <ctime>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
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

#include "../../tickle/common/BenchStats.h" // shared instrumentation - see its own header
#include "../../tickle/common/CpuFreq.h"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr uint64_t ns_per_s = 1000000000ULL;
    constexpr double ns_per_s_real = 1e9;
    constexpr double ns_per_ms = 1e6;
    constexpr double percent = 100.0;
    constexpr double default_interval_s = 1.0;
    constexpr double default_duration_s = 10.0;
    constexpr int32_t history_depth = 8;
    constexpr uint32_t response_wait_ns = 500U * 1000U * 1000U; // 500ms
    constexpr time_t discovery_wait_s = 2;                      // see the CycloneDDS client's identical comment

    volatile sig_atomic_t g_interrupted = 0;

    void handle_sigint(int signum) {
        (void)signum;
        g_interrupted = 1;
    }

    auto now_ns() -> uint64_t {
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - glibc defines it in bits/time.h
        return (static_cast<uint64_t>(ts.tv_sec) * ns_per_s) + static_cast<uint64_t>(ts.tv_nsec);
    }

    // Armed at the very top of main(), before any middleware setup, so the counters cover discovery too -
    // identically for all three frameworks, which is what makes them comparable.
    struct BenchStats g_bench_stats;
    std::array<char, BENCH_STATS_FIELDS_MAX> g_bench_fields {};

    // CPU frequency around each round trip (2026-09-25). A tail excursion after the scheduler-driven poll
    // has two platform explanations besides the change itself: an ordinary loss recovery, or the ondemand
    // governor lowering the package clock once the client stopped spinning and a P-state change - made
    // through the firmware, with a latency the kernel reports as unknown - landing on a round trip.
    // Sampled AFTER the round trip is recorded, never between send and receive, so it cannot perturb what
    // it measures. Identical in all three frameworks' clients, per the fairness rule.
    struct BenchCpuFreq g_rtt_freq;

    struct rtt_stats {
        uint64_t transmitted = 0;
        uint64_t received = 0;
        double min_ms = -1.0;
        double max_ms = 0.0;
        double sum_ms = 0.0;
        double cpu_mhz_at_max = -1.0;
    };

    struct client_options {
        double interval_s = default_interval_s;
        double duration_s = default_duration_s;
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.duration_s = atof(argv[++i]);
            }
        }
        return opts;
    }

    void record_rtt(rtt_stats& stats, double rtt_ms) {
        stats.received++;
        if (stats.min_ms < 0.0 || rtt_ms < stats.min_ms) {
            stats.min_ms = rtt_ms;
        }
        const bool new_max = rtt_ms > stats.max_ms;
        if (new_max) {
            stats.max_ms = rtt_ms;
        }
        stats.sum_ms += rtt_ms;
        BenchCpuFreq_sample(&g_rtt_freq, now_ns(), 0);
        if (new_max) {
            stats.cpu_mhz_at_max = BenchCpuFreq_last_mhz(&g_rtt_freq);
        }
    }

    // Drain every currently-buffered sample, keeping only the newest - a real, confirmed FastDDS behavior
    // found while debugging this exact loop: DataReader's own default history can hold more than one
    // not-yet-taken sample, and take_next_sample() is FIFO (oldest first), so a single take() here would
    // return an already-stale response (matching an *older* request) whenever the reader had more than one
    // queued, not the fresh one this request's own RTT should be measured against.
    auto take_newest(DataReader* reader, Bench& resp) -> bool {
        SampleInfo info;
        bool have_fresh = false;
        while (reader->take_next_sample(&resp, &info) == ReturnCode_t::RETCODE_OK) {
            if (info.valid_data) {
                have_fresh = true;
            }
        }
        return have_fresh;
    }

    void ping_once(DataWriter* writer, DataReader* reader, uint32_t seq, rtt_stats& stats) {
        Bench req;
        req.seq(seq);
        req.send_ns(now_ns());
        writer->write(&req);
        stats.transmitted++;

        const eprosima::fastrtps::Duration_t timeout {0, response_wait_ns};
        if (!reader->wait_for_unread_message(timeout)) {
            return;
        }
        Bench resp;
        if (take_newest(reader, resp) && resp.seq() == req.seq()) {
            record_rtt(stats, static_cast<double>(now_ns() - resp.send_ns()) / ns_per_ms);
        }
    }

    void report(const rtt_stats& stats) {
        const uint64_t lost = stats.transmitted - stats.received;
        const double loss_pct = stats.transmitted > 0
                                    ? (percent * static_cast<double>(lost) / static_cast<double>(stats.transmitted))
                                    : 0.0;
        const double avg = stats.received > 0 ? stats.sum_ms / static_cast<double>(stats.received) : 0.0;

        printf("\n--- fastdds reliable_latency statistics ---\n");
        printf("%lu sent, %lu received, %.0f%% loss\n", static_cast<unsigned long>(stats.transmitted),
               static_cast<unsigned long>(stats.received), loss_pct);
        if (stats.received > 0) {
            printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", stats.min_ms, avg, stats.max_ms);
        }
        bench_stats_end(&g_bench_stats);
        printf("RESULT: framework=fastdds scenario=reliable_latency sent=%lu recv=%lu loss_pct=%.0f "
               "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f cpu_mhz_mean=%.1f cpu_mhz_min=%.1f cpu_mhz_max=%.1f "
               "cpu_mhz_at_rtt_max=%.1f %s\n",
               static_cast<unsigned long>(stats.transmitted), static_cast<unsigned long>(stats.received), loss_pct,
               stats.min_ms, avg, stats.max_ms, BenchCpuFreq_mean_mhz(&g_rtt_freq), BenchCpuFreq_min_mhz(&g_rtt_freq),
               BenchCpuFreq_max_mhz(&g_rtt_freq), stats.cpu_mhz_at_max,
               bench_stats_fields(&g_bench_stats, BENCH_ROLE_SENDER, stats.transmitted, BENCH_SAMPLE_BYTES,
                                  g_bench_fields.data(), g_bench_fields.size()));
    }

} // namespace

auto main(int argc, char** argv) -> int {
    bench_stats_begin(&g_bench_stats);
    BenchCpuFreq_init(&g_rtt_freq);
    const client_options opts = parse_options(argc, argv);

    std::signal(SIGINT, handle_sigint);

    DomainParticipant* const participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (participant == nullptr) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }

    const TypeSupport type(new BenchPubSubType());
    type.register_type(participant);

    Topic* const ping_topic = participant->create_topic("ping", "Bench", TOPIC_QOS_DEFAULT);
    Topic* const pong_topic = participant->create_topic("pong", "Bench", TOPIC_QOS_DEFAULT);

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = history_depth;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = history_depth;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;

    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    Subscriber* const subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataWriter* const writer = publisher->create_datawriter(ping_topic, wqos);
    DataReader* const reader = subscriber->create_datareader(pong_topic, rqos);
    if (writer == nullptr || reader == nullptr) {
        fprintf(stderr, "create_datawriter/datareader failed\n");
        return 1;
    }

    const struct timespec discovery_wait = {discovery_wait_s, 0};
    nanosleep(&discovery_wait, nullptr);

    rtt_stats stats;
    uint32_t seq = 0;
    const uint64_t deadline = now_ns() + static_cast<uint64_t>(opts.duration_s * ns_per_s_real);
    const auto interval_ns = static_cast<uint64_t>(opts.interval_s * ns_per_s_real);
    while (g_interrupted == 0 && now_ns() < deadline) {
        ping_once(writer, reader, ++seq, stats);
        const struct timespec sleep_ts = {static_cast<time_t>(interval_ns / ns_per_s),
                                          static_cast<long>(interval_ns % ns_per_s)};
        nanosleep(&sleep_ts, nullptr);
    }

    report(stats);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
