// One idle TickLE node with E publishers, for measuring what discovery alone costs on the wire
// (2026-09-26, rmw_tickle/DISCOVERY_PLAN.md M1). It publishes nothing, so every byte its interface
// carries is discovery (plus one-time ARP). discovery_scaling.sh runs N of these in private netns on a
// bridge and reads the interfaces' byte counters.
//
// Usage: discovery_node <node_id> <endpoints> <seconds> <broadcast address>
// Endpoint names are ROS-like in length ("robot_NN/sensor_NN/points"), one Publisher each on the p1 Bench
// type, so each announced entity carries names of a realistic size.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>

#include "Bench.h"

#define MAX_ENDPOINTS 64
#define NAME_BYTES 48
#define NS_PER_SEC 1000000000LL
#define POLL_NS (100LL * 1000 * 1000)
#define ARG_COUNT 5
#define DECIMAL 10

static struct tt_Node node;
static struct tt_Publisher pubs[MAX_ENDPOINTS];
static char names[MAX_ENDPOINTS][NAME_BYTES];

static int64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now); // NOLINT(misc-include-cleaner) - <time.h> is the public header
    return ((int64_t)now.tv_sec * NS_PER_SEC) + now.tv_nsec;
}

int main(int argc, char** argv) {
    if (argc != ARG_COUNT) {
        (void)fprintf(stderr, "usage: %s <node_id> <endpoints> <seconds> <broadcast address>\n", argv[0]);
        return 2;
    }
    long node_id = strtol(argv[1], NULL, DECIMAL);
    long endpoints = strtol(argv[2], NULL, DECIMAL);
    long seconds = strtol(argv[3], NULL, DECIMAL);
    if (node_id < 1 || node_id > UINT8_MAX - 1 || endpoints < 0 || endpoints > MAX_ENDPOINTS || seconds < 1) {
        (void)fprintf(stderr, "node_id 1-254, endpoints 0-%d, seconds >= 1\n", MAX_ENDPOINTS);
        return 2;
    }
    tt_log_set_level(
        TT_LOG_ERROR); // receivers' discovery tables are core-default sized; their warnings are not the measurement
    _tt_CONFIG.broadcast = argv[4];
    _tt_CONFIG.node_id = (uint8_t)node_id;

    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        (void)fprintf(stderr, "cannot create node: %d\n", ret);
        return 1;
    }
    for (long i = 0; i < endpoints; i++) {
        (void)snprintf(names[i], NAME_BYTES, "robot_%02ld/sensor_%02ld/points", node_id, i);
        ret = tt_Node_create_publisher(&node, &pubs[i], &BenchTopic, names[i]);
        if (ret != 0) {
            (void)fprintf(stderr, "cannot create publisher %ld: %d\n", i, ret);
            return 1;
        }
    }
    int64_t end = now_ns() + (seconds * NS_PER_SEC);
    while (now_ns() < end) {
        (void)tt_Node_poll(&node, POLL_NS);
    }
    (void)printf("RESULT: node=%ld endpoints=%ld seconds=%ld\n", node_id, endpoints, seconds);
    tt_Node_destroy(&node);
    return 0;
}
