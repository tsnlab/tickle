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
#define DEFAULT_MESSAGE_SIZE BULKDATA__PAYLOAD_CAPACITY

// DEFAULT_INTERVAL_SECONDS of 0 means no send interval at all: publish as fast as poll()
// allows, which is what a throughput benchmark should default to. Pass -i to rate-limit to a
// specific interval instead (e.g. to target a specific Mbps for a given -s).
#define DEFAULT_INTERVAL_SECONDS 0.0

static volatile sig_atomic_t g_interrupted = 0; // a stop trigger fired (duration elapsed, or Ctrl+C)
static volatile sig_atomic_t g_exit_now = 0;    // the main loop's actual exit condition

static bool stopping = false;
static uint64_t stop_time = 0; // when stopping began - print_summary()'s own "elapsed" anchor,
                               // so shutdown_grace_s's drain doesn't dilute the reported avg Mbps

// RELIABLE's own retransmission (QoS roadmap #5) needs this Publisher to still be around to
// respond to a Subscriber's ACKNACK for one of the last few messages sent - tt_Node_destroy()
// right when sending stops used to tear it down immediately, silently dropping any
// then-still-recovering loss near the end of *every* run regardless of tc's own configured loss
// rate. Found via run_perf.sh's real loss-injection scenarios: reliable's own loss_pct plateaued
// well above the p^(tt_RELIABLE_RETRY + 1) theoretical model even after every other known bug
// (rmw_tickle/PLAN.md, Milestone 18) was fixed, and landed on the *same* value at two different
// loss levels - a fixed few tail-end losses per run, independent of p, matched a shutdown race far
// better than anything left in tickle.c's own RELIABLE logic. tt_RELIABLE_RETRY full give-up
// cycles (tt_CALL_RETRY_INTERVAL apart) fit comfortably inside this with room for real RTT.
// 0 for BEST_EFFORT - costs nothing, matches the exit-immediately behavior it always had.
#define RELIABLE_SHUTDOWN_GRACE_SEC 1.0
static double shutdown_grace_s = 0.0;

static void finish_grace_period(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_exit_now = 1;
}

// Stops sending immediately but - unlike the exit-right-away behavior this used to have - only
// actually exits after shutdown_grace_s more (0 for BEST_EFFORT, so this is a no-op change for
// it). Idempotent, and safe to call from either a scheduled callback (handle_duration_elapsed,
// which has `node`/`time` on hand) or the main loop noticing g_interrupted after a signal handler
// set it (which doesn't - signal handlers may only touch a volatile sig_atomic_t, not call this).
static void begin_stopping(struct tt_Node* node, uint64_t time) {
    if (stopping) {
        return;
    }
    stopping = true;
    g_interrupted = 1;
    stop_time = time;
    if (shutdown_grace_s <= 0.0) {
        g_exit_now = 1;
        return;
    }
    tt_Node_schedule(node, time + (uint64_t)(shutdown_grace_s * (double)tt_SECOND), finish_grace_period, NULL);
}

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1; // begin_stopping() itself needs `node` - the main loop calls it instead
}

static void handle_duration_elapsed(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    begin_stopping(node, time);
}

static struct BulkData bulk = {0}; // static: zero-initialized, reused for every publish

// QoS roadmap #5 (RELIABILITY/RELIABLE) - only actually used (pub.reliable_cache pointed at it)
// when -R is passed; otherwise inert, matching tt_Publisher.reliable_cache's own "NULL costs
// nothing" default. rmw_tickle/PLAN.md's own "DDS semantic-parity backlog" row 2 - entries[]/
// capacity are this file's own backing array now, not an embedded tt_MAX_RELIABLE_HISTORY-sized
// one (struct tt_ReliableCache's own doc comment, tickle.h) - sized to MAX_RELIABLE_DEPTH below,
// deliberately far past the old 64 default, so -K can reach a genuinely deeper depth than this
// build's own former hard ceiling to actually test whether that improves RELIABLE's own tc-loss
// recovery at high throughput, or - per that same doc comment's honest answer - the Subscriber's
// own fixed-width received_bitmap is the real bottleneck regardless.
#define MAX_RELIABLE_DEPTH 8192
// B1 (rmw_tickle/PLAN.md) - index slots are fixed-size and cheap (24B each); the byte arena is
// malloc()ed once at startup instead, sized from this run's own -s payload size. A static arena
// would have to assume the maximum (-s fills a whole Ethernet frame), i.e. the same ~12MB this
// file used to burn at MAX_RELIABLE_DEPTH regardless of the payload actually used. malloc() in an
// example is fine - the no-malloc rule is TickLE core's own (DESIGN.md), not its callers'.
static struct tt_ReliableCacheIndex reliable_cache_index[MAX_RELIABLE_DEPTH];
static struct tt_ReliableCache reliable_cache = {0};
static uint8_t* reliable_cache_arena = NULL;

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
    // stop_time (when sending stopped), not tt_get_ns() (now) - shutdown_grace_s's own drain
    // period runs between those two and sends nothing, so using "now" here would dilute avg_mbps
    // by however long that drain took, understating reported RELIABLE throughput for no reason.
    double elapsed_s = (double)(stop_time - start_time) / (double)tt_SECOND;
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
    fprintf(stderr, "                [-n topic_name] [-l log_level] [-B] [-R] [-K reliable_cache_depth]\n");
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
    fprintf(stderr, "  -B  batch sends instead of flushing each one immediately (default: flush immediately -\n"
                    "      pass -B for the old always-batch behavior, worth it when -i 0 floods many small\n"
                    "      messages back-to-back - see DESIGN.md's \"RPC and Publish flush immediately by\n"
                    "      default; batching is opt-in\")\n");
    fprintf(stderr, "  -R  RELIABLE instead of BEST_EFFORT delivery (QoS roadmap #5, rmw_tickle/PLAN.md) -\n"
                    "      retains published samples for retransmission on a Subscriber's ACKNACK\n");
    fprintf(stderr,
            "  -K  reliable cache depth, requires -R (default: tt_MAX_RELIABLE_HISTORY=%d) -\n"
            "      the real, freely-configurable retention window (struct tt_ReliableCache.\n"
            "      depth), up to this build's own MAX_RELIABLE_DEPTH=%d, independent of a\n"
            "      rebuild\n",
            tt_MAX_RELIABLE_HISTORY, MAX_RELIABLE_DEPTH);
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
    opts->batch = false;
    opts->reliable = false;
    opts->reliable_depth = 0;

    return tt_example_parse_args(argc, argv, opts,
                                 TT_EXAMPLE_OPT_INTERVAL | TT_EXAMPLE_OPT_DURATION | TT_EXAMPLE_OPT_MESSAGE_SIZE |
                                     TT_EXAMPLE_OPT_BATCH | TT_EXAMPLE_OPT_RELIABLE | TT_EXAMPLE_OPT_RELIABLE_DEPTH);
}

