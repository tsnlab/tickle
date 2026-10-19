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
 * HIL 3-way QoS-matrix comparison, scenario "liveliness_loss_detection" (rmw_tickle/COMPARISON.md) -
 * FastDDS native (no rmw) subscriber role. See the CycloneDDS twin's own doc comment for the full
 * mechanism and, crucially, why both timestamps compared here (last received sample, loss
 * detected) are taken on this same host's own clock - never against the writer's own kill time on
 * a different host, the exact cross-host clock-sync pitfall this exercise's own earlier work
 * ("Item 5" in COMPARISON.md) got burned by once already.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the listener,
 * the detection wait, the QoS and the RESULT line are unchanged.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h> // NOLINT(modernize-deprecated-headers) - nanosleep() is POSIX, not in <ctime>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/core/status/LivelinessChangedStatus.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
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

    constexpr double default_lease_s = 2.0;
    constexpr double default_safety_cap_s = 30.0;
    constexpr double announcements_per_lease = 3.0;
    constexpr double match_timeout_s = 10.0;
    constexpr long detect_poll_ns = 50L * 1000L * 1000L; // 50ms
    constexpr long settle_ns = 300L * 1000L * 1000L;     // 300ms

    // Written by the listener on a Fast DDS thread and read by main(), so atomics: main() then has a happens-before
    // edge to what the listener stored (a data race ThreadSanitizer reported on 2026-10-20; COMPARISON to-do 16).
    // Sequentially consistent, and the listener stores the detection time before the flag, so a main() that sees the
    // flag reads the time with it. Nothing measurable at this scenario's 10 samples/s.
    std::atomic<uint64_t> g_last_recv_ns {0};
    std::atomic<uint64_t> g_loss_detect_ns {0};
    std::atomic<bool> g_loss_detected {false};

    class LivelinessListener : public DataReaderListener {
      public:
        void on_liveliness_changed(DataReader* reader, const LivelinessChangedStatus& status) override {
            (void)reader;
            if (status.not_alive_count > 0 && !g_loss_detected.load()) {
                g_loss_detect_ns.store(harness::now_ns());
                g_loss_detected.store(true);
            }
        }
        void on_data_available(DataReader* reader) override {
            Bench sample;
            SampleInfo info;
            while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
                if (info.valid_data) {
                    received_count.fetch_add(1);
                    g_last_recv_ns.store(harness::now_ns());
                }
            }
        }
        [[nodiscard]] auto received() const -> uint64_t {
            return received_count.load();
        }

      private:
        std::atomic<uint64_t> received_count {0};
    };

    struct server_options {
        double lease_s = default_lease_s;
        double safety_cap_s = default_safety_cap_s;
    };

    auto parse_options(int argc, char** argv) -> server_options {
        server_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
                opts.lease_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.safety_cap_s = atof(argv[++i]);
            }
        }
        return opts;
    }

    auto reader_qos(double lease_s) -> DataReaderQos {
        DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
        rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        rqos.liveliness().kind = AUTOMATIC_LIVELINESS_QOS;
        rqos.liveliness().lease_duration = eprosima::fastrtps::Duration_t(lease_s);
        // See client.cpp's own doc comment for why this is required, not a tuning choice.
        rqos.liveliness().announcement_period = eprosima::fastrtps::Duration_t(lease_s / announcements_per_lease);
        return rqos;
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

    static LivelinessListener listener;
    Subscriber* const subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* const reader = subscriber->create_datareader(topic, reader_qos(opts.lease_s), &listener);
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

    const uint64_t deadline = harness::now_ns() + harness::seconds_to_ns(opts.safety_cap_s);
    while (!harness::interrupted() && harness::now_ns() < deadline && !g_loss_detected.load()) {
        const struct timespec poll_interval = {0, detect_poll_ns};
        nanosleep(&poll_interval, nullptr);
    }

    // A brief real settle window - the loss listener may fire a few ms after this poll loop's own
    // last check.
    const struct timespec settle = {0, settle_ns};
    nanosleep(&settle, nullptr);

    // Each read once, the flag first: it is the listener's last store.
    const bool loss_detected = g_loss_detected.load();
    const uint64_t loss_detect_ns = g_loss_detect_ns.load();
    const uint64_t last_recv_ns = g_last_recv_ns.load();
    const double detect_latency_ms = (loss_detected && last_recv_ns > 0)
                                         ? static_cast<double>(loss_detect_ns - last_recv_ns) / harness::ns_per_ms
                                         : -1.0;

    printf("RESULT: framework=fastdds scenario=liveliness_loss_detection role=server recv=%lu "
           "loss_detected=%d detect_latency_ms=%.3f lease_s=%.3f transport_profile=%s\n",
           static_cast<unsigned long>(listener.received()), static_cast<int>(loss_detected), detect_latency_ms,
           opts.lease_s, harness::transport_profile());

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
