/*
 * HIL 3-way QoS-matrix comparison, scenario "durability_late_join" (rmw_tickle/comparison.md) -
 * FastDDS native (no rmw) publisher role. Mirrors the now-fixed CycloneDDS scenario pair exactly
 * (examples/perf_hil/cyclonedds/durability_late_join/server.c's own doc comment has the full real
 * bug history this design already bakes in): publishes the full backlog on "ping" *before* any
 * subscriber exists, then waits for the late subscriber's own ack on "pong" - writer+reader on
 * both ends (a reader-only/writer-only participant pair was a real, separate discovery-asymmetry
 * bug found earlier this exercise; every later scenario sidesteps it this way).
 *
 * -D: durable (TRANSIENT_LOCAL) instead of the default volatile - the actual variable under test.
 *
 * KEEP_LAST(backlog_count) on regular history, not a shallower depth (2026-09-20, baked in from
 * the start here - see the CycloneDDS twin's own doc comment for the real, bisected bug this
 * avoids): TRANSIENT_LOCAL durability replays a late joiner directly from the writer's own regular
 * history cache when no separate persistence service is running, independent of whatever
 * durability_service's own depth claims - a shallower regular depth silently caps what's ever
 * replayable.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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
    uint32_t backlog_count = 20;
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

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.reliability().max_blocking_time = eprosima::fastrtps::Duration_t(1, 0);
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = (int32_t)backlog_count;
    if (durable) {
        wqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
        wqos.durability_service().history_kind = KEEP_LAST_HISTORY_QOS;
        wqos.durability_service().history_depth = (int32_t)backlog_count;
    }

    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* writer = publisher->create_datawriter(data_topic, wqos);
    if (!writer) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    DataReaderQos ack_qos = DATAREADER_QOS_DEFAULT;
    ack_qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* ack_reader = subscriber->create_datareader(ack_topic, ack_qos);
    if (!ack_reader) {
        fprintf(stderr, "create_datareader failed\n");
        return 1;
    }

    printf("Publisher: durable=%d, publishing %u backlog samples before any subscriber exists\n", durable,
           backlog_count);
    for (uint32_t i = 1; i <= backlog_count; i++) {
        Bench msg;
        msg.seq(i);
        msg.send_ns(now_ns());
        if (!writer->write(&msg)) {
            fprintf(stderr, "write(seq=%u) failed\n", i);
        }
    }

    printf("Waiting up to 40s for the late subscriber's own ack...\n");
    uint64_t deadline = now_ns() + 40ULL * 1000000000ULL;
    bool acked = false;
    while (!g_interrupted && now_ns() < deadline && !acked) {
        eprosima::fastrtps::Duration_t timeout{1, 0};
        if (!ack_reader->wait_for_unread_message(timeout)) {
            continue;
        }
        Bench ack;
        SampleInfo info;
        if (ack_reader->take_next_sample(&ack, &info) == ReturnCode_t::RETCODE_OK && info.valid_data) {
            acked = true;
        }
    }

    printf("RESULT: framework=fastdds scenario=durability_late_join role=server durable=%d "
           "backlog_sent=%u acked=%d\n",
           durable, backlog_count, acked);

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