// Split out of main() purely to keep that function's own cognitive complexity under clang-tidy's
// threshold - adding !stopping/g_interrupted handling for shutdown_grace_s (see its own doc
// comment) tipped main() over once combined with its already-substantial setup sequence.
static void run_send_loop(struct tt_Node* node, struct tt_Publisher* pub, uint64_t next_send_time,
                          uint64_t send_interval_ns, int64_t poll_timeout) {
    tt_ret_t ret = tt_RET_OK;
    while (!g_exit_now && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        // One publish attempt per due tick, then let poll() flush/schedule/receive: pacing
        // comes from comparing wall-clock time against next_send_time, not from a tight
        // send-more-if-you-can loop (that would starve poll()). Gated on !stopping too - once a
        // stop trigger has fired, no new sends during the shutdown_grace_s drain below, only
        // servicing whatever ACKNACKs are still arriving for what was already sent.
        uint64_t now = tt_get_ns();
        if (!stopping && now >= next_send_time) {
            tt_ret_t pub_ret = tt_Publisher_publish(pub, (struct tt_Data*)&bulk);
            if (pub_ret == tt_RET_OK) {
                bulk.seq++;
                total_sent_msgs++;
                total_sent_bytes += bulk.payload_count;
                interval_sent_msgs++;
                interval_sent_bytes += bulk.payload_count;
            } else if (pub_ret == tt_RET_OUT_OF_BUFFER) {
                total_buffer_full++;
            }
            next_send_time += send_interval_ns;
        }

        ret = tt_Node_poll(node, poll_timeout);

        // Checked here (right after poll() returns), not waited on elsewhere: this runs every
        // iteration regardless of how long until the next scheduled report()/finish_grace_period,
        // so Ctrl+C is caught right away rather than up to a poll cycle late - same reasoning as
        // perf_server.c's own main loop.
        if (g_interrupted && !stopping) {
            begin_stopping(node, tt_get_ns());
        }
    }
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (opts.message_size > BULKDATA__PAYLOAD_CAPACITY) {
        printf("Requested message size %u exceeds the max %d; clamping.\n", opts.message_size,
               BULKDATA__PAYLOAD_CAPACITY);
        opts.message_size = BULKDATA__PAYLOAD_CAPACITY;
    }
    bulk.payload_count = opts.message_size;

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
    pub.batch = opts.batch; // -B - see tt_Publisher.batch's own doc comment (tickle.h)
    if (opts.batch) {
        printf("Batching sends (node_flush()'s own tt_NODE_TX_INTERVAL cadence), not flushing "
               "each one immediately\n");
    }
    if (opts.reliable) {
        uint32_t depth = opts.reliable_depth != 0 ? opts.reliable_depth : (uint32_t)tt_MAX_RELIABLE_HISTORY;
        if (depth > (uint32_t)MAX_RELIABLE_DEPTH) {
            printf("Requested reliable cache depth %u exceeds this build's own MAX_RELIABLE_DEPTH (%d); clamping.\n",
                   depth, MAX_RELIABLE_DEPTH);
            depth = (uint32_t)MAX_RELIABLE_DEPTH;
        }
        uint32_t arena_bytes = tt_RELIABLE_CACHE_ARENA_BYTES(depth, tt_RELIABLE_RECORD_BYTES(opts.message_size));
        reliable_cache_arena = malloc(arena_bytes);
        if (reliable_cache_arena == NULL) {
            printf("Cannot allocate a %u-byte reliable cache arena\n", arena_bytes);
            return 1;
        }
        reliable_cache.index = reliable_cache_index;
        reliable_cache.capacity = (uint16_t)depth;
        reliable_cache.depth = (uint16_t)depth;
        reliable_cache.arena = reliable_cache_arena;
        reliable_cache.arena_size = arena_bytes;
        pub.reliable_cache = &reliable_cache;           // -R - see tt_Publisher.reliable_cache's own doc comment
        pub.reliable = true;                            // -R - see tt_Publisher.reliable's own doc comment
        shutdown_grace_s = RELIABLE_SHUTDOWN_GRACE_SEC; // see shutdown_grace_s's own doc comment
        printf("RELIABLE delivery (retained-sample cache depth %u)\n", depth);
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

    run_send_loop(&node, &pub, next_send_time, send_interval_ns, poll_timeout);

    print_summary(start_time);

    tt_Node_destroy(&node);
    free(reliable_cache_arena); // NULL unless -R was passed; free(NULL) is a no-op

    return 0;
}
