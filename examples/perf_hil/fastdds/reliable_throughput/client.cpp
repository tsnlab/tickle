/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_throughput" (rmw_tickle/COMPARISON.MD) -
 * FastDDS native (no rmw) client/sender role. Mirrors the CycloneDDS scenario pair exactly - a
 * one-way stream (no pong), the receiver (server.cpp) is the authoritative side for loss/
 * throughput since only it can see what actually arrived.
 *
 * Brought up to the repository's C++ clang-tidy checks on 2026-09-25 (see harness_common.hpp); the
 * send loop, its pacing, its QoS, the teardown drain and the RESULT line are unchanged.
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
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>
#include <fastdds/rtps/common/Time_t.h>
#include <fastrtps/types/TypesBase.h>

#include "../../tickle/common/BenchStats.h" // shared instrumentation - see its own header
#include "../../tickle/common/CpuPlace.h"   // shared with the TickLE harness - see its own header
#include "../harness_common.hpp"
#include "Bench.h"
#include "BenchPubSubTypes.h"

using namespace eprosima::fastdds::dds;

namespace {

    constexpr double default_duration_s = 10.0;
    constexpr double default_drain_s = 3.0;           // cap on the teardown wait-for-acknowledgements below
    constexpr int32_t default_keepall_samples = 4000; // resource_limits - see the QoS comment in main(), and -N
    constexpr time_t discovery_wait_s = 2;            // see the latency scenarios' own identical comment
    constexpr double ms_per_s = 1000.0;
    constexpr uint64_t cpu_place_period_ns = 100ULL * 1000ULL * 1000ULL; // 100ms

    struct client_options {
        double duration_s = default_duration_s;
        double interval_s = 0.0;       // 0 = as fast as possible
        double max_blocking_ms = -1.0; // -B: RELIABILITY max_blocking_time; <0 keeps FastDDS's default (100ms)
        // -K <depth>: KEEP_LAST at that depth instead of the KEEP_ALL default (2026-09-25). The
        // campaign's Q0 baseline uses KEEP_ALL for all three because that is the only configuration
        // where all three make the same promise, but KEEP_LAST is what rclcpp and TickLE actually
        // default to, so without this there is no cross-vendor cell for the configuration users get.
        // Depth 64, not 8: this file's own notes record KEEP_LAST(8) as the bisected cause of a real
        // 53% loss, so a shallow depth would re-measure that finding rather than the default.
        int keep_last_depth = 0; // 0 = KEEP_ALL, unchanged default
        // -C <seconds>: the teardown drain cap. Diagnostic only (2026-09-26): with a longer cap, a writer
        // that drained=timeout at 3 s shows whether it was slow or would never finish. Scored runs keep
        // the common 3 s, and every RESULT line prints drain_cap_s= so the two cannot be mixed up.
        double drain_s = default_drain_s;
        // -N <samples>: KEEP_ALL's history bound, RESOURCE_LIMITS max_samples and max_samples_per_instance,
        // 4000 by default (2026-09-26, the fairness audit, COMPARISON.MD 4.4). The bound differed between
        // the three frameworks by default; the campaign now passes all three the same number of samples,
        // and every RESULT line's keepall_samples= says which it was. The same letter in all three.
        int32_t keepall_samples = default_keepall_samples;
    };

