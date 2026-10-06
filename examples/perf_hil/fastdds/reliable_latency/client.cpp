/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_latency" (rmw_tickle/COMPARISON.md) -
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h> // NOLINT(modernize-deprecated-headers) - nanosleep() is POSIX, not in <ctime>

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
#include "../../tickle/common/BenchWindow.h"
#include "../../tickle/common/CpuFreq.h"
#include "../../tickle/common/RttQuantiles.h"
#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr double default_interval_s = 1.0;
    constexpr double default_duration_s = 10.0;
    constexpr int32_t history_depth = 8;
    constexpr uint32_t response_wait_ns = 500U * 1000U * 1000U; // 500ms
    constexpr time_t discovery_wait_s = 2;                      // see the CycloneDDS client's identical comment

    // CPU frequency around each round trip (2026-09-25). A tail excursion after the scheduler-driven poll
    // has two platform explanations besides the change itself: an ordinary loss recovery, or the ondemand
    // governor lowering the package clock once the client stopped spinning and a P-state change - made
    // through the firmware, with a latency the kernel reports as unknown - landing on a round trip.
    // Sampled AFTER the round trip is recorded, never between send and receive, so it cannot perturb what
    // it measures. Identical in all three frameworks' clients, per the fairness rule.
    struct BenchCpuFreq g_rtt_freq;
    struct BenchRtt g_rtt;

    // The whole run's counts (transmitted, received) and the measured window's (BenchWindow.h): only a measured round
    // trip enters the RTT statistics.
    struct rtt_stats {
        uint64_t transmitted = 0;
        uint64_t received = 0;
        uint64_t measured_sent = 0;
        uint64_t measured_recv = 0;
        uint32_t warmup_sent = 0;
        uint32_t cooldown_sent = 0;
        double min_ms = -1.0;
        double max_ms = 0.0;
        double sum_ms = 0.0;
        double cpu_mhz_at_max = -1.0;
    };

    struct client_options {
        double interval_s = default_interval_s;
        double duration_s = default_duration_s; // the measured window, between warm-up and cool-down
        uint32_t warmup_rtts = BENCH_WARMUP_ROUND_TRIPS;
        uint32_t cooldown_rtts = BENCH_COOLDOWN_ROUND_TRIPS;
        double edge_interval_s = BENCH_EDGE_INTERVAL_S;
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.duration_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
                opts.warmup_rtts = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
            } else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
                opts.cooldown_rtts = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
            } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
                opts.edge_interval_s = atof(argv[++i]);
            }
        }
        return opts;
    }

    void record_rtt(rtt_stats& stats, double rtt_ms, bool measured) {
        stats.received++;
        if (!measured) {
            return; // a warm-up or cool-down round trip: counted in recv=, kept out of every statistic
        }
        stats.measured_recv++;
        if (stats.min_ms < 0.0 || rtt_ms < stats.min_ms) {
            stats.min_ms = rtt_ms;
        }
        const bool new_max = rtt_ms > stats.max_ms;
        if (new_max) {
            stats.max_ms = rtt_ms;
        }
        stats.sum_ms += rtt_ms;
        BenchRtt_add(&g_rtt, rtt_ms);
        BenchCpuFreq_sample(&g_rtt_freq, harness::now_ns(), 0);
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

    void ping_once(DataWriter* writer, DataReader* reader, uint32_t seq, bool measured, rtt_stats& stats) {
        Bench req;
        req.seq(seq);
        req.send_ns(harness::now_ns());
        writer->write(&req);
        stats.transmitted++;
        if (measured) {
            stats.measured_sent++;
        }

        const eprosima::fastrtps::Duration_t timeout {0, response_wait_ns};
        if (!reader->wait_for_unread_message(timeout)) {
            return;
        }
        Bench resp;
        if (take_newest(reader, resp) && resp.seq() == req.seq()) {
            record_rtt(stats, static_cast<double>(harness::now_ns() - resp.send_ns()) / harness::ns_per_ms, measured);
        }
    }

    // Warm-up, measured window, cool-down (BenchWindow.h), paced exactly as the TickLE and CycloneDDS clients pace
    // them: the edge interval between edge round trips, the measured -i before every measured ping including the
    // first, and before the first cool-down ping.
    void run_phases(DataWriter* writer, DataReader* reader, const client_options& opts, rtt_stats& stats) {
        uint32_t seq = 0;
        while (!harness::interrupted() && stats.warmup_sent < opts.warmup_rtts) {
            ping_once(writer, reader, ++seq, false, stats);
            stats.warmup_sent++;
            harness::sleep_seconds(stats.warmup_sent < opts.warmup_rtts ? opts.edge_interval_s : opts.interval_s);
        }
        const uint64_t deadline = harness::now_ns() + harness::seconds_to_ns(opts.duration_s);
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            ping_once(writer, reader, ++seq, true, stats);
            harness::sleep_seconds(opts.interval_s);
        }
        while (!harness::interrupted() && stats.cooldown_sent < opts.cooldown_rtts) {
            ping_once(writer, reader, ++seq, false, stats);
            stats.cooldown_sent++;
            harness::sleep_seconds(opts.edge_interval_s);
        }
    }

    void report(const rtt_stats& stats, const client_options& opts) {
        const uint64_t lost = stats.transmitted - stats.received;
        const double loss_pct =
            stats.transmitted > 0
                ? (harness::percent * static_cast<double>(lost) / static_cast<double>(stats.transmitted))
                : 0.0;
        const double avg = stats.measured_recv > 0 ? stats.sum_ms / static_cast<double>(stats.measured_recv) : 0.0;

        printf("\n--- fastdds reliable_latency statistics ---\n");
        printf("%lu sent, %lu received, %.0f%% loss\n", static_cast<unsigned long>(stats.transmitted),
               static_cast<unsigned long>(stats.received), loss_pct);
        if (stats.measured_recv > 0) {
            printf("rtt min/avg/max = %.3f/%.3f/%.3f ms over %lu measured round trips\n", stats.min_ms, avg,
                   stats.max_ms, static_cast<unsigned long>(stats.measured_recv));
        }
        bench_stats_end(&harness::g_bench_stats);
        // transport_profile= is the arm's identity, and until 2026-10-02 only the throughput client printed it.
        // S6's latency cell checks each rep's RESULT line for it, so without this field that check could never pass
        // - the FastDDS arm was refused on every repetition and the cell reported nothing, which reads as a FastDDS
        // finding and is a harness defect. Same source as the throughput client's, so the two cells name the arm the
        // same way.
        printf("RESULT: framework=fastdds scenario=reliable_latency sent=%lu recv=%lu loss_pct=%.0f "
               "rtt_min_ms=%.3f rtt_avg_ms=%.3f rtt_max_ms=%.3f cpu_mhz_mean=%.1f cpu_mhz_min=%.1f cpu_mhz_max=%.1f "
               "cpu_mhz_at_rtt_max=%.1f rtt_p50_ms=%.3f rtt_p99_ms=%.3f rtt_kept=%u "
               "warmup=%u cooldown=%u measured=%lu measured_sent=%lu edge_interval_s=%.4f window=%s "
               "transport_profile=%s %s\n",
               static_cast<unsigned long>(stats.transmitted), static_cast<unsigned long>(stats.received), loss_pct,
               stats.min_ms, avg, stats.max_ms, BenchCpuFreq_mean_mhz(&g_rtt_freq), BenchCpuFreq_min_mhz(&g_rtt_freq),
               BenchCpuFreq_max_mhz(&g_rtt_freq), stats.cpu_mhz_at_max, BenchRtt_quantile(&g_rtt, BENCH_RTT_P50),
               BenchRtt_quantile(&g_rtt, BENCH_RTT_P99), static_cast<unsigned>(g_rtt.count), stats.warmup_sent,
               stats.cooldown_sent, static_cast<unsigned long>(stats.measured_recv),
               static_cast<unsigned long>(stats.measured_sent), opts.edge_interval_s,
               stats.measured_recv > 0 ? "ok" : "fail:no_measured_round_trip", harness::transport_profile(),
               harness::bench_fields(BENCH_ROLE_SENDER, stats.transmitted));
    }

} // namespace

auto main(int argc, char** argv) -> int {
    bench_stats_begin(&harness::g_bench_stats);
    BenchCpuFreq_init(&g_rtt_freq);
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
    run_phases(writer, reader, opts, stats);

    report(stats, opts);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
