/*
 * HIL 3-way QoS-matrix comparison, scenario "liveliness_loss_detection" (rmw_tickle/comparison.md) -
 * CycloneDDS native (no rmw) publisher role. Publishes at a steady cadence (`-i`, default 100ms)
 * with LIVELINESS AUTOMATIC and a matched lease duration (`-L`, default 2s, matched exactly against
 * server.c's own reader QoS - the actual variable under test) until it either runs out its own
 * generous safety cap (`-d`) or - the real point of this scenario - gets killed out from under
 * itself (`kill -9`, a genuine crash simulation, not this process's own SIGINT handler) by the
 * orchestrating test script partway through. There is deliberately no code here reacting to being
 * killed - server.c's own doc comment has the actual detection side of this scenario.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dds/dds.h>

#include "../common.h"
#include "Bench.h"

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    double interval_s = 0.1;
    double lease_s = 2.0;
    double duration_s = 60.0; // generous safety cap - this run is normally ended by kill -9, not this
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            lease_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    dds_qset_liveliness(qos, DDS_LIVELINESS_AUTOMATIC, (dds_duration_t)(lease_s * 1e9));
    dds_entity_t writer = dds_create_writer(participant, topic, qos, NULL);
    dds_delete_qos(qos);
    if (writer < 0) {
        fprintf(stderr, "dds_create_writer failed\n");
        return 1;
    }

    if (!wait_for_writer_match(participant, writer, 10.0)) {
        fprintf(stderr, "timed out waiting for a matched reader\n");
        return 1;
    }

    printf("Publisher: matched (pid=%d), publishing every %.3fs until killed or %.1fs safety cap\n", getpid(),
           interval_s, duration_s);

    uint32_t seq = 0;
    uint64_t interval_ns = (uint64_t)(interval_s * 1e9);
    uint64_t start = now_ns();
    uint64_t deadline = start + (uint64_t)(duration_s * 1e9);

    while (!g_interrupted && now_ns() < deadline) {
        struct Bench msg = {.seq = ++seq, .send_ns = now_ns()};
        dds_write(writer, &msg);
        struct timespec pace = {.tv_sec = (time_t)(interval_ns / 1000000000ULL),
                                .tv_nsec = (long)(interval_ns % 1000000000ULL)};
        nanosleep(&pace, NULL);
    }

    printf("RESULT: framework=cyclonedds scenario=liveliness_loss_detection role=client sent=%u "
           "(ran to completion, not killed)\n",
           seq);

    dds_delete(participant);
    return 0;
}
