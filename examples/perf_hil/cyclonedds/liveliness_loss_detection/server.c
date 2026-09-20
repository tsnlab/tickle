/*
 * HIL 3-way QoS-matrix comparison, scenario "liveliness_loss_detection" (rmw_tickle/comparison.md) -
 * CycloneDDS native (no rmw) subscriber role. Mirrors client.c's own LIVELINESS AUTOMATIC + lease
 * duration exactly and monitors this side's own LIVELINESS_CHANGED_STATUS via a listener - the
 * real thing under test is the *latency* between the writer actually going silent (this side's own
 * last received sample) and this side genuinely detecting the loss (`not_alive_count` going from 0
 * to 1) - both timestamps taken on this same host's own clock, deliberately, to avoid the exact
 * cross-host clock-sync pitfall this whole exercise's own earlier cross-host RTT work (comparison.md's
 * own "Item 5" section) got burned by: comparing this side's own detection time against the writer's
 * own kill time (a different host, a different clock) would need real clock sync this scenario has
 * no way to guarantee; comparing it against this side's own last-received timestamp needs none.
 */
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static volatile uint64_t g_last_recv_ns = 0;
static volatile uint64_t g_loss_detect_ns = 0;
static volatile bool g_loss_detected = false;

static void on_liveliness_changed(dds_entity_t reader, const dds_liveliness_changed_status_t status, void* arg) {
    (void)reader;
    (void)arg;
    if (status.not_alive_count > 0 && !g_loss_detected) {
        g_loss_detected = true;
        g_loss_detect_ns = now_ns();
    }
}

int main(int argc, char** argv) {
    double lease_s = 2.0;
    double safety_cap_s = 30.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            lease_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &Bench_desc, "stream", NULL, NULL);

    dds_listener_t* listener = dds_create_listener(NULL);
    dds_lset_liveliness_changed(listener, on_liveliness_changed);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    dds_qset_liveliness(qos, DDS_LIVELINESS_AUTOMATIC, (dds_duration_t)(lease_s * 1e9));
    dds_entity_t reader = dds_create_reader(participant, topic, qos, listener);
    dds_delete_qos(qos);
    dds_delete_listener(listener);
    if (reader < 0) {
        fprintf(stderr, "dds_create_reader failed\n");
        return 1;
    }

    if (!wait_for_reader_match(participant, reader, 10.0)) {
        fprintf(stderr, "timed out waiting for a matched writer\n");
        return 1;
    }

    dds_entity_t waitset = dds_create_waitset(participant);
    dds_set_status_mask(reader, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(waitset, reader, 0);

    uint64_t received = 0;
    struct Bench sample;
    void* samples[1] = {&sample};
    dds_sample_info_t infos[1];

    uint64_t deadline = now_ns() + (uint64_t)(safety_cap_s * 1e9);
    while (!g_interrupted && now_ns() < deadline && !g_loss_detected) {
        dds_return_t rc = dds_waitset_wait(waitset, NULL, 0, DDS_MSECS(200));
        if (rc <= 0) {
            continue;
        }
        dds_return_t n;
        while ((n = dds_take(reader, samples, infos, 1, 1)) > 0) {
            if (infos[0].valid_data) {
                received++;
                g_last_recv_ns = now_ns();
            }
        }
    }

    // A brief real settle window - the loss listener may fire a few ms after the last data-
    // available wake that broke the loop above.
    struct timespec settle = {0, 300 * 1000 * 1000};
    nanosleep(&settle, NULL);

    double detect_latency_ms =
        (g_loss_detected && g_last_recv_ns > 0) ? (double)(g_loss_detect_ns - g_last_recv_ns) / 1e6 : -1.0;

    printf("RESULT: framework=cyclonedds scenario=liveliness_loss_detection role=server recv=%lu "
           "loss_detected=%d detect_latency_ms=%.3f lease_s=%.3f\n",
           (unsigned long)received, g_loss_detected, detect_latency_ms, lease_s);

    dds_delete(participant);
    return 0;
}
