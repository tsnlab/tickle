/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>

#include "../common/cli_opts.h"
#include "PingPong.h"

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint32_t seq = 0;
static uint32_t pending_seq = 0;
static uint32_t target_count = 0; // 0 = unlimited
static uint32_t completed = 0;    // pings whose outcome (reply or drop) is known
static uint64_t send_interval_ns = 0;

static uint64_t transmitted = 0;
static uint64_t received = 0;
static double rtt_min_ms = -1.0;
static double rtt_max_ms = 0.0;
static double rtt_sum_ms = 0.0;
static double rtt_sum_sq_ms = 0.0;

static void note_completed(void) {
    completed++;
    if (target_count > 0 && completed >= target_count) {
        g_interrupted = 1;
    }
}

static void ping_callback(struct tt_Client* client, int8_t return_code, struct PingPongResponse* response) {
    (void)client;

    if (return_code == 0 && response == NULL) {
        printf("Request timeout for icmp_seq=%u (dropped)\n", pending_seq);
        note_completed();
        return;
    }
    if (return_code != 0) {
        printf("Error, return_code: %d (icmp_seq=%u dropped)\n", return_code, pending_seq);
        note_completed();
        return;
    }

    double rtt_ms = (double)(tt_get_ns() - response->timestamp) / (double)tt_MILLISECOND;

    received++;
    if (rtt_min_ms < 0.0 || rtt_ms < rtt_min_ms) {
        rtt_min_ms = rtt_ms;
    }
    if (rtt_ms > rtt_max_ms) {
        rtt_max_ms = rtt_ms;
    }
    rtt_sum_ms += rtt_ms;
    rtt_sum_sq_ms += rtt_ms * rtt_ms;

    printf("seq=%u time=%.3f ms\n", response->seq, rtt_ms);
    note_completed();
}

static void ping(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Client* client = param;

    uint32_t this_seq = seq;
    struct PingPongRequest request = {.seq = this_seq, .timestamp = tt_get_ns()};
    tt_ret_t ret = tt_Client_call(client, (struct tt_Request*)&request);
    if (ret == tt_RET_OK) {
        seq++;
        transmitted++;
        pending_seq = this_seq;
    } else if (ret == tt_RET_ILLEGAL_STATUS) {
        printf("Previous ping still awaiting a reply, skipping this interval\n");
    } else {
        printf("Cannot send ping: %d\n", ret);
    }

    if (target_count == 0 || transmitted < target_count) {
        tt_Node_schedule(node, time + send_interval_ns, ping, client);
    }
}

// The verifying side of the ping/pong round trip: pong.c never logs per-request (see its own
// comment), so this ping's own tally - not pong.log - is the real evidence a round trip
// happened. Latency is what this pair measures, so RESULT reports the numbers themselves rather
// than a pass/fail verdict (see set_bool/uint64's client.c/subscriber.c for the functional-
// verification counterpart) - a reader (human or CI log scraper) judges "good" or "bad" against
// the actual rtt/loss values.
static void print_statistics(uint64_t start_time) {
    uint64_t lost = transmitted - received;
    double loss_pct = transmitted > 0 ? (100.0 * (double)lost / (double)transmitted) : 0.0;
    double elapsed_ms = (double)(tt_get_ns() - start_time) / (double)tt_MILLISECOND;

    printf("\n--- ping statistics ---\n");
    printf("%llu packets transmitted, %llu received, %.0f%% packet loss, time %.0fms\n",
           (unsigned long long)transmitted, (unsigned long long)received, loss_pct, elapsed_ms);

    if (received > 0) {
        double avg = rtt_sum_ms / (double)received;
        double variance = (rtt_sum_sq_ms / (double)received) - (avg * avg);
        double mdev = variance > 0.0 ? sqrt(variance) : 0.0;
        printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n", rtt_min_ms, avg, rtt_max_ms, mdev);
        printf("RESULT: sent=%llu recv=%llu loss_pct=%.0f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
               (unsigned long long)transmitted, (unsigned long long)received, loss_pct, avg, rtt_max_ms);
    } else {
        printf("RESULT: sent=%llu recv=%llu loss_pct=%.0f\n", (unsigned long long)transmitted,
               (unsigned long long)received, loss_pct);
    }
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-c count] [-i interval_seconds]\n"
            "          [-n endpoint_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -c  stop after this many pings (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -i  seconds between pings (default 1)\n");
    fprintf(stderr, "  -n  service name to rendezvous with pong on (default ping_pong_server)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->interval_s = 1.0;
    opts->name = "ping_pong_server";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;
    opts->count = 0;

    return tt_example_parse_args(argc, argv, opts, TT_EXAMPLE_OPT_COUNT | TT_EXAMPLE_OPT_INTERVAL);
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    target_count = opts.count;

    send_interval_ns = (uint64_t)(opts.interval_s * (double)tt_SECOND);

    _tt_CONFIG.broadcast = opts.broadcast;
    if (opts.port != 0) {
        _tt_CONFIG.port = opts.port;
    }
    if (opts.bind_addr != NULL) {
        _tt_CONFIG.addr = opts.bind_addr;
    }
    if (opts.log_level_set) {
        tt_log_set_level(opts.log_level);
    }

    // sigaction (not signal()) so SA_RESTART is off: an interrupted blocking recv
    // returns immediately instead of silently restarting with the same wait.
    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    printf("Node created(#%d)\n", node.id);

    struct tt_Client client;
    ret = tt_Node_create_client(&node, &client, &PingPongService, opts.name, (tt_CLIENT_CALLBACK)ping_callback);
    if (ret != 0) {
        printf("Cannot create client: %d\n", ret);
        return ret;
    }

    uint64_t start_time = tt_get_ns();
    tt_Node_schedule(&node, start_time, ping, &client);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    print_statistics(start_time);

    tt_Node_destroy(&node);

    return 0;
}