    auto parse_options(int argc, char** argv) -> client_options {
        client_options opts;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-K") == 0 && i + 1 < argc) {
                opts.keep_last_depth = atoi(argv[++i]);
            } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                opts.duration_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
                opts.interval_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-B") == 0 && i + 1 < argc) {
                opts.max_blocking_ms = atof(argv[++i]);
            } else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
                opts.drain_s = atof(argv[++i]);
            } else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc) {
                opts.keepall_samples = static_cast<int32_t>(atoi(argv[++i]));
            }
        }
        return opts;
    }

    // scenario "reliable_throughput" - RELIABLE, matched exactly across frameworks
    // (COMPARISON.MD's design principle 3). KEEP_ALL + generous resource_limits (2026-09-21, real
    // bug found the hard way - see the CycloneDDS twin's own identical fix, which this file never
    // got the matching update for): the shallow KEEP_LAST(8) this file used before real-CI-caught
    // reliable_throughput's own RELIABLE never recovering any real tc/netem-injected loss at all
    // (COMPARISON.MD §3/§6 - CycloneDDS's own KEEP_ALL+resource_limits(4000) fully recovers 1%/5%
    // injected loss every time, this file's own shallow depth=8 recovered none of it) - a shallow
    // writer history queue evicts a lost sample long before a NACK-driven retry can land, the exact
    // same root cause CycloneDDS's own client.c doc comment already names.
    auto writer_qos(const client_options& opts) -> DataWriterQos {
        DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
        wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        if (opts.keep_last_depth > 0) {
            wqos.history().kind = KEEP_LAST_HISTORY_QOS;
            wqos.history().depth = opts.keep_last_depth;
        } else {
            wqos.history().kind = KEEP_ALL_HISTORY_QOS;
        }
        wqos.resource_limits().max_samples = opts.keepall_samples;
        wqos.resource_limits().max_samples_per_instance = opts.keepall_samples;
        if (opts.max_blocking_ms >= 0.0) {
            // Field-wise, not a Duration_t constructor: the type lives in eprosima::fastrtps on FastDDS
            // 2.x (the rig's jazzy) but in eprosima::fastdds on 3.x, while seconds/nanosec exist in both.
            const double blocking_s = opts.max_blocking_ms / ms_per_s;
            wqos.reliability().max_blocking_time.seconds = static_cast<int32_t>(blocking_s);
            wqos.reliability().max_blocking_time.nanosec = static_cast<uint32_t>(
                (blocking_s - static_cast<double>(static_cast<int32_t>(blocking_s))) * harness::ns_per_s_real);
        }
        return wqos;
    }

    struct send_result {
        uint64_t sent = 0;
        // Writes the DataWriter refused (e.g. a timeout once KEEP_ALL's resource_limits stay full past
        // max_blocking_time). seq is still consumed, so the server counts each one as lost too;
        // write_fail lets the two be told apart (rmw_tickle/PLAN.md Phase 3, item 5).
        uint64_t write_fail = 0;
    };

    auto send_until(DataWriter* writer, uint64_t deadline, double interval_s, BenchCpuPlace& cpu_place) -> send_result {
        send_result result;
        uint32_t seq = 0;
        while (!harness::interrupted() && harness::now_ns() < deadline) {
            Bench msg;
            msg.seq(++seq);
            msg.send_ns(harness::now_ns());
            BenchCpuPlace_sample(&cpu_place, harness::now_ns(), cpu_place_period_ns);
            if (writer->write(&msg)) {
                result.sent++;
            } else {
                result.write_fail++;
            }
            if (interval_s > 0.0) {
                harness::sleep_seconds(interval_s);
            }
        }
        return result;
    }

    // Teardown drain, matching the TickLE and CycloneDDS harnesses so all three are measured the
    // same way at the end of a run - see the CycloneDDS twin's own comment for why a tail sample
    // is otherwise invisible. wait_for_acknowledgments() is FastDDS's own equivalent.
    auto drain(DataWriter* writer, double drain_s) -> const char* {
        // Field-wise, like max_blocking_time in writer_qos(): Duration_t's namespace differs between 2.x
        // and 3.x, while seconds/nanosec exist in both.
        eprosima::fastrtps::Duration_t drain_wait;
        drain_wait.seconds = static_cast<int32_t>(drain_s);
        drain_wait.nanosec =
            static_cast<uint32_t>((drain_s - static_cast<double>(drain_wait.seconds)) * harness::ns_per_s_real);
        // == RETCODE_OK, not a bool test: on FastDDS 2.x (the rig's jazzy) this returns
        // fastrtps::types::ReturnCode_t, whose operator bool() is deleted; 3.x returns the
        // fastdds::dds enum. Comparing against the 2.x constant is what compiles where we measure.
        return writer->wait_for_acknowledgments(drain_wait) == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK
                   ? "acked"
                   : "timeout";
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

    Publisher* const publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    DataWriter* const writer = publisher->create_datawriter(topic, writer_qos(opts));
    if (writer == nullptr) {
        fprintf(stderr, "create_datawriter failed\n");
        return 1;
    }

    const struct timespec discovery_wait = {discovery_wait_s, 0};
    nanosleep(&discovery_wait, nullptr);

    // Same CPU-placement sampling as the CycloneDDS and TickLE harnesses - see the CycloneDDS
    // twin's comment for why the DDS columns need this measured rather than assumed.
    struct BenchCpuPlace cpu_place;
    BenchCpuPlace_init(&cpu_place);
    const uint64_t start = harness::now_ns();
    const send_result result =
        send_until(writer, start + harness::seconds_to_ns(opts.duration_s), opts.interval_s, cpu_place);

    const char* const drained = drain(writer, opts.drain_s);

    const double elapsed_s = static_cast<double>(harness::now_ns() - start) / harness::ns_per_s_real;
    const double mbps = harness::mbps(result.sent, sizeof(Bench), elapsed_s);
    bench_stats_end(&harness::g_bench_stats);
    printf("RESULT: framework=fastdds scenario=reliable_throughput role=client sent=%lu write_fail=%lu "
           "elapsed_s=%.3f send_mbps=%.3f max_blocking_ms=%.3f drained=%s drain_cap_s=%.1f cpu_main=%d "
           "cpu_main_share=%.2f cpu_migrations=%u keep_all=%d keep_last_depth=%d keepall_samples=%d "
           "transport_profile=%s %s\n",
           static_cast<unsigned long>(result.sent), static_cast<unsigned long>(result.write_fail), elapsed_s, mbps,
           opts.max_blocking_ms, drained, opts.drain_s, BenchCpuPlace_main_cpu(&cpu_place),
           BenchCpuPlace_main_share(&cpu_place), cpu_place.migrations, opts.keep_last_depth > 0 ? 0 : 1,
           opts.keep_last_depth, static_cast<int>(opts.keepall_samples), harness::transport_profile(),
           harness::bench_fields(BENCH_ROLE_SENDER, result.sent));

    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
