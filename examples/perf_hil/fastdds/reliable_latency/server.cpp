/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_latency" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) server/pong role. Subscribes on "ping", republishes the exact same
 * sample (same seq/send_ns) on "pong" immediately - mirrors the CycloneDDS scenario pair exactly
 * (COMPARISON.MD's design principle 1: identical data/behavior, only the framework differs).
 */
#include <csignal>
#include <cstdio>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
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

int main() {
    std::signal(SIGINT, handle_sigint);

    DomainParticipant* participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (!participant) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }

    TypeSupport type(new BenchPubSubType());
    type.register_type(participant);

    Topic* ping_topic = participant->create_topic("ping", "Bench", TOPIC_QOS_DEFAULT);
    Topic* pong_topic = participant->create_topic("pong", "Bench", TOPIC_QOS_DEFAULT);

    // scenario "reliable_latency" - BEST_EFFORT on both sides, matching CycloneDDS's own QoS
    // exactly (COMPARISON.MD's design principle 3).
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = 8;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = 8;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;

    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataReader* reader = subscriber->create_datareader(ping_topic, rqos);
    DataWriter* writer = publisher->create_datawriter(pong_topic, wqos);
    if (!reader || !writer) {
        fprintf(stderr, "create_datareader/datawriter failed\n");
        return 1;
    }

    printf("FastDDS reliable_latency server: reader on 'ping', writer on 'pong'\n");

    Bench sample;
    SampleInfo info;
    while (!g_interrupted) {
        eprosima::fastrtps::Duration_t timeout {1, 0};
        if (reader->wait_for_unread_message(timeout)) {
            if (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK && info.valid_data) {
                writer->write(&sample);
            }
        }
    }

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
