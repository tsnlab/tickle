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

// DEFAULT_MESSAGE_SIZE fills one standard (1500-byte MTU) Ethernet frame as full as this
// protocol allows, so every packet carries the most payload it can without IP fragmentation.
// See "Message size: filling an Ethernet frame" in README.md for the derivation.
#define DEFAULT_MESSAGE_SIZE BULK_MAX_PAYLOAD_SIZE

// DEFAULT_INTERVAL_SECONDS of 0 means no send interval at all: publish as fast as poll()
// allows, which is what a throughput benchmark should default to. Pass -i to rate-limit to a
// specific interval instead (e.g. to target a specific Mbps for a given -s).
#define DEFAULT_INTERVAL_SECONDS 0.0

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

static struct BulkData bulk = {0}; // static: zero-initialized, reused for every publish

static uint64_t total_sent_msgs = 0;
static uint64_t total_sent_bytes = 0;
static uint64_t total_buffer_full = 0;

static uint64_t interval_sent_msgs = 0;
static uint64_t interval_sent_bytes = 0;

static void report(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;

    char sent_buf[TT_GROUPED_BUF_LEN];
    char buffer_full_buf[TT_GROUPED_BUF_LEN];
    char megabytes_buf[TT_GROUPED_F3_BUF_LEN];
    char mbps_buf[TT_GROUPED_F3_BUF_LEN];
    const double bytes_per_mb = 1e6;
    double megabytes = (double)interval_sent_bytes / bytes_per_mb;
    double mbps = ((double)interval_sent_bytes * 8) / bytes_per_mb;
    printf("sent %s msgs, %s MB, %s Mbps this interval (%s buffer-full so far)\n",
           tt_format_grouped(interval_sent_msgs, sent_buf), tt_format_grouped_f3(megabytes, megabytes_buf),
           tt_format_grouped_f3(mbps, mbps_buf), tt_format_grouped(total_buffer_full, buffer_full_buf));

    interval_sent_msgs = 0;
    interval_sent_bytes = 0;

    tt_Node_schedule(node, time + tt_SECOND, report, NULL);
}

