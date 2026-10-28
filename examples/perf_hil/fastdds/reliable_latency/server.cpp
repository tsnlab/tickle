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
 * FastDDS native (no rmw) server/pong role. Subscribes on "ping", republishes the exact same
 * sample (same seq/send_ns) on "pong" immediately - mirrors the CycloneDDS scenario pair exactly
 * (COMPARISON.md's design principle 1: identical data/behavior, only the framework differs).
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the
 * echo loop, its QoS and its timing are unchanged.
 */
#include <cstdint>
#include <cstdio>

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

#include "../../tickle/common/LatencyReliability.h" // best_effort_latency is this file with BEST_EFFORT
#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr int32_t history_depth = 8;

    // Echoes whatever arrives on the reader straight back on the writer, until SIGINT.
    void echo_until_interrupted(DataReader* reader, DataWriter* writer) {
        Bench sample;
        SampleInfo info;
        while (!harness::interrupted()) {
            const eprosima::fastrtps::Duration_t timeout {1, 0};
            if (reader->wait_for_unread_message(timeout)) {
                if (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK && info.valid_data) {
                    writer->write(&sample);
                }
            }
        }
    }

} // namespace

auto main() -> int {
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

    // scenario "reliable_latency" - RELIABLE on both sides, matching CycloneDDS's own QoS exactly
    // (COMPARISON.md's design principle 3). This comment said BEST_EFFORT until 2026-09-25, above code
    // that has always set RELIABLE; the code was right. best_effort_latency: the same with BEST_EFFORT
    // (LatencyReliability.h).
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = history_depth;
    rqos.reliability().kind = BENCH_LATENCY_BEST_EFFORT ? BEST_EFFORT_RELIABILITY_QOS : RELIABLE_RELIABILITY_QOS;
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = history_depth;
    wqos.reliability().kind = BENCH_LATENCY_BEST_EFFORT ? BEST_EFFORT_RELIABILITY_QOS : RELIABLE_RELIABILITY_QOS;

    Subscriber* const subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataReader* const reader = subscriber->create_datareader(ping_topic, rqos);
    DataWriter* const writer = publisher->create_datawriter(pong_topic, wqos);
    if (reader == nullptr || writer == nullptr) {
        fprintf(stderr, "create_datareader/datawriter failed\n");
        return 1;
    }

    // Not a RESULT line (the DDS latency servers print none); the treatment, read back, for the server's log.
    printf("FastDDS " BENCH_LATENCY_SCENARIO " server: reader on 'ping', writer on 'pong', reliability=%s\n",
           bench_reliability_name(writer->get_qos().reliability().kind == RELIABLE_RELIABILITY_QOS ? 1 : 0,
                                  reader->get_qos().reliability().kind == RELIABLE_RELIABILITY_QOS ? 1 : 0));

    echo_until_interrupted(reader, writer);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
