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
 * HIL 3-way QoS-matrix comparison, scenario "best_effort_throughput" (rmw_tickle/COMPARISON.md) -
 * FastDDS native (no rmw) server/receiver role. The authoritative side for loss/throughput - only
 * it can see what actually arrived (see client.cpp's own doc comment).
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the
 * receive loop, its loss accounting, its QoS and the RESULT line are unchanged.
 */
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/core/status/BaseStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/subscriber/qos/SubscriberQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>
#include <fastdds/rtps/common/Time_t.h>

#include "../../tickle/common/BenchHistory.h"
#include "../../tickle/common/BenchStats.h" // shared instrumentation - see its own header
#include "../../tickle/common/BenchWindow.h"
#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr double default_safety_cap_s = 40.0;
    constexpr double safety_cap_buffer_s = 15.0;

    // What arrived, windowed by the sender's own clock exactly as the client windows what it sent (BenchWindow.h).
    struct BenchWindow g_window;
    std::array<char, BENCH_WINDOW_FIELDS_MAX> g_window_fields {};

    // -K <depth>: KEEP_LAST depth of the writer and the reader; absent, the product default (BenchHistory.h).
    int g_history = BENCH_HISTORY_DEFAULT;
    std::array<char, BENCH_HISTORY_FIELD_MAX> g_history_arg {};
    std::array<char, BENCH_HISTORY_FIELD_MAX> g_history_field {};

    // Fast DDS's own account of the samples it threw away (examples/perf_hil/experiments/fastdds_be_delivery.sh).
    //
    // fdds_sample_lost= is always printed: the reader's SAMPLE_LOST status, which 2.14's StatelessReader raises for a
    // gap in the sequence numbers it was NOTIFIED of. A sample that reached the reader's history and was dropped
    // afterwards is not in it - which is what makes it worth printing beside our own lost=.
    //
    // fdds_warn_*= only under BENCH_FASTDDS_COUNT_WARNINGS=1, because it changes the run: it raises Fast DDS's log
    // verbosity to Warning, so every discard below costs a formatted string and a trip through the log thread. The
    // three are the warnings 2.14.6 logs when a data-sharing reader discards a sample the writer has reused:
    //   overridden - ReadTakeCommand::check_datasharing_validity, at take: "Change <sn> from <guid> is overidden"
    //   dirty      - ReaderPool::get_next_unread_payload: "Dirty data detected on datasharing writer"
    //   overtook   - ReaderPool::ensure_reading_reference_is_in_bounds: "overtook reader in datasharing pool"
    // Matched on the message text because that is all a LogConsumer is given; a text that stopped matching would
    // read as zero, so the arm that uses this also has to show the counter is alive (fdds_warn_total > 0 on F1).
    struct discard_counts {
        std::atomic<uint64_t> overridden {0};
        std::atomic<uint64_t> dirty {0};
        std::atomic<uint64_t> overtook {0};
        std::atomic<uint64_t> total {0}; // every warning or error that reached the consumer
    };
    discard_counts g_discards;
    bool g_count_warnings = false;
    constexpr size_t discard_fields_max = 192;
    std::array<char, discard_fields_max> g_discard_fields {};

    class DiscardCounter : public LogConsumer {
      public:
        void Consume(const Log::Entry& entry) override {
            g_discards.total.fetch_add(1, std::memory_order_relaxed);
            const std::string& msg = entry.message;
            if (msg.find("is overidden") != std::string::npos) {
                g_discards.overridden.fetch_add(1, std::memory_order_relaxed);
            } else if (msg.find("Dirty data detected") != std::string::npos) {
                g_discards.dirty.fetch_add(1, std::memory_order_relaxed);
            } else if (msg.find("overtook reader") != std::string::npos) {
                g_discards.overtook.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    void arm_warning_counter() {
        const char* flag = std::getenv("BENCH_FASTDDS_COUNT_WARNINGS");
        g_count_warnings = flag != nullptr && flag[0] != '\0' && flag[0] != '0';
        if (!g_count_warnings) {
            return;
        }
        Log::ClearConsumers(); // the default stdout consumer would print every discard
        Log::RegisterConsumer(std::make_unique<DiscardCounter>());
        Log::SetVerbosity(Log::Kind::Warning);
    }

    auto discard_fields(const DataReader* reader) -> const char* {
        SampleLostStatus lost;
        const bool have_lost = reader->get_sample_lost_status(lost) == ReturnCode_t::RETCODE_OK;
        if (!g_count_warnings) {
            snprintf(g_discard_fields.data(), g_discard_fields.size(), "fdds_sample_lost=%d fdds_warn=off",
                     have_lost ? static_cast<int>(lost.total_count) : -1);
            return g_discard_fields.data();
        }
        Log::Flush(); // the consumer runs on the log thread; what is still queued has not been counted
        snprintf(g_discard_fields.data(), g_discard_fields.size(),
                 "fdds_sample_lost=%d fdds_warn=on fdds_warn_overridden=%lu fdds_warn_dirty=%lu "
                 "fdds_warn_overtook=%lu fdds_warn_total=%lu",
                 have_lost ? static_cast<int>(lost.total_count) : -1,
                 static_cast<unsigned long>(g_discards.overridden.load()),
                 static_cast<unsigned long>(g_discards.dirty.load()),
                 static_cast<unsigned long>(g_discards.overtook.load()),
                 static_cast<unsigned long>(g_discards.total.load()));
        return g_discard_fields.data();
    }

    // -K <depth> (tickle/common/BenchHistory.h): KEEP_LAST at that depth, with the resource limits raised to hold it.
    // Fast DDS 2.14's default max_samples_per_instance is 400 (max_samples 5000), and a depth above it is an
    // inconsistent QoS the entity refuses to be created with. Without -K nothing is set: the product default.
    template <typename Qos> void apply_history_depth(Qos& qos, int depth) {
        if (depth <= 0) {
            return;
        }
        qos.history().kind = KEEP_LAST_HISTORY_QOS;
        qos.history().depth = depth;
        if (qos.resource_limits().max_samples_per_instance < depth) {
            qos.resource_limits().max_samples_per_instance = depth;
        }
        if (qos.resource_limits().max_samples < depth) {
            qos.resource_limits().max_samples = depth;
        }
    }

    // The history the entity actually runs, read back from it rather than from the QoS that was passed in.
    auto history_field(const HistoryQosPolicy& history) -> const char* {
        if (history.kind == KEEP_ALL_HISTORY_QOS) {
            snprintf(g_history_field.data(), g_history_field.size(), "history=keep_all");
        } else {
            snprintf(g_history_field.data(), g_history_field.size(), "history=keep_last:%d",
                     static_cast<int>(history.depth));
        }
        return g_history_field.data();
    }

    auto parse_safety_cap(int argc, char** argv) -> double {
        double safety_cap_s = default_safety_cap_s;
        BenchWindow_init(&g_window);
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                safety_cap_s = atof(argv[++i]);
            } else if (!BenchWindow_parse_arg(&g_window, argc, argv, &i)) {
                (void)BenchHistory_parse_arg(&g_history, argc, argv, &i);
            }
        }
        // The client sends for warm-up + -d + cool-down, so this side's backstop covers all three.
        safety_cap_s += g_window.warmup_s + g_window.cooldown_s;
        // +15s buffer (2026-09-21, real bug found the hard way - see the identical fix on the TickLE
        // twin's own server.c for the full story): run_scenario.sh forwards the same -d to both
        // sides, but it means "the client's own send duration" there vs. "this side's own don't-hang-
        // forever cap" here - taken verbatim, this side could exit and stop counting before the
        // client (which starts several seconds later, run_scenario.sh's own pre-client sleep) had
        // even finished sending.
        return safety_cap_s + safety_cap_buffer_s;
    }

    void receive_until(DataReader* reader, uint64_t deadline, harness::stream_stats& stats) {
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            const eprosima::fastrtps::Duration_t timeout {1, 0};
            if (!reader->wait_for_unread_message(timeout)) {
                continue;
            }
            Bench sample;
            SampleInfo info;
            while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
                if (info.valid_data) {
                    harness::count_sample(stats, sample.seq());
                    BenchWindow_add(&g_window, sample.send_ns());
                }
            }
        }
    }

    void report(const harness::stream_stats& stats, const DataReader* reader) {
        const double elapsed_s = harness::stream_elapsed_s(stats);
        const double loss_pct = harness::stream_loss_pct(stats);
        const double mbps = harness::mbps(stats.received, sizeof(Bench), elapsed_s);

        bench_stats_end(&harness::g_bench_stats);

        printf("RESULT: framework=fastdds scenario=best_effort_throughput role=server recv=%lu lost=%lu "
               "loss_pct=%.6f elapsed_s=%.3f recv_mbps=%.6f %s %s %s %s %s transport_profile=%s\n",
               static_cast<unsigned long>(stats.received), static_cast<unsigned long>(stats.lost), loss_pct, elapsed_s,
               mbps,
               BenchWindow_fields(&g_window, "recv", "recv", BENCH_SAMPLE_BYTES, g_window_fields.data(),
                                  g_window_fields.size()),
               BenchHistory_arg_field(g_history, g_history_arg.data(), g_history_arg.size()),
               history_field(reader->get_qos().history()), discard_fields(reader),
               harness::bench_fields(BENCH_ROLE_RECEIVER, stats.received), harness::transport_profile());
    }

} // namespace

auto main(int argc, char** argv) -> int {
    // Armed at the very top, before any middleware setup, so the counters cover discovery
    // too - identically for all three frameworks, which is what makes them comparable.
    bench_stats_begin(&harness::g_bench_stats);
    const double safety_cap_s = parse_safety_cap(argc, argv);

    harness::install_sigint_handler();
    arm_warning_counter(); // before any entity exists, so no discard is logged before the consumer is in place

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
    rqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    apply_history_depth(rqos, g_history);
    harness::apply_datasharing_policy(rqos);

    Subscriber* const subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* const reader = subscriber->create_datareader(topic, rqos);
    if (reader == nullptr) {
        fprintf(stderr, "create_datareader failed\n");
        return 1;
    }

    harness::stream_stats stats;
    receive_until(reader, harness::now_ns() + harness::seconds_to_ns(safety_cap_s), stats);
    report(stats, reader);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
