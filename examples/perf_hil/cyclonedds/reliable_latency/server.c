/*
 * HIL 3-way QoS-matrix comparison, scenario "reliable_latency" (rmw_tickle/COMPARISON.MD) -
 * CycloneDDS native (no rmw) server/pong role. Subscribes on "ping", republishes the exact same
 * sample (same seq/send_ns) on "pong" immediately - the client's own RTT measurement is entirely
 * client-side, this side never logs per-request (matches TickLE's own examples/linux/ping_pong/
 * pong.c precedent).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <dds/dds.h>

#include "Bench.h"

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) {
        fprintf(stderr, "dds_create_participant: %s\n", dds_strretcode(-participant));
        return 1;
    }

    dds_entity_t ping_topic = dds_create_topic(participant, &Bench_desc, "ping", NULL, NULL);
    dds_entity_t pong_topic = dds_create_topic(participant, &Bench_desc, "pong", NULL, NULL);
    if (ping_topic < 0 || pong_topic < 0) {
        fprintf(stderr, "dds_create_topic failed\n");
        return 1;
    }

    // scenario "reliable_latency" - RELIABLE on both sides (COMPARISON.MD's design principle
    // 3: same QoS value across frameworks) + HISTORY depth=8 (COMPARISON.MD's own QoS value
    // matrix), the standing baseline depth used everywhere RELIABLE is on in this exercise.
    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);

    dds_entity_t reader = dds_create_reader(participant, ping_topic, qos, NULL);
    dds_entity_t writer = dds_create_writer(participant, pong_topic, qos, NULL);
    dds_delete_qos(qos);
    if (reader < 0 || writer < 0) {
        fprintf(stderr, "dds_create_reader/writer failed\n");
        return 1;
    }

    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, reader, 0);

    printf("CycloneDDS reliable_latency server: reader on 'ping', writer on 'pong'\n");

    struct Bench sample;
    void* samples[1] = {&sample};
    dds_sample_info_t infos[1];

    while (!g_interrupted) {
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_SECS(1));
        if (rc < 0) {
            break;
        }
        if (rc == 0) {
            continue; // timed out, just re-check g_interrupted
        }
        dds_return_t n = dds_take(reader, samples, infos, 1, 1);
        if (n > 0 && infos[0].valid_data) {
            dds_write(writer, &sample);
        }
    }

    dds_delete(participant);
    return 0;
}