// Throughput is what this pair measures, so - like ping.c's print_statistics() - RESULT reports
// the numbers themselves rather than a pass/fail verdict; perf_server.c's own RESULT line is the
// authoritative side (it can see loss, this side can't), this one's just for visibility into
// what the sender itself achieved.
static void print_summary(uint64_t start_time) {
    char sent_buf[TT_GROUPED_BUF_LEN];
    char buffer_full_buf[TT_GROUPED_BUF_LEN];
    char megabytes_buf[TT_GROUPED_F3_BUF_LEN];
    char avg_mbps_buf[TT_GROUPED_F3_BUF_LEN];
    const double bytes_per_mb = 1e6;
    double elapsed_s = (double)(tt_get_ns() - start_time) / (double)tt_SECOND;
    double megabytes = (double)total_sent_bytes / bytes_per_mb;
    double avg_mbps = elapsed_s > 0.0 ? ((double)total_sent_bytes * 8) / bytes_per_mb / elapsed_s : 0.0;

    printf("\n--- bulk_topic send statistics ---\n");
    printf("%s messages sent, %s MB, %.3f sec, avg %s Mbps, %s times tx buffer was full\n",
           tt_format_grouped(total_sent_msgs, sent_buf), tt_format_grouped_f3(megabytes, megabytes_buf), elapsed_s,
           tt_format_grouped_f3(avg_mbps, avg_mbps_buf), tt_format_grouped(total_buffer_full, buffer_full_buf));
    printf("RESULT: sent=%s avg_mbps=%s buffer_full=%s\n", tt_format_grouped(total_sent_msgs, sent_buf),
           tt_format_grouped_f3(avg_mbps, avg_mbps_buf), tt_format_grouped(total_buffer_full, buffer_full_buf));
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-I node_id]\n", prog);
    fprintf(stderr, "                [-s message_size_bytes] [-i interval_seconds] [-d duration_seconds]\n");
    fprintf(stderr, "                [-n topic_name] [-l log_level]\n");
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -I  explicit node ID 1-254 (default: auto-detect from -a/-b's subnet)\n");
    fprintf(stderr, "  -s  payload bytes per message (default/max %d: fills one Ethernet frame)\n",
            DEFAULT_MESSAGE_SIZE);
    fprintf(stderr, "  -i  seconds between sends (default %g = as fast as poll() allows)\n", DEFAULT_INTERVAL_SECONDS);
    fprintf(stderr, "  -d  exit automatically after this many seconds (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -n  topic name to publish on (default bulk_topic)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->node_id = 0;
    opts->message_size = DEFAULT_MESSAGE_SIZE;
    opts->interval_s = DEFAULT_INTERVAL_SECONDS;
    opts->duration_s = 0.0;
    opts->name = "bulk_topic";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;

    return tt_example_parse_args(argc, argv, opts,
                                 TT_EXAMPLE_OPT_INTERVAL | TT_EXAMPLE_OPT_DURATION | TT_EXAMPLE_OPT_MESSAGE_SIZE);
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (opts.message_size > BULK_MAX_PAYLOAD_SIZE) {
        printf("Requested message size %u exceeds the max %d; clamping.\n", opts.message_size, BULK_MAX_PAYLOAD_SIZE);
        opts.message_size = BULK_MAX_PAYLOAD_SIZE;
    }
    bulk.size = opts.message_size;

    uint64_t send_interval_ns = opts.interval_s > 0.0 ? (uint64_t)(opts.interval_s * (double)tt_SECOND) : 0;

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

    struct tt_Publisher pub;
    ret = tt_Node_create_publisher(&node, &pub, &BulkTopic, opts.name);
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }

    const double bytes_per_mb = 1e6;
    double target_mbps = opts.interval_s > 0.0 ? ((double)opts.message_size * 8) / bytes_per_mb / opts.interval_s : 0.0;
    if (opts.interval_s > 0.0) {
        printf("Sending %u-byte messages every %g sec (target %.3f Mbps)\n", opts.message_size, opts.interval_s,
               target_mbps);
    } else {
        printf("Sending %u-byte messages as fast as poll() allows\n", opts.message_size);
    }

    uint64_t start_time = tt_get_ns();
    uint64_t next_send_time = start_time;
    tt_Node_schedule(&node, start_time + tt_SECOND, report, NULL);
    if (opts.duration_s > 0.0) {
        tt_Node_schedule(&node, start_time + (uint64_t)(opts.duration_s * (double)tt_SECOND), handle_duration_elapsed,
                         NULL);
    }

    // In "as fast as poll() allows" mode (-i 0), pass 0 for a non-blocking poll pass between
    // sends (run due scheduler work + drain RX, no wait). Anything else - even a nominal 1ns -
    // rounds up to a full 1ms poll() wait, which throttled send rate ~40x once the Publisher
    // started unicasting to its lone discovered Subscriber and stopped seeing its own broadcast
    // loop back to keep that wait short. A rate-limited run (-i > 0) keeps -1: it's genuinely
    // idle between sends, so the wait lets it service inbound traffic without spinning a core.
    const int64_t poll_timeout = send_interval_ns == 0 ? 0 : -1;

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        // One publish attempt per due tick, then let poll() flush/schedule/receive: pacing
        // comes from comparing wall-clock time against next_send_time, not from a tight
        // send-more-if-you-can loop (that would starve poll() - and this interrupt check).
        uint64_t now = tt_get_ns();
        if (now >= next_send_time) {
            tt_ret_t pub_ret = tt_Publisher_publish(&pub, (struct tt_Data*)&bulk);
            if (pub_ret == tt_RET_OK) {
                bulk.seq++;
                total_sent_msgs++;
                total_sent_bytes += bulk.size;
                interval_sent_msgs++;
                interval_sent_bytes += bulk.size;
            } else if (pub_ret == tt_RET_OUT_OF_BUFFER) {
                total_buffer_full++;
            }
            next_send_time += send_interval_ns;
        }

        ret = tt_Node_poll(&node, poll_timeout);
    }

    print_summary(start_time);

    tt_Node_destroy(&node);

    return 0;
}
