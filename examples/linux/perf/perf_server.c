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

#include "../../format.h"
#include "../common/cli_opts.h"
#include "Bulk.h"

static volatile sig_atomic_t g_interrupted = 0; // raw SIGINT flag - see main()'s own comment
static bool g_exit_now = false;                 // the main loop's actual exit condition

static bool stopping = false;       // a stop trigger has fired; draining cooldown_s more now
static uint64_t cooldown_start = 0; // ns timestamp once stopping - >= this is cooldown

static double warmup_s = 0.0;   // -w: seconds after the first real message before counting starts
static double cooldown_s = 0.0; // -W: seconds to keep receiving (uncounted) after the stop
                                // trigger (-d elapsed, or Ctrl+C) before actually exiting - see
                                // this file's own comment on why this isn't computed by looking
                                // backward from a known -d instead.

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void finish_cooldown(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_exit_now = true;
}

// Called once, the first time a stop condition is detected (-d elapsed, or Ctrl+C) - rather than
// exiting immediately, keeps receiving for cooldown_s more seconds (uncounted - see
// bulk_callback()'s own gating) before actually exiting. Works the same way whether the run was
// bounded (-d) or not (stopped with Ctrl+C), since either way this only ever needs "cooldown_s
// seconds from now", never advance knowledge of when the run will end. Idempotent - only the
// first trigger takes effect.
static void begin_stopping(struct tt_Node* node, uint64_t time) {
    if (stopping) {
        return;
    }
    stopping = true;
    cooldown_start = time;
    if (cooldown_s <= 0.0) {
        g_exit_now = true;
        return;
    }
    tt_Node_schedule(node, time + (uint64_t)(cooldown_s * (double)tt_SECOND), finish_cooldown, NULL);
}

static void handle_duration_elapsed(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    begin_stopping(node, time);
}

static bool have_first = false;
static uint32_t expected_seq = 0;
static uint64_t first_recv_time = 0; // ns timestamp of the first real message - anchors warm-up,
                                     // not this process's own start_time (see this file's own
                                     // comment: perf_server's -d intentionally runs longer than
                                     // perf_client's, so "since I started" would count idle time
                                     // before perf_client even begins sending as warm-up).

static uint64_t total_received_msgs = 0;
static uint64_t total_received_bytes = 0;
static uint64_t total_dropped = 0;

static uint64_t interval_received_msgs = 0;
static uint64_t interval_received_bytes = 0;

static void bulk_callback(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct BulkData* data) {
    (void)sub;
    (void)seq_no; // truncated to 16 bits by the framework; data->seq is the real 32-bit one

    if (!have_first) {
        first_recv_time = time;
    }

    bool gap = have_first && data->seq != expected_seq;
    // Unsigned wraparound makes this correct even if seq itself has wrapped past UINT32_MAX.
    uint32_t gap_count = gap ? data->seq - expected_seq : 0;
    expected_seq = data->seq + 1;
    have_first = true;

    interval_received_msgs++;
    interval_received_bytes += data->size;

    bool in_warmup = (double)(time - first_recv_time) / (double)tt_SECOND < warmup_s;
    bool in_cooldown = stopping && time >= cooldown_start;
    if (!in_warmup && !in_cooldown) {
        total_received_msgs++;
        total_received_bytes += data->size;
        total_dropped += gap_count;
    }
}

static void report(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;

    char recv_buf[TT_GROUPED_BUF_LEN];
    char dropped_buf[TT_GROUPED_BUF_LEN];
    char megabytes_buf[TT_GROUPED_F3_BUF_LEN];
    char mbps_buf[TT_GROUPED_F3_BUF_LEN];
    const double bytes_per_mb = 1e6;
    double megabytes = (double)interval_received_bytes / bytes_per_mb;
    double mbps = ((double)interval_received_bytes * 8) / bytes_per_mb;

    bool in_warmup = have_first && (double)(time - first_recv_time) / (double)tt_SECOND < warmup_s;
    bool in_cooldown = stopping && time >= cooldown_start;
    const char* tag = "";
    if (in_warmup) {
        tag = " (warmup)";
    } else if (in_cooldown) {
        tag = " (cooldown)";
    }

    printf("recv %s msgs, %s MB, %s Mbps this interval (%s dropped so far)%s\n",
           tt_format_grouped(interval_received_msgs, recv_buf), tt_format_grouped_f3(megabytes, megabytes_buf),
           tt_format_grouped_f3(mbps, mbps_buf), tt_format_grouped(total_dropped, dropped_buf), tag);

    interval_received_msgs = 0;
    interval_received_bytes = 0;

    tt_Node_schedule(node, time + tt_SECOND, report, NULL);
}

