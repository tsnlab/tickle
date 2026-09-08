/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

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
#include "Bulk.h"

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void handle_duration_elapsed(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

static bool have_first = false;
static uint32_t expected_seq = 0;

static uint64_t total_received_msgs = 0;
static uint64_t total_received_bytes = 0;
static uint64_t total_dropped = 0;

static uint64_t interval_received_msgs = 0;
static uint64_t interval_received_bytes = 0;

static void bulk_callback(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct BulkData* data) {
    (void)sub;
    (void)time;
    (void)seq_no; // truncated to 16 bits by the framework; data->seq is the real 32-bit one

    if (have_first && data->seq != expected_seq) {
        // Unsigned wraparound makes this correct even if seq itself has wrapped past UINT32_MAX.
        total_dropped += data->seq - expected_seq;
    }
    expected_seq = data->seq + 1;
    have_first = true;

    total_received_msgs++;
    total_received_bytes += data->size;
    interval_received_msgs++;
    interval_received_bytes += data->size;
}

static void report(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;

    const double bytes_per_mb = 1e6;
    double megabytes = (double)interval_received_bytes / bytes_per_mb;
    double mbps = ((double)interval_received_bytes * 8) / bytes_per_mb;
    printf("recv %llu msgs, %.3f MB, %.3f Mbps this interval (%llu dropped so far)\n",
           (unsigned long long)interval_received_msgs, megabytes, mbps, (unsigned long long)total_dropped);

    interval_received_msgs = 0;
    interval_received_bytes = 0;

    tt_Node_schedule(node, time + tt_SECOND, report, NULL);
}

static void print_summary(uint64_t start_time) {
    const double bytes_per_mb = 1e6;
    const double percent_scale = 100.0;
    double elapsed_s = (double)(tt_get_ns() - start_time) / (double)tt_SECOND;
    double megabytes = (double)total_received_bytes / bytes_per_mb;
    double avg_mbps = elapsed_s > 0.0 ? ((double)total_received_bytes * 8) / bytes_per_mb / elapsed_s : 0.0;
    uint64_t expected_total = total_received_msgs + total_dropped;
    double loss_pct = expected_total > 0 ? (percent_scale * (double)total_dropped / (double)expected_total) : 0.0;

    printf("\n--- bulk_topic receive statistics ---\n");
    printf("%llu messages received, %llu dropped, %.1f%% loss, %.3f MB, %.3f sec, avg %.3f Mbps\n",
           (unsigned long long)total_received_msgs, (unsigned long long)total_dropped, loss_pct, megabytes, elapsed_s,
           avg_mbps);
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-d duration_seconds]\n"
            "          [-n topic_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -d  exit automatically after this many seconds (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -n  topic name to subscribe to (default bulk_topic)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->duration_s = 0.0;
    opts->name = "bulk_topic";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;

    return tt_example_parse_args(argc, argv, opts, TT_EXAMPLE_OPT_DURATION);
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }

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

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BulkTopic, opts.name, (tt_SUBSCRIBER_CALLBACK)bulk_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }

    uint64_t start_time = tt_get_ns();
    tt_Node_schedule(&node, start_time + tt_SECOND, report, NULL);
    if (opts.duration_s > 0.0) {
        tt_Node_schedule(&node, start_time + (uint64_t)(opts.duration_s * (double)tt_SECOND), handle_duration_elapsed,
                         NULL);
    }

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    print_summary(start_time);

    tt_Node_destroy(&node);

    return 0;
}
