/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Fast DDS (2.14, the rig Pis' ROS 2 jazzy install; port of the PC's 3.x node, same logic) node of the T1 framework comparison (roles and semantics: t1fw_common.h). Transport and interface are
// set by the XML profile in FASTDDS_DEFAULT_PROFILES_FILE (the harness writes it); the QoS below is the comparison's.
#include <atomic>
#include <csignal>
#include <mutex>
#include <vector>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "Blob.h"
#include "BlobPubSubTypes.h"

extern "C" {
#include "t1fw_common.h"
}

using namespace eprosima::fastdds::dds;

static struct t1_opts opt;
static struct t1_score sc;
static std::mutex score_mutex;
static DataWriter* wr = nullptr;
static std::atomic<int> w_matched{0};
static std::atomic<int> r_matched{0};
static std::atomic<uint32_t> pongs_failed{0};
static std::vector<uint8_t> txbuf;
static std::mutex tx_mutex;

static int send_sample(uint32_t len, uint64_t send_ns, uint32_t seq) {
    std::lock_guard<std::mutex> lock(tx_mutex);
    t1_hdr_put(txbuf.data(), send_ns, seq);
    Blob msg;
    msg.data().assign(txbuf.begin(), txbuf.begin() + len);
    return wr->write(&msg) ? 0 : -1; // 2.14: write(void*) returns bool (3.x: ReturnCode_t)
}

class WListener : public DataWriterListener {
public:
    void on_publication_matched(DataWriter*, const PublicationMatchedStatus& st) override { w_matched = st.current_count; }
};

class RListener : public DataReaderListener {
public:
    bool answer = false;
    void on_subscription_matched(DataReader*, const SubscriptionMatchedStatus& st) override { r_matched = st.current_count; }
    void on_data_available(DataReader* reader) override {
        Blob msg;
        SampleInfo info;
        while (reader->take_next_sample(&msg, &info) == ReturnCode_t::RETCODE_OK) {
            if (info.instance_state != ALIVE_INSTANCE_STATE || !info.valid_data || msg.data().size() < T1_HDR) {
                continue;
            }
            uint64_t send_ns;
            uint32_t seq;
            t1_hdr_get(msg.data().data(), &send_ns, &seq);
            uint64_t now = t1_now_ns();
            int first;
            {
                std::lock_guard<std::mutex> lock(score_mutex);
                first = t1_record(&sc, seq, send_ns, now);
            }
            if (first && answer && send_sample(T1_HDR, send_ns, seq) != 0) {
                pongs_failed++;
            }
        }
    }
};

static bool wait_match(bool need_w, bool need_r) {
    uint64_t deadline = t1_now_ns() + (uint64_t)(T1_MATCH_WAIT_S * 1e9);
    auto ok = [&] { return (!need_w || w_matched > 0) && (!need_r || r_matched > 0); };
    while (!ok() && t1_now_ns() < deadline) {
        t1_sleep_until(t1_now_ns() + 10000000ULL);
    }
    return ok();
}

