/*
 * HIL 3-way QoS-matrix comparison, scenario "durability_late_join" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) subscriber role, deliberately started *after* the publisher has already
 * sent its whole backlog (run_scenario.sh's own orchestration sleeps before starting this side) -
 * the actual thing under test: does TRANSIENT_LOCAL (-D) deliver that backlog anyway, while
 * VOLATILE (default) delivers none of it. See server.cpp's own doc comment for why this side also
 * has a writer (the ack), not just a reader.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
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

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    bool durable = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-D") == 0) {
            durable = true;
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

    Topic* data_topic = participant->create_topic("ping", "Bench", TOPIC_QOS_DEFAULT);
    Topic* ack_topic = participant->create_topic("pong", "Bench", TOPIC_QOS_DEFAULT);

    // Matches server.cpp's own backlog_count (20) - see its own doc comment for the real bug this
    // avoids.
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = 20;
    if (durable) {
        rqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    }

    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* reader = subscriber->create_datareader(data_topic, rqos);
    if (!reader) {
        fprintf(stderr, "create_datareader failed\n");
        return 1;
    }

    DataWriterQos ack_qos = DATAWRITER_QOS_DEFAULT;
    ack_qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* ack_writer = publisher->create_datawriter(ack_topic, ack_qos);
    if (!ack_writer) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    // Actively wait for a real match on both, matching the CycloneDDS twin's own approach - the
    // real, confirmed reason a plain sleep isn't good enough is documented on the CycloneDDS side's
    // own common.h/client.c doc comments.
    {
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        bool reader_matched = false, writer_matched = false;
        for (;;) {
            eprosima::fastdds::dds::SubscriptionMatchedStatus sub_status;
            reader->get_subscription_matched_status(sub_status);
            reader_matched = sub_status.current_count > 0;
            eprosima::fastdds::dds::PublicationMatchedStatus pub_status;
            ack_writer->get_publication_matched_status(pub_status);
            writer_matched = pub_status.current_count > 0;
            if (reader_matched && writer_matched) {
                break;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
            if (elapsed >= 15.0) {
                break;
            }
            struct timespec poll_interval = {0, 50 * 1000 * 1000};
            nanosleep(&poll_interval, nullptr);
        }
        if (!reader_matched || !writer_matched) {
            fprintf(stderr, "timed out waiting for a match\n");
            return 1;
        }
    }

    uint64_t start = now_ns();
    // 15s, not a short window - see the CycloneDDS twin's own doc comment: RELIABLE redelivery of a
    // durability backlog goes through real ACKNACK round-trips per sample, not an instant replay.
    uint64_t deadline = start + 15ULL * 1000000000ULL;
    uint32_t received = 0;
    while (!g_interrupted && now_ns() < deadline) {
        eprosima::fastrtps::Duration_t timeout {1, 0};
        if (!reader->wait_for_unread_message(timeout)) {
            continue;
        }
        Bench sample;
        SampleInfo info;
        while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
            if (info.valid_data) {
                received++;
            }
        }
    }
    double backlog_delivery_ms = received > 0 ? (double)(now_ns() - start) / 1e6 : -1.0;

    Bench ack;
    ack.seq(1);
    ack.send_ns(now_ns());
    ack_writer->write(&ack);

    printf("RESULT: framework=fastdds scenario=durability_late_join role=client durable=%d "
           "received=%u backlog_delivery_ms=%.3f\n",
           durable, received, backlog_delivery_ms);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
