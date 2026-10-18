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
 * FastDDS native (no rmw) client/sender role. Mirrors the CycloneDDS scenario pair exactly - a
 * one-way stream (no pong), the receiver (server.cpp) is the authoritative side for loss/
 * throughput since only it can see what actually arrived.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the
 * send loop, its pacing, its QoS and the RESULT line are unchanged.
 */
#include <array>
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

#include "../../tickle/common/BenchFreshness.h"
#include "../../tickle/common/BenchHistory.h"
#include "../../tickle/common/BenchStats.h" // shared instrumentation - see its own header
#include "../../tickle/common/BenchWindow.h"
#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr double default_duration_s = 10.0;
    constexpr double match_timeout_s = 10.0;

    // The measured window: -d seconds between a warm-up and a cool-down (BenchWindow.h).
    struct BenchWindow g_window;
    std::array<char, BENCH_WINDOW_FIELDS_MAX> g_window_fields {};

    // -K <depth>: KEEP_LAST depth of the writer and the reader; absent, the product default (BenchHistory.h).
    // KEEP_LAST freshness (BenchFreshness.h): --fresh stamps each write with its creation time - the slot's due time
    // with -i, the write's own stamp at max rate - and --burst B writes B samples back to back per slot (default 1).
    struct bench_fresh_sender g_fresh;
    std::array<char, BENCH_FRESH_FIELDS_MAX> g_fresh_fields {};

    int g_history = BENCH_HISTORY_DEFAULT;
    std::array<char, BENCH_HISTORY_FIELD_MAX> g_history_arg {};
    std::array<char, BENCH_HISTORY_FIELD_MAX> g_history_field {};

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

    struct client_options {
        double duration_s = default_duration_s;
        double interval_s = 0.0; // 0 = as fast as possible
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        BenchWindow_init(&g_window);
        bench_fresh_sender_init(&g_fresh);
        for (int i = 1; i < argc; i++) {
            if (bench_fresh_sender_parse_arg(&g_fresh, argc, argv, &i)) {
                continue;
            }
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.duration_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            } else if (!BenchWindow_parse_arg(&g_window, argc, argv, &i)) {
                (void)BenchHistory_parse_arg(&g_history, argc, argv, &i);
            }
        }
        return opts;
    }

    // Actively waits for a real match instead of a blind sleep - a real, confirmed CycloneDDS
    // discovery bug (COMPARISON.md's own "Blocked" section) made this necessary there; adding it here
    // too on the first real failure of this exact scenario on FastDDS - a blind sleep alone isn't
    // reliable for a write-only participant on a topic other than the two latency scenarios' own
    // "ping"/"pong", confirmed the hard way (0 received, 100% loss, reproduced twice).
    auto wait_for_writer_match(DataWriter* writer, double timeout_s) -> bool {
        return harness::wait_until(timeout_s, [writer] {
            PublicationMatchedStatus status;
            writer->get_publication_matched_status(status);
            return status.current_count > 0;
        });
    }

    // --fresh: the write's creation time - the slot's due time with -i, else the write's own stamp (BenchFreshness.h).
    void fresh_stamp(Bench& msg, int64_t due_wall, uint64_t due) {
        if (!g_fresh.on) {
            return;
        }
        bench_fresh_stamp(&g_fresh, msg.payload().data(), msg.payload().size(),
                          g_fresh.due_schedule ? due_wall : bench_fresh_wall_ns(),
                          g_fresh.due_schedule ? due : msg.send_ns());
    }

    // With -i the schedule is "write, then sleep one interval": a slot is due one interval after the previous slot's
    // last write returned (BenchFreshness.h records it; the pacing is as it was).
    auto send_until(DataWriter* writer, uint64_t start, uint64_t deadline, double interval_s) -> uint64_t {
        uint32_t seq = 0;
        uint64_t sent = 0;
        uint64_t due = start;
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            const int64_t due_wall = g_fresh.on && g_fresh.due_schedule ? bench_fresh_mono_to_wall(due) : 0;
            for (uint32_t k = 0; k < g_fresh.burst; k++) {
                Bench msg;
                msg.seq(++seq);
                msg.send_ns(harness::now_ns());
                fresh_stamp(msg, due_wall, due);
                if (writer->write(&msg)) {
                    sent++;
                    BenchWindow_add(&g_window, msg.send_ns());
                }
            }
            if (interval_s > 0.0) {
                due = g_fresh.on ? harness::now_ns() + harness::seconds_to_ns(interval_s) : due;
                harness::sleep_seconds(interval_s);
            }
        }
        return sent;
    }

} // namespace

auto main(int argc, char** argv) -> int {
    // Armed at the very top, before any middleware setup, so the counters cover discovery
    // too - identically for all three frameworks, which is what makes them comparable.
    bench_stats_begin(&harness::g_bench_stats);
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

    // scenario "best_effort_throughput" - BEST_EFFORT, matched exactly across frameworks
    // (COMPARISON.md's design principle 3).
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    apply_history_depth(wqos, g_history);
    harness::apply_datasharing_policy(wqos);

    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* const writer = publisher->create_datawriter(topic, wqos);
    if (writer == nullptr) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    if (!wait_for_writer_match(writer, match_timeout_s)) {
        fprintf(stderr, "timed out waiting for a matched reader\n");
        return 1;
    }

    const uint64_t start = harness::now_ns();
    // -d is the measured window; the client sends for warm-up + -d + cool-down (BenchWindow.h).
    const double send_s = g_window.warmup_s + opts.duration_s + g_window.cooldown_s;
    // The measured window on this side's schedule, as BenchWindow.h reads it: after the warm-up, for -d seconds.
    g_fresh.due_schedule = opts.interval_s > 0.0;
    const uint64_t win_lo = start + harness::seconds_to_ns(g_window.warmup_s);
    bench_fresh_sender_window(&g_fresh, win_lo, win_lo + harness::seconds_to_ns(opts.duration_s));
    const uint64_t sent = send_until(writer, start, start + harness::seconds_to_ns(send_s), opts.interval_s);

    const double elapsed_s = static_cast<double>(harness::now_ns() - start) / harness::ns_per_s_real;
    const double mbps = harness::mbps(sent, sizeof(Bench), elapsed_s);
    bench_stats_end(&harness::g_bench_stats);
    printf("RESULT: framework=fastdds scenario=best_effort_throughput role=client sent=%lu elapsed_s=%.3f "
           "send_mbps=%.6f %s %s %s %s transport_profile=%s%s\n",
           static_cast<unsigned long>(sent), elapsed_s, mbps,
           BenchWindow_fields(&g_window, "sent", "send", BENCH_SAMPLE_BYTES, g_window_fields.data(),
                              g_window_fields.size()),
           BenchHistory_arg_field(g_history, g_history_arg.data(), g_history_arg.size()),
           history_field(writer->get_qos().history()), harness::bench_fields(BENCH_ROLE_SENDER, sent),
           harness::transport_profile(),
           bench_fresh_sender_fields(&g_fresh, g_fresh_fields.data(), g_fresh_fields.size()));

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