int main(int argc, char** argv) {
    if (t1_parse(argc, argv, &opt) != 0) {
        fprintf(stderr, "usage: fastdds_node ping|echo|pub|sub [-d s] [-m be|rel] [-s B] [-r Hz] [-K depth] [-Q s] [-B s] [-q stop] [-l file]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    uint64_t start = t1_now_ns();
    txbuf.assign(opt.size > T1_HDR ? opt.size : T1_HDR, 0);

    DomainParticipant* part = DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (part == nullptr) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }
    TypeSupport type(new BlobPubSubType());
    type.register_type(part);
    Publisher* publisher = part->create_publisher(PUBLISHER_QOS_DEFAULT);
    Subscriber* subscriber = part->create_subscriber(SUBSCRIBER_QOS_DEFAULT);

    const char* wname = nullptr;
    const char* rname = nullptr;
    switch (opt.role) {
    case T1_PING: wname = "wifi_ping"; rname = "wifi_pong"; break;
    case T1_ECHO: wname = "wifi_pong"; rname = "wifi_ping"; break;
    case T1_PUB: wname = "wifi_ping"; break;
    case T1_SUB: rname = "wifi_ping"; break;
    }
    WListener wl;
    RListener rl;
    rl.answer = opt.role == T1_ECHO;
    eprosima::fastrtps::Duration_t block(static_cast<int32_t>(opt.block_s), 0);
    DataReader* rd = nullptr;
    if (rname != nullptr) {
        Topic* topic = part->create_topic(rname, type.get_type_name(), TOPIC_QOS_DEFAULT);
        DataReaderQos qos = DATAREADER_QOS_DEFAULT;
        qos.reliability().kind = opt.reliable ? RELIABLE_RELIABILITY_QOS : BEST_EFFORT_RELIABILITY_QOS;
        qos.history().kind = KEEP_LAST_HISTORY_QOS;
        qos.history().depth = static_cast<int32_t>(opt.depth);
        qos.durability().kind = VOLATILE_DURABILITY_QOS;
        rd = subscriber->create_datareader(topic, qos, &rl);
    }
    if (wname != nullptr) {
        Topic* topic = part->create_topic(wname, type.get_type_name(), TOPIC_QOS_DEFAULT);
        DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
        qos.reliability().kind = opt.reliable ? RELIABLE_RELIABILITY_QOS : BEST_EFFORT_RELIABILITY_QOS;
        qos.reliability().max_blocking_time = block;
        qos.history().kind = KEEP_LAST_HISTORY_QOS;
        qos.history().depth = static_cast<int32_t>(opt.depth);
        qos.durability().kind = VOLATILE_DURABILITY_QOS;
        wr = publisher->create_datawriter(topic, qos, &wl);
    }
    if ((wname != nullptr && wr == nullptr) || (rname != nullptr && rd == nullptr)) {
        fprintf(stderr, "endpoint creation failed\n");
        return 1;
    }

    uint32_t published = 0;
    uint32_t failed = 0;
    bool is_matched = true;
    if (opt.role == T1_PING) {
        sc.warmup = (uint32_t)opt.rate_hz;
        is_matched = wait_match(true, true);
        if (is_matched) {
            uint64_t period = (uint64_t)(1e9 / opt.rate_hz);
            uint64_t end = t1_now_ns() + (uint64_t)(opt.seconds * 1e9);
            for (uint64_t next = t1_now_ns(); next < end && published < T1_MAX_SEQ; next += period) {
                t1_sleep_until(next);
                if (send_sample(opt.size, t1_now_ns(), published) == 0) {
                    published++;
                } else {
                    failed++;
                }
            }
            t1_sleep_until(t1_now_ns() + (uint64_t)(T1_DRAIN_S * 1e9));
        }
    } else if (opt.role == T1_ECHO || opt.role == T1_SUB) {
        sc.warmup = opt.role == T1_ECHO ? (uint32_t)opt.rate_hz : 0;
        uint64_t end = start + (uint64_t)(opt.seconds * 1e9);
        while (t1_now_ns() < end && !t1_stop_asked(&opt)) {
            t1_sleep_until(t1_now_ns() + 10000000ULL);
        }
        failed = pongs_failed;
    } else {
        is_matched = wait_match(true, false);
        if (is_matched) {
            uint32_t n = (uint32_t)(opt.seconds * opt.rate_hz);
            uint64_t period = (uint64_t)(1e9 / opt.rate_hz);
            uint64_t next = t1_now_ns();
            uint64_t pub_t0 = next;
            for (uint32_t i = 0; i < n; i++, next += period) {
                t1_sleep_until(next);
                if (send_sample(opt.size, t1_now_ns(), i) == 0) {
                    published++;
                } else {
                    failed++;
                }
            }
            t1_sleep_until(pub_t0 + (uint64_t)((opt.seconds + opt.drain_s) * 1e9));
        }
    }
    {
        std::lock_guard<std::mutex> lock(score_mutex);
        t1_result("fastdds", &opt, &sc, is_matched, published, failed, start, t1_now_ns(), nullptr);
    }
    _exit(0); // no participant teardown: the namespace is deleted next, and a hung teardown must not hide the RESULT
}
