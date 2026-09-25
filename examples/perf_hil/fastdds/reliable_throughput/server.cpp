/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) server/receiver role. The authoritative side for loss/throughput - only
 * it can see what actually arrived (see client.cpp's own doc comment).
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "../../tickle/common/BenchStats.h" // shared instrumentation - see its own header
#include "Bench.h"

static struct BenchStats g_bench_stats;
static char g_bench_fields[BENCH_STATS_FIELDS_MAX];
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
    // Armed at the very top, before any middleware setup, so the counters cover discovery
    // too - identically for all three frameworks, which is what makes them comparable.
    bench_stats_begin(&g_bench_stats);
    double safety_cap_s = 40.0;
    // -K <depth>: KEEP_LAST at that depth instead of the KEEP_ALL default (2026-09-25). The
    // campaign's Q0 baseline uses KEEP_ALL for all three because that is the only configuration
    // where all three make the same promise, but KEEP_LAST is what rclcpp and TickLE actually
    // default to, so without this there is no cross-vendor cell for the configuration users get.
    // Depth 64, not 8: this file's own notes record KEEP_LAST(8) as the bisected cause of a real
    // 53% loss, so a shallow depth would re-measure that finding rather than the default.
    int keep_last_depth = 0; // 0 = KEEP_ALL, unchanged default
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-K") == 0 && i + 1 < argc) {
            keep_last_depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }
    // +15s buffer - see best_effort_throughput/server.cpp's own doc comment (this same directory)
    // for the real bug this avoids.
    safety_cap_s += 15.0;

    std::signal(SIGINT, handle_sigint);

    DomainParticipant* participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (!participant) {
        fprintf(stderr, "create_participant failed\n");
        return 1;
    }

    TypeSupport type(new BenchPubSubType());
    type.register_type(participant);

    Topic* topic = participant->create_topic("stream", "Bench", TOPIC_QOS_DEFAULT);

    // KEEP_ALL + generous resource_limits - matches client.cpp's own identical fix, see its doc
    // comment for the real bug this avoids.
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    if (keep_last_depth > 0) {
        rqos.history().kind = KEEP_LAST_HISTORY_QOS;
        rqos.history().depth = keep_last_depth;
    } else {
        rqos.history().kind = KEEP_ALL_HISTORY_QOS;
    }
    rqos.resource_limits().max_samples = 4000;

    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    DataReader* reader = subscriber->create_datareader(topic, rqos);
    if (!reader) {
        fprintf(stderr, "create_datareader failed\n");
        return 1;
    }

    uint64_t received = 0, lost = 0;
    uint32_t last_seq = 0;
    bool first = true;
    uint64_t first_recv_ns = 0, last_recv_ns = 0;

    // Lifetime (2026-09-25): an absolute cap only until the first sample arrives, then an idle
    // cap. It was absolute throughout - -d + 15 s from start - and that truncated the one cell that
    // needed longer: P4 under 5% loss needs at least 16.8 s of wire time, so the server exited while
    // the client was still retransmitting, leaving 200 samples undelivered and peer_acks_end=0 in
    // what read as a protocol result. Any fixed figure only moves that cliff to a worse condition.
    // "Don't hang forever" means "stop when nothing is arriving", so that is what it now checks.
    // run_scenario.sh still ends the normal case with SIGINT as soon as the client finishes; these
    // are only the backstops. Identical in all three frameworks' servers - a lifetime rule that
    // differed would hand whichever lived longest the samples the other was cut off from.
    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(safety_cap_s * 1e9);
    const uint64_t idle_cap_ns = (uint64_t)(15.0 * 1e9);
    while (!g_interrupted) {
        uint64_t now = now_ns();
        if (first ? now >= deadline : now - last_recv_ns >= idle_cap_ns) {
            break;
        }
        eprosima::fastrtps::Duration_t timeout {1, 0};
        if (!reader->wait_for_unread_message(timeout)) {
            continue;
        }
        Bench sample;
        SampleInfo info;
        while (reader->take_next_sample(&sample, &info) == ReturnCode_t::RETCODE_OK) {
            if (!info.valid_data) {
                continue;
            }
            if (first) {
                first = false;
                first_recv_ns = now_ns();
                last_seq = sample.seq();
            } else if (sample.seq() > last_seq + 1) {
                lost += (sample.seq() - last_seq - 1);
                last_seq = sample.seq();
            } else {
                last_seq = sample.seq();
            }
            last_recv_ns = now_ns();
            received++;
        }
    }

    double elapsed_s = received > 0 ? (double)(last_recv_ns - first_recv_ns) / 1e9 : 0.0;
    uint64_t total = received + lost;
    double loss_pct = total > 0 ? (100.0 * (double)lost / (double)total) : 0.0;
    double mbps = elapsed_s > 0.0 ? ((double)received * sizeof(Bench) * 8.0) / 1e6 / elapsed_s : 0.0;

    bench_stats_end(&g_bench_stats);

    printf("RESULT: framework=fastdds scenario=reliable_throughput role=server recv=%lu lost=%lu "
           "loss_pct=%.1f elapsed_s=%.3f recv_mbps=%.3f keep_all=%d keep_last_depth=%d %s\n",
           (unsigned long)received, (unsigned long)lost, loss_pct, elapsed_s, mbps, keep_last_depth > 0 ? 0 : 1,
           keep_last_depth,
           bench_stats_fields(&g_bench_stats, BENCH_ROLE_RECEIVER, received, BENCH_SAMPLE_BYTES, g_bench_fields,
                              sizeof g_bench_fields));

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
