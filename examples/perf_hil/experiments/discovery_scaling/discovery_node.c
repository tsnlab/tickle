// One idle TickLE node with E publishers, for measuring what discovery alone costs on the wire
// (2026-09-26, rmw_tickle/DISCOVERY_PLAN.md M1). It publishes nothing, so every byte its interface
// carries is discovery (plus one-time ARP). discovery_scaling.sh runs N of these in private netns on a
// bridge and reads the interfaces' byte counters.
//
// Usage: discovery_node <node_id> <endpoints> <seconds> <broadcast address> [expected entities]
// With [expected entities] (DISCOVERY_PLAN.md M3/M5) the node attaches a discovery table and reports when it first
// held that many alive entities (tt_Discovery_count(), counted inside the discovery callback, under the node's
// lock), how often it later fell below it, and when it last changed across it. That needs the core and this file
// built with the same -Dtt_MAX_DISCOVERED_ENTITIES large enough for the run, since the table embeds that array.
// Endpoint names are ROS-like in length ("robot_NN/sensor_NN/points"), one Publisher each on the p1 Bench
// type, so each announced entity carries names of a realistic size.
#include <stdbool.h>
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
#define ARG_COUNT_JOIN 6
#define MS_PER_NS 1000000.0
#define SETTLE_NS (2 * NS_PER_SEC)
#define DECIMAL 10

static struct tt_Node node;
static struct tt_Publisher pubs[MAX_ENDPOINTS];
static char names[MAX_ENDPOINTS][NAME_BYTES];
static struct tt_Discovery discovery;

struct join_state {
    int64_t start_ns;
    uint32_t expected;
    int64_t reached_ns; // first time the count met expected, 0 before
    int64_t last_change_ns;
    uint32_t dips; // times the count fell below expected after reaching it
    int complete;
    int complete_before_exit; // sampled SETTLE_NS before the run ends, before any peer's goodbye can arrive
};
static struct join_state join;

static int64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now); // NOLINT(misc-include-cleaner) - <time.h> is the public header
    return ((int64_t)now.tv_sec * NS_PER_SEC) + now.tv_nsec;
}

static void on_discovery(struct tt_Node* discovering, uint8_t node_id, uint32_t endpoint_id, uint8_t kind,
                         bool departed, void* param) {
    (void)discovering;
    (void)node_id;
    (void)endpoint_id;
    (void)kind;
    (void)departed;
    struct join_state* state = param;
    int complete = tt_Discovery_count(&discovery) >= state->expected;
    if (complete != state->complete) {
        int64_t now = now_ns();
        state->last_change_ns = now;
        if (complete && state->reached_ns == 0) {
            state->reached_ns = now;
        } else if (!complete) {
            state->dips++;
        }
        state->complete = complete;
    }
}

int main(int argc, char** argv) {
    if (argc != ARG_COUNT && argc != ARG_COUNT_JOIN) {
        (void)fprintf(stderr, "usage: %s <node_id> <endpoints> <seconds> <broadcast address> [expected entities]\n",
                      argv[0]);
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

    join.start_ns = now_ns();
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        (void)fprintf(stderr, "cannot create node: %d\n", ret);
        return 1;
    }
    if (argc == ARG_COUNT_JOIN) {
        join.expected = (uint32_t)strtoul(argv[5], NULL, DECIMAL);
        ret = tt_Node_set_discovery(&node, &discovery, on_discovery, &join);
        if (ret != 0) {
            (void)fprintf(stderr, "cannot attach discovery: %d\n", ret);
            return 1;
        }
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
    int sampled = 0;
    while (now_ns() < end) {
        (void)tt_Node_poll(&node, POLL_NS);
        if (!sampled && now_ns() >= end - SETTLE_NS) {
            tt_Node_lock(&node); // the callback updates join.complete under the node's lock
            join.complete_before_exit = join.complete;
            tt_Node_unlock(&node);
            sampled = 1;
        }
    }
    (void)printf("RESULT: node=%ld endpoints=%ld seconds=%ld", node_id, endpoints, seconds);
    if (argc == ARG_COUNT_JOIN) {
        // -1: never reached. Times are ms after this process started, before tt_Node_create().
        (void)printf(
            " expected=%u reached_ms=%.1f dips=%u last_change_ms=%.1f complete_at_exit=%d complete_2s_before_exit=%d",
            join.expected, join.reached_ns ? (double)(join.reached_ns - join.start_ns) / MS_PER_NS : -1.0, join.dips,
            join.last_change_ns ? (double)(join.last_change_ns - join.start_ns) / MS_PER_NS : -1.0, join.complete,
            join.complete_before_exit);
    }
    (void)printf("\n");
    tt_Node_destroy(&node);
    return 0;
}