// The verifying side of the perf round trip: it can see drops (perf_client.c can't - it never
// hears back), so this is the authoritative measurement, same role ping.c's print_statistics()
// plays for latency. Still numbers, not a pass/fail verdict - throughput/loss is a spectrum a
// human or CI log scraper judges against a threshold, not a binary outcome the way "did the
// response ever arrive at all" is for set_bool/uint64.
static void print_summary(uint64_t start_time) {
    char recv_buf[TT_GROUPED_BUF_LEN];
    char dropped_buf[TT_GROUPED_BUF_LEN];
    char megabytes_buf[TT_GROUPED_F3_BUF_LEN];
    char avg_mbps_buf[TT_GROUPED_F3_BUF_LEN];
    const double bytes_per_mb = 1e6;
    const double percent_scale = 100.0;

    // The counted window excludes warm-up/cool-down, so its duration is measured the same way -
    // from when warm-up ended (first real message + warmup_s) to when cool-down began (or now, if
    // this somehow got here without ever stopping - shouldn't normally happen, but a safe
    // fallback) - not this process's own start_time/-d window, which can run measurably longer
    // than the real traffic (see first_recv_time's own comment on why).
    uint64_t window_start = have_first ? first_recv_time + (uint64_t)(warmup_s * (double)tt_SECOND) : start_time;
    uint64_t window_end = stopping ? cooldown_start : tt_get_ns();
    double elapsed_s = window_end > window_start ? (double)(window_end - window_start) / (double)tt_SECOND : 0.0;

    double megabytes = (double)total_received_bytes / bytes_per_mb;
    double avg_mbps = elapsed_s > 0.0 ? ((double)total_received_bytes * 8) / bytes_per_mb / elapsed_s : 0.0;
    uint64_t expected_total = total_received_msgs + total_dropped;
    double loss_pct = expected_total > 0 ? (percent_scale * (double)total_dropped / (double)expected_total) : 0.0;

    printf("\n--- bulk_topic receive statistics ---\n");
    printf("%s messages received, %s dropped, %.1f%% loss, %s MB, %.3f sec, avg %s Mbps\n",
           tt_format_grouped(total_received_msgs, recv_buf), tt_format_grouped(total_dropped, dropped_buf), loss_pct,
           tt_format_grouped_f3(megabytes, megabytes_buf), elapsed_s, tt_format_grouped_f3(avg_mbps, avg_mbps_buf));
    printf("RESULT: recv=%s dropped=%s loss_pct=%.1f avg_mbps=%s\n", tt_format_grouped(total_received_msgs, recv_buf),
           tt_format_grouped(total_dropped, dropped_buf), loss_pct, tt_format_grouped_f3(avg_mbps, avg_mbps_buf));
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-I node_id]\n"
            "          [-d duration_seconds] [-w warmup_seconds] [-W cooldown_seconds]\n"
            "          [-n topic_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -I  explicit node ID 1-254 (default: auto-detect from -a/-b's subnet)\n");
    fprintf(stderr, "  -d  exit automatically after this many seconds (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -w  seconds after the first real message to start counting (default 0)\n");
    fprintf(stderr, "  -W  seconds to keep receiving (uncounted) after the stop trigger before\n");
    fprintf(stderr, "      actually exiting (default 0 = stop immediately)\n");
    fprintf(stderr, "  -n  topic name to subscribe to (default bulk_topic)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->node_id = 0;
    opts->duration_s = 0.0;
    opts->warmup = 0.0;
    opts->cooldown = 0.0;
    opts->name = "bulk_topic";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;

    return tt_example_parse_args(argc, argv, opts, TT_EXAMPLE_OPT_DURATION | TT_EXAMPLE_OPT_WARMUP_COOLDOWN);
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    warmup_s = opts.warmup;
    cooldown_s = opts.cooldown;

    _tt_CONFIG.broadcast = opts.broadcast;
    if (opts.port != 0) {
        _tt_CONFIG.port = opts.port;
    }
    if (opts.bind_addr != NULL) {
        _tt_CONFIG.addr = opts.bind_addr;
    }
    if (opts.node_id != 0) {
        _tt_CONFIG.node_id = opts.node_id;
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
    while (!g_exit_now && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
        // Checked here (right after poll() returns), not waited on elsewhere: this runs every
        // iteration regardless of how long until the next scheduled report()/finish_cooldown, so
        // Ctrl+C is caught right away rather than up to a second late.
        if (g_interrupted && !stopping) {
            begin_stopping(&node, tt_get_ns());
        }
    }

    print_summary(start_time);

    tt_Node_destroy(&node);

    return 0;
}
